// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// review.* (E7-03): a clip is delivered to a person and the project waits for the answer. The record lives under
// <data>/reviews/<tenant>/<review_id>.json: pending -> approved | rejected | changes_requested | expired. The object is a cheap facade over those files
// (the waiters share one process-wide condition variable), so it can be made per call. Records are only visible to their tenant.
#pragma once

#include "Command.h"
#include "DeliveryService.h"
#include "JobManager.h"
#include "ProjectManager.h"

#include <atomic>
#include <functional>

namespace rovox
{

class Review
{
public:
    /** Runs a project command on the message thread as the calling client (the caller's context decides the tenant). */
    using CommandRunner = std::function<CommandResult (const std::string& projectId, const std::string& command, const json& params)>;

    struct RequestArgs
    {
        std::string tenant = "local", projectId, adapter, message, title, clientRequestId;
        json source = json::object();       // {"job_id": <succeeded preview or render job>} | {"file": "renders/mix.mp3"}
        bool checkpoint = false;            // remember the last accepted revision: a rejection rolls the agent's later steps back
        bool force = false;                 // deliver although the project is on hold
        int ttlS = 24 * 3600;               // pending -> expired after this long (1 s .. 7 days)
        int deliveryWaitMs = 0;             // wait this long for the delivery job before answering
        const std::atomic<bool>* abort = nullptr;
        json origin = nullptr;
    };

    Review (ProjectManager&, JobManager&, DeliveryService&, CommandRunner);

    /** Delivers the clip and records "pending". Respects delivery_hold (delivery.status "held"). Returns the public view, with "created". */
    Result<json> request (const RequestArgs&);

    /** verdict: approved | rejected | changes_requested. approved accepts the agent's changes (agent.accept up to the revision of the request);
        rejected with a checkpoint undoes the agent's steps after it unless a person edited in between (then it only suggests). */
    Result<json> resolve (const std::string& tenant, const std::string& reviewId, const std::string& verdict, const std::string& comment, const json& origin);

    Result<json> get (const std::string& tenant, const std::string& reviewId);
    /** Long poll: returns as soon as the review is no longer pending, or after `waitS` seconds (or when `abort` becomes true). */
    Result<json> wait (const std::string& tenant, const std::string& reviewId, double waitS, const std::atomic<bool>* abort = nullptr);
    Result<json> list (const std::string& tenant, const std::string& projectId, const std::string& status);

    /** Marks every pending review past its time as expired (all tenants); returns how many. Reads do the same for the record they touch. */
    int sweepExpired();

private:
    fs::path tenantDir (const std::string& tenant) const;
    json view (json record);
    json deliveryState (const json& record);
    json rollback (const std::string& projectId, std::uint64_t checkpoint);
    void announce (const std::string& type, const json& record);

    ProjectManager& projects_;
    JobManager& jobs_;
    DeliveryService& delivery_;
    CommandRunner run_;
};

} // namespace rovox
