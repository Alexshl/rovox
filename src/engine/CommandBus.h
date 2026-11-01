// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Command.h"
#include "MessageThreadExecutor.h"

#include <future>

namespace rovox
{

struct Op
{
    std::string name;
    json params = json::object();
};

/** The only way to change a project. Every mutating command is exactly: one named undo transaction, one revision
    increment, one origin-tagged event; a failing command leaves no partial change (its transaction is rolled back). */
class CommandBus
{
public:
    CommandBus (ProjectManager&, CommandRegistry&, MessageThreadExecutor&);

    /** From any thread (needs a running message loop unless called on the message thread). */
    std::future<CommandResult> submit (std::string name, json params, CommandContext);
    /** Several operations, one atomic undo step, one revision, one event. All-or-nothing. */
    std::future<CommandResult> submitBatch (std::vector<Op>, CommandContext);

    /** Message thread only. */
    CommandResult runNow (const std::string& name, const json& params, const CommandContext&);
    CommandResult runBatchNow (const std::vector<Op>&, const CommandContext&);

    CommandRegistry& registry()     { return registry_; }
    MessageThreadExecutor& executor()   { return executor_; }

private:
    CommandResult runInProject (Project&, const std::vector<Op>&, const CommandContext&, bool isBatch);
    static std::string labelFor (const std::vector<Op>&, const CommandContext&);

    ProjectManager& projects_;
    CommandRegistry& registry_;
    MessageThreadExecutor& executor_;
};

} // namespace rovox
