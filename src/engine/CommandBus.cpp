// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "CommandBus.h"

#include "JsonSchema.h"
#include "ProjectManager.h"

#include <chrono>

namespace rovox
{

json CommandResult::toJson() const
{
    json changesJson = json::array();

    for (auto& c : changes)
        changesJson.push_back (c.toJson());

    json j { { "ok", ok }, { "revision", revision } };

    if (ok)
    {
        j["data"] = data;
        j["changes"] = changesJson;
    }
    else if (error)
    {
        j["error"] = error->toJson();
    }

    return j;
}

CommandResult CommandResult::fail (Error e, std::uint64_t revision)
{
    CommandResult r;
    r.error = std::move (e);
    r.revision = revision;
    return r;
}

//==============================================================================
namespace
{
class LambdaCommand : public Command
{
public:
    LambdaCommand (std::string n, json params, json result, bool m, ProjectCommandFn p, GlobalCommandFn g, bool manual)
        : name_ (std::move (n)), params_ (std::move (params)), result_ (std::move (result)), mutates_ (m), manual_ (manual),
          project_ (std::move (p)), global_ (std::move (g)) {}

    std::string_view name() const override              { return name_; }
    const json& paramsSchema() const override           { return params_; }
    const json& resultSchema() const override           { return result_; }
    bool mutates() const override                       { return mutates_; }
    bool manualUndo() const override                    { return manual_; }
    bool isGlobal() const override                      { return global_ != nullptr; }

    Result<json> execute (Project& p, const json& params, const CommandContext& c) override
    {
        return project_ ? project_ (p, params, c) : Command::execute (p, params, c);
    }

    Result<json> executeGlobal (ProjectManager& pm, const json& params, const CommandContext& c) override
    {
        return global_ ? global_ (pm, params, c) : Command::executeGlobal (pm, params, c);
    }

private:
    std::string name_;
    json params_, result_;
    bool mutates_, manual_;
    ProjectCommandFn project_;
    GlobalCommandFn global_;
};
} // namespace

std::unique_ptr<Command> makeProjectCommand (std::string name, json paramsSchema, json resultSchema, bool mutates, ProjectCommandFn fn, bool manualUndo)
{
    return std::make_unique<LambdaCommand> (std::move (name), std::move (paramsSchema), std::move (resultSchema), mutates, std::move (fn), nullptr, manualUndo);
}

std::unique_ptr<Command> makeGlobalCommand (std::string name, json paramsSchema, json resultSchema, bool mutates, GlobalCommandFn fn)
{
    return std::make_unique<LambdaCommand> (std::move (name), std::move (paramsSchema), std::move (resultSchema), mutates, nullptr, std::move (fn), false);
}

void CommandRegistry::add (std::unique_ptr<Command> c)
{
    auto key = std::string (c->name());
    commands_[key] = std::move (c);
}

Command* CommandRegistry::find (const std::string& name) const
{
    auto it = commands_.find (name);
    return it == commands_.end() ? nullptr : it->second.get();
}

std::vector<Command*> CommandRegistry::all() const
{
    std::vector<Command*> out;

    for (auto& [k, c] : commands_)
        out.push_back (c.get());

    return out;
}

//==============================================================================
CommandBus::CommandBus (ProjectManager& p, CommandRegistry& r, MessageThreadExecutor& e) : projects_ (p), registry_ (r), executor_ (e) {}

std::future<CommandResult> CommandBus::submit (std::string name, json params, CommandContext ctx)
{
    return executor_.run ([this, name = std::move (name), params = std::move (params), ctx = std::move (ctx)]
    {
        return runNow (name, params, ctx);
    });
}

std::future<CommandResult> CommandBus::submitBatch (std::vector<Op> ops, CommandContext ctx)
{
    return executor_.run ([this, ops = std::move (ops), ctx = std::move (ctx)]
    {
        return runBatchNow (ops, ctx);
    });
}

std::string CommandBus::labelFor (const std::vector<Op>& ops, const CommandContext& ctx)
{
    std::string what = ctx.undoLabel;

    if (what.empty())
    {
        if (ops.size() == 1)
        {
            what = ops[0].name;

            if (ops[0].params.is_object() && ops[0].params.contains ("name") && ops[0].params["name"].is_string())
                what += " " + ops[0].params["name"].get<std::string>();
        }
        else
        {
            what = "batch (" + std::to_string (ops.size()) + " ops)";
        }
    }

    return (ctx.origin.label.empty() ? std::string (Origin::kindName (ctx.origin.kind)) : ctx.origin.label) + ": " + what;
}

CommandResult CommandBus::runNow (const std::string& name, const json& params, const CommandContext& ctx)
{
    auto* cmd = registry_.find(name);

    if (cmd == nullptr)
        return CommandResult::fail (makeError (errc::notFound, "unknown command " + name));

    if (cmd->isGlobal())
    {
        if (auto v = validateJson (cmd->paramsSchema(), params); ! v.ok())
            return CommandResult::fail (v.error());

        if (ctx.dryRun)
        {
            CommandResult r;
            r.ok = true;
            r.data = { { "dry_run", true }, { "valid", true } };
            return r;
        }

        try
        {
            auto out = cmd->executeGlobal (projects_, params, ctx);
            CommandResult r;
            r.ok = out.ok();

            if (out.ok())
                r.data = std::move (out.value());
            else
                r.error = out.error();

            return r;
        }
        catch (const std::exception& e)
        {
            return CommandResult::fail (makeError (errc::internal, e.what()));
        }
    }

    auto* project = projects_.find (ctx.projectId, ctx.tenant);

    if (project == nullptr)
        return CommandResult::fail (makeError (errc::notFound, "no open project " + ctx.projectId));

    return runInProject (*project, { Op { name, params } }, ctx, false);
}

CommandResult CommandBus::runBatchNow (const std::vector<Op>& ops, const CommandContext& ctx)
{
    auto* project = projects_.find (ctx.projectId, ctx.tenant);

    if (project == nullptr)
        return CommandResult::fail (makeError (errc::notFound, "no open project " + ctx.projectId));

    if (ops.empty())
        return CommandResult::fail (makeError (errc::invalidParams, "batch is empty"), project->revision());

    for (auto& op : ops)
    {
        auto* cmd = registry_.find (op.name);

        if (cmd == nullptr)
            return CommandResult::fail (makeError (errc::notFound, "unknown command " + op.name), project->revision());

        if (cmd->isGlobal() || cmd->manualUndo())
            return CommandResult::fail (makeError (errc::invalidParams, op.name + " cannot be part of a batch"), project->revision());
    }

    return runInProject (*project, ops, ctx, true);
}

CommandResult CommandBus::runInProject (Project& project, const std::vector<Op>& ops, const CommandContext& ctx, bool isBatch)
{
    const auto t0 = std::chrono::steady_clock::now();

    // 1. optimistic concurrency
    if (ctx.ifRevision.has_value() && *ctx.ifRevision != project.revision())
        return CommandResult::fail (makeError (errc::revisionConflict, "project revision is " + std::to_string (project.revision()) + ", expected "
                                                                          + std::to_string (*ctx.ifRevision),
                                               json { { "current_revision", project.revision() }, { "expected_revision", *ctx.ifRevision } }),
                                    project.revision());

    // 2. validation (all ops before anything is applied)
    bool mutating = false, manual = false;

    for (auto& op : ops)
    {
        auto* cmd = registry_.find (op.name);

        if (auto v = validateJson (cmd->paramsSchema(), op.params); ! v.ok())
            return CommandResult::fail (v.error(), project.revision());

        mutating = mutating || cmd->mutates();
        manual = manual || cmd->manualUndo();
    }

    if (ctx.dryRun)
    {
        CommandResult r;
        r.ok = true;
        r.revision = project.revision();
        r.data = { { "dry_run", true }, { "valid", true } };
        return r;
    }

    auto& undo = project.edit().getUndoManager();
    const auto label = labelFor (ops, ctx);

    // 3. transaction + change scope
    if (mutating && ! manual)
        undo.beginNewTransaction (toJuce (label));

    project.collector().beginScope();

    CommandResult result;
    json batchData = json::array();
    std::optional<Error> failure;

    for (auto& op : ops)
    {
        auto* cmd = registry_.find (op.name);

        try
        {
            auto out = cmd->execute (project, op.params, ctx);

            if (! out.ok())
            {
                failure = out.error();
                break;
            }

            if (isBatch)
                batchData.push_back ({ { "command", op.name }, { "result", out.value() } });
            else
                result.data = std::move (out.value());
        }
        catch (const std::exception& e)
        {
            failure = makeError (errc::internal, std::string (op.name) + ": " + e.what());
            break;
        }
    }

    if (failure.has_value() && mutating && ! manual)
        undo.undoCurrentTransactionOnly();      // inside the scope: the rollback itself is not reported as a change

    auto changes = project.collector().endScope();
    const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();

    json logEntry { { "t", nowIso8601() }, { "origin", ctx.origin.toJson() }, { "name", isBatch ? "batch" : ops[0].name },
                    { "params", isBatch ? json::array() : ActivityLog::redact (ops[0].params) }, { "duration_ms", ms } };

    if (isBatch)
        for (auto& op : ops)
            logEntry["params"].push_back ({ { "command", op.name }, { "params", ActivityLog::redact (op.params) } });

    if (failure.has_value())
    {
        logEntry["ok"] = false;
        logEntry["error"] = failure->toJson();
        logEntry["revision"] = project.revision();
        project.activity().add (std::move (logEntry));
        return CommandResult::fail (*failure, project.revision());
    }

    result.ok = true;

    if (isBatch)
        result.data = { { "results", batchData } };

    if (mutating && ! changes.empty())
    {
        result.revision = project.commit (ctx.origin, label, changes);
        result.changes = std::move (changes);

        // history mirrors the UndoManager: undo/redo move steps between the stacks themselves (see commands)
        if (! manual)
        {
            project.undoSteps().push_back ({ result.revision, label, nowIso8601(), ctx.origin });
            project.redoSteps().clear();
        }
    }
    else
    {
        result.revision = project.revision();
    }

    logEntry["ok"] = true;
    logEntry["revision"] = result.revision;
    project.activity().add (std::move (logEntry));
    return result;
}

} // namespace rovox
