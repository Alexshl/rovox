// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Result.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rovox
{

struct Event
{
    std::uint64_t id = 0;       // hub-wide, strictly increasing
    std::string tenant;
    std::string projectId;
    json body;                  // the wire form; carries "event_id" and "tenant_id" too
};

struct SubscribeOptions
{
    std::string tenant = "local";   // a subscriber only ever sees events of its tenant
    std::string projectId;          // empty = all projects of the tenant
    size_t queueLimit = 1024;
};

/** One subscriber's bounded queue. When it fills up, the queue is replaced by a single
    `{"type":"resync_required"}` event and further events are dropped until the consumer has read it:
    a slow consumer never blocks publishers (the message thread) or other consumers. */
class Subscription
{
public:
    explicit Subscription (SubscribeOptions o) : options_ (std::move (o)) {}

    /** Blocks up to `timeout` for the next event. */
    bool pop (Event& out, std::chrono::milliseconds timeout);
    bool tryPop (Event& out)        { return pop (out, std::chrono::milliseconds (0)); }
    size_t pending() const;
    const SubscribeOptions& options() const { return options_; }

private:
    friend class EventHub;
    void push (const Event&);

    SubscribeOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
    bool resyncQueued_ = false;
};

class EventHub
{
public:
    std::shared_ptr<Subscription> subscribe (SubscribeOptions = {});

    /** Never blocks on subscribers. Returns the event id. */
    std::uint64_t publish (const std::string& tenant, const std::string& projectId, json body);

    /** Synchronous tap: called on the publishing thread, in publish order, before publish() returns. It must be cheap and
        must not block (the MCP layer only appends to bounded in-memory rings). Returns an id for removeObserver(). */
    using Observer = std::function<void (const Event&)>;
    int addObserver (Observer);
    void removeObserver (int id);

private:
    std::mutex mutex_;
    std::vector<std::weak_ptr<Subscription>> subscribers_;
    std::vector<std::pair<int, Observer>> observers_;
    int nextObserver_ = 1;
    std::uint64_t nextId_ = 1;
};

} // namespace rovox
