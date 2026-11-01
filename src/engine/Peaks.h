// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Waveform peaks for the web UI (task 310). A base pyramid (min / max per 256 samples, all channels merged) is built once
// per file and kept in memory; any requested window is reduced from it, or read from the file when the zoom is finer
// than the pyramid. Thread safe; meant for HTTP threads.
#pragma once

#include "Tracktion.h"

#include <map>
#include <memory>
#include <mutex>

namespace rovox
{

struct PeakResult
{
    std::vector<std::int8_t> data;      // bins x (min, max), -127..127 = -1..+1
    int channels = 1;
    double sampleRate = 44100.0;
};

class PeakStore
{
public:
    /** [startS, endS) of the source file, reduced to `bins` min/max pairs. */
    Result<PeakResult> compute (const fs::path& file, double startS, double endS, int bins);

    static constexpr int baseBlock = 256;

private:
    struct Pyramid
    {
        std::mutex buildMutex;
        bool built = false;
        std::vector<std::int8_t> mins, maxs;
        double sampleRate = 44100.0;
        std::int64_t frames = 0;
        int channels = 1;
        std::uintmax_t size = 0;
        fs::file_time_type mtime;
    };

    std::shared_ptr<Pyramid> pyramidFor (const fs::path&);

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Pyramid>> pyramids_;
};

} // namespace rovox
