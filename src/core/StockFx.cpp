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
    }

    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

double biquadMagnitudeDb (const BiquadCoeffs& c, double sr, double freqHz)
{
    const double w = 2.0 * pi * freqHz / sr;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    const auto h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
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
        const bool gainType = b.type == BandType::Peak || b.type == BandType::LowShelf || b.type == BandType::HighShelf;
        active_[i] = b.enabled && ! (gainType && std::abs (b.gainDb) < 1.0e-9);
        coeffs_[i] = designBiquad (b.type, sampleRate_, b.freqHz, b.gainDb, b.bandwidthOct);
    }
}

double Eq::magnitudeDb (double freqHz) const
{
    double sum = 0.0;

    for (size_t i = 0; i < EqParams::maxBands; ++i)
        if (active_[i])
            sum += biquadMagnitudeDb (coeffs_[i], sampleRate_, freqHz);

    return sum;
}

void Eq::process (float* const* io, int numChannels, int numSamples)
{
    const int nch = std::min(numChannels, 2);

    for (int c = 0; c < nch; ++c)
    {
        float* x = io[c];

        for (size_t b = 0; b < EqParams::maxBands; ++b)
        {
            if (! active_[b])
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

} // namespace rovox::fx
