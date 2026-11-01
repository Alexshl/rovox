// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Mcp.h"

#include "Ids.h"
#include "JsonSchema.h"
#include "Sha1.h"
#include "Version.h"

#include <algorithm>
#include <fstream>
#include <random>

namespace rovox::mcp
{

namespace
{
constexpr size_t maxEvents = 2000, maxProjectEvents = 1000, sessionRing = 1000;
constexpr int pageSize = 100;

std::string randomHex (int bytes)
{
    std::random_device rd;
    std::string out;
    char buf[3];

    for (int i = 0; i < bytes; ++i)
    {
        std::snprintf (buf, sizeof (buf), "%02x", (unsigned) (rd() & 0xff));
        out += buf;
    }

    return out;
}

std::string dumpSafe (const json& j)    { return j.dump (-1, ' ', false, json::error_handler_t::replace); }

std::string replaceChar (std::string s, char from, char to)
{
    std::replace (s.begin(), s.end(), from, to);
    return s;
}
} // namespace

json rpcError (const json& id, int code, const std::string& message, json data)
{
    json e { { "code", code }, { "message", message } };

    if (! data.is_null())
        e["data"] = std::move (data);

    return { { "jsonrpc", "2.0" }, { "id", id }, { "error", e } };
}

json rpcResult (const json& id, json result)
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move (result) } };
}

//==============================================================================
Session::Session (std::string i, std::string s, std::string t) : id (std::move (i)), shortId (std::move (s)), tenant (std::move (t)) {}

Origin Session::origin() const
{
    // the client's own name ("Claude Code") is what the web UI shows next to the agent's changes; clientInfo is written once, at initialize
    std::string label = "mcp:" + shortId;

    if (initialized.load() && clientInfo.is_object() && clientInfo.contains ("name") && clientInfo["name"].is_string())
        if (auto name = clientInfo["name"].get<std::string>(); ! name.empty() && name.size() <= 64)
            label = name;

    return { Origin::Kind::mcp, shortId, label };
}

std::uint64_t Session::push (json message)
{
    std::uint64_t id2;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        id2 = nextId_++;
        ring_.emplace_back (id2, std::move (message));

        while (ring_.size() > sessionRing)
            ring_.pop_front();
    }

    cv_.notify_all();
    return id2;
}

bool Session::next (std::uint64_t& cursor, json& out, std::chrono::milliseconds timeout, const std::atomic<bool>* stop)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock (mutex_);

    for (;;)
    {
        for (auto& [id2, m] : ring_)
            if (id2 > cursor)
            {
                cursor = id2;
                out = m;
                return true;
            }

        if ((stop != nullptr && stop->load()) || std::chrono::steady_clock::now() >= deadline)
            return false;

        cv_.wait_until (lock, std::min (deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds (200)));
    }
}

std::uint64_t Session::lastMessageId() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return nextId_ - 1;
}

void Session::touch()
{
    std::lock_guard<std::mutex> lock (mutex_);
    lastActive_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point Session::lastActive() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return lastActive_;
}

std::optional<std::uint64_t> Session::lastSeen (const std::string& project) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    auto it = lastSeen_.find (project);
    return it == lastSeen_.end() ? std::nullopt : std::optional<std::uint64_t> (it->second);
}

void Session::setLastSeen (const std::string& project, std::uint64_t revision)
{
    std::lock_guard<std::mutex> lock (mutex_);
    lastSeen_[project] = revision;
}

ToolOutput ToolOutput::success (json data, std::optional<std::uint64_t> revision, std::string project)
{
    ToolOutput o;
    o.data = std::move (data);
    o.revision = revision;
    o.projectId = std::move (project);
    return o;
}

ToolOutput ToolOutput::failure (Error e, std::optional<std::uint64_t> revision, std::string project)
{
    ToolOutput o;
    o.ok = false;
    o.error = std::move (e);
    o.revision = revision;
    o.projectId = std::move (project);
    return o;
}

//==============================================================================
McpServer::McpServer (ProjectManager& p, CommandBus& b, EventHub& h, JobManager& j, JobService& js, McpConfig c)
    : projects_ (p), bus_ (b), hub_ (h), jobs_ (j), jobService_ (js), config_ (std::move (c))
{
    buildTools();
    observerId_ = hub_.addObserver ([this] (const Event& e) { onHubEvent (e); });
    flusher_ = std::thread ([this] { flusherLoop(); });
}

void McpServer::shutdown()
{
    stopping_ = true;
    flushCv_.notify_all();
    eventCv_.notify_all();

    std::lock_guard<std::mutex> lock (sessionsMutex_);

    for (auto& [id, s] : sessions_)
        s->streamGeneration++;
}

McpServer::~McpServer()
{
    hub_.removeObserver (observerId_);
    shutdown();

    if (flusher_.joinable())
        flusher_.join();
}

std::shared_ptr<Session> McpServer::createSession (const std::string& tenant)
{
    std::lock_guard<std::mutex> lock (sessionsMutex_);
    auto s = std::make_shared<Session> (randomHex (16), "s" + std::to_string (nextShortId_++), tenant);
    sessions_[s->id] = s;
    return s;
}

std::shared_ptr<Session> McpServer::findSession (const std::string& id)
{
    std::lock_guard<std::mutex> lock (sessionsMutex_);
    auto it = sessions_.find (id);
    return it == sessions_.end() ? nullptr : it->second;
}

void McpServer::closeSession (const std::string& id)
{
    std::shared_ptr<Session> s;

    {
        std::lock_guard<std::mutex> lock (sessionsMutex_);
        auto it = sessions_.find (id);

        if (it == sessions_.end())
            return;

        s = it->second;
        sessions_.erase (it);
    }

    s->streamGeneration++;          // wakes and ends an open SSE stream
}

int McpServer::sweepSessions()
{
    std::vector<std::string> expired;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock (sessionsMutex_);

        for (auto& [id, s] : sessions_)
            if (std::chrono::duration_cast<std::chrono::seconds> (now - s->lastActive()).count() > config_.sessionTtlS)
                expired.push_back (id);
    }

    for (auto& id : expired)
        closeSession (id);

    return (int) expired.size();
}

//==============================================================================
CommandContext McpServer::contextFor (Session& s, const std::string& projectId) const
{
    CommandContext ctx;
    ctx.projectId = projectId;
    ctx.tenant = s.tenant;
    ctx.origin = s.origin();
    return ctx;
}

CommandResult McpServer::runCommand (const std::string& name, const json& params, const CommandContext& ctx, const std::atomic<bool>* cancelled)
{
    auto future = bus_.submit (name, params, ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (120);

    while (future.wait_for (std::chrono::milliseconds (100)) != std::future_status::ready)
    {
        if (cancelled != nullptr && cancelled->load())
            return CommandResult::fail (makeError (errc::cancelled, "the request was cancelled by the client"));

        if (std::chrono::steady_clock::now() > deadline)
            return CommandResult::fail (makeError (errc::busy, "the message thread did not answer in time"));
    }

    try
    {
        return future.get();
    }
    catch (const std::exception& e)
    {
        return CommandResult::fail (makeError (errc::busy, e.what()));
    }
}

CommandResult McpServer::runBatch (std::vector<Op> ops, const CommandContext& ctx, const std::atomic<bool>* cancelled)
{
    auto future = bus_.submitBatch (std::move (ops), ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (120);

    while (future.wait_for (std::chrono::milliseconds (100)) != std::future_status::ready)
    {
        if (cancelled != nullptr && cancelled->load())
            return CommandResult::fail (makeError (errc::cancelled, "the request was cancelled by the client"));

        if (std::chrono::steady_clock::now() > deadline)
            return CommandResult::fail (makeError (errc::busy, "the message thread did not answer in time"));
    }

    try
    {
        return future.get();
    }
    catch (const std::exception& e)
    {
        return CommandResult::fail (makeError (errc::busy, e.what()));
    }
}

//==============================================================================
std::vector<const ToolDef*> McpServer::tools() const
{
    std::vector<const ToolDef*> out;

    for (auto& [n, t] : tools_)
        out.push_back (&t);

    return out;
}

const ToolDef* McpServer::findTool (const std::string& name) const
{
    auto it = tools_.find (name);

    if (it != tools_.end())
        return &it->second;

    auto alias = toolAliases_.find (name);
    return alias == toolAliases_.end() ? nullptr : &tools_.at (alias->second);
}

bool McpServer::mayBlock (const json& m) const
{
    if (! isRequest (m) || m.value ("method", "") != "tools/call" || ! m.contains ("params") || ! m["params"].is_object())
        return false;

    const auto* tool = findTool (m["params"].value ("name", ""));

    if (tool == nullptr || ! tool->mayBlock)
        return false;

    const auto& args = m["params"].contains ("arguments") && m["params"]["arguments"].is_object() ? m["params"]["arguments"] : json::object();
    // a wait on a job; without wait_s the call returns at once
    return args.value ("wait_s", tool->defaultWaitS) > 0.0;
}

//==============================================================================
std::optional<json> McpServer::handle (Session& s, const json& m, RequestContext& rc)
{
    s.touch();
    rc.session = &s;

    if (! m.is_object())
        return rpcError (nullptr, -32600, "a JSON-RPC message must be an object");

    if (isRequest (m))
    {
        const auto& id = m["id"];

        if (! (id.is_string() || id.is_number()) || ! m["method"].is_string() || m.value ("jsonrpc", "") != "2.0")
            return rpcError (id.is_string() || id.is_number() ? id : json (nullptr), -32600, "invalid request");

        return handleRequest (s, m, rc);
    }

    if (isNotification (m))
    {
        const auto method = m.value ("method", "");

        if (method == "notifications/initialized")
        {
            s.initialized = true;
        }
        else if (method == "notifications/cancelled" && m.contains ("params") && m["params"].is_object() && m["params"].contains ("requestId"))
        {
            std::lock_guard<std::mutex> lock (inflightMutex_);

            if (auto it = inflight_.find (s.id + "|" + m["params"]["requestId"].dump()); it != inflight_.end())
                it->second->store (true);
        }

        return std::nullopt;
    }

    return std::nullopt;        // a response to a server request: we send none
}

json McpServer::handleRequest (Session& s, const json& req, RequestContext& rc)
{
    const auto& id = req["id"];
    const auto method = req["method"].get<std::string>();
    const json params = req.contains ("params") && req["params"].is_object() ? req["params"] : json::object();

    try
    {
        if (method == "initialize")
        {
            const auto requested = params.value ("protocolVersion", "");
            s.protocolVersion = (requested == "2025-06-18" || requested == "2025-03-26") ? requested : latestProtocolVersion;
            s.clientInfo = params.value ("clientInfo", json::object());
            s.clientCapabilities = params.value ("capabilities", json::object());
            s.initialized = true;

            json info { { "name", "rovox" }, { "title", "Rovox" }, { "version", rovoxVersion() } };

            if (! config_.sourceUrl.empty())
                info["source_url"] = config_.sourceUrl;

            return rpcResult (id, { { "protocolVersion", s.protocolVersion },
                                    { "capabilities", { { "tools", { { "listChanged", false } } }, { "resources", { { "subscribe", true }, { "listChanged", true } } }, { "logging", json::object() } } },
                                    { "serverInfo", info },
                                    { "instructions", "Rovox is a headless DAW. Every project tool takes project_id; ids (trk_, itm_, fx_, mrk_, job_) are stable GUIDs, never indexes. "
                                                      "Times: {s}, {bar,beat,tick} (1-based) or {qn}; levels in dB. One tool call is one undo step. "
                                                      "Long work (render.start, render.preview, analyze.audio, import.plan) runs as a job: pass wait_s to wait for it, or poll job.get. "
                                                      "Every project response may carry external_changes: edits made by the user since your last call. "
                                                      "Source code: " + config_.sourceUrl } });
        }

        if (method == "ping")
            return rpcResult (id, json::object());

        if (! s.initialized)
            return rpcError (id, -32600, "the session is not initialized: send initialize first");

        if (method == "tools/list")
        {
            std::vector<const ToolDef*> all = tools();
            size_t offset = 0;

            if (params.contains ("cursor"))
            {
                try { offset = std::stoul (params["cursor"].get<std::string>()); }
                catch (...) { return rpcError (id, -32602, "invalid cursor"); }
            }

            json list = json::array();

            for (size_t i = offset; i < all.size() && i < offset + pageSize; ++i)
            {
                const auto& t = *all[i];
                json name = config_.underscoreNames ? json (replaceChar (t.name, '.', '_')) : json (t.name);
                json entry { { "name", name }, { "title", t.title }, { "description", t.description }, { "inputSchema", t.inputSchema },
                             { "outputSchema", { { "type", "object" }, { "properties", { { "ok", { { "type", "boolean" } } }, { "data", { { "type", "object" } } }, { "revision", { { "type", "integer" } } },
                                                                                          { "error", { { "type", "object" } } }, { "external_changes", { { "type", "object" } } } } }, { "required", { "ok" } } } } };

                if (! t.annotations.empty())
                    entry["annotations"] = t.annotations;

                list.push_back (entry);
            }

            json result { { "tools", list } };

            if (offset + pageSize < all.size())
                result["nextCursor"] = std::to_string (offset + pageSize);

            return rpcResult (id, result);
        }

        if (method == "tools/call")
        {
            const std::string key = s.id + "|" + id.dump();
            {
                std::lock_guard<std::mutex> lock (inflightMutex_);
                inflight_[key] = rc.cancelled;
            }

            struct Done { McpServer& m; std::string k; ~Done() { std::lock_guard<std::mutex> l (m.inflightMutex_); m.inflight_.erase (k); } } done { *this, key };

            if (params.contains ("_meta") && params["_meta"].is_object() && params["_meta"].contains ("progressToken"))
                rc.progressToken = params["_meta"]["progressToken"];

            auto r = handleToolsCall (s, params, rc);
            return r.contains ("error") ? json { { "jsonrpc", "2.0" }, { "id", id }, { "error", r["error"] } } : rpcResult (id, r);
        }

        if (method == "resources/list")
        {
            json list = json::array();
            list.push_back ({ { "uri", "rovox://projects" }, { "name", "projects" }, { "title", "Open projects" }, { "mimeType", "application/json" } });

            for (auto& pr : projects_.list (s.tenant))
            {
                list.push_back ({ { "uri", "rovox://project/" + pr.id + "/summary" }, { "name", pr.name + " (summary)" }, { "mimeType", "application/json" } });
                list.push_back ({ { "uri", "rovox://project/" + pr.id + "/transport" }, { "name", pr.name + " (transport)" }, { "mimeType", "application/json" } });
            }

            return rpcResult (id, { { "resources", list } });
        }

        if (method == "resources/templates/list")
        {
            return rpcResult (id, { { "resourceTemplates", json::array ({
                { { "uriTemplate", "rovox://project/{id}/summary" }, { "name", "project summary" }, { "mimeType", "application/json" } },
                { { "uriTemplate", "rovox://project/{id}/transport" }, { "name", "transport state" }, { "mimeType", "application/json" } },
                { { "uriTemplate", "rovox://jobs/{id}" }, { "name", "job status" }, { "mimeType", "application/json" } },
                { { "uriTemplate", "rovox://project/{id}/renders/{name}" }, { "name", "rendered file" } },
                { { "uriTemplate", "rovox://project/{id}/previews/{name}" }, { "name", "preview clip" } } }) } });
        }

        if (method == "resources/read")
        {
            if (! params.contains ("uri") || ! params["uri"].is_string())
                return rpcError (id, -32602, "uri is required");

            std::optional<Error> error;
            auto contents = readResource (s, params["uri"].get<std::string>(), error);

            if (error)
                return rpcError (id, error->code == errc::notFound ? -32002 : -32602, error->message, error->toJson());

            return rpcResult (id, { { "contents", contents } });
        }

        if (method == "resources/subscribe" || method == "resources/unsubscribe")
        {
            if (! params.contains ("uri") || ! params["uri"].is_string())
                return rpcError (id, -32602, "uri is required");

            std::lock_guard<std::mutex> lock (s.stateMutex);

            if (method == "resources/subscribe")
                s.subscribedUris.insert (params["uri"].get<std::string>());
            else
                s.subscribedUris.erase (params["uri"].get<std::string>());

            return rpcResult (id, json::object());
        }

        if (method == "logging/setLevel")
        {
            s.logLevel = params.value ("level", "info");
            return rpcResult (id, json::object());
        }

        return rpcError (id, -32601, "method not found: " + method);
    }
    catch (const std::exception& e)
    {
        return rpcError (id, -32603, std::string ("internal error: ") + e.what());
    }
}

//==============================================================================
json McpServer::toolResult (const ToolOutput& out) const
{
    json structured;

    if (out.ok)
        structured = { { "ok", true }, { "data", out.data } };
    else
        structured = { { "ok", false }, { "error", out.error ? out.error->toJson() : json { { "code", errc::internal }, { "message", "unknown error" } } } };

    if (out.revision)
        structured["revision"] = *out.revision;

    json content = json::array ({ { { "type", "text" }, { "text", dumpSafe (structured) } } });

    for (auto& c : out.extraContent)
        content.push_back (c);

    return { { "content", content }, { "structuredContent", structured }, { "isError", ! out.ok } };
}

json McpServer::handleToolsCall (Session& s, const json& params, RequestContext& rc)
{
    if (! params.contains ("name") || ! params["name"].is_string())
        return { { "error", { { "code", -32602 }, { "message", "name is required" } } } };

    const auto name = params["name"].get<std::string>();
    const auto* tool = findTool (name);

    if (tool == nullptr)
    {
        // reserved namespaces answer NOT_IMPLEMENTED with the list of what exists (task 210), anything else is an unknown tool
        static const char* reserved[] = { "template.", "notation.", "recipe.", "midi.", "routing.", "transport.set", "transport.get", "mixer.", "delivery.",
                                          "plugin.", "fx.presets", "fx.preset_load", "fx.move", "track.duplicate", "item.duplicate", "item.create_midi" };

        for (auto* r : reserved)
            if (name.rfind (r, 0) == 0 || replaceChar (name, '_', '.').rfind (r, 0) == 0)
            {
                json available = json::array();

                for (auto* t : tools())
                    available.push_back (t->name);

                return toolResult (ToolOutput::failure (makeError (errc::notImplemented, name + " is reserved and not implemented in this version", json { { "available", available } })));
            }

        return { { "error", { { "code", -32602 }, { "message", "unknown tool: " + name } } } };
    }

    json args = params.contains ("arguments") ? params["arguments"] : json::object();

    if (! args.is_object())
        return { { "error", { { "code", -32602 }, { "message", "arguments must be an object" } } } };

    std::string projectId = args.value ("project_id", "");

    if (! projectId.empty() && ! s.lastSeen (projectId))
        if (auto rev = projects_.revisionOf (projectId))
            s.setLastSeen (projectId, *rev);

    ToolOutput out;

    if (auto v = validateJson (tool->inputSchema, args); ! v.ok())
    {
        out = ToolOutput::failure (v.error(), std::nullopt, projectId);
    }
    else
    {
        try
        {
            ToolCall call { *this, s, rc, args, tool->name };
            out = tool->handler (call);
        }
        catch (const std::exception& e)
        {
            out = ToolOutput::failure (makeError (errc::internal, tool->name + ": " + e.what()));
        }
    }

    auto result = toolResult (out);
    const auto project = out.projectId.empty() ? projectId : out.projectId;

    if (! project.empty() && tool->name != "project.delete" && tool->name != "project.close")
    {
        auto ext = externalChanges (s, project, false);

        if (! ext.is_null())
        {
            result["structuredContent"]["external_changes"] = ext;
            result["content"][0]["text"] = dumpSafe (result["structuredContent"]);
        }
    }

    return result;
}

//==============================================================================
json McpServer::externalChanges (Session& s, const std::string& project, bool)
{
    const auto current = projects_.revisionOf (project);

    if (! current)
        return nullptr;

    const auto seen = s.lastSeen (project);
    s.setLastSeen (project, *current);

    if (! seen || *seen >= *current)
        return nullptr;

    json summary = json::array();
    int count = 0;

    {
        std::lock_guard<std::mutex> lock (eventMutex_);
        auto it = projectEvents_.find (project);

        if (it != projectEvents_.end())
            for (auto& e : it->second)
            {
                if (e.revision <= *seen || e.revision > *current)
                    continue;

                if (e.origin.value ("session", "") == s.shortId && e.origin.value ("kind", "") == "mcp")
                    continue;

                ++count;

                if (summary.size() < 20)
                    summary.push_back (e.undoLabel.empty() ? e.origin.value ("label", "") + ": change" : e.undoLabel);
            }
    }

    if (count == 0)
        return nullptr;

    return { { "since_revision", *seen }, { "count", count }, { "summary", summary }, { "truncated", count > 20 } };
}

Result<json> McpServer::diffSince (const std::string& project, std::uint64_t since)
{
    const auto current = projects_.revisionOf (project);

    if (! current)
        return makeError (errc::notFound, "no open project " + project);

    std::lock_guard<std::mutex> lock (eventMutex_);
    auto it = projectEvents_.find (project);
    json changes = json::array();

    if (since >= *current)
        return json { { "from_revision", since }, { "to_revision", *current }, { "changes", changes } };

    if (it == projectEvents_.end() || it->second.empty() || it->second.front().revision > since + 1)
        return makeError (errc::resyncRequired, "the change buffer does not reach back to revision " + std::to_string (since) + "; read the project with project.get",
                          json { { "current_revision", *current } });

    for (auto& e : it->second)
        if (e.revision > since)
            changes.push_back ({ { "revision", e.revision }, { "origin", e.origin }, { "undo_label", e.undoLabel }, { "entities", e.changes } });

    return json { { "from_revision", since }, { "to_revision", *current }, { "changes", changes }, { "note", "before/after values are not recorded; entities lists what changed, project.get returns the current values" } };
}

json McpServer::pollEvents (const std::string& tenant, const std::string& project, std::uint64_t since, size_t limit, std::chrono::milliseconds wait, const std::atomic<bool>* cancelled)
{
    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::unique_lock<std::mutex> lock (eventMutex_);

    for (;;)
    {
        json out = json::array();
        std::uint64_t last = since;

        for (auto& e : events_)
        {
            const auto eid = e.value ("event_id", (std::uint64_t) 0);

            if (eid <= since || e.value ("tenant_id", "") != tenant || (! project.empty() && e.value ("project_id", "") != project))
                continue;

            if (out.size() >= limit)
                break;

            out.push_back (e);
            last = eid;
        }

        const bool resync = ! events_.empty() && since > 0 && events_.front().value ("event_id", (std::uint64_t) 0) > since + 1000000;

        if (! out.empty() || std::chrono::steady_clock::now() >= deadline || stopping_ || (cancelled != nullptr && cancelled->load()))
            return { { "events", out }, { "next_event_id", last }, { "resync_required", resync } };

        eventCv_.wait_until (lock, std::min (deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds (100)));
    }
}

//==============================================================================
void McpServer::onHubEvent (const Event& e)
{
    const auto type = e.body.value ("type", "");
    std::string summaryUri;
    json forwarded;
    std::string projectId = e.projectId;

    {
        std::lock_guard<std::mutex> lock (eventMutex_);
        json body = e.body;
        body["project_id"] = e.projectId.empty() ? body.value ("project_id", "") : e.projectId;
        events_.push_back (body);

        while (events_.size() > maxEvents)
            events_.pop_front();

        if (type == "project.changed")
        {
            ProjectEvent pe;
            pe.eventId = e.id;
            pe.revision = e.body.value ("revision", (std::uint64_t) 0);
            pe.origin = e.body.value ("origin", json::object());
            pe.changes = e.body.value ("changes", json::array());
            pe.undoLabel = e.body.value ("undo_label", "");
            pe.tenant = e.tenant;
            auto& ring = projectEvents_[e.projectId];
            ring.push_back (pe);

            while (ring.size() > maxProjectEvents)
                ring.pop_front();

            summaryUri = "rovox://project/" + e.projectId + "/summary";
            forwarded = { { "jsonrpc", "2.0" }, { "method", "notifications/rovox/project_changed" },
                          { "params", { { "project_id", e.projectId }, { "revision", pe.revision }, { "origin", pe.origin }, { "undo_label", pe.undoLabel }, { "changes", pe.changes } } } };
        }
    }

    eventCv_.notify_all();

    if (! summaryUri.empty())
        markDirty (summaryUri, false);

    if (type == "job.updated" && e.body.contains ("job"))
        markDirty ("rovox://jobs/" + e.body["job"].value ("job_id", ""), true);

    if (! forwarded.is_null())
    {
        const auto origin = forwarded["params"]["origin"];
        notifySubscribers (forwarded, [&] (Session& s)
        {
            std::lock_guard<std::mutex> lock (s.stateMutex);
            return s.tenant == e.tenant && (s.eventProjects.count ("*") || s.eventProjects.count (projectId))
                   && ! (origin.value ("kind", "") == "mcp" && origin.value ("session", "") == s.shortId);
        });
    }
}

void McpServer::notifySubscribers (const json& n, const std::function<bool (Session&)>& wants)
{
    std::vector<std::shared_ptr<Session>> copy;

    {
        std::lock_guard<std::mutex> lock (sessionsMutex_);

        for (auto& [id, s] : sessions_)
            copy.push_back (s);
    }

    for (auto& s : copy)
        if (wants (*s))
            s->push (n);
}

void McpServer::markDirty (const std::string& uri, bool immediate)
{
    {
        std::lock_guard<std::mutex> lock (eventMutex_);
        const auto due = std::chrono::steady_clock::now() + (immediate ? std::chrono::milliseconds (0) : std::chrono::milliseconds (250));
        auto it = dirtyUris_.find (uri);

        if (it == dirtyUris_.end())
            dirtyUris_[uri] = due;
        else if (immediate)
            it->second = std::min (it->second, due);        // keep the earlier deadline: debounce, not postponement
    }

    flushCv_.notify_all();
}

void McpServer::announceProjectsChanged()
{
    markDirty ("rovox://projects", true);
}

void McpServer::flusherLoop()
{
    std::unique_lock<std::mutex> lock (eventMutex_);

    while (! stopping_)
    {
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds (500);

        for (auto& [uri, due] : dirtyUris_)
            next = std::min (next, due);

        flushCv_.wait_until (lock, next);

        if (stopping_)
            break;

        std::vector<std::string> due;
        const auto now = std::chrono::steady_clock::now();

        for (auto it = dirtyUris_.begin(); it != dirtyUris_.end();)
        {
            if (it->second <= now)
            {
                due.push_back (it->first);
                it = dirtyUris_.erase (it);
            }
            else
            {
                ++it;
            }
        }

        if (due.empty())
            continue;

        lock.unlock();

        for (auto& uri : due)
            notifySubscribers ({ { "jsonrpc", "2.0" }, { "method", "notifications/resources/updated" }, { "params", { { "uri", uri } } } }, [&] (Session& s)
            {
                std::lock_guard<std::mutex> l (s.stateMutex);
                return s.subscribedUris.count (uri) > 0;
            });

        lock.lock();
    }
}

//==============================================================================
json McpServer::readResource (Session& s, const std::string& uri, std::optional<Error>& error)
{
    auto text = [&] (const json& j) { return json::array ({ { { "uri", uri }, { "mimeType", "application/json" }, { "text", dumpSafe (j) } } }); };
    auto fail = [&] (const std::string& code, const std::string& msg) { error = makeError (code, msg, json { { "uri", uri } }); return json(); };

    if (uri == "rovox://projects")
    {
        json list = json::array();

        for (auto& p : projects_.list (s.tenant))
            list.push_back (p.toJson());

        return text ({ { "projects", list } });
    }

    static const std::string prefix = "rovox://project/", jobs = "rovox://jobs/";

    if (uri.rfind (jobs, 0) == 0)
    {
        auto info = jobs_.get (uri.substr (jobs.size()));

        if (! info || info->tenant != s.tenant)
            return fail (errc::notFound, "no such job");

        return text (info->toJson());
    }

    if (uri.rfind (prefix, 0) == 0)
    {
        const auto rest = uri.substr (prefix.size());
        const auto slash = rest.find ('/');

        if (slash == std::string::npos)
            return fail (errc::invalidParams, "unknown resource");

        const auto project = rest.substr (0, slash), what = rest.substr (slash + 1);
        auto summary = projects_.revisionOf (project);

        if (! summary)
            return fail (errc::notFound, "no open project " + project);

        if (what == "summary")
        {
            auto r = runCommand ("project.get", { { "include", json::array ({ "tracks", "markers", "tempo" }) } }, contextFor (s, project));

            if (! r.ok)
            {
                error = r.error;
                return json();
            }

            return text (r.data);
        }

        if (what == "transport")
        {
            auto r = runCommand ("transport.get", json::object(), contextFor (s, project));

            if (! r.ok)
            {
                error = r.error;
                return json();
            }

            return text (r.data);
        }

        for (const char* dir : { "renders/", "previews/" })
        {
            const std::string d = dir;

            if (what.rfind (d, 0) != 0)
                continue;

            auto name = what.substr (d.size());

            if (name.empty() || name.find ('/') != std::string::npos || name.find ("..") != std::string::npos || name[0] == '.')
                return fail (errc::invalidParams, "bad file name");

            auto r = runCommand ("project.info", json::object(), contextFor (s, project));

            if (! r.ok)
            {
                error = r.error;
                return json();
            }

            const fs::path dirPath = fs::path (r.data.value ("path", "")) / (d == "renders/" ? "renders" : "previews");
            fs::path file = dirPath / name;

            if (name == "current")
            {
                std::error_code ec;
                std::optional<fs::file_time_type> newest;

                for (auto& f : fs::directory_iterator (dirPath, ec))
                    if (f.is_regular_file() && f.path().filename().string().rfind ("pv_", 0) == 0 && f.path().extension() != ".json" && (! newest || f.last_write_time() > *newest))
                    {
                        newest = f.last_write_time();
                        file = f.path();
                    }
            }

            std::error_code ec;

            if (! fs::is_regular_file (file, ec))
                return fail (errc::notFound, "no such file");

            const auto ext = file.extension().string();
            const std::string mime = ext == ".mp3" ? "audio/mpeg" : (ext == ".flac" ? "audio/flac" : (ext == ".wav" ? "audio/wav" : "application/octet-stream"));
            const auto size = (std::uint64_t) fs::file_size (file, ec);

            if (size > 4u * 1024 * 1024)
                return json::array ({ { { "uri", uri }, { "mimeType", "application/json" },
                                        { "text", dumpSafe ({ { "path", file.string() }, { "bytes", size }, { "mimeType", mime }, { "url", "/api/projects/" + project + "/" + d + file.filename().string() },
                                                              { "note", "larger than 4 MiB: fetch it over HTTP or from the path" } }) } } });

            std::ifstream in (file, std::ios::binary);
            std::string bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
            return json::array ({ { { "uri", uri }, { "mimeType", mime }, { "blob", base64Encode (reinterpret_cast<const std::uint8_t*> (bytes.data()), bytes.size()) } } });
        }
    }

    return fail (errc::notFound, "unknown resource " + uri);
}

} // namespace rovox::mcp
