// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RppParser.h"

#include <sstream>

namespace rovox
{

std::vector<std::string> tokenizeRppLine (const std::string& line)
{
    std::vector<std::string> tokens;
    size_t i = 0;

    while (i < line.size())
    {
        while (i < line.size() && std::isspace ((unsigned char) line[i]))
            ++i;

        if (i >= line.size())
            break;

        const char c = line[i];

        if (c == '"' || c == '\'' || c == '`')
        {
            const auto end = line.find (c, i + 1);
            const auto stop = end == std::string::npos ? line.size() : end;
            tokens.push_back (line.substr (i + 1, stop - i - 1));
            i = stop == line.size() ? stop : stop + 1;
        }
        else
        {
            const auto start = i;

            while (i < line.size() && ! std::isspace ((unsigned char) line[i]))
                ++i;

            tokens.push_back (line.substr (start, i - start));
        }
    }

    return tokens;
}

const std::vector<std::string>* RppNode::line (const std::string& key) const
{
    for (auto& e : entries)
        if (! e.isBlock() && ! e.tokens.empty() && e.tokens[0] == key)
            return &e.tokens;

    return nullptr;
}

std::vector<const std::vector<std::string>*> RppNode::lines (const std::string& key) const
{
    std::vector<const std::vector<std::string>*> out;

    for (auto& e : entries)
        if (! e.isBlock() && ! e.tokens.empty() && e.tokens[0] == key)
            out.push_back (&e.tokens);

    return out;
}

std::vector<const RppNode*> RppNode::blocks (const std::string& n) const
{
    std::vector<const RppNode*> out;

    for (auto& e : entries)
        if (e.isBlock() && e.children[0].name == n)
            out.push_back (&e.children[0]);

    return out;
}

const RppNode* RppNode::block (const std::string& n) const
{
    auto all = blocks (n);
    return all.empty() ? nullptr : all.front();
}

double RppNode::number (const std::string& key, size_t arg, double fallback) const
{
    if (auto* l = line (key); l != nullptr && l->size() > arg + 1)
        try { return std::stod ((*l)[arg + 1]); } catch (...) {}

    return fallback;
}

std::string RppNode::toText (int indent) const
{
    std::ostringstream out;
    const std::string pad ((size_t) indent * 2, ' ');
    out << pad << "<" << name;

    for (auto& a : args)
        out << " " << (a.find (' ') != std::string::npos || a.empty() ? "\"" + a + "\"" : a);

    out << "\n";

    for (auto& e : entries)
    {
        if (e.isBlock())
        {
            out << e.children[0].toText (indent + 1);
        }
        else
        {
            out << pad << "  ";

            for (size_t i = 0; i < e.tokens.size(); ++i)
                out << (i ? " " : "") << (e.tokens[i].find (' ') != std::string::npos || e.tokens[i].empty() ? "\"" + e.tokens[i] + "\"" : e.tokens[i]);

            out << "\n";
        }
    }

    out << pad << ">\n";
    return out.str();
}

Result<RppNode> parseRpp (const std::string& text)
{
    std::istringstream in (text);
    std::string raw;
    std::vector<RppNode> stack;
    RppNode root;
    bool haveRoot = false;
    int lineNo = 0;

    while (std::getline (in, raw))
    {
        ++lineNo;
        size_t b = raw.find_first_not_of (" \t\r");

        if (b == std::string::npos)
            continue;

        const auto line = raw.substr (b, raw.find_last_not_of (" \t\r") - b + 1);

        if (line[0] == '<')
        {
            auto tokens = tokenizeRppLine (line.substr (1));
            RppNode node;

            if (! tokens.empty())
            {
                node.name = tokens[0];
                node.args.assign (tokens.begin() + 1, tokens.end());
            }

            stack.push_back (std::move (node));
        }
        else if (line == ">")
        {
            if (stack.empty())
                return makeError (errc::invalidParams, "RPP line " + std::to_string (lineNo) + ": '>' without an open block");

            RppNode done = std::move (stack.back());
            stack.pop_back();

            if (stack.empty())
            {
                root = std::move (done);
                haveRoot = true;
            }
            else
            {
                RppEntry e;
                e.children.push_back (std::move (done));
                stack.back().entries.push_back (std::move (e));
            }
        }
        else if (! stack.empty())
        {
            RppEntry e;
            e.tokens = tokenizeRppLine (line);
            stack.back().entries.push_back (std::move (e));
        }
    }

    if (! stack.empty())
        return makeError (errc::invalidParams, "RPP ends inside an open block <" + stack.back().name + ">");

    if (! haveRoot || root.name != "REAPER_PROJECT")
        return makeError (errc::invalidParams, "not a REAPER project (no <REAPER_PROJECT block)");

    return root;
}

} // namespace rovox
