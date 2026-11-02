// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Gateway.h"

#include "Transport.h"
#include "Version.h"

#include <cstring>

#include <unistd.h>

namespace rovox
{

namespace
{
std::string hostOfOrigin (std::string o)
{
    if (const auto scheme = o.find ("://"); scheme != std::string::npos)
        o = o.substr (scheme + 3);

    if (const auto slash = o.find ('/'); slash != std::string::npos)
        o = o.substr (0, slash);

    if (! o.empty() && o[0] == '[')
        return o.substr (0, o.find (']') + 1);

    return o.substr (0, o.find (':'));
}

bool isLoopbackName (const std::string& h)
{
    return h == "localhost" || h == "127.0.0.1" || h == "[::1]" || h == "::1";
}

std::string cookieValue (const std::string& cookies, const std::string& name)
{
    size_t pos = 0;

    while (pos < cookies.size())
    {
        auto end = cookies.find (';', pos);
        auto item = cookies.substr (pos, end == std::string::npos ? std::string::npos : end - pos);
        const auto first = item.find_first_not_of (' ');
        item = first == std::string::npos ? std::string() : item.substr (first);

        if (item.rfind (name + "=", 0) == 0)
            return item.substr (name.size() + 1);

        if (end == std::string::npos)
            break;

        pos = end + 1;
    }

    return {};
}

std::string queryValue (const std::string& query, const std::string& name)
{
    size_t pos = 0;

    while (pos < query.size())
    {
        auto end = query.find ('&', pos);
        const auto item = query.substr (pos, end == std::string::npos ? std::string::npos : end - pos);

        if (item.rfind (name + "=", 0) == 0)
            return item.substr (name.size() + 1);

        if (end == std::string::npos)
            break;

        pos = end + 1;
    }

    return {};
}

bool constantTimeEquals (const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;

    unsigned char diff = 0;

    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char) (a[i] ^ b[i]);

    return diff == 0;
}

/** A string field, "" when it is missing, null or of another type (clients may send explicit nulls). */
std::string stringField (const json& msg, const char* key)
{
    auto it = msg.find (key);
    return it != msg.end() && it->is_string() ? it->get<std::string>() : std::string();
}

template <typename T>
void put (std::vector<std::uint8_t>& out, T v)
{
    const auto* p = (const std::uint8_t*) &v;
    out.insert (out.end(), p, p + sizeof (T));
}
} // namespace

bool Gateway::tokenMatches (const std::string& expected, const std::string& authorization, const std::string& cookie, const std::string& query)
{
    if (expected.empty())
        return true;

    if (authorization == "Bearer " + expected)
        return true;

    return constantTimeEquals (cookieValue (cookie, "rovox_token"), expected) || constantTimeEquals (queryValue (query, "token"), expected);
}

bool Gateway::originAllowed (const std::string& origin, const std::string& host, const std::vector<std::string>& allowed)
{
    if (origin.empty())
        return true;                    // not a browser

    const auto originHost = hostOfOrigin (origin);

    if (isLoopbackName (originHost) || (! host.empty() && originHost == hostOfOrigin (host)))
        return true;

    return std::find (allowed.begin(), allowed.end(), origin) != allowed.end();
}

//==============================================================================
Gateway::Gateway (EngineHost& host, ProjectManager& pm, CommandBus& bus, EventHub& hub, MessageThreadExecutor& executor, GatewayOptions o)
    : host_ (host), projects_ (pm), bus_ (bus), hub_ (hub), executor_ (executor), options_ (std::move (o))
{
    observerId_ = hub_.addObserver ([this] (const Event& e) { onHubEvent (e); });

    if (auto* stream = host_.stream())
        stream->setPacketSink ([this] (const std::vector<std::uint8_t>& p) { onAudioPacket (p); });

    startTimer (33);
}

Gateway::~Gateway()
{
    stopTimer();
    shutdown();
    hub_.removeObserver (observerId_);

    if (auto* stream = host_.stream())
        stream->setPacketSink ({});
}

void Gateway::shutdown()
{
    stopping_ = true;

    for (auto& c : clients())
        c->conn->close();

    for (int i = 0; i < 100 && clientCount() > 0; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
}

int Gateway::clientCount() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return (int) clients_.size();
}

std::vector<std::shared_ptr<Gateway::Client>> Gateway::clients()
{
    std::lock_guard<std::mutex> lock (mutex_);
    return clients_;
}

int Gateway::projectIndex (const std::string& projectId)
{
    std::lock_guard<std::mutex> lock (mutex_);
    auto it = projectIndex_.find (projectId);

    if (it == projectIndex_.end())
        it = projectIndex_.emplace (projectId, (int) projectIndex_.size() % 250 + 1).first;

    return it->second;
}

void Gateway::handleUpgrade (int fd, WsRequest req)
{
    auto header = [&req] (const char* name) { auto it = req.headers.find (name); return it == req.headers.end() ? std::string() : it->second; };

    if (stopping_)
        return wsSendHttp (fd, 503, "Service Unavailable"), void (::close (fd));

    if (! tokenMatches (options_.token, header ("authorization"), header ("cookie"), req.query))
        return wsSendHttp (fd, 401, "Unauthorized", "missing or wrong token"), void (::close (fd));

    if (! originAllowed (header ("origin"), header ("host"), options_.allowedOrigins))
        return wsSendHttp (fd, 403, "Forbidden", "Origin not allowed"), void (::close (fd));

    if (clientCount() >= (int) options_.maxClients)
        return wsSendHttp (fd, 503, "Service Unavailable", "too many WebSocket connections"), void (::close (fd));

    if (! wsHandshake (fd, req))
        return wsSendHttp (fd, 400, "Bad Request", "not a WebSocket upgrade"), void (::close (fd));

    auto client = std::make_shared<Client>();
    client->session = "ws_" + std::to_string (nextSession_++);
    client->conn = std::make_shared<WsConnection> (fd);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        clients_.push_back (client);
    }

    client->conn->run ([this, client] (const std::string& text)
    {
        try
        {
            onMessage (client, text);
        }
        catch (const std::exception& e)
        {
            client->conn->sendText (json { { "t", "error" }, { "message", e.what() } }.dump (-1, ' ', false, json::error_handler_t::replace));
        }
    });

    {
        std::lock_guard<std::mutex> lock (mutex_);
        clients_.erase (std::remove (clients_.begin(), clients_.end(), client), clients_.end());
    }

    ::close (fd);
}

//==============================================================================
void Gateway::onMessage (const std::shared_ptr<Client>& c, const std::string& text)
{
    const auto msg = json::parse (text, nullptr, false);

    if (! msg.is_object() || ! msg.contains ("t") || ! msg["t"].is_string())
    {
        c->conn->sendText (json { { "t", "error" }, { "message", "frames must be JSON objects with a \"t\" field" } }.dump (-1, ' ', false, json::error_handler_t::replace));
        return;
    }

    const auto t = msg["t"].get<std::string>();

    if (t == "hello")
    {
        json audioInfo { { "mode", toString (host_.audioMode()) }, { "available", host_.stream() != nullptr && host_.stream()->ready() } };

        if (host_.stream() != nullptr)
            audioInfo.update ({ { "sample_rate", BrowserStream::sampleRate }, { "frame_samples", BrowserStream::frameSize } });

        c->conn->sendText (json { { "t", "welcome" }, { "session", c->session }, { "server_version", rovoxVersion() }, { "audio", audioInfo },
                                  { "source_url", options_.sourceUrl } }.dump (-1, ' ', false, json::error_handler_t::replace));
    }
    else if (t == "sub")
    {
        handleSub (c, msg);
    }
    else if (t == "unsub")
    {
        std::lock_guard<std::mutex> lock (c->mutex);
        c->subs.erase (stringField (msg, "project_id"));
    }
    else if (t == "cmd")
    {
        handleCmd (c, msg);
    }
    else if (t == "audio")
    {
        c->wantAudio = msg.contains ("on") && msg["on"].is_boolean() && msg["on"].get<bool>();
        c->needDiscontinuity = true;
    }
    else if (t == "ping")
    {
        c->conn->sendText (json { { "t", "pong" }, { "n", msg.contains ("n") && msg["n"].is_number_integer() ? msg["n"].get<int>() : 0 } }.dump (-1, ' ', false, json::error_handler_t::replace));
    }
    else if (t == "stats")
    {
        json s { { "t", "stats" }, { "clients", clientCount() }, { "dropped_frames", c->conn->droppedFrames.load() } };

        if (host_.stream() != nullptr)
            s["stream"] = host_.stream()->stats();

        c->conn->sendText (s.dump (-1, ' ', false, json::error_handler_t::replace));
    }
    else
    {
        c->conn->sendText (json { { "t", "error" }, { "message", "unknown frame type " + t } }.dump (-1, ' ', false, json::error_handler_t::replace));
    }
}

void Gateway::handleSub (const std::shared_ptr<Client>& c, const json& msg)
{
    const auto projectId = stringField (msg, "project_id");

    if (projectId.empty())
    {
        c->conn->sendText (json { { "t", "error" }, { "message", "project_id is required" } }.dump (-1, ' ', false, json::error_handler_t::replace));
        return;
    }

    Sub sub;
    sub.pidx = projectIndex (projectId);
    sub.state = sub.transport = sub.meters = false;

    const json channels = msg.contains ("channels") && msg["channels"].is_array() ? msg["channels"] : json::array ({ "state", "transport", "meters" });

    for (auto& ch : channels)
    {
        const auto name = ch.is_string() ? ch.get<std::string>() : std::string();
        sub.state = sub.state || name == "state";
        sub.transport = sub.transport || name == "transport";
        sub.meters = sub.meters || name == "meters";
    }

    {
        std::lock_guard<std::mutex> lock (c->mutex);
        c->subs[projectId] = sub;
    }

    auto snapshot = executor_.run ([this, projectId, pidx = sub.pidx] { return buildSnapshot (projectId, pidx); });

    if (snapshot.wait_for (std::chrono::seconds (30)) != std::future_status::ready)
        return c->conn->sendText (json { { "t", "error" }, { "message", "snapshot timed out" }, { "project_id", projectId } }.dump (-1, ' ', false, json::error_handler_t::replace));

    c->conn->sendText (snapshot.get().dump (-1, ' ', false, json::error_handler_t::replace));
}

json Gateway::buildSnapshot (const std::string& projectId, int pidx)
{
    CommandContext ctx;
    ctx.projectId = projectId;
    ctx.origin = { Origin::Kind::ui, "gateway", "ui" };
    auto state = bus_.runNow ("project.get", json::object(), ctx);

    if (! state.ok)
        return { { "t", "error" }, { "project_id", projectId }, { "message", state.error ? state.error->message : "no such project" }, { "code", state.error ? state.error->code : "NOT_FOUND" } };

    auto* project = projects_.find (projectId, "local");
    json index = json::array ({ "master" });

    for (auto* t : te::getAudioTracks (project->edit()))
        index.push_back (Project::idOf (t->state));

    return { { "t", "snapshot" }, { "project_id", projectId }, { "pidx", pidx }, { "revision", state.revision }, { "state", state.data },
             { "transport", project->transport().state() }, { "meter_index", index }, { "accepted_revision", project->acceptedRevision().load() } };
}

void Gateway::handleCmd (const std::shared_ptr<Client>& c, const json& msg)
{
    const auto id = stringField (msg, "id");
    auto reply = [&] (json body)
    {
        body["t"] = "result";
        body["id"] = id;
        c->conn->sendText (body.dump (-1, ' ', false, json::error_handler_t::replace));
    };

    // several operations as one atomic undo step: {"ops":[{"name":..,"params":{..}}, ...]}
    const bool isBatch = msg.contains ("ops") && msg["ops"].is_array();

    if (! isBatch && (! msg.contains ("name") || ! msg["name"].is_string()))
        return reply ({ { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "name is required" } } } });

    const auto name = isBatch ? std::string ("batch") : msg["name"].get<std::string>();
    CommandContext ctx;
    ctx.projectId = stringField (msg, "project_id");
    ctx.origin = { Origin::Kind::ui, c->session, "ui" };
    ctx.undoLabel = stringField (msg, "undo_label");

    if (msg.contains ("if_revision") && msg["if_revision"].is_number_unsigned())
        ctx.ifRevision = msg["if_revision"].get<std::uint64_t>();

    // a series with the same coalesce_key within one second is one undo step (a fader drag)
    const auto key = stringField (msg, "coalesce_key");

    if (! key.empty())
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock (c->mutex);
        ctx.coalesce = c->coalesceKey == key && c->coalesceProject == ctx.projectId && now - c->coalesceAt < std::chrono::seconds (1);
        c->coalesceKey = key;
        c->coalesceProject = ctx.projectId;
        c->coalesceAt = now;
    }

    std::future<CommandResult> future;

    if (isBatch)
    {
        std::vector<Op> ops;

        for (auto& o : msg["ops"])
            if (o.is_object() && o.contains ("name") && o["name"].is_string())
                ops.push_back ({ o["name"].get<std::string>(), (o.contains ("params") && o["params"].is_object() ? o["params"] : json::object()) });

        future = bus_.submitBatch (std::move (ops), ctx);
    }
    else
    {
        future = bus_.submit (name, (msg.contains ("params") && msg["params"].is_object() ? msg["params"] : json::object()), ctx);
    }

    if (future.wait_for (std::chrono::seconds (120)) != std::future_status::ready)
        return reply ({ { "ok", false }, { "error", { { "code", errc::busy }, { "message", "the message thread did not answer in time" } } } });

    auto result = future.get();
    json body { { "ok", result.ok }, { "revision", result.revision } };

    if (result.ok)
        body["data"] = std::move (result.data);
    else if (result.error)
        body["error"] = result.error->toJson();

    reply (std::move (body));
}

//==============================================================================
void Gateway::onHubEvent (const Event& e)
{
    const auto type = e.body.value ("type", "");
    const bool projectState = type == "project.changed" || type == "agent.accepted" || type == "storage.changed";
    const bool projectTransport = type == "transport.changed";

    json out = e.body;
    out["t"] = "event";
    std::string text;

    for (auto& c : clients())
    {
        bool deliver = false;

        if (! e.projectId.empty() && (projectState || projectTransport))
        {
            std::lock_guard<std::mutex> lock (c->mutex);
            auto it = c->subs.find (e.projectId);
            deliver = it != c->subs.end() && (projectState ? it->second.state : it->second.transport);
        }
        else if (e.projectId.empty() || type == "job.updated")
        {
            deliver = true;
        }

        if (! deliver)
            continue;

        if (text.empty())
            text = out.dump (-1, ' ', false, json::error_handler_t::replace);

        c->conn->sendText (text);
    }
}

void Gateway::onAudioPacket (const std::vector<std::uint8_t>& packet)
{
    std::shared_ptr<const std::vector<std::uint8_t>> frame;
    size_t headerLen = 0;

    for (auto& c : clients())
    {
        if (! c->wantAudio)
            continue;

        if (frame == nullptr)
            frame = encodeWsFrame (0x2, packet.data(), packet.size(), &headerLen);

        auto toSend = frame;

        if (c->needDiscontinuity.exchange (false))
        {
            auto copy = std::make_shared<std::vector<std::uint8_t>> (*frame);
            (*copy)[headerLen + 1] |= 1;
            toSend = copy;
        }

        if (c->conn->sendBinary (toSend, true))
            c->needDiscontinuity = true;
    }
}

void Gateway::timerCallback()
{
    const auto all = clients();

    if (all.empty())
        return;

    std::map<std::string, std::vector<std::shared_ptr<Client>>> byProject;

    for (auto& c : all)
    {
        std::lock_guard<std::mutex> lock (c->mutex);

        for (auto& [pid, sub] : c->subs)
            byProject[pid].push_back (c);
    }

    for (auto& [pid, subscribers] : byProject)
    {
        auto* project = projects_.find (pid, "local");

        if (project == nullptr)
            continue;

        auto& transport = project->transport();
        const bool playing = transport.wantsPlaying();
        const bool wasPlaying = wasPlaying_.count (pid) > 0;
        const int pidx = projectIndex (pid);

        if (playing || wasPlaying)
        {
            std::vector<std::uint8_t> pos;
            put<std::uint8_t> (pos, 3);
            put<std::uint8_t> (pos, (std::uint8_t) pidx);
            put<std::uint16_t> (pos, host_.stream() != nullptr ? host_.stream()->epoch() : 0);
            put<double> (pos, transport.positionSeconds());
            put<std::uint8_t> (pos, playing ? 1 : 0);
            const auto posFrame = encodeWsFrame (0x2, pos.data(), pos.size());

            std::vector<std::uint8_t> meters;
            put<std::uint8_t> (meters, 2);
            put<std::uint8_t> (meters, (std::uint8_t) pidx);
            put<std::uint16_t> (meters, 0);
            std::uint16_t count = 0;
            std::uint32_t index = 1;

            for (auto& r : project->meters().sampleLatest())
            {
                const std::uint32_t i = r.trackId == "master" ? 0 : index++;
                put<std::uint32_t> (meters, i);
                put<float> (meters, playing ? r.peakDb[0] : -100.0f);
                put<float> (meters, playing ? r.peakDb[1] : -100.0f);
                ++count;
            }

            std::memcpy (&meters[2], &count, 2);
            const auto meterFrame = encodeWsFrame (0x2, meters.data(), meters.size());

            for (auto& c : subscribers)
            {
                Sub sub;
                {
                    std::lock_guard<std::mutex> lock (c->mutex);
                    auto it = c->subs.find (pid);

                    if (it == c->subs.end())
                        continue;

                    sub = it->second;
                }

                if (sub.transport)
                    c->conn->sendBinary (posFrame, true);

                if (sub.meters)
                    c->conn->sendBinary (meterFrame, true);
            }
        }

        if (playing)
            wasPlaying_.insert (pid);
        else
            wasPlaying_.erase (pid);
    }
}

} // namespace rovox
