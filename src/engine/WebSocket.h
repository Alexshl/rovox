// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Minimal RFC 6455 server side (task 300): handshake, text/binary frames, ping/pong, close, with one writer thread and a
// bounded queue per connection. The same port as HTTP: a subclass of httplib::Server looks at the first bytes of every
// accepted connection and hands `GET /ws` upgrades to a callback instead of httplib.
#pragma once

#include "Result.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Server; }

namespace rovox
{

struct WsRequest
{
    std::string path;                               // "/ws"
    std::string query;                              // raw, without '?'
    std::map<std::string, std::string> headers;     // lower-case names
    std::string remote;
};

/** Encoded frame bytes (unmasked, server to client). */
std::shared_ptr<const std::vector<std::uint8_t>> encodeWsFrame (std::uint8_t opcode, const void* payload, size_t n, size_t* headerLen = nullptr);

class WsConnection
{
public:
    explicit WsConnection (int fd);
    ~WsConnection();

    bool alive() const noexcept         { return alive_; }
    void close();

    /** Control and JSON traffic. Never dropped; when the queue is hopelessly long the connection is closed (the client reconnects and resyncs). */
    void sendText (const std::string&);
    /** Binary frames. `droppable` frames (audio, meters) are dropped oldest-first once the queue exceeds `maxQueuedBytes`;
        returns true if frames had to be dropped (the caller flags a discontinuity). */
    bool sendBinary (std::shared_ptr<const std::vector<std::uint8_t>> frame, bool droppable);

    /** Reads frames on the calling thread until the peer closes; text frames go to `onText`. Starts and joins the writer thread. */
    void run (const std::function<void (const std::string&)>& onText);

    size_t maxQueuedBytes = 32 * 1024;
    std::atomic<std::int64_t> droppedFrames { 0 };

private:
    struct Item
    {
        std::shared_ptr<const std::vector<std::uint8_t>> frame;
        bool droppable;
    };

    void writerLoop();
    bool readExact (void* data, size_t n);

    int fd_;
    std::atomic<bool> alive_ { true };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    size_t queued_ = 0;
    std::thread writer_;
};

/** Writes a complete HTTP response to a raw socket (used for upgrade rejections). */
void wsSendHttp (int fd, int status, const std::string& reason, const std::string& body = {});
/** Sends the 101 response for `req`; false if the request is not a valid upgrade. */
bool wsHandshake (int fd, const WsRequest& req);

/** A cpp-httplib server that passes `GET /ws` upgrade requests to `onUpgrade` (called on a pool thread, owns and must close the fd). */
std::unique_ptr<httplib::Server> makeServerWithUpgrade (std::function<void (int fd, WsRequest)> onUpgrade);

} // namespace rovox
