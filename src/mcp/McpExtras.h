// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// recipe.* (task 600/660, MVP): a recipe is a named list of tool calls with ${params.x} / ${steps.N.path} substitution, stored as JSON
// under <data>/recipes, or <data>/tenants/<t>/recipes for another tenant (plus built-in ones, visible to all). Running it calls the same tools an agent would, so every step is validated, logged with the
// caller's origin and, for project commands, part of ONE undo step.
#pragma once

#include "Mcp.h"

namespace rovox::mcp
{

json builtinRecipes();
/** Where a tenant's saved recipes live (as Templates: `local` keeps <data>/recipes). */
fs::path recipesDirOf (const fs::path& dataDir, const std::string& tenant);
std::optional<json> loadRecipe (const fs::path& recipesDir, const std::string& id);
json listRecipes (const fs::path& recipesDir);
Result<json> saveRecipe (const fs::path& recipesDir, json recipe);
Result<void> deleteRecipe (const fs::path& recipesDir, const std::string& id);

/** Replaces "${params.x}" / "${steps.N.a.b}" in strings (a string that is exactly one reference keeps the value's type). */
json substitute (const json& value, const json& params, const json& stepResults);

/** How many recipes are running on this thread (a recipe step that is itself a recipe.run runs synchronously). */
int recipeDepth();

/** The recipe's defaults, the caller's params and project_id merged, then checked against the recipe's params schema. */
Result<json> resolveRecipeParams (const json& recipe, const json& callerParams, const std::string& projectId);

/** Runs the tool calls of one recipe run. Every step is an ordinary tool call, validated and scope-checked like the caller's own, with origin
    `recipe:<id>` and the caller's tenant and principal (a step never has more rights than the caller). Between steps it checks for cancellation
    (the JobContext of an async run, else the request's flag) and reports progress `step i/N`. Failures carry details {step, code, report}. */
class RecipeRunner
{
public:
    /** `job`: the JobContext of an async run (progress and cancel go through it); null for a synchronous run. */
    RecipeRunner (ToolCall& caller, std::string recipeId, std::string projectId, JobContext* job = nullptr);

    /** How many steps there will be (progress denominator); a JSON recipe knows it up front, a procedural one calls this once it does. */
    void setTotal (size_t steps)                { total_ = steps; }
    /** Runs one tool. The Result holds the tool's data. A cancelled run removes what it created (cleanup()) and fails with CANCELLED. */
    Result<json> step (const std::string& tool, json args);
    /** A file the recipe wrote that must disappear if the run is cancelled. */
    void trackFile (fs::path file);
    /** Deletes the ephemeral projects (project.new with ephemeral:true) and the tracked files of this run. Idempotent. */
    void cleanup();
    bool cancelled() const;

    const json& results() const                 { return results_; }        // data of every finished step, for ${steps.N...}
    const json& report() const                  { return report_; }
    const std::string& projectId() const        { return lastProject_; }    // the project of the latest step result, else the caller's
    std::optional<std::uint64_t> revision() const   { return revision_; }

private:
    Result<json> fail (Error, size_t index, const std::string& tool);
    void progress (size_t index, const std::string& tool);

    McpServer& server_;
    std::string recipeId_, callerProject_, lastProject_;
    JobContext* job_;
    Session session_;                           // same id and principal as the caller, labelled `recipe:<id>`
    RequestContext rc_;
    size_t total_ = 0;
    json results_ = json::array(), report_ = json::array();
    std::optional<std::uint64_t> revision_;
    bool coalesce_ = false, cleaned_ = false;
    std::vector<std::string> ephemeralProjects_;
    std::vector<fs::path> files_;
};

/** Runs a recipe (JSON steps, or a procedural built-in) and stops at the first failing step. `params` are the caller's recipe parameters.
    `job`: set by an async run (see recipe.run). */
ToolOutput runRecipe (ToolCall&, const json& recipe, const json& params, const std::string& projectId, JobContext* job = nullptr);

} // namespace rovox::mcp
