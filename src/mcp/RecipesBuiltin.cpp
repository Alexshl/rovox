// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RecipesBuiltin.h"

#include <cstdio>
#include <optional>

namespace rovox::mcp
{

namespace
{
/** render_mixdown: the master as one file, or one file per loudness target in `variants_lufs` (the name gets the target as a suffix). */
Result<json> renderMixdown (RecipeRunner& run, const json& params)
{
    std::vector<std::optional<double>> targets;

    if (params.contains ("variants_lufs") && params["variants_lufs"].is_array() && ! params["variants_lufs"].empty())
    {
        for (auto& v : params["variants_lufs"])
            targets.push_back (v.get<double>());
    }
    else if (params.contains ("target_lufs") && params["target_lufs"].is_number())
    {
        targets.push_back (params["target_lufs"].get<double>());
    }
    else
    {
        targets.push_back (std::nullopt);
    }

    const bool suffixed = params.contains ("variants_lufs") && params["variants_lufs"].is_array() && ! params["variants_lufs"].empty();
    const std::string name = params.value ("name", "mixdown");
    run.setTotal (targets.size());
    json renders = json::array();

    for (auto target : targets)
    {
        json args { { "project_id", params.value ("project_id", "") }, { "scope", "master" }, { "format", { { "container", params.value ("container", "wav") } } }, { "wait_s", 60 } };
        std::string fileName = name;

        if (target)
        {
            char suffix[32];
            std::snprintf (suffix, sizeof (suffix), "%g", *target);
            args["target_lufs"] = *target;
            args["check"] = true;

            if (suffixed)
                fileName += "_" + std::string (suffix) + "lufs";
        }

        args["name"] = fileName;
        auto r = run.step ("render.start", std::move (args));

        if (! r.ok())
            return r.error();

        // a cancelled run deletes the files written so far
        if (r.value().contains ("result") && r.value()["result"].is_object() && r.value()["result"].contains ("files") && r.value()["result"]["files"].is_array())
            for (auto& f : r.value()["result"]["files"])
                if (f.is_object() && f.contains ("path") && f["path"].is_string())
                    run.trackFile (f["path"].get<std::string>());

        renders.push_back (r.value());
    }

    return json { { "renders", renders } };
}

std::vector<ProceduralRecipe> makeRecipes()
{
    std::vector<ProceduralRecipe> out;

    ProceduralRecipe mixdown;
    mixdown.definition = {
        { "id", "render_mixdown" }, { "builtin", true }, { "procedural", true }, { "name", "Render the master mixdown" },
        { "description", "Renders the master to one file, or to one file per loudness target in variants_lufs (name_<target>lufs). One render step each, with progress; cancelling removes the files already written." },
        { "params", { { "type", "object" }, { "required", { "project_id" } },
                      { "properties", { { "project_id", { { "type", "string" } } }, { "name", { { "type", "string" }, { "default", "mixdown" } } },
                                        { "container", { { "type", "string" }, { "enum", { "mp3", "wav", "flac" } }, { "default", "wav" } } },
                                        { "target_lufs", { { "type", "number" } } },
                                        { "variants_lufs", { { "type", "array" }, { "items", { { "type", "number" } } } } } } } } },
        { "defaults", { { "name", "mixdown" }, { "container", "wav" } } },
        { "steps", json::array() } };
    mixdown.run = renderMixdown;
    out.push_back (std::move (mixdown));
    return out;
}
} // namespace

const std::vector<ProceduralRecipe>& proceduralRecipes()
{
    static const std::vector<ProceduralRecipe> recipes = makeRecipes();
    return recipes;
}

const ProceduralRecipe* findProceduralRecipe (const std::string& id)
{
    for (auto& r : proceduralRecipes())
        if (r.definition["id"] == id)
            return &r;

    return nullptr;
}

} // namespace rovox::mcp
