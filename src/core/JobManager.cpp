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
        case JobState::interrupted: return "interrupted";
    }

    return "failed";
}

static std::optional<JobState> parseState (const std::string& s)
{
    for (auto st : { JobState::queued, JobState::running, JobState::succeeded, JobState::failed, JobState::cancelled, JobState::interrupted })
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
    if (! callback.is_null())       j["callback"] = callback;
    return j;
}

static std::optional<JobInfo> parseJobInfo (const json& j)
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
    i.callback = j.value ("callback", json (nullptr));

    if (j.contains ("error") && j["error"].is_object())
        i.error = Error { j["error"].value ("code", errc::internal), j["error"].value ("message", ""), j["error"].value ("details", json (nullptr)) };

    return i;
}

std::optional<JobInfo> JobInfo::fromJson (const json& j)
{
    try
    {
        return parseJobInfo (j);
    }
    catch (const std::exception&)       // a record of the wrong shape (type_error): treated as unreadable, never a crash
    {
        return std::nullopt;
    }
}

struct JobManager::Entry
{
    JobInfo info;
    JobFn fn;
    int priority = 0;
    std::uint64_t seq = 0;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>> (false);
    double lastReported = 0;
    bool interrupted = false;           // under mutex_: cut short by drain(); a result other than success becomes `interrupted`
    std::function<void (const JobInfo&)> onFinished;
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
                e->fn = nullptr;

                if (! e->info.callback.is_null())           // nobody will deliver it any more: finalise its callback state
                    e->info.callback = { { "state", "callback_failed" }, { "attempts", 0 }, { "last_error", "shutdown" } };

                e->onFinished = nullptr;
                ++e->info.revision;
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

bool JobManager::draining() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return draining_;
}

int JobManager::drain (std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::pair<JobInfo, std::function<void (const JobInfo&)>>> queuedDone;
    int cut = 0;

    const auto runningCount = [this]
    {
        int n = 0;

        for (auto& [id, e] : jobs_)
            if (e->info.state == JobState::running)
                ++n;

        return n;
    };

    {
        std::unique_lock<std::mutex> lock (mutex_);

        if (stopping_)
            return 0;

        draining_ = true;

        // what has not started never will: it ends interrupted at once instead of delaying the stop
        for (auto& [name, p] : pools_)
        {
            for (auto& e : p->queue)
            {
                e->interrupted = true;
                e->cancel->store (true);
                e->info.state = JobState::interrupted;
                e->info.error = makeError (errc::interrupted, "the server stopped before this job started");
                e->info.stage = "interrupted";
                e->info.finished = isoNow();
                e->fn = nullptr;
                auto finishedFn = std::move (e->onFinished);
                e->onFinished = nullptr;
                JobInfo snap = e->info;
                snap.revision = ++e->info.revision;
                queuedDone.emplace_back (std::move (snap), std::move (finishedFn));
                ++cut;
            }

            p->queue.clear();
        }
    }

    cv_.notify_all();

    for (auto& [snap, finishedFn] : queuedDone)
    {
        persist (snap);
        report (snap);

        if (finishedFn)
            finishedFn (snap);
    }

    std::unique_lock<std::mutex> lock (mutex_);

    while (runningCount() > 0 && std::chrono::steady_clock::now() < deadline)
        cv_.wait_until (lock, std::min (deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds (100)));

    // the deadline passed: ask what still runs to stop; a job normally notices within a second
    for (auto& [id, e] : jobs_)
    {
        if (e->info.state == JobState::running)
        {
            e->interrupted = true;
            e->cancel->store (true);
            ++cut;
        }
    }

    const auto grace = std::chrono::steady_clock::now() + std::chrono::seconds (5);

    while (runningCount() > 0 && std::chrono::steady_clock::now() < grace)
        cv_.wait_until (lock, std::min (grace, std::chrono::steady_clock::now() + std::chrono::milliseconds (100)));

    return cut;
}

JobManager::Submitted JobManager::submit (JobSpec spec, JobFn fn)
{
    std::shared_ptr<Entry> entry;
    JobInfo snapshot;
    std::optional<Error> submittedError;

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
        entry->info.callback = spec.callback;
        entry->onFinished = std::move (spec.onFinished);
        entry->info.created = isoNow();
        entry->info.stage = "queued";
        jobs_[entry->info.id] = entry;

        if (! spec.clientRequestId.empty())
            byRequest_[spec.tenant + "|" + spec.projectId + "|" + spec.clientRequestId] = entry->info.id;

        std::optional<Error> refused;

        if (stopping_)
        {
            entry->info.state = JobState::cancelled;
            entry->info.error = makeError (errc::cancelled, "the server is shutting down");
            entry->info.finished = isoNow();
        }
        else if (draining_)
        {
            refused = makeError (errc::busy, "the server is draining: no new jobs are accepted");
            entry->info.state = JobState::failed;
            entry->info.error = refused;
            entry->info.stage = "failed";
            entry->info.finished = isoNow();
            entry->fn = nullptr;
            entry->onFinished = nullptr;
        }
        else
        {
            pit->second->queue.push_back (entry);
            pit->second->cv.notify_one();
        }

        snapshot = entry->info;
        snapshot.revision = ++entry->info.revision;
        evictOldFinished();
        submittedError = refused;
    }

    persist (snapshot);
    report (snapshot);
    return { snapshot.id, true, std::move (submittedError) };
}

bool JobManager::cancel (const std::string& id)
{
    JobInfo snapshot;
    bool queuedCancel = false;
    std::function<void (const JobInfo&)> finishedFn;

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
            finishedFn = std::move (e->onFinished);
            queuedCancel = true;
            snapshot = e->info;
            snapshot.revision = ++e->info.revision;
        }
    }

    if (queuedCancel)
    {
        cv_.notify_all();
        persist (snapshot);
        report (snapshot);

        if (finishedFn)
            finishedFn (snapshot);
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

            if (! dir_.empty() && isValidId (id, "job"))       // the id becomes part of a path: nothing but a real job id gets through
            {
                std::ifstream in (dir_ / (id + ".json"));
                auto j = json::parse (in, nullptr, false);

                if (! j.is_discarded())
                    if (auto info = JobInfo::fromJson (j); info && info->id == id)
                        return info;
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
            started.revision = ++e->info.revision;
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
        std::function<void (const JobInfo&)> finishedFn;

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
            else if (e->interrupted)
            {
                i.state = JobState::interrupted;
                i.stage = "interrupted";
                i.error = makeError (errc::interrupted, "the server stopped before this job finished");
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
            finishedFn = std::move (e->onFinished);
            done = i;
            done.revision = ++i.revision;
        }

        cv_.notify_all();
        persist (done);
        report (done);

        if (finishedFn)
            finishedFn (done);
    }
}

void JobManager::setCallbackState (const std::string& id, const json& state)
{
    JobInfo snapshot;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto it = jobs_.find (id);

        if (it == jobs_.end())
            return;

        it->second->info.callback = state;
        snapshot = it->second->info;
        snapshot.revision = ++it->second->info.revision;
    }

    cv_.notify_all();
    persist (snapshot);
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
            snapshot.revision = ++e.info.revision;
        }
    }

    cv_.notify_all();

    if (announce)
        report (snapshot);
}

void JobManager::report (const JobInfo& i)
{
    if (! onChange_)
        return;

    std::lock_guard<std::recursive_mutex> lock (reportMutex_);

    {
        std::vector<std::string> evicted;
        {
            std::lock_guard<std::mutex> jobsLock (mutex_);
            evicted.swap (evictedForReport_);
        }

        for (auto& id : evicted)
            reportedRevision_.erase (id);
    }

    auto& seen = reportedRevision_[i.id];

    // a stale snapshot (e.g. "queued" announced after the worker already finished the job) is never delivered
    if (i.revision != 0 && (i.revision < seen.first || (seen.second && ! isFinished (i.state))))
        return;

    seen = { std::max (seen.first, i.revision), seen.second || isFinished (i.state) };
    onChange_ (i);
}

void JobManager::persist (const JobInfo& i)
{
    if (dir_.empty())
        return;

    std::lock_guard<std::mutex> lock (persistMutex_);

    {
        std::vector<std::string> evicted;
        {
            std::lock_guard<std::mutex> jobsLock (mutex_);          // persistMutex_ -> mutex_ only; mutex_ holders never take persistMutex_
            evicted.swap (evictedForPersist_);
        }

        for (auto& id : evicted)
            persistedRevision_.erase (id);
    }

    // an older snapshot must never replace a newer one that another thread wrote first
    if (auto it = persistedRevision_.find (i.id); it != persistedRevision_.end() && it->second >= i.revision && i.revision != 0)
        return;

    std::error_code ec;
    const auto tmp = dir_ / (i.id + "." + std::to_string (tmpCounter_++) + ".json.tmp");
    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        out << i.toJson().dump() << "\n";
    }
    fs::rename (tmp, dir_ / (i.id + ".json"), ec);

    if (ec)
        fs::remove (tmp, ec);
    else
        persistedRevision_[i.id] = std::max (persistedRevision_[i.id], i.revision);
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

        evictedForReport_.push_back (finished[i]);
        evictedForPersist_.push_back (finished[i]);
        jobs_.erase (finished[i]);
    }
}

} // namespace rovox
