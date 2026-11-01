// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Ids.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

namespace rovox
{

namespace
{
constexpr std::string_view crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct UlidState
{
    std::mutex mutex;
    std::mt19937_64 rng { std::random_device {}() };
    std::uint64_t lastMs = 0;
    std::array<std::uint8_t, 10> lastRandom {};     // 80 bits, big endian
};

UlidState& state()
{
    static UlidState s;
    return s;
}
} // namespace

std::string newUlid()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock (s.mutex);

    auto ms = (std::uint64_t) std::chrono::duration_cast<std::chrono::milliseconds> (
                  std::chrono::system_clock::now().time_since_epoch()).count();

    if (ms <= s.lastMs)
    {
        // same millisecond (or the clock stepped back): keep ordering by incrementing the random part
        ms = s.lastMs;

        for (int i = 9; i >= 0; --i)
            if (++s.lastRandom[(size_t) i] != 0)
                break;
    }
    else
    {
        s.lastMs = ms;

        for (auto& b : s.lastRandom)
            b = (std::uint8_t) (s.rng() & 0xff);
    }

    std::string out (26, '0');

    for (int i = 9; i >= 0; --i)
    {
        out[(size_t) i] = crockford[ms & 31];
        ms >>= 5;
    }

    // 80 random bits -> 16 base32 characters
    unsigned __int128 r = 0;

    for (auto b : s.lastRandom)
        r = (r << 8) | b;

    for (int i = 25; i >= 10; --i)
    {
        out[(size_t) i] = crockford[(size_t) (r & 31)];
        r >>= 5;
    }

    return out;
}

std::string newId (std::string_view prefix)
{
    std::string id (prefix);
    id += '_';
    id += newUlid();
    return id;
}

bool isValidId (std::string_view id, std::string_view prefix)
{
    if (id.size() != prefix.size() + 1 + 26 || id.substr (0, prefix.size()) != prefix || id[prefix.size()] != '_')
        return false;

    for (auto c : id.substr (prefix.size() + 1))
        if (crockford.find (c) == std::string_view::npos)
            return false;

    return true;
}

bool isValidTenantId (std::string_view tenant)
{
    if (tenant.empty() || tenant.size() > 64)
        return false;

    for (auto c : tenant)
        if (! ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;

    return true;
}

} // namespace rovox
