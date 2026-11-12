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
    std::string subject;        // the end user behind an authenticated key (platform user id); empty = not known
    std::string client;         // the calling application behind an authenticated key; empty = not known

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

    json toJson() const
    {
        json j { { "kind", kindName (kind) }, { "session", session }, { "label", label } };

        if (! subject.empty())
            j["subject"] = subject;

        if (! client.empty())
            j["client"] = client;

        return j;
    }

    static Origin system()                  { return { Kind::system, "", "system" }; }
    static Origin cli()                     { return { Kind::cli, "cli", "cli" }; }
};

} // namespace rovox
