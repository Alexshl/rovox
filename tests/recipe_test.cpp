// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E8-01: recipe.run as a job (progress `step i/N`, cancel between steps, ephemeral projects removed on cancel), procedural built-in
// recipes in the same registry as the JSON ones, errors that carry {step, code}, and steps that never have more rights than the caller.
// Tenant ids are unique to this file (`rec_*`) because every test of the process shares one data directory.
#include "Auth.h"
#include "InstrumentPacks.h"
#include "JobManager.h"
#include "Mcp.h"
#include "RenderJobs.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <atomic>
#include <thread>

using namespace rovox;

namespace
{
/** Tool calls block on the message thread, so they run on a client thread while this thread pumps. */
struct Harness
{
    Harness() : jobs (fs::path {}), service (env.pm, jobs, env.executor), server (env.pm, env.bus, env.hub, jobs, service, {}) {}
    ~Harness() { jobs.shutdown(); }

    std::shared_ptr<mcp::Session> session (const Principal& principal)
    {
        auto s = server.createSession (principal);
        rpc (s, "initialize", { { "protocolVersion", "2025-06-18" }, { "capabilities", json::object() }, { "clientInfo", { { "name", "recipe-test" }, { "version", "1" } } } });
        mcp::RequestContext rc;
        server.handle (*s, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }, rc);
        return s;
    }

    std::shared_ptr<mcp::Session> session (const std::string& tenant = "local")
    {
        Principal p;
        p.tenant = tenant;
        return session (p);
    }

    json rpc (const std::shared_ptr<mcp::Session>& s, const std::string& method, json params)
    {
        json req { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", std::move (params) } };
        std::atomic<bool> done { false };
        json out;

        std::thread t ([&]
        {
            mcp::RequestContext rc;
            auto r = server.handle (*s, req, rc);
            out = r ? *r : json (nullptr);
            done = true;
        });

        while (! done)
            test::pumpMessages (2);

        t.join();
        return out;
    }

    /** tools/call; the structuredContent {ok, data | error}. */
    json tool (const std::shared_ptr<mcp::Session>& s, const std::string& name, json args = json::object())
    {
        auto r = rpc (s, "tools/call", { { "name", name }, { "arguments", std::move (args) } });

        if (! r.contains ("result"))
            return { { "ok", false }, { "protocol_error", r.value ("error", json (nullptr)) } };

        return r["result"]["structuredContent"];
    }

    json waitJob (const std::shared_ptr<mcp::Session>& s, const std::string& id, double seconds = 60)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double> (seconds);

        for (;;)
        {
            auto j = tool (s, "job.get", { { "job_id", id }, { "wait_s", 2 } });
            REQUIRE (j["ok"] == true);

            if (j["data"]["state"] != "queued" && j["data"]["state"] != "running")
                return j["data"];

            REQUIRE (std::chrono::steady_clock::now() < deadline);
        }
    }

    test::Env env;
    JobManager jobs;
    JobService service;
    mcp::McpServer server;
};

json step (const std::string& tool, json arguments = json::object())
{
    return { { "tool", tool }, { "arguments", std::move (arguments) } };
}

/** accompaniment.v1 needs the shipped General MIDI soundfont (the flute-piano template plays it); without it the test skips, ROVOX_REQUIRE_SF2=1 makes that a failure. */
bool haveSoundfont()
{
    std::error_code ec;

    if (fs::exists (InstrumentPacks::builtinSoundfont(), ec))
        return true;

    const bool require = std::getenv ("ROVOX_REQUIRE_SF2") != nullptr;
    std::cerr << (require ? "    the image should have the General MIDI soundfont but it is missing\n" : "    skipped: no General MIDI soundfont installed\n");
    CHECK (! require);
    return false;
}

/** 8 bars of 3/4: a triad per bar, a bass note on the downbeat, a side-stick pulse and a melody note on beat 2 of every bar. */
json waltzParts()
{
    json melody = json::array(), chords = json::array(), bass = json::array(), drums = json::array();

    for (int bar = 0; bar < 8; ++bar)
    {
        const double at = bar * 3.0;
        melody.push_back ({ { "pitch", 72 + (bar % 3) }, { "start_beats", at + 1.0 }, { "length_beats", 2.0 }, { "velocity", 90 } });

        for (int pitch : { 57, 60, 64 })
            chords.push_back ({ { "pitch", pitch }, { "start_beats", at }, { "length_beats", 3.0 }, { "velocity", 70 } });

        bass.push_back ({ { "pitch", 45 }, { "start_beats", at }, { "length_beats", 1.0 }, { "velocity", 90 } });

        for (int beat = 0; beat < 3; ++beat)
            drums.push_back ({ { "pitch", beat == 0 ? 36 : 42 }, { "start_beats", at + beat }, { "length_beats", 0.25 }, { "velocity", beat == 0 ? 100 : 70 } });
    }

    return { { "melody", { { "notes", melody } } }, { "chords", { { "notes", chords } } }, { "bass", { { "notes", bass } } }, { "drums", { { "notes", drums } } } };
}

bool hasProject (Harness& h, const std::shared_ptr<mcp::Session>& s, const std::string& name)
{
    auto list = h.tool (s, "project.list", json::object());
    REQUIRE (list["ok"] == true);

    for (auto& p : list["data"]["projects"])
        if (p.value ("name", "") == name)
            return true;

    return false;
}
} // namespace

ROVOX_TEST (recipe, procedural_built_in_is_listed_with_its_params_schema)
{
    Harness h;
    auto s = h.session();
    auto list = h.tool (s, "recipe.list");
    REQUIRE (list["ok"] == true);
    json found = nullptr;

    for (auto& r : list["data"]["recipes"])
        if (r["recipe_id"] == "render_mixdown")
            found = r;

    REQUIRE (found.is_object());
    CHECK (found["builtin"] == true);
    CHECK (found["procedural"] == true);
    CHECK (found["params"]["properties"].contains ("variants_lufs"));
    CHECK (found["params"]["required"][0] == "project_id");

    auto got = h.tool (s, "recipe.get", { { "recipe_id", "render_mixdown" } });
    REQUIRE (got["ok"] == true);
    CHECK (got["data"]["recipe"]["procedural"] == true);

    // a built-in id can be neither overwritten nor deleted
    CHECK (h.tool (s, "recipe.save", { { "recipe", { { "id", "render_mixdown" }, { "steps", json::array ({ step ("project.list") }) } } } })["ok"] == false);
    CHECK (h.tool (s, "recipe.delete", { { "recipe_id", "render_mixdown" } })["ok"] == false);

    // parameters are checked against the schema before any step runs
    auto bad = h.tool (s, "recipe.run", { { "recipe_id", "render_mixdown" }, { "params", { { "container", "ogg" } } } });
    CHECK (bad["ok"] == false);
    CHECK (bad["error"]["code"] == "INVALID_PARAMS");
}

ROVOX_TEST (recipe, procedural_recipe_errors_carry_the_step_and_the_code)
{
    Harness h;
    auto s = h.session();
    auto r = h.tool (s, "recipe.run", { { "recipe_id", "render_mixdown" }, { "project_id", "p_00000000000000000000000000" }, { "params", { { "variants_lufs", { -16, -14 } } } } });
    CHECK (r["ok"] == false);
    REQUIRE (r["error"].contains ("details"));
    CHECK (r["error"]["details"]["step"] == 0);
    CHECK (r["error"]["details"]["code"] == r["error"]["code"]);
    CHECK (r["error"]["details"]["report"].size() == 1);
}

ROVOX_TEST (recipe, json_recipe_errors_carry_the_step_and_the_code)
{
    Harness h;
    auto s = h.session();
    json recipe { { "id", "rec-two-steps" }, { "name", "Two steps" }, { "steps", json::array ({ step ("project.list"), step ("no.such_tool") }) } };
    REQUIRE (h.tool (s, "recipe.save", { { "recipe", recipe } })["ok"] == true);

    auto r = h.tool (s, "recipe.run", { { "recipe_id", "rec-two-steps" } });
    CHECK (r["ok"] == false);
    CHECK (r["error"]["code"] == "NOT_FOUND");
    CHECK (r["error"]["details"]["step"] == 1);
    CHECK (r["error"]["details"]["code"] == "NOT_FOUND");
    CHECK (r["error"]["details"]["report"].size() == 2);
    CHECK (h.tool (s, "recipe.delete", { { "recipe_id", "rec-two-steps" } })["ok"] == true);
}

ROVOX_TEST (recipe, async_run_returns_a_job_whose_result_is_the_report)
{
    Harness h;
    auto s = h.session();
    json recipe { { "id", "rec-async-ok" }, { "name", "Async" },
                  { "steps", json::array ({ step ("project.new", { { "name", "rec-async-ok-project" }, { "ephemeral", true }, { "ttl_s", 3600 } }),
                                            step ("track.create", { { "project_id", "${steps.0.project_id}" }, { "name", "Bass" } }),
                                            step ("track.list", { { "project_id", "${steps.0.project_id}" } }) }) } };
    REQUIRE (h.tool (s, "recipe.save", { { "recipe", recipe } })["ok"] == true);

    auto started = h.tool (s, "recipe.run", { { "recipe_id", "rec-async-ok" }, { "async", true } });
    REQUIRE (started["ok"] == true);
    REQUIRE (started["data"]["job_id"].is_string());

    auto job = h.waitJob (s, started["data"]["job_id"]);
    CHECK (job["kind"] == "recipe");
    CHECK (job["state"] == "succeeded");
    CHECK (job["progress"].get<double>() == 1.0);
    CHECK (job["result"]["ok"] == true);
    CHECK (job["result"]["steps"].size() == 3);
    CHECK (job["result"]["steps"][2]["result"]["tracks"].size() == 1);

    // the steps ran as recipe:<id>
    const std::string project = job["result"]["project_id"];
    auto activity = h.tool (s, "project.activity", { { "project_id", project } });
    REQUIRE (activity["ok"] == true);
    CHECK (activity["data"].dump().find ("recipe:rec-async-ok") != std::string::npos);

    CHECK (h.tool (s, "project.delete", { { "project_id", project } })["ok"] == true);
    CHECK (h.tool (s, "recipe.delete", { { "recipe_id", "rec-async-ok" } })["ok"] == true);
}

ROVOX_TEST (recipe, async_run_refuses_bad_parameters_before_a_job_exists)
{
    Harness h;
    auto s = h.session();
    json recipe { { "id", "rec-async-params" }, { "name", "Params" },
                  { "params", { { "type", "object" }, { "required", { "name" } }, { "properties", { { "name", { { "type", "string" } } } } } } },
                  { "steps", json::array ({ step ("project.list") }) } };
    REQUIRE (h.tool (s, "recipe.save", { { "recipe", recipe } })["ok"] == true);

    auto r = h.tool (s, "recipe.run", { { "recipe_id", "rec-async-params" }, { "async", true } });
    CHECK (r["ok"] == false);
    CHECK (r["error"]["code"] == "INVALID_PARAMS");
    CHECK (h.tool (s, "recipe.run", { { "recipe_id", "rec-async-params" }, { "async", true }, { "project_id", "p_00000000000000000000000000" }, { "params", { { "name", "x" } } } })["error"]["code"] == "NOT_FOUND");
    CHECK (h.tool (s, "recipe.delete", { { "recipe_id", "rec-async-params" } })["ok"] == true);
}

ROVOX_TEST (recipe, async_cancel_stops_between_steps_and_removes_the_ephemeral_project)
{
    Harness h;
    auto s = h.session();

    // an ephemeral project, then so many cheap steps that the job is still running when the cancel arrives
    json steps = json::array ({ step ("project.new", { { "name", "rec-cancel-project" }, { "ephemeral", true }, { "ttl_s", 3600 } }) });

    for (int i = 0; i < 400; ++i)
        steps.push_back (step ("project.list"));

    json recipe { { "id", "rec-cancel" }, { "name", "Cancel" }, { "steps", steps } };
    REQUIRE (h.tool (s, "recipe.save", { { "recipe", recipe } })["ok"] == true);

    auto started = h.tool (s, "recipe.run", { { "recipe_id", "rec-cancel" }, { "async", true } });
    REQUIRE (started["ok"] == true);
    const std::string jobId = started["data"]["job_id"];

    auto cancel = h.tool (s, "job.cancel", { { "job_id", jobId } });
    REQUIRE (cancel["ok"] == true);
    CHECK (cancel["data"]["cancel_requested"] == true);

    auto job = h.waitJob (s, jobId);
    CHECK (job["state"] == "cancelled");
    CHECK (job["error"]["code"] == "CANCELLED");
    CHECK (! hasProject (h, s, "rec-cancel-project"));
    CHECK (h.tool (s, "recipe.delete", { { "recipe_id", "rec-cancel" } })["ok"] == true);
}

ROVOX_TEST (recipe, steps_never_have_more_rights_than_the_caller)
{
    const std::string tenant = "rec_scope";
    Harness h;
    Principal limited;
    limited.tenant = tenant;
    limited.scopes = ScopeSet::of ({ Scope::render, Scope::projectsWrite });      // may run recipes, may not read projects
    auto s = h.session (limited);

    // project.list needs projects:read, which this caller lacks: the step is refused even though recipe.run itself is allowed
    json recipe { { "id", "rec-scope" }, { "name", "Scope" }, { "steps", json::array ({ step ("project.list") }) } };
    REQUIRE (h.tool (s, "recipe.save", { { "recipe", recipe } })["ok"] == true);

    auto r = h.tool (s, "recipe.run", { { "recipe_id", "rec-scope" } });
    CHECK (r["ok"] == false);
    CHECK (r["error"]["code"] == "FORBIDDEN_SCOPE");
    CHECK (r["error"]["details"]["step"] == 0);

    std::error_code ec;
    fs::remove_all (test::host().dataDir() / "tenants" / tenant, ec);
}

ROVOX_TEST (recipe, accompaniment_is_listed_with_its_params_schema)
{
    Harness h;
    auto s = h.session();
    auto got = h.tool (s, "recipe.get", { { "recipe_id", "accompaniment.v1" } });
    REQUIRE (got["ok"] == true);
    CHECK (got["data"]["recipe"]["procedural"] == true);
    CHECK (got["data"]["recipe"]["params"]["required"].dump().find ("parts") != std::string::npos);
    CHECK (got["data"]["recipe"]["params"]["properties"]["click"]["enum"].size() == 2);
}

ROVOX_TEST (recipe, accompaniment_without_chords_is_refused_before_anything_is_made)
{
    Harness h;
    auto s = h.session();
    const std::string name = "rec-acc-empty";

    for (auto parts : { json::object(), json { { "chords", { { "notes", json::array() } } } }, json { { "melody", waltzParts()["melody"] } } })
    {
        auto r = h.tool (s, "recipe.run", { { "recipe_id", "accompaniment.v1" }, { "params", { { "name", name }, { "bpm", 90 }, { "time_signature", { { "numerator", 3 }, { "denominator", 4 } } }, { "parts", parts } } } });
        CHECK (r["ok"] == false);
        CHECK (r["error"]["code"] == "INVALID_PARAMS");
    }

    CHECK (! hasProject (h, s, "accompaniment " + name));
    auto async = h.tool (s, "recipe.run", { { "recipe_id", "accompaniment.v1" }, { "async", true }, { "params", { { "bpm", 90 }, { "parts", json::object() } } } });
    CHECK (async["ok"] == false);
    CHECK (async["error"]["code"] == "INVALID_PARAMS");
}

ROVOX_TEST (recipe, accompaniment_makes_the_files_and_the_manifest_for_every_tempo_variant)
{
    if (! haveSoundfont())
        return;

    Harness h;
    auto s = h.session();
    auto r = h.tool (s, "recipe.run", { { "recipe_id", "accompaniment.v1" },
                                        { "params", { { "name", "rec-acc-files" }, { "key", "A minor" }, { "bpm", 90 }, { "time_signature", { { "numerator", 3 }, { "denominator", 4 } } },
                                                      { "tempo_variants_pct", { 100, 80 } }, { "parts", waltzParts() },
                                                      { "outputs", { "minus_wav", "plus_wav", "click_wav", "midi" } } } } });
    REQUIRE (r["ok"] == true);
    const auto manifest = r["data"]["result"];

    // one bar of 3/4 at 90 bpm: 3 x 60000 / 90 = 2000 ms; at 80 % (72 bpm) 2500 ms
    CHECK (std::abs (manifest["intro_offset_ms"].get<double>() - 2000.0) <= 1.0);
    REQUIRE (manifest["variants"].size() == 2);
    CHECK (std::abs (manifest["variants"][0]["intro_offset_ms"].get<double>() - 2000.0) <= 1.0);
    CHECK (std::abs (manifest["variants"][1]["intro_offset_ms"].get<double>() - 2500.0) <= 1.0);
    CHECK (manifest["bars"] == 8);
    CHECK (manifest["template"]["id"] == "flute-piano");
    CHECK (manifest["instrument_packs"].size() >= 1);
    CHECK (manifest["attributions"].size() == manifest["instrument_packs"].size());

    // the beat grid: the count-in bar plus 8 bars, the first beat of the music right at the intro offset
    const auto beats = manifest["variants"][0]["beats"];
    REQUIRE (beats.size() == 27);
    CHECK (beats[0]["t_ms"].get<double>() == 0.0);
    CHECK (beats[0]["count_in"] == true);
    CHECK (beats[3]["count_in"] == false);
    CHECK (std::abs (beats[3]["t_ms"].get<double>() - 2000.0) <= 1.0);
    CHECK (std::abs (manifest["variants"][1]["beats"][3]["t_ms"].get<double>() - 2500.0) <= 1.0);

    for (auto& v : manifest["variants"])
        for (auto* kind : { "minus_wav", "plus_wav", "click_wav" })
        {
            REQUIRE (v["files"].contains (kind));
            CHECK (v["files"][kind]["url"].get<std::string>().size() > 0);
            CHECK (v["files"][kind]["sha256"].get<std::string>().size() == 64);
            CHECK (! v["files"][kind].contains ("abs_path"));
        }

    // 9 bars of 2 s (100 %) at least: the count-in is silence, not trimmed
    CHECK (manifest["variants"][0]["files"]["minus_wav"]["duration_s"].get<double>() >= 18.0);
    CHECK (manifest["variants"][1]["files"]["minus_wav"]["duration_s"].get<double>() >= 22.5);
    CHECK (manifest["midi"].is_object());

    // the steps ran as the recipe, and the project is ephemeral: it goes away by itself after ttl_s
    const std::string project = manifest["ephemeral_project_id"];
    auto listed = h.tool (s, "project.list", { { "include_ephemeral", true } });
    bool ephemeral = false;

    for (auto& p : listed["data"]["projects"])
        if (p.value ("project_id", "") == project)
            ephemeral = p.value ("ephemeral", false);

    CHECK (ephemeral);
    CHECK (h.tool (s, "project.activity", { { "project_id", project } })["data"].dump().find ("recipe:accompaniment.v1") != std::string::npos);
    CHECK (h.tool (s, "project.delete", { { "project_id", project } })["ok"] == true);
}

ROVOX_TEST (recipe, accompaniment_two_runs_give_identical_wav_files)
{
    if (! haveSoundfont())
        return;

    Harness h;
    auto s = h.session();
    std::string sha[2], clickSha[2];

    for (int i = 0; i < 2; ++i)
    {
        auto r = h.tool (s, "recipe.run", { { "recipe_id", "accompaniment.v1" },
                                            { "params", { { "name", "rec-acc-same" }, { "bpm", 90 }, { "time_signature", { { "numerator", 3 }, { "denominator", 4 } } }, { "seed", 7 },
                                                          { "parts", waltzParts() }, { "outputs", { "minus_wav", "click_wav" } } } } });
        REQUIRE (r["ok"] == true);
        const auto files = r["data"]["result"]["variants"][0]["files"];
        sha[i] = files["minus_wav"]["sha256"];
        clickSha[i] = files["click_wav"]["sha256"];
        CHECK (sha[i].size() == 64);
        CHECK (h.tool (s, "project.delete", { { "project_id", r["data"]["result"]["ephemeral_project_id"] } })["ok"] == true);
    }

    CHECK (sha[0] == sha[1]);
    CHECK (clickSha[0] == clickSha[1]);
}

ROVOX_TEST (recipe, accompaniment_embedded_click_makes_no_click_file)
{
    if (! haveSoundfont())
        return;

    Harness h;
    auto s = h.session();
    auto r = h.tool (s, "recipe.run", { { "recipe_id", "accompaniment.v1" },
                                        { "params", { { "name", "rec-acc-embedded" }, { "bpm", 90 }, { "time_signature", { { "numerator", 3 }, { "denominator", 4 } } }, { "click", "embedded" },
                                                      { "count_in_bars", 2 }, { "parts", waltzParts() }, { "outputs", { "minus_wav", "click_wav" } } } } });
    REQUIRE (r["ok"] == true);
    const auto manifest = r["data"]["result"];
    CHECK (manifest["click"] == "embedded");
    CHECK (std::abs (manifest["intro_offset_ms"].get<double>() - 4000.0) <= 1.0);
    CHECK (manifest["variants"][0]["files"].contains ("minus_wav"));
    CHECK (! manifest["variants"][0]["files"].contains ("click_wav"));
    CHECK (h.tool (s, "project.delete", { { "project_id", manifest["ephemeral_project_id"] } })["ok"] == true);
}

ROVOX_TEST (recipe, accompaniment_cancel_mid_run_removes_the_ephemeral_project)
{
    if (! haveSoundfont())
        return;

    Harness h;
    auto s = h.session();
    auto started = h.tool (s, "recipe.run", { { "recipe_id", "accompaniment.v1" }, { "async", true },
                                              { "params", { { "name", "rec-acc-cancel" }, { "bpm", 90 }, { "time_signature", { { "numerator", 3 }, { "denominator", 4 } } },
                                                            { "tempo_variants_pct", { 100, 90, 80 } }, { "parts", waltzParts() }, { "outputs", { "minus_wav", "plus_wav", "click_wav" } } } } });
    REQUIRE (started["ok"] == true);
    const std::string jobId = started["data"]["job_id"];
    CHECK (h.tool (s, "job.cancel", { { "job_id", jobId } })["ok"] == true);

    auto job = h.waitJob (s, jobId, 120);
    CHECK (job["state"] == "cancelled");
    CHECK (job["error"]["code"] == "CANCELLED");
    CHECK (! hasProject (h, s, "accompaniment rec-acc-cancel"));
}
