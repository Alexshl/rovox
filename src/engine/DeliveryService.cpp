// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "DeliveryService.h"

#include "Ids.h"
#include "ProjectFormat.h"

#include <fstream>
#include <thread>

namespace rovox
{

namespace
{
constexpr const char* journalFile = "delivery/journal.json";

bool isInside (const fs::path& root, const fs::path& file)
{
    std::error_code ec;
    const auto r = fs::weakly_canonical (root, ec), f = fs::weakly_canonical (file, ec);
    auto [a, b] = std::mismatch (r.begin(), r.end(), f.begin(), f.end());
    return a == r.end();
}
} // namespace

DeliveryService::DeliveryService (ProjectManager& p, JobManager& j, MessageThreadExecutor& e, fs::path configFile)
    : projects_ (p), jobs_ (j), executor_ (e), configFile_ (std::move (configFile)),
      registry_ (DeliveryRegistry::load (configFile_, p.host().dataDir() / "exports"))
{
    std::error_code ec;
    configSeen_ = fs::exists (configFile_, ec);

    if (configSeen_)
        configTime_ = fs::last_write_time (configFile_, ec);
}

void DeliveryService::maybeReload()
{
    std::error_code ec;
    const bool exists = fs::exists (configFile_, ec);
    const auto time = exists ? fs::last_write_time (configFile_, ec) : fs::file_time_type {};

    if (exists != configSeen_ || (exists && time != configTime_))
    {
        configSeen_ = exists;
        configTime_ = time;
        registry_ = DeliveryRegistry::load (configFile_, projects_.host().dataDir() / "exports");
    }
}

DeliveryRegistry& DeliveryService::registry()
{
    return registry_;
}

json DeliveryService::listAdapters()
{
    std::lock_guard<std::mutex> lock (mutex_);
    maybeReload();
    json out = json::array();

    for (auto& a : registry_.all())
        out.push_back (a->describe());

    json result { { "adapters", out } };

    if (! registry_.warnings().empty())
        result["config_warnings"] = registry_.warnings();

    return result;
}

//==============================================================================
Result<DeliveryService::Project> DeliveryService::projectInfo (const std::string& tenant, const std::string& projectId)
{
    auto fut = executor_.run ([&]() -> Result<Project>
    {
        auto* p = projects_.find (projectId, tenant);

        if (p == nullptr)
            return makeError (errc::notFound, "no open project " + projectId);

        static const juce::Identifier holdId ("rovox:deliveryHold");
        return Project { p->dir(), (bool) p->edit().state.getProperty (holdId, false) };
    });

    if (fut.wait_for (std::chrono::seconds (60)) != std::future_status::ready)
        return makeError (errc::busy, "the message thread did not answer in time");

    return fut.get();
}

Result<fs::path> DeliveryService::resolveSource (const DeliverySendArgs& a, const Project& project)
{
    fs::path file;

    if (a.source.contains ("job_id"))
    {
        auto info = jobs_.get (a.source["job_id"].get<std::string>());

        if (! info || info->tenant != a.tenant || info->projectId != a.projectId)
            return makeError (errc::notFound, "no job " + a.source["job_id"].get<std::string>() + " in this project");

        if (info->state != JobState::succeeded)
            return makeError (errc::preconditionFailed, "job " + info->id + " has not succeeded (state " + toString (info->state) + "): there is no file to deliver");

        const auto& result = info->result;

        if (result.is_object() && result.contains ("file") && result["file"].is_object())
            file = result["file"].value ("abs_path", "");
        else if (result.is_object() && result.contains ("clip") && result["clip"].is_object())
            file = result["clip"].value ("abs_path", "");
        else if (result.is_object() && result.contains ("files") && result["files"].is_array() && ! result["files"].empty())
            file = result["files"][0].value ("abs_path", "");

        if (file.empty())
            return makeError (errc::preconditionFailed, "job " + info->id + " produced no file (only renders and previews do)");
    }
    else if (a.source.contains ("file"))
    {
        const fs::path rel = a.source["file"].get<std::string>();

        if (rel.is_absolute() || rel.string().find ("..") != std::string::npos)
            return makeError (errc::invalidParams, "source.file is a path inside the project, such as renders/mix.mp3", json { { "path", "/source/file" } });

        file = project.dir / rel;

        if (auto* storage = projects_.storage(); storage != nullptr && rel.begin() != rel.end() && *rel.begin() == "renders")
            if (std::error_code probe; ! fs::exists (file, probe))
                storage->fetchRender (a.tenant, a.projectId, rel.filename().string(), file);
    }
    else
    {
        return makeError (errc::invalidParams, "source is {\"job_id\": ...} (a render or preview job) or {\"file\": \"renders/<name>\"}", json { { "path", "/source" } });
    }

    std::error_code ec;

    if (! fs::is_regular_file (file, ec))
        return makeError (errc::notFound, "the file to deliver does not exist: " + file.filename().string());

    if (! isInside (project.dir, file))
        return makeError (errc::forbiddenPath, "only files of the project can be delivered");

    return file;
}

json DeliveryService::readJournal (const fs::path& dir)
{
    std::ifstream in (dir / journalFile);

    if (! in)
        return json::array();

    auto j = json::parse (in, nullptr, false);
    return j.is_array() ? j : json::array();
}

void DeliveryService::writeJournal (const fs::path& dir, const json& j)
{
    std::error_code ec;
    fs::create_directories (dir / "delivery", ec);
    writeFileAtomic (dir / journalFile, j.dump (1), false);
}

json* DeliveryService::findEntry (json& journal, const std::string& id)
{
    for (auto& e : journal)
        if (e.value ("delivery_id", "") == id)
            return &e;

    return nullptr;
}

void DeliveryService::logActivity (const std::string& tenant, const std::string& projectId, const std::string& what, const json& detail, bool ok)
{
    // Waits for the message thread (never fire and forget: the closure would outlive this service). Called from job and transport threads only,
    // or from the message thread itself, where the executor runs the closure at once.
    auto done = executor_.run ([=, this]
    {
        if (auto* p = projects_.find (projectId, tenant))
            p->activity().add ({ { "t", nowIso8601() }, { "origin", { { "kind", "system" }, { "session", "" }, { "label", "delivery" } } }, { "name", what },
                                 { "params", ActivityLog::redact (detail) }, { "ok", ok }, { "revision", p->revision() }, { "mutating", false } });
    });

    done.wait_for (std::chrono::seconds (10));
}

//==============================================================================
namespace
{
json publicEntry (json e)
{
    e.erase ("file_path");
    e.erase ("options");
    return e;
}
} // namespace

Result<DeliveryStarted> DeliveryService::send (const DeliverySendArgs& a)
{
    std::unique_lock<std::mutex> lock (mutex_);
    maybeReload();

    auto adapter = registry_.find (a.adapter);

    if (adapter == nullptr)
    {
        json ids = json::array();

        for (auto& x : registry_.all())
            ids.push_back (x->id());

        return makeError (errc::notFound, "no delivery adapter " + a.adapter, json { { "adapters", ids } });
    }

    lock.unlock();
    auto project = projectInfo (a.tenant, a.projectId);

    if (! project.ok())
        return project.error();

    auto file = resolveSource (a, project.value());

    if (! file.ok())
        return file.error();

    lock.lock();
    auto journal = readJournal (project.value().dir);

    if (! a.clientRequestId.empty())
        for (auto& e : journal)
            if (e.value ("client_request_id", "") == a.clientRequestId && e.value ("adapter", "") == a.adapter)
            {
                DeliveryStarted s;
                s.existing = publicEntry (e);
                return s;
            }

    json entry { { "delivery_id", newId ("dlv") }, { "adapter", a.adapter }, { "status", "pending" }, { "file", file.value().filename().string() },
                 { "file_path", file.value().string() }, { "caption", a.caption }, { "title", a.title }, { "options", a.options }, { "client_request_id", a.clientRequestId },
                 { "created_at", nowIso8601() }, { "attempts", 0 }, { "source", a.source }, { "origin", a.origin } };
    DeliveryStarted started;

    if (project.value().hold && ! a.force)
    {
        entry["status"] = "held";
        journal.push_back (entry);
        writeJournal (project.value().dir, journal);
        lock.unlock();
        started.held = publicEntry (entry);
        logActivity (a.tenant, a.projectId, "delivery.held", { { "delivery_id", entry["delivery_id"] }, { "adapter", a.adapter }, { "file", entry["file"] } }, true);
        projects_.hub().publish (a.tenant, a.projectId, { { "type", "delivery.updated" }, { "project_id", a.projectId }, { "delivery", started.held } });
        return started;
    }

    journal.push_back (entry);
    writeJournal (project.value().dir, journal);
    lock.unlock();
    auto job = startJob (a.tenant, a.projectId, entry["delivery_id"].get<std::string>(), a.origin);

    if (! job.ok())
        return job.error();

    started.jobId = job.value();
    return started;
}

Result<std::vector<DeliveryStarted>> DeliveryService::release (const std::string& tenant, const std::string& projectId, const std::string& deliveryId, const json& origin)
{
    auto project = projectInfo (tenant, projectId);

    if (! project.ok())
        return project.error();

    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto journal = readJournal (project.value().dir);

        for (auto& e : journal)
        {
            if (e.value ("status", "") != "held" || (! deliveryId.empty() && e.value ("delivery_id", "") != deliveryId))
                continue;

            e["status"] = "pending";            // taken out of the held state under the lock: a second release cannot pick it up again
            ids.push_back (e["delivery_id"].get<std::string>());
        }

        if (! deliveryId.empty() && ids.empty())
        {
            if (findEntry (journal, deliveryId) == nullptr)
                return makeError (errc::notFound, "no delivery " + deliveryId);

            return makeError (errc::preconditionFailed, "delivery " + deliveryId + " is not held (it was released or sent already)");
        }

        writeJournal (project.value().dir, journal);
    }

    std::vector<DeliveryStarted> started;

    for (auto& id : ids)
    {
        auto job = startJob (tenant, projectId, id, origin);

        if (! job.ok())
            return job.error();

        DeliveryStarted s;
        s.jobId = job.value();
        started.push_back (s);
    }

    return started;
}

Result<json> DeliveryService::list (const std::string& tenant, const std::string& projectId, const std::string& status)
{
    auto project = projectInfo (tenant, projectId);

    if (! project.ok())
        return project.error();

    std::lock_guard<std::mutex> lock (mutex_);
    json out = json::array();

    for (auto& e : readJournal (project.value().dir))
        if (status.empty() || e.value ("status", "") == status)
            out.push_back (publicEntry (e));

    return json { { "deliveries", out }, { "hold", project.value().hold } };
}

Result<std::string> DeliveryService::startJob (const std::string& tenant, const std::string& projectId, const std::string& deliveryId, const json& origin)
{
    JobSpec spec;
    spec.kind = "delivery";
    spec.projectId = projectId;
    spec.tenant = tenant;
    spec.pool = "io";
    spec.origin = origin;

    auto submitted = jobs_.submit (std::move (spec), [this, tenant, projectId, deliveryId] (JobContext& jc) -> Result<json>
    {
        auto project = projectInfo (tenant, projectId);

        if (! project.ok())
            return project.error();

        const auto dir = project.value().dir;
        std::shared_ptr<DeliveryAdapter> adapter;
        DeliveryRequest request;
        {
            std::lock_guard<std::mutex> lock (mutex_);
            auto journal = readJournal (dir);
            auto* e = findEntry (journal, deliveryId);

            if (e == nullptr)
                return makeError (errc::notFound, "no delivery " + deliveryId);

            if (e->value ("status", "") != "pending")
                return makeError (errc::preconditionFailed, "delivery " + deliveryId + " is already " + e->value ("status", "?"));

            adapter = registry_.find (e->value ("adapter", ""));

            if (adapter == nullptr)
                return makeError (errc::notFound, "the adapter " + e->value ("adapter", "") + " is no longer configured");

            request.file = e->value ("file_path", "");
            request.caption = e->value ("caption", "");
            request.title = e->value ("title", "");
            request.options = e->value ("options", json::object());
            (*e)["status"] = "sending";
            writeJournal (dir, journal);
        }

        jc.progress (0.1, "sending to " + adapter->id());
        const int maxAttempts = 3;
        Result<json> res = makeError (errc::internal, "not sent");
        int attempt = 0;

        for (; attempt < maxAttempts; ++attempt)
        {
            res = adapter->send (request, [&jc] { return jc.cancelled(); });

            if (res.ok() || res.error().code != errc::busy || jc.cancelled())
                break;

            for (int waited = 0; waited < backoffBaseMs_ * (1 << attempt) && ! jc.cancelled(); waited += 20)
                std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        json out;
        {
            std::lock_guard<std::mutex> lock (mutex_);
            auto journal = readJournal (dir);

            if (auto* e = findEntry (journal, deliveryId))
            {
                (*e)["attempts"] = attempt + 1;
                (*e)["finished_at"] = nowIso8601();

                if (res.ok())
                {
                    (*e)["status"] = "sent";
                    (*e)["receipt"] = res.value();
                }
                else
                {
                    (*e)["status"] = "failed";
                    (*e)["error"] = res.error().toJson();
                }

                out = publicEntry (*e);
                writeJournal (dir, journal);
            }
        }

        logActivity (tenant, projectId, res.ok() ? "delivery.sent" : "delivery.failed",
                     { { "delivery_id", deliveryId }, { "adapter", adapter->id() }, { "file", request.file.filename().string() }, { "attempts", attempt + 1 },
                       { "error", res.ok() ? json (nullptr) : json (res.error().message) } }, res.ok());
        projects_.hub().publish (tenant, projectId, { { "type", "delivery.updated" }, { "project_id", projectId }, { "delivery", out } });

        if (! res.ok())
            return Error { res.error().code == errc::busy ? "DELIVERY_FAILED" : res.error().code, res.error().message, json { { "delivery", out } } };

        return out;
    });

    return submitted.id;
}

} // namespace rovox
