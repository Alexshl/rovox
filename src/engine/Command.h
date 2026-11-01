// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "ChangeCollector.h"
#include "Origin.h"
#include "Project.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

namespace rovox
{

class ProjectManager;

struct CommandContext
{
    std::string projectId;                      // ignored by global commands
    std::string tenant = "local";               // comes from the authenticated session, never from tool params
    Origin origin = Origin::cli();
    std::optional<std::uint64_t> ifRevision;    // optimistic concurrency: REVISION_CONFLICT if the project moved on
    std::string undoLabel;                      // empty = generated from the command name
    bool dryRun = false;                        // validate only, apply nothing
};

struct CommandResult
{
    bool ok = false;
    json data = json::object();
    std::optional<Error> error;
    std::uint64_t revision = 0;
    std::vector<ChangedEntity> changes;

    json toJson() const;
    static CommandResult fail (Error e, std::uint64_t revision = 0);
};

/** A named, schema-described operation. The JSON Schema is the single source for MCP inputSchema, WS and validation. */
class Command
{
public:
    virtual ~Command() = default;
    virtual std::string_view name() const = 0;
    virtual const json& paramsSchema() const = 0;
    virtual const json& resultSchema() const = 0;
    virtual bool mutates() const { return true; }
    /** undo / redo drive the UndoManager themselves instead of running inside a fresh transaction. */
    virtual bool manualUndo() const { return false; }
    /** Global commands (project.new, project.list ...) work on the ProjectManager, not on one project. */
    virtual bool isGlobal() const { return false; }

    /** Runs on the message thread inside an open undo transaction and change scope. */
    virtual Result<json> execute (Project&, const json& params, const CommandContext&) { return makeError (errc::internal, "not a project command"); }
    virtual Result<json> executeGlobal (ProjectManager&, const json& params, const CommandContext&) { return makeError (errc::internal, "not a global command"); }
};

using ProjectCommandFn = std::function<Result<json> (Project&, const json&, const CommandContext&)>;
using GlobalCommandFn = std::function<Result<json> (ProjectManager&, const json&, const CommandContext&)>;

std::unique_ptr<Command> makeProjectCommand (std::string name, json paramsSchema, json resultSchema, bool mutates, ProjectCommandFn fn, bool manualUndo = false);
std::unique_ptr<Command> makeGlobalCommand (std::string name, json paramsSchema, json resultSchema, bool mutates, GlobalCommandFn fn);

class CommandRegistry
{
public:
    void add (std::unique_ptr<Command>);
    Command* find (const std::string& name) const;
    std::vector<Command*> all() const;

private:
    std::map<std::string, std::unique_ptr<Command>> commands_;
};

/** project.*, track.*, fx.*, marker.*, undo / redo / history.list, import.* */
void registerBuiltinCommands (CommandRegistry&);

} // namespace rovox
