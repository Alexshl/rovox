// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Event webhooks (E9-01): an EventHub observer that POSTs selected events of a tenant to that tenant's webhook, configured as
//     [tenants.<id>.webhooks]
//     url = "https://hooks.example.com/rovox"
//     secret_ref = "hook"                       # a [callbacks.secrets.<name>] entry
//     events = ["job.*", "render.done"]         # globs ('*', '?') on the event type; empty or missing = nothing is sent
// The config file is re-read when its mtime or size changes. Delivery is the job-callback machinery: validateCallback() (host in [callbacks] allow_hosts,
// no private address unless allow_private, connection pinned to the checked address), X-Rovox-Signature = "sha256=" + HMAC(secret, timestamp + "." + body),
// the retry schedule (1, 5, 30, 120 s; 5 attempts) and the dispatcher thread (JobCallbackDispatcher::enqueueBody). Delivery is at-least-once: de-duplicate
// by (event_id, type).
// Body: {event_id, type, tenant, project_id?, ts, data}. `data` is the event without its transport fields; keys that look like secrets are removed and a path below
// the tenant's project root is given relative to it (any other path below the host data directory is replaced by "[redacted]").
// Events published for tenant A only ever go to A's webhook. The observer only filters and enqueues: it never blocks a publisher. Per tenant at most
// `queueLimit` events wait (the oldest is dropped and "webhook.overflow" is logged), so a receiver that is down cannot grow memory.
// Event types seen on the hub are forwarded under their own name; additionally job.updated of a render job in a final state is also sent as render.done
// (data: job_id, kind, state, project_id, finished, error). project.saved, media.added and usage.recorded are forwarded when something publishes them.
#pragma once

#include "EventHub.h"
#include "JobCallback.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rovox
{

struct WebhookConfig
{
    std::string url, secretRef;
    std::vector<std::string> events;
};

/** The [tenants.<id>.webhooks] tables of a TOML file; a missing or unreadable file gives no webhooks. */
std::map<std::string, WebhookConfig> loadWebhookConfigs (const std::filesystem::path& tomlFile);

class EventWebhooks
{
public:
    struct Options
    {
        std::filesystem::path configFile;
        std::function<std::filesystem::path (const std::string& tenant)> tenantRoot;    // base of the relative project paths; optional
        std::filesystem::path hostRoot;                                                 // paths below it that are not below the tenant root are redacted
        HostResolver resolver;                                                          // tests; default getaddrinfo
        size_t queueLimit = 256;                                                        // per tenant
        std::chrono::seconds revalidateAfter { 60 };                                    // the checked address is looked up again after this
    };

    EventWebhooks (EventHub&, Options);
    ~EventWebhooks();

    void shutdown();

    /** The dispatcher shared by every tenant (tests: setDelayScale, setDeliverOverride). */
    JobCallbackDispatcher& dispatcher()     { return dispatcher_; }
    /** Events dropped because a queue was full. */
    long overflowCount() const              { return overflow_.load(); }
    /** Events dropped because the webhook failed validation (host not allowed, private address, unknown secret ...). */
    long refusedCount() const               { return refused_.load(); }

    /** '*' matches any run of characters, '?' one character; everything else literally. */
    static bool globMatch (const std::string& pattern, const std::string& text);

private:
    struct Pending
    {
        std::string id, type, body;
        std::string tenant;
        WebhookConfig config;
        CallbackSettings settings;
        std::uint64_t generation = 0;
    };

    struct Checked
    {
        CallbackTarget target;
        std::uint64_t generation = 0;
        std::chrono::steady_clock::time_point at;
        bool valid = false;
    };

    void onEvent (const Event&);
    void reloadLocked();
    json dataOf (const Event&, const json& source) const;
    void loop();
    void log (const std::string& line);

    EventHub& hub_;
    Options options_;
    JobCallbackDispatcher dispatcher_;
    int observerId_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, WebhookConfig> configs_;
    CallbackSettings settings_;
    std::uint64_t generation_ = 1, secretsGeneration_ = 0;
    bool configSeen_ = false;
    std::filesystem::file_time_type configTime_ {};
    std::uintmax_t configSize_ = 0;
    std::map<std::string, std::deque<Pending>> pending_;
    std::map<std::string, Checked> checked_;        // by tenant; the worker only
    bool stopping_ = false;
    std::atomic<long> overflow_ { 0 }, refused_ { 0 };
    std::thread thread_;
};

} // namespace rovox
