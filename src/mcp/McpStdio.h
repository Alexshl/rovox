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

    /** Blocks until the input ends. Tool calls run off the reader thread (calls that wait on a job on workers, the rest in arrival
        order on one lane), so cancel and ping are handled while a call runs. */
    void run();

private:
    void write (const json&);

    McpServer& server_;
    std::istream& in_;
    std::ostream& out_;
    std::mutex writeMutex_;
};

} // namespace rovox::mcp
