// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// recipe.* (task 600/660, MVP): a recipe is a named list of tool calls with ${params.x} / ${steps.N.path} substitution, stored as JSON
// under <data>/recipes (plus built-in ones). Running it calls the same tools an agent would, so every step is validated, logged with the
// caller's origin and, for project commands, part of ONE undo step.
#pragma once

#include "Mcp.h"

namespace rovox::mcp
{

json builtinRecipes();
std::optional<json> loadRecipe (const fs::path& dataDir, const std::string& id);
json listRecipes (const fs::path& dataDir);
Result<json> saveRecipe (const fs::path& dataDir, json recipe);
Result<void> deleteRecipe (const fs::path& dataDir, const std::string& id);

/** Replaces "${params.x}" / "${steps.N.a.b}" in strings (a string that is exactly one reference keeps the value's type). */
json substitute (const json& value, const json& params, const json& stepResults);

/** Runs the steps in order; stops at the first failing one. `args` carries project_id and the recipe parameters. */
ToolOutput runRecipe (ToolCall&, const json& recipe, const json& params, const std::string& projectId);

} // namespace rovox::mcp
