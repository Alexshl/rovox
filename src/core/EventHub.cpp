// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "EventHub.h"

namespace rovox
{

bool Subscription::pop (Event& out, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock (mutex_);

    if (! cv_.wait_for (lock, timeout, [this] { return ! queue_.empty(); }))
        return false;

    out = std::move (queue_.front());
    queue_.pop_front();

    if (queue_.empty())
        resyncQueued_ = false;      // the consumer has seen the resync marker: start delivering again

    return true;
}

size_t Subscription::pending() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return queue_.size();
}

void Subscription::push (const Event& e)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (resyncQueued_)
            return;

        if (queue_.size() >= options_.queueLimit)
        {
            queue_.clear();
            Event marker;
            marker.id = e.id;
            marker.tenant = e.tenant;
            marker.projectId = options_.projectId;
            marker.body = json { { "type", "resync_required" }, { "event_id", e.id }, { "tenant_id", e.tenant } };
            queue_.push_back (std::move (marker));
            resyncQueued_ = true;
        }
        else
        {
            queue_.push_back (e);
        }
    }

    cv_.notify_one();
}

std::shared_ptr<Subscription> EventHub::subscribe (SubscribeOptions o)
{
    auto sub = std::make_shared<Subscription> (std::move (o));
    std::lock_guard<std::mutex> lock (mutex_);
    subscribers_.push_back (sub);
    return sub;
}

std::uint64_t EventHub::publish (const std::string& tenant, const std::string& projectId, json body)
{
    std::vector<std::shared_ptr<Subscription>> targets;
    Event e;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        e.id = nextId_++;

        for (auto it = subscribers_.begin(); it != subscribers_.end();)
        {
            if (auto s = it->lock())
            {
                targets.push_back (std::move (s));
                ++it;
            }
            else
            {
                it = subscribers_.erase (it);
            }
        }
    }

    e.tenant = tenant;
    e.projectId = projectId;
    body["event_id"] = e.id;
    body["tenant_id"] = tenant;
    e.body = std::move (body);

    for (auto& s : targets)
        if (s->options().tenant == tenant && (s->options().projectId.empty() || s->options().projectId == projectId))
            s->push (e);

    return e.id;
}

} // namespace rovox
