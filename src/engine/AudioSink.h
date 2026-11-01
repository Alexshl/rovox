// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Audio file writing (WAV / FLAC through JUCE, MP3 through libmp3lame) and chunked float reading.
#pragma once

#include "Tracktion.h"

#include <memory>

namespace rovox
{

struct SinkSpec
{
    std::string container = "wav";      // wav | flac | mp3
    int bitDepth = 24;                  // wav 16 | 24 | 32 (float), flac 16 | 24; 16-bit gets TPDF dither
    int bitrateKbps = 320;
    double sampleRate = 44100.0;
    int channels = 2;
};

class AudioSink
{
public:
    virtual ~AudioSink() = default;
    virtual bool write (const float* const* planar, int numFrames) = 0;
    virtual bool close() = 0;
};

Result<std::unique_ptr<AudioSink>> createSink (const SinkSpec&, const fs::path& dest);

/** Reads a (float) audio file in blocks. The pointers returned by read() stay valid until the next call. */
class FloatReader
{
public:
    virtual ~FloatReader() = default;
    virtual std::int64_t totalFrames() const = 0;
    virtual unsigned channels() const = 0;
    virtual double sampleRate() const = 0;
    virtual const float* const* read (std::int64_t start, int numFrames) = 0;
};

Result<std::unique_ptr<FloatReader>> openFloatReader (const fs::path&);

} // namespace rovox
