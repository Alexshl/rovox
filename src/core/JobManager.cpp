// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "JobManager.h"

#include "Ids.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
std::string isoNow()
{
    const auto t = std::time (nullptr);
    std::tm tm {};
    gmtime_r (&t, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

constexpr size_t maxJobsInMemory = 500;
} // namespace

const char* toString (JobState s)
{
    switch (s)
    {
        case JobState::queued:      return "queued";
        case JobState::running:     return "running";
        case JobState::succeeded:   return "succeeded";
        case JobState::failed:      return "failed";
        case JobState::cancelled:   return "cancelled";
    }

    return "failed";
}

static std::optional<JobState> parseState (const std::string& s)
{
    for (auto st : { JobState::queued, JobState::running, JobState::succeeded, JobState::failed, JobState::cancelled })
        if (s == toString (st))
            return st;

    return std::nullopt;
}

json JobInfo::toJson() const
{
    json j { { "job_id", id }, { "kind", kind }, { "project_id", projectId }, { "tenant_id", tenant }, { "state", toString (state) },
             { "progress", std::round (progress * 1000.0) / 1000.0 }, { "stage", stage }, { "created", created } };

    if (! clientRequestId.empty())  j["client_request_id"] = clientRequestId;
    if (! started.empty())          j["started"] = started;
    if (! finished.empty())         j["finished"] = finished;
    if (! origin.is_null())         j["origin"] = origin;
    if (! result.is_null())         j["result"] = result;
    if (error)                      j["error"] = error->toJson();
    return j;
}

std::optional<JobInfo> JobInfo::fromJson (const json& j)
{
    if (! j.is_object() || ! j.contains ("job_id") || ! j.contains ("state"))
        return std::nullopt;

    auto st = parseState (j.value ("state", ""));

    if (! st)
        return std::nullopt;

    JobInfo i;
    i.id = j.value ("job_id", "");
    i.kind = j.value ("kind", "");
    i.projectId = j.value ("project_id", "");
    i.tenant = j.value ("tenant_id", "local");
    i.state = *st;
    i.progress = j.value ("progress", 0.0);
    i.stage = j.value ("stage", "");
    i.clientRequestId = j.value ("client_request_id", "");
    i.created = j.value ("created", "");
    i.started = j.value ("started", "");
    i.finished = j.value ("finished", "");
    i.origin = j.value ("origin", json (nullptr));
    i.result = j.value ("result", json (nullptr));

    if (j.contains ("error") && j["error"].is_object())
        i.error = Error { j["error"].value ("code", errc::internal), j["error"].value ("message", ""), j["error"].value ("details", json (nullptr)) };

    return i;
}

struct JobManager::Entry
{
    JobInfo info;
    JobFn fn;
    int priority = 0;
    std::uint64_t seq = 0;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>> (false);
    double lastReported = 0;
};

struct JobManager::Pool
{
    std::string name;
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<Entry>> queue;      // picked by (priority desc, seq asc)
    std::condition_variable cv;
};

//==============================================================================
void JobContext::progress (double fraction, const std::string& stage)
{
    manager_.setProgress (id_, std::clamp (fraction, 0.0, 1.0), stage);
}

JobManager::JobManager (fs::path dir, JobPools sizes, ChangeFn onChange) : dir_ (std::move (dir)), onChange_ (std::move (onChange))
{
    if (! dir_.empty())
    {
        std::error_code ec;
        fs::create_directories (dir_, ec);

        for (auto& f : fs::directory_iterator (dir_, ec))
        {
            if (f.path().extension() != ".json")
                continue;

            std::ifstream in (f.path());
            auto j = json::parse (in, nullptr, false);

            if (j.is_discarded())
                continue;

            auto info = JobInfo::fromJson (j);

            if (! info)
                continue;

            if (! isFinished (info->state))
            {
                const std::string was = toString (info->state);
                info->state = JobState::failed;
                info->error = makeError (errc::serverRestarted, "the server restarted while this job was " + was);
                info->finished = isoNow();
                persist (*info);
            }

            auto e = std::make_shared<Entry>();
            e->info = std::move (*info);

            if (! e->info.clientRequestId.empty())
                byRequest_[e->info.tenant + "|" + e->info.projectId + "|" + e->info.clientRequestId] = e->info.id;

            jobs_[e->info.id] = std::move (e);
        }

        evictOldFinished();
    }

    auto addPool = [this] (const std::string& name, int n)
    {
        auto p = std::make_unique<Pool>();
        p->name = name;
        auto* raw = p.get();

        for (int i = 0; i < std::max (1, n); ++i)
            raw->threads.emplace_back ([this, raw] { worker (*raw); });

        pools_[name] = std::move (p);
    };

    addPool ("render", sizes.render);
    addPool ("analysis", sizes.analysis);
    addPool ("io", sizes.io);
}

JobManager::~JobManager()
{
    shutdown();
}

void JobManager::shutdown()
{
    std::vector<std::shared_ptr<Entry>> dropped;

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (stopping_)
            return;

        stopping_ = true;

        for (auto& [id, e] : jobs_)
            if (! isFinished (e->info.state))
                e->cancel->store (true);

        for (auto& [name, p] : pools_)
        {
            for (auto& e : p->queue)
            {
                e->info.state = JobState::cancelled;
                e->info.error = makeError (errc::cancelled, "the server is shutting down");
                e->info.finished = isoNow();
                dropped.push_back (e);
            }

            p->queue.clear();
            p->cv.notify_all();
        }
    }

    cv_.notify_all();

    for (auto& e : dropped)
        persist (e->info);

    for (auto& [name, p] : pools_)
        for (auto& t : p->threads)
            if (t.joinable())
                t.join();
}

JobManager::Submitted JobManager::submit (JobSpec spec, JobFn fn)
{
    std::shared_ptr<Entry> entry;
    JobInfo snapshot;

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (! spec.clientRequestId.empty())
            if (auto it = byRequest_.find (spec.tenant + "|" + spec.projectId + "|" + spec.clientRequestId); it != byRequest_.end() && jobs_.count (it->second))
                return { it->second, false };

        auto pit = pools_.find (spec.pool);

        if (pit == pools_.end())
            pit = pools_.find ("render");

        static std::uint64_t seq = 0;
        entry = std::make_shared<Entry>();
        entry->fn = std::move (fn);
        entry->priority = spec.priority;
        entry->seq = ++seq;
        entry->info.id = newId ("job");
        entry->info.kind = spec.kind;
        entry->info.projectId = spec.projectId;
        entry->info.tenant = spec.tenant;
        entry->info.pool = pit->first;
        entry->info.clientRequestId = spec.clientRequestId;
        entry->info.origin = spec.origin;
        entry->info.created = isoNow();
        entry->info.stage = "queued";
        jobs_[entry->info.id] = entry;

        if (! spec.clientRequestId.empty())
            byRequest_[spec.tenant + "|" + spec.projectId + "|" + spec.clientRequestId] = entry->info.id;

        if (stopping_)
        {
            entry->info.state = JobState::cancelled;
            entry->info.error = makeError (errc::cancelled, "the server is shutting down");
            entry->info.finished = isoNow();
        }
        else
        {
            pit->second->queue.push_back (entry);
            pit->second->cv.notify_one();
        }

        snapshot = entry->info;
        evictOldFinished();
    }

    persist (snapshot);
    report (snapshot);
    return { snapshot.id, true };
}

bool JobManager::cancel (const std::string& id)
{
    JobInfo snapshot;
    bool queuedCancel = false;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto it = jobs_.find (id);

        if (it == jobs_.end() || isFinished (it->second->info.state))
            return false;

        auto& e = it->second;
        e->cancel->store (true);

        if (e->info.state == JobState::queued)
        {
            auto& q = pools_[e->info.pool]->queue;
            q.erase (std::remove (q.begin(), q.end(), e), q.end());
            e->info.state = JobState::cancelled;
            e->info.error = makeError (errc::cancelled, "cancelled before it started");
            e->info.finished = isoNow();
            e->info.stage = "cancelled";
            e->fn = nullptr;
            queuedCancel = true;
            snapshot = e->info;
        }
    }

    if (queuedCancel)
    {
        cv_.notify_all();
        persist (snapshot);
        report (snapshot);
    }

    return true;
}

std::optional<JobInfo> JobManager::get (const std::string& id, std::chrono::milliseconds wait, const std::atomic<bool>* abort)
{
    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::unique_lock<std::mutex> lock (mutex_);

    for (;;)
    {
        auto it = jobs_.find (id);

        if (it == jobs_.end())
        {
            lock.unlock();

            if (! dir_.empty())
            {
                std::ifstream in (dir_ / (id + ".json"));
                auto j = json::parse (in, nullptr, false);

                if (! j.is_discarded())
                    return JobInfo::fromJson (j);
            }

            return std::nullopt;
        }

        if (isFinished (it->second->info.state) || wait.count() <= 0 || std::chrono::steady_clock::now() >= deadline || (abort != nullptr && abort->load()))
            return it->second->info;

        cv_.wait_until (lock, std::min (deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds (100)));
    }
}

std::vector<JobInfo> JobManager::list (const Filter& f) const
{
    std::vector<JobInfo> out;
    std::lock_guard<std::mutex> lock (mutex_);

    for (auto& [id, e] : jobs_)
    {
        if (! f.project.empty() && e->info.projectId != f.project)  continue;
        if (! f.tenant.empty() && e->info.tenant != f.tenant)       continue;
        if (f.state && e->info.state != *f.state)                   continue;
        if (! f.kind.empty() && e->info.kind != f.kind)             continue;
        out.push_back (e->info);
    }

    std::sort (out.begin(), out.end(), [] (const JobInfo& a, const JobInfo& b) { return a.id > b.id; });      // newest first (ULIDs sort by time)
    return out;
}

void JobManager::worker (Pool& pool)
{
    for (;;)
    {
        std::shared_ptr<Entry> e;
        JobInfo started;

        {
            std::unique_lock<std::mutex> lock (mutex_);
            pool.cv.wait (lock, [&] { return stopping_ || ! pool.queue.empty(); });

            if (pool.queue.empty())
                return;                         // stopping

            auto best = std::min_element (pool.queue.begin(), pool.queue.end(), [] (auto& a, auto& b)
            {
                return a->priority != b->priority ? a->priority > b->priority : a->seq < b->seq;
            });

            e = *best;
            pool.queue.erase (best);
            e->info.state = JobState::running;
            e->info.started = isoNow();
            e->info.stage = "starting";
            started = e->info;
        }

        cv_.notify_all();
        report (started);

        JobContext ctx (*this, e->info.id, e->cancel);
        Result<json> out = makeError (errc::internal, "job did not run");

        try
        {
            out = e->fn (ctx);
        }
        catch (const std::exception& ex)
        {
            out = makeError (errc::internal, std::string ("job threw: ") + ex.what());
        }
        catch (...)
        {
            out = makeError (errc::internal, "job threw");
        }

        JobInfo done;

        {
            std::lock_guard<std::mutex> lock (mutex_);
            auto& i = e->info;
            i.finished = isoNow();

            if (out.ok())
            {
                i.state = JobState::succeeded;
                i.progress = 1.0;
                i.stage = "done";
                i.result = std::move (out.value());
            }
            else if (out.error().code == errc::cancelled)
            {
                i.state = JobState::cancelled;
                i.stage = "cancelled";
                i.error = out.error();
            }
            else
            {
                i.state = JobState::failed;
                i.stage = "failed";
                i.error = out.error();
            }

            e->fn = nullptr;            // drop captured snapshots now
            done = i;
        }

        cv_.notify_all();
        persist (done);
        report (done);
    }
}

void JobManager::setProgress (const std::string& id, double fraction, const std::string& stage)
{
    JobInfo snapshot;
    bool announce = false;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto it = jobs_.find (id);

        if (it == jobs_.end() || it->second->info.state != JobState::running)
            return;

        auto& e = *it->second;
        const bool stageChanged = e.info.stage != stage;
        e.info.progress = std::max (e.info.progress, fraction);
        e.info.stage = stage;

        if (e.info.progress - e.lastReported >= 0.05 || stageChanged)
        {
            e.lastReported = e.info.progress;
            announce = true;
            snapshot = e.info;
        }
    }

    cv_.notify_all();

    if (announce)
        report (snapshot);
}

void JobManager::report (const JobInfo& i)
{
    if (onChange_)
        onChange_ (i);
}

void JobManager::persist (const JobInfo& i)
{
    if (dir_.empty())
        return;

    std::error_code ec;
    const auto tmp = dir_ / (i.id + ".json.tmp");
    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        out << i.toJson().dump() << "\n";
    }
    fs::rename (tmp, dir_ / (i.id + ".json"), ec);
}

void JobManager::evictOldFinished()
{
    // caller holds the lock (or is the constructor)
    if (jobs_.size() <= maxJobsInMemory)
        return;

    std::vector<std::string> finished;

    for (auto& [id, e] : jobs_)
        if (isFinished (e->info.state))
            finished.push_back (id);

    std::sort (finished.begin(), finished.end());

    for (size_t i = 0; jobs_.size() > maxJobsInMemory && i < finished.size(); ++i)
    {
        const auto& e = jobs_[finished[i]];

        if (! e->info.clientRequestId.empty())
            byRequest_.erase (e->info.tenant + "|" + e->info.projectId + "|" + e->info.clientRequestId);

        jobs_.erase (finished[i]);
    }
}

} // namespace rovox
