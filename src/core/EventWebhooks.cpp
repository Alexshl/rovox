// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "EventWebhooks.h"

#include <toml++/toml.h>

#include <algorithm>
#include <ctime>
#include <iostream>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

bool secretLooking (const std::string& key)
{
    const auto k = lower (key);

    for (const char* word : { "secret", "token", "password", "passwd", "credential", "apikey", "api_key", "authorization", "private_key", "signature" })
        if (k.find (word) != std::string::npos)
            return true;

    return false;
}

bool under (const std::string& s, const fs::path& root)
{
    auto r = root.generic_string();

    while (r.size() > 1 && r.back() == '/')
        r.pop_back();

    return ! r.empty() && (s == r || (s.size() > r.size() && s.compare (0, r.size(), r) == 0 && s[r.size()] == '/'));
}

std::string isoNow()
{
    const auto t = std::time (nullptr);
    std::tm tm {};
    ::gmtime_r (&t, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
} // namespace

std::map<std::string, WebhookConfig> loadWebhookConfigs (const fs::path& tomlFile)
{
    std::map<std::string, WebhookConfig> out;
    std::error_code ec;

    if (tomlFile.empty() || ! fs::exists (tomlFile, ec))
        return out;

    try
    {
        const auto doc = toml::parse_file (tomlFile.string());
        auto* tenants = doc["tenants"].as_table();

        if (tenants == nullptr)
            return out;

        for (auto&& [id, node] : *tenants)
        {
            auto* t = node.as_table();
            auto* hook = t != nullptr ? (*t)["webhooks"].as_table() : nullptr;

            if (hook == nullptr)
                continue;

            WebhookConfig c;
            c.url = (*hook)["url"].value_or (std::string());
            c.secretRef = (*hook)["secret_ref"].value_or (std::string());

            if (auto* events = (*hook)["events"].as_array())
                for (auto&& e : *events)
                    if (auto v = e.value<std::string>())
                        c.events.push_back (*v);

            out[std::string (id.str())] = std::move (c);
        }
    }
    catch (const toml::parse_error&)
    {
        return {};
    }

    return out;
}

bool EventWebhooks::globMatch (const std::string& pattern, const std::string& text)
{
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;

    while (t < text.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]) && pattern[p] != '*')
        {
            ++p;
            ++t;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            mark = t;
        }
        else if (star != std::string::npos)
        {
            p = star + 1;
            t = ++mark;
        }
        else
        {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*')
        ++p;

    return p == pattern.size();
}

//==============================================================================
EventWebhooks::EventWebhooks (EventHub& hub, Options options)
    : hub_ (hub), options_ (std::move (options)),
      dispatcher_ ([this] (const std::string& id, const json& state)
      {
          const auto last = state.value ("last_error", std::string());
          const auto s = state.value ("state", std::string());
          const auto tenant = id.substr (0, id.find (':'));

          if (last == "overflow")
          {
              ++overflow_;
              log ("webhook.overflow tenant=" + tenant + " dropped=" + id);
          }
          else if (s == "callback_failed")
          {
              log ("webhook.failed tenant=" + tenant + " event=" + id + " attempts=" + std::to_string (state.value ("attempts", 0)));
          }
      })
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        reloadLocked();
    }

    thread_ = std::thread ([this] { loop(); });
    observerId_ = hub_.addObserver ([this] (const Event& e) { onEvent (e); });
}

EventWebhooks::~EventWebhooks()
{
    shutdown();
}

void EventWebhooks::shutdown()
{
    if (observerId_ != 0)
    {
        hub_.removeObserver (observerId_);      // not under mutex_: it waits for a running publish, whose observer takes mutex_
        observerId_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (stopping_)
            return;

        stopping_ = true;
    }

    cv_.notify_all();

    if (thread_.joinable())
        thread_.join();

    dispatcher_.shutdown();
}

void EventWebhooks::log (const std::string& line)
{
    std::cerr << line << std::endl;
}

void EventWebhooks::reloadLocked()
{
    std::error_code ec;
    const bool exists = ! options_.configFile.empty() && fs::exists (options_.configFile, ec);
    const auto time = exists ? fs::last_write_time (options_.configFile, ec) : fs::file_time_type {};
    const auto size = exists ? fs::file_size (options_.configFile, ec) : std::uintmax_t (0);

    if (configSeen_ == exists && (! exists || (time == configTime_ && size == configSize_)))
        return;

    configSeen_ = exists;
    configTime_ = time;
    configSize_ = size;
    configs_ = loadWebhookConfigs (options_.configFile);
    settings_ = CallbackSettings::load (options_.configFile);
    ++generation_;
}

json EventWebhooks::dataOf (const Event& e, const json& source) const
{
    json data = source;

    if (data.is_object())
        for (const char* k : { "event_id", "tenant_id", "type" })
            data.erase (k);

    const auto root = options_.tenantRoot ? options_.tenantRoot (e.tenant) : fs::path();

    std::function<void (json&)> clean = [&] (json& j)
    {
        if (j.is_object())
        {
            for (auto it = j.begin(); it != j.end();)
            {
                if (secretLooking (it.key()))
                    it = j.erase (it);
                else
                {
                    clean (it.value());
                    ++it;
                }
            }
        }
        else if (j.is_array())
        {
            for (auto& v : j)
                clean (v);
        }
        else if (j.is_string())
        {
            const auto s = j.get<std::string>();

            if (! s.empty() && s.front() == '/')
            {
                if (! root.empty() && under (s, root))
                    j = fs::path (s).lexically_relative (root).generic_string();
                else if (! options_.hostRoot.empty() && under (s, options_.hostRoot))
                    j = "[redacted]";
            }
        }
    };

    clean (data);
    return data;
}

void EventWebhooks::onEvent (const Event& e)
{
    const auto type = e.body.value ("type", std::string());

    if (type.empty())
        return;

    std::lock_guard<std::mutex> lock (mutex_);

    if (stopping_)
        return;

    reloadLocked();
    auto it = configs_.find (e.tenant);

    if (it == configs_.end() || it->second.events.empty())
        return;

    std::vector<std::pair<std::string, json>> outgoing;
    outgoing.emplace_back (type, e.body);

    if (type == "job.updated" && e.body.contains ("job") && e.body["job"].is_object())
    {
        const auto& job = e.body["job"];
        const auto state = job.value ("state", std::string());

        if (job.value ("kind", std::string()) == "render" && (state == "succeeded" || state == "failed" || state == "cancelled"))
        {
            json done { { "job_id", job.value ("job_id", "") }, { "kind", "render" }, { "state", state }, { "project_id", job.value ("project_id", "") },
                        { "finished", job.value ("finished", "") } };

            if (job.contains ("error"))
                done["error"] = job["error"];

            outgoing.emplace_back ("render.done", std::move (done));
        }
    }

    for (auto& [name, source] : outgoing)
    {
        const bool wanted = std::any_of (it->second.events.begin(), it->second.events.end(), [&] (const std::string& g) { return globMatch (g, name); });

        if (! wanted)
            continue;

        json body { { "event_id", e.id }, { "type", name }, { "tenant", e.tenant }, { "ts", isoNow() }, { "data", dataOf (e, source) } };

        if (! e.projectId.empty())
            body["project_id"] = e.projectId;

        Pending p;
        p.id = e.tenant + ":" + std::to_string (e.id) + ":" + name;
        p.type = name;
        p.body = body.dump();
        p.tenant = e.tenant;
        p.config = it->second;
        p.settings = settings_;
        p.generation = generation_;
        auto& queue = pending_[e.tenant];

        while (options_.queueLimit > 0 && queue.size() >= options_.queueLimit)
        {
            ++overflow_;
            log ("webhook.overflow tenant=" + e.tenant + " dropped=" + queue.front().id);
            queue.pop_front();
        }

        queue.push_back (std::move (p));
    }

    cv_.notify_all();
}

void EventWebhooks::loop()
{
    std::string lastTenant;
    std::uint64_t appliedSecrets = 0;

    for (;;)
    {
        Pending item;

        {
            std::unique_lock<std::mutex> lock (mutex_);

            for (;;)
            {
                if (stopping_)
                    return;

                auto next = pending_.upper_bound (lastTenant);

                for (size_t i = 0; i < pending_.size(); ++i, ++next)
                {
                    if (next == pending_.end())
                        next = pending_.begin();

                    if (! next->second.empty())
                    {
                        item = std::move (next->second.front());
                        next->second.pop_front();
                        lastTenant = next->first;
                        break;
                    }
                }

                if (! item.id.empty())
                    break;

                cv_.wait (lock);
            }
        }

        if (item.generation != appliedSecrets)
        {
            dispatcher_.setSecrets (item.settings.secrets);
            appliedSecrets = item.generation;
        }

        auto& cached = checked_[item.tenant];
        const auto now = std::chrono::steady_clock::now();

        if (cached.generation != item.generation || cached.at == std::chrono::steady_clock::time_point {} || now - cached.at > options_.revalidateAfter)
        {
            auto target = validateCallback ({ { "url", item.config.url }, { "secret_ref", item.config.secretRef } }, item.settings, options_.resolver);
            cached.generation = item.generation;
            cached.at = now;
            cached.valid = target.ok();

            if (target.ok())
                cached.target = target.value();
            else
                log ("webhook.refused tenant=" + item.tenant + " reason=" + target.error().message);
        }

        if (! cached.valid)
        {
            ++refused_;
            continue;
        }

        dispatcher_.enqueueBody (item.id, item.type, std::move (item.body), cached.target, item.tenant, options_.queueLimit);
    }
}

} // namespace rovox
