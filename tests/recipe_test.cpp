// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E8-01: recipe.run as a job (progress `step i/N`, cancel between steps, ephemeral projects removed on cancel), procedural built-in
// recipes in the same registry as the JSON ones, errors that carry {step, code}, and steps that never have more rights than the caller.
// Tenant ids are unique to this file (`rec_*`) because every test of the process shares one data directory.
#include "Auth.h"
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
