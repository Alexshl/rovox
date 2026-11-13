// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Procedural built-in recipes: recipes written in C++ for what the JSON step list cannot say (loops, arithmetic). They sit in the same
// registry as the JSON recipes (McpExtras: builtinRecipes / loadRecipe / listRecipes), so recipe.list and recipe.get show their `params`
// schema and recipe.run runs them through the same path. Every step is still an ordinary tool call made through RecipeRunner::step, which
// gives it origin `recipe:<id>`, the caller's scopes and tenant, progress and cancellation.
#pragma once

#include "McpExtras.h"

namespace rovox::mcp
{

struct ProceduralRecipe
{
    /** What recipe.list / recipe.get show: {id, name, description, params (JSON schema), defaults, builtin:true, procedural:true, steps:[]}. */
    json definition;
    /** `params` are the defaults merged with the caller's values and already checked against definition["params"]. The result goes to
        recipe.run's `result`. A failing runner.step() Error is returned as is. */
    std::function<Result<json> (RecipeRunner&, const json& params)> run;
};

const std::vector<ProceduralRecipe>& proceduralRecipes();
const ProceduralRecipe* findProceduralRecipe (const std::string& id);

} // namespace rovox::mcp
