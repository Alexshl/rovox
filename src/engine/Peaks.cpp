// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Peaks.h"

#include "AudioSink.h"

#include <algorithm>
#include <cmath>

namespace rovox
{

namespace
{
std::int8_t quantise (float v)
{
    return (std::int8_t) std::clamp ((int) std::lround (v * 127.0f), -127, 127);
}
} // namespace

std::shared_ptr<PeakStore::Pyramid> PeakStore::pyramidFor (const fs::path& file)
{
    std::error_code ec;
    const auto size = fs::file_size (file, ec);
    const auto mtime = fs::last_write_time (file, ec);
    const auto key = file.string();

    std::lock_guard<std::mutex> lock (mutex_);
    auto& slot = pyramids_[key];

    if (slot == nullptr || slot->size != size || slot->mtime != mtime)
    {
        slot = std::make_shared<Pyramid>();
        slot->size = size;
        slot->mtime = mtime;
    }

    if (pyramids_.size() > 512)
        for (auto it = pyramids_.begin(); it != pyramids_.end() && pyramids_.size() > 384;)
            it = it->second.use_count() == 1 && it->first != key ? pyramids_.erase (it) : std::next (it);

    return slot;
}

Result<PeakResult> PeakStore::compute (const fs::path& file, double startS, double endS, int bins)
{
    if (bins < 1 || ! (endS >= startS))
        return makeError (errc::invalidParams, "bad peak window");

    auto reader = openFloatReader (file);

    if (! reader.ok())
        return reader.error();

    auto& r = *reader.value();
    PeakResult out;
    out.channels = (int) r.channels();
    out.sampleRate = r.sampleRate();
    out.data.assign ((size_t) bins * 2, 0);

    const double rate = r.sampleRate();
    const std::int64_t total = r.totalFrames();
    const std::int64_t s0 = std::clamp<std::int64_t> ((std::int64_t) std::floor (startS * rate), 0, total);
    const std::int64_t s1 = std::clamp<std::int64_t> ((std::int64_t) std::ceil (endS * rate), s0, total);

    if (s1 <= s0)
        return out;

    const double perBin = (double) (s1 - s0) / bins;

    if (perBin >= baseBlock * 2)
    {
        auto p = pyramidFor (file);
        std::lock_guard<std::mutex> build (p->buildMutex);

        if (! p->built)
        {
            const auto blocks = (size_t) ((total + baseBlock - 1) / baseBlock);
            p->mins.assign (blocks, 0);
            p->maxs.assign (blocks, 0);
            p->sampleRate = rate;
            p->frames = total;
            p->channels = (int) r.channels();

            constexpr int chunk = baseBlock * 256;

            for (std::int64_t pos = 0; pos < total; pos += chunk)
            {
                const int n = (int) std::min<std::int64_t> (chunk, total - pos);
                const float* const* data = r.read (pos, n);

                if (data == nullptr)
                    return makeError (errc::io, "cannot read " + file.string());

                for (int b = 0; b * baseBlock < n; ++b)
                {
                    float lo = 0, hi = 0;
                    const int end = std::min (n, (b + 1) * baseBlock);

                    for (unsigned c = 0; c < r.channels(); ++c)
                        for (int i = b * baseBlock; i < end; ++i)
                        {
                            lo = std::min (lo, data[c][i]);
                            hi = std::max (hi, data[c][i]);
                        }

                    const auto idx = (size_t) (pos / baseBlock + b);
                    p->mins[idx] = quantise (lo);
                    p->maxs[idx] = quantise (hi);
                }
            }

            p->built = true;
        }

        for (int b = 0; b < bins; ++b)
        {
            const auto from = (std::int64_t) (s0 + std::floor (b * perBin)), to = (std::int64_t) (s0 + std::floor ((b + 1) * perBin));
            std::int8_t lo = 0, hi = 0;

            for (std::int64_t k = from / baseBlock; k <= std::max (from, to - 1) / baseBlock && k < (std::int64_t) p->mins.size(); ++k)
            {
                lo = std::min (lo, p->mins[(size_t) k]);
                hi = std::max (hi, p->maxs[(size_t) k]);
            }

            out.data[(size_t) b * 2] = lo;
            out.data[(size_t) b * 2 + 1] = hi;
        }

        return out;
    }

    // finer than the pyramid: read the window itself (at most bins * 2 * baseBlock frames)
    const int n = (int) (s1 - s0);
    const float* const* data = r.read (s0, n);

    if (data == nullptr)
        return makeError (errc::io, "cannot read " + file.string());

    for (int b = 0; b < bins; ++b)
    {
        const int from = (int) std::floor (b * perBin), to = std::min (n, std::max (from + 1, (int) std::floor ((b + 1) * perBin)));
        float lo = 0, hi = 0;

        for (unsigned c = 0; c < r.channels(); ++c)
            for (int i = from; i < to; ++i)
            {
                lo = std::min (lo, data[c][i]);
                hi = std::max (hi, data[c][i]);
            }

        out.data[(size_t) b * 2] = quantise (lo);
        out.data[(size_t) b * 2 + 1] = quantise (hi);
    }

    return out;
}

} // namespace rovox
