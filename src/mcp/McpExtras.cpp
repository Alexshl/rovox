// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "McpExtras.h"

#include "JsonSchema.h"
#include "ProjectFormat.h"
#include "RecipesBuiltin.h"
#include "Scopes.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace rovox::mcp
{

namespace
{
bool validRecipeId (const std::string& id)
{
    return ! id.empty() && id.size() <= 64 && std::all_of (id.begin(), id.end(), [] (unsigned char c) { return std::isalnum (c) || c == '-' || c == '_'; });
}

const json* walk (const json& root, const std::string& path)
{
    const json* cur = &root;
    size_t pos = 0;

    while (pos <= path.size())
    {
        const auto dot = path.find ('.', pos);
        const auto key = path.substr (pos, dot == std::string::npos ? std::string::npos : dot - pos);

        if (cur->is_array())
        {
            char* end = nullptr;
            const auto idx = std::strtoul (key.c_str(), &end, 10);

            if (end == key.c_str() || idx >= cur->size())
                return nullptr;

            cur = &(*cur)[idx];
        }
        else if (cur->is_object() && cur->contains (key))
        {
            cur = &(*cur)[key];
        }
        else
        {
            return nullptr;
        }

        if (dot == std::string::npos)
            break;

        pos = dot + 1;
    }

    return cur;
}

const json* lookup (const std::string& ref, const json& params, const json& steps)
{
    if (ref.rfind ("params.", 0) == 0)     return walk (params, ref.substr (7));
    if (ref.rfind ("steps.", 0) == 0)      return walk (steps, ref.substr (6));
    return nullptr;
}
} // namespace

json builtinRecipes()
{
    auto recipes = json::array ({
        { { "id", "render_and_deliver" }, { "builtin", true }, { "name", "Render the mix and deliver it" },
          { "description", "Renders the master to a file at a loudness target (with the render checks on) and sends it with a delivery adapter. Respects delivery_hold." },
          { "params", { { "type", "object" }, { "required", { "project_id", "adapter" } },
                        { "properties", { { "project_id", { { "type", "string" } } }, { "adapter", { { "type", "string" } } }, { "target_lufs", { { "type", "number" }, { "default", -14 } } },
                                          { "container", { { "type", "string" }, { "enum", { "mp3", "wav", "flac" } }, { "default", "mp3" } } }, { "caption", { { "type", "string" } } },
                                          { "name", { { "type", "string" }, { "default", "mix" } } } } } } },
          { "defaults", { { "target_lufs", -14 }, { "container", "mp3" }, { "caption", "" }, { "name", "mix" } } },
          { "steps", json::array ({
              { { "tool", "render.start" }, { "arguments", { { "name", "${params.name}" }, { "scope", "master" }, { "format", { { "container", "${params.container}" } } },
                                                                { "target_lufs", "${params.target_lufs}" }, { "check", true }, { "wait_s", 60 } } } },
              { { "tool", "delivery.send" }, { "arguments", { { "source", { { "job_id", "${steps.0.job_id}" } } }, { "adapter", "${params.adapter}" }, { "caption", "${params.caption}" }, { "wait_s", 60 } } } } }) } },

        { { "id", "band_from_template" }, { "builtin", true }, { "name", "New project from a template" },
          { "description", "Creates a project and applies a project template (see template.list)." },
          { "params", { { "type", "object" }, { "required", { "template_id" } },
                        { "properties", { { "template_id", { { "type", "string" } } }, { "name", { { "type", "string" } } } } } } },
          { "defaults", { { "name", "New song" } } },
          { "steps", json::array ({
              { { "tool", "project.new" }, { "arguments", { { "name", "${params.name}" } } } },
              { { "tool", "template.apply" }, { "arguments", { { "project_id", "${steps.0.project_id}" }, { "template_id", "${params.template_id}" } } } } }) } } });

    // procedural ones (C++, RecipesBuiltin.cpp) are listed next to the JSON ones with their params schema; they have no JSON steps
    for (auto& p : proceduralRecipes())
        recipes.push_back (p.definition);

    return recipes;
}

fs::path recipesDirOf (const fs::path& dataDir, const std::string& tenant)
{
    return tenant == "local" ? dataDir / "recipes" : dataDir / "tenants" / tenant / "recipes";
}

std::optional<json> loadRecipe (const fs::path& recipesDir, const std::string& id)
{
    for (auto& r : builtinRecipes())
        if (r["id"] == id)
            return r;

    if (! validRecipeId (id))
        return std::nullopt;

    std::ifstream in (recipesDir / (id + ".json"));
    auto doc = json::parse (in, nullptr, false);
    return doc.is_object() ? std::optional<json> (doc) : std::nullopt;
}

json listRecipes (const fs::path& recipesDir)
{
    json out = json::array();

    auto add = [&] (const json& r)
    {
        out.push_back ({ { "recipe_id", r["id"] }, { "name", r.value ("name", "") }, { "description", r.value ("description", "") }, { "builtin", r.value ("builtin", false) }, { "procedural", r.value ("procedural", false) },
                         { "params", r.value ("params", json::object()) }, { "steps", r.value ("steps", json::array()).size() } });
    };

    for (auto& r : builtinRecipes())
        add (r);

    std::error_code ec;

    for (auto& f : fs::directory_iterator (recipesDir, ec))
        if (f.path().extension() == ".json")
            if (auto r = loadRecipe (recipesDir, f.path().stem().string()))
                add (*r);

    return out;
}

Result<json> saveRecipe (const fs::path& recipesDir, json recipe)
{
    const auto id = recipe.value ("id", "");

    if (! validRecipeId (id))
        return makeError (errc::invalidParams, "a recipe id is letters, digits, - and _", json { { "path", "/recipe/id" } });

    for (auto& r : builtinRecipes())
        if (r["id"] == id)
            return makeError (errc::preconditionFailed, "that id belongs to a built-in recipe");

    if (! recipe.contains ("steps") || ! recipe["steps"].is_array() || recipe["steps"].empty())
        return makeError (errc::invalidParams, "a recipe needs steps: [{tool, arguments}]", json { { "path", "/recipe/steps" } });

    for (size_t i = 0; i < recipe["steps"].size(); ++i)
    {
        const auto& s = recipe["steps"][i];

        if (! s.is_object() || ! s.contains ("tool") || ! s["tool"].is_string())
            return makeError (errc::invalidParams, "every step needs a tool name", json { { "path", "/recipe/steps/" + std::to_string (i) } });

        const auto tool = s["tool"].get<std::string>();

        // judge the canonical name: the only aliases are the underscore spellings (recipe_run -> recipe.run), so undo that first
        auto canonical = tool;
        std::replace (canonical.begin(), canonical.end(), '_', '.');

        if (canonical.rfind ("recipe.", 0) == 0 || canonical == "batch")
            return makeError (errc::invalidParams, "a recipe cannot call " + tool, json { { "path", "/recipe/steps/" + std::to_string (i) } });
    }

    recipe.erase ("builtin");
    std::error_code ec;
    fs::create_directories (recipesDir, ec);

    if (auto w = writeFileAtomic (recipesDir / (id + ".json"), recipe.dump (2), false); ! w.ok())
        return w.error();

    return recipe;
}

Result<void> deleteRecipe (const fs::path& recipesDir, const std::string& id)
{
    for (auto& r : builtinRecipes())
        if (r["id"] == id)
            return makeError (errc::preconditionFailed, "built-in recipes cannot be deleted");

    std::error_code ec;

    if (! validRecipeId (id) || ! fs::remove (recipesDir / (id + ".json"), ec))
        return makeError (errc::notFound, "no recipe " + id);

    return {};
}

json substitute (const json& v, const json& params, const json& steps)
{
    if (v.is_string())
    {
        const auto s = v.get<std::string>();

        if (s.size() > 3 && s.rfind ("${", 0) == 0 && s.back() == '}' && s.find ("${", 2) == std::string::npos)
        {
            if (auto* found = lookup (s.substr (2, s.size() - 3), params, steps))
                return *found;

            return nullptr;
        }

        std::string out;
        size_t pos = 0;

        for (;;)
        {
            const auto open = s.find ("${", pos);

            if (open == std::string::npos)
            {
                out += s.substr (pos);
                break;
            }

            const auto close = s.find ('}', open);

            if (close == std::string::npos)
            {
                out += s.substr (pos);
                break;
            }

            out += s.substr (pos, open - pos);

            if (auto* found = lookup (s.substr (open + 2, close - open - 2), params, steps))
                out += found->is_string() ? found->get<std::string>() : found->dump();

            pos = close + 1;
        }

        return out;
    }

    if (v.is_array())
    {
        json out = json::array();

        for (auto& e : v)
            out.push_back (substitute (e, params, steps));

        return out;
    }

    if (v.is_object())
    {
        json out = json::object();

        for (auto& [k, e] : v.items())
        {
            auto sub = substitute (e, params, steps);

            if (! sub.is_null())            // an unset optional parameter drops the argument
                out[k] = std::move (sub);
        }

        return out;
    }

    return v;
}

// how many recipes are running on this thread (nested recipe.run calls): bounded by maxRecipeDepth
thread_local int recipesRunningHere = 0;

int recipeDepth()
{
    return recipesRunningHere;
}

Result<json> resolveRecipeParams (const json& recipe, const json& callerParams, const std::string& projectId)
{
    json params = recipe.value ("defaults", json::object());

    for (auto& [k, v] : callerParams.items())
        params[k] = v;

    if (! projectId.empty())
        params["project_id"] = projectId;

    if (recipe.contains ("params"))
        if (auto v = validateJson (recipe["params"], params); ! v.ok())
            return makeError (errc::invalidParams, "recipe parameters: " + v.error().message, v.error().details);

    return params;
}

//==============================================================================
RecipeRunner::RecipeRunner (ToolCall& caller, std::string recipeId, std::string projectId, JobContext* job)
    : server_ (caller.server), recipeId_ (std::move (recipeId)), callerProject_ (projectId), lastProject_ (std::move (projectId)), job_ (job),
      session_ (caller.session.id, caller.session.shortId, caller.session.principal)
{
    // Session::origin() takes its label from clientInfo.name: the journal and the web UI show the steps as `recipe:<id>`
    session_.clientInfo = { { "name", "recipe:" + recipeId_ } };
    session_.initialized = true;
    rc_.session = &session_;
    rc_.emit = caller.rc.emit;
    rc_.progressToken = caller.rc.progressToken;

    if (job_ == nullptr)
        rc_.cancelled = caller.rc.cancelled;        // a synchronous run stops when the client cancels the request
}

bool RecipeRunner::cancelled() const
{
    return (job_ != nullptr && job_->cancelled()) || rc_.cancelled->load() || server_.stoppingFlag().load();
}

void RecipeRunner::progress (size_t index, const std::string& tool)
{
    if (job_ == nullptr)
        return;

    const auto total = std::max (total_, index + 1);
    job_->progress ((double) index / (double) total, "step " + std::to_string (index + 1) + "/" + std::to_string (total) + ": " + tool);
}

void RecipeRunner::trackFile (fs::path file)
{
    files_.push_back (std::move (file));
}

void RecipeRunner::cleanup()
{
    if (std::exchange (cleaned_, true))
        return;

    if (const auto* del = server_.findTool ("project.delete"); del != nullptr && ! missingScope (session_.principal, del->name))
        for (auto it = ephemeralProjects_.rbegin(); it != ephemeralProjects_.rend(); ++it)
        {
            try
            {
                ToolCall call { server_, session_, rc_, json { { "project_id", *it } }, del->name };
                del->handler (call);
            }
            catch (const std::exception&)
            {
            }
        }

    std::error_code ec;

    for (auto& f : files_)
        fs::remove (f, ec);
}

Result<json> RecipeRunner::fail (Error error, size_t index, const std::string& tool)
{
    report_.push_back ({ { "step", index }, { "tool", tool }, { "ok", false }, { "error", error.toJson() } });

    json details = error.details.is_object() ? error.details : json::object();
    details["step"] = index;
    details["code"] = error.code;
    details["recipe_step"] = index;
    details["report"] = report_;
    error.message = "recipe step " + std::to_string (index) + " (" + tool + ") failed: " + error.message;
    error.details = details;
    return error;
}

Result<json> RecipeRunner::step (const std::string& toolName, json args)
{
    const size_t index = report_.size();

    if (cancelled())
    {
        cleanup();
        return fail (makeError (errc::cancelled, "the recipe was cancelled"), index, toolName);
    }

    progress (index, toolName);
    const auto* tool = server_.findTool (toolName);

    if (tool == nullptr)
        return fail (makeError (errc::notFound, "no tool " + toolName), index, toolName);

    // the caller's rights, not more: scopes are checked for every step (the CommandBus checks commands again)
    if (auto missing = missingScope (session_.principal, tool->name))
        return fail (forbiddenScopeError (*missing, tool->name), index, toolName);

    if (! args.contains ("project_id") && ! callerProject_.empty() && tool->inputSchema.value ("properties", json::object()).contains ("project_id"))
        args["project_id"] = callerProject_;

    if (auto v = validateJson (tool->inputSchema, args); ! v.ok())
        return fail (makeError (errc::invalidParams, v.error().message, json { { "path", v.error().details } }), index, toolName);

    const bool opensEphemeral = tool->name == "project.new" && args.value ("ephemeral", false);

    if (coalesce_)
        args["coalesce"] = true;            // the steps of a recipe form one undo step

    ToolOutput out;

    try
    {
        ToolCall inner { server_, session_, rc_, args, tool->name };
        out = tool->handler (inner);
    }
    catch (const std::exception& e)
    {
        out = ToolOutput::failure (makeError (errc::internal, tool->name + ": " + e.what()));
    }

    if (out.revision)
        revision_ = out.revision;

    if (! out.projectId.empty())
        lastProject_ = out.projectId;

    if (! out.ok)
        return fail (out.error.value_or (makeError (errc::internal, "step failed")), index, toolName);

    if (opensEphemeral && out.data.is_object() && out.data.contains ("project_id") && out.data["project_id"].is_string())
        ephemeralProjects_.push_back (out.data["project_id"].get<std::string>());

    report_.push_back ({ { "step", index }, { "tool", toolName }, { "ok", true }, { "result", out.data } });
    results_.push_back (out.data);

    if (auto* cmd = server_.bus().registry().find (tool->name); cmd != nullptr && ! cmd->isGlobal() && cmd->mutates() && ! cmd->manualUndo())
        coalesce_ = true;

    return out.data;
}

//==============================================================================
ToolOutput runRecipe (ToolCall& c, const json& recipe, const json& callerParams, const std::string& projectId, JobContext* job)
{
    constexpr int maxRecipeDepth = 4;

    struct DepthGuard
    {
        DepthGuard()  { ++recipesRunningHere; }
        ~DepthGuard() { --recipesRunningHere; }
    } depthGuard;

    if (recipesRunningHere > maxRecipeDepth)
        return ToolOutput::failure (makeError (errc::preconditionFailed, "recipes are nested too deeply (limit " + std::to_string (maxRecipeDepth) + ")"), std::nullopt, projectId);

    auto resolved = resolveRecipeParams (recipe, callerParams, projectId);

    if (! resolved.ok())
        return ToolOutput::failure (resolved.error(), std::nullopt, projectId);

    const json& params = resolved.value();
    const std::string id = recipe.value ("id", "");
    RecipeRunner runner (c, id, projectId, job);
    json result = nullptr;

    if (recipe.value ("procedural", false))
    {
        const auto* procedural = findProceduralRecipe (id);

        if (procedural == nullptr)
            return ToolOutput::failure (makeError (errc::notFound, "no procedural recipe " + id), std::nullopt, projectId);

        auto r = procedural->run (runner, params);

        if (! r.ok())
            return ToolOutput::failure (r.error(), runner.revision(), runner.projectId());

        result = std::move (r.value());
    }
    else
    {
        runner.setTotal (recipe["steps"].size());

        for (auto& step : recipe["steps"])
        {
            auto r = runner.step (step["tool"].get<std::string>(), substitute (step.value ("arguments", json::object()), params, runner.results()));

            if (! r.ok())
                return ToolOutput::failure (r.error(), runner.revision(), runner.projectId());
        }
    }

    json data { { "recipe_id", recipe["id"] }, { "ok", true }, { "steps", runner.report() }, { "project_id", runner.projectId() } };

    if (! result.is_null())
        data["result"] = std::move (result);

    return ToolOutput::success (std::move (data), runner.revision(), runner.projectId());
}

} // namespace rovox::mcp
