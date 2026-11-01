// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "AudioSink.h"

#include <lame/lame.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rovox
{

namespace
{
class JuceSink : public AudioSink
{
public:
    JuceSink (std::unique_ptr<juce::AudioFormatWriter> w, int bits, int channels) : writer_ (std::move (w)), bits_ (bits), ints_ ((size_t) channels), ptrs_ ((size_t) channels) {}

    bool write (const float* const* planar, int n) override
    {
        if (bits_ == 32)
            return writer_->writeFromFloatArrays (planar, (int) ints_.size(), n);

        for (size_t c = 0; c < ints_.size(); ++c)
        {
            ints_[c].resize ((size_t) n);

            for (int i = 0; i < n; ++i)
            {
                const double x = planar[c][i];

                if (bits_ == 16)
                {
                    const double tpdf = uniform() + uniform() - 1.0;                   // +-1 LSB triangular dither
                    const auto v = std::clamp (std::llround (x * 32767.0 + tpdf), -32768LL, 32767LL);
                    ints_[c][(size_t) i] = (int) (v * 65536);
                }
                else
                {
                    const auto v = std::clamp (std::llround (x * 8388607.0), -8388608LL, 8388607LL);
                    ints_[c][(size_t) i] = (int) (v * 256);
                }
            }

            ptrs_[c] = ints_[c].data();
        }

        return writer_->write (ptrs_.data(), n);
    }

    bool close() override
    {
        writer_->flush();
        writer_.reset();
        return true;
    }

private:
    double uniform()
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return (double) (state_ >> 11) * (1.0 / 9007199254740992.0);
    }

    std::unique_ptr<juce::AudioFormatWriter> writer_;
    int bits_;
    std::vector<std::vector<int>> ints_;
    std::vector<const int*> ptrs_;
    std::uint64_t state_ = 0x9e3779b97f4a7c15ULL;   // fixed seed: a render is reproducible
};

class Mp3Sink : public AudioSink
{
public:
    Mp3Sink (lame_t l, FILE* f, int channels) : lame_ (l), file_ (f), channels_ (channels) {}

    ~Mp3Sink() override
    {
        if (file_ != nullptr)
            std::fclose (file_);

        if (lame_ != nullptr)
            lame_close (lame_);
    }

    bool write (const float* const* planar, int n) override
    {
        buffer_.resize ((size_t) (1.25 * n + 7200));
        const float* right = channels_ > 1 ? planar[1] : planar[0];
        const int bytes = lame_encode_buffer_ieee_float (lame_, planar[0], right, n, buffer_.data(), (int) buffer_.size());
        return bytes >= 0 && (bytes == 0 || std::fwrite (buffer_.data(), 1, (size_t) bytes, file_) == (size_t) bytes);
    }

    bool close() override
    {
        buffer_.resize (7200);
        const int bytes = lame_encode_flush (lame_, buffer_.data(), (int) buffer_.size());
        const bool ok = bytes >= 0 && (bytes == 0 || std::fwrite (buffer_.data(), 1, (size_t) bytes, file_) == (size_t) bytes);
        const bool closed = std::fclose (file_) == 0;
        file_ = nullptr;
        return ok && closed;
    }

private:
    lame_t lame_;
    FILE* file_;
    int channels_;
    std::vector<unsigned char> buffer_;
};

class JuceFloatReader : public FloatReader
{
public:
    JuceFloatReader (std::unique_ptr<juce::AudioFormatReader> r) : reader_ (std::move (r)), buffer_ ((int) reader_->numChannels, 65536), ptrs_ (reader_->numChannels) {}

    std::int64_t totalFrames() const override   { return reader_->lengthInSamples; }
    unsigned channels() const override          { return reader_->numChannels; }

    const float* const* read (std::int64_t start, int n) override
    {
        if (n > buffer_.getNumSamples())
            buffer_.setSize (buffer_.getNumChannels(), n);

        if (! reader_->read (&buffer_, 0, n, start, true, true))
            return nullptr;

        for (unsigned c = 0; c < reader_->numChannels; ++c)
            ptrs_[c] = buffer_.getReadPointer ((int) c);

        return ptrs_.data();
    }

private:
    std::unique_ptr<juce::AudioFormatReader> reader_;
    juce::AudioBuffer<float> buffer_;
    std::vector<const float*> ptrs_;
};
} // namespace

Result<std::unique_ptr<AudioSink>> createSink (const SinkSpec& s, const fs::path& dest)
{
    if (s.channels < 1 || s.channels > 2)
        return makeError (errc::invalidParams, "channels must be 1 or 2", json { { "path", "/format/channels" } });

    if (s.container == "mp3")
    {
        lame_t l = lame_init();

        if (l == nullptr)
            return makeError (errc::internal, "lame_init failed");

        lame_set_in_samplerate (l, (int) s.sampleRate);
        lame_set_out_samplerate (l, (int) s.sampleRate);
        lame_set_num_channels (l, s.channels);
        lame_set_mode (l, s.channels == 1 ? MONO : JOINT_STEREO);
        lame_set_VBR (l, vbr_off);
        lame_set_brate (l, s.bitrateKbps);

        if (lame_init_params (l) < 0)
        {
            lame_close (l);
            return makeError (errc::invalidParams, "the MP3 encoder rejects sample rate " + std::to_string ((int) s.sampleRate) + " / bitrate " + std::to_string (s.bitrateKbps));
        }

        FILE* f = std::fopen (dest.c_str(), "wb");

        if (f == nullptr)
        {
            lame_close (l);
            return makeError (errc::io, "cannot write " + dest.string());
        }

        return std::unique_ptr<AudioSink> (new Mp3Sink (l, f, s.channels));
    }

    int bits = s.bitDepth;
    std::unique_ptr<juce::AudioFormat> format;

    if (s.container == "flac")
    {
        if (bits != 16 && bits != 24)
            bits = 24;

        format = std::make_unique<juce::FlacAudioFormat>();
    }
    else
    {
        if (bits != 16 && bits != 24 && bits != 32)
            return makeError (errc::invalidParams, "wav bit depth must be 16, 24 or 32", json { { "path", "/format/bit_depth" } });

        format = std::make_unique<juce::WavAudioFormat>();
    }

    auto file = toFile (dest);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return makeError (errc::io, "cannot write " + dest.string());

    std::unique_ptr<juce::AudioFormatWriter> writer (format->createWriterFor (stream.get(), s.sampleRate, (unsigned) s.channels, bits, {}, 0));

    if (writer == nullptr)
        return makeError (errc::invalidParams, "cannot create a " + s.container + " writer for " + std::to_string (bits) + " bit at " + std::to_string ((int) s.sampleRate) + " Hz");

    stream.release();       // owned by the writer now
    return std::unique_ptr<AudioSink> (new JuceSink (std::move (writer), bits, s.channels));
}

Result<std::unique_ptr<FloatReader>> openFloatReader (const fs::path& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (toFile (file)));

    if (r == nullptr)
        return makeError (errc::io, "cannot read " + file.string());

    return std::unique_ptr<FloatReader> (new JuceFloatReader (std::move (r)));
}

} // namespace rovox
