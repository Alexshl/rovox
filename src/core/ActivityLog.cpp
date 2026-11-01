// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ActivityLog.h"

#include <algorithm>

namespace rovox
{

namespace
{
bool looksSecret (std::string key)
{
    std::transform (key.begin(), key.end(), key.begin(), [] (unsigned char c) { return (char) std::tolower (c); });

    for (const char* needle : { "token", "secret", "password", "passwd", "authorization", "api_key", "apikey", "private_key" })
        if (key.find (needle) != std::string::npos)
            return true;

    return false;
}
} // namespace

json ActivityLog::redact (const json& params)
{
    if (params.is_object())
    {
        json out = json::object();

        for (auto it = params.begin(); it != params.end(); ++it)
            out[it.key()] = looksSecret (it.key()) ? json ("***") : redact (it.value());

        return out;
    }

    if (params.is_array())
    {
        json out = json::array();

        for (auto& v : params)
            out.push_back (redact (v));

        return out;
    }

    return params;
}

void ActivityLog::add (json entry)
{
    std::lock_guard<std::mutex> lock (mutex_);
    entries_.push_back (std::move (entry));

    while (entries_.size() > capacity_)
        entries_.pop_front();
}

std::vector<json> ActivityLog::list (size_t limit) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto n = std::min (limit, entries_.size());
    return { entries_.end() - (std::ptrdiff_t) n, entries_.end() };
}

size_t ActivityLog::size() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return entries_.size();
}

} // namespace rovox
