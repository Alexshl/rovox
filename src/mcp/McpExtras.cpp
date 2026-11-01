// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "McpExtras.h"

#include "JsonSchema.h"
#include "ProjectFormat.h"

#include <fstream>

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
    return json::array ({
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
}

std::optional<json> loadRecipe (const fs::path& dataDir, const std::string& id)
{
    for (auto& r : builtinRecipes())
        if (r["id"] == id)
            return r;

    if (! validRecipeId (id))
        return std::nullopt;

    std::ifstream in (dataDir / "recipes" / (id + ".json"));
    auto doc = json::parse (in, nullptr, false);
    return doc.is_object() ? std::optional<json> (doc) : std::nullopt;
}

json listRecipes (const fs::path& dataDir)
{
    json out = json::array();

    auto add = [&] (const json& r)
    {
        out.push_back ({ { "recipe_id", r["id"] }, { "name", r.value ("name", "") }, { "description", r.value ("description", "") }, { "builtin", r.value ("builtin", false) },
                         { "params", r.value ("params", json::object()) }, { "steps", r.value ("steps", json::array()).size() } });
    };

    for (auto& r : builtinRecipes())
        add (r);

    std::error_code ec;

    for (auto& f : fs::directory_iterator (dataDir / "recipes", ec))
        if (f.path().extension() == ".json")
            if (auto r = loadRecipe (dataDir, f.path().stem().string()))
                add (*r);

    return out;
}

Result<json> saveRecipe (const fs::path& dataDir, json recipe)
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

        if (tool.rfind ("recipe.", 0) == 0 || tool == "batch")
            return makeError (errc::invalidParams, "a recipe cannot call " + tool, json { { "path", "/recipe/steps/" + std::to_string (i) } });
    }

    recipe.erase ("builtin");
    std::error_code ec;
    fs::create_directories (dataDir / "recipes", ec);

    if (auto w = writeFileAtomic (dataDir / "recipes" / (id + ".json"), recipe.dump (2), false); ! w.ok())
        return w.error();

    return recipe;
}

Result<void> deleteRecipe (const fs::path& dataDir, const std::string& id)
{
    for (auto& r : builtinRecipes())
        if (r["id"] == id)
            return makeError (errc::preconditionFailed, "built-in recipes cannot be deleted");

    std::error_code ec;

    if (! validRecipeId (id) || ! fs::remove (dataDir / "recipes" / (id + ".json"), ec))
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

ToolOutput runRecipe (ToolCall& c, const json& recipe, const json& callerParams, const std::string& projectId)
{
    json params = recipe.value ("defaults", json::object());

    for (auto& [k, v] : callerParams.items())
        params[k] = v;

    if (! projectId.empty())
        params["project_id"] = projectId;

    if (recipe.contains ("params"))
        if (auto v = validateJson (recipe["params"], params); ! v.ok())
            return ToolOutput::failure (makeError (errc::invalidParams, "recipe parameters: " + v.error().message, v.error().details), std::nullopt, projectId);

    json results = json::array(), report = json::array();
    std::string lastProject = projectId;
    std::optional<std::uint64_t> revision;
    bool coalesce = false;

    for (size_t i = 0; i < recipe["steps"].size(); ++i)
    {
        const auto& step = recipe["steps"][i];
        const auto toolName = step["tool"].get<std::string>();
        const auto* tool = c.server.findTool (toolName);

        if (tool == nullptr)
            return ToolOutput::failure (makeError (errc::notFound, "step " + std::to_string (i) + ": no tool " + toolName, json { { "report", report } }), revision, lastProject);

        json args = substitute (step.value ("arguments", json::object()), params, results);

        if (! args.contains ("project_id") && ! projectId.empty() && tool->inputSchema.value ("properties", json::object()).contains ("project_id"))
            args["project_id"] = projectId;

        if (auto v = validateJson (tool->inputSchema, args); ! v.ok())
            return ToolOutput::failure (makeError (errc::invalidParams, "step " + std::to_string (i) + " (" + toolName + "): " + v.error().message,
                                                   json { { "path", v.error().details }, { "report", report } }), revision, lastProject);

        if (coalesce)
            args["coalesce"] = true;        // the steps of a recipe form one undo step

        ToolCall inner { c.server, c.session, c.rc, args, tool->name };
        auto out = tool->handler (inner);
        json entry { { "step", i }, { "tool", toolName }, { "ok", out.ok } };

        if (out.revision)
            revision = out.revision;

        if (! out.projectId.empty())
            lastProject = out.projectId;

        if (! out.ok)
        {
            entry["error"] = out.error ? out.error->toJson() : json (nullptr);
            report.push_back (entry);
            auto error = out.error.value_or (makeError (errc::internal, "step failed"));
            json details = error.details.is_object() ? error.details : json::object();
            details["recipe_step"] = i;
            details["report"] = report;
            error.message = "recipe step " + std::to_string (i) + " (" + toolName + ") failed: " + error.message;
            error.details = details;
            return ToolOutput::failure (error, revision, lastProject);
        }

        entry["result"] = out.data;
        report.push_back (entry);
        results.push_back (out.data);

        if (auto* cmd = c.server.bus().registry().find (tool->name); cmd != nullptr && ! cmd->isGlobal() && cmd->mutates() && ! cmd->manualUndo())
            coalesce = true;
    }

    return ToolOutput::success ({ { "recipe_id", recipe["id"] }, { "ok", true }, { "steps", report }, { "project_id", lastProject } }, revision, lastProject);
}

} // namespace rovox::mcp
