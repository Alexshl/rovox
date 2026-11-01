// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <string>
#include <string_view>

namespace rovox
{

/** 26-character ULID (Crockford base32): 48-bit millisecond timestamp + 80 random bits.
    Strictly increasing within one process, so ids sort by creation time. */
std::string newUlid();

/** Prefixed entity id, e.g. newId ("trk") -> "trk_01J...". Prefixes: p, trk, itm, fx, snd, mrk, med, ipl, job, up. */
std::string newId (std::string_view prefix);

/** True if `id` is `<prefix>_<26 ULID characters>`. */
bool isValidId (std::string_view id, std::string_view prefix);

/** Tenant ids are `[a-z0-9_-]{1,64}`; this keeps `..` and path separators out of data paths. */
bool isValidTenantId (std::string_view tenant);

} // namespace rovox
