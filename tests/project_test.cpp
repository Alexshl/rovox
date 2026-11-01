// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Ids.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

using namespace rovox;

namespace
{
std::string readText (const fs::path& p)
{
    std::ifstream in (p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** 3 tracks, 5 clips, 2 plugins, 4 markers, all through the command layer except two extra clips. */
std::string buildRichProject (test::Env& env)
{
    const auto id = env.newProject ("Рахів");
    const auto dir = test::makeTempDir ("rich");
    test::writeWav (dir / "a.wav", { test::sine (440, -12, 3.0, 44100) }, 44100);
    test::writeWav (dir / "b.wav", { test::sine (660, -12, 2.5, 44100) }, 44100);
    test::writeWav (dir / "c.wav", { test::sine (880, -12, 2.0, 44100) }, 44100);

    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "a.wav").string(), (dir / "b.wav").string(), (dir / "c.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] }, { "rename", { { "a", "Гітара" } } } });

    auto& p = env.project (id);
    auto tracks = te::getAudioTracks (p.edit());
    REQUIRE (tracks.size() == 3);
    // two more clips outside any command (ids are assigned by the collector)
    for (int i = 0; i < 2; ++i)
    {
        auto* any = tracks[i]->getClips()[0];
        auto* audio = dynamic_cast<te::AudioClipBase*> (any);
        te::insertWaveClip (*tracks[i + 1], "extra", audio->getOriginalFile(), { .time = { te::TimePosition::fromSeconds (10.0 + i), te::TimeDuration::fromSeconds (1.0) } },
                            te::DeleteExistingClips::no);
    }

    test::pumpMessages (50);

    env.ok (id, "fx.add", { { "track_id", env.ok (id, "track.list")["tracks"][0]["track_id"] }, { "builtin", "compressor" } });
    env.ok (id, "fx.add", { { "track_id", "master" }, { "builtin", "reverb" } });

    for (int i = 0; i < 4; ++i)
        env.ok (id, "marker.create", { { "time_s", 1.5 * i }, { "name", "M" + std::to_string (i) } });

    return id;
}
} // namespace

ROVOX_TEST (project, create_save_reopen_roundtrip)
{
    test::Env env;
    const auto id = buildRichProject (env);
    const auto before = test::snapshot (env, id);

    CHECK (before["tracks"].size() == 3);
    CHECK (before["markers"].size() == 4);

    std::size_t clips = 0;

    for (auto& t : before["tracks"])
        clips += t["items"].size();

    CHECK (clips == 5);

    std::size_t externalAndBuiltinFx = 0;

    for (auto& pl : before["plugins"])
        externalAndBuiltinFx += (pl["type"] == "compressor" || pl["type"] == "reverb") ? 1 : 0;

    CHECK (externalAndBuiltinFx == 2);

    // every entity has a stable prefixed id
    for (auto& t : before["tracks"])
    {
        CHECK (isValidId (t["track_id"].get<std::string>(), "trk"));

        for (auto& i : t["items"])
            CHECK (isValidId (i["item_id"].get<std::string>(), "itm"));
    }

    auto& p = env.project (id);
    const auto dir = p.dir();
    REQUIRE (p.save().ok());

    const auto editXml = readText (dir / "edit.xml");
    CHECK (editXml.find ("rovox:id") != std::string::npos);
    CHECK (editXml.find ("source=\"media/") != std::string::npos);      // media paths are relative to the project
    CHECK (editXml.find (dir.string()) == std::string::npos);

    REQUIRE (env.pm.close (id, SaveMode::save).ok());
    auto reopened = env.pm.open (dir.string());
    REQUIRE (reopened.ok());
    CHECK (reopened.value() == id);

    const auto after = test::snapshot (env, id);
    CHECK (before == after);
    CHECK (after["name"] == "Рахів");
}

ROVOX_TEST (project, one_command_one_undo_one_revision_one_event)
{
    test::Env env;
    const auto id = env.newProject();
    auto sub = env.hub.subscribe ({ .projectId = id });

    const std::vector<std::pair<std::string, json>> mutations = {
        { "track.create", { { "name", "Flute" }, { "volume_db", -3.0 } } },
        { "marker.create", { { "time_s", 4.0 }, { "name", "Verse" } } },
    };

    for (auto& [name, params] : mutations)
    {
        const auto revBefore = env.project (id).revision();
        const auto undoBefore = env.project (id).edit().getUndoManager().getNumActionsInCurrentTransaction();
        (void) undoBefore;
        auto r = env.run (id, name, params);
        REQUIRE (r.ok);
        CHECK (r.revision == revBefore + 1);
        CHECK (! r.changes.empty());
        test::pumpMessages (30);

        // exactly one event from the command, tagged with the origin
        Event e;
        int fromCommand = 0;

        while (sub->tryPop (e))
            if (e.body["origin"]["kind"] == "mcp")
            {
                ++fromCommand;
                CHECK (e.body["revision"] == r.revision);
                CHECK (e.body["origin"]["label"] == "Claude");
                CHECK (e.body["type"] == "project.changed");
                CHECK (e.body["undo_label"].get<std::string>().rfind ("Claude: " + name, 0) == 0);
            }

        CHECK (fromCommand == 1);

        // one undo reverts the whole command
        const auto snapBefore = test::snapshot (env, id);
        auto u = env.run (id, "undo");
        REQUIRE (u.ok);
        CHECK (u.data["undone_label"].get<std::string>().rfind ("Claude: " + name, 0) == 0);
        CHECK (test::snapshot (env, id) != snapBefore);
        REQUIRE (env.run (id, "redo").ok);
        CHECK (test::snapshot (env, id) == snapBefore);
        test::pumpMessages (30);

        while (sub->tryPop (e))
        {}
    }

    // Tracktion's lastSignificantChange housekeeping never shows up as a change or as a system event
    test::pumpMessages (100);
    Event e;

    while (sub->tryPop (e))
        for (auto& c : e.body["changes"])
            for (auto& f : c["fields"])
                CHECK (f != "lastSignificantChange");
}

ROVOX_TEST (project, new_project_is_quiet_and_has_unity_master)
{
    test::Env env;
    const auto id = env.newProject();
    auto sub = env.hub.subscribe ({ .projectId = id });
    CHECK_NEAR (env.project (id).edit().getMasterVolumePlugin()->getVolumeDb(), 0.0, 1e-6);
    const auto rev = env.project (id).revision();
    test::pumpMessages (200);
    CHECK (env.project (id).revision() == rev);
    Event e;
    CHECK (! sub->tryPop (e));
    CHECK (! env.project (id).dirty() || rev == 0);
}

ROVOX_TEST (project, parameter_changes_are_reported_by_their_own_command)
{
    test::Env env;
    const auto id = env.newProject();
    const auto trackId = env.ok (id, "track.create", { { "name", "A" } })["track_id"].get<std::string>();
    test::pumpMessages (100);
    auto sub = env.hub.subscribe ({ .projectId = id });
    auto r = env.run (id, "track.update", { { "track_id", trackId }, { "volume_db", -9.0 }, { "pan", 0.25 } });
    REQUIRE (r.ok);
    CHECK (! r.changes.empty());
    test::pumpMessages (200);

    // the values reached the tree inside the command's scope, not later as anonymous system changes
    Event e;
    int system = 0;

    while (sub->tryPop (e))
        system += e.body["origin"]["kind"] == "system" ? 1 : 0;

    CHECK (system == 0);
    CHECK_NEAR (env.ok (id, "track.list")["tracks"][0]["volume_db"].get<double>(), -9.0, 1e-4);
    env.ok (id, "undo");
    CHECK_NEAR (env.ok (id, "track.list")["tracks"][0]["volume_db"].get<double>(), 0.0, 1e-4);
}

ROVOX_TEST (project, history_lists_steps_with_origin)
{
    test::Env env;
    const auto id = env.newProject();
    env.ok (id, "track.create", { { "name", "A" } });
    env.runAs ({ Origin::Kind::ui, "ws_1", "alex" }, id, "track.create", { { "name", "B" } });
    auto h = env.ok (id, "history.list");
    REQUIRE (h["undo"].size() == 2);
    CHECK (h["undo"][0]["origin"]["kind"] == "mcp");
    CHECK (h["undo"][1]["origin"]["kind"] == "ui");
    CHECK (h["undo"][1]["label"].get<std::string>().rfind ("alex: ", 0) == 0);
    env.ok (id, "undo");
    h = env.ok (id, "history.list");
    CHECK (h["undo"].size() == 1);
    CHECK (h["redo"].size() == 1);
}

ROVOX_TEST (project, batch_of_50_is_one_undo_step)
{
    test::Env env;
    const auto id = env.newProject();
    std::vector<Op> ops;

    for (int i = 0; i < 50; ++i)
        ops.push_back ({ "track.create", { { "name", "T" + std::to_string (i) } } });

    CommandContext ctx;
    ctx.projectId = id;
    ctx.origin = { Origin::Kind::mcp, "s", "Claude" };
    auto sub = env.hub.subscribe ({ .projectId = id });
    const auto rev = env.project (id).revision();
    auto r = env.bus.runBatchNow (ops, ctx);
    REQUIRE (r.ok);
    CHECK (r.revision == rev + 1);
    CHECK (te::getAudioTracks (env.project (id).edit()).size() == 50);
    test::pumpMessages (30);
    int events = 0;
    Event e;

    while (sub->tryPop (e))
        events += e.body["origin"]["kind"] == "mcp" ? 1 : 0;

    CHECK (events == 1);
    REQUIRE (env.run (id, "undo").ok);
    CHECK (te::getAudioTracks (env.project (id).edit()).size() == 0);
}

ROVOX_TEST (project, failing_batch_leaves_no_partial_changes)
{
    test::Env env;
    const auto id = env.newProject();
    env.ok (id, "track.create", { { "name", "Keep" } });
    const auto before = test::snapshot (env, id);
    const auto rev = env.project (id).revision();
    const auto activity = env.project (id).activity().size();

    CommandContext ctx;
    ctx.projectId = id;
    ctx.origin = { Origin::Kind::mcp, "s", "Claude" };

    // second operation fails in execute (unknown track), after the first one changed the Edit
    auto r = env.bus.runBatchNow ({ { "track.create", { { "name", "Partial" } } },
                                    { "track.update", { { "track_id", "trk_DOES_NOT_EXIST" }, { "name", "x" } } },
                                    { "track.create", { { "name", "Never" } } } }, ctx);
    const auto activityAfter = env.project (id).activity().size();
    CHECK (! r.ok);
    REQUIRE (r.error.has_value());
    CHECK (r.error->code == errc::notFound);
    CHECK (test::snapshot (env, id) == before);
    CHECK (env.project (id).revision() == rev);
    CHECK (activityAfter == activity + 1);          // a failed command is logged, with ok=false
    auto sub = env.hub.subscribe ({ .projectId = id });
    test::pumpMessages (50);
    {
        Event e;

        while (sub->tryPop (e))
            std::cerr << "    event after rollback: " << e.body.dump() << "\n";
    }

    CHECK (env.project (id).revision() == rev);         // the rollback itself caused no system revision either

    // a single failing command too
    auto r2 = env.run (id, "track.update", { { "track_id", "trk_NOPE" } });
    CHECK (! r2.ok);
    CHECK (test::snapshot (env, id) == before);

    // invalid params are reported with a pointer to the offending field and change nothing
    auto r3 = env.run (id, "track.create", { { "volume_db", 500 } });
    CHECK (! r3.ok);
    REQUIRE (r3.error.has_value());
    CHECK (r3.error->code == errc::invalidParams);
    CHECK (r3.error->details["path"] == "/volume_db");
    CHECK (env.run (id, "track.create", { { "bogus", 1 } }).error->code == errc::invalidParams);
    CHECK (test::snapshot (env, id) == before);
}

ROVOX_TEST (project, stale_if_revision_is_a_conflict)
{
    test::Env env;
    const auto id = env.newProject();
    env.ok (id, "track.create", { { "name", "A" } });
    const auto rev = env.project (id).revision();
    const auto before = test::snapshot (env, id);
    auto r = env.run (id, "track.create", { { "name", "B" } }, rev - 1);
    CHECK (! r.ok);
    REQUIRE (r.error.has_value());
    CHECK (r.error->code == errc::revisionConflict);
    CHECK (r.error->details["current_revision"] == rev);
    CHECK (test::snapshot (env, id) == before);
    CHECK (env.run (id, "track.create", { { "name", "B" } }, rev).ok);
}

ROVOX_TEST (project, dry_run_validates_without_applying)
{
    test::Env env;
    const auto id = env.newProject();
    CommandContext ctx;
    ctx.projectId = id;
    ctx.dryRun = true;
    const auto rev = env.project (id).revision();
    CHECK (env.bus.runNow ("track.create", { { "name", "x" } }, ctx).ok);
    CHECK (! env.bus.runNow ("track.create", { { "volume_db", 999 } }, ctx).ok);
    CHECK (env.project (id).revision() == rev);
    CHECK (te::getAudioTracks (env.project (id).edit()).isEmpty());
}

ROVOX_TEST (project, changes_outside_commands_are_system_events)
{
    test::Env env;
    const auto id = env.newProject();
    auto sub = env.hub.subscribe ({ .projectId = id });
    const auto rev = env.project (id).revision();
    auto* track = env.project (id).edit().insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (env.project (id).edit()), nullptr).get();
    REQUIRE (track != nullptr);
    test::pumpMessages (100);
    CHECK (env.project (id).revision() > rev);

    Event e;
    bool sawSystem = false;

    while (sub->tryPop (e))
        sawSystem = sawSystem || e.body["origin"]["kind"] == "system";

    CHECK (sawSystem);
    CHECK (isValidId (Project::idOf (track->state), "trk"));
}

ROVOX_TEST (project, slow_subscriber_gets_resync_and_does_not_block_others)
{
    EventHub hub;
    auto slow = hub.subscribe ({ .queueLimit = 8 });
    auto fast = hub.subscribe ({ .queueLimit = 8 });
    std::atomic<int> fastCount { 0 };
    std::thread reader ([&]
    {
        Event e;

        while (fast->pop (e, std::chrono::milliseconds (500)))
            ++fastCount;
    });

    const auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < 1000; ++i)
    {
        hub.publish ("local", "p_x", { { "type", "project.changed" }, { "revision", i } });

        if (i % 4 == 0)
            std::this_thread::sleep_for (std::chrono::microseconds (200));      // let the reader keep up
    }

    const auto publishMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    reader.join();
    CHECK (publishMs < 1000.0);         // 1000 publishes with a stuck subscriber: nobody waited for it
    CHECK (fastCount.load() > 500);

    Event e;
    REQUIRE (slow->tryPop (e));
    CHECK (e.body["type"] == "resync_required");
    CHECK (slow->pending() == 0);
}

ROVOX_TEST (project, events_are_filtered_by_tenant)
{
    EventHub hub;
    auto a = hub.subscribe ({ .tenant = "a" });
    auto b = hub.subscribe ({ .tenant = "b" });
    hub.publish ("a", "p_1", { { "type", "project.changed" } });
    Event e;
    CHECK (a->tryPop (e));
    CHECK (! b->tryPop (e));
}

ROVOX_TEST (project, tenants_do_not_see_each_others_projects)
{
    test::Env env;
    CreateOptions oa, ob;
    oa.tenant = "alice";
    ob.tenant = "bob";
    oa.name = "A";
    ob.name = "B";
    auto a = env.pm.create (oa), b = env.pm.create (ob);
    REQUIRE (a.ok());
    REQUIRE (b.ok());

    CommandContext ctxA;
    ctxA.tenant = "alice";
    auto la = env.bus.runNow ("project.list", json::object(), ctxA);
    REQUIRE (la.ok);
    REQUIRE (la.data["projects"].size() == 1);
    CHECK (la.data["projects"][0]["project_id"] == a.value());

    // bob cannot address alice's project, neither as a command target nor by opening it
    CommandContext cross;
    cross.tenant = "bob";
    cross.projectId = a.value();
    CHECK (env.bus.runNow ("track.list", json::object(), cross).error->code == errc::notFound);
    CHECK (env.pm.find (a.value(), "bob") == nullptr);
    CHECK (env.pm.find (a.value(), "alice") != nullptr);
    auto opened = env.pm.open (a.value(), { .tenant = "bob" });
    CHECK (! opened.ok());

    // hostile tenant ids are rejected
    CreateOptions bad;
    bad.tenant = "../x";
    CHECK (! env.pm.create (bad).ok());
    CHECK (env.pm.projectDir ("alice", a.value()).string().find ("tenants/alice/projects") != std::string::npos);
}

ROVOX_TEST (project, ephemeral_projects_are_hidden_and_swept_after_ttl)
{
    test::Env env;
    const auto keep = env.newProject ("keep");
    const auto temp = env.newProject ("temp", true, 2);
    const auto tempDir = env.project (temp).dir();

    CHECK (env.pm.list ("local").size() >= 1);

    bool visibleByDefault = false;

    for (auto& s : env.pm.list ("local"))
        visibleByDefault = visibleByDefault || s.id == temp;

    CHECK (! visibleByDefault);

    bool visibleWithFlag = false;

    for (auto& s : env.pm.list ("local", true))
        visibleWithFlag = visibleWithFlag || s.id == temp;

    CHECK (visibleWithFlag);

    // set_ttl through the command layer
    CommandContext ctx;
    ctx.projectId = temp;
    auto r = env.bus.runNow ("project.set_ttl", { { "project_id", temp }, { "ttl_s", 2 } }, ctx);
    CHECK (r.ok);

    const auto now = std::time (nullptr);
    CHECK (env.pm.sweepExpired (now).empty());                  // not expired yet

    env.project (temp).activeJobs() = 1;
    CHECK (env.pm.sweepExpired (now + 100).empty());            // expired but a job is running on it
    CHECK (fs::exists (tempDir));
    env.project (temp).activeJobs() = 0;

    const auto removed = env.pm.sweepExpired (now + 100);
    REQUIRE (removed.size() == 1);
    CHECK (removed[0] == temp);
    CHECK (! fs::exists (tempDir));
    CHECK (env.pm.find (keep) != nullptr);                      // a persistent project is never swept
    REQUIRE (! env.pm.ttlLog().empty());
    CHECK (env.pm.ttlLog().back()["origin"] == "system:ttl");

    // an ephemeral project that is not open (left over on disk) is swept as well
    const auto orphan = env.newProject ("orphan", true, 1);
    const auto orphanDir = env.project (orphan).dir();
    REQUIRE (env.pm.close (orphan, SaveMode::save).ok());
    CHECK (fs::exists (orphanDir));
    env.pm.sweepExpired (std::time (nullptr) + 100);
    CHECK (! fs::exists (orphanDir));
}

ROVOX_TEST (project, manifest_without_tenant_opens_as_local_and_old_schema_migrates)
{
    test::Env env;
    const auto id = env.newProject ("legacy");
    auto& p = env.project (id);
    const auto dir = p.dir();
    REQUIRE (p.save().ok());
    REQUIRE (env.pm.close (id, SaveMode::discard).ok());

    // schema 0: pre-release key names, no tenant_id / ephemeral
    json legacy { { "schema", 0 }, { "id", id }, { "name", "legacy" }, { "revision", 0 }, { "rate", 48000 } };
    std::ofstream (dir / "rovox.json") << legacy.dump();
    auto opened = env.pm.open (dir.string());
    REQUIRE (opened.ok());
    auto& q = env.project (opened.value());
    CHECK (q.tenant() == "local");
    CHECK (! q.manifest().ephemeral);
    CHECK (q.manifest().sampleRate == 48000.0);
    CHECK (q.manifest().schema == 1);

    auto migrated = migrateManifest (legacy);
    REQUIRE (migrated.ok());
    CHECK (migrated.value()["project_id"] == id);
    CHECK (! migrated.value().contains ("rate"));

    CHECK (! migrateManifest ({ { "schema", 99 }, { "project_id", "x" } }).ok());
}

ROVOX_TEST (project, interrupted_save_recovers_old_or_new_revision)
{
    test::Env env;
    const auto id = env.newProject ("crash");
    env.ok (id, "track.create", { { "name", "One" } });
    auto& p = env.project (id);
    REQUIRE (p.save().ok());
    const auto dir = p.dir();
    const auto savedRevision = p.revision();

    env.ok (id, "track.create", { { "name", "Two" } });

    // crash after the temp files were written: the old pair is untouched
    CHECK (! p.save ({ .fault = SaveFault::afterTempFiles }).ok());
    {
        auto r = readProject (dir);
        REQUIRE (r.ok());
        CHECK (r.value().manifest.revision == savedRevision);
        CHECK (r.value().editState.getChildWithName (te::IDs::TRACK).isValid());
    }

    fs::remove (dir / "edit.xml.tmp");
    fs::remove (dir / "rovox.json.tmp");

    // crash between the two renames: new edit.xml, old manifest -> opens as the new revision
    CHECK (! p.save ({ .fault = SaveFault::betweenRenames }).ok());
    {
        auto r = readProject (dir);
        REQUIRE (r.ok());
        CHECK (r.value().manifest.revision == p.revision());
        CHECK (! r.value().warnings.empty());
    }

    fs::remove (dir / "rovox.json.tmp");
    REQUIRE (env.pm.close (id, SaveMode::discard).ok());
    auto reopened = env.pm.open (dir.string());
    REQUIRE (reopened.ok());
    CHECK (te::getAudioTracks (env.project (reopened.value()).edit()).size() == 2);

    // a corrupted edit.xml falls back to the newest autosave
    REQUIRE (env.project (id).autosave().ok());
    REQUIRE (env.pm.close (id, SaveMode::discard).ok());
    std::ofstream (dir / "edit.xml", std::ios::trunc) << "<<< not xml";
    auto r = readProject (dir);
    REQUIRE (r.ok());
    CHECK (! r.value().warnings.empty());
}

ROVOX_TEST (project, autosave_rotates_and_is_offered_for_recovery)
{
    test::Env env;
    const auto id = env.newProject ("auto");
    auto& p = env.project (id);
    const auto dir = p.dir();
    REQUIRE (p.save().ok());

    for (int i = 0; i < 25; ++i)
    {
        env.ok (id, "track.create", { { "name", "T" + std::to_string (i) } });
        REQUIRE (p.autosave().ok());
    }

    const auto all = listAutosaves (dir);
    CHECK ((int) all.size() == autosaveKeep);
    CHECK (all.back() == p.revision());

    REQUIRE (env.pm.close (id, SaveMode::discard).ok());     // crash: newer state only in autosave
    auto plain = readProject (dir);
    REQUIRE (plain.ok());
    REQUIRE (plain.value().newerAutosave.has_value());

    auto recovered = env.pm.open (dir.string(), { .preferAutosave = true });
    REQUIRE (recovered.ok());
    CHECK (te::getAudioTracks (env.project (recovered.value()).edit()).size() == 25);
}

ROVOX_TEST (project, second_open_of_a_locked_project_is_refused)
{
    test::Env env;
    const auto id = env.newProject ("locked");
    const auto dir = env.project (id).dir();
    REQUIRE (env.pm.close (id, SaveMode::save).ok());

    // Project A holds the flock in this process; a second ProjectManager (= another open file description) must be refused
    test::Env other;
    auto first = other.pm.open (dir.string());
    REQUIRE (first.ok());

    auto second = env.pm.open (dir.string());
    CHECK (! second.ok());
    CHECK (second.error().code == errc::busy);

    auto readOnly = env.pm.open (dir.string(), { .readOnly = true });
    CHECK (readOnly.ok());
}

ROVOX_TEST (project, track_limit_is_an_error_not_a_hang)
{
    test::Env env;
    const auto id = env.newProject();
    CHECK (test::host().engine().getEngineBehaviour().getEditLimits().maxNumTracks >= 4096);

    std::vector<Op> ops;

    for (int i = 0; i < 450; ++i)       // beyond Tracktion's default of 400
        ops.push_back ({ "track.create", json::object() });

    CommandContext ctx;
    ctx.projectId = id;
    auto r = env.bus.runBatchNow (ops, ctx);
    CHECK (r.ok);
    CHECK (te::getAudioTracks (env.project (id).edit()).size() == 450);
}

ROVOX_TEST (project, track_commands_use_ids_and_units)
{
    test::Env env;
    const auto id = env.newProject();
    const auto a = env.ok (id, "track.create", { { "name", "A" }, { "volume_db", -6.0 }, { "pan", 0.5 }, { "color", "#ff8800" } })["track_id"].get<std::string>();
    const auto b = env.ok (id, "track.create", { { "name", "B" } })["track_id"].get<std::string>();
    auto tracks = env.ok (id, "track.list")["tracks"];
    CHECK_NEAR (tracks[0]["volume_db"].get<double>(), -6.0, 1e-4);
    CHECK_NEAR (tracks[0]["pan"].get<double>(), 0.5, 1e-4);
    CHECK (tracks[0]["color"] == "#ff8800");

    env.ok (id, "track.update", { { "track_id", b }, { "name", "B2" }, { "mute", true }, { "volume_db", -12 } });
    env.ok (id, "track.move", { { "track_id", b }, { "to_index", 0 } });
    tracks = env.ok (id, "track.list")["tracks"];
    CHECK (tracks[0]["track_id"] == b);
    CHECK (tracks[0]["name"] == "B2");
    CHECK (tracks[0]["mute"] == true);
    CHECK (tracks[1]["track_id"] == a);

    env.ok (id, "track.delete", { { "track_id", a } });
    CHECK (env.ok (id, "track.list")["tracks"].size() == 1);
    env.ok (id, "undo");
    CHECK (env.ok (id, "track.list")["tracks"].size() == 2);
}

ROVOX_TEST (project, activity_log_redacts_secrets)
{
    CHECK (ActivityLog::redact ({ { "token", "abc" }, { "nested", { { "api_key", "k" }, { "name", "ok" } } } }) ==
           json ({ { "token", "***" }, { "nested", { { "api_key", "***" }, { "name", "ok" } } } }));

    test::Env env;
    const auto id = env.newProject();
    env.ok (id, "track.create", { { "name", "A" } });
    auto log = env.ok (id, "project.activity")["activity"];
    REQUIRE (! log.empty());
    CHECK (log.back()["name"] == "track.create");
    CHECK (log.back()["ok"] == true);
    CHECK (log.back()["origin"]["label"] == "Claude");
}

ROVOX_TEST (project, executor_survives_16_threads_x_1000_calls)
{
    MessageThreadExecutor exec;
    std::atomic<int> done { 0 }, wrongThread { 0 };
    std::atomic<bool> allSubmitted { false };
    std::vector<std::thread> threads;

    for (int t = 0; t < 16; ++t)
        threads.emplace_back ([&]
        {
            for (int i = 0; i < 1000; ++i)
            {
                auto f = exec.run ([&] { if (! juce::MessageManager::getInstance()->isThisTheMessageThread()) ++wrongThread; return 1; });
                done += f.get();
            }
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

    while (done.load() < 16000 && std::chrono::steady_clock::now() < deadline)
        test::pumpMessages (5);

    for (auto& t : threads)
        t.join();

    CHECK (done.load() == 16000);
    CHECK (wrongThread.load() == 0);

    // a task that waits longer than its deadline for the busy message thread is dropped with a timeout error
    std::future<int> late;
    std::thread submitter ([&] { late = exec.run ([] { return 1; }, std::chrono::milliseconds (1)); });
    submitter.join();
    std::this_thread::sleep_for (std::chrono::milliseconds (30));
    test::pumpMessages (30);
    bool timedOut = false;

    try { late.get(); } catch (const ExecutorTimeout&) { timedOut = true; }

    CHECK (timedOut);
}

ROVOX_TEST (project, five_projects_close_two_list_returns_three_with_stable_ids)
{
    test::Env env;
    std::vector<std::string> ids;

    for (int i = 0; i < 5; ++i)
        ids.push_back (env.newProject ("P" + std::to_string (i)));

    REQUIRE (env.pm.close (ids[1], SaveMode::save).ok());
    REQUIRE (env.pm.close (ids[3], SaveMode::save).ok());

    std::set<std::string> listed;

    for (auto& s : env.pm.list ("local"))
        listed.insert (s.id);

    CHECK (listed == std::set<std::string> ({ ids[0], ids[2], ids[4] }));
    CHECK (env.pm.find (ids[2]) != nullptr);
    CHECK (env.pm.find (ids[1]) == nullptr);
}

ROVOX_TEST (project, every_mutating_command_is_exactly_one_step)
{
    using Params = std::function<json (test::Env&, const std::string&)>;
    const auto rppDir = test::makeTempDir ("onestep");
    test::writeWav (rppDir / "a.wav", { test::sine (440, -12, 2.0, 44100) }, 44100);
    std::ofstream (rppDir / "p.RPP") << "<REAPER_PROJECT 0.1\n  TEMPO 100 4 4 0\n  MARKER 1 1 \"m\" 0 0 1\n  <TRACK\n    NAME \"t\"\n    <ITEM\n      POSITION 0\n      LENGTH 1\n      <SOURCE WAVE\n        FILE \"a.wav\"\n      >\n    >\n  >\n>\n";

    auto firstTrack = [] (test::Env& e, const std::string& id) { return e.ok (id, "track.list")["tracks"][0]["track_id"]; };
    const std::map<std::string, Params> cases = {
        { "track.create",   [] (test::Env&, const std::string&) { return json { { "name", "New" }, { "volume_db", -3 } }; } },
        { "track.update",   [&] (test::Env& e, const std::string& id) { return json { { "track_id", firstTrack (e, id) }, { "name", "Renamed" }, { "pan", 0.3 } }; } },
        { "track.delete",   [&] (test::Env& e, const std::string& id) { return json { { "track_id", firstTrack (e, id) } }; } },
        { "track.move",     [&] (test::Env& e, const std::string& id) { return json { { "track_id", firstTrack (e, id) }, { "to_index", 1 } }; } },
        { "fx.add",         [&] (test::Env& e, const std::string& id) { return json { { "track_id", firstTrack (e, id) }, { "builtin", "equaliser" } }; } },
        { "fx.remove",      [&] (test::Env& e, const std::string& id) { return json { { "plugin_id", e.ok (id, "fx.add", { { "track_id", firstTrack (e, id) }, { "builtin", "reverb" } })["plugin_id"] } }; } },
        { "marker.create",  [] (test::Env&, const std::string&) { return json { { "time_s", 2.5 }, { "name", "Chorus" } }; } },
        { "marker.update",  [] (test::Env& e, const std::string& id) { return json { { "marker_id", e.ok (id, "marker.list")["markers"][0]["marker_id"] }, { "name", "Bridge" } }; } },
        { "marker.delete",  [] (test::Env& e, const std::string& id) { return json { { "marker_id", e.ok (id, "marker.list")["markers"][0]["marker_id"] } }; } },
        { "project.update", [] (test::Env&, const std::string&) { return json { { "name", "Renamed project" } }; } },
        { "import.apply",   [&] (test::Env& e, const std::string& id) { return json { { "plan_id", e.ok (id, "import.plan", { { "paths", { (rppDir / "a.wav").string() } } })["plan_id"] } }; } },
        { "import.rpp",     [&] (test::Env&, const std::string&) { return json { { "path", (rppDir / "p.RPP").string() } }; } },
    };

    // a new mutating project command without a case here fails this test
    test::Env registryEnv;

    for (auto* c : registryEnv.registry.all())
        if (c->mutates() && ! c->isGlobal() && ! c->manualUndo())
            CHECK (cases.count (std::string (c->name())) == 1);

    for (auto& [name, makeParams] : cases)
    {
        test::Env env;
        const auto id = env.newProject();
        env.ok (id, "track.create", { { "name", "A" } });
        env.ok (id, "track.create", { { "name", "B" } });
        env.ok (id, "marker.create", { { "time_s", 1.0 }, { "name", "M" } });
        test::pumpMessages (50);

        const auto params = makeParams (env, id);
        const auto before = test::snapshot (env, id);
        const auto rev = env.project (id).revision();
        auto sub = env.hub.subscribe ({ .projectId = id });
        auto r = env.run (id, name, params);

        if (! r.ok)
            std::cerr << "    " << name << " failed: " << r.error->toJson() << "\n";

        REQUIRE (r.ok);
        CHECK (r.revision == rev + 1);
        test::pumpMessages (50);
        Event e;
        int events = 0;

        while (sub->tryPop (e))
        {
            events += e.body["origin"]["kind"] == "mcp" ? 1 : 0;

            if (e.body["origin"]["kind"] == "system")
                std::cerr << "    " << name << " system event: " << e.body["changes"].dump() << "\n";
        }

        if (events != 1 || env.project (id).revision() != rev + 1)
            std::cerr << "    " << name << ": events " << events << ", revision " << env.project (id).revision() << " (expected " << rev + 1 << ")\n";

        CHECK (events == 1);
        CHECK (env.project (id).revision() == rev + 1);

        const auto after = test::snapshot (env, id);
        CHECK (after != before);
        REQUIRE (env.run (id, "undo").ok);
        test::pumpMessages (30);
        CHECK (test::snapshot (env, id) == before);          // one undo restores everything
        REQUIRE (env.run (id, "redo").ok);
        test::pumpMessages (30);
        CHECK (test::snapshot (env, id) == after);
    }
}
