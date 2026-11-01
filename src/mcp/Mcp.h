// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// MCP server (task 200, 210, 220): JSON-RPC 2.0, MCP 2025-06-18. Transport-independent core; stdio and Streamable HTTP
// are thin wrappers (McpStdio, McpHttp). Every tool is an adapter over the CommandBus or the JobService.
#pragma once

#include "CommandBus.h"
#include "EventHub.h"
#include "JobManager.h"
#include "ProjectManager.h"
#include "RenderJobs.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace rovox::mcp
{

inline constexpr const char* latestProtocolVersion = "2025-06-18";

struct McpConfig
{
    std::string sourceUrl = "https://github.com/Alexshl/rovox";   // AGPL section 13: where the running source is
    bool underscoreNames = false;           // list tools as track_list instead of track.list (calls accept both)
    bool reaperCompat = false;              // extra tool names of the old mcp__reaper__ bridge (list_projects, update_tracks, ...)
    std::vector<std::string> allowedOrigins;    // HTTP: Origin values accepted besides loopback
    int sessionTtlS = 1800;                 // idle HTTP sessions expire
    std::function<Result<json> (const std::string&)> uploadBatchStatus;     // set by `serve`; enables upload.status
    std::function<bool()> audioActive;      // optional
};

/** One client connection. The outbound queue is a ring with ids so that an SSE stream can resume after a drop. */
class Session
{
public:
    Session (std::string id, std::string shortId, std::string tenant);

    const std::string id, shortId, tenant;
    json clientInfo = json::object(), clientCapabilities = json::object();
    std::string protocolVersion = latestProtocolVersion;
    std::atomic<bool> initialized { false };

    Origin origin() const;

    /** Appends a JSON-RPC message (notification) to the outbound ring; returns its id. */
    std::uint64_t push (json message);
    /** Blocks up to `timeout` for a message with id > cursor. Returns false on timeout or when `stop` becomes true. */
    bool next (std::uint64_t& cursor, json& out, std::chrono::milliseconds timeout, const std::atomic<bool>* stop = nullptr);
    std::uint64_t lastMessageId() const;

    void touch();
    std::chrono::steady_clock::time_point lastActive() const;
    std::atomic<int> streamGeneration { 0 };            // a new GET stream replaces the old one

    // per-project bookkeeping for external_changes
    std::optional<std::uint64_t> lastSeen (const std::string& project) const;
    void setLastSeen (const std::string& project, std::uint64_t revision);

    std::set<std::string> subscribedUris;               // resources/subscribe
    std::set<std::string> eventProjects;                // events.subscribe ("*" = all)
    std::string logLevel = "info";
    mutable std::mutex stateMutex;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::uint64_t, json>> ring_;
    std::uint64_t nextId_ = 1;
    std::map<std::string, std::uint64_t> lastSeen_;
    std::chrono::steady_clock::time_point lastActive_ = std::chrono::steady_clock::now();
};

/** Where a request's own notifications (progress) go. Null = the session's outbound ring. */
using Emitter = std::function<void (const json&)>;

struct RequestContext
{
    Session* session = nullptr;
    Emitter emit;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>> (false);
    json progressToken = nullptr;
};

struct ToolOutput
{
    bool ok = true;
    json data = json::object();
    std::optional<Error> error;
    std::optional<std::uint64_t> revision;
    std::string projectId;                  // for external_changes
    std::vector<json> extraContent;         // e.g. audio content

    static ToolOutput success (json data, std::optional<std::uint64_t> revision = std::nullopt, std::string project = {});
    static ToolOutput failure (Error, std::optional<std::uint64_t> revision = std::nullopt, std::string project = {});
};

struct ToolCall
{
    class McpServer& server;
    Session& session;
    RequestContext& rc;
    json args;
    std::string name;
};

struct ToolDef
{
    std::string name, title, description;
    json inputSchema;
    json annotations = json::object();
    std::function<ToolOutput (ToolCall&)> handler;
    bool mayBlock = false;                  // can wait on a job: the transport must not run it on the ordered lane
    double defaultWaitS = 0;                // wait_s when the caller gives none
};

class McpServer
{
public:
    McpServer (ProjectManager&, CommandBus&, EventHub&, JobManager&, JobService&, McpConfig = {});
    ~McpServer();

    std::shared_ptr<Session> createSession (const std::string& tenant = "local");
    std::shared_ptr<Session> findSession (const std::string& id);
    void closeSession (const std::string& id);
    int sweepSessions();                    // idle HTTP sessions; returns how many were closed

    /** Handles one JSON-RPC message. A request returns its response; notifications and client responses return nullopt.
        Blocks while a tool waits for the message thread or a job: call it from a transport thread, never from the message thread. */
    std::optional<json> handle (Session&, const json& message, RequestContext&);

    /** True if this request may take long (wait for a job): transports run it on a worker instead of the ordered lane. */
    bool mayBlock (const json& message) const;

    static bool isRequest (const json& m)       { return m.is_object() && m.contains ("method") && m.contains ("id"); }
    static bool isNotification (const json& m)  { return m.is_object() && m.contains ("method") && ! m.contains ("id"); }

    const McpConfig& config() const             { return config_; }
    /** Ends open SSE streams and long polls; call before stopping the HTTP server. */
    void shutdown();
    const std::atomic<bool>& stoppingFlag() const   { return stopping_; }
    std::vector<const ToolDef*> tools() const;
    const ToolDef* findTool (const std::string& name) const;

    // used by tools
    ProjectManager& projects()                  { return projects_; }
    CommandBus& bus()                           { return bus_; }
    JobManager& jobs()                          { return jobs_; }
    JobService& jobService()                    { return jobService_; }
    EventHub& hub()                             { return hub_; }

    /** Waits for the command-bus future with a deadline and cancellation. */
    CommandResult runCommand (const std::string& name, const json& params, const CommandContext&, const std::atomic<bool>* cancelled = nullptr);
    CommandResult runBatch (std::vector<Op>, const CommandContext&, const std::atomic<bool>* cancelled = nullptr);
    CommandContext contextFor (Session&, const std::string& projectId) const;

    /** events.poll: events with event_id > since (all projects of the tenant, or one), waiting up to `wait`. */
    json pollEvents (const std::string& tenant, const std::string& project, std::uint64_t since, size_t limit, std::chrono::milliseconds wait, const std::atomic<bool>* cancelled);
    /** project.diff */
    Result<json> diffSince (const std::string& project, std::uint64_t since);
    json externalChanges (Session&, const std::string& project, bool baselineOnly);

    json readResource (Session&, const std::string& uri, std::optional<Error>& error);
    void announceProjectsChanged();

private:
    friend class ToolRegistry;
    void buildTools();
    json handleRequest (Session&, const json& req, RequestContext&);
    json handleToolsCall (Session&, const json& params, RequestContext&);
    json toolResult (const ToolOutput&) const;
    void onHubEvent (const Event&);
    void flusherLoop();
    void markDirty (const std::string& uri, bool immediate);
    void notifySubscribers (const json& notification, const std::function<bool (Session&)>& wants);

    ProjectManager& projects_;
    CommandBus& bus_;
    EventHub& hub_;
    JobManager& jobs_;
    JobService& jobService_;
    McpConfig config_;

    std::map<std::string, ToolDef> tools_;
    std::map<std::string, std::string> toolAliases_;          // alternative spelling -> canonical name

    std::mutex sessionsMutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    int nextShortId_ = 1;

    // event rings
    struct ProjectEvent
    {
        std::uint64_t eventId = 0, revision = 0;
        json origin, changes;
        std::string undoLabel, tenant;
    };
    std::mutex eventMutex_;
    std::condition_variable eventCv_;
    std::deque<json> events_;                                  // every hub event, newest last (capped)
    std::map<std::string, std::deque<ProjectEvent>> projectEvents_;
    std::map<std::string, std::uint64_t> projectEventFloor_;   // lowest revision the ring can answer for
    std::map<std::string, std::chrono::steady_clock::time_point> dirtyUris_;
    std::condition_variable flushCv_;
    std::thread flusher_;
    std::atomic<bool> stopping_ { false };
    int observerId_ = 0;

    std::mutex inflightMutex_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> inflight_;
};

/** Convenience used by tests and transports: a response object. */
json rpcError (const json& id, int code, const std::string& message, json data = nullptr);
json rpcResult (const json& id, json result);

} // namespace rovox::mcp
