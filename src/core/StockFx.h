// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// DSP of the Rovox stock effects (EQ, compressor, gate, delay, reverb). Own implementations of textbook algorithms: RBJ Audio EQ
// Cookbook biquads, a feed-forward compressor with a soft knee (Giannoulis et al.), a hysteresis gate, a multi-tap delay and a
// Freeverb-style reverb (eight damped combs and four allpasses). No JUCE, no allocation outside prepare(), deterministic.
// All process() functions work in place on up to two channels.
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace rovox::fx
{

//==============================================================================
enum class BandType : int { LowShelf = 0, HighShelf, Peak, LowPass, HighPass, Notch, BandPass };

struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;     // normalised by a0
};

/** RBJ cookbook design. The bandwidth in octaves is turned into a Q (Q = sqrt(2^bw) / (2^bw - 1)). Gain applies to the shelves and the peak. */
BiquadCoeffs designBiquad (BandType, double sampleRate, double freqHz, double gainDb, double bandwidthOct);
double biquadMagnitudeDb (const BiquadCoeffs&, double sampleRate, double freqHz);

struct EqBand
{
    bool enabled = true;
    BandType type = BandType::Peak;
    double freqHz = 1000.0, gainDb = 0.0, bandwidthOct = 1.0;
    bool operator== (const EqBand&) const = default;
};

struct EqParams
{
    static constexpr int maxBands = 8;
    std::array<EqBand, maxBands> bands;
    double outputGainDb = 0.0;
    bool operator== (const EqParams&) const = default;
};

class Eq
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const EqParams&);
    const EqParams& params() const noexcept     { return params_; }

    /** Magnitude (dB) of the whole filter bank, without the output gain, at a frequency. */
    double magnitudeDb (double freqHz) const;
    void process (float* const* io, int numChannels, int numSamples);

private:
    struct State { double z1 = 0.0, z2 = 0.0; };
    double sampleRate_ = 44100.0, outGain_ = 1.0;
    EqParams params_;
    std::array<BiquadCoeffs, EqParams::maxBands> coeffs_;
    std::array<bool, EqParams::maxBands> active_ {};
    std::array<std::array<State, 2>, EqParams::maxBands> state_;
};

//==============================================================================
struct CompParams
{
    double thresholdDb = -18.0, ratio = 4.0, attackMs = 10.0, releaseMs = 100.0, kneeDb = 6.0;
    double lookaheadMs = 0.0;       // "pre-comp": the audio is delayed by this much, the detector is not (latency)
    double makeupDb = 0.0;
    bool autoMakeup = false;        // adds the reduction a 0 dBFS input would get
    double rmsMs = 0.0;             // detector window; 0 = peak
    double mix = 1.0;               // wet share 0..1
    bool operator== (const CompParams&) const = default;
};

class Comp
{
public:
    static constexpr double maxLookaheadMs = 50.0;

    void prepare (double sampleRate);       // allocates the lookahead line
    void reset();
    void setParams (const CompParams&);
    int latencySamples() const noexcept     { return lookSamples_; }

    /** Static curve: gain reduction (dB, >= 0) for an input level. */
    static double gainReductionDb (const CompParams&, double inputDb);

    void process (float* const* io, int numChannels, int numSamples);
    double lastGainReductionDb() const noexcept { return lastReductionDb_; }     // largest in the last block

private:
    double sampleRate_ = 44100.0, attackCoef_ = 0.0, releaseCoef_ = 0.0, rmsCoef_ = 0.0, ms_ = 0.0, gr_ = 0.0, lastReductionDb_ = 0.0;
    CompParams params_;
    int lookSamples_ = 0;
    size_t writePos_ = 0;
    std::array<std::vector<float>, 2> line_;
};

//==============================================================================
struct GateParams
{
    double thresholdDb = -40.0, attackMs = 1.0, holdMs = 50.0, releaseMs = 100.0, hysteresisDb = 3.0, rangeDb = -80.0;
    bool operator== (const GateParams&) const = default;
};

class Gate
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const GateParams&);
    void process (float* const* io, int numChannels, int numSamples);
    bool isOpen() const noexcept                { return open_; }
    double gainDb() const noexcept;             // current gain of the gate

private:
    double sampleRate_ = 44100.0, attackCoef_ = 0.0, releaseCoef_ = 0.0, detCoef_ = 0.0, env_ = 0.0, gain_ = 1.0, rangeLin_ = 0.0;
    long holdSamples_ = 0, holdLeft_ = 0;
    bool open_ = false;
    GateParams params_;
};

//==============================================================================
struct DelayTap
{
    bool enabled = true;
    bool sync = false;              // true: `beats` (quarter notes) at the project tempo, false: `timeMs`
    double timeMs = 250.0, beats = 0.5, gainDb = 0.0, pan = 0.0;
    bool operator== (const DelayTap&) const = default;
};

struct DelayParams
{
    static constexpr int maxTaps = 4;
    std::array<DelayTap, maxTaps> taps;
    double feedbackPct = 0.0;       // tap 1 is fed back into the line (0 .. 95)
    double lowPassHz = 20000.0, highPassHz = 20.0;      // in the feedback path
    double wetDb = -6.0, dryDb = 0.0;
    double bpm = 120.0;
    bool operator== (const DelayParams&) const = default;
};

class Delay
{
public:
    static constexpr double maxSeconds = 8.0;

    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const DelayParams&);
    void process (float* const* io, int numChannels, int numSamples);
    /** Time until the echoes have died away (-60 dB), from the parameters alone. */
    static double tailSeconds (const DelayParams&);

    /** Delay of a tap in samples under the current parameters. */
    double tapSamples (int tap) const;

private:
    double sampleRate_ = 44100.0;
    DelayParams params_;
    bool snap_ = true;
    std::array<double, DelayParams::maxTaps> current_ {}, target_ {};
    std::array<std::vector<float>, 2> line_;
    size_t writePos_ = 0;
    struct Filter { double lp = 0.0, hpLp = 0.0; };
    std::array<Filter, 2> filter_;
};

//==============================================================================
struct VerbParams
{
    double roomSize = 0.5, damping = 0.5, preDelayMs = 0.0, width = 1.0, lowCutHz = 20.0, highCutHz = 20000.0, wetDb = -12.0, dryDb = 0.0;
    bool operator== (const VerbParams&) const = default;
};

class Verb
{
public:
    static constexpr double maxPreDelayMs = 250.0;

    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const VerbParams&);
    void process (float* const* io, int numChannels, int numSamples);
    static double tailSeconds (const VerbParams&);

private:
    struct Comb { std::vector<float> buf; size_t pos = 0; double store = 0.0; };
    struct Allpass { std::vector<float> buf; size_t pos = 0; };
    struct Channel
    {
        std::array<Comb, 8> combs;
        std::array<Allpass, 4> allpasses;
        std::vector<float> pre;
        size_t prePos = 0;
        double lp = 0.0, hpLp = 0.0;
    };

    double sampleRate_ = 44100.0, feedback_ = 0.84, damp1_ = 0.2, damp2_ = 0.8;
    size_t preSamples_ = 0;
    VerbParams params_;
    std::array<Channel, 2> ch_;
};

} // namespace rovox::fx
