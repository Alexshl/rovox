// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// WebSocket gateway of the web UI (task 300): one socket per browser tab carries commands, project events, 30 Hz meters and
// playhead positions, and the Opus audio of the browser stream. It adds no model of its own: commands go through the
// CommandBus with origin `ui`, events come from the EventHub.
//
//   -> {"t":"hello","token":"...","client":"web/0.1"}      <- {"t":"welcome","session":"ws_3","server_version":..,"audio":..}
//   -> {"t":"sub","project_id":"p_..","channels":["state","transport","meters"]}
//   <- {"t":"snapshot","project_id","pidx","revision","state":{project.get},"transport":{..},"meter_index":["master","trk_.."],"accepted_revision"}
//   -> {"t":"cmd","id":"c_7","name":"track.update","params":{..},"project_id":"p_..","if_revision":12,"coalesce_key":"fader:trk_1"}
//   <- {"t":"result","id":"c_7","ok":true,"revision":13,"data":{..}}  |  {"ok":false,"error":{..}}
//   <- {"t":"event", ...project.changed / transport.changed / agent.accepted / projects.changed / job.updated body...}
//   <- {"t":"resync","project_id":"p_.."}
//   -> {"t":"audio","on":true}                                (start / stop receiving 0x01 audio frames)
//   binary <-  0x01 audio (see BrowserStream.h) | 0x02 meters | 0x03 transport position
//     0x02: u8 2 | u8 pidx | u16 count | count x { u32 index | f32 peakL_dB | f32 peakR_dB }   (index into meter_index)
//     0x03: u8 3 | u8 pidx | u16 epoch | f64 position_s | u8 playing                            (little endian)
#pragma once

#include "CommandBus.h"
#include "EventHub.h"
#include "ProjectManager.h"
#include "WebSocket.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace rovox
{

struct GatewayOptions
{
    std::string token;                          // empty = open
    std::vector<std::string> allowedOrigins;    // besides loopback names and the Host of the request itself
    size_t maxClients = 16;
    std::string sourceUrl;
};

class Gateway : private juce::Timer
{
public:
    /** Message thread (the 30 Hz timer and the audio hand-off live there). */
    Gateway (EngineHost&, ProjectManager&, CommandBus&, EventHub&, MessageThreadExecutor&, GatewayOptions);
    ~Gateway() override;

    /** HTTP pool thread; takes ownership of the socket, returns when the connection is gone. */
    void handleUpgrade (int fd, WsRequest);

    /** Closes every connection (before the HTTP server stops). */
    void shutdown();

    int clientCount() const;

    /** Authorisation of an HTTP request / upgrade: `Authorization: Bearer`, cookie `rovox_token`, or `?token=`. */
    static bool tokenMatches (const std::string& expected, const std::string& authorization, const std::string& cookie, const std::string& query);
    static bool originAllowed (const std::string& origin, const std::string& host, const std::vector<std::string>& allowed);

private:
    struct Sub
    {
        int pidx = 0;
        bool state = true, transport = true, meters = true;
    };

    struct Client
    {
        std::string session;
        std::shared_ptr<WsConnection> conn;
        std::mutex mutex;
        std::map<std::string, Sub> subs;        // by project id
        std::atomic<bool> wantAudio { false }, needDiscontinuity { false };
        std::string coalesceKey, coalesceProject;
        std::chrono::steady_clock::time_point coalesceAt;
    };

    void timerCallback() override;
    void onMessage (const std::shared_ptr<Client>&, const std::string& text);
    void handleSub (const std::shared_ptr<Client>&, const json&);
    void handleCmd (const std::shared_ptr<Client>&, const json&);
    json buildSnapshot (const std::string& projectId, int pidx);
    void onHubEvent (const Event&);
    void onAudioPacket (const std::vector<std::uint8_t>&);
    std::vector<std::shared_ptr<Client>> clients();
    int projectIndex (const std::string& projectId);

    EngineHost& host_;
    ProjectManager& projects_;
    CommandBus& bus_;
    EventHub& hub_;
    MessageThreadExecutor& executor_;
    GatewayOptions options_;
    int observerId_ = 0;
    std::atomic<int> nextSession_ { 1 };
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::map<std::string, int> projectIndex_;
    std::set<std::string> wasPlaying_;
    std::atomic<bool> stopping_ { false };
};

} // namespace rovox
