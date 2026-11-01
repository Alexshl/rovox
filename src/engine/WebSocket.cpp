// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "WebSocket.h"

#include "Sha1.h"

// Everything httplib.h includes, before the access hack below, so that the standard library is never compiled with it.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>

// httplib::Server::process_and_close_socket (the per-connection entry point) is a private virtual. Overriding it is legal,
// calling the base implementation is not, and the override has to do exactly that for every connection that is not a
// WebSocket upgrade. The layout of the class does not depend on the access specifier, so this one translation unit sees
// the member as protected. The library is pinned by the Docker image (Debian bookworm, cpp-httplib 0.11.4).
#define private protected
#include <httplib.h>
#undef private

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
 #define MSG_NOSIGNAL 0
#endif

namespace rovox
{

namespace
{
bool sendAll (int fd, const void* data, size_t n)
{
    auto* p = (const std::uint8_t*) data;

    while (n > 0)
    {
        const auto w = ::send (fd, p, n, MSG_NOSIGNAL);

        if (w <= 0)
            return false;

        p += w;
        n -= (size_t) w;
    }

    return true;
}

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t");
    const auto b = s.find_last_not_of (" \t\r");
    return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
}
} // namespace

std::shared_ptr<const std::vector<std::uint8_t>> encodeWsFrame (std::uint8_t opcode, const void* payload, size_t n, size_t* headerLen)
{
    auto f = std::make_shared<std::vector<std::uint8_t>>();
    f->reserve (n + 10);
    f->push_back ((std::uint8_t) (0x80 | opcode));

    if (n < 126)
    {
        f->push_back ((std::uint8_t) n);
    }
    else if (n < 65536)
    {
        f->push_back (126);
        f->push_back ((std::uint8_t) (n >> 8));
        f->push_back ((std::uint8_t) n);
    }
    else
    {
        f->push_back (127);

        for (int i = 7; i >= 0; --i)
            f->push_back ((std::uint8_t) ((std::uint64_t) n >> (i * 8)));
    }

    if (headerLen != nullptr)
        *headerLen = f->size();

    f->insert (f->end(), (const std::uint8_t*) payload, (const std::uint8_t*) payload + n);
    return f;
}

//==============================================================================
WsConnection::WsConnection (int fd) : fd_ (fd)
{
    int one = 1;
    ::setsockopt (fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int sndbuf = 64 * 1024;         // a small kernel buffer makes a stalled client show up as back-pressure in our own queue
    ::setsockopt (fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
}

WsConnection::~WsConnection()
{
    close();

    if (writer_.joinable())
        writer_.join();
}

void WsConnection::close()
{
    if (alive_.exchange (false))
    {
        ::shutdown (fd_, SHUT_RDWR);
        std::lock_guard<std::mutex> lock (mutex_);
        cv_.notify_all();
    }
}

void WsConnection::sendText (const std::string& text)
{
    auto frame = encodeWsFrame (0x1, text.data(), text.size());
    bool hopeless = false;
    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (! alive_)
            return;

        if (queued_ > 8 * 1024 * 1024)      // a client that does not read for this long is gone; it reconnects and resyncs
        {
            hopeless = true;
        }
        else
        {
            queued_ += frame->size();
            queue_.push_back ({ std::move (frame), false });
            cv_.notify_one();
        }
    }

    if (hopeless)
        close();
}

bool WsConnection::sendBinary (std::shared_ptr<const std::vector<std::uint8_t>> frame, bool droppable)
{
    bool dropped = false;
    std::lock_guard<std::mutex> lock (mutex_);

    if (! alive_)
        return false;

    if (droppable)
    {
        while (queued_ > maxQueuedBytes)
        {
            auto it = std::find_if (queue_.begin(), queue_.end(), [] (const Item& i) { return i.droppable; });

            if (it == queue_.end())
                break;

            queued_ -= it->frame->size();
            queue_.erase (it);
            ++droppedFrames;
            dropped = true;
        }
    }

    queued_ += frame->size();
    queue_.push_back ({ std::move (frame), droppable });
    cv_.notify_one();
    return dropped;
}

void WsConnection::writerLoop()
{
    while (alive_)
    {
        Item item;
        {
            std::unique_lock<std::mutex> lock (mutex_);
            cv_.wait (lock, [this] { return ! queue_.empty() || ! alive_; });

            if (! alive_)
                break;

            item = std::move (queue_.front());
            queued_ -= item.frame->size();
            queue_.pop_front();
        }

        if (! sendAll (fd_, item.frame->data(), item.frame->size()))
            break;
    }

    close();
}

bool WsConnection::readExact (void* data, size_t n)
{
    auto* p = (std::uint8_t*) data;

    while (n > 0)
    {
        const auto r = ::recv (fd_, p, n, 0);

        if (r <= 0)
            return false;

        p += r;
        n -= (size_t) r;
    }

    return true;
}

void WsConnection::run (const std::function<void (const std::string&)>& onText)
{
    writer_ = std::thread ([this] { writerLoop(); });
    std::string message;

    while (alive_)
    {
        std::uint8_t h[2];

        if (! readExact (h, 2))
            break;

        const bool fin = (h[0] & 0x80) != 0;
        const std::uint8_t opcode = h[0] & 0xF;
        std::uint64_t len = h[1] & 0x7F;

        if (len == 126)
        {
            std::uint8_t e[2];

            if (! readExact (e, 2))
                break;

            len = (std::uint64_t) e[0] << 8 | e[1];
        }
        else if (len == 127)
        {
            std::uint8_t e[8];

            if (! readExact (e, 8))
                break;

            len = 0;

            for (auto b : e)
                len = len << 8 | b;
        }

        std::uint8_t mask[4] = {};

        if ((h[1] & 0x80) == 0 || ! readExact (mask, 4))
            break;                      // clients must mask (RFC 6455 5.1)

        if (len > (1u << 20))
            break;

        std::vector<std::uint8_t> payload ((size_t) len);

        if (len > 0 && ! readExact (payload.data(), (size_t) len))
            break;

        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask[i % 4];

        if (opcode == 0x8)
            break;

        if (opcode == 0x9)
        {
            auto pong = encodeWsFrame (0xA, payload.data(), payload.size());
            std::lock_guard<std::mutex> lock (mutex_);
            queued_ += pong->size();
            queue_.push_back ({ pong, false });
            cv_.notify_one();
        }
        else if (opcode == 0x1 || opcode == 0x0)
        {
            message.append ((const char*) payload.data(), payload.size());

            if (fin)
            {
                onText (message);
                message.clear();
            }
        }
    }

    close();

    if (writer_.joinable())
        writer_.join();
}

//==============================================================================
void wsSendHttp (int fd, int status, const std::string& reason, const std::string& body)
{
    const std::string head = "HTTP/1.1 " + std::to_string (status) + " " + reason + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
                             + std::to_string (body.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll (fd, head.data(), head.size());
    sendAll (fd, body.data(), body.size());
}

bool wsHandshake (int fd, const WsRequest& req)
{
    auto it = req.headers.find ("sec-websocket-key");
    auto up = req.headers.find ("upgrade");

    if (it == req.headers.end() || up == req.headers.end() || lower (up->second) != "websocket")
        return false;

    Sha1 sha;
    const std::string key = it->second + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha.update (key.data(), key.size());
    const auto d = sha.digest();
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                             + base64Encode (d.data(), d.size()) + "\r\n\r\n";
    return sendAll (fd, resp.data(), resp.size());
}

//==============================================================================
namespace
{
class UpgradeServer : public httplib::Server
{
public:
    explicit UpgradeServer (std::function<void (int, WsRequest)> fn) : onUpgrade (std::move (fn)) {}

    bool process_and_close_socket (socket_t sock) override
    {
        if (! looksLikeUpgrade (sock))
            return httplib::Server::process_and_close_socket (sock);

        WsRequest req;
        std::string head;
        char ch;

        while (head.size() < 16384 && head.find ("\r\n\r\n") == std::string::npos)
        {
            if (::recv (sock, &ch, 1, 0) <= 0)
            {
                ::close (sock);
                return false;
            }

            head.push_back (ch);
        }

        std::istringstream in (head);
        std::string method, target, version;
        in >> method >> target >> version;
        const auto q = target.find ('?');
        req.path = target.substr (0, q);
        req.query = q == std::string::npos ? std::string() : target.substr (q + 1);
        std::string line;
        std::getline (in, line);

        while (std::getline (in, line))
        {
            const auto colon = line.find (':');

            if (colon != std::string::npos)
                req.headers[lower (line.substr (0, colon))] = trim (line.substr (colon + 1));
        }

        sockaddr_storage addr {};
        socklen_t len = sizeof addr;
        char host[64] = "?";

        if (::getpeername (sock, (sockaddr*) &addr, &len) == 0)
            ::getnameinfo ((sockaddr*) &addr, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST);

        req.remote = host;
        onUpgrade ((int) sock, std::move (req));
        return true;
    }

private:
    /** `GET /ws` followed by a space or '?', judged from the first bytes without consuming them. */
    static bool looksLikeUpgrade (socket_t sock)
    {
        char buf[16];

        for (int attempt = 0; attempt < 50; ++attempt)
        {
            pollfd pfd { sock, POLLIN, 0 };

            if (::poll (&pfd, 1, 100) <= 0)
                continue;

            const auto n = ::recv (sock, buf, sizeof buf, MSG_PEEK);

            if (n <= 0)
                return false;

            if (n < 8)
            {
                if (std::memcmp (buf, "GET /ws", (size_t) std::min<ssize_t> (n, 7)) != 0)
                    return false;

                continue;       // wait for the rest of the request line
            }

            return std::memcmp (buf, "GET /ws", 7) == 0 && (buf[7] == ' ' || buf[7] == '?');
        }

        return false;
    }

    std::function<void (int, WsRequest)> onUpgrade;
};
} // namespace

std::unique_ptr<httplib::Server> makeServerWithUpgrade (std::function<void (int fd, WsRequest)> onUpgrade)
{
    return std::make_unique<UpgradeServer> (std::move (onUpgrade));
}

} // namespace rovox
