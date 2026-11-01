// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Spike P4: in-process MCP server (JSON-RPC 2.0, newline-delimited over stdio) with two open Tracktion Edits.
//   rovox_spikes mcp [--work DIR]
// Protocol: initialize / notifications/initialized / ping / tools/list / tools/call.
// Threads: stdin reader -> message thread (MessageManager::callAsync, FIFO) -> single stdout writer (mutex).
// Every mutating tools/call is one undo transaction and produces exactly one event {project_id, revision, origin}.
// Edits that happen outside a command scope (Tracktion's own writes) are caught by a ValueTree listener and reported
// with origin "system" (coalesced through an AsyncUpdater).

#include "SpikeUtil.h"

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

using namespace spike;
using namespace te::literals;

namespace
{
constexpr const char* kProtocolVersion = "2025-06-18";

struct Project;
struct App;

struct Event
{
    juce::int64 id = 0;
    juce::String projectId, origin, tool;
    juce::int64 revision = 0;
    juce::StringArray trackIds;
    int coalescedChanges = 0;
};

struct Project : private juce::ValueTree::Listener, private juce::AsyncUpdater
{
    Project (App& a, juce::String pid, juce::String n, std::unique_ptr<te::Edit> e)
        : app (a), id (std::move (pid)), name (std::move (n)), edit (std::move (e))
    {
        edit->state.addListener (this);
    }

    ~Project() override
    {
        cancelPendingUpdate();
        edit->state.removeListener (this);
    }

    // ---- scope: marks a block of changes as belonging to one origin; commit() emits exactly one event ----
    struct Scope
    {
        Scope (Project& p, juce::String o, juce::String t) : project (p), previousOrigin (p.currentOrigin)
        {
            project.currentOrigin = std::move (o);
            project.scopeTool = std::move (t);
            project.changesInScope = 0;
            project.touchedTracks.clear();
        }

        ~Scope()
        {
            project.currentOrigin = previousOrigin;
        }

        void commit()
        {
            if (project.changesInScope > 0)
                project.emit (project.currentOrigin, project.scopeTool, project.changesInScope, project.touchedTracks);
        }

        Project& project;
        juce::String previousOrigin;
    };

    void emit (const juce::String& origin, const juce::String& tool, int changes, const juce::StringArray& tracks);

    te::AudioTrack* findAudioTrack (const juce::String& trackId) const
    {
        for (auto* t : te::getAudioTracks (*edit))
            if (t->itemID.toString() == trackId)
                return t;

        return nullptr;
    }

    App& app;
    juce::String id, name;
    std::unique_ptr<te::Edit> edit;
    std::atomic<juce::int64> revision { 0 };
    juce::String currentOrigin = "system", scopeTool;
    int changesInScope = 0;
    juce::StringArray touchedTracks;
    int pendingSystemChanges = 0;
    juce::String firstSystemChange;

private:
    void noteChange (const juce::ValueTree& t, const juce::Identifier& what)
    {
        // Finding: Tracktion stamps EDIT.lastSignificantChange (a timestamp) after every real change. It is not a
        // user-visible change, and counting it would double every event, so it is filtered out here.
        static const juce::Identifier lastSignificantChange ("lastSignificantChange");

        if (what == lastSignificantChange)
            return;

        // entity: nearest ancestor that is a TRACK / AUDIOTRACK with an id
        juce::String trackId;

        for (auto v = t; v.isValid(); v = v.getParent())
            if (v.hasProperty (te::IDs::id) && v.hasType (te::IDs::TRACK))
            {
                trackId = v[te::IDs::id].toString();
                break;
            }

        if (currentOrigin != "system")
        {
            ++changesInScope;

            if (trackId.isNotEmpty())
                touchedTracks.addIfNotAlreadyThere (trackId);
        }
        else
        {
            if (pendingSystemChanges == 0)
                firstSystemChange = t.getType().toString() + "." + what.toString();

            ++pendingSystemChanges;
            triggerAsyncUpdate();
        }
    }

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override  { noteChange (t, p); }
    void valueTreeChildAdded (juce::ValueTree& t, juce::ValueTree& c) override              { noteChange (t, c.getType()); }
    void valueTreeChildRemoved (juce::ValueTree& t, juce::ValueTree& c, int) override       { noteChange (t, c.getType()); }
    void valueTreeChildOrderChanged (juce::ValueTree& t, int, int) override                 { noteChange (t, "order"); }

    void handleAsyncUpdate() override
    {
        const auto n = pendingSystemChanges;
        pendingSystemChanges = 0;
        emit ("system", "system:" + firstSystemChange, n, {});
    }
};

struct App
{
    App (const juce::File& dir) : workDir (dir)
    {
        auto behaviour = std::make_unique<SpikeBehaviour>();
        behaviour->autoInit = false;
        behaviour->systemDevices = false;
        engine = std::make_unique<te::Engine> ("rovox_p4_mcp", nullptr, std::move (behaviour));

        for (auto [pid, name] : { std::pair<const char*, const char*> { "A", "Song A" }, { "B", "Song B" } })
        {
            auto edit = te::loadEditFromState (*engine, te::createEmptyEdit (*engine), te::Edit::EditRole::forEditing);
            projects.push_back (std::make_unique<Project> (*this, juce::String ("proj-") + pid, name, std::move (edit)));
        }
    }

    Project* find (const juce::var& id)
    {
        for (auto& p : projects)
            if (p->id == id.toString())
                return p.get();

        return nullptr;
    }

    void writeLine (const juce::var& v)
    {
        const auto s = juce::JSON::toString (v, true);   // single line
        std::lock_guard<std::mutex> l (outMutex);
        std::fwrite (s.toRawUTF8(), 1, s.getNumBytesAsUTF8(), stdout);
        std::fputc ('\n', stdout);
        std::fflush (stdout);
    }

    // ---- JSON-RPC helpers ----
    static juce::var rpcResult (const juce::var& id, const juce::var& result)
    {
        return makeObject ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } });
    }

    static juce::var rpcError (const juce::var& id, int code, const juce::String& msg)
    {
        return makeObject ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", makeObject ({ { "code", code }, { "message", msg } }) } });
    }

    static juce::var toolResult (const juce::var& structured, bool isError = false)
    {
        juce::Array<juce::var> content;
        content.add (makeObject ({ { "type", "text" }, { "text", juce::JSON::toString (structured, true) } }));
        return makeObject ({ { "content", content }, { "structuredContent", structured }, { "isError", isError } });
    }

    static juce::var toolError (const juce::String& code, const juce::String& message, juce::var extra = {})
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("code", code);
        o->setProperty ("message", message);

        if (auto* eo = extra.getDynamicObject())
            for (auto& nv : eo->getProperties())
                o->setProperty (nv.name, nv.value);

        return toolResult (juce::var (o), true);
    }

    static juce::var tool (const char* name, const char* description, std::initializer_list<std::pair<const char*, const char*>> props,
                           std::initializer_list<const char*> required)
    {
        auto* p = new juce::DynamicObject();

        for (auto& [k, type] : props)
            p->setProperty (k, makeObject ({ { "type", type } }));

        juce::Array<juce::var> req;

        for (auto r : required)
            req.add (r);

        return makeObject ({ { "name", name }, { "description", description },
                             { "inputSchema", makeObject ({ { "type", "object" }, { "properties", juce::var (p) }, { "required", req } }) } });
    }

    juce::var toolsList()
    {
        juce::Array<juce::var> t;
        t.add (tool ("project.list", "List open projects with revision.", {}, {}));
        t.add (tool ("track.list", "List audio tracks of a project.", { { "project_id", "string" } }, { "project_id" }));
        t.add (tool ("track.add", "Add an audio track (one undo step).", { { "project_id", "string" }, { "name", "string" }, { "sine_hz", "number" }, { "if_revision", "integer" } }, { "project_id" }));
        t.add (tool ("track.set_volume_db", "Set track volume in dB (one undo step).", { { "project_id", "string" }, { "track_id", "string" }, { "volume_db", "number" }, { "if_revision", "integer" } }, { "project_id", "track_id", "volume_db" }));
        t.add (tool ("render", "Render the project offline to a WAV and return level stats.", { { "project_id", "string" }, { "seconds", "number" } }, { "project_id" }));
        t.add (tool ("undo", "Undo the last transaction of this project only.", { { "project_id", "string" } }, { "project_id" }));
        t.add (tool ("events.poll", "Return events with id > since.", { { "since", "integer" }, { "limit", "integer" } }, {}));
        t.add (tool ("debug.sleep", "Block the message thread for N ms (spike: shows head-of-line blocking).", { { "ms", "integer" } }, { "ms" }));
        t.add (tool ("debug.ui_set_volume_db", "Simulated UI-origin mutation (own undo transaction).", { { "project_id", "string" }, { "track_id", "string" }, { "volume_db", "number" } }, { "project_id", "track_id", "volume_db" }));
        t.add (tool ("debug.ui_burst", "Start an internal thread posting N UI-origin volume changes to the message thread.", { { "project_id", "string" }, { "track_id", "string" }, { "count", "integer" }, { "interval_us", "integer" } }, { "project_id", "track_id", "count" }));
        return makeObject ({ { "tools", t } });
    }

    juce::var checkRevision (Project& p, const juce::var& args)
    {
        if (args.hasProperty ("if_revision") && (juce::int64) args["if_revision"] != p.revision.load())
            return toolError ("REVISION_CONFLICT", "project revision changed", makeObject ({ { "expected", args["if_revision"] }, { "current", (juce::int64) p.revision.load() } }));

        return {};
    }

    juce::var callTool (const juce::String& name, const juce::var& args)
    {
        if (name == "project.list")
        {
            juce::Array<juce::var> a;

            for (auto& p : projects)
                a.add (makeObject ({ { "project_id", p->id }, { "name", p->name }, { "revision", (juce::int64) p->revision.load() },
                                     { "tracks", (int) te::getAudioTracks (*p->edit).size() } }));

            return toolResult (makeObject ({ { "projects", a } }));
        }

        if (name == "events.poll")
        {
            const juce::int64 since = args.hasProperty ("since") ? (juce::int64) args["since"] : 0;
            const int limit = args.hasProperty ("limit") ? (int) args["limit"] : 1000;
            juce::Array<juce::var> a;

            for (auto& e : events)
                if (e.id > since && a.size() < limit)
                {
                    juce::Array<juce::var> ents;

                    for (auto& tid : e.trackIds)
                        ents.add (makeObject ({ { "kind", "track" }, { "id", tid } }));

                    a.add (makeObject ({ { "type", "project.changed" }, { "event_id", e.id }, { "project_id", e.projectId }, { "revision", e.revision },
                                         { "origin", e.origin }, { "tool", e.tool }, { "entities", ents }, { "coalesced_changes", e.coalescedChanges } }));
                }

            return toolResult (makeObject ({ { "events", a }, { "last_event_id", nextEventId - 1 } }));
        }

        if (name == "debug.sleep")
        {
            juce::Thread::sleep ((int) args["ms"]);
            return toolResult (makeObject ({ { "slept_ms", (int) args["ms"] } }));
        }

        auto* p = find (args["project_id"]);

        if (p == nullptr)
            return toolError ("PROJECT_NOT_FOUND", "no such project_id: " + args["project_id"].toString());

        auto& edit = *p->edit;

        if (name == "track.list")
        {
            juce::Array<juce::var> a;

            for (auto* t : te::getAudioTracks (edit))
                a.add (makeObject ({ { "track_id", t->itemID.toString() }, { "name", t->getName() }, { "volume_db", t->getVolumePlugin()->getVolumeDb() } }));

            return toolResult (makeObject ({ { "project_id", p->id }, { "revision", (juce::int64) p->revision.load() }, { "tracks", a } }));
        }

        if (name == "track.add")
        {
            if (auto conflict = checkRevision (*p, args); ! conflict.isVoid())
                return conflict;

            const auto trackName = args.hasProperty ("name") ? args["name"].toString() : juce::String ("Track");
            edit.getUndoManager().beginNewTransaction ("mcp:s1: track.add " + trackName);
            Project::Scope scope (*p, "mcp:s1", "track.add");
            auto track = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

            if (track == nullptr)
                return toolError ("INTERNAL", "insertNewAudioTrack failed");

            track->setName (trackName);

            if (args.hasProperty ("sine_hz"))
            {
                const double hz = args["sine_hz"];
                const auto f = workDir.getChildFile ("sine_" + juce::String ((int) hz) + ".wav");

                if (! f.existsAsFile())
                {
                    std::vector<float> s (48000 * 10);

                    for (size_t i = 0; i < s.size(); ++i)
                        s[i] = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / 48000.0);

                    writeWav (f, { s }, 48000.0);
                }

                te::insertWaveClip (*track, {}, f, { .time = { 0_tp, 10_td } }, te::DeleteExistingClips::no);
            }

            scope.commit();
            return toolResult (makeObject ({ { "project_id", p->id }, { "track_id", track->itemID.toString() }, { "revision", (juce::int64) p->revision.load() },
                                             { "undo_description", edit.getUndoManager().getUndoDescription() },
                                             { "actions_in_transaction", edit.getUndoManager().getNumActionsInCurrentTransaction() } }));
        }

        if (name == "track.set_volume_db")
        {
            if (auto conflict = checkRevision (*p, args); ! conflict.isVoid())
                return conflict;

            auto* track = p->findAudioTrack (args["track_id"].toString());

            if (track == nullptr)
                return toolError ("TRACK_NOT_FOUND", "no such track_id: " + args["track_id"].toString());

            edit.getUndoManager().beginNewTransaction ("mcp:s1: track.set_volume_db " + track->getName());
            Project::Scope scope (*p, "mcp:s1", "track.set_volume_db");
            track->getVolumePlugin()->setVolumeDb ((float) (double) args["volume_db"]);
            scope.commit();
            return toolResult (makeObject ({ { "project_id", p->id }, { "revision", (juce::int64) p->revision.load() },
                                             { "actions_in_transaction", edit.getUndoManager().getNumActionsInCurrentTransaction() } }));
        }

        if (name == "debug.ui_set_volume_db")
        {
            auto* track = p->findAudioTrack (args["track_id"].toString());

            if (track == nullptr)
                return toolError ("TRACK_NOT_FOUND", "no such track_id");

            uiSetVolume (*p, *track, (float) (double) args["volume_db"]);
            return toolResult (makeObject ({ { "revision", (juce::int64) p->revision.load() } }));
        }

        if (name == "debug.ui_burst")
        {
            const auto pid = p->id, tid = args["track_id"].toString();
            const int count = args["count"], interval = args.hasProperty ("interval_us") ? (int) args["interval_us"] : 200;

            if (uiThread.joinable())
                uiThread.join();

            uiThread = std::thread ([this, pid, tid, count, interval]
            {
                for (int i = 0; i < count; ++i)
                {
                    juce::MessageManager::callAsync ([this, pid, tid, i]
                    {
                        if (auto* pr = find (pid))
                            if (auto* tr = pr->findAudioTrack (tid))
                                uiSetVolume (*pr, *tr, -3.0f - (float) (i % 20));
                    });
                    std::this_thread::sleep_for (std::chrono::microseconds (interval));
                }
            });

            return toolResult (makeObject ({ { "started", true }, { "count", count } }));
        }

        if (name == "undo")
        {
            auto& um = edit.getUndoManager();
            const auto before = um.getUndoDescription();
            Project::Scope scope (*p, "mcp:s1", "undo");
            const bool ok = um.undo();
            scope.commit();
            return toolResult (makeObject ({ { "project_id", p->id }, { "undone", ok }, { "undone_description", before },
                                             { "revision", (juce::int64) p->revision.load() } }));
        }

        if (name == "render")
        {
            const double seconds = args.hasProperty ("seconds") ? (double) args["seconds"] : 2.0;
            const auto out = workDir.getChildFile ("render_" + p->id + ".wav");
            float peak = 0, rms = 0;
            const auto t0 = nowMs();
            const auto err = renderBlocking (edit, makeRenderParams (edit, out, 48000.0, seconds), &peak, &rms);
            const auto ms = nowMs() - t0;

            if (err.isNotEmpty())
                return toolError ("RENDER_FAILED", err);

            return toolResult (makeObject ({ { "project_id", p->id }, { "path", out.getFullPathName() }, { "seconds", seconds }, { "render_ms", ms },
                                             { "peak_linear", peak }, { "rms_linear", rms }, { "revision", (juce::int64) p->revision.load() } }));
        }

        return toolError ("UNKNOWN_TOOL", "unknown tool: " + name);
    }

    void uiSetVolume (Project& p, te::AudioTrack& track, float db)
    {
        p.edit->getUndoManager().beginNewTransaction ("ui: volume " + track.getName());
        Project::Scope scope (p, "ui", "ui.set_volume");
        track.getVolumePlugin()->setVolumeDb (db);
        scope.commit();
    }

    /** Returns an invalid var for notifications (no response). */
    juce::var handle (const juce::var& req)
    {
        const auto id = req["id"];
        const bool isNotification = ! req.hasProperty ("id");
        const auto method = req["method"].toString();
        const auto params = req["params"];

        if (method == "initialize")
        {
            const auto requested = params["protocolVersion"].toString();
            return rpcResult (id, makeObject ({ { "protocolVersion", requested.isNotEmpty() ? requested : juce::String (kProtocolVersion) },
                                                { "capabilities", makeObject ({ { "tools", makeObject ({ { "listChanged", false } }) } }) },
                                                { "serverInfo", makeObject ({ { "name", "rovox-spike-p4" }, { "version", "0.0.1" } }) } }));
        }

        if (isNotification)
            return {};                                              // notifications/initialized, notifications/cancelled, ...

        if (method == "ping")        return rpcResult (id, makeObject ({}));
        if (method == "tools/list")  return rpcResult (id, toolsList());

        if (method == "tools/call")
            return rpcResult (id, callTool (params["name"].toString(), params["arguments"]));

        return rpcError (id, -32601, "Method not found: " + method);
    }

    juce::File workDir;
    std::unique_ptr<te::Engine> engine;
    std::vector<std::unique_ptr<Project>> projects;
    std::vector<Event> events;
    juce::int64 nextEventId = 1;
    std::mutex outMutex;
    std::thread uiThread;
};

void Project::emit (const juce::String& origin, const juce::String& tool, int changes, const juce::StringArray& tracks)
{
    Event e;
    e.id = app.nextEventId++;
    e.projectId = id;
    e.origin = origin;
    e.tool = tool;
    e.revision = ++revision;
    e.trackIds = tracks;
    e.coalescedChanges = changes;
    app.events.push_back (e);

    // push notification too (clients that cannot poll)
    app.writeLine (makeObject ({ { "jsonrpc", "2.0" }, { "method", "notifications/rovox/project_changed" },
                                 { "params", makeObject ({ { "event_id", e.id }, { "project_id", id }, { "revision", e.revision }, { "origin", origin }, { "tool", tool } }) } }));
}
}

int runMcp (int argc, char** argv)
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rovox_p4");

    for (int i = 1; i + 1 < argc; i += 2)
        if (juce::String (argv[i]) == "--work")
            dir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i + 1]);

    dir.createDirectory();
    int code = 0;

    {
        App app (dir);
        std::fprintf (stderr, "rovox-spike-p4: MCP on stdio, projects: %s, %s\n", app.projects[0]->id.toRawUTF8(), app.projects[1]->id.toRawUTF8());

        std::thread reader ([&app]
        {
            std::string line;

            while (std::getline (std::cin, line))
            {
                if (line.empty())
                    continue;

                juce::var req;

                if (! juce::JSON::parse (juce::String::fromUTF8 (line.c_str()), req).wasOk() || ! req.isObject())
                {
                    app.writeLine (App::rpcError ({}, -32700, "Parse error"));
                    continue;
                }

                juce::MessageManager::callAsync ([&app, req]
                {
                    const auto resp = app.handle (req);

                    if (! resp.isVoid())
                        app.writeLine (resp);
                });
            }

            juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
        });

        juce::MessageManager::getInstance()->runDispatchLoop();
        reader.join();

        if (app.uiThread.joinable())
            app.uiThread.join();
    }

    return code;
}
