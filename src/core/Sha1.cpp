// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Sha1.h"

#include <algorithm>
#include <cstring>

namespace rovox
{

namespace
{
inline std::uint32_t rotl (std::uint32_t x, int n)  { return (x << n) | (x >> (32 - n)); }
}

Sha1::Sha1()
{
    static constexpr std::uint32_t init[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::memcpy (h_, init, sizeof (h_));
}

void Sha1::block (const std::uint8_t* p)
{
    std::uint32_t w[80];

    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t) p[i * 4] << 24 | (std::uint32_t) p[i * 4 + 1] << 16 | (std::uint32_t) p[i * 4 + 2] << 8 | p[i * 4 + 3];

    for (int i = 16; i < 80; ++i)
        w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    auto a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

    for (int i = 0; i < 80; ++i)
    {
        std::uint32_t f, k;

        if (i < 20)         { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40)    { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60)    { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else                { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

        const auto t = rotl (a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl (b, 30); b = a; a = t;
    }

    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update (const void* data, size_t size)
{
    auto* p = static_cast<const std::uint8_t*> (data);
    totalBytes_ += size;

    while (size > 0)
    {
        const auto n = std::min (size, sizeof (buffer_) - bufferSize_);
        std::memcpy (buffer_ + bufferSize_, p, n);
        bufferSize_ += n;
        p += n;
        size -= n;

        if (bufferSize_ == sizeof (buffer_))
        {
            block (buffer_);
            bufferSize_ = 0;
        }
    }
}

std::array<std::uint8_t, 20> Sha1::digest()
{
    const std::uint64_t bits = totalBytes_ * 8;
    const std::uint8_t pad = 0x80, zero = 0;
    update (&pad, 1);

    while (bufferSize_ != 56)
        update (&zero, 1);

    std::uint8_t len[8];

    for (int i = 0; i < 8; ++i)
        len[i] = (std::uint8_t) (bits >> (56 - 8 * i));

    update (len, 8);
    std::array<std::uint8_t, 20> out {};

    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[(size_t) (i * 4 + j)] = (std::uint8_t) (h_[i] >> (24 - 8 * j));

    return out;
}

std::string base64Encode (const std::uint8_t* data, size_t size)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;

    for (size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t v = (std::uint32_t) data[i] << 16 | (i + 1 < size ? (std::uint32_t) data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < size ? t[(v >> 6) & 63] : '=';
        out += i + 2 < size ? t[v & 63] : '=';
    }

    return out;
}

std::string base64Decode (const std::string& in)
{
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;

    for (char c : in)
    {
        int v;

        if (c >= 'A' && c <= 'Z')       v = c - 'A';
        else if (c >= 'a' && c <= 'z')  v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')  v = c - '0' + 52;
        else if (c == '+')              v = 62;
        else if (c == '/')              v = 63;
        else if (c == '=')              break;
        else                            return {};

        acc = (acc << 6) | (std::uint32_t) v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            out += (char) ((acc >> bits) & 0xff);
        }
    }

    return out;
}

} // namespace rovox
