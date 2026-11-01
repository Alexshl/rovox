// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Result.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace rovox
{

/** Ring buffer of the last N commands of a project. Params are stored with secrets redacted. */
class ActivityLog
{
public:
    explicit ActivityLog (size_t capacity = 1000) : capacity_ (capacity) {}

    void add (json entry);      // {t, origin, name, params, ok, revision, duration_ms}
    std::vector<json> list (size_t limit = 100) const;     // newest last
    size_t size() const;

    /** Replaces the values of keys that look like secrets (token, secret, password, key, authorization) with "***". */
    static json redact (const json& params);

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<json> entries_;
};

} // namespace rovox
