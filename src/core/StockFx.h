// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// DSP of the Rovox stock effects (EQ, compressor, gate, delay, reverb). Own implementations of textbook algorithms: RBJ Audio EQ
// Cookbook biquads, a feed-forward compressor with a soft knee (Giannoulis et al.), a hysteresis gate, a multi-tap delay and a
// Freeverb-style reverb (eight damped combs and four allpasses). No JUCE, no allocation outside prepare(), deterministic.
// All process() functions work in place on up to two channels.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace rovox::fx
{

//==============================================================================
enum class BandType : int { LowShelf = 0, HighShelf, Peak, LowPass, HighPass, Notch, BandPass,
                            // the ReaEQ band types that have no counterpart above (always designed the ReaEQ way)
                            AllPass, PeakAlt2, ParallelBandPass, PeakAlt };

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

/** ReaEQ filter design (reaperCompat): the bandwidth turns into the RBJ alpha as alpha = sin(w0) * sinh(ln2/2 * BW * w0 / sin(w0)), the shelves use the
    shelf slope S = min(1 / BW^2, 1.2), the pass / notch / band-pass / all-pass types ignore the gain, "Band (alt 2)" keeps its poles at the BW
    alpha whatever the gain is, "Band (alt)" is the Band with a gain floor, a gain of -150 dB or less is -inf (amplitude 0: shelves become a high / low pass, the Band a notch), and all ParallelBandPass bands are summed in parallel (the sum then goes through the other bands in series). */
BiquadCoeffs designReaperBiquad (BandType, double sampleRate, double freqHz, double gainDb, double bandwidthOct);

struct EqParams
{
    static constexpr int maxBands = 8;
    std::array<EqBand, maxBands> bands;
    double outputGainDb = 0.0;
    bool reaperCompat = false;      // design the bands the ReaEQ way (designReaperBiquad); false: the Rovox design, same as before
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
    static bool isParallel (const EqBand& b)    { return b.type == BandType::ParallelBandPass; }

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
/** ReaDelay behaviour: every tap is its own delay line with its own feedback loop (so the taps do not share a line, unlike Delay). A tap's
    signal is the delayed input run through the loop filters (low-pass then high-pass, 12 dB/oct, 0 dB at the corner), the repeat n is
    feedback^(n-1) * filter^n * input. Resolution, width, pan and volume come after the loop; the wet gain after the sum of the taps.
    The delay is a whole number of samples: round((timeMs + musical * 1/8 note) * fs / 1000). */
struct ReaDelayTap
{
    bool enabled = true;
    double timeMs = 0.0;
    double musical = 2.0;           // in 1/8 notes at the project tempo
    double feedback = 0.0;          // amplitude (0 = -inf dB)
    double lowPassHz = 20000.0;     // >= 20000: no filter
    double highPassHz = 0.0;        // <= 0: no filter
    int bits = 24;                  // 24: no truncation
    double width = 1.0;             // -1 .. 1
    double volume = 1.0;            // amplitude
    double pan = 0.0;               // -1 .. 1, balance law
    bool operator== (const ReaDelayTap&) const = default;
};

struct ReaDelayParams
{
    static constexpr int maxTaps = 4;
    std::array<ReaDelayTap, maxTaps> taps;
    double wet = 0.5, dry = 1.0;    // amplitudes
    double bpm = 120.0;
    bool operator== (const ReaDelayParams&) const = default;
};

class ReaDelay
{
public:
    static constexpr double maxSeconds = 12.0;

    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const ReaDelayParams&);
    void process (float* const* io, int numChannels, int numSamples);
    static double tailSeconds (const ReaDelayParams&);

    /** Delay of a tap in whole samples, at least 1. */
    static long delaySamples (const ReaDelayTap&, double bpm, double sampleRate);

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run (double x) noexcept; };
    struct Loop
    {
        std::vector<float> line;
        Biquad lp, hp;
    };

    double sampleRate_ = 44100.0;
    ReaDelayParams params_;
    std::array<std::array<Loop, 2>, ReaDelayParams::maxTaps> loop_;
    std::array<long, ReaDelayParams::maxTaps> delay_ {};
    size_t writePos_ = 0;
};

//==============================================================================
/** Detector filter of ReaComp / ReaGate: a 12 dB/oct low-pass and high-pass (RBJ, Q = 1, 0 dB at the corner). The low-pass is off at 20 kHz and
    above, the high-pass at 0.5 Hz and below. */
class DetectorFilter
{
public:
    void set (double sampleRate, double lowPassHz, double highPassHz);
    void reset() noexcept;
    double run (double x) noexcept;

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run (double x) noexcept; };
    Biquad lp_, hp_;
    bool lpOn_ = false, hpOn_ = false;
};

//==============================================================================
/** ReaGate behaviour. Amplitudes (dry, wet) are linear. The closed gate is exactly silent. All times are milliseconds: the offset D is 0.5 ms
    (fractional samples at any rate). Attack: the gain rises by 1 / (attack - D) per sample and an attack that has started always completes. The gate
    stays held for floor(hold - D) + max(floor(attack - D) - 1, 0) samples after the level fell below the close threshold (the attack time is added to
    the hold), then the gain falls by 1 / (release - D) per sample. The level is compared with a threshold that moves from the open to the close
    threshold, linearly in amplitude, with the current gain. Pre-open delays the audio by floor(pre-open - D) samples (reported as latency) while
    the detector and the gain run on the undelayed input, so the gate opens that much before the audio; the hold is lengthened by the same amount so
    that closing stays on the audio. */
struct ReaGateParams
{
    double thresholdDb = -150.0, attackMs = 3.0, releaseMs = 100.0, preOpenMs = 0.0, holdMs = 0.0, lowPassHz = 20000.0, highPassHz = 0.0;
    double hysteresisDb = 0.0;      // the gate closes below threshold + hysteresis
    double rmsMs = 0.0;             // 0 = peak detector
    double dry = 0.0, wet = 1.0;    // amplitudes
    bool invertWet = false;         // wet passes while the gate is closed
    bool operator== (const ReaGateParams&) const = default;
};

class ReaGate
{
public:
    static constexpr double maxPreOpenMs = 250.0, maxRmsMs = 1000.0;

    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const ReaGateParams&);
    int latencySamples() const noexcept     { return look_; }
    static int latencySamples (const ReaGateParams&, double sampleRate);

    void process (float* const* io, int numChannels, int numSamples);
    double lastGain() const noexcept        { return gain_; }

private:
    double sampleRate_ = 44100.0, gain_ = 0.0, openLin_ = 0.0, closeLin_ = 0.0, attackStep_ = 1.0, releaseStep_ = 1.0, rmsSum_[2] = { 0.0, 0.0 };
    bool instantAttack_ = true, instantRelease_ = true;
    long holdSamples_ = 0, holdLeft_ = 0;
    int look_ = 0, rmsN_ = 0;
    size_t writePos_ = 0, rmsPos_ = 0;
    bool open_ = false;
    ReaGateParams params_;
    std::array<DetectorFilter, 2> filter_;
    std::array<std::vector<float>, 2> line_;
    std::array<std::vector<double>, 2> rmsRing_;
};

//==============================================================================
/** Soft knee of ReaComp: the gain reduction (dB, for a ratio of infinity) at `over` dB above the threshold, K(over) = 0 below -W and over above +W.
    Built from the measured knee tables (W = 3, 6, 12, 24 dB). The default knee has K(x) = x + K(-x); K(0) is the mean-of-linear-gains law
    -20 log10 ((1 + 10^(-W/20)) / 2) (it matches the measurements to 0.002 dB), and the shape K(-x) / K(0) against x / W is interpolated from the
    tables (monotone cubic in x, linear in W between the tables; below 3 dB the 3 dB shape, above 24 dB the 24 dB shape). The "legacy" knee
    (Legacy Attack/Knee Options other than the default) only has a 12 dB table; its normalised shape is scaled to every W. */
class ReaKnee
{
public:
    ReaKnee (double kneeDb, bool legacy);
    double reduction (double overDb) const noexcept;
    double kneeDb() const noexcept          { return width_; }

private:
    double width_ = 0.0;
    std::vector<double> shape_;             // K(x) / W for x / W = -1 .. 1, evenly spaced
};

/** ReaComp behaviour: hard or soft knee, peak or boxcar-RMS detector (linked channels: the larger one), detector low-pass / high-pass, the
    attack / release envelope of the specification (see process()), pre-comp as a latency-compensated lookahead (the gain reduction starts that much before the loud audio, its release stays on the audio), make-up
    and dry / wet sum. */
struct ReaCompParams
{
    double thresholdDb = 0.0, ratio = 4.0, attackMs = 3.0, releaseMs = 100.0, preCompMs = 0.0, lowPassHz = 20000.0, highPassHz = 0.0;
    double rmsMs = 5.0, kneeDb = 0.0;
    bool legacyKnee = false;        // Legacy Attack/Knee Options: the legacy knee (floor (4 n) even)
    bool attackFromEnvelope = false;    // Legacy Attack/Knee Options n > 0.5: an attack starts from the current envelope, not from the threshold
    bool autoMakeup = false;        // make-up gain -threshold * (1 - 1 / ratio) / 2
    double dry = 0.0, wet = 1.0;    // amplitudes
    bool operator== (const ReaCompParams&) const = default;
    static constexpr double infiniteRatio = 99.0;       // the ratio knob shows "inf" from here
};

class ReaComp
{
public:
    static constexpr double maxPreCompMs = 250.0, maxRmsMs = 1000.0;

    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const ReaCompParams&);
    int latencySamples() const noexcept     { return look_; }
    static int latencySamples (const ReaCompParams&, double sampleRate);

    /** Gain (dB) of the static curve for a detector level, without make-up. */
    static double staticGainDb (const ReaCompParams&, double levelDb);
    static double makeupDb (const ReaCompParams&);

    void process (float* const* io, int numChannels, int numSamples);
    double lastGainReductionDb() const noexcept { return lastReductionDb_; }

private:
    double sampleRate_ = 44100.0, attackCoef_ = 0.0, releaseCoef_ = 0.0, env_ = 0.0, thresholdLin_ = 1.0, slope_ = 1.0, makeup_ = 0.0;
    double lastReductionDb_ = 0.0, rmsSum_[2] = { 0.0, 0.0 };
    int look_ = 0, rmsN_ = 0;
    size_t writePos_ = 0, rmsPos_ = 0;
    ReaCompParams params_;
    ReaKnee knee_ { 0.0, false };
    std::array<DetectorFilter, 2> filter_;
    std::array<std::vector<float>, 2> line_;
    std::array<std::vector<double>, 2> rmsRing_;
    // sliding minimum of the gain over the pre-comp window (monotonic queue in a ring): the gain falls early but is released on the audio
    std::vector<double> minVal_;
    std::vector<long long> minAt_;
    size_t minHead_ = 0, minCount_ = 0;
    long long sample_ = 0;
};

//==============================================================================
/** ReaLimit behaviour: the input is boosted by -threshold, limited so that sample peaks stay just below 0 dBFS (0.009 dB below), then scaled by the
    ceiling. The fast stage (linear amplitude) falls along a line that reaches the level each peak needs on the peak sample, 2 ms after it starts, and
    releases along a line back to 1 that ends 10 ms after the last over-threshold sample; it never releases in a gap if a peak needing as much (or
    more) reduction follows within 10 ms. The slow stage (dB) charges at 4 x the release rate while the fast stage reduces more than it does and
    releases linearly at `releaseDbPerSec` dB per second; the applied gain is the lower of the two stages. All times are milliseconds. The audio is
    delayed by the 10 ms window (reported as latency). */
struct ReaLimitParams
{
    double thresholdDb = 0.0, ceilingDb = 0.0;
    double releaseDbPerSec = 15.0;      // the knob's "inf" is 150 dB/s
    static constexpr double infiniteRelease = 150.0;
    bool operator== (const ReaLimitParams&) const = default;
};

class ReaLimit
{
public:
    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const ReaLimitParams&);
    int latencySamples() const noexcept     { return std::max (0, window_); }

    void process (float* const* io, int numChannels, int numSamples);
    double lastGainReductionDb() const noexcept { return lastReductionDb_; }

private:
    double sampleRate_ = 44100.0, boost_ = 1.0, out_ = 1.0, fast_ = 1.0, slowDb_ = 0.0, lastReductionDb_ = 0.0;
    int lookahead_ = 1, window_ = 1;        // 2 ms attack ramp, 10 ms hold / release window (samples)
    bool releasing_ = false;
    long long index_ = 0, lastOver_ = -1;   // output sample counter, index of the last sample that needed a reduction
    size_t pos_ = 0;
    ReaLimitParams params_;
    std::array<std::vector<float>, 2> audio_;
    std::vector<double> required_;
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

//==============================================================================
/** ReaVerbate behaviour (reaper_compat of the Rovox Verb): a Freeverb-type reverberator, but with 10 lowpass-feedback combs and 6 allpasses per channel,
    no mono sum of the inputs (each channel feeds only its own reverberator, the right one is spread by 23 samples at 44.1 kHz), input gain 0.015.
    Line length = floor((base + spread) * fs / 44100). All parameters are the normalized knob positions n (0..1) of the effect:
    comb feedback 0.3 + 0.68 n_room, damping 0.4 n_damp, wet and dry amplitude 2 n, width 2 n - 1; the pre-delay of the wet path is floor(n * 0.5 * fs)
    samples; low-pass / high-pass (wet only) are RBJ biquads with Q = 1 at 20000 n Hz, off at n = 1 / n = 0. */
struct ReaVerbParams
{
    double wet = 0.25, dry = 0.5, room = 0.294117647, damp = 0.5, width = 1.0, delay = 0.0, lowpass = 1.0, hipass = 0.0;
    bool operator== (const ReaVerbParams&) const = default;
};

class ReaVerb
{
public:
    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const ReaVerbParams&);
    void process (float* const* io, int numChannels, int numSamples);
    static double tailSeconds (const ReaVerbParams&);

    /** Length in samples of a line with this 44.1 kHz base length and stereo spread. */
    static long lineLength (int base, int spread, double sampleRate);

private:
    static constexpr size_t numCombs = 10, numAllpasses = 6;
    struct Comb { std::vector<double> buf; size_t pos = 0; double store = 0.0; };
    struct Allpass { std::vector<double> buf; size_t pos = 0; };
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run (double x) noexcept; };
    struct Channel
    {
        std::array<Comb, numCombs> combs;
        std::array<Allpass, numAllpasses> allpasses;
        std::vector<double> pre;
        size_t prePos = 0;
        Biquad lp, hp;
    };

    void designFilters();

    double sampleRate_ = 44100.0, feedback_ = 0.5, damp_ = 0.2;
    size_t preSamples_ = 0;
    bool lpOn_ = false, hpOn_ = false;
    ReaVerbParams params_;
    std::array<Channel, 2> ch_;
};

//==============================================================================
/** ReaFir (EQ mode) from its saved state. The curve is a list of (frequency Hz, gain dB) points: flat beyond the end points, a smoothstep in
    log-frequency between neighbours (approximate: the measured transition is within 0.35 dB of it). A flat curve is an exact gain without any latency
    (the REAPER render is the input once its reported latency is compensated). Any other curve is a zero-phase linear-phase FIR (frequency sampling on
    a grid of fftSize bins, cosine-tapered to +-fftSize/4 samples; approximate), delayed by latencySamples() = fftSize/4 which the host compensates. */
struct ReaFirPoint
{
    double freqHz = 1000.0, gainDb = 0.0;
    bool operator== (const ReaFirPoint&) const = default;
};

struct ReaFirParams
{
    int fftSize = 4096;
    std::vector<ReaFirPoint> points;        // sorted by frequency; empty = flat 0 dB
    double outputGain = 1.0;                // amplitude
    bool operator== (const ReaFirParams&) const = default;
};

class ReaFir
{
public:
    void prepare (double sampleRate);       // allocates
    void reset();
    void setParams (const ReaFirParams&);   // designs the FIR (allocates): not for the audio thread
    void process (float* const* io, int numChannels, int numSamples);

    int latencySamples() const              { return firActive_ ? half_ : 0; }
    bool isFlat() const                     { return ! firActive_; }
    static bool curveIsFlat (const std::vector<ReaFirPoint>&);
    /** Curve gain in dB at a frequency. */
    static double curveGainDb (const std::vector<ReaFirPoint>&, double hz);

private:
    double sampleRate_ = 44100.0, gain_ = 1.0;
    bool firActive_ = false;
    int half_ = 0;                          // FIR support is -half_ .. +half_
    ReaFirParams params_;
    std::vector<double> taps_;              // 2 * half_ + 1
    std::array<std::vector<double>, 2> history_;
    size_t pos_ = 0;
};

} // namespace rovox::fx
