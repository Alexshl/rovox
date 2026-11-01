// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// delivery.* (task 510): sends a render or preview to an adapter as a job, with the project's `delivery_hold` switch, idempotency
// (client_request_id), retries with backoff, a journal per project and entries in the project activity log.
#pragma once

#include "Delivery.h"
#include "JobManager.h"
#include "MessageThreadExecutor.h"
#include "ProjectManager.h"

namespace rovox
{

struct DeliverySendArgs
{
    std::string tenant = "local", projectId, adapter, caption, title, clientRequestId;
    json source = json::object();       // {"job_id": "job_..."} | {"file": "renders/mix.mp3"} (relative to the project)
    json options = json::object();
    bool force = false;                 // send although the project is on hold
    json origin = nullptr;
};

struct DeliveryStarted
{
    std::string jobId;                  // set when a send job was started
    json held = nullptr;                // set when the file was queued because of delivery_hold
    json existing = nullptr;            // set when client_request_id matched a finished delivery
};

class DeliveryService
{
public:
    /** `configFile`: TOML with [delivery.<kind>.<name>] tables (reloaded when it changes). */
    DeliveryService (ProjectManager&, JobManager&, MessageThreadExecutor&, fs::path configFile);

    json listAdapters();
    DeliveryRegistry& registry();       // tests add mock adapters

    Result<DeliveryStarted> send (const DeliverySendArgs&);
    /** Sends the held deliveries of a project (one id, or all). Each is sent exactly once. */
    Result<std::vector<DeliveryStarted>> release (const std::string& tenant, const std::string& projectId, const std::string& deliveryId, const json& origin);
    Result<json> list (const std::string& tenant, const std::string& projectId, const std::string& status);

    /** Sleep before retry n (1-based) in ms; tests shorten it. */
    void setRetryBackoffMs (int baseMs)     { backoffBaseMs_ = baseMs; }

private:
    struct Project { fs::path dir; bool hold = false; };
    Result<Project> projectInfo (const std::string& tenant, const std::string& projectId);
    Result<fs::path> resolveSource (const DeliverySendArgs&, const Project&);
    Result<std::string> startJob (const std::string& tenant, const std::string& projectId, const std::string& deliveryId, const json& origin);

    json readJournal (const fs::path& dir);
    void writeJournal (const fs::path& dir, const json&);
    json* findEntry (json& journal, const std::string& id);
    void logActivity (const std::string& tenant, const std::string& projectId, const std::string& what, const json& detail, bool ok);
    void maybeReload();

    ProjectManager& projects_;
    JobManager& jobs_;
    MessageThreadExecutor& executor_;
    fs::path configFile_;
    std::mutex mutex_;                  // journal files and registry
    DeliveryRegistry registry_;
    fs::file_time_type configTime_ {};
    bool configSeen_ = false;
    int backoffBaseMs_ = 1000;
};

} // namespace rovox
