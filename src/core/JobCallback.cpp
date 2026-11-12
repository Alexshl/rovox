// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "JobCallback.h"

#include "Hmac.h"
#include "JobManager.h"
#include "Url.h"

#include <httplib.h>

#include <toml++/toml.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>

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

bool privateV4 (const std::uint8_t* b)
{
    return b[0] == 0 || b[0] == 10 || b[0] == 127 || (b[0] == 169 && b[1] == 254) || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168)
           || (b[0] == 100 && (b[1] & 0xc0) == 64) || (b[0] == 198 && (b[1] & 0xfe) == 18) || b[0] >= 224;
}

/** Every address the host resolves to (numeric strings). */
std::vector<std::string> resolve (const std::string& host, int port)
{
    std::vector<std::string> out;
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;

    if (getaddrinfo (host.c_str(), std::to_string (port).c_str(), &hints, &res) != 0)
        return out;

    for (auto* a = res; a != nullptr; a = a->ai_next)
    {
        char buf[INET6_ADDRSTRLEN] = {};

        if (a->ai_family == AF_INET)
            inet_ntop (AF_INET, &reinterpret_cast<sockaddr_in*> (a->ai_addr)->sin_addr, buf, sizeof (buf));
        else if (a->ai_family == AF_INET6)
            inet_ntop (AF_INET6, &reinterpret_cast<sockaddr_in6*> (a->ai_addr)->sin6_addr, buf, sizeof (buf));
        else
            continue;

        out.push_back (buf);
    }

    freeaddrinfo (res);
    return out;
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

bool isPrivateAddress (const std::string& ip)
{
    in_addr a4 {};

    if (inet_pton (AF_INET, ip.c_str(), &a4) == 1)
        return privateV4 (reinterpret_cast<const std::uint8_t*> (&a4));

    in6_addr a6 {};

    if (inet_pton (AF_INET6, ip.c_str(), &a6) != 1)
        return true;        // not an address we can judge: treat as unsafe

    const auto* b = a6.s6_addr;
    static const std::uint8_t zeros[12] = {};

    if (std::memcmp (b, zeros, 10) == 0 && b[10] == 0xff && b[11] == 0xff)          // ::ffff:a.b.c.d
        return privateV4 (b + 12);

    if (std::memcmp (b, zeros, 12) == 0)                                            // ::a.b.c.d (IPv4-compatible, deprecated), :: and ::1
        return true;

    static const std::uint8_t nat64[12] = { 0x00, 0x64, 0xff, 0x9b };

    if (std::memcmp (b, nat64, 12) == 0)                                            // 64:ff9b::/96: the embedded IPv4 address decides
        return privateV4 (b + 12);

    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && b[4] == 0x00 && b[5] == 0x01)        // 64:ff9b:1::/48 local-use NAT64
        return true;

    if (b[0] == 0x20 && b[1] == 0x02)                                               // 6to4 2002::/16: the embedded IPv4 address decides
        return privateV4 (b + 2);

    return (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || (b[0] & 0xfe) == 0xfc || b[0] == 0xff;
}

Result<CallbackTarget> validateCallback (const json& callback, const CallbackSettings& settings, const HostResolver& resolver)
{
    if (! callback.is_object() || ! callback.contains ("url") || ! callback["url"].is_string() || ! callback.contains ("secret_ref") || ! callback["secret_ref"].is_string())
        return bad ("callback is {url, secret_ref}", "/callback");

    CallbackTarget target;
    target.url = callback["url"].get<std::string>();
    target.secretName = callback["secret_ref"].get<std::string>();
    const auto url = parseUrl (target.url);

    if (! url.valid || url.host.find ('@') != std::string::npos || url.port <= 0 || url.port > 65535)
        return bad ("callback.url must be an http or https URL", "/callback/url");

    std::string host = url.host;

    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr (1, host.size() - 2);

    if (std::find (settings.allowHosts.begin(), settings.allowHosts.end(), lower (host)) == settings.allowHosts.end())
        return bad ("the host of callback.url is not listed in [callbacks] allow_hosts of the server configuration", "/callback/url");

    const auto addresses = resolver ? resolver (host, url.port) : resolve (host, url.port);

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

void JobCallbackDispatcher::enqueue (const JobInfo& finished, CallbackTarget target)
{
    auto j = finished.toJson();
    j.erase ("callback");
    j["event"] = "job.finished";

    Task t;
    t.jobId = finished.id;
    t.body = j.dump();
    t.url = std::move (target.url);
    t.secretName = std::move (target.secretName);
    t.ip = std::move (target.resolvedIp);
    t.due = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (! stopping_)
        {
            tasks_.push_back (std::move (t));
            cv_.notify_all();
            return;
        }
    }

    onState_ (finished.id, { { "state", "callback_failed" }, { "attempts", 0 }, { "last_error", "shutdown" } });
}

Result<void> JobCallbackDispatcher::deliver (const Task& t)
{
    SecretRef ref;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto it = secrets_.find (t.secretName);

        if (it != secrets_.end())
            ref = it->second;
    }

    const auto secret = ref.resolve();

    if (secret.empty())
        return makeError (errc::preconditionFailed, "the callback secret has no value");

    const auto url = parseUrl (t.url);

    if (! url.valid)
        return makeError (errc::preconditionFailed, "the callback URL is invalid");

    const auto timestamp = std::to_string (std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count());
    httplib::Headers headers;
    headers.emplace ("X-Rovox-Timestamp", timestamp);
    headers.emplace ("X-Rovox-Event", "job.finished");
    headers.emplace ("X-Rovox-Signature", signature (secret, timestamp, t.body));

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
