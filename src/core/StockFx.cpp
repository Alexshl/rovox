// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "StockFx.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace rovox::fx
{

namespace
{
constexpr double pi = 3.14159265358979323846;

double dbToLin (double db)                  { return std::pow (10.0, db / 20.0); }
double dbToLinOrZero (double db)            { return db <= -96.0 ? 0.0 : dbToLin (db); }
double linToDb (double lin)                 { return 20.0 * std::log10 (std::max (lin, 1.0e-9)); }
double flush (double v)                     { return std::abs (v) < 1.0e-25 ? 0.0 : v; }
float toFloat (double v)                   { return std::abs (v) < 1.0e-30 ? 0.0f : (float) v; }     // no subnormal floats leave an effect
double timeCoef (double ms, double sr)      { return ms <= 0.01 ? 0.0 : std::exp (-1.0 / (ms * 0.001 * sr)); }
double onePole (double hz, double sr)       { return 1.0 - std::exp (-2.0 * pi * std::clamp (hz, 1.0, 0.45 * sr) / sr); }
} // namespace

//==============================================================================
BiquadCoeffs designBiquad (BandType type, double sr, double freqHz, double gainDb, double bwOct)
{
    const double f = std::clamp (freqHz, 5.0, 0.49 * sr);
    const double bw = std::clamp (bwOct, 0.01, 10.0);
    const double q = std::sqrt (std::pow (2.0, bw)) / (std::pow (2.0, bw) - 1.0);
    const double w0 = 2.0 * pi * f / sr, cosw = std::cos (w0), sinw = std::sin (w0);
    const double alpha = sinw / (2.0 * q);
    const double A = std::pow (10.0, gainDb / 40.0), sqA2alpha = 2.0 * std::sqrt (A) * alpha;
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type)
    {
        case BandType::Peak:
            b0 = 1.0 + alpha * A;  b1 = -2.0 * cosw;  b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;  a1 = -2.0 * cosw;  a2 = 1.0 - alpha / A;
            break;
        case BandType::LowShelf:
            b0 = A * ((A + 1.0) - (A - 1.0) * cosw + sqA2alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cosw - sqA2alpha);
            a0 = (A + 1.0) + (A - 1.0) * cosw + sqA2alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw);
            a2 = (A + 1.0) + (A - 1.0) * cosw - sqA2alpha;
            break;
        case BandType::HighShelf:
            b0 = A * ((A + 1.0) + (A - 1.0) * cosw + sqA2alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosw - sqA2alpha);
            a0 = (A + 1.0) - (A - 1.0) * cosw + sqA2alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw);
            a2 = (A + 1.0) - (A - 1.0) * cosw - sqA2alpha;
            break;
        case BandType::LowPass:
            b0 = (1.0 - cosw) / 2.0;  b1 = 1.0 - cosw;  b2 = b0;
            a0 = 1.0 + alpha;  a1 = -2.0 * cosw;  a2 = 1.0 - alpha;
            break;
        case BandType::HighPass:
            b0 = (1.0 + cosw) / 2.0;  b1 = -(1.0 + cosw);  b2 = b0;
            a0 = 1.0 + alpha;  a1 = -2.0 * cosw;  a2 = 1.0 - alpha;
            break;
        case BandType::Notch:
            b0 = 1.0;  b1 = -2.0 * cosw;  b2 = 1.0;
            a0 = 1.0 + alpha;  a1 = -2.0 * cosw;  a2 = 1.0 - alpha;
            break;
        case BandType::BandPass:
            b0 = alpha;  b1 = 0.0;  b2 = -alpha;
            a0 = 1.0 + alpha;  a1 = -2.0 * cosw;  a2 = 1.0 - alpha;
            break;
        case BandType::AllPass:
        case BandType::PeakAlt2:
        case BandType::ParallelBandPass:
            return designReaperBiquad (type, sr, freqHz, gainDb, bwOct);       // only exist as ReaEQ types
    }

    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

BiquadCoeffs designReaperBiquad (BandType type, double sr, double freqHz, double gainDb, double bwOct)
{
    const double f = std::clamp (freqHz, 5.0, 0.49 * sr);
    const double bw = std::max (bwOct, 1.0e-3);
    const double w0 = 2.0 * pi * f / sr, c = std::cos (w0), s = std::sin (w0);
    const double alpha = s * std::sinh (0.5 * std::log (2.0) * bw * w0 / s);
    const double A = std::pow (10.0, gainDb / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type)
    {
        case BandType::Peak:
            b0 = 1.0 + alpha * A;  b1 = -2.0 * c;  b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;  a1 = -2.0 * c;  a2 = 1.0 - alpha / A;
            break;
        case BandType::PeakAlt2:
            b0 = 1.0 + alpha * A * A;  b1 = -2.0 * c;  b2 = 1.0 - alpha * A * A;
            a0 = 1.0 + alpha;          a1 = -2.0 * c;  a2 = 1.0 - alpha;
            break;
        case BandType::LowShelf:
        case BandType::HighShelf:
        {
            const double slope = std::min (1.0 / (bw * bw), 1.2);
            const double as = 0.5 * s * std::sqrt (std::max (0.0, (A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0));
            const double beta = 2.0 * std::sqrt (A) * as;

            if (type == BandType::LowShelf)
            {
                b0 = A * ((A + 1.0) - (A - 1.0) * c + beta);
                b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * c);
                b2 = A * ((A + 1.0) - (A - 1.0) * c - beta);
                a0 = (A + 1.0) + (A - 1.0) * c + beta;
                a1 = -2.0 * ((A - 1.0) + (A + 1.0) * c);
                a2 = (A + 1.0) + (A - 1.0) * c - beta;
            }
            else
            {
                b0 = A * ((A + 1.0) + (A - 1.0) * c + beta);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c);
                b2 = A * ((A + 1.0) + (A - 1.0) * c - beta);
                a0 = (A + 1.0) - (A - 1.0) * c + beta;
                a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c);
                a2 = (A + 1.0) - (A - 1.0) * c - beta;
            }

            break;
        }
        case BandType::LowPass:
            b0 = (1.0 - c) / 2.0;  b1 = 1.0 - c;  b2 = b0;
            a0 = 1.0 + alpha;  a1 = -2.0 * c;  a2 = 1.0 - alpha;
            break;
        case BandType::HighPass:
            b0 = (1.0 + c) / 2.0;  b1 = -(1.0 + c);  b2 = b0;
            a0 = 1.0 + alpha;  a1 = -2.0 * c;  a2 = 1.0 - alpha;
            break;
        case BandType::AllPass:
            b0 = 1.0 - alpha;  b1 = -2.0 * c;  b2 = 1.0 + alpha;
            a0 = 1.0 + alpha;  a1 = -2.0 * c;  a2 = 1.0 - alpha;
            break;
        case BandType::Notch:
            b0 = 1.0;  b1 = -2.0 * c;  b2 = 1.0;
            a0 = 1.0 + alpha;  a1 = -2.0 * c;  a2 = 1.0 - alpha;
            break;
        case BandType::BandPass:
        case BandType::ParallelBandPass:
            b0 = alpha;  b1 = 0.0;  b2 = -alpha;
            a0 = 1.0 + alpha;  a1 = -2.0 * c;  a2 = 1.0 - alpha;
            break;
    }

    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

namespace
{
std::complex<double> biquadResponse (const BiquadCoeffs& c, double sr, double freqHz)
{
    const double w = 2.0 * pi * freqHz / sr;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}
} // namespace

double biquadMagnitudeDb (const BiquadCoeffs& c, double sr, double freqHz)
{
    return 20.0 * std::log10 (std::max (std::abs (biquadResponse (c, sr, freqHz)), 1.0e-12));
}

//==============================================================================
void Eq::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    reset();
    setParams (params_);
}

void Eq::reset()
{
    for (auto& band : state_)
        for (auto& s : band)
            s = {};
}

void Eq::setParams (const EqParams& p)
{
    params_ = p;
    outGain_ = dbToLin (p.outputGainDb);

    for (size_t i = 0; i < EqParams::maxBands; ++i)
    {
        const auto& b = p.bands[i];
        const bool gainType = b.type == BandType::Peak || b.type == BandType::LowShelf || b.type == BandType::HighShelf || b.type == BandType::PeakAlt2;
        active_[i] = b.enabled && ! (gainType && std::abs (b.gainDb) < 1.0e-9);
        coeffs_[i] = p.reaperCompat ? designReaperBiquad (b.type, sampleRate_, b.freqHz, b.gainDb, b.bandwidthOct)
                                    : designBiquad (b.type, sampleRate_, b.freqHz, b.gainDb, b.bandwidthOct);
    }
}

double Eq::magnitudeDb (double freqHz) const
{
    double sum = 0.0;
    std::complex<double> parallel (0.0, 0.0);
    bool anyParallel = false;

    for (size_t i = 0; i < EqParams::maxBands; ++i)
    {
        if (! active_[i])
            continue;

        if (isParallel (params_.bands[i]))
        {
            parallel += biquadResponse (coeffs_[i], sampleRate_, freqHz);
            anyParallel = true;
        }
        else
        {
            sum += biquadMagnitudeDb (coeffs_[i], sampleRate_, freqHz);
        }
    }

    if (anyParallel)
        sum += 20.0 * std::log10 (std::max (std::abs (parallel), 1.0e-12));

    return sum;
}

void Eq::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min(numChannels, 2);
    bool anyParallel = false;

    for (size_t b = 0; b < EqParams::maxBands; ++b)
        anyParallel = anyParallel || (active_[b] && isParallel (params_.bands[b]));

    for (int c = 0; c < nch; ++c)
    {
        float* x = io[c];

        if (anyParallel)
        {
            // the parallel band-pass bands all see the input; their sum is the signal that goes on (in chunks, so that nothing is allocated)
            constexpr int chunk = 256;
            float in[chunk];

            for (int start = 0; start < numSamples; start += chunk)
            {
                const int len = std::min (chunk, numSamples - start);
                double sum[chunk] = {};
                std::copy (x + start, x + start + len, in);

                for (size_t b = 0; b < EqParams::maxBands; ++b)
                {
                    if (! active_[b] || ! isParallel (params_.bands[b]))
                        continue;

                    const auto k = coeffs_[b];
                    auto& s = state_[b][(size_t) c];
                    double z1 = s.z1, z2 = s.z2;

                    for (int i = 0; i < len; ++i)
                    {
                        const double v = in[i];
                        const double out = k.b0 * v + z1;
                        z1 = k.b1 * v - k.a1 * out + z2;
                        z2 = k.b2 * v - k.a2 * out;
                        sum[i] += toFloat (out);
                    }

                    s.z1 = z1;
                    s.z2 = z2;
                }

                for (int i = 0; i < len; ++i)
                    x[start + i] = toFloat (sum[i]);
            }
        }

        for (size_t b = 0; b < EqParams::maxBands; ++b)
        {
            if (! active_[b] || isParallel (params_.bands[b]))
                continue;

            const auto k = coeffs_[b];
            auto& s = state_[b][(size_t) c];
            double z1 = s.z1, z2 = s.z2;

            for (int i = 0; i < numSamples; ++i)
            {
                const double in = x[i];
                const double out = k.b0 * in + z1;
                z1 = k.b1 * in - k.a1 * out + z2;
                z2 = k.b2 * in - k.a2 * out;
                x[i] = toFloat (out);
            }

            s.z1 = z1;
            s.z2 = z2;
        }

        if (outGain_ != 1.0)
            for (int i = 0; i < numSamples; ++i)
                x[i] = toFloat (x[i] * outGain_);
    }
}

//==============================================================================
void Comp::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    for (auto& l : line_)
        l.assign ((size_t) std::ceil (maxLookaheadMs * 0.001 * sampleRate) + 2, 0.0f);

    reset();
    setParams (params_);
}

void Comp::reset()
{
    for (auto& l : line_)
        std::fill (l.begin(), l.end(), 0.0f);

    writePos_ = 0;
    ms_ = 0.0;
    gr_ = 0.0;
    lastReductionDb_ = 0.0;
}

void Comp::setParams (const CompParams& p)
{
    params_ = p;
    params_.ratio = std::max (1.0, p.ratio);
    params_.kneeDb = std::max (0.0, p.kneeDb);
    attackCoef_ = timeCoef (p.attackMs, sampleRate_);
    releaseCoef_ = timeCoef (p.releaseMs, sampleRate_);
    rmsCoef_ = p.rmsMs > 0.0 ? timeCoef (p.rmsMs, sampleRate_) : 0.0;
    const int look = std::clamp ((int) std::lround (p.lookaheadMs * 0.001 * sampleRate_), 0, std::max (0, (int) (line_[0].size()) - 2));

    if (look != lookSamples_)
    {
        lookSamples_ = look;

        for (auto& l : line_)
            std::fill (l.begin(), l.end(), 0.0f);

        writePos_ = 0;
    }
}

double Comp::gainReductionDb (const CompParams& p, double inputDb)
{
    const double slope = 1.0 - 1.0 / std::max (1.0, p.ratio);
    const double over = inputDb - p.thresholdDb;

    if (p.kneeDb <= 0.0)
        return over > 0.0 ? slope * over : 0.0;

    if (2.0 * over < -p.kneeDb)
        return 0.0;

    if (2.0 * std::abs (over) <= p.kneeDb)
    {
        const double t = over + p.kneeDb / 2.0;
        return slope * t * t / (2.0 * p.kneeDb);
    }

    return slope * over;
}

void Comp::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const double autoMakeup = params_.autoMakeup ? std::max (0.0, -params_.thresholdDb * (1.0 - 1.0 / params_.ratio)) : 0.0;
    const double makeup = params_.makeupDb + autoMakeup;
    const size_t cap = (size_t) lookSamples_;
    double worst = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double lvl = 0.0, sumsq = 0.0;

        for (int c = 0; c < nch; ++c)
        {
            const double v = io[c][i];
            lvl = std::max (lvl, std::abs (v));
            sumsq += v * v;
        }

        if (params_.rmsMs > 0.0)
        {
            ms_ = flush (rmsCoef_ * ms_ + (1.0 - rmsCoef_) * sumsq / nch);
            lvl = std::sqrt (ms_);
        }

        const double target = gainReductionDb (params_, linToDb (lvl));
        const double coef = target > gr_ ? attackCoef_ : releaseCoef_;
        gr_ = target + coef * (gr_ - target);

        if (gr_ < 1.0e-9)
            gr_ = 0.0;

        worst = std::max (worst, gr_);
        const double gain = dbToLin (makeup - gr_);

        for (int c = 0; c < nch; ++c)
        {
            const float in = io[c][i];
            float delayed = in;

            if (cap > 0)
            {
                delayed = line_[(size_t) c][writePos_];
                line_[(size_t) c][writePos_] = in;
            }

            const double wet = (double) delayed * gain;
            io[c][i] = toFloat (params_.mix >= 1.0 ? wet : params_.mix * wet + (1.0 - params_.mix) * (double) delayed);
        }

        if (cap > 0 && ++writePos_ >= cap)
            writePos_ = 0;
    }

    lastReductionDb_ = worst;
}

//==============================================================================
void Gate::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    detCoef_ = timeCoef (5.0, sampleRate);
    reset();
    setParams (params_);
}

void Gate::reset()
{
    env_ = 0.0;
    open_ = false;
    holdLeft_ = 0;
    gain_ = rangeLin_;
}

void Gate::setParams (const GateParams& p)
{
    params_ = p;
    attackCoef_ = timeCoef (p.attackMs, sampleRate_);
    releaseCoef_ = timeCoef (p.releaseMs, sampleRate_);
    holdSamples_ = (long) std::lround (std::max (0.0, p.holdMs) * 0.001 * sampleRate_);
    const bool wasInitial = gain_ == rangeLin_ && ! open_;
    rangeLin_ = dbToLinOrZero (std::min (0.0, p.rangeDb));

    if (wasInitial)
        gain_ = rangeLin_;
}

double Gate::gainDb() const noexcept
{
    return linToDb (gain_);
}

void Gate::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);

    for (int i = 0; i < numSamples; ++i)
    {
        double lvl = 0.0;

        for (int c = 0; c < nch; ++c)
            lvl = std::max (lvl, (double) std::abs (io[c][i]));

        env_ = flush (lvl > env_ ? lvl : env_ * detCoef_);
        const double envDb = linToDb (env_);

        if (envDb > params_.thresholdDb)
        {
            open_ = true;
            holdLeft_ = holdSamples_;
        }
        else if (open_)
        {
            if (envDb >= params_.thresholdDb - std::max (0.0, params_.hysteresisDb))
                holdLeft_ = holdSamples_;       // inside the hysteresis band: stays open
            else if (holdLeft_ > 0)
                --holdLeft_;
            else
                open_ = false;
        }

        const double target = open_ ? 1.0 : rangeLin_;
        const double coef = target > gain_ ? attackCoef_ : releaseCoef_;
        gain_ = target + coef * (gain_ - target);

        if (std::abs (gain_ - target) < 1.0e-9)
            gain_ = target;

        for (int c = 0; c < nch; ++c)
            io[c][i] = toFloat (io[c][i] * gain_);
    }
}

//==============================================================================
void Delay::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    for (auto& l : line_)
        l.assign ((size_t) std::ceil (maxSeconds * sampleRate) + 4, 0.0f);

    reset();
    setParams (params_);
}

void Delay::reset()
{
    for (auto& l : line_)
        std::fill (l.begin(), l.end(), 0.0f);

    writePos_ = 0;
    filter_ = {};
    snap_ = true;
}

double Delay::tapSamples (int t) const
{
    const auto& tap = params_.taps[(size_t) t];
    const double ms = tap.sync ? tap.beats * 60000.0 / std::max (20.0, params_.bpm) : tap.timeMs;
    const double maxSamples = (double) line_[0].size() - 3.0;
    return std::clamp (ms * 0.001 * sampleRate_, 1.0, std::max (1.0, maxSamples));
}

void Delay::setParams (const DelayParams& p)
{
    params_ = p;

    for (int t = 0; t < DelayParams::maxTaps; ++t)
    {
        target_[(size_t) t] = tapSamples (t);

        if (snap_)
            current_[(size_t) t] = target_[(size_t) t];
    }
}

double Delay::tailSeconds (const DelayParams& p)
{
    const auto seconds = [&] (const DelayTap& tap)
    {
        return std::min (maxSeconds, (tap.sync ? tap.beats * 60.0 / std::max (20.0, p.bpm) : tap.timeMs * 0.001));
    };

    double longest = seconds (p.taps[0]);

    for (const auto& tap : p.taps)
        if (tap.enabled)
            longest = std::max (longest, seconds (tap));

    const double fb = std::min (0.95, p.feedbackPct / 100.0);
    const double repeats = fb > 0.001 ? std::min (60.0, std::ceil (60.0 / (-20.0 * std::log10 (fb)))) : 0.0;
    return std::min (60.0, longest + repeats * seconds (p.taps[0]));
}

void Delay::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const size_t cap = line_[0].size();
    const double wetGain = dbToLinOrZero (params_.wetDb), dryGain = dbToLinOrZero (params_.dryDb);
    const double fb = std::clamp (params_.feedbackPct / 100.0, 0.0, 0.95);
    const bool lpOn = params_.lowPassHz < 19999.0, hpOn = params_.highPassHz > 20.0001;
    const double lpA = onePole (params_.lowPassHz, sampleRate_), hpA = onePole (params_.highPassHz, sampleRate_);
    double tapGain[DelayParams::maxTaps][2];

    for (int t = 0; t < DelayParams::maxTaps; ++t)
    {
        const auto& tap = params_.taps[(size_t) t];
        const double g = tap.enabled ? dbToLin (tap.gainDb) : 0.0;
        tapGain[t][0] = g * (tap.pan > 0.0 ? 1.0 - tap.pan : 1.0);
        tapGain[t][1] = g * (tap.pan < 0.0 ? 1.0 + tap.pan : 1.0);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        for (size_t t = 0; t < DelayParams::maxTaps; ++t)
        {
            const double d = target_[t] - current_[t];
            current_[t] = std::abs (d) <= 0.25 ? target_[t] : current_[t] + (d > 0 ? 0.25 : -0.25);
        }

        for (int c = 0; c < nch; ++c)
        {
            auto& line = line_[(size_t) c];
            auto& f = filter_[(size_t) c];
            const double x = io[c][i];
            double wet = 0.0, fbSource = 0.0;

            for (size_t t = 0; t < DelayParams::maxTaps; ++t)
            {
                double pos = (double) writePos_ - current_[t];

                if (pos < 0.0)
                    pos += (double) cap;

                const size_t i0 = std::min ((size_t) pos, cap - 1);
                const size_t i1 = i0 + 1 >= cap ? 0 : i0 + 1;
                const double frac = pos - (double) i0;
                const double v = frac == 0.0 ? (double) line[i0] : (double) line[i0] * (1.0 - frac) + (double) line[i1] * frac;

                if (t == 0)
                    fbSource = v;

                wet += v * tapGain[t][c];
            }

            if (lpOn)
            {
                f.lp += lpA * (fbSource - f.lp);
                f.lp = flush (f.lp);
                fbSource = f.lp;
            }

            if (hpOn)
            {
                f.hpLp += hpA * (fbSource - f.hpLp);
                f.hpLp = flush (f.hpLp);
                fbSource -= f.hpLp;
            }

            line[writePos_] = (float) flush (x + fbSource * fb);
            io[c][i] = toFloat (x * dryGain + wet * wetGain);
        }

        if (++writePos_ >= cap)
            writePos_ = 0;
    }

    snap_ = false;      // the delay times glide from now on
}

//==============================================================================
double ReaDelay::Biquad::run (double x) noexcept
{
    const double out = b0 * x + z1;
    z1 = flush (b1 * x - a1 * out + z2);
    z2 = flush (b2 * x - a2 * out);
    return out;
}

namespace
{
/** RBJ low-pass / high-pass with Q = 1 (0 dB at the corner frequency). */
void designLoopFilter (bool lowPass, double sr, double hz, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    const double w0 = 2.0 * pi * std::clamp (hz, 1.0, 0.49 * sr) / sr, c = std::cos (w0), alpha = std::sin (w0) / 2.0;
    const double a0 = 1.0 + alpha;
    b0 = (lowPass ? (1.0 - c) : (1.0 + c)) / 2.0 / a0;
    b1 = (lowPass ? (1.0 - c) : -(1.0 + c)) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
}
} // namespace

void ReaDelay::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    for (auto& tap : loop_)
        for (auto& l : tap)
            l.line.assign ((size_t) std::ceil (maxSeconds * sampleRate) + 4, 0.0f);

    reset();
    setParams (params_);
}

void ReaDelay::reset()
{
    for (auto& tap : loop_)
        for (auto& l : tap)
        {
            std::fill (l.line.begin(), l.line.end(), 0.0f);
            l.lp.z1 = l.lp.z2 = l.hp.z1 = l.hp.z2 = 0.0;
        }

    writePos_ = 0;
}

long ReaDelay::delaySamples (const ReaDelayTap& tap, double bpm, double sampleRate)
{
    const double eighthMs = 30000.0 / std::max (20.0, bpm);
    const double ms = std::max (0.0, tap.timeMs) + std::max (0.0, tap.musical) * eighthMs;
    return std::max (1L, std::lround (ms * sampleRate / 1000.0));
}

void ReaDelay::setParams (const ReaDelayParams& p)
{
    params_ = p;

    for (size_t t = 0; t < (size_t) ReaDelayParams::maxTaps; ++t)
    {
        const auto& tap = p.taps[t];
        const long cap = loop_[t][0].line.empty() ? 0 : (long) loop_[t][0].line.size() - 2;
        delay_[t] = cap > 0 ? std::min (delaySamples (tap, p.bpm, sampleRate_), cap) : 1;

        for (auto& l : loop_[t])
        {
            if (tap.lowPassHz < 20000.0)
                designLoopFilter (true, sampleRate_, tap.lowPassHz, l.lp.b0, l.lp.b1, l.lp.b2, l.lp.a1, l.lp.a2);
            else
                l.lp = { 1, 0, 0, 0, 0, l.lp.z1, l.lp.z2 };

            if (tap.highPassHz > 0.0)
                designLoopFilter (false, sampleRate_, tap.highPassHz, l.hp.b0, l.hp.b1, l.hp.b2, l.hp.a1, l.hp.a2);
            else
                l.hp = { 1, 0, 0, 0, 0, l.hp.z1, l.hp.z2 };
        }
    }
}

double ReaDelay::tailSeconds (const ReaDelayParams& p)
{
    double longest = 0.0;

    for (const auto& tap : p.taps)
    {
        if (! tap.enabled)
            continue;

        const double d = (double) delaySamples (tap, p.bpm, 48000.0) / 48000.0;
        const double fb = std::min (0.999, tap.feedback);
        const double repeats = fb > 0.001 ? std::min (60.0, std::ceil (60.0 / (-20.0 * std::log10 (fb)))) : 0.0;
        longest = std::max (longest, d * (1.0 + repeats));
    }

    return std::min (60.0, longest);
}

void ReaDelay::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);

    if (loop_[0][0].line.empty())
        return;

    const size_t cap = loop_[0][0].line.size();

    for (int i = 0; i < numSamples; ++i)
    {
        double x[2] = { io[0][i], nch > 1 ? (double) io[1][i] : 0.0 };
        double sum[2] = { 0.0, 0.0 };

        for (size_t t = 0; t < (size_t) ReaDelayParams::maxTaps; ++t)
        {
            const auto& tap = params_.taps[t];

            if (! tap.enabled)
                continue;

            double p[2] = { 0.0, 0.0 };
            const size_t readPos = (writePos_ + cap - (size_t) delay_[t]) % cap;

            for (int c = 0; c < nch; ++c)
            {
                auto& l = loop_[t][(size_t) c];
                double v = l.hp.run (l.lp.run ((double) l.line[readPos]));
                l.line[writePos_] = toFloat (flush (x[c] + tap.feedback * v));
                p[c] = v;
            }

            if (tap.bits < 24)
            {
                const double step = std::ldexp (1.0, tap.bits);
                p[0] = std::trunc (p[0] * step) / step;
                p[1] = std::trunc (p[1] * step) / step;
            }

            if (nch > 1)
            {
                const double mid = 0.5 * (p[0] + p[1]), side = 0.5 * (p[0] - p[1]);
                p[0] = mid + tap.width * side;
                p[1] = mid - tap.width * side;
            }

            sum[0] += p[0] * tap.volume * (tap.pan > 0.0 ? 1.0 - tap.pan : 1.0);
            sum[1] += p[1] * tap.volume * (tap.pan < 0.0 ? 1.0 + tap.pan : 1.0);
        }

        for (int c = 0; c < nch; ++c)
            io[c][i] = toFloat (x[c] * params_.dry + sum[c] * params_.wet);

        if (++writePos_ >= cap)
            writePos_ = 0;
    }
}

//==============================================================================
namespace
{
constexpr int combTunings[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr int allpassTunings[4] = { 556, 441, 341, 225 };
constexpr int stereoSpread = 23;
}

void Verb::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    const double scale = sampleRate / 44100.0;

    for (size_t c = 0; c < 2; ++c)
    {
        const int spread = c == 0 ? 0 : stereoSpread;

        for (size_t i = 0; i < 8; ++i)
            ch_[c].combs[i].buf.assign ((size_t) std::max (1L, std::lround ((combTunings[i] + spread) * scale)), 0.0f);

        for (size_t i = 0; i < 4; ++i)
            ch_[c].allpasses[i].buf.assign ((size_t) std::max (1L, std::lround ((allpassTunings[i] + spread) * scale)), 0.0f);
    }

    ch_[0].pre.assign ((size_t) std::ceil (maxPreDelayMs * 0.001 * sampleRate) + 2, 0.0f);
    reset();
    setParams (params_);
}

void Verb::reset()
{
    for (auto& c : ch_)
    {
        for (auto& comb : c.combs)
        {
            std::fill (comb.buf.begin(), comb.buf.end(), 0.0f);
            comb.pos = 0;
            comb.store = 0.0;
        }

        for (auto& ap : c.allpasses)
        {
            std::fill (ap.buf.begin(), ap.buf.end(), 0.0f);
            ap.pos = 0;
        }

        std::fill (c.pre.begin(), c.pre.end(), 0.0f);
        c.prePos = 0;
        c.lp = c.hpLp = 0.0;
    }
}

void Verb::setParams (const VerbParams& p)
{
    params_ = p;
    feedback_ = std::clamp (p.roomSize, 0.0, 1.0) * 0.28 + 0.7;
    damp1_ = std::clamp (p.damping, 0.0, 1.0) * 0.4;
    damp2_ = 1.0 - damp1_;
    preSamples_ = (size_t) std::clamp ((long) std::lround (p.preDelayMs * 0.001 * sampleRate_), 0L, (long) ch_[0].pre.size() - 2);
}

double Verb::tailSeconds (const VerbParams& p)
{
    const double feedback = std::clamp (p.roomSize, 0.0, 1.0) * 0.28 + 0.7;
    const double loop = 1400.0 / 44100.0;
    return std::min (60.0, -3.0 * loop / std::log10 (feedback) + p.preDelayMs * 0.001);
}

void Verb::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const double wetGain = dbToLinOrZero (params_.wetDb) * 3.0, dryGain = dbToLinOrZero (params_.dryDb);
    const double width = std::clamp (params_.width, 0.0, 1.0);
    const double wet1 = width * 0.5 + 0.5, wet2 = (1.0 - width) * 0.5;
    const bool hpOn = params_.lowCutHz > 20.0001, lpOn = params_.highCutHz < 19999.0;
    const double hpA = onePole (params_.lowCutHz, sampleRate_), lpA = onePole (params_.highCutHz, sampleRate_);
    auto& pre = ch_[0].pre;
    const size_t preCap = pre.size();

    for (int i = 0; i < numSamples; ++i)
    {
        const double inL = io[0][i], inR = nch > 1 ? io[1][i] : inL;
        const double mono = (inL + inR) * 0.015;
        double fed = mono;

        if (preSamples_ > 0)
        {
            fed = pre[(ch_[0].prePos + preCap - preSamples_) % preCap];
            pre[ch_[0].prePos] = (float) mono;
        }

        ch_[0].prePos = (ch_[0].prePos + 1) % preCap;
        double w[2];

        for (size_t c = 0; c < 2; ++c)
        {
            double sum = 0.0;

            for (auto& comb : ch_[c].combs)
            {
                const double out = comb.buf[comb.pos];
                comb.store = flush (out * damp2_ + comb.store * damp1_);
                comb.buf[comb.pos] = (float) flush (fed + comb.store * feedback_);

                if (++comb.pos >= comb.buf.size())
                    comb.pos = 0;

                sum += out;
            }

            for (auto& ap : ch_[c].allpasses)
            {
                const double bufOut = ap.buf[ap.pos];
                const double out = -sum + bufOut;
                ap.buf[ap.pos] = (float) flush (sum + bufOut * 0.5);
                sum = out;

                if (++ap.pos >= ap.buf.size())
                    ap.pos = 0;
            }

            w[c] = sum;
        }

        double y[2] = { w[0] * wet1 + w[1] * wet2, w[1] * wet1 + w[0] * wet2 };

        for (size_t c = 0; c < 2; ++c)
        {
            auto& s = ch_[c];

            if (hpOn)
            {
                s.hpLp = flush (s.hpLp + hpA * (y[c] - s.hpLp));
                y[c] -= s.hpLp;
            }

            if (lpOn)
            {
                s.lp = flush (s.lp + lpA * (y[c] - s.lp));
                y[c] = s.lp;
            }
        }

        if (nch > 1)
        {
            io[0][i] = toFloat (inL * dryGain + y[0] * wetGain);
            io[1][i] = toFloat (inR * dryGain + y[1] * wetGain);
        }
        else
        {
            io[0][i] = toFloat (inL * dryGain + 0.5 * (y[0] + y[1]) * wetGain);
        }
    }
}

//==============================================================================
double DetectorFilter::Biquad::run (double x) noexcept
{
    const double out = b0 * x + z1;
    z1 = flush (b1 * x - a1 * out + z2);
    z2 = flush (b2 * x - a2 * out);
    return out;
}

void DetectorFilter::set (double sampleRate, double lowPassHz, double highPassHz)
{
    lpOn_ = lowPassHz > 0.0 && lowPassHz < 20000.0 && lowPassHz < 0.49 * sampleRate;
    hpOn_ = highPassHz > 0.5;

    if (lpOn_)
        designLoopFilter (true, sampleRate, lowPassHz, lp_.b0, lp_.b1, lp_.b2, lp_.a1, lp_.a2);

    if (hpOn_)
        designLoopFilter (false, sampleRate, highPassHz, hp_.b0, hp_.b1, hp_.b2, hp_.a1, hp_.a2);
}

void DetectorFilter::reset() noexcept
{
    lp_.z1 = lp_.z2 = hp_.z1 = hp_.z2 = 0.0;
}

double DetectorFilter::run (double x) noexcept
{
    return hpOn_ ? hp_.run (lpOn_ ? lp_.run (x) : x) : (lpOn_ ? lp_.run (x) : x);
}

namespace
{
/** The 0.5 ms offset of the ReaGate timings: 24 samples at 48 kHz, fractional at other rates (measured at 44.1, 48 and 96 kHz). */
double gateOffsetSamples (double sampleRate)            { return sampleRate / 2000.0; }
long roundSamples (double v)                            { return v > 0.0 ? std::lround (v) : 0L; }
/** A time in ms as a (fractional) sample count minus the offset; 0 when the time does not exceed the offset. */
double gateSamples (double ms, double sampleRate)
{
    const double v = ms * sampleRate / 1000.0;
    return v > gateOffsetSamples (sampleRate) ? v - gateOffsetSamples (sampleRate) : 0.0;
}
}

//==============================================================================
int ReaGate::latencySamples (const ReaGateParams& p, double sampleRate)
{
    const double ms = std::clamp (p.preOpenMs, 0.0, maxPreOpenMs);
    return (int) std::floor (gateSamples (ms, sampleRate));
}

void ReaGate::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    for (auto& l : line_)
        l.assign ((size_t) std::ceil (maxPreOpenMs * sampleRate / 1000.0) + 8, 0.0f);

    for (auto& r : rmsRing_)
        r.assign ((size_t) std::ceil (maxRmsMs * sampleRate / 1000.0) + 8, 0.0);

    reset();
    setParams (params_);
}

void ReaGate::reset()
{
    for (auto& l : line_)
        std::fill (l.begin(), l.end(), 0.0f);

    for (auto& r : rmsRing_)
        std::fill (r.begin(), r.end(), 0.0);

    for (auto& f : filter_)
        f.reset();

    rmsSum_[0] = rmsSum_[1] = 0.0;
    writePos_ = rmsPos_ = 0;
    gain_ = 0.0;
    holdLeft_ = 0;
    open_ = false;
}

void ReaGate::setParams (const ReaGateParams& p)
{
    params_ = p;
    const double d = gateOffsetSamples (sampleRate_), ms = sampleRate_ / 1000.0;
    look_ = latencySamples (p, sampleRate_);
    openLin_ = p.thresholdDb <= -149.0 ? 0.0 : dbToLin (p.thresholdDb);
    closeLin_ = p.thresholdDb <= -149.0 || p.hysteresisDb <= -99.0 ? 0.0 : dbToLin (p.thresholdDb + p.hysteresisDb);

    const double attack = gateSamples (p.attackMs, sampleRate_), release = gateSamples (p.releaseMs, sampleRate_), hold = gateSamples (p.holdMs, sampleRate_);
    instantAttack_ = attack <= 0.0;
    instantRelease_ = release <= 0.0;
    attackStep_ = instantAttack_ ? 1.0 : 1.0 / attack;
    releaseStep_ = instantRelease_ ? 1.0 : 1.0 / release;
    // the attack time is added to the hold
    holdSamples_ = (long) std::floor (hold) + std::max ((long) std::floor (attack) - 1, 0L) + look_;       // closing stays on the audio when the audio is delayed for the pre-open

    const int n = p.rmsMs > 0.0 ? (int) std::clamp (roundSamples (std::min (p.rmsMs, maxRmsMs) * ms - d), 1L, (long) rmsRing_[0].size() - 1) : 0;

    if (n != rmsN_)
    {
        rmsN_ = n;
        rmsSum_[0] = rmsSum_[1] = 0.0;
        rmsPos_ = 0;

        for (auto& r : rmsRing_)
            std::fill (r.begin(), r.end(), 0.0);
    }

    for (auto& f : filter_)
        f.set (sampleRate_, p.lowPassHz, p.highPassHz);
}

void ReaGate::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const size_t cap = line_[0].size();

    if (cap == 0 || nch < 1)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        double x[2] = { io[0][i], nch > 1 ? (double) io[1][i] : 0.0 };
        double level = 0.0;

        for (int c = 0; c < nch; ++c)
        {
            const double d = filter_[(size_t) c].run (x[c]);

            if (rmsN_ > 0)
            {
                auto& ring = rmsRing_[(size_t) c];
                const double sq = d * d;
                rmsSum_[c] = std::max (0.0, rmsSum_[c] + sq - ring[rmsPos_]);
                ring[rmsPos_] = sq;
                level = std::max (level, std::sqrt (rmsSum_[c] / (double) rmsN_));
            }
            else
            {
                level = std::max (level, std::abs (d));
            }
        }

        if (rmsN_ > 0 && ++rmsPos_ >= (size_t) rmsN_)
            rmsPos_ = 0;

        // the level is compared with a threshold that moves from the open to the close level with the gain (linear in amplitude)
        const double threshold = openLin_ + (closeLin_ - openLin_) * gain_;

        if (! open_ && level >= threshold)
        {
            open_ = true;
        }
        else if (open_ && level < threshold)
        {
            open_ = false;
            holdLeft_ = holdSamples_;
        }

        if (open_ || holdLeft_ > 0)
        {
            if (! open_)
                --holdLeft_;

            gain_ = instantAttack_ ? 1.0 : std::min (1.0, gain_ + attackStep_);     // an attack that has started always completes
        }
        else
        {
            gain_ = instantRelease_ ? 0.0 : std::max (0.0, gain_ - releaseStep_);
        }

        double delayed[2] = { x[0], x[1] };

        if (look_ > 0)
        {
            const size_t read = (writePos_ + cap - (size_t) look_) % cap;

            for (int c = 0; c < nch; ++c)
            {
                delayed[c] = line_[(size_t) c][read];
                line_[(size_t) c][writePos_] = (float) x[c];
            }

            if (++writePos_ >= cap)
                writePos_ = 0;
        }

        const double wetGain = params_.wet * (params_.invertWet ? 1.0 - gain_ : gain_);

        for (int c = 0; c < nch; ++c)
            io[c][i] = toFloat (delayed[c] * (params_.dry + wetGain));
    }
}

//==============================================================================
namespace
{
// ReaComp's default soft knee (Legacy Attack/Knee Options at its default): gain reduction K(x) for a ratio of infinity, measured as a ramp through the
// knee. The lower half of each table, x = -W .. 0 in 1 dB steps. The upper half follows from K(x) = x + K(-x) (the measured upper half agrees to
// 0.002 dB; the top of the measurement range cuts off the 24 dB table).
constexpr double kneeTable3[]  = { 0.0, 0.4078, 0.8647, 1.3502 };
constexpr double kneeTable6[]  = { 0.0, 0.3301, 0.7034, 1.1039, 1.5325, 1.9894, 2.4749 };
constexpr double kneeTable12[] = { 0.0, 0.2001, 0.4301, 0.681, 0.9542, 1.2508, 1.572, 1.9188, 2.2921, 2.6927, 3.1212, 3.5781, 4.0637 };
constexpr double kneeTable24[] = { 0.0, 0.0597, 0.1295, 0.2071, 0.2934, 0.3892, 0.4955, 0.6132, 0.7433, 0.8871, 1.0457, 1.2202, 1.4119, 1.6221, 1.852,
                                   2.103, 2.3761, 2.6728, 2.994, 3.3408, 3.7141, 4.1146, 4.5432, 5.0001, 5.4856 };
// The other knee (Legacy Attack/Knee Options 0, 0.5 or 1), only measured at W = 12: K(x) for x = -12 .. +12.
constexpr double legacyTable12[] = { 0.0, 0.0942, 0.205, 0.3295, 0.4691, 0.6257, 0.8015, 0.9986, 1.2199, 1.4682, 1.7467, 2.0592, 2.4099, 2.8034, 3.2448,
                                     3.7401, 4.2959, 4.9195, 5.6192, 6.4042, 7.285, 8.2733, 9.3823, 10.6265, 11.9988 };

/** Monotone piecewise cubic Hermite interpolation (Fritsch-Carlson). */
class Pchip
{
public:
    Pchip (std::vector<double> x, std::vector<double> y) : x_ (std::move (x)), y_ (std::move (y)), m_ (y_.size(), 0.0)
    {
        const size_t n = y_.size();
        std::vector<double> h (n - 1), delta (n - 1);

        for (size_t k = 0; k + 1 < n; ++k)
        {
            h[k] = x_[k + 1] - x_[k];
            delta[k] = (y_[k + 1] - y_[k]) / h[k];
        }

        m_[0] = delta[0];
        m_[n - 1] = delta[n - 2];

        for (size_t k = 1; k + 1 < n; ++k)
        {
            if (delta[k - 1] * delta[k] <= 0.0)
                continue;

            const double w1 = 2.0 * h[k] + h[k - 1], w2 = h[k] + 2.0 * h[k - 1];
            m_[k] = (w1 + w2) / (w1 / delta[k - 1] + w2 / delta[k]);
        }
    }

    double operator() (double x) const
    {
        x = std::clamp (x, x_.front(), x_.back());
        size_t k = (size_t) (std::upper_bound (x_.begin(), x_.end(), x) - x_.begin());
        k = k == 0 ? 0 : std::min (k - 1, y_.size() - 2);
        const double h = x_[k + 1] - x_[k], t = (x - x_[k]) / h, t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y_[k] + (t3 - 2 * t2 + t) * h * m_[k] + (-2 * t3 + 3 * t2) * y_[k + 1] + (t3 - t2) * h * m_[k + 1];
    }

private:
    std::vector<double> x_, y_, m_;
};

/** The tables hold K(-x) at x = 0 .. W (nominal knee width in the file name, in 1 dB steps, from -W up to 0). The knees were measured at W - 0.05 (the
    measurement lands on the lower rounding edge of the displayed value): K(-x) is zero from x = W - 0.05, which is where the shape ends. The shape is
    f(u) = K(-u * width) / K(0) for u = 0 .. 1. */
struct KneeSet { int nominal; const double* table; };

Pchip kneeShapeOf (const KneeSet& set)
{
    const double width = (double) set.nominal - 0.05;
    std::vector<double> u, f;

    for (int j = 0; j < set.nominal; ++j)
    {
        u.push_back ((double) j / width);
        f.push_back (set.table[set.nominal - j] / set.table[set.nominal]);
    }

    u.push_back (1.0);
    f.push_back (0.0);
    return Pchip (std::move (u), std::move (f));
}

/** Gain at the threshold of the default knee: the mean of the linear gains at the two ends of the knee. */
double kneeCentreDb (double width)
{
    return -20.0 * std::log10 (0.5 * (1.0 + std::pow (10.0, -width / 20.0)));
}

constexpr size_t kneeGrid = 513;        // K(x) / W for x / W = -1 .. 1
} // namespace

ReaKnee::ReaKnee (double kneeDb, bool legacy) : width_ (kneeDb < 0.05 ? 0.0 : std::min (kneeDb, 24.0))
{
    if (width_ <= 0.0)
        return;

    shape_.resize (kneeGrid);

    if (legacy)
    {
        std::vector<double> x (25), y (25);

        for (size_t k = 0; k < 25; ++k)
        {
            x[k] = -1.0 + (double) k / 12.0;
            y[k] = legacyTable12[k] / 12.0;
        }

        const Pchip curve (std::move (x), std::move (y));

        for (size_t g = 0; g < kneeGrid; ++g)
            shape_[g] = curve (-1.0 + 2.0 * (double) g / (double) (kneeGrid - 1));
    }
    else
    {
        static const KneeSet sets[4] = { { 3, kneeTable3 }, { 6, kneeTable6 }, { 12, kneeTable12 }, { 24, kneeTable24 } };
        const Pchip shapes[4] = { kneeShapeOf (sets[0]), kneeShapeOf (sets[1]), kneeShapeOf (sets[2]), kneeShapeOf (sets[3]) };
        const auto measuredWidth = [] (size_t i) { return (double) sets[i].nominal - 0.05; };
        size_t lo = 0;

        while (lo < 2 && width_ > measuredWidth (lo + 1))
            ++lo;

        const double blend = std::clamp ((width_ - measuredWidth (lo)) / (measuredWidth (lo + 1) - measuredWidth (lo)), 0.0, 1.0);
        const double centre = kneeCentreDb (width_);

        for (size_t g = 0; g < kneeGrid; ++g)
        {
            const double t = -1.0 + 2.0 * (double) g / (double) (kneeGrid - 1), u = std::abs (t);
            const double below = centre * ((1.0 - blend) * shapes[lo] (u) + blend * shapes[lo + 1] (u));       // K(-|x|)
            shape_[g] = (t <= 0.0 ? below : u * width_ + below) / width_;                                       // K(x) = x + K(-x) above the threshold
        }
    }

    shape_.front() = 0.0;
    shape_.back() = 1.0;

    for (size_t g = 1; g < kneeGrid; ++g)       // the gain reduction never falls with the level
        shape_[g] = std::clamp (shape_[g], shape_[g - 1], 1.0);
}

double ReaKnee::reduction (double overDb) const noexcept
{
    if (width_ <= 0.0)
        return std::max (0.0, overDb);

    if (overDb <= -width_)
        return 0.0;

    if (overDb >= width_)
        return overDb;

    const double pos = (overDb / width_ + 1.0) * 0.5 * (double) (kneeGrid - 1);
    const size_t k = std::min ((size_t) pos, kneeGrid - 2);
    const double t = pos - (double) k;
    return width_ * (shape_[k] * (1.0 - t) + shape_[k + 1] * t);
}

//==============================================================================
int ReaComp::latencySamples (const ReaCompParams& p, double sampleRate)
{
    return (int) roundSamples (std::clamp (p.preCompMs, 0.0, maxPreCompMs) * sampleRate / 1000.0);
}

double ReaComp::makeupDb (const ReaCompParams& p)
{
    return p.autoMakeup ? -p.thresholdDb * (1.0 - (p.ratio >= ReaCompParams::infiniteRatio ? 0.0 : 1.0 / std::max (1.0, p.ratio))) * 0.5 : 0.0;
}

double ReaComp::staticGainDb (const ReaCompParams& p, double levelDb)
{
    const double slope = 1.0 - (p.ratio >= ReaCompParams::infiniteRatio ? 0.0 : 1.0 / std::max (1.0, p.ratio));
    return -slope * ReaKnee (p.kneeDb, p.legacyKnee).reduction (levelDb - p.thresholdDb);
}

void ReaComp::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    for (auto& l : line_)
        l.assign ((size_t) std::ceil (maxPreCompMs * sampleRate / 1000.0) + 8, 0.0f);

    for (auto& r : rmsRing_)
        r.assign ((size_t) std::ceil (maxRmsMs * sampleRate / 1000.0) + 8, 0.0);

    minVal_.assign (line_[0].size(), 1.0);
    minAt_.assign (line_[0].size(), 0);
    reset();
    setParams (params_);
}

void ReaComp::reset()
{
    for (auto& l : line_)
        std::fill (l.begin(), l.end(), 0.0f);

    for (auto& r : rmsRing_)
        std::fill (r.begin(), r.end(), 0.0);

    for (auto& f : filter_)
        f.reset();

    rmsSum_[0] = rmsSum_[1] = 0.0;
    writePos_ = rmsPos_ = 0;
    env_ = 0.0;
    lastReductionDb_ = 0.0;
    minHead_ = minCount_ = 0;
    sample_ = 0;
}

void ReaComp::setParams (const ReaCompParams& p)
{
    if (p.kneeDb != params_.kneeDb || p.legacyKnee != params_.legacyKnee)
        knee_ = ReaKnee (p.kneeDb, p.legacyKnee);

    params_ = p;
    const double ms = sampleRate_ / 1000.0;
    attackCoef_ = p.attackMs <= 0.0 ? 0.0 : std::exp (-1.0 / (p.attackMs * ms));
    releaseCoef_ = p.releaseMs <= 0.0 ? 0.0 : std::exp (-1.0 / (p.releaseMs * ms));
    thresholdLin_ = dbToLin (p.thresholdDb - knee_.kneeDb());       // an attack starts from the start of the knee (the threshold for a hard knee)
    slope_ = 1.0 - (p.ratio >= ReaCompParams::infiniteRatio ? 0.0 : 1.0 / std::max (1.0, p.ratio));
    makeup_ = dbToLin (makeupDb (p));
    look_ = std::min (latencySamples (p, sampleRate_), (int) line_[0].size() - 2);

    const int n = p.rmsMs > 0.0 ? (int) std::clamp (roundSamples (std::min (p.rmsMs, maxRmsMs) * ms), 1L, (long) rmsRing_[0].size() - 1) : 0;

    if (n != rmsN_)
    {
        rmsN_ = n;
        rmsSum_[0] = rmsSum_[1] = 0.0;
        rmsPos_ = 0;

        for (auto& r : rmsRing_)
            std::fill (r.begin(), r.end(), 0.0);
    }

    for (auto& f : filter_)
        f.set (sampleRate_, p.lowPassHz, p.highPassHz);
}

void ReaComp::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const size_t cap = line_[0].size();

    if (cap == 0 || nch < 1)
        return;

    double worst = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double x[2] = { io[0][i], nch > 1 ? (double) io[1][i] : 0.0 };
        double level = 0.0;

        for (int c = 0; c < nch; ++c)
        {
            const double d = filter_[(size_t) c].run (x[c]);

            if (rmsN_ > 0)
            {
                auto& ring = rmsRing_[(size_t) c];
                const double sq = d * d;
                rmsSum_[c] = std::max (0.0, rmsSum_[c] + sq - ring[rmsPos_]);
                ring[rmsPos_] = sq;
                level = std::max (level, std::sqrt (rmsSum_[c] / (double) rmsN_));
            }
            else
            {
                level = std::max (level, std::abs (d));
            }
        }

        if (rmsN_ > 0 && ++rmsPos_ >= (size_t) rmsN_)
            rmsPos_ = 0;

        // the envelope starts an attack from the threshold level (Legacy Attack/Knee Options n <= 0.5; from the current envelope above that) and releases
        // toward the input level. Measured only at n = 0, 0.1, 0.2, 0.25, 0.3, 0.4, 0.5, 0.6, 0.75, 0.9, 1: the rules floor (4 n) odd = default knee and
        // n > 0.5 = no threshold clamp are inferred, the exact boundaries between the sampled values are unmeasured.
        if (level > env_)
        {
            if (! params_.attackFromEnvelope)
                env_ = std::max (env_, thresholdLin_);


            env_ = level + attackCoef_ * (env_ - level);
        }
        else
        {
            env_ = level + releaseCoef_ * (env_ - level);
        }

        env_ = flush (env_);
        double reduction = 0.0;

        if (env_ > 1.0e-12)
            reduction = slope_ * knee_.reduction (20.0 * std::log10 (env_) - params_.thresholdDb);

        worst = std::max (worst, reduction);
        double gain = (reduction > 0.0 ? dbToLin (-reduction) : 1.0) * makeup_;
        double delayed[2] = { x[0], x[1] };

        if (look_ > 0)
        {
            // the audio sample that leaves now is `look_` samples old; it gets the lowest gain of the window from its own time up to now
            const size_t qcap = minVal_.size();

            while (minCount_ > 0 && minVal_[(minHead_ + minCount_ - 1) % qcap] >= gain)
                --minCount_;

            minVal_[(minHead_ + minCount_) % qcap] = gain;
            minAt_[(minHead_ + minCount_) % qcap] = sample_;
            ++minCount_;

            while (minAt_[minHead_] < sample_ - look_)
            {
                minHead_ = (minHead_ + 1) % qcap;
                --minCount_;
            }

            gain = minVal_[minHead_];
            const size_t read = (writePos_ + cap - (size_t) look_) % cap;

            for (int c = 0; c < nch; ++c)
            {
                delayed[c] = line_[(size_t) c][read];
                line_[(size_t) c][writePos_] = (float) x[c];
            }

            if (++writePos_ >= cap)
                writePos_ = 0;
        }

        ++sample_;

        for (int c = 0; c < nch; ++c)
            io[c][i] = toFloat (delayed[c] * (params_.dry + params_.wet * gain));
    }

    lastReductionDb_ = worst;
}

//==============================================================================
namespace
{
constexpr double limitMarginDb = -0.009;        // where the measured limiter settles, before the ceiling
constexpr double slowChargeFactor = 4.0;        // slow-stage charge rate / release rate (measured fit optimum)
}

void ReaLimit::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    lookahead_ = std::max (2, (int) std::lround (0.002 * sampleRate));
    window_ = std::max (lookahead_, (int) std::lround (0.010 * sampleRate));

    for (auto& a : audio_)
        a.assign ((size_t) window_ + 1, 0.0f);

    required_.assign ((size_t) window_ + 1, 1.0);
    reset();
    setParams (params_);
}

void ReaLimit::reset()
{
    for (auto& a : audio_)
        std::fill (a.begin(), a.end(), 0.0f);

    std::fill (required_.begin(), required_.end(), 1.0);
    pos_ = 0;
    fast_ = 1.0;
    releasing_ = false;
    index_ = 0;
    lastOver_ = -1;
    slowDb_ = 0.0;
    lastReductionDb_ = 0.0;
}

void ReaLimit::setParams (const ReaLimitParams& p)
{
    params_ = p;
    boost_ = dbToLin (-p.thresholdDb);
    out_ = dbToLin (p.ceilingDb);
}

void ReaLimit::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const size_t ring = required_.size();

    if (audio_[0].empty() || nch < 1)
        return;

    const double margin = dbToLin (limitMarginDb);
    const double release = std::max (params_.releaseDbPerSec, 0.0) / sampleRate_, charge = slowChargeFactor * release;
    const int window = window_, lookahead = lookahead_;
    double worst = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double peak = 0.0;

        for (int c = 0; c < nch; ++c)
        {
            const double x = io[c][i];
            audio_[(size_t) c][pos_] = (float) x;
            peak = std::max (peak, std::abs (x));
        }

        peak *= boost_;
        required_[pos_] = peak > margin ? margin / peak : 1.0;

        if (++pos_ >= ring)
            pos_ = 0;

        // the oldest entry (at pos_) is the audio sample the gain is computed for; the window ahead of it holds the next `window` requirements
        const long long n = index_++;
        auto ahead = [&] (int k) { return required_[(pos_ + (size_t) k) % ring]; };

        if (! releasing_)
        {
            // idle: a straight line from the current gain that reaches a requirement exactly on its own sample, the first reduced sample being
            // `lookahead` samples before the peak
            double need = fast_;

            for (int k = 0; k < lookahead; ++k)
            {
                const double a = ahead (k);

                if (a < 1.0)
                    need = std::min (need, fast_ - (fast_ - a) / (k + 1.0));
            }

            if (need < fast_)
            {
                fast_ = need;
                releasing_ = true;
                lastOver_ = n;
            }
        }
        else
        {
            if (ahead (0) < 1.0)
                lastOver_ = n;

            // a line back to 1 that is reached `window` samples after the last over-threshold sample, kept under the line to every requirement inside the window
            const long long d = lastOver_ + window - n;
            double slope = d > 0 ? (1.0 - fast_) / (double) d : 1.0 - fast_;

            for (int k = 0; k <= window; ++k)
            {
                const double a = ahead (k);

                if (a < 1.0)
                    slope = std::min (slope, (a - fast_) / (k + 1.0));
            }

            fast_ += slope;

            if (fast_ >= 1.0 - 1.0e-12)
            {
                fast_ = 1.0;
                releasing_ = false;
            }
        }

        // slow stage in dB: charges at 4 x the release rate while the fast stage reduces more, never beyond it; releases linearly
        const double fastDb = fast_ < 1.0 ? -20.0 * std::log10 (fast_) : 0.0;
        slowDb_ = fastDb > slowDb_ ? std::min (fastDb, slowDb_ + charge) : std::max (0.0, slowDb_ - release);
        double gain = fast_;

        if (slowDb_ > 0.0)
            gain = std::min (gain, dbToLin (-slowDb_));

        if (gain < 1.0)
            worst = std::max (worst, -20.0 * std::log10 (gain));

        for (int c = 0; c < nch; ++c)
            io[c][i] = toFloat (audio_[(size_t) c][pos_] * boost_ * gain * out_);
    }

    lastReductionDb_ = worst;
}

//==============================================================================
namespace
{
constexpr int reaVerbCombs[10] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617, 1685, 1748 };
constexpr int reaVerbAllpasses[6] = { 556, 441, 341, 225, 180, 153 };
constexpr int reaVerbSpread = 23;
}

long ReaVerb::lineLength (int base, int spread, double sampleRate)
{
    return std::max (1L, (long) std::floor ((base + spread) * sampleRate / 44100.0 + 1.0e-9));
}

double ReaVerb::Biquad::run (double x) noexcept
{
    const double out = b0 * x + z1;
    z1 = flush (b1 * x - a1 * out + z2);
    z2 = flush (b2 * x - a2 * out);
    return out;
}

void ReaVerb::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    for (size_t c = 0; c < 2; ++c)
    {
        const int spread = c == 0 ? 0 : reaVerbSpread;

        for (size_t i = 0; i < numCombs; ++i)
            ch_[c].combs[i].buf.assign ((size_t) lineLength (reaVerbCombs[i], spread, sampleRate), 0.0);

        for (size_t i = 0; i < numAllpasses; ++i)
            ch_[c].allpasses[i].buf.assign ((size_t) lineLength (reaVerbAllpasses[i], spread, sampleRate), 0.0);

        ch_[c].pre.assign ((size_t) std::floor (0.5 * sampleRate) + 2, 0.0);
    }

    reset();
    setParams (params_);
}

void ReaVerb::reset()
{
    for (auto& c : ch_)
    {
        for (auto& comb : c.combs)
        {
            std::fill (comb.buf.begin(), comb.buf.end(), 0.0);
            comb.pos = 0;
            comb.store = 0.0;
        }

        for (auto& ap : c.allpasses)
        {
            std::fill (ap.buf.begin(), ap.buf.end(), 0.0);
            ap.pos = 0;
        }

        std::fill (c.pre.begin(), c.pre.end(), 0.0);
        c.prePos = 0;
        c.lp.z1 = c.lp.z2 = c.hp.z1 = c.hp.z2 = 0.0;
    }
}

void ReaVerb::designFilters()
{
    const auto design = [this] (bool low, double n, Biquad& f)
    {
        const double w0 = 2.0 * pi * 20000.0 * n / sampleRate_, cosw = std::cos (w0), alpha = std::sin (w0) / 2.0;      // Q = 1
        const double a0 = 1.0 + alpha;
        f.b0 = (low ? (1.0 - cosw) / 2.0 : (1.0 + cosw) / 2.0) / a0;
        f.b1 = (low ? 1.0 - cosw : -(1.0 + cosw)) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * cosw / a0;
        f.a2 = (1.0 - alpha) / a0;
    };

    lpOn_ = params_.lowpass < 1.0 - 1.0e-6;
    hpOn_ = params_.hipass > 1.0e-9;

    for (auto& c : ch_)
    {
        if (lpOn_)      design (true, std::clamp (params_.lowpass, 1.0e-6, 1.0), c.lp);
        if (hpOn_)      design (false, std::clamp (params_.hipass, 1.0e-6, 1.0), c.hp);
    }
}

void ReaVerb::setParams (const ReaVerbParams& p)
{
    params_ = p;
    feedback_ = 0.3 + 0.68 * std::clamp (p.room, 0.0, 1.0);
    damp_ = 0.4 * std::clamp (p.damp, 0.0, 1.0);
    preSamples_ = (size_t) std::clamp ((long) std::floor (std::clamp (p.delay, 0.0, 1.0) * 0.5 * sampleRate_), 0L, (long) ch_[0].pre.size() - 2);
    designFilters();
}

double ReaVerb::tailSeconds (const ReaVerbParams& p)
{
    const double feedback = 0.3 + 0.68 * std::clamp (p.room, 0.0, 1.0);
    const double loop = 1800.0 / 44100.0;
    return std::min (60.0, -3.0 * loop / std::log10 (feedback) + std::clamp (p.delay, 0.0, 1.0) * 0.5);
}

void ReaVerb::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, 2);
    const double wet = 2.0 * std::clamp (params_.wet, 0.0, 1.0), dry = 2.0 * std::clamp (params_.dry, 0.0, 1.0);
    const double width = 2.0 * std::clamp (params_.width, 0.0, 1.0) - 1.0;
    const double wet1 = wet * (width / 2.0 + 0.5), wet2 = wet * (1.0 - width) / 2.0;
    const double f = feedback_, d = damp_;

    for (int i = 0; i < numSamples; ++i)
    {
        const double in[2] = { io[0][i], nch > 1 ? (double) io[1][i] : (double) io[0][i] };
        double rev[2];

        for (size_t c = 0; c < 2; ++c)
        {
            const double u = 0.015 * in[c];
            double sum = 0.0;

            for (auto& comb : ch_[c].combs)
            {
                const double out = comb.buf[comb.pos];
                comb.store = flush (out * (1.0 - d) + comb.store * d);
                comb.buf[comb.pos] = flush (u + comb.store * f);

                if (++comb.pos >= comb.buf.size())
                    comb.pos = 0;

                sum += out;
            }

            for (auto& ap : ch_[c].allpasses)
            {
                const double bo = ap.buf[ap.pos];
                const double y = -sum + bo;
                ap.buf[ap.pos] = flush (sum + 0.5 * bo);
                sum = y;

                if (++ap.pos >= ap.buf.size())
                    ap.pos = 0;
            }

            rev[c] = sum;
        }

        const double mixed[2] = { wet1 * rev[0] + wet2 * rev[1], wet1 * rev[1] + wet2 * rev[0] };
        double out[2];

        for (size_t c = 0; c < 2; ++c)
        {
            auto& s = ch_[c];
            double y = mixed[c];

            if (lpOn_)      y = s.lp.run (y);
            if (hpOn_)      y = s.hp.run (y);

            if (preSamples_ > 0)
            {
                const size_t cap = s.pre.size();
                s.pre[s.prePos] = y;
                y = s.pre[(s.prePos + cap - preSamples_) % cap];
                s.prePos = (s.prePos + 1) % cap;
            }

            out[c] = y + dry * in[c];
        }

        io[0][i] = toFloat (out[0]);

        if (nch > 1)
            io[1][i] = toFloat (out[1]);
    }
}

//==============================================================================
bool ReaFir::curveIsFlat (const std::vector<ReaFirPoint>& pts)
{
    for (auto& p : pts)
        if (std::abs (p.gainDb - pts.front().gainDb) > 1.0e-6)
            return false;

    return true;
}

double ReaFir::curveGainDb (const std::vector<ReaFirPoint>& pts, double hz)
{
    if (pts.empty())
        return 0.0;

    if (hz <= pts.front().freqHz)
        return pts.front().gainDb;

    for (size_t i = 1; i < pts.size(); ++i)
        if (hz <= pts[i].freqHz)
        {
            const auto& a = pts[i - 1];
            const auto& b = pts[i];
            const double span = std::log (b.freqHz / a.freqHz);

            if (! (span > 0.0))
                return b.gainDb;

            const double t = std::clamp (std::log (hz / a.freqHz) / span, 0.0, 1.0);
            return a.gainDb + (b.gainDb - a.gainDb) * t * t * (3.0 - 2.0 * t);
        }

    return pts.back().gainDb;
}

void ReaFir::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    setParams (params_);
}

void ReaFir::reset()
{
    for (auto& h : history_)
        std::fill (h.begin(), h.end(), 0.0);

    pos_ = 0;
}

void ReaFir::setParams (const ReaFirParams& p)
{
    params_ = p;
    std::sort (params_.points.begin(), params_.points.end(), [] (const ReaFirPoint& a, const ReaFirPoint& b) { return a.freqHz < b.freqHz; });
    firActive_ = ! curveIsFlat (params_.points);
    taps_.clear();
    half_ = 0;

    if (! firActive_)
    {
        gain_ = params_.outputGain * (params_.points.empty() ? 1.0 : std::pow (10.0, params_.points.front().gainDb / 20.0));
        return;
    }

    gain_ = 1.0;
    const int n = std::clamp (params_.fftSize, 64, 65536);
    half_ = n / 4;
    std::vector<double> mag ((size_t) n / 2 + 1), cosTable ((size_t) n);

    for (size_t k = 0; k < mag.size(); ++k)
        mag[k] = params_.outputGain * std::pow (10.0, curveGainDb (params_.points, (double) k * sampleRate_ / n) / 20.0);

    for (size_t k = 0; k < cosTable.size(); ++k)
        cosTable[k] = std::cos (2.0 * pi * (double) k / n);

    taps_.assign ((size_t) (2 * half_ + 1), 0.0);

    for (int i = 0; i <= half_; ++i)
    {
        double acc = mag[0];

        for (int k = 1; k < n / 2; ++k)
            acc += 2.0 * mag[(size_t) k] * cosTable[(size_t) (((long long) k * i) % n)];

        acc += mag[(size_t) n / 2] * ((i & 1) ? -1.0 : 1.0);
        const double w = 0.5 * (1.0 + std::cos (pi * i / (half_ + 1)));
        taps_[(size_t) (half_ + i)] = taps_[(size_t) (half_ - i)] = acc / n * w;
    }

    for (auto& h : history_)
        h.assign (taps_.size() * 2, 0.0);

    pos_ = 0;
}

void ReaFir::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min(numChannels, 2);

    if (! firActive_)
    {
        if (gain_ != 1.0)
            for (int c = 0; c < nch; ++c)
                for (int i = 0; i < numSamples; ++i)
                    io[c][i] = toFloat (io[c][i] * gain_);

        return;
    }

    const size_t len = taps_.size();

    for (int c = 0; c < nch; ++c)
    {
        auto& h = history_[(size_t) c];
        size_t pos = pos_;

        for (int i = 0; i < numSamples; ++i)
        {
            h[pos] = h[pos + len] = io[c][i];
            const double* x = h.data() + pos + 1;
            double acc = 0.0;

            for (size_t j = 0; j < len; ++j)
                acc += taps_[j] * x[j];

            io[c][i] = toFloat (acc);
            pos = pos + 1 >= len ? 0 : pos + 1;
        }
    }

    pos_ = (pos_ + (size_t) numSamples) % len;
}

} // namespace rovox::fx
