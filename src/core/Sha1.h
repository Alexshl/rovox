// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rovox
{

/** Streaming SHA-1 (RFC 3174). Only used for the per-chunk `Upload-Checksum: sha1` of the tus protocol, never for security. */
class Sha1
{
public:
    Sha1();
    void update (const void* data, size_t size);
    std::array<std::uint8_t, 20> digest();

private:
    void block (const std::uint8_t*);

    std::uint32_t h_[5];
    std::uint8_t buffer_[64];
    size_t bufferSize_ = 0;
    std::uint64_t totalBytes_ = 0;
};

std::string base64Encode (const std::uint8_t* data, size_t size);
std::string base64Decode (const std::string&);      // empty on invalid input

} // namespace rovox
