// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rovox
{

/** Streaming SHA-256 (FIPS 180-4). */
class Sha256
{
public:
    Sha256();
    void update (const void* data, size_t size);
    std::array<std::uint8_t, 32> digest();      // finishes the hash
    std::string hexDigest();

private:
    void block (const std::uint8_t*);

    std::uint32_t h_[8];
    std::uint8_t buffer_[64];
    size_t bufferSize_ = 0;
    std::uint64_t totalBytes_ = 0;
};

std::string sha256Hex (const std::string& data);

/** Hash of a whole file read in 1 MiB chunks; nullopt if it cannot be read. */
std::optional<std::string> sha256File (const std::filesystem::path&);

} // namespace rovox
