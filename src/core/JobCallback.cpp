// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "JobCallback.h"

#include "Hmac.h"
#include "Ids.h"
#include "JobManager.h"
#include "Url.h"

#include <httplib.h>

#include <toml++/toml.h>

#include <algorithm>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
constexpr int retryDelaysS[] = { 1, 5, 30, 120, 600 };      // pause after the 1st, 2nd ... failed attempt
constexpr int maxAttempts = 5;                              // task 220: up to 5 attempts, the 5th failure is final

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

Error bad (const std::string& message, const std::string& path)
{
    return makeError (errc::invalidParams, message, json { { "path", path } });
}
} // namespace

//==============================================================================
CallbackSettings CallbackSettings::load (const fs::path& tomlFile)
{
    CallbackSettings s;
    std::error_code ec;

    if (tomlFile.empty() || ! fs::exists (tomlFile, ec))
        return s;

    try
    {
        const auto doc = toml::parse_file (tomlFile.string());
        auto* t = doc["callbacks"].as_table();

        if (t == nullptr)
            return s;

        if (auto* hosts = (*t)["allow_hosts"].as_array())
            for (auto&& h : *hosts)
                if (auto v = h.value<std::string>())
                    s.allowHosts.push_back (lower (*v));

        s.allowPrivate = (*t)["allow_private"].value_or (false);

        if (auto* secrets = (*t)["secrets"].as_table())
            for (auto&& [name, node] : *secrets)
                if (auto* st = node.as_table())
                    s.secrets[std::string (name.str())] = SecretRef { (*st)["env"].value_or (std::string()), (*st)["file"].value_or (std::string()) };
    }
    catch (const toml::parse_error&)
    {
        return CallbackSettings {};
    }

    return s;
}

Result<CallbackTarget> validateCallback (const json& callback, const CallbackSettings& settings, const HostResolver& resolver)
{
    if (! callback.is_object() || ! callback.contains ("url") || ! callback["url"].is_string() || ! callback.contains ("secret_ref") || ! callback["secret_ref"].is_string())
        return bad ("callback is {url, secret_ref}", "/callback");

    if (callback.contains ("secret_ref_next") && ! callback["secret_ref_next"].is_string())
        return bad ("callback.secret_ref_next is a secret name", "/callback/secret_ref_next");

    CallbackTarget target;
    target.url = callback["url"].get<std::string>();
    target.secretName = callback["secret_ref"].get<std::string>();

    if (callback.contains ("secret_ref_next"))
        target.nextSecretName = callback["secret_ref_next"].get<std::string>();

    const auto url = parseUrl (target.url);

    if (! url.valid || url.host.find ('@') != std::string::npos || url.port <= 0 || url.port > 65535)
        return bad ("callback.url must be an http or https URL", "/callback/url");

    std::string host = url.host;

    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr (1, host.size() - 2);

    if (std::find (settings.allowHosts.begin(), settings.allowHosts.end(), lower (host)) == settings.allowHosts.end())
        return bad ("the host of callback.url is not listed in [callbacks] allow_hosts of the server configuration", "/callback/url");

    const auto addresses = resolver ? resolver (host, url.port) : resolveHost (host, url.port);

    if (addresses.empty())
        return bad ("the host of callback.url cannot be resolved", "/callback/url");

    if (! settings.allowPrivate)
        for (auto& a : addresses)
            if (isPrivateAddress (a))
                return bad ("callback.url resolves to a loopback, private or link-local address; the server configuration does not allow that ([callbacks] allow_private)", "/callback/url");

    target.resolvedIp = addresses.front();

    auto it = settings.secrets.find (target.secretName);

    if (it == settings.secrets.end())
        return bad ("callback.secret_ref names no secret in [callbacks.secrets] of the server configuration", "/callback/secret_ref");

    if (it->second.resolve().empty())
        return bad ("the secret named by callback.secret_ref has no value (its environment variable or file is empty)", "/callback/secret_ref");

    if (! target.nextSecretName.empty())
    {
        auto next = settings.secrets.find (target.nextSecretName);

        if (next == settings.secrets.end() || next->second.resolve().empty())
            return bad ("callback.secret_ref_next names no secret with a value in [callbacks.secrets] of the server configuration", "/callback/secret_ref_next");
    }

    return target;
}

//==============================================================================
JobCallbackDispatcher::JobCallbackDispatcher (StateFn onState, std::map<std::string, SecretRef> secrets) : onState_ (std::move (onState)), secrets_ (std::move (secrets))
{
    thread_ = std::thread ([this] { loop(); });
}

JobCallbackDispatcher::~JobCallbackDispatcher()
{
    shutdown();
}

void JobCallbackDispatcher::shutdown()
{
    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (stopping_)
            return;

        stopping_ = true;
    }

    cv_.notify_all();

    // an attempt that is blocked on a slow receiver is cut off: stop() the client until the loop has left
    while (! loopDone_.load())
    {
        {
            std::lock_guard<std::mutex> lock (mutex_);

            if (activeClient_ != nullptr)
                activeClient_->stop();
        }

        std::unique_lock<std::mutex> lock (mutex_);
        cv_.wait_for (lock, std::chrono::milliseconds (20), [this] { return loopDone_.load(); });
    }

    if (thread_.joinable())
        thread_.join();
}

void JobCallbackDispatcher::setDelayScale (double scale)
{
    std::lock_guard<std::mutex> lock (mutex_);
    scale_ = scale;
}

void JobCallbackDispatcher::setSecrets (std::map<std::string, SecretRef> secrets)
{
    std::lock_guard<std::mutex> lock (mutex_);
    secrets_ = std::move (secrets);
}

std::string JobCallbackDispatcher::signature (const std::string& secret, const std::string& timestamp, const std::string& body)
{
    return "sha256=" + hmacSha256Hex (secret, timestamp + "." + body);
}

std::string JobCallbackDispatcher::signatureHeader (const std::vector<std::string>& secrets, const std::string& timestamp, const std::string& body)
{
    std::string out;

    for (auto& secret : secrets)
    {
        if (secret.empty())
            continue;

        out += (out.empty() ? "" : ",") + signature (secret, timestamp, body);
    }

    return out;
}

void JobCallbackDispatcher::enqueue (const JobInfo& finished, CallbackTarget target)
{
    auto j = finished.toJson();
    j.erase ("callback");
    j["event"] = "job.finished";
    enqueueBody (finished.id, "job.finished", j.dump(), std::move (target));
}

void JobCallbackDispatcher::enqueueBody (const std::string& id, const std::string& event, std::string body, CallbackTarget target, const std::string& group, size_t maxPerGroup)
{
    Task t;
    t.jobId = id;
    t.event = event;
    t.eventId = newUlid();
    t.group = group;
    t.body = std::move (body);
    t.url = std::move (target.url);
    t.secretName = std::move (target.secretName);
    t.nextSecretName = std::move (target.nextSecretName);
    t.ip = std::move (target.resolvedIp);
    t.due = std::chrono::steady_clock::now();
    std::vector<Task> dropped;
    bool rejected = false;

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (! stopping_)
        {
            if (! group.empty() && maxPerGroup > 0)
                for (;;)
                {
                    const auto inGroup = (size_t) std::count_if (tasks_.begin(), tasks_.end(), [&] (const Task& x) { return x.group == group; });

                    if (inGroup < maxPerGroup)
                        break;

                    auto oldest = std::find_if (tasks_.begin(), tasks_.end(), [&] (const Task& x) { return x.group == group; });
                    dropped.push_back (std::move (*oldest));
                    tasks_.erase (oldest);
                }

            tasks_.push_back (std::move (t));
            cv_.notify_all();
        }
        else
        {
            rejected = true;
        }
    }

    for (auto& d : dropped)
        onState_ (d.jobId, { { "state", "callback_failed" }, { "attempts", d.attempts }, { "last_error", "overflow" } });

    if (rejected)
        onState_ (id, { { "state", "callback_failed" }, { "attempts", 0 }, { "last_error", "shutdown" } });
}

Result<void> JobCallbackDispatcher::deliver (const Task& t)
{
    SecretRef ref, nextRef;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto it = secrets_.find (t.secretName);

        if (it != secrets_.end())
            ref = it->second;

        if (! t.nextSecretName.empty())
            if (auto next = secrets_.find (t.nextSecretName); next != secrets_.end())
                nextRef = next->second;
    }

    const auto secret = ref.resolve();
    const auto nextSecret = nextRef.resolve();

    if (secret.empty())
        return makeError (errc::preconditionFailed, "the callback secret has no value");

    const auto url = parseUrl (t.url);

    if (! url.valid)
        return makeError (errc::preconditionFailed, "the callback URL is invalid");

    const auto timestamp = std::to_string (std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count());
    httplib::Headers headers;
    headers.emplace ("X-Rovox-Timestamp", timestamp);
    headers.emplace ("X-Rovox-Event", t.event.empty() ? std::string ("job.finished") : t.event);
    headers.emplace ("X-Rovox-Event-Id", t.eventId);
    headers.emplace ("X-Rovox-Signature", signatureHeader ({ secret, nextSecret }, timestamp, t.body));

    httplib::Client client (url.origin);
    client.set_hostname_addr_map ({ { url.host, t.ip } });      // connect to the address that was checked, not to a second DNS answer
    client.set_connection_timeout (10, 0);
    client.set_read_timeout (30, 0);
    client.set_write_timeout (30, 0);

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (stopping_)
            return makeError (errc::busy, "shutdown");

        activeClient_ = &client;
    }

    auto res = client.Post (url.path.empty() ? "/" : url.path, headers, t.body, "application/json");

    {
        std::lock_guard<std::mutex> lock (mutex_);
        activeClient_ = nullptr;
    }

    if (! res)
        return makeError (errc::busy, "the callback could not be delivered (no HTTP answer)");

    if (res->status / 100 != 2)
        return makeError (errc::busy, "the callback was answered with HTTP " + std::to_string (res->status));

    return {};
}

void JobCallbackDispatcher::loop()
{
    auto finish = [this]
    {
        std::lock_guard<std::mutex> lock (mutex_);
        loopDone_ = true;
        cv_.notify_all();
    };

    for (;;)
    {
        Task task;

        {
            std::unique_lock<std::mutex> lock (mutex_);

            for (;;)
            {
                if (stopping_)
                {
                    auto left = std::move (tasks_);
                    tasks_.clear();
                    lock.unlock();

                    for (auto& t : left)
                        onState_ (t.jobId, { { "state", "callback_failed" }, { "attempts", t.attempts }, { "last_error", "shutdown" } });

                    finish();
                    return;
                }

                auto next = std::min_element (tasks_.begin(), tasks_.end(), [] (const Task& a, const Task& b) { return a.due < b.due; });

                if (next != tasks_.end() && next->due <= std::chrono::steady_clock::now())
                {
                    task = std::move (*next);
                    tasks_.erase (next);
                    break;
                }

                if (next == tasks_.end())       cv_.wait (lock);
                else                            cv_.wait_until (lock, next->due);
            }
        }

        ++task.attempts;
        Result<void> result;

        try
        {
            result = deliverOverride_ ? deliverOverride_ (task.jobId, task.url) : deliver (task);
        }
        catch (const std::exception& e)      // never let an exception end the process: it is a failed attempt
        {
            result = makeError (errc::internal, std::string ("the callback delivery threw: ") + e.what());
        }
        catch (...)
        {
            result = makeError (errc::internal, "the callback delivery threw");
        }

        {
            std::lock_guard<std::mutex> lock (mutex_);
            activeClient_ = nullptr;        // a throw may have skipped the reset; the client on the stack is gone
        }

        if (result.ok())
        {
            onState_ (task.jobId, { { "state", "delivered" }, { "attempts", task.attempts } });
        }
        else if (task.attempts >= maxAttempts || stoppingNow())
        {
            const bool shutDown = task.attempts < maxAttempts;
            const auto message = shutDown ? std::string ("shutdown") : result.error().message;
            onState_ (task.jobId, { { "state", "callback_failed" }, { "attempts", task.attempts }, { "last_error", message } });
        }
        else
        {
            double scale;

            {
                std::lock_guard<std::mutex> lock (mutex_);
                scale = scale_;
            }

            onState_ (task.jobId, { { "state", "pending" }, { "attempts", task.attempts }, { "last_error", result.error().message } });
            task.due = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (retryDelaysS[task.attempts - 1] * scale));
            std::lock_guard<std::mutex> lock (mutex_);
            tasks_.push_back (std::move (task));
        }
    }
}

} // namespace rovox
