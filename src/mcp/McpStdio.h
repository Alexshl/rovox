// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// MCP over stdio (task 200): newline-delimited JSON-RPC. One reader, one writer, nothing else on stdout.
#pragma once

#include "Mcp.h"

#include <iosfwd>

namespace rovox::mcp
{

class StdioTransport
{
public:
    StdioTransport (McpServer&, std::istream& in, std::ostream& out);

    /** Blocks until the input ends. Requests that cannot block run on the reader thread in arrival order; calls that wait on a
        job run on worker threads so they never hold up the ones behind them. */
    void run();

private:
    void write (const json&);

    McpServer& server_;
    std::istream& in_;
    std::ostream& out_;
    std::mutex writeMutex_;
};

} // namespace rovox::mcp
