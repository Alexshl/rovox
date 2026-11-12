// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Completion callback of a job (task 220): when a job that was started with callback {url, secret_ref} finishes, the server POSTs the job as JSON to
// the URL, signed like a delivery webhook: X-Rovox-Signature = "sha256=" + HMAC-SHA256(secret, timestamp + "." + body), X-Rovox-Timestamp = unix seconds.
// A failing callback is tried up to 5 times, pausing 1, 5, 30 and 120 s between the attempts; then it is `callback_failed` while the job itself stays as it finished.
// The URL must be http(s), its host listed in [callbacks] allow_hosts, and the address it resolves to must not be loopback / private / link-local
// unless [callbacks] allow_private = true. The connection goes to the address that was checked. Secrets are read from [callbacks.secrets.<name>]
// (env = "VAR" or file = "path") and never appear in a result, an error or a log line.
#pragma once

#include "Delivery.h"
#include "Result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Client; }

namespace rovox
{

struct JobInfo;

struct CallbackSettings
{
    std::vector<std::string> allowHosts;                // exact host names (case-insensitive); empty = no callback is accepted
    bool allowPrivate = false;
    std::map<std::string, SecretRef> secrets;           // by name, for callback.secret_ref

    /** Reads the [callbacks] table of a TOML file; a missing or unreadable file gives the defaults (everything rejected). */
    static CallbackSettings load (const std::filesystem::path& tomlFile);
};

struct CallbackTarget
{
    std::string url, secretName, resolvedIp;
};

/** True for loopback, private (RFC 1918), link-local, unique-local (fc00::/7), unspecified, multicast, carrier-grade NAT (100.64/10) and benchmarking (198.18/15) addresses;
    IPv4-compatible (::a.b.c.d) always, IPv4-mapped, NAT64 (64:ff9b::/96) and 6to4 (2002::/16) by the IPv4 address they carry. */
bool isPrivateAddress (const std::string& ip);

/** Resolves a host to numeric addresses; the default is getaddrinfo. Tests inject one. */
using HostResolver = std::function<std::vector<std::string> (const std::string& host, int port)>;

/** Checks callback {url, secret_ref} of render.start: INVALID_PARAMS for a bad shape, a non-http(s) URL, a host that is not allowed,
    an unresolvable host, a private address (unless allowed) or an unknown / unset secret. On success the host is resolved to the address to connect to. */
Result<CallbackTarget> validateCallback (const json& callback, const CallbackSettings&, const HostResolver& resolver = nullptr);

class JobCallbackDispatcher
{
public:
    /** `onState` gets {state: pending | delivered | callback_failed, attempts, ...} after every attempt. */
    using StateFn = std::function<void (const std::string& jobId, const json& state)>;

    explicit JobCallbackDispatcher (StateFn onState, std::map<std::string, SecretRef> secrets = {});
    ~JobCallbackDispatcher();

    /** Makes the retry delays `scale` times as long (1 = real: 1, 5, 30, 120 s); tests use a tiny value. */
    void setDelayScale (double scale);
    void setSecrets (std::map<std::string, SecretRef>);

    /** Posts the finished job (without its `callback` field) to the target, retrying as described above. */
    void enqueue (const JobInfo& finished, CallbackTarget);

    /** The general form: posts `body` (JSON text) signed like a job callback, with `event` as X-Rovox-Event. `id` is what `onState` reports.
        With a non-empty `group` and `maxPerGroup` > 0, at most that many tasks of the group wait at once: the oldest waiting one is dropped
        (reported as callback_failed with last_error "overflow") so that a dead receiver cannot grow memory. Never blocks. */
    void enqueueBody (const std::string& id, const std::string& event, std::string body, CallbackTarget, const std::string& group = {}, size_t maxPerGroup = 0);

    void shutdown();

    /** Replaces the HTTP delivery (tests: makes an attempt succeed, fail or throw). Set before enqueue. */
    void setDeliverOverride (std::function<Result<void> (const std::string& jobId, const std::string& url)> fn)
    {
        std::lock_guard<std::mutex> lock (mutex_);
        deliverOverride_ = std::move (fn);
    }

    static std::string signature (const std::string& secret, const std::string& timestamp, const std::string& body);

private:
    struct Task
    {
        std::string jobId, event, group, body, url, secretName, ip;
        int attempts = 0;
        std::chrono::steady_clock::time_point due;
    };

    void loop();
    Result<void> deliver (const Task&);
    bool stoppingNow() { std::lock_guard<std::mutex> lock (mutex_); return stopping_; }

    StateFn onState_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Task> tasks_;
    std::map<std::string, SecretRef> secrets_;
    double scale_ = 1.0;
    bool stopping_ = false;
    std::atomic<bool> loopDone_ { false };
    std::function<Result<void> (const std::string&, const std::string&)> deliverOverride_;
    httplib::Client* activeClient_ = nullptr;           // the attempt in flight, under mutex_
    std::thread thread_;
};

} // namespace rovox
