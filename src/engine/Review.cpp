// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Review.h"

#include "Ids.h"
#include "ProjectFormat.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace rovox
{

namespace
{
std::mutex gMutex;                      // record files; also the lock of gCv
std::condition_variable gCv;            // every state change wakes every waiter (they re-read the file)

constexpr int maxTtlS = 7 * 24 * 3600;

std::int64_t epochNow()
{
    return std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string isoFromEpoch (std::int64_t t)
{
    const std::time_t tt = (std::time_t) t;
    std::tm tm {};
    gmtime_r (&tt, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::optional<json> readRecord (const fs::path& file)
{
    std::ifstream in (file);

    if (! in)
        return std::nullopt;

    auto j = json::parse (in, nullptr, false);

    if (! j.is_object())
        return std::nullopt;

    return j;
}

void writeRecord (const fs::path& file, const json& record)
{
    std::error_code ec;
    fs::create_directories (file.parent_path(), ec);
    writeFileAtomic (file, record.dump (1), false);
}

/** pending past its time becomes expired. */
bool expireIfDue (json& record)
{
    if (record.value ("status", "") != "pending" || epochNow() < record.value ("expires_at_epoch", std::int64_t (0)))
        return false;

    record["status"] = "expired";
    record["resolved_at"] = nowIso8601();
    return true;
}

bool terminalDelivery (const std::string& status)
{
    return status == "sent" || status == "failed";
}

/** What a client may see: no internal bookkeeping. */
json publicBase (json record)
{
    for (auto* key : { "expires_at_epoch", "delivery_job_id", "source", "client_request_id" })
        record.erase (key);

    return record;
}

Error errorOf (const CommandResult& r)
{
    return r.error ? *r.error : makeError (errc::internal, "the command failed");
}
} // namespace

Review::Review (ProjectManager& p, JobManager& j, DeliveryService& d, CommandRunner r)
    : projects_ (p), jobs_ (j), delivery_ (d), run_ (std::move (r))
{
}

fs::path Review::tenantDir (const std::string& tenant) const
{
    return projects_.host().dataDir() / "reviews" / tenant;
}

void Review::announce (const std::string& type, const json& record)
{
    gCv.notify_all();
    projects_.hub().publish (record.value ("tenant_id", "local"), record.value ("project_id", ""),
                             { { "type", type }, { "project_id", record.value ("project_id", "") }, { "review", publicBase (record) } });
}

//==============================================================================
json Review::deliveryState (const json& record)
{
    json d = record.value ("delivery", json::object());

    if (terminalDelivery (d.value ("status", "")))
        return d;

    const auto jobId = record.value ("delivery_job_id", "");

    if (! jobId.empty())
    {
        auto info = jobs_.get (jobId);

        if (! info)
            return d;

        if (info->state == JobState::succeeded)
        {
            d["status"] = "sent";

            if (info->result.is_object())
            {
                d["delivery_id"] = info->result.value ("delivery_id", "");
                d["receipt"] = info->result.value ("receipt", json (nullptr));
            }
        }
        else if (info->state == JobState::failed || info->state == JobState::cancelled)
        {
            d["status"] = "failed";
            d["error"] = info->error ? json { { "code", info->error->code }, { "message", info->error->message } }
                                     : json { { "code", errc::cancelled }, { "message", "the delivery was cancelled" } };
        }

        return d;
    }

    const auto deliveryId = d.value ("delivery_id", "");

    if (d.value ("status", "") == "held" && ! deliveryId.empty())
    {
        auto l = delivery_.list (record.value ("tenant_id", "local"), record.value ("project_id", ""), "");

        if (l.ok())
            for (auto& e : l.value()["deliveries"])
                if (e.value ("delivery_id", "") == deliveryId)
                {
                    d["status"] = e.value ("status", "held");

                    if (e.contains ("receipt"))
                        d["receipt"] = e["receipt"];

                    if (e.contains ("error"))
                        d["error"] = e["error"];
                }
    }

    return d;
}

json Review::view (json record)
{
    auto d = deliveryState (record);

    if (terminalDelivery (d.value ("status", "")) && d != record.value ("delivery", json::object()))
    {
        std::lock_guard<std::mutex> lock (gMutex);
        const auto file = tenantDir (record.value ("tenant_id", "local")) / (record.value ("review_id", "") + ".json");

        if (auto current = readRecord (file))
        {
            (*current)["delivery"] = d;
            writeRecord (file, *current);
        }
    }

    record["delivery"] = d;
    return publicBase (std::move (record));
}

//==============================================================================
Result<json> Review::request (const RequestArgs& a)
{
    if (! isValidTenantId (a.tenant))
        return makeError (errc::invalidParams, "invalid tenant");

    if (a.adapter.empty())
        return makeError (errc::invalidParams, "deliver_to is a delivery adapter id from delivery.list_adapters", json { { "path", "/deliver_to" } });

    if (a.ttlS < 1 || a.ttlS > maxTtlS)
        return makeError (errc::invalidParams, "ttl_s is 1 .. 604800 seconds", json { { "path", "/ttl_s" } });

    if (! a.clientRequestId.empty())
    {
        std::optional<json> existing;
        {
            std::lock_guard<std::mutex> lock (gMutex);
            std::error_code ec;

            for (fs::directory_iterator it (tenantDir (a.tenant), ec), end; ! ec && it != end; it.increment (ec))
                if (auto r = readRecord (it->path()))
                    if (r->value ("project_id", "") == a.projectId && r->value ("client_request_id", "") == a.clientRequestId)
                        existing = *r;
        }

        if (existing)
        {
            auto v = view (*existing);
            v["created"] = false;
            return v;
        }
    }

    json checkpointRevision = nullptr;

    if (a.checkpoint)
    {
        // agent.accept up to 0 changes nothing and answers with the last accepted revision; it also proves the project belongs to the caller
        auto accepted = run_ (a.projectId, "agent.accept", { { "up_to_revision", 0 } });

        if (! accepted.ok)
            return errorOf (accepted);

        checkpointRevision = accepted.data.value ("accepted_revision", 0);
    }

    json clip = nullptr, loudness = nullptr;

    if (a.source.is_object() && a.source.contains ("job_id") && a.source["job_id"].is_string())
        if (auto info = jobs_.get (a.source["job_id"].get<std::string>()))
            if (info->tenant == a.tenant && info->projectId == a.projectId && info->result.is_object())
            {
                if (info->result.contains ("clip") && info->result["clip"].is_object())
                {
                    clip = info->result["clip"];
                    clip.erase ("abs_path");
                }

                if (info->result.contains ("loudness") && info->result["loudness"].is_object())
                    loudness = info->result["loudness"];
            }

    std::string caption = a.message;

    if (loudness.is_object() && loudness.contains ("integrated_lufs") && loudness["integrated_lufs"].is_number())
    {
        char buf[64];
        std::snprintf (buf, sizeof (buf), "%.1f LUFS", loudness["integrated_lufs"].get<double>());
        caption += (caption.empty() ? "" : "\n") + std::string (buf);

        if (loudness.contains ("true_peak_dbtp") && loudness["true_peak_dbtp"].is_number())
        {
            std::snprintf (buf, sizeof (buf), ", %.1f dBTP", loudness["true_peak_dbtp"].get<double>());
            caption += buf;
        }
    }

    DeliverySendArgs send;
    send.tenant = a.tenant;
    send.projectId = a.projectId;
    send.adapter = a.adapter;
    send.caption = caption;
    send.title = a.title.empty() ? "Rovox review" : a.title;
    send.source = a.source;
    send.force = a.force;
    send.origin = a.origin;
    auto started = delivery_.send (send);

    if (! started.ok())
        return started.error();

    json delivery { { "adapter", a.adapter }, { "status", "sending" } };
    std::string jobId;

    if (! started.value().held.is_null())
    {
        delivery["status"] = "held";
        delivery["delivery_id"] = started.value().held.value ("delivery_id", "");
    }
    else if (! started.value().existing.is_null())
    {
        delivery["status"] = started.value().existing.value ("status", "sent");
        delivery["delivery_id"] = started.value().existing.value ("delivery_id", "");
        delivery["receipt"] = started.value().existing.value ("receipt", json (nullptr));
    }
    else
    {
        jobId = started.value().jobId;
        delivery["job_id"] = jobId;

        if (a.deliveryWaitMs > 0)
            jobs_.get (jobId, std::chrono::milliseconds (a.deliveryWaitMs), a.abort);
    }

    const auto now = epochNow();
    json record { { "review_id", newId ("rv") }, { "tenant_id", a.tenant }, { "project_id", a.projectId }, { "status", "pending" },
                  { "created_at", nowIso8601() }, { "expires_at", isoFromEpoch (now + a.ttlS) }, { "expires_at_epoch", now + a.ttlS },
                  { "message", a.message }, { "adapter", a.adapter }, { "checkpoint", a.checkpoint }, { "checkpoint_revision", checkpointRevision },
                  { "revision_at_request", projects_.revisionOf (a.projectId).value_or (0) }, { "clip", clip }, { "loudness", loudness },
                  { "delivery", delivery }, { "delivery_job_id", jobId }, { "client_request_id", a.clientRequestId }, { "source", a.source }, { "origin", a.origin } };
    {
        std::lock_guard<std::mutex> lock (gMutex);
        writeRecord (tenantDir (a.tenant) / (record["review_id"].get<std::string>() + ".json"), record);
    }

    announce ("review.requested", record);
    auto v = view (record);
    v["created"] = true;
    return v;
}

//==============================================================================
json Review::rollback (const std::string& projectId, std::uint64_t checkpoint)
{
    auto h = run_ (projectId, "history.list", json::object());

    if (! h.ok)
        return { { "performed", false }, { "reason", errorOf (h).message } };

    const json undo = h.data.value ("undo", json::array());
    size_t steps = 0;
    bool person = false;

    for (auto it = undo.rbegin(); it != undo.rend(); ++it)
    {
        if (it->value ("revision", std::uint64_t (0)) <= checkpoint)
            break;

        const auto kind = it->contains ("origin") && (*it)["origin"].is_object() ? (*it)["origin"].value ("kind", "") : std::string();
        person = person || kind == "ui" || kind == "cli";
        ++steps;
    }

    if (steps == 0)
        return { { "performed", false }, { "undone_steps", 0 }, { "reason", "no steps after the checkpoint" } };

    if (person)
        return { { "performed", false }, { "suggested_undo_steps", steps }, { "reason", "a person edited the project after the checkpoint: undo is suggested, not done" } };

    std::uint64_t revision = 0;

    for (size_t i = 0; i < steps; ++i)
    {
        auto r = run_ (projectId, "undo", json::object());

        if (! r.ok)
            return { { "performed", false }, { "undone_steps", i }, { "reason", errorOf (r).message } };

        revision = r.revision;
    }

    return { { "performed", true }, { "undone_steps", steps }, { "revision", revision } };
}

Result<json> Review::resolve (const std::string& tenant, const std::string& reviewId, const std::string& verdict, const std::string& comment, const json& origin)
{
    if (verdict != "approved" && verdict != "rejected" && verdict != "changes_requested")
        return makeError (errc::invalidParams, "verdict is approved, rejected or changes_requested", json { { "path", "/verdict" } });

    if (! isValidTenantId (tenant) || ! isValidId (reviewId, "rv"))
        return makeError (errc::notFound, "no review " + reviewId);

    const auto file = tenantDir (tenant) / (reviewId + ".json");
    json record;
    bool expired = false, same = false;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        auto r = readRecord (file);

        if (! r || r->value ("tenant_id", "") != tenant)
            return makeError (errc::notFound, "no review " + reviewId);

        expired = expireIfDue (*r);

        if (expired)
            writeRecord (file, *r);

        const auto status = r->value ("status", "");

        if (status == "pending")
        {
            (*r)["status"] = verdict;
            (*r)["resolved_at"] = nowIso8601();
            (*r)["comment"] = truncateUtf8 (comment, 2000);
            (*r)["resolved_by"] = origin;
            writeRecord (file, *r);
        }
        else
        {
            same = status == verdict;
        }

        record = *r;
    }

    if (expired)
        announce ("review.expired", record);

    if (record.value ("status", "") != verdict || (expired && ! same))
        return makeError (errc::preconditionFailed, "review " + reviewId + " is already " + record.value ("status", ""), json { { "status", record["status"] } });

    if (same)
    {
        auto v = view (record);
        v["changed"] = false;
        return v;
    }

    // the state is "taken" under the lock above, so a second answer cannot repeat the side effects
    const auto projectId = record.value ("project_id", "");

    if (verdict == "approved")
    {
        auto r = run_ (projectId, "agent.accept", { { "up_to_revision", record.value ("revision_at_request", std::uint64_t (0)) } });
        record["accepted_revision"] = r.ok ? json (r.data.value ("accepted_revision", 0)) : json (nullptr);

        if (! r.ok)
            record["accept_error"] = errorOf (r).message;
    }
    else if (verdict == "rejected" && record.value ("checkpoint", false) && record["checkpoint_revision"].is_number())
    {
        record["rollback"] = rollback (projectId, record["checkpoint_revision"].get<std::uint64_t>());
    }

    {
        std::lock_guard<std::mutex> lock (gMutex);

        if (auto current = readRecord (file))
        {
            for (auto* key : { "accepted_revision", "accept_error", "rollback" })
                if (record.contains (key))
                    (*current)[key] = record[key];

            writeRecord (file, *current);
        }
    }

    announce ("review.resolved", record);
    auto v = view (record);
    v["changed"] = true;
    return v;
}

//==============================================================================
Result<json> Review::get (const std::string& tenant, const std::string& reviewId)
{
    if (! isValidTenantId (tenant) || ! isValidId (reviewId, "rv"))
        return makeError (errc::notFound, "no review " + reviewId);

    const auto file = tenantDir (tenant) / (reviewId + ".json");
    json record;
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        auto r = readRecord (file);

        if (! r || r->value ("tenant_id", "") != tenant)
            return makeError (errc::notFound, "no review " + reviewId);

        expired = expireIfDue (*r);

        if (expired)
            writeRecord (file, *r);

        record = *r;
    }

    if (expired)
        announce ("review.expired", record);

    return view (record);
}

Result<json> Review::wait (const std::string& tenant, const std::string& reviewId, double waitS, const std::atomic<bool>* abort)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds ((int) (std::max (0.0, waitS) * 1000));

    for (;;)
    {
        auto r = get (tenant, reviewId);

        if (! r.ok() || r.value().value ("status", "") != "pending")
            return r;

        const auto left = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - std::chrono::steady_clock::now());

        if (left.count() <= 0 || (abort != nullptr && abort->load()))
            return r;

        std::unique_lock<std::mutex> lock (gMutex);
        gCv.wait_for (lock, std::min (left, std::chrono::milliseconds (250)));
    }
}

Result<json> Review::list (const std::string& tenant, const std::string& projectId, const std::string& status)
{
    if (! isValidTenantId (tenant))
        return makeError (errc::invalidParams, "invalid tenant");

    std::vector<json> found, expiredNow;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        std::error_code ec;

        for (fs::directory_iterator it (tenantDir (tenant), ec), end; ! ec && it != end; it.increment (ec))
        {
            if (it->path().extension() != ".json")
                continue;

            auto r = readRecord (it->path());

            if (! r || r->value ("tenant_id", "") != tenant)
                continue;

            if (expireIfDue (*r))
            {
                writeRecord (it->path(), *r);
                expiredNow.push_back (*r);
            }

            if ((projectId.empty() || r->value ("project_id", "") == projectId) && (status.empty() || r->value ("status", "") == status))
                found.push_back (*r);
        }
    }

    for (auto& r : expiredNow)
        announce ("review.expired", r);

    std::sort (found.begin(), found.end(), [] (const json& x, const json& y) { return x.value ("review_id", "") > y.value ("review_id", ""); });
    json out = json::array();

    for (auto& r : found)
        out.push_back (publicBase (r));

    return json { { "reviews", out } };
}

int Review::sweepExpired()
{
    std::vector<json> expiredNow;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        std::error_code ec;

        for (fs::directory_iterator tenants (projects_.host().dataDir() / "reviews", ec), end; ! ec && tenants != end; tenants.increment (ec))
        {
            std::error_code ec2;

            for (fs::directory_iterator it (tenants->path(), ec2), end2; ! ec2 && it != end2; it.increment (ec2))
                if (it->path().extension() == ".json")
                    if (auto r = readRecord (it->path()))
                        if (expireIfDue (*r))
                        {
                            writeRecord (it->path(), *r);
                            expiredNow.push_back (*r);
                        }
        }
    }

    for (auto& r : expiredNow)
        announce ("review.expired", r);

    return (int) expiredNow.size();
}

} // namespace rovox
