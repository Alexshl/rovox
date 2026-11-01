// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER project (.RPP) reader (task 150): a block tokenizer for `<NAME args ... >` with parameter lines. Unknown blocks
// and lines are kept as they are, so nothing in a file can make parsing fail except broken nesting.
#pragma once

#include "Result.h"

#include <string>
#include <vector>

namespace rovox
{

struct RppNode;

/** One entry of a block body, in file order: either a parameter line or a nested block. */
struct RppEntry
{
    std::vector<std::string> tokens;        // for a line: [key, args...]
    std::vector<RppNode> children;          // 0 or 1 element (vector only because RppNode is incomplete here)
    bool isBlock() const { return ! children.empty(); }
};

struct RppNode
{
    std::string name;                       // "TRACK", "ITEM", "SOURCE" ...
    std::vector<std::string> args;          // tokens after the name on the opening line
    std::vector<RppEntry> entries;

    /** First parameter line with this key (tokens after the key), or nullptr. */
    const std::vector<std::string>* line (const std::string& key) const;
    /** All parameter lines with this key. */
    std::vector<const std::vector<std::string>*> lines (const std::string& key) const;
    std::vector<const RppNode*> blocks (const std::string& name) const;
    const RppNode* block (const std::string& name) const;
    double number (const std::string& key, size_t arg, double fallback) const;

    /** The block as text again (for keeping unsupported chunks verbatim). */
    std::string toText (int indent = 0) const;
};

Result<RppNode> parseRpp (const std::string& text);

/** Splits one RPP line into tokens: whitespace separated, with "double", 'single' or `backtick` quoting. */
std::vector<std::string> tokenizeRppLine (const std::string& line);

} // namespace rovox
