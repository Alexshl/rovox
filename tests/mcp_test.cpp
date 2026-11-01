// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// M2: MCP protocol conformance, tools as adapters over the command layer, two projects at once, one undo step per call,
// events and external_changes, jobs (cancel, idempotency, restart), preview and analysis, HTTP transport.
#include "HttpServer.h"
#include "JobManager.h"
#include "Loudness.h"
#include "McpHttp.h"
#include "McpStdio.h"
#include "RenderJobs.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

using namespace rovox;

namespace
{
/** Everything an MCP session needs, driven from client threads while the test (message) thread pumps the message loop. */
struct Harness
{
    explicit Harness (fs::path jobsDir = {}, mcp::McpConfig cfg = {})
        : jobs (std::move (jobsDir)), service (env.pm, jobs, env.executor), server (env.pm, env.bus, env.hub, jobs, service, std::move (cfg)),
          session (server.createSession())
    {
        initialize();
    }

    ~Harness() { jobs.shutdown(); }

    /** Runs `fn` on a client thread and keeps the message loop alive until it returns. */
    template <typename F>
    auto onClient (F&& fn)
    {
        using R = std::invoke_result_t<F>;
        std::atomic<bool> done { false };
        std::exception_ptr error;
        std::optional<R> result;

        std::thread t ([&]
        {
            try { result = fn(); }
            catch (...) { error = std::current_exception(); }
            done = true;
        });

        while (! done)
            test::pumpMessages (2);

        t.join();

        if (error)
            std::rethrow_exception (error);

        return std::move (*result);
    }

    json rpc (const std::string& method, json params = json::object(), json id = 1, mcp::Session* s = nullptr)
    {
        json req { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move (params) } };
        return onClient ([&] { mcp::RequestContext rc; auto r = server.handle (s != nullptr ? *s : *session, req, rc); return r ? *r : json (nullptr); });
    }

    void initialize()
    {
        auto r = rpc ("initialize", { { "protocolVersion", "2025-06-18" }, { "capabilities", json::object() }, { "clientInfo", { { "name", "test" }, { "version", "1" } } } });
        CHECK (r.contains ("result"));
        mcp::RequestContext rc;
        server.handle (*session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }, rc);
    }

    /** tools/call; returns the structuredContent (ok/data/error/revision/external_changes). */
    json tool (const std::string& name, json args = json::object(), mcp::Session* s = nullptr)
    {
        auto r = rpc ("tools/call", { { "name", name }, { "arguments", std::move (args) } }, 7, s);

        if (! r.contains ("result"))
        {
            std::cerr << "    tools/call " << name << " gave a protocol error: " << r.dump() << "\n";
            return { { "ok", false }, { "protocol_error", r.value ("error", json (nullptr)) } };
        }

        return r["result"]["structuredContent"];
    }

    json data (const std::string& name, json args = json::object())
    {
        auto r = tool (name, std::move (args));

        if (! r.value ("ok", false))
            std::cerr << "    " << name << " failed: " << r.dump() << "\n";

        REQUIRE (r.value ("ok", false));
        return r["data"];
    }

    std::string newProject (const std::string& name = "mcp")
    {
        return data ("project.new", { { "name", name } })["project_id"];
    }

    json waitJob (const std::string& id, double seconds = 60)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double> (seconds);

        for (;;)
        {
            auto j = data ("job.get", { { "job_id", id }, { "wait_s", 2 } });

            if (j["state"] != "queued" && j["state"] != "running")
                return j;

            REQUIRE (std::chrono::steady_clock::now() < deadline);
        }
    }

    test::Env env;
    JobManager jobs;
    JobService service;
    mcp::McpServer server;
    std::shared_ptr<mcp::Session> session;
};

/** A project with `n` mono sine tracks (imported through the command layer). */
std::string sineTracks (test::Env& env, const std::vector<double>& dbfs, double seconds, const std::string& name = "sines")
{
    const auto id = env.newProject (name);
    const auto dir = test::makeTempDir ("mcp-sines");
    json paths = json::array();

    for (size_t i = 0; i < dbfs.size(); ++i)
    {
        const auto f = dir / ("t" + std::to_string (i + 1) + ".wav");
        auto samples = test::sine (440.0 * double (i + 1), dbfs[i], seconds, 44100.0);
        test::writeWav (f, { samples }, 44100.0, 24);
        paths.push_back (f.string());
    }

    auto plan = env.ok (id, "import.plan", { { "paths", paths } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    return id;
}

std::string firstTrackId (const json& trackList)    { return trackList["tracks"][0]["track_id"]; }
} // namespace

//==============================================================================
// protocol conformance

ROVOX_TEST (mcp, initialize_negotiates_version_and_reports_server_info)
{
    Harness h;
    auto s = h.server.createSession();
    auto r = h.rpc ("initialize", { { "protocolVersion", "2025-03-26" }, { "capabilities", json::object() }, { "clientInfo", { { "name", "x" } } } }, "init-1", s.get());
    CHECK (r["id"] == "init-1");
    CHECK (r["result"]["protocolVersion"] == "2025-03-26");
    CHECK (r["result"]["serverInfo"]["name"] == "rovox");
    CHECK (r["result"]["serverInfo"].contains ("source_url"));
    CHECK (r["result"]["serverInfo"].contains ("version"));
    CHECK (r["result"]["capabilities"]["tools"].is_object());
    CHECK (r["result"]["capabilities"]["resources"]["subscribe"] == true);

    auto s2 = h.server.createSession();
    auto r2 = h.rpc ("initialize", { { "protocolVersion", "1999-01-01" } }, 1, s2.get());
    CHECK (r2["result"]["protocolVersion"] == "2025-06-18");       // unknown version: the server answers with its latest
}

ROVOX_TEST (mcp, request_ids_of_any_shape_are_echoed_and_errors_follow_json_rpc)
{
    Harness h;

    for (json id : { json (1001), json ("abc"), json (123456789012LL), json (0), json ("") })
    {
        auto r = h.rpc ("ping", json::object(), id);
        CHECK (r["id"] == id);
        CHECK (r["result"].is_object());
    }

    auto unknown = h.rpc ("no/such/method");
    CHECK (unknown["error"]["code"] == -32601);

    auto badTool = h.rpc ("tools/call", { { "name", "no.such.tool" }, { "arguments", json::object() } });
    CHECK (badTool["error"]["code"] == -32602);

    auto badArgs = h.rpc ("tools/call", { { "name", "project.list" }, { "arguments", json::array() } });
    CHECK (badArgs["error"]["code"] == -32602);

    mcp::RequestContext rc;
    auto nullId = h.onClient ([&] { return h.server.handle (*h.session, { { "jsonrpc", "2.0" }, { "id", nullptr }, { "method", "ping" } }, rc); });
    REQUIRE (nullId.has_value());
    CHECK ((*nullId)["error"]["code"] == -32600);

    auto notification = h.onClient ([&] { return h.server.handle (*h.session, { { "jsonrpc", "2.0" }, { "method", "notifications/whatever" } }, rc); });
    CHECK (! notification.has_value());

    auto notInitialized = h.server.createSession();
    auto early = h.rpc ("tools/list", json::object(), 1, notInitialized.get());
    CHECK (early["error"]["code"] == -32600);
    CHECK (h.rpc ("ping", json::object(), 1, notInitialized.get()).contains ("result"));      // ping is always allowed
}

ROVOX_TEST (mcp, tools_list_has_schemas_unique_names_and_pagination)
{
    Harness h;
    auto r = h.rpc ("tools/list");
    const auto& tools = r["result"]["tools"];
    CHECK (tools.size() >= 40);
    std::set<std::string> names;

    for (auto& t : tools)
    {
        const auto name = t["name"].get<std::string>();
        CHECK (names.insert (name).second);
        CHECK (t["inputSchema"]["type"] == "object");
        CHECK (t["outputSchema"]["type"] == "object");
        CHECK (! t["description"].get<std::string>().empty());

        // every project tool takes project_id; only these may go without
        static const std::set<std::string> global = { "project.list", "project.new", "project.open", "job.get", "job.cancel", "job.list", "events.poll", "upload.status" };

        if (! global.count (name))
            CHECK (t["inputSchema"]["properties"].contains ("project_id"));
    }

    for (auto* must : { "project.list", "project.new", "track.update", "item.split", "marker.upsert", "tempo.set", "fx.add", "fx.update", "undo", "batch", "render.start",
                        "render.preview", "render.preflight", "job.get", "job.cancel", "project.diff", "events.poll", "events.subscribe", "analyze.audio", "import.plan",
                        "transport.get", "transport.set", "mixer.get_meters", "agent.accept" })
        CHECK (names.count (must) == 1);

    // names with underscores are accepted too, for clients that dislike dots
    auto viaUnderscore = h.rpc ("tools/call", { { "name", "project_list" }, { "arguments", json::object() } });
    CHECK (viaUnderscore["result"]["isError"] == false);

    // reserved namespaces answer NOT_IMPLEMENTED, not "unknown tool"
    for (auto* reserved : { "template.list", "notation.import", "recipe.run", "midi.edit" })
    {
        auto rr = h.tool (reserved, json::object());
        CHECK (rr["error"]["code"] == "NOT_IMPLEMENTED");
        CHECK (rr["error"]["details"]["available"].is_array());
    }

    // cursor pagination: pages of 100, a cursor beyond the end is empty
    auto page = h.rpc ("tools/list", { { "cursor", "1000" } });
    CHECK (page["result"]["tools"].empty());
    CHECK (h.rpc ("tools/list", { { "cursor", "garbage" } }).contains ("error"));
}

ROVOX_TEST (mcp, underscore_mode_lists_underscored_names)
{
    mcp::McpConfig cfg;
    cfg.underscoreNames = true;
    Harness h ({}, cfg);
    auto r = h.rpc ("tools/list");
    std::set<std::string> names;

    for (auto& t : r["result"]["tools"])
        names.insert (t["name"].get<std::string>());

    CHECK (names.count ("track_list") == 1);
    CHECK (names.count ("track.list") == 0);
    CHECK (h.tool ("track_list", { { "project_id", "p_x" } })["error"]["code"] == "NOT_FOUND");
}

ROVOX_TEST (mcp, tool_errors_carry_codes_and_validation_paths)
{
    Harness h;
    const auto id = h.newProject();

    auto missing = h.tool ("track.update", { { "project_id", id }, { "track_id", "trk_nope" }, { "volume_db", -3 } });
    CHECK (missing["ok"] == false);
    CHECK (missing["error"]["code"] == "NOT_FOUND");

    auto range = h.tool ("track.create", { { "project_id", id }, { "volume_db", 99 } });
    CHECK (range["error"]["code"] == "INVALID_PARAMS");
    CHECK (range["error"]["details"]["path"] == "/volume_db");

    auto noProject = h.tool ("track.list", { { "project_id", "p_00000000000000000000000000" } });
    CHECK (noProject["error"]["code"] == "NOT_FOUND");

    auto unknownField = h.tool ("track.create", { { "project_id", id }, { "bogus", 1 } });
    CHECK (unknownField["error"]["code"] == "INVALID_PARAMS");

    auto missingId = h.tool ("track.list", json::object());
    CHECK (missingId["error"]["code"] == "INVALID_PARAMS");
}

//==============================================================================
// tools are adapters over commands: undo steps, revisions, batch

ROVOX_TEST (mcp, every_mutating_call_is_one_named_undo_step_with_the_session_as_origin)
{
    Harness h;
    const auto id = h.newProject();
    auto t1 = h.data ("track.create", { { "project_id", id }, { "name", "A" } })["track_id"];
    auto t2 = h.data ("track.create", { { "project_id", id }, { "name", "B" } })["track_id"];
    auto t3 = h.data ("track.create", { { "project_id", id }, { "name", "C" } })["track_id"];

    const auto stepsBefore = h.data ("history.list", { { "project_id", id } })["undo"].size();

    // one call that changes three tracks = one step
    auto upd = h.tool ("track.update", { { "project_id", id }, { "undo_label", "balance" },
                                         { "updates", json::array ({ { { "track_id", t1 }, { "volume_db", -6.0 } }, { { "track_id", t2 }, { "pan", -0.5 } },
                                                                      { { "track_id", t3 }, { "name", "Gamma" }, { "mute", true } } }) } });
    REQUIRE (upd["ok"] == true);
    auto history = h.data ("history.list", { { "project_id", id } });
    CHECK (history["undo"].size() == stepsBefore + 1);
    CHECK (history["undo"].back()["label"] == "test: balance");        // the label is the client's own name (clientInfo), else mcp:<session>
    CHECK (history["undo"].back()["origin"]["kind"] == "mcp");
    CHECK (history["undo"].back()["origin"]["session"] == h.session->shortId);

    auto tracks = h.data ("track.list", { { "project_id", id } })["tracks"];
    CHECK_NEAR (tracks[0]["volume_db"].get<double>(), -6.0, 0.01);
    CHECK_NEAR (tracks[1]["pan"].get<double>(), -0.5, 0.01);
    CHECK (tracks[2]["name"] == "Gamma");
    CHECK (tracks[2]["mute"] == true);

    // one undo restores all three
    auto undone = h.tool ("undo", { { "project_id", id } });
    CHECK (undone["ok"] == true);
    tracks = h.data ("track.list", { { "project_id", id } })["tracks"];
    CHECK_NEAR (tracks[0]["volume_db"].get<double>(), 0.0, 0.01);
    CHECK_NEAR (tracks[1]["pan"].get<double>(), 0.0, 0.01);
    CHECK (tracks[2]["name"] == "C");
    CHECK (tracks[2]["mute"] == false);

    // redo brings them back
    CHECK (h.tool ("redo", { { "project_id", id } })["ok"] == true);
    CHECK_NEAR (h.data ("track.list", { { "project_id", id } })["tracks"][0]["volume_db"].get<double>(), -6.0, 0.01);

    // a failing call changes nothing and adds no step
    const auto before = h.data ("history.list", { { "project_id", id } })["undo"].size();
    auto bad = h.tool ("track.update", { { "project_id", id }, { "updates", json::array ({ { { "track_id", t1 }, { "volume_db", -20.0 } }, { { "track_id", "trk_nope" }, { "volume_db", 0 } } }) } });
    CHECK (bad["ok"] == false);
    CHECK (h.data ("history.list", { { "project_id", id } })["undo"].size() == before);
    CHECK_NEAR (h.data ("track.list", { { "project_id", id } })["tracks"][0]["volume_db"].get<double>(), -6.0, 0.01);
}

ROVOX_TEST (mcp, batch_is_atomic_one_undo_step_and_fx_params_are_undoable)
{
    Harness h;
    const auto id = sineTracks (h.env, { -20.0, -20.0 }, 2.0);
    auto list = h.data ("track.list", { { "project_id", id } });
    const std::string t1 = list["tracks"][0]["track_id"], t2 = list["tracks"][1]["track_id"];
    const auto steps = h.data ("history.list", { { "project_id", id } })["undo"].size();

    auto b = h.tool ("batch", { { "project_id", id }, { "undo_label", "mix pass" },
                                { "ops", json::array ({ { { "tool", "track.update" }, { "arguments", { { "track_id", t1 }, { "volume_db", -4.0 } } } },
                                                        { { "tool", "fx.add" }, { "arguments", { { "track_id", t1 }, { "builtin", "compressor" } } } },
                                                        { { "tool", "marker.upsert" }, { "arguments", { { "markers", json::array ({ { { "name", "Verse" }, { "time", { { "bar", 2 } } } } }) } } } },
                                                        { { "tool", "track.update" }, { "arguments", { { "track_id", t2 }, { "pan", 0.4 } } } } }) } });
    REQUIRE (b["ok"] == true);
    CHECK (b["data"]["results"].size() == 4);
    CHECK (h.data ("history.list", { { "project_id", id } })["undo"].size() == steps + 1);
    CHECK (h.data ("fx.list", { { "project_id", id }, { "target", t1 } })["fx"].size() >= 2);      // compressor + fader
    CHECK (h.data ("marker.list", { { "project_id", id } })["markers"].size() == 1);

    // atomic: a failing op rolls the others back
    const auto markersBefore = h.data ("marker.list", { { "project_id", id } })["markers"].size();
    auto bad = h.tool ("batch", { { "project_id", id }, { "ops", json::array ({ { { "tool", "marker.upsert" }, { "arguments", { { "markers", json::array ({ { { "name", "X" }, { "time_s", 1.0 } } }) } } } },
                                                                              { { "tool", "track.update" }, { "arguments", { { "track_id", "trk_nope" }, { "volume_db", 0 } } } } }) } });
    CHECK (bad["ok"] == false);
    CHECK (h.data ("marker.list", { { "project_id", id } })["markers"].size() == markersBefore);

    // global and manual-undo tools are refused inside a batch
    CHECK (h.tool ("batch", { { "project_id", id }, { "ops", json::array ({ { { "tool", "undo" }, { "arguments", json::object() } } }) } })["error"]["code"] == "INVALID_PARAMS");

    // plugin parameters in their own unit, one undo step
    auto fx = h.data ("fx.list", { { "project_id", id }, { "target", t1 } })["fx"];
    std::string compressor;

    for (auto& p : fx)
        if (p["type"] == "compressor")
            compressor = p["plugin_id"];

    REQUIRE (! compressor.empty());
    auto params = h.data ("fx.params", { { "project_id", id }, { "plugin_id", compressor } })["fx"]["params"];
    REQUIRE (params.size() > 0);
    const auto ident = params[0]["ident"].get<std::string>();
    const double lo = params[0]["min"], hi = params[0]["max"], target = lo + (hi - lo) * 0.25;
    auto up = h.tool ("fx.update", { { "project_id", id }, { "plugin_id", compressor }, { "params", { { ident, target } } } });
    REQUIRE (up["ok"] == true);
    auto after = h.data ("fx.params", { { "project_id", id }, { "plugin_id", compressor } })["fx"]["params"][0];
    CHECK_NEAR (after["value"].get<double>(), target, 1e-3 + std::abs (target) * 1e-3);
    CHECK (h.tool ("fx.update", { { "project_id", id }, { "plugin_id", compressor }, { "params", { { ident, hi + 100.0 } } } })["error"]["code"] == "INVALID_PARAMS");
    CHECK (h.tool ("fx.update", { { "project_id", id }, { "plugin_id", compressor }, { "params", { { "no_such_param", 1.0 } } } })["error"]["code"] == "NOT_FOUND");
}

ROVOX_TEST (mcp, if_revision_conflicts_and_master_track_can_be_set)
{
    Harness h;
    const auto id = h.newProject();
    auto first = h.tool ("track.create", { { "project_id", id }, { "name", "A" } });
    const auto rev = first["revision"].get<std::uint64_t>();

    auto stale = h.tool ("track.create", { { "project_id", id }, { "name", "B" }, { "if_revision", rev - 1 } });
    CHECK (stale["ok"] == false);
    CHECK (stale["error"]["code"] == "REVISION_CONFLICT");
    CHECK (stale["error"]["details"]["current_revision"] == rev);

    auto fresh = h.tool ("track.create", { { "project_id", id }, { "name", "B" }, { "if_revision", rev } });
    CHECK (fresh["ok"] == true);
    CHECK (fresh["revision"].get<std::uint64_t>() == rev + 1);

    auto dry = h.tool ("track.create", { { "project_id", id }, { "name", "C" }, { "dry_run", true } });
    CHECK (dry["data"]["dry_run"] == true);
    CHECK (h.data ("track.list", { { "project_id", id } })["tracks"].size() == 2);

    auto master = h.tool ("track.update", { { "project_id", id }, { "track_id", "master" }, { "volume_db", -1.5 } });
    CHECK (master["ok"] == true);
    CHECK_NEAR (h.data ("track.list", { { "project_id", id } })["master"]["volume_db"].get<double>(), -1.5, 0.01);
    CHECK (h.tool ("track.update", { { "project_id", id }, { "track_id", "master" }, { "name", "x" } })["error"]["code"] == "INVALID_PARAMS");
}

ROVOX_TEST (mcp, items_split_keeps_ids_and_tempo_set_changes_the_tempo)
{
    Harness h;
    const auto id = sineTracks (h.env, { -20.0 }, 8.0);
    auto items = h.data ("item.list", { { "project_id", id } })["items"];
    REQUIRE (items.size() == 1);
    const std::string item = items[0]["item_id"];

    auto split = h.tool ("item.split", { { "project_id", id }, { "item_id", item }, { "at", { { "s", 3.0 } } } });
    REQUIRE (split["ok"] == true);
    const std::string left = split["data"]["left_id"], right = split["data"]["right_id"];
    CHECK (left == item);
    CHECK (right != item);

    // update by the old (left) id after the split, and the right part is a separate item
    auto upd = h.tool ("item.update", { { "project_id", id }, { "item_id", left }, { "name", "intro" } });
    CHECK (upd["ok"] == true);
    items = h.data ("item.list", { { "project_id", id } })["items"];
    CHECK (items.size() == 2);
    CHECK_NEAR (h.data ("item.list", { { "project_id", id } })["items"][0]["length_s"].get<double>() + h.data ("item.list", { { "project_id", id } })["items"][1]["length_s"].get<double>(), 8.0, 0.01);

    // mute is part of the undo step too (Tracktion writes it without an UndoManager; items and tracks are fixed up)
    REQUIRE (h.tool ("item.update", { { "project_id", id }, { "item_id", left }, { "mute", true } })["ok"] == true);
    CHECK (h.data ("item.list", { { "project_id", id } })["items"][0]["muted"] == true);
    REQUIRE (h.tool ("undo", { { "project_id", id } })["ok"] == true);
    CHECK (h.data ("item.list", { { "project_id", id } })["items"][0]["muted"] == false);

    // a split outside the item is refused
    CHECK (h.tool ("item.split", { { "project_id", id }, { "item_id", left }, { "at", { { "s", 7.5 } } } })["error"]["code"] == "INVALID_PARAMS");
    CHECK (h.tool ("item.delete", { { "project_id", id }, { "item_id", right } })["ok"] == true);

    // SetTempo is not a no-op: bar 3 moves with the tempo
    auto bar3at120 = h.data ("time.convert", { { "project_id", id }, { "time", { { "bar", 3 } } } })["s"].get<double>();
    CHECK_NEAR (bar3at120, 4.0, 0.01);
    REQUIRE (h.tool ("tempo.set", { { "project_id", id }, { "points", json::array ({ { { "bpm", 60.0 } } }) } })["ok"] == true);
    CHECK_NEAR (h.data ("time.convert", { { "project_id", id }, { "time", { { "bar", 3 } } } })["s"].get<double>(), 8.0, 0.01);
    CHECK_NEAR (h.data ("tempo.get", { { "project_id", id } })["points"][0]["bpm"].get<double>(), 60.0, 1e-6);
    CHECK (h.tool ("undo", { { "project_id", id } })["ok"] == true);
    CHECK_NEAR (h.data ("tempo.get", { { "project_id", id } })["points"][0]["bpm"].get<double>(), 120.0, 1e-6);
}

//==============================================================================
// two projects at once

ROVOX_TEST (mcp, two_projects_are_independent_under_concurrent_calls)
{
    Harness h;
    const auto a = h.newProject ("A"), b = h.newProject ("B");
    auto other = h.server.createSession();
    h.rpc ("initialize", { { "protocolVersion", "2025-06-18" } }, 1, other.get());

    std::atomic<int> failures { 0 };
    auto worker = [&] (mcp::Session* s, const std::string& project, int tracks)
    {
        for (int i = 0; i < tracks; ++i)
        {
            json req { { "jsonrpc", "2.0" }, { "id", i }, { "method", "tools/call" },
                       { "params", { { "name", "track.create" }, { "arguments", { { "project_id", project }, { "name", "T" + std::to_string (i) } } } } } };
            mcp::RequestContext rc;
            auto r = h.server.handle (*s, req, rc);

            if (! r || (*r)["result"]["isError"] != false)
                ++failures;
        }
    };

    std::atomic<int> finished { 0 };
    std::thread t1 ([&] { worker (h.session.get(), a, 25); ++finished; });
    std::thread t2 ([&] { worker (other.get(), b, 40); ++finished; });

    while (finished < 2)
        test::pumpMessages (2);

    t1.join();
    t2.join();
    CHECK (failures == 0);
    CHECK (h.data ("track.list", { { "project_id", a } })["tracks"].size() == 25);
    CHECK (h.data ("track.list", { { "project_id", b } })["tracks"].size() == 40);
    const auto revA = h.env.project (a).revision(), revB = h.env.project (b).revision();

    // undo in A leaves B untouched
    REQUIRE (h.tool ("undo", { { "project_id", a } })["ok"] == true);
    CHECK (h.data ("track.list", { { "project_id", a } })["tracks"].size() == 24);
    CHECK (h.data ("track.list", { { "project_id", b } })["tracks"].size() == 40);
    CHECK (h.env.project (b).revision() == revB);
    CHECK (h.env.project (a).revision() > revA);

    // origins differ per session
    auto hist = h.data ("history.list", { { "project_id", b } })["undo"];
    CHECK (hist.back()["origin"]["session"] == other->shortId);
    CHECK (hist.back()["origin"]["session"] != h.session->shortId);

    // project list shows both
    CHECK (h.data ("project.list").at ("projects").size() >= 2);
}

//==============================================================================
// events and external changes

ROVOX_TEST (mcp, external_changes_and_diff_report_what_others_did)
{
    Harness h;
    const auto id = sineTracks (h.env, { -20.0, -20.0 }, 1.0);
    auto tracks = h.data ("track.list", { { "project_id", id } });
    const auto baseline = tracks["revision"].get<std::uint64_t>();
    const std::string t1 = firstTrackId (tracks);

    // the "UI" changes a fader and moves a marker
    const Origin ui { Origin::Kind::ui, "ws_1", "alex@browser" };
    REQUIRE (h.env.runAs (ui, id, "track.update", { { "track_id", t1 }, { "volume_db", -4.0 } }).ok);
    auto mk = h.env.runAs (ui, id, "marker.create", { { "time_s", 2.0 }, { "name", "Chorus" } });
    REQUIRE (mk.ok);
    REQUIRE (h.env.runAs (ui, id, "marker.update", { { "marker_id", mk.data["marker_id"] }, { "time_s", 3.0 } }).ok);

    // any tool of the agent now reports them, with human labels
    auto r = h.tool ("marker.list", { { "project_id", id } });
    REQUIRE (r.contains ("external_changes"));
    CHECK (r["external_changes"]["count"] == 3);
    CHECK (r["external_changes"]["since_revision"] == baseline);
    CHECK (r["external_changes"]["summary"][0].get<std::string>().find ("alex@browser") != std::string::npos);

    // once reported, not again; the agent's own changes are not echoed
    auto again = h.tool ("track.update", { { "project_id", id }, { "track_id", t1 }, { "pan", 0.1 } });
    CHECK (! again.contains ("external_changes"));

    // diff
    auto diff = h.data ("project.diff", { { "project_id", id }, { "since_revision", baseline } });
    CHECK (diff["changes"].size() == 4);                       // three by the UI, one by the agent
    CHECK (diff["changes"][0]["origin"]["kind"] == "ui");
    CHECK (! diff["changes"][0]["entities"].empty());       // a fader lives on the track's volume plugin: the entity is a plugin or the track
    auto none = h.data ("project.diff", { { "project_id", id }, { "since_revision", h.env.project (id).revision() } });
    CHECK (none["changes"].empty());

    // the change buffer holds 1000 events: after more than that, an old revision needs a resync
    for (int i = 0; i < 1010; ++i)
        REQUIRE (h.env.run (id, "track.update", { { "track_id", t1 }, { "pan", (i % 2) * 0.1 } }).ok);

    auto stale = h.tool ("project.diff", { { "project_id", id }, { "since_revision", baseline } });
    CHECK (stale["error"]["code"] == "RESYNC_REQUIRED");
    CHECK (stale["error"]["details"]["current_revision"].is_number());
}

ROVOX_TEST (mcp, events_poll_and_subscriptions_deliver_notifications)
{
    Harness h;
    const auto id = h.newProject();

    auto empty = h.data ("events.poll", { { "project_id", id }, { "since_event_id", 0 } });
    const auto cursor = empty["next_event_id"].get<std::uint64_t>();

    REQUIRE (h.tool ("events.subscribe", { { "project_id", id } })["ok"] == true);
    auto s = h.session;
    std::uint64_t notifyCursor = s->lastMessageId();
    h.rpc ("resources/subscribe", { { "uri", "rovox://project/" + id + "/summary" } });

    // a change made by someone else
    const Origin ui { Origin::Kind::ui, "ws_1", "alex@browser" };
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE (h.env.runAs (ui, id, "track.create", { { "name", "Guitar" } }).ok);

    auto poll = h.data ("events.poll", { { "project_id", id }, { "since_event_id", cursor }, { "wait_s", 5 } });
    REQUIRE (poll["events"].size() >= 1);
    CHECK (poll["events"][0]["type"] == "project.changed");
    CHECK (poll["events"][0]["origin"]["kind"] == "ui");
    bool trackAdded = false;

    for (auto& c : poll["events"][0]["changes"])
        trackAdded = trackAdded || (c["kind"] == "track" && c["op"] == "add");

    CHECK (trackAdded);

    // push: custom notification at once, resources/updated after the 250 ms debounce (within 500 ms)
    bool sawChanged = false, sawUpdated = false;
    double updatedAfterMs = 0;

    h.onClient ([&]
    {
        json m;

        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds (3) && ! (sawChanged && sawUpdated))
        {
            if (! s->next (notifyCursor, m, std::chrono::milliseconds (50)))
                continue;

            sawChanged = sawChanged || m["method"] == "notifications/rovox/project_changed";

            if (m["method"] == "notifications/resources/updated" && ! sawUpdated)
            {
                sawUpdated = true;
                updatedAfterMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
            }
        }

        return 0;
    });

    CHECK (sawChanged);
    CHECK (sawUpdated);
    CHECK (updatedAfterMs <= 500.0);
    std::cerr << "    resources/updated arrived " << updatedAfterMs << " ms after the change\n";

    // a transport position change is not an event: nothing published while idle
    const auto quiet = s->lastMessageId();
    test::pumpMessages (400);
    CHECK (s->lastMessageId() == quiet);

    // resources
    auto summary = h.rpc ("resources/read", { { "uri", "rovox://project/" + id + "/summary" } });
    REQUIRE (summary.contains ("result"));
    auto body = json::parse (summary["result"]["contents"][0]["text"].get<std::string>());
    CHECK (body["tracks"].size() == 1);
    auto projects = h.rpc ("resources/read", { { "uri", "rovox://projects" } });
    CHECK (projects["result"]["contents"][0]["mimeType"] == "application/json");
    CHECK (h.rpc ("resources/read", { { "uri", "rovox://project/p_none/summary" } }).contains ("error"));
    CHECK (h.rpc ("resources/list")["result"]["resources"].size() >= 3);
}

//==============================================================================
// jobs

ROVOX_TEST (mcp, job_manager_idempotency_restart_and_cancel_of_queued_jobs)
{
    const auto dir = test::makeTempDir ("jobs");
    std::string runningId, queuedId, doneId;

    {
        JobManager jm (dir, { .render = 1 });
        std::atomic<bool> release { false };
        auto a = jm.submit ({ .kind = "render", .projectId = "p_1" }, [&] (JobContext& jc) -> Result<json>
        {
            while (! release && ! jc.cancelled())
                std::this_thread::sleep_for (std::chrono::milliseconds (5));

            return jc.cancelled() ? Result<json> (makeError (errc::cancelled, "stop")) : Result<json> (json { { "ok", 1 } });
        });
        runningId = a.id;

        auto q = jm.submit ({ .kind = "render", .projectId = "p_1", .clientRequestId = "req-1" }, [] (JobContext&) -> Result<json> { return json { { "never", true } }; });
        queuedId = q.id;
        CHECK (q.created);

        // the same client_request_id returns the same job
        auto again = jm.submit ({ .kind = "render", .projectId = "p_1", .clientRequestId = "req-1" }, [] (JobContext&) -> Result<json> { return json::object(); });
        CHECK (! again.created);
        CHECK (again.id == queuedId);
        CHECK (jm.get (queuedId)->state == JobState::queued);

        // cancelling a queued job is immediate
        CHECK (jm.cancel (queuedId));
        CHECK (jm.get (queuedId)->state == JobState::cancelled);
        CHECK (! jm.cancel (queuedId));

        // priority: preview (10) runs before render (0) when both wait
        std::vector<std::string> order;
        std::mutex om;
        auto r1 = jm.submit ({ .kind = "render", .projectId = "p_1" }, [&] (JobContext&) -> Result<json> { std::lock_guard<std::mutex> l (om); order.push_back ("render"); return json::object(); });
        auto pv = jm.submit ({ .kind = "preview", .projectId = "p_1", .priority = 10 }, [&] (JobContext&) -> Result<json> { std::lock_guard<std::mutex> l (om); order.push_back ("preview"); return json::object(); });
        release = true;
        jm.get (r1.id, std::chrono::seconds (10));
        jm.get (pv.id, std::chrono::seconds (10));
        auto fin = jm.get (runningId, std::chrono::seconds (10));
        CHECK (fin->state == JobState::succeeded);
        doneId = runningId;
        REQUIRE (order.size() == 2);
        CHECK (order[0] == "preview");

        // a job that never finishes before the "crash"
        std::atomic<bool> hold { true };
        jm.submit ({ .kind = "render", .projectId = "p_2" }, [&] (JobContext&) -> Result<json>
        {
            while (hold)
                std::this_thread::sleep_for (std::chrono::milliseconds (5));

            return json::object();
        });
        std::this_thread::sleep_for (std::chrono::milliseconds (100));

        // simulate the crash: copy the on-disk records while it is still running, then let the manager go
        const auto crash = test::makeTempDir ("jobs-crash");
        for (auto& f : fs::directory_iterator (dir))
            fs::copy_file (f.path(), crash / f.path().filename());

        hold = false;
        jm.shutdown();

        JobManager restarted (crash);
        int restartedFailures = 0;

        for (auto& j : restarted.list())
        {
            if (j.error && j.error->code == "SERVER_RESTARTED")
                ++restartedFailures;

            CHECK (isFinished (j.state));
        }

        CHECK (restartedFailures == 1);
        CHECK (restarted.get (doneId)->state == JobState::succeeded);
    }
}

ROVOX_TEST (mcp, render_start_with_wait_returns_the_result_in_one_call_and_normalizes_to_the_target)
{
    Harness h;
    const auto id = sineTracks (h.env, { -23.0 }, 10.0);

    auto r = h.tool ("render.start", { { "project_id", id }, { "name", "mix" }, { "wait_s", 60 }, { "target_lufs", -16.0 }, { "tail_s", 0 }, { "format", { { "container", "wav" }, { "bit_depth", 24 } } } });
    REQUIRE (r["ok"] == true);
    CHECK (r["data"]["state"] == "succeeded");
    const auto& res = r["data"]["result"];
    CHECK_NEAR (res["stats"]["integrated_lufs"].get<double>(), -16.0, 0.5);
    CHECK (res["loudness"]["mode"] == "normalize");
    CHECK (res["loudness"]["before_lufs"].get<double>() < res["loudness"]["after_lufs"].get<double>());
    CHECK (res["file"]["duration_s"].get<double>() > 9.9);
    CHECK (fs::exists (res["file"]["abs_path"].get<std::string>()));
    CHECK (r["data"]["progress"] == 1.0);

    // without wait_s the call returns a job at once
    auto started = h.tool ("render.start", { { "project_id", id }, { "name", "second" } });
    REQUIRE (started["ok"] == true);
    const std::string jobId = started["data"]["job_id"];
    auto done = h.waitJob (jobId);
    CHECK (done["state"] == "succeeded");
    CHECK (h.data ("job.list", { { "project_id", id }, { "state", "succeeded" } })["jobs"].size() >= 2);
    CHECK (h.rpc ("resources/read", { { "uri", "rovox://jobs/" + jobId } }).contains ("result"));

    // idempotency
    auto a = h.data ("render.start", { { "project_id", id }, { "name", "idem" }, { "client_request_id", "abc" } });
    auto b = h.data ("render.start", { { "project_id", id }, { "name", "idem" }, { "client_request_id", "abc" } });
    CHECK (a["job_id"] == b["job_id"]);
    CHECK (b["created"] == false);
    h.waitJob (a["job_id"]);
}

ROVOX_TEST (mcp, render_check_fails_and_without_check_warns)
{
    Harness h;
    const auto id = sineTracks (h.env, { 0.0 }, 3.0);       // full-scale sine, then +6 dB on the track: clips
    auto list = h.data ("track.list", { { "project_id", id } });
    REQUIRE (h.tool ("track.update", { { "project_id", id }, { "track_id", firstTrackId (list) }, { "volume_db", 6.0 } })["ok"] == true);
    REQUIRE (h.tool ("track.update", { { "project_id", id }, { "track_id", "master" }, { "volume_db", 6.0 } })["ok"] == true);

    auto warn = h.tool ("render.start", { { "project_id", id }, { "name", "w" }, { "wait_s", 60 }, { "tail_s", 0 } });
    CHECK (warn["ok"] == true);
    CHECK (warn["data"]["result"]["preflight"]["warnings"].size() >= 1);

    auto strict = h.tool ("render.start", { { "project_id", id }, { "name", "s" }, { "wait_s", 60 }, { "tail_s", 0 }, { "check", true } });
    CHECK (strict["ok"] == false);
    CHECK (strict["error"]["code"] == "PREFLIGHT_FAILED");
    CHECK (strict["error"]["details"]["job_id"].is_string());
    CHECK (! fs::exists (h.env.project (id).rendersDir() / "s.wav"));

    auto pre = h.tool ("render.preflight", { { "project_id", id } });
    CHECK (pre["ok"] == true);
    CHECK (pre["data"]["state"] == "succeeded");
}

ROVOX_TEST (mcp, job_cancel_stops_a_render_quickly_and_leaves_no_file)
{
    Harness h;
    // long material so that the render is still running when the cancel arrives
    const auto id = h.newProject ("long");
    const auto dir = test::makeTempDir ("long");
    test::writeWav (dir / "long.wav", { test::sine (330.0, -20.0, 1500.0, 8000.0) }, 8000.0, 16);
    auto plan = h.data ("import.plan", { { "project_id", id }, { "paths", { (dir / "long.wav").string() } } });
    h.data ("import.apply", { { "project_id", id }, { "plan_id", plan["result"]["plan_id"] } });

    // a previous render of the same name must stay as it is
    const auto target = h.env.project (id).rendersDir() / "long.wav";
    fs::create_directories (target.parent_path());
    { std::ofstream (target, std::ios::binary) << "previous render"; }

    auto started = h.data ("render.start", { { "project_id", id }, { "name", "long" } });
    const std::string job = started["job_id"];

    // wait until it is really rendering
    for (int i = 0; i < 400; ++i)
    {
        auto j = h.data ("job.get", { { "job_id", job } });

        if (j["state"] == "running" && j["progress"].get<double>() > 0.06)
            break;

        test::pumpMessages (10);
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto cancelled = h.data ("job.cancel", { { "job_id", job } });
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::cerr << "    cancel took " << seconds << " s, state " << cancelled["state"] << "\n";
    CHECK (cancelled["state"] == "cancelled");
    CHECK (seconds <= 1.0);

    std::string previous;
    { std::ifstream in (target); std::getline (in, previous); }
    CHECK (previous == "previous render");

    int strays = 0;

    for (auto& f : fs::directory_iterator (h.env.project (id).rendersDir()))
        if (f.path().filename().string()[0] == '.')
            ++strays;

    CHECK (strays == 0);
    CHECK (h.data ("job.get", { { "job_id", job } })["error"]["code"] == "CANCELLED");

    // the project is usable afterwards and the active-job counter is back to zero
    test::pumpMessages (200);
    CHECK (h.env.project (id).activeJobs().load() == 0);
}

ROVOX_TEST (mcp, long_work_does_not_block_other_calls)
{
    Harness h;
    const auto id = sineTracks (h.env, { -20.0, -20.0, -20.0 }, 120.0, "slow");
    const auto other = h.newProject ("other");

    auto started = h.data ("render.start", { { "project_id", id }, { "name", "slow" }, { "format", { { "container", "wav" }, { "bit_depth", 24 } } } });
    const std::string job = started["job_id"];

    // while it renders, quick calls on the other project answer in milliseconds
    double worst = 0;
    int calls = 0;

    for (int i = 0; i < 60; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = h.tool ("track.create", { { "project_id", other }, { "name", "x" + std::to_string (i) } });
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        worst = std::max (worst, ms);
        ++calls;
        CHECK (r["ok"] == true);
        auto state = h.data ("job.get", { { "job_id", job } })["state"];

        if (state != "running" && state != "queued")
            break;
    }

    std::cerr << "    " << calls << " quick calls during the render, worst " << worst << " ms\n";
    CHECK (worst < 500.0);
    h.tool ("job.cancel", { { "job_id", job } });
    h.waitJob (job);
}

ROVOX_TEST (mcp, preview_gives_a_clip_and_analysis_and_keeps_at_most_ten_files)
{
    Harness h;
    const auto id = sineTracks (h.env, { -20.0, -60.0, -60.0 }, 20.0);

    auto r = h.tool ("render.preview", { { "project_id", id }, { "range", { { "start", { { "bar", 1 } } }, { "end", { { "bar", 9 } } } } }, { "format", "mp3" },
                                         { "analysis", json::array ({ "loudness", "spectrum_bands", "per_bar", "tracks_presence" }) }, { "wait_s", 60 } });
    REQUIRE (r["ok"] == true);
    REQUIRE (r["data"]["state"] == "succeeded");
    const auto& res = r["data"]["result"];
    CHECK_NEAR (res["clip"]["duration_s"].get<double>(), 16.0, 0.05);       // 8 bars at 120 BPM in 4/4
    CHECK (res["clip"]["container"] == "mp3");
    CHECK (fs::exists (res["clip"]["abs_path"].get<std::string>()));
    CHECK (res["clip"]["url"].get<std::string>().find ("/previews/pv_") != std::string::npos);
    CHECK (res["loudness"]["integrated_lufs"].get<double>() > -40.0);
    CHECK (res["spectrum_bands_db"].contains ("sub_20_60"));
    CHECK (res["spectrum_bands_db"].contains ("high_6k_20k"));
    CHECK (res["per_bar"].size() == 8);

    // the loud sine is at 440 Hz (low_60_250 .. lowmid_250_2k): the lowmid band dominates
    const double lowMid = res["spectrum_bands_db"]["lowmid_250_2k"], high = res["spectrum_bands_db"]["high_6k_20k"];
    CHECK (lowMid > high + 20.0);

    // per-bar loudness of steady material is steady
    double lo = 1e9, hi = -1e9;

    for (auto& b : res["per_bar"])
    {
        lo = std::min (lo, b["lufs"].get<double>());
        hi = std::max (hi, b["lufs"].get<double>());
    }

    CHECK (hi - lo < 0.5);

    // presence: one loud track out of three
    REQUIRE (res["tracks"].size() == 3);
    CHECK (res["tracks"][0]["share_pct"].get<double>() > 98.0);
    CHECK (res["tracks"][1]["share_pct"].get<double>() < 1.0);
    CHECK (res["tracks"][2]["share_pct"].get<double>() < 1.0);

    // the clip is readable as an MCP resource
    auto read = h.rpc ("resources/read", { { "uri", res["clip"]["uri"] } });
    CHECK (read.contains ("result"));
    auto current = h.rpc ("resources/read", { { "uri", "rovox://project/" + id + "/previews/current" } });
    CHECK (current.contains ("result"));

    // embed_audio adds audio content
    auto embedded = h.rpc ("tools/call", { { "name", "render.preview" }, { "arguments", { { "project_id", id }, { "range", { { "start", { { "s", 0 } } }, { "end", { { "s", 2 } } } } },
                                                                                          { "embed_audio", true }, { "analysis", json::array ({ "loudness" }) } } } });
    bool hasAudio = false;

    for (auto& c : embedded["result"]["content"])
        hasAudio = hasAudio || c["type"] == "audio";

    CHECK (hasAudio);

    // 20 previews in a row leave at most 10 files
    for (int i = 0; i < 20; ++i)
        REQUIRE (h.tool ("render.preview", { { "project_id", id }, { "range", { { "start", { { "s", 0 } } }, { "end", { { "s", 1 } } } } }, { "analysis", json::array ({ "loudness" }) }, { "wait_s", 30 } })["ok"] == true);

    int files = 0;

    for (auto& f : fs::directory_iterator (h.env.project (id).dir() / "previews"))
        if (f.path().filename().string().rfind ("pv_", 0) == 0 && f.path().extension() != ".json")
            ++files;

    CHECK (files <= 10);
    CHECK (files >= 1);
    CHECK (fs::is_empty (h.env.project (id).rendersDir()) || ! fs::exists (h.env.project (id).rendersDir()));       // previews never touch renders/

    // a range longer than 300 s is refused
    CHECK (h.tool ("render.preview", { { "project_id", id }, { "range", { { "start", { { "s", 0 } } }, { "end", { { "s", 400 } } } } } })["error"]["code"] == "INVALID_PARAMS");
}

ROVOX_TEST (mcp, analyze_audio_on_a_track_and_import_plan_as_a_job)
{
    Harness h;
    const auto id = sineTracks (h.env, { -20.0, -40.0 }, 4.0);
    auto list = h.data ("track.list", { { "project_id", id } });
    const std::string loud = list["tracks"][0]["track_id"], quiet = list["tracks"][1]["track_id"];

    auto a = h.data ("analyze.audio", { { "project_id", id }, { "target", loud }, { "kinds", json::array ({ "loudness", "spectrum_bands" }) } });
    auto b = h.data ("analyze.audio", { { "project_id", id }, { "target", quiet }, { "kinds", json::array ({ "loudness" }) } });
    REQUIRE (a["state"] == "succeeded");
    REQUIRE (b["state"] == "succeeded");
    CHECK (a["result"]["loudness"]["integrated_lufs"].get<double>() > b["result"]["loudness"]["integrated_lufs"].get<double>() + 15.0);
    CHECK (a["result"].contains ("spectrum_bands_db"));
    CHECK (! a["result"].contains ("clip"));
    CHECK (! fs::exists (h.env.project (id).dir() / "previews") || fs::is_empty (h.env.project (id).dir() / "previews"));

    // import.plan is a job too; the plan is stored in the project so import.apply works with its id
    const auto empty = h.newProject ("zprj");
    const auto folder = test::makeR20Folder (test::makeTempDir ("r20"), "ZOOM0001.zprj", 4, 1.0);
    auto planJob = h.data ("import.plan", { { "project_id", empty }, { "paths", { folder.string() } } });
    REQUIRE (planJob["state"] == "succeeded");
    CHECK (planJob["result"]["tracks"].size() == 4);
    auto applied = h.tool ("import.apply", { { "project_id", empty }, { "plan_id", planJob["result"]["plan_id"] }, { "rename", { { "REC01", "Guitar" } } } });
    REQUIRE (applied["ok"] == true);
    auto names = h.data ("track.list", { { "project_id", empty } })["tracks"];
    CHECK (names.size() == 4);
    CHECK (names[0]["name"] == "Guitar");
}

//==============================================================================
// stdio framing

ROVOX_TEST (mcp, stdio_transport_frames_lines_and_never_writes_anything_else)
{
    Harness h;
    std::stringstream in, out;
    in << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t"}}})" << "\n"
       << R"({"jsonrpc":"2.0","method":"notifications/initialized"})" << "\n"
       << "\n"
       << "this is not json\n"
       << R"([{"jsonrpc":"2.0","id":5,"method":"ping"}])" << "\n"
       << R"({"jsonrpc":"2.0","id":1001,"method":"ping"})" << "\n"
       << R"({"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"project.list","arguments":{}}})" << "\n"
       << R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"job.get","arguments":{"job_id":"job_none","wait_s":1}}})" << "\n";

    h.onClient ([&] { mcp::StdioTransport (h.server, in, out).run(); return 0; });

    std::vector<json> lines;
    std::string line;

    while (std::getline (out, line))
        lines.push_back (json::parse (line));        // every line must be JSON: a parse error here fails the test

    std::set<std::string> ids;
    bool sawParseError = false, sawBatchError = false;

    for (auto& l : lines)
    {
        CHECK (l["jsonrpc"] == "2.0");

        if (l.contains ("error") && l["error"]["code"] == -32700)
        {
            sawParseError = true;
            CHECK (l["id"].is_null());
        }
        else if (l.contains ("error") && l["error"]["code"] == -32600)
        {
            sawBatchError = true;
        }
        else
        {
            ids.insert (l["id"].dump());
        }
    }

    CHECK (sawParseError);
    CHECK (sawBatchError);
    CHECK (ids.count ("1") == 1);
    CHECK (ids.count ("1001") == 1);
    CHECK (ids.count ("\"abc\"") == 1);
    CHECK (ids.count ("3") == 1);
}

//==============================================================================
// HTTP transport

namespace
{
struct HttpFixture
{
    HttpFixture (const std::string& token = {}, const std::string& bind = "127.0.0.1")
        : h(), server (test::host(), h.env.pm, h.env.bus, h.env.hub, [&] { ServerOptions o; o.port = 0; o.bind = bind; o.token = token; return o; }())
    {
        server.mount ([this] (httplib::Server& svr) { mcp::installMcpRoutes (svr, h.server, { .allowedOrigins = { "https://rovox.example" }, .loopbackBind = true }); });
        REQUIRE (server.start().ok());
    }

    ~HttpFixture() { h.server.shutdown(); server.stop(); }

    std::string bearer;

    httplib::Headers headers (std::initializer_list<std::pair<std::string, std::string>> extra = {}) const
    {
        httplib::Headers hs { { "Accept", "application/json, text/event-stream" }, { "MCP-Protocol-Version", "2025-06-18" } };

        if (! bearer.empty())
            hs.emplace ("Authorization", "Bearer " + bearer);

        for (auto& [k, v] : extra)
        {
            hs.erase (k);
            hs.emplace (k, v);
        }

        return hs;
    }

    Harness h;
    HttpServer server;
};

struct HttpReply { int status = 0; json body; std::string session; std::string raw; };

HttpReply post (httplib::Client& c, const httplib::Headers& hs, const json& body)
{
    auto res = c.Post ("/mcp", hs, body.dump(), "application/json");
    HttpReply r;

    if (! res)
        return r;

    r.status = res->status;
    r.session = res->get_header_value ("Mcp-Session-Id");
    r.raw = res->body;
    r.body = json::parse (res->body, nullptr, false);
    return r;
}

json initBody()
{
    return { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" }, { "params", { { "protocolVersion", "2025-06-18" }, { "capabilities", json::object() }, { "clientInfo", { { "name", "http-test" } } } } } };
}
} // namespace

ROVOX_TEST (mcp, http_session_lifecycle_tools_and_delete)
{
    HttpFixture f;
    auto reply = f.h.onClient ([&]
    {
        httplib::Client c ("127.0.0.1", f.server.port());
        std::vector<HttpReply> out;
        out.push_back (post (c, f.headers(), initBody()));
        const auto sid = out[0].session;
        out.push_back (post (c, f.headers ({ { "Mcp-Session-Id", sid } }), { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }));
        out.push_back (post (c, f.headers ({ { "Mcp-Session-Id", sid } }), { { "jsonrpc", "2.0" }, { "id", 1001 }, { "method", "tools/call" }, { "params", { { "name", "project.list" }, { "arguments", json::object() } } } }));
        out.push_back (post (c, f.headers ({ { "Mcp-Session-Id", sid } }), { { "jsonrpc", "2.0" }, { "id", "abc" }, { "method", "tools/list" } }));
        out.push_back (post (c, f.headers(), { { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" } }));                                   // no session header
        out.push_back (post (c, f.headers ({ { "Mcp-Session-Id", "deadbeef" } }), { { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" } })); // unknown session
        auto bad = c.Post ("/mcp", f.headers ({ { "Mcp-Session-Id", sid } }), "{not json", "application/json");
        HttpReply b;
        b.status = bad ? bad->status : 0;
        b.body = bad ? json::parse (bad->body, nullptr, false) : json();
        out.push_back (b);
        auto del = c.Delete ("/mcp", f.headers ({ { "Mcp-Session-Id", sid } }));
        HttpReply d;
        d.status = del ? del->status : 0;
        out.push_back (d);
        out.push_back (post (c, f.headers ({ { "Mcp-Session-Id", sid } }), { { "jsonrpc", "2.0" }, { "id", 3 }, { "method", "ping" } }));             // after DELETE
        return out;
    });

    REQUIRE (reply.size() == 9);
    CHECK (reply[0].status == 200);
    CHECK (! reply[0].session.empty());
    CHECK (reply[0].body["result"]["serverInfo"]["name"] == "rovox");
    CHECK (reply[1].status == 202);
    CHECK (reply[2].status == 200);
    CHECK (reply[2].body["id"] == 1001);
    CHECK (reply[2].body["result"]["isError"] == false);
    CHECK (reply[3].body["id"] == "abc");
    CHECK (reply[3].body["result"]["tools"].size() >= 40);
    CHECK (reply[4].status == 400);
    CHECK (reply[5].status == 404);
    CHECK (reply[6].status == 400);
    CHECK (reply[6].body["error"]["code"] == -32700);
    CHECK (reply[7].status == 204);
    CHECK (reply[8].status == 404);
}

ROVOX_TEST (mcp, http_rejects_bad_origin_host_and_missing_token)
{
    {
        HttpFixture f ("s3cret");
        auto r = f.h.onClient ([&]
        {
            httplib::Client c ("127.0.0.1", f.server.port());
            std::vector<HttpReply> out;
            out.push_back (post (c, f.headers(), initBody()));                                                       // no token
            f.bearer = "s3cret";
            out.push_back (post (c, f.headers(), initBody()));                                                       // ok
            out.push_back (post (c, f.headers ({ { "Origin", "https://evil.example" } }), initBody()));              // DNS rebinding / foreign page
            out.push_back (post (c, f.headers ({ { "Origin", "https://rovox.example" } }), initBody()));             // allowed origin
            out.push_back (post (c, f.headers ({ { "Origin", "http://localhost:3000" } }), initBody()));             // loopback origin
            out.push_back (post (c, f.headers ({ { "Host", "attacker.example" } }), initBody()));                    // rebinding via Host
            out.push_back (post (c, f.headers ({ { "MCP-Protocol-Version", "1999-01-01" } }), initBody()));
            return out;
        });

        CHECK (r[0].status == 401);
        CHECK (r[1].status == 200);
        CHECK (r[2].status == 403);
        CHECK (r[3].status == 200);
        CHECK (r[4].status == 200);
        CHECK (r[5].status == 403);
        CHECK (r[6].status == 400);
    }

    CHECK (mcp::originAllowed ("http://127.0.0.1:4810", {}));
    CHECK (mcp::originAllowed ("http://[::1]:4810", {}));
    CHECK (! mcp::originAllowed ("http://evil.example", {}));
    CHECK (! mcp::originAllowed ("http://localhost.evil.example", {}));
    CHECK (mcp::hostAllowed ("localhost:4810"));
    CHECK (! mcp::hostAllowed ("rovox.example:4810"));
}

ROVOX_TEST (mcp, http_sse_stream_delivers_notifications_and_resumes_after_a_drop)
{
    HttpFixture f;
    const auto project = f.h.newProject();

    struct Result { std::vector<std::string> first, second; std::string sid; std::string lastId; };

    auto r = f.h.onClient ([&]
    {
        Result out;
        httplib::Client c ("127.0.0.1", f.server.port());
        c.set_read_timeout (3, 0);
        auto init = post (c, f.headers(), initBody());
        out.sid = init.session;
        post (c, f.headers ({ { "Mcp-Session-Id", out.sid } }), { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } });
        post (c, f.headers ({ { "Mcp-Session-Id", out.sid } }), { { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/call" },
                                                                   { "params", { { "name", "events.subscribe" }, { "arguments", { { "project_id", project } } } } } });

        // GET stream; a change by someone else arrives as an event; the callback aborts the stream after the first one
        auto readStream = [&] (std::vector<std::string>& into, const httplib::Headers& hs, std::function<void()> trigger)
        {
            httplib::Client sc ("127.0.0.1", f.server.port());
            sc.set_read_timeout (3, 0);
            std::string buffer;
            bool triggered = false;
            sc.Get ("/mcp", hs, [&] (const httplib::Response& res)
            {
                if (res.status == 200 && ! triggered)
                {
                    triggered = true;
                    std::thread (trigger).detach();     // the headers are out: the stream is open
                }

                return res.status == 200;
            }, [&] (const char* data, size_t len)
            {
                buffer.append (data, len);
                size_t pos;

                while ((pos = buffer.find ("\n\n")) != std::string::npos)
                {
                    auto ev = buffer.substr (0, pos);
                    buffer.erase (0, pos + 2);

                    if (ev.find ("data:") != std::string::npos)
                        into.push_back (ev);
                }

                return into.empty();
            });
        };

        auto change = [&] { f.h.env.executor.run ([&] { f.h.env.runAs ({ Origin::Kind::ui, "ws_1", "alex@browser" }, project, "track.create", { { "name", "T" } }); }).wait(); };

        readStream (out.first, f.headers ({ { "Accept", "text/event-stream" }, { "Mcp-Session-Id", out.sid } }), change);

        if (! out.first.empty())
        {
            const auto idPos = out.first[0].find ("id: ");
            out.lastId = out.first[0].substr (idPos + 4, out.first[0].find ('\n', idPos) - idPos - 4);
        }

        // a second change happens while no stream is open; the reconnect with Last-Event-ID replays it
        f.h.env.executor.run ([&] { f.h.env.runAs ({ Origin::Kind::ui, "ws_1", "alex@browser" }, project, "track.create", { { "name", "T2" } }); }).wait();
        readStream (out.second, f.headers ({ { "Accept", "text/event-stream" }, { "Mcp-Session-Id", out.sid }, { "Last-Event-ID", out.lastId } }), [] {});
        return out;
    });

    REQUIRE (r.first.size() == 1);
    CHECK (r.first[0].find ("notifications/rovox/project_changed") != std::string::npos);
    CHECK (r.first[0].find ("alex@browser") != std::string::npos);
    REQUIRE (r.second.size() >= 1);
    CHECK (r.second[0].find ("notifications/rovox/project_changed") != std::string::npos);

    // the session survived the dropped stream
    auto still = f.h.onClient ([&]
    {
        httplib::Client c ("127.0.0.1", f.server.port());
        return post (c, f.headers ({ { "Mcp-Session-Id", r.sid } }), { { "jsonrpc", "2.0" }, { "id", 9 }, { "method", "ping" } });
    });
    CHECK (still.status == 200);
}

ROVOX_TEST (mcp, http_post_with_progress_token_answers_with_an_event_stream)
{
    HttpFixture f;
    const auto id = sineTracks (f.h.env, { -20.0 }, 20.0);

    auto reply = f.h.onClient ([&]
    {
        httplib::Client c ("127.0.0.1", f.server.port());
        c.set_read_timeout (30, 0);
        auto init = post (c, f.headers(), initBody());
        post (c, f.headers ({ { "Mcp-Session-Id", init.session } }), { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } });
        json call { { "jsonrpc", "2.0" }, { "id", 5 }, { "method", "tools/call" },
                    { "params", { { "name", "render.start" }, { "_meta", { { "progressToken", "tok-1" } } },
                                  { "arguments", { { "project_id", id }, { "name", "p" }, { "wait_s", 60 }, { "target_lufs", -16.0 } } } } } };
        auto res = c.Post ("/mcp", f.headers ({ { "Mcp-Session-Id", init.session } }), call.dump(), "application/json");
        return res ? std::make_pair (res->status, std::make_pair (res->get_header_value ("Content-Type"), res->body)) : std::make_pair (0, std::make_pair (std::string(), std::string()));
    });

    CHECK (reply.first == 200);
    CHECK (reply.second.first.find ("text/event-stream") != std::string::npos);
    CHECK (reply.second.second.find ("\"id\":5") != std::string::npos);
    CHECK (reply.second.second.find ("\"state\":\"succeeded\"") != std::string::npos);
    CHECK (reply.second.second.find ("notifications/progress") != std::string::npos);
    CHECK (reply.second.second.find ("tok-1") != std::string::npos);
}

ROVOX_TEST (mcp, http_light_calls_are_fast)
{
    HttpFixture f;
    auto times = f.h.onClient ([&]
    {
        httplib::Client c ("127.0.0.1", f.server.port());
        auto init = post (c, f.headers(), initBody());
        post (c, f.headers ({ { "Mcp-Session-Id", init.session } }), { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } });
        const auto hs = f.headers ({ { "Mcp-Session-Id", init.session } });
        std::vector<double> ms;

        for (int i = 0; i < 400; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            post (c, hs, { { "jsonrpc", "2.0" }, { "id", i }, { "method", "tools/call" }, { "params", { { "name", "project.list" }, { "arguments", json::object() } } } });
            ms.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count());
        }

        return ms;
    });

    std::sort (times.begin(), times.end());
    const double p50 = times[times.size() / 2], p99 = times[times.size() * 99 / 100];
    std::cerr << "    project.list over HTTP: p50 " << p50 << " ms, p99 " << p99 << " ms\n";
    // the harness pumps the message loop in 2 ms slices and shares the machine with the build, so this only guards against gross
    // regressions; the p99 < 10 ms target is measured against the real server (tools/mcp_e2e.py --bench) in the live run
    CHECK (p99 < 30.0);
}

ROVOX_TEST (mcp, reaper_compat_names_map_onto_the_rovox_tools)
{
    mcp::McpConfig cfg;
    cfg.reaperCompat = true;
    Harness h ({}, cfg);
    auto created = h.tool ("new_project", { { "name", "shim" } });
    REQUIRE (created["ok"] == true);
    const std::string id = created["data"]["project_id"];
    REQUIRE (h.tool ("create_tracks", { { "project_id", id }, { "tracks", json::array ({ { { "name", "Drums" } }, { { "name", "Bass" } } }) } })["ok"] == true);
    auto list = h.tool ("list_tracks", { { "project_id", id } });
    REQUIRE (list["data"]["tracks"].size() == 2);
    const std::string drums = list["data"]["tracks"][0]["track_id"];
    CHECK (h.tool ("update_tracks", { { "project_id", id }, { "updates", json::array ({ { { "track_id", drums }, { "volume_db", -3.0 } } }) } })["ok"] == true);
    CHECK (h.tool ("upsert_markers", { { "project_id", id }, { "markers", json::array ({ { { "name", "Verse" }, { "position_s", 4.0 } } }) } })["ok"] == true);
    CHECK (h.tool ("list_markers", { { "project_id", id } })["data"]["markers"].size() == 1);
    CHECK (h.tool ("add_fx", { { "project_id", id }, { "target", { { "track_id", drums } } }, { "name_or_id", "ReaComp compressor" } })["ok"] == true);
    CHECK (h.tool ("get_tempo_map", { { "project_id", id } })["data"]["points"].size() == 1);
    CHECK (h.tool ("get_undo_history", { { "project_id", id } })["ok"] == true);

    // off by default
    Harness plain;
    CHECK (plain.rpc ("tools/call", { { "name", "list_tracks" }, { "arguments", json::object() } }).contains ("error"));
}

ROVOX_TEST (mcp, transport_and_meter_tools_are_real_commands_not_reserved_stubs)
{
    Harness h;
    const auto project = h.tool ("project.new", { { "name", "t" } })["data"]["project_id"].get<std::string>();

    auto state = h.tool ("transport.get", { { "project_id", project } });
    CHECK (state["ok"] == true);
    CHECK (state["data"]["state"] == "stopped");
    CHECK_NEAR (state["data"]["position_s"].get<double>(), 0.0, 1e-9);

    // seeking and the loop work without an audio device; playing needs one and says so (not NOT_IMPLEMENTED)
    auto seek = h.tool ("transport.set", { { "project_id", project }, { "position_s", 2.5 }, { "loop", { { "enabled", true }, { "start_s", 1.0 }, { "end_s", 3.0 } } } });
    CHECK (seek["ok"] == true);
    CHECK_NEAR (seek["data"]["position_s"].get<double>(), 2.5, 1e-6);
    CHECK (seek["data"]["loop"]["enabled"] == true);

    auto play = h.tool ("transport.set", { { "project_id", project }, { "state", "playing" } });
    CHECK (play["ok"] == false);
    CHECK (play["error"]["code"] == "PRECONDITION_FAILED");
    CHECK (play["error"]["message"].get<std::string>().find ("--audio browser") != std::string::npos);

    auto meters = h.tool ("mixer.get_meters", { { "project_id", project } });
    CHECK (meters["ok"] == true);
    CHECK (meters["data"]["tracks"].is_array());

    auto accepted = h.tool ("agent.accept", { { "project_id", project }, { "up_to_revision", 5 } });
    CHECK (accepted["data"]["accepted_revision"] == 5);
}
