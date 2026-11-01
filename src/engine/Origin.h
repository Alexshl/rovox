// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Result.h"

#include <string>

namespace rovox
{

struct Origin
{
    enum class Kind { ui, mcp, cli, system, skill };

    Kind kind = Kind::cli;
    std::string session;        // e.g. "ws_12", "mcp_3"
    std::string label;          // human readable: "Claude", "alex@browser"

    static const char* kindName (Kind k)
    {
        switch (k)
        {
            case Kind::ui:      return "ui";
            case Kind::mcp:     return "mcp";
            case Kind::cli:     return "cli";
            case Kind::system:  return "system";
            case Kind::skill:   return "skill";
        }

        return "system";
    }

    json toJson() const     { return { { "kind", kindName (kind) }, { "session", session }, { "label", label } }; }

    static Origin system()                  { return { Kind::system, "", "system" }; }
    static Origin cli()                     { return { Kind::cli, "cli", "cli" }; }
};

} // namespace rovox
