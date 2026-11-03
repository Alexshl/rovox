// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "StockPlugins.h"

#include "DrumsPlugin.h"
#include "FadeStage.h"
#include "PanStage.h"
#include "Limiter.h"
#include "StockFx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <sstream>

namespace rovox
{

//==============================================================================
namespace
{
std::string lowerOf (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

struct ParamDef
{
    std::string id, name, unit;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    int decimals = 1;
    float centre = 0.0f;                    // value that sits in the middle of the control (skew), 0 = linear
    std::vector<std::string> choices;       // value = index
};

ParamDef num (std::string id, std::string name, float min, float max, float def, std::string unit, int decimals = 1, float centre = 0.0f)
{
    return { std::move (id), std::move (name), std::move (unit), min, max, def, decimals, centre, {} };
}

ParamDef toggle (std::string id, std::string name, bool def)
{
    return { std::move (id), std::move (name), "", 0.0f, 1.0f, def ? 1.0f : 0.0f, 0, 0.0f, { "off", "on" } };
}

ParamDef choice (std::string id, std::string name, std::vector<std::string> choices, int def)
{
    const auto top = (float) choices.size() - 1.0f;
    return { std::move (id), std::move (name), "", 0.0f, top, (float) def, 0, 0.0f, std::move (choices) };
}

juce::String formatValue (const ParamDef& d, float v)
{
    if (! d.choices.empty())
        return toJuce (d.choices[(size_t) std::clamp ((int) std::lround (v - d.min), 0, (int) d.choices.size() - 1)]);

    return juce::String (v, d.decimals) + (d.unit.empty() ? juce::String() : " " + toJuce (d.unit));
}

float parseValue (const ParamDef& d, const juce::String& s)
{
    if (! d.choices.empty())
    {
        const auto t = s.trim();

        for (size_t i = 0; i < d.choices.size(); ++i)
            if (t.equalsIgnoreCase (toJuce (d.choices[i])))
                return (float) i;

        if (d.choices.size() == 2 && (t.equalsIgnoreCase ("true") || t.equalsIgnoreCase ("yes")))
            return 1.0f;

        if (d.choices.size() == 2 && (t.equalsIgnoreCase ("false") || t.equalsIgnoreCase ("no")))
            return 0.0f;
    }

    return s.getFloatValue();
}

/** A gain knob in dB whose bottom end (-150) means -inf (silence). */
double gainOrSilence (double db)
{
    return db <= -149.0 ? 0.0 : std::pow (10.0, db / 20.0);
}

//==============================================================================
/** Base of the stock effects: parameters kept in the plugin state (so that they are saved and undoable), stereo in / out. */
class StockPlugin : public te::Plugin
{
public:
    StockPlugin (te::PluginCreationInfo info, std::vector<ParamDef> definitions) : te::Plugin (info), defs_ (std::move (definitions))
    {
        auto* um = getUndoManager();

        for (size_t i = 0; i < defs_.size(); ++i)
        {
            const auto& d = defs_[i];
            values_.push_back (std::make_unique<juce::CachedValue<float>>());
            values_.back()->referTo (state, juce::Identifier (d.id), um, d.def);
            juce::NormalisableRange<float> range (d.min, d.max);

            if (d.centre > d.min && d.centre < d.max)
                range.setSkewForCentre (d.centre);

            auto* ap = addParam (toJuce (d.id), toJuce (d.name), range,
                                 [def = d] (float v) { return formatValue (def, v); },
                                 [def = d] (const juce::String& s) { return parseValue (def, s); });
            ap->attachToCurrentValue (*values_.back());
            params_.push_back (ap);
        }
    }

    ~StockPlugin() override
    {
        notifyListenersOfDeletion();

        for (auto& ap : params_)
            ap->detachFromCurrentValue();
    }

    juce::String getVendor() override                       { return "Rovox"; }
    juce::String getShortName (int) override                { return getName(); }
    juce::String getSelectableDescription() override        { return getName(); }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return juce::jmin (numInputChannels, 2); }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }
    void deinitialise() override                            {}
    bool producesAudioWhenNoAudioInput() override          { return isAutomationNeeded() || ! noTail(); }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        prepareDsp (info.sampleRate);
    }

    void reset() override                                   { resetDsp(); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        const int n = std::min (2, fc.destBuffer->getNumChannels());

        if (n == 0)
            return;

        juce::ScopedNoDenormals noDenormals;
        float* io[2] = { nullptr, nullptr };

        for (int c = 0; c < n; ++c)
            io[c] = fc.destBuffer->getWritePointer (c, fc.bufferStartSample);

        run (io, n, fc.bufferNumSamples, fc);
    }

    /** Values in the unit of the parameter, with the automation of the moment. */
    float value (size_t i) const                            { return params_[i]->getCurrentValue(); }
    bool flag (size_t i) const                              { return value (i) >= 0.5f; }
    int choiceIndex (size_t i) const                        { return (int) std::lround (value (i)); }

    json extras() const
    {
        json units = json::object();

        for (auto& d : defs_)
            units[d.id] = d.choices.empty() ? json (d.unit) : json ("choice: " + [&] { std::string all; for (auto& c : d.choices) all += (all.empty() ? "" : "|") + c; return all; }());

        return { { "units", units }, { "meters", meters() }, { "latency_samples", latencySamples() } };
    }

protected:
    virtual void prepareDsp (double sampleRate) = 0;
    virtual void resetDsp() = 0;
    virtual void run (float* const* io, int numChannels, int numSamples, const te::PluginRenderContext&) = 0;
    virtual json meters() const                             { return json::object(); }
    virtual int latencySamples() const                      { return 0; }

    /** The input of the effect, kept for the generic Wet / Delta of an imported REAPER FX (see mixDry). */
    void saveDry (float* const* io, int numChannels, int numSamples)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            if ((int) dry_[(size_t) c].size() < numSamples)
                dry_[(size_t) c].resize ((size_t) numSamples);

            std::copy (io[c], io[c] + numSamples, dry_[(size_t) c].begin());
        }
    }

    /** Wet: out = wet * processed + (1 - wet) * input (linear crossfade); delta: out = processed - input, whatever the wet. */
    void mixDry (float* const* io, int numChannels, int numSamples, double wet, bool delta)
    {
        for (int c = 0; c < numChannels; ++c)
            for (int i = 0; i < numSamples; ++i)
            {
                const double processed = io[c][i], in = dry_[(size_t) c][(size_t) i];
                io[c][i] = (float) (delta ? processed - in : wet * processed + (1.0 - wet) * in);
            }
    }

    std::array<std::vector<float>, 2> dry_;
    std::vector<ParamDef> defs_;
    std::vector<std::unique_ptr<juce::CachedValue<float>>> values_;
    std::vector<te::AutomatableParameter::Ptr> params_;
    double sr_ = 44100.0;
};

//==============================================================================
class RovoxEq : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxeq";

    RovoxEq (te::PluginCreationInfo info) : StockPlugin (info, definitions()) {}

    juce::String getName() const override           { return "Rovox EQ"; }
    juce::String getPluginType() override           { return xmlTypeName; }

private:
    static std::vector<ParamDef> definitions()
    {
        static const std::vector<std::string> types { "low_shelf", "high_shelf", "peak", "low_pass", "high_pass", "notch", "band_pass",
                                                       "all_pass", "peak_alt2", "parallel_band_pass", "peak_alt" };
        struct Default { int type; float freq, bw; bool on; };
        static const Default d[8] = { { 0, 100, 2, true }, { 2, 400, 1, true }, { 2, 2500, 1, true }, { 1, 8000, 2, true },
                                      { 4, 30, 1, false }, { 2, 800, 1, false }, { 2, 5000, 1, false }, { 3, 18000, 1, false } };
        std::vector<ParamDef> out;

        for (int b = 0; b < 8; ++b)
        {
            const auto p = "b" + std::to_string (b + 1) + "_";
            const auto n = "Band " + std::to_string (b + 1) + " ";
            out.push_back (toggle (p + "on", n + "enabled", d[b].on));
            out.push_back (choice (p + "type", n + "type", types, d[b].type));
            out.push_back (num (p + "freq", n + "frequency", 20.0f, 20000.0f, d[b].freq, "Hz", 1, 1000.0f));
            out.push_back (num (p + "gain", n + "gain", -150.0f, 30.0f, 0.0f, "dB", 2));
            out.push_back (num (p + "bw", n + "bandwidth", 0.05f, 6.0f, d[b].bw, "oct", 2));
        }

        out.push_back (num ("out_gain", "Output gain", -24.0f, 24.0f, 0.0f, "dB", 2));
        out.push_back (toggle ("reaper_compat", "ReaEQ filter design", false));      // imported ReaEQ: filters designed as ReaEQ does (src/core/StockFx.h)
        out.push_back (num ("fx_wet", "FX wet mix", 0.0f, 100.0f, 100.0f, "%", 1));  // generic Wet / Delta of an imported REAPER FX
        out.push_back (toggle ("fx_delta", "FX delta (processed - input)", false));
        return out;
    }

    void prepareDsp (double sr) override        { sr_ = sr; eq_.prepare (sr); }
    void resetDsp() override                    { eq_.reset(); }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        fx::EqParams p;

        for (size_t b = 0; b < 8; ++b)
        {
            p.bands[b] = { flag (b * 5), (fx::BandType) choiceIndex (b * 5 + 1), value (b * 5 + 2), value (b * 5 + 3), value (b * 5 + 4) };
        }

        p.outputGainDb = value (40);
        p.reaperCompat = flag (41);

        if (! (p == eq_.params()))
            eq_.setParams (p);

        const double wet = std::clamp ((double) value (42) / 100.0, 0.0, 1.0);
        const bool delta = flag (43), mix = delta || wet < 1.0;

        if (mix)
            saveDry (io, n, count);

        eq_.process (io, n, count);

        if (mix)
            mixDry (io, n, count, wet, delta);
    }

    fx::Eq eq_;
};

//==============================================================================
class RovoxComp : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxcomp";

    RovoxComp (te::PluginCreationInfo info) : StockPlugin (info, definitions()) {}

    juce::String getName() const override           { return "Rovox Comp"; }
    juce::String getPluginType() override           { return xmlTypeName; }
    double getLatencySeconds() override             { return (double) latencySamples() / sampleRate; }

private:
    // parameter order: the Rovox compressor (10), then the ReaComp mode: reaper_compat, fx_wet, fx_delta, dry, wet, lowpass, hipass, legacy knee
    static constexpr size_t compatFlag = 10, fxWet = 11, fxDelta = 12, reaDry = 13, reaWet = 14, reaLowPass = 15, reaHighPass = 16, reaLegacy = 17;
    static constexpr double nativeMaxLookaheadMs = 20.0, nativeMaxRmsMs = 200.0;

    static std::vector<ParamDef> definitions()
    {
        return { num ("threshold", "Threshold", -100.0f, 6.0f, -18.0f, "dB", 1, -30.0f),
                 num ("ratio", "Ratio", 1.0f, 100.0f, 4.0f, ":1", 2, 6.0f),
                 num ("attack", "Attack", 0.0f, 500.0f, 10.0f, "ms", 2, 30.0f),
                 num ("release", "Release", 0.0f, 5000.0f, 100.0f, "ms", 1, 300.0f),
                 num ("knee", "Knee", 0.0f, 30.0f, 6.0f, "dB", 1),
                 num ("lookahead", "Pre-comp (lookahead)", 0.0f, 250.0f, 0.0f, "ms", 2, 20.0f),
                 num ("makeup", "Makeup gain", -12.0f, 36.0f, 0.0f, "dB", 1),
                 toggle ("auto_makeup", "Auto makeup", false),
                 num ("rms", "RMS size (0 = peak)", 0.0f, 1000.0f, 0.0f, "ms", 1, 20.0f),
                 num ("mix", "Wet (parallel mix)", 0.0f, 100.0f, 100.0f, "%", 0),
                 toggle ("reaper_compat", "ReaComp mode", false),       // imported ReaComp: ReaComp's detector, envelope, knee and mix (src/core/StockFx.h)
                 num ("fx_wet", "FX wet mix", 0.0f, 100.0f, 100.0f, "%", 1),      // generic Wet / Delta of an imported REAPER FX
                 toggle ("fx_delta", "FX delta (processed - input)", false),
                 num ("dry", "Dry (ReaComp mode)", -150.0f, 6.0f, -150.0f, "dB", 1),
                 num ("wet", "Wet (ReaComp mode)", -150.0f, 6.0f, 0.0f, "dB", 1),
                 num ("lowpass", "Detector low-pass (ReaComp mode)", 0.0f, 20000.0f, 20000.0f, "Hz", 0),
                 num ("hipass", "Detector high-pass (ReaComp mode)", 0.0f, 20000.0f, 0.0f, "Hz", 0),
                 num ("legacy_knee", "Legacy attack/knee option (ReaComp mode)", 0.0f, 1.0f, 0.25f, "", 2) };
    }

    bool compat() const                         { return flag (compatFlag); }

    fx::CompParams readParams() const
    {
        return { value (0), value (1), value (2), value (3), value (4), std::min ((double) value (5), nativeMaxLookaheadMs), value (6), flag (7),
                 std::min ((double) value (8), nativeMaxRmsMs), value (9) / 100.0 };
    }

    fx::ReaCompParams readReaParams() const
    {
        fx::ReaCompParams p;
        p.thresholdDb = value (0);
        p.ratio = value (1);
        p.attackMs = value (2);
        p.releaseMs = value (3);
        p.preCompMs = value (5);
        p.lowPassHz = value (reaLowPass);
        p.highPassHz = value (reaHighPass);
        p.rmsMs = value (8);
        p.kneeDb = value (4);
        const double legacy = value (reaLegacy);
        p.legacyKnee = ((long) std::floor (4.0 * legacy + 1.0e-6) & 1) == 0;       // floor (4 n) odd: the default knee
        p.attackFromEnvelope = legacy > 0.5 + 1.0e-6;
        p.autoMakeup = flag (7);
        p.dry = gainOrSilence (value (reaDry));
        p.wet = gainOrSilence (value (reaWet));
        return p;
    }

    void prepareDsp (double sr) override
    {
        sr_ = sr;
        comp_.prepare (sr);
        comp_.setParams (readParams());

        if (compat())
            prepareRea();
    }

    void prepareRea()
    {
        reaComp_.prepare (sr_);
        reaComp_.setParams (readReaParams());
        reaReady_ = true;
    }

    void resetDsp() override                    { comp_.reset(); if (reaReady_) reaComp_.reset(); }

    int latencySamples() const override
    {
        if (compat())
            return fx::ReaComp::latencySamples (readReaParams(), sampleRate);

        return (int) std::lround (std::min ((double) value (5), nativeMaxLookaheadMs) * 0.001 * sampleRate);
    }

    json meters() const override                { return { { "gain_reduction_db", (double) grDb_.load() } }; }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        if (compat())
        {
            if (! reaReady_)
                prepareRea();

            const double wet = std::clamp ((double) value (fxWet) / 100.0, 0.0, 1.0);
            const bool delta = flag (fxDelta), mix = delta || wet < 1.0;

            if (mix)
                saveDry (io, n, count);

            reaComp_.setParams (readReaParams());
            reaComp_.process (io, n, count);
            grDb_ = (float) reaComp_.lastGainReductionDb();

            if (mix)
                mixDry (io, n, count, wet, delta);

            return;
        }

        comp_.setParams (readParams());
        comp_.process (io, n, count);
        grDb_ = (float) comp_.lastGainReductionDb();
    }

    fx::Comp comp_;
    fx::ReaComp reaComp_;
    bool reaReady_ = false;
    std::atomic<float> grDb_ { 0.0f };
};

//==============================================================================
class RovoxGate : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxgate";

    RovoxGate (te::PluginCreationInfo info) : StockPlugin (info, definitions()) {}

    juce::String getName() const override           { return "Rovox Gate"; }
    juce::String getPluginType() override           { return xmlTypeName; }
    double getLatencySeconds() override             { return (double) latencySamples() / sampleRate; }

private:
    // parameter order: the Rovox gate (6), then the ReaGate mode: reaper_compat, fx_wet, fx_delta, pre-open, RMS size, lowpass, hipass, dry, wet,
    // invert wet, hysteresis
    static constexpr size_t compatFlag = 6, fxWet = 7, fxDelta = 8, reaPreOpen = 9, reaRms = 10, reaLowPass = 11, reaHighPass = 12, reaDry = 13, reaWet = 14,
                            reaInvert = 15, reaHysteresis = 16;

    static std::vector<ParamDef> definitions()
    {
        return { num ("threshold", "Threshold", -100.0f, 6.0f, -40.0f, "dB", 1, -40.0f),
                 num ("attack", "Attack", 0.0f, 500.0f, 1.0f, "ms", 2, 10.0f),
                 num ("hold", "Hold", 0.0f, 2000.0f, 50.0f, "ms", 1, 100.0f),
                 num ("release", "Release", 0.0f, 5000.0f, 100.0f, "ms", 1, 300.0f),
                 num ("hysteresis", "Hysteresis", 0.0f, 24.0f, 3.0f, "dB", 1),
                 num ("range", "Range (closed attenuation)", -100.0f, 0.0f, -80.0f, "dB", 1),
                 toggle ("reaper_compat", "ReaGate mode", false),       // imported ReaGate: ReaGate's detector and gain trajectory (src/core/StockFx.h)
                 num ("fx_wet", "FX wet mix", 0.0f, 100.0f, 100.0f, "%", 1),      // generic Wet / Delta of an imported REAPER FX
                 toggle ("fx_delta", "FX delta (processed - input)", false),
                 num ("pre_open", "Pre-open (ReaGate mode)", 0.0f, 250.0f, 0.0f, "ms", 1, 20.0f),
                 num ("rms", "RMS size (ReaGate mode, 0 = peak)", 0.0f, 1000.0f, 0.0f, "ms", 1, 20.0f),
                 num ("lowpass", "Detector low-pass (ReaGate mode)", 0.0f, 20000.0f, 20000.0f, "Hz", 0),
                 num ("hipass", "Detector high-pass (ReaGate mode)", 0.0f, 20000.0f, 0.0f, "Hz", 0),
                 num ("dry", "Dry (ReaGate mode)", -150.0f, 6.0f, -150.0f, "dB", 1),
                 num ("wet", "Wet (ReaGate mode)", -150.0f, 6.0f, 0.0f, "dB", 1),
                 toggle ("invert_wet", "Invert wet (ReaGate mode)", false),
                 num ("rea_hysteresis", "Hysteresis (ReaGate mode)", -100.0f, 24.0f, 0.0f, "dB", 1, 0.0f) };
    }

    bool compat() const                         { return flag (compatFlag); }

    fx::GateParams readParams() const
    {
        return { value (0), value (1), value (2), value (3), value (4), value (5) };
    }

    fx::ReaGateParams readReaParams() const
    {
        fx::ReaGateParams p;
        p.thresholdDb = value (0);
        p.attackMs = value (1);
        p.holdMs = value (2);
        p.releaseMs = value (3);
        p.preOpenMs = value (reaPreOpen);
        p.lowPassHz = value (reaLowPass);
        p.highPassHz = value (reaHighPass);
        p.hysteresisDb = value (reaHysteresis);
        p.rmsMs = value (reaRms);
        p.dry = gainOrSilence (value (reaDry));
        p.wet = gainOrSilence (value (reaWet));
        p.invertWet = flag (reaInvert);
        return p;
    }

    void prepareDsp (double sr) override
    {
        sr_ = sr;
        gate_.prepare (sr);
        gate_.setParams (readParams());

        if (compat())
            prepareRea();
    }

    void prepareRea()
    {
        reaGate_.prepare (sr_);
        reaGate_.setParams (readReaParams());
        reaReady_ = true;
    }

    void resetDsp() override                    { gate_.reset(); if (reaReady_) reaGate_.reset(); }
    int latencySamples() const override         { return compat() ? fx::ReaGate::latencySamples (readReaParams(), sampleRate) : 0; }
    json meters() const override                { return { { "gate_gain_db", (double) gainDb_.load() }, { "open", open_.load() } }; }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        if (compat())
        {
            if (! reaReady_)
                prepareRea();

            const double wet = std::clamp ((double) value (fxWet) / 100.0, 0.0, 1.0);
            const bool delta = flag (fxDelta), mix = delta || wet < 1.0;

            if (mix)
                saveDry (io, n, count);

            reaGate_.setParams (readReaParams());
            reaGate_.process (io, n, count);
            gainDb_ = (float) (20.0 * std::log10 (std::max (reaGate_.lastGain(), 1.0e-6)));
            open_ = reaGate_.lastGain() > 0.5;

            if (mix)
                mixDry (io, n, count, wet, delta);

            return;
        }

        gate_.setParams (readParams());
        gate_.process (io, n, count);
        gainDb_ = (float) gate_.gainDb();
        open_ = gate_.isOpen();
    }

    fx::Gate gate_;
    fx::ReaGate reaGate_;
    bool reaReady_ = false;
    std::atomic<float> gainDb_ { 0.0f };
    std::atomic<bool> open_ { false };
};

//==============================================================================
class RovoxLimit : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxlimit";

    RovoxLimit (te::PluginCreationInfo info) : StockPlugin (info, definitions()) {}

    juce::String getName() const override           { return "Rovox Limit"; }
    juce::String getPluginType() override           { return xmlTypeName; }
    double getLatencySeconds() override             { return (double) latencySamples() / sampleRate; }

private:
    // parameter order: the Rovox limiter (3), then the ReaLimit mode: reaper_compat, fx_wet, fx_delta, threshold, release rate (dB/s)
    static constexpr size_t compatFlag = 3, fxWet = 4, fxDelta = 5, reaThreshold = 6, reaRelease = 7;

    static std::vector<ParamDef> definitions()
    {
        return { num ("input_gain", "Input gain", 0.0f, 24.0f, 0.0f, "dB", 1),
                 num ("ceiling", "Ceiling (true peak)", -24.0f, 0.0f, -1.0f, "dBTP", 2),
                 num ("release", "Release", 1.0f, 2000.0f, 80.0f, "ms", 1, 100.0f),
                 toggle ("reaper_compat", "ReaLimit mode", false),      // imported ReaLimit: sample-peak limiter with ReaLimit's release (src/core/StockFx.h)
                 num ("fx_wet", "FX wet mix", 0.0f, 100.0f, 100.0f, "%", 1),      // generic Wet / Delta of an imported REAPER FX
                 toggle ("fx_delta", "FX delta (processed - input)", false),
                 num ("threshold", "Threshold (ReaLimit mode)", -60.0f, 12.0f, 0.0f, "dB", 2),
                 num ("release_rate", "Release (ReaLimit mode, 150 = inf)", 1.0f, 1000.0f, 15.0f, "dB/s", 1, 20.0f) };
    }

    bool compat() const                         { return flag (compatFlag); }

    fx::ReaLimitParams readReaParams() const
    {
        return { value (reaThreshold), value (1), value (reaRelease) };
    }

    void prepareDsp (double sr) override
    {
        sr_ = sr;
        limiter_.prepare (sr, 2);
        limiter_.setParams (value (0), value (1), value (2), 0.1);

        if (compat())
            prepareRea();
    }

    void prepareRea()
    {
        reaLimit_.prepare (sr_);
        reaLimit_.setParams (readReaParams());
        reaReady_ = true;
    }

    void resetDsp() override                    { limiter_.reset(); if (reaReady_) reaLimit_.reset(); }

    int latencySamples() const override
    {
        if (! compat())
            return limiter_.latencySamples();

        return reaReady_ ? reaLimit_.latencySamples() : std::max (0, (int) std::lround (0.010 * sampleRate));
    }

    json meters() const override                { return { { "gain_reduction_db", (double) grDb_.load() } }; }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        if (compat())
        {
            if (! reaReady_)
                prepareRea();

            const double wet = std::clamp ((double) value (fxWet) / 100.0, 0.0, 1.0);
            const bool delta = flag (fxDelta), mix = delta || wet < 1.0;

            if (mix)
                saveDry (io, n, count);

            reaLimit_.setParams (readReaParams());
            reaLimit_.process (io, n, count);
            grDb_ = (float) reaLimit_.lastGainReductionDb();

            if (mix)
                mixDry (io, n, count, wet, delta);

            return;
        }

        limiter_.setParams (value (0), value (1), value (2), 0.1);
        limiter_.process (io, n, count);
        grDb_ = (float) limiter_.lastGainReductionDb();
    }

    StreamingLimiter limiter_;
    fx::ReaLimit reaLimit_;
    bool reaReady_ = false;
    std::atomic<float> grDb_ { 0.0f };
};

//==============================================================================
class RovoxDelay : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxdelay";

    RovoxDelay (te::PluginCreationInfo info) : StockPlugin (info, definitions()) {}

    juce::String getName() const override           { return "Rovox Delay"; }
    juce::String getPluginType() override           { return xmlTypeName; }
    bool noTail() override                          { return false; }
    double getTailLength() const override           { return compat() ? fx::ReaDelay::tailSeconds (readReaParams (bpm_.load())) : fx::Delay::tailSeconds (readParams (bpm_.load())); }

private:
    // parameter order: 4 x (on, sync, time, beats, gain, pan), feedback, lowpass, highpass, wet, dry, then the ReaDelay mode:
    // reaper_compat, fx_wet, fx_delta, and per tap (musical, fb, lp, hp, bits, width)
    static constexpr size_t perTap = 6, shared = 4 * perTap, compatFlag = shared + 5, fxWet = compatFlag + 1, fxDelta = compatFlag + 2,
                            extra = compatFlag + 3, perTapExtra = 6;

    static std::vector<ParamDef> definitions()
    {
        static const float times[4] = { 250.0f, 500.0f, 750.0f, 1000.0f };
        std::vector<ParamDef> out;

        for (int t = 0; t < 4; ++t)
        {
            const auto p = "t" + std::to_string (t + 1) + "_";
            const auto n = "Tap " + std::to_string (t + 1) + " ";
            out.push_back (toggle (p + "on", n + "enabled", t == 0));
            out.push_back (toggle (p + "sync", n + "tempo sync", false));
            out.push_back (num (p + "time", n + "time", 0.0f, 10000.0f, times[t], "ms", 2, 500.0f));
            out.push_back (num (p + "beats", n + "length", 0.0625f, 16.0f, 0.5f * (float) (t + 1), "beats", 4, 1.0f));
            out.push_back (num (p + "gain", n + "gain", -60.0f, 12.0f, 0.0f, "dB", 1));
            out.push_back (num (p + "pan", n + "pan", -1.0f, 1.0f, t == 1 ? -0.5f : t == 2 ? 0.5f : 0.0f, "", 2));
        }

        out.push_back (num ("feedback", "Feedback (tap 1)", 0.0f, 95.0f, 30.0f, "%", 1));
        out.push_back (num ("lowpass", "Feedback low-pass", 200.0f, 20000.0f, 20000.0f, "Hz", 0, 4000.0f));
        out.push_back (num ("highpass", "Feedback high-pass", 20.0f, 2000.0f, 20.0f, "Hz", 0, 200.0f));
        out.push_back (num ("wet", "Wet", -96.0f, 12.0f, -6.0f, "dB", 1));
        out.push_back (num ("dry", "Dry", -96.0f, 12.0f, 0.0f, "dB", 1));
        out.push_back (toggle ("reaper_compat", "ReaDelay mode", false));      // imported ReaDelay: separate loop per tap, whole-sample delay (src/core/StockFx.h)
        out.push_back (num ("fx_wet", "FX wet mix", 0.0f, 100.0f, 100.0f, "%", 1));      // generic Wet / Delta of an imported REAPER FX
        out.push_back (toggle ("fx_delta", "FX delta (processed - input)", false));

        for (int t = 0; t < 4; ++t)
        {
            const auto p = "t" + std::to_string (t + 1) + "_";
            const auto n = "Tap " + std::to_string (t + 1) + " ";
            out.push_back (num (p + "musical", n + "length (1/8 notes, ReaDelay mode)", 0.0f, 256.0f, 0.0f, "1/8", 3, 8.0f));
            out.push_back (num (p + "fb", n + "feedback (ReaDelay mode)", -150.0f, 12.0f, -150.0f, "dB", 2));
            out.push_back (num (p + "lp", n + "low-pass (ReaDelay mode)", 0.0f, 20000.0f, 20000.0f, "Hz", 0));
            out.push_back (num (p + "hp", n + "high-pass (ReaDelay mode)", 0.0f, 20000.0f, 0.0f, "Hz", 0));
            out.push_back (num (p + "bits", n + "resolution (ReaDelay mode)", 2.0f, 24.0f, 24.0f, "bits", 0));
            out.push_back (num (p + "width", n + "stereo width (ReaDelay mode)", -1.0f, 1.0f, 1.0f, "", 2));
        }

        return out;
    }

    bool compat() const                         { return flag (compatFlag); }

    fx::DelayParams readParams (double bpm) const
    {
        fx::DelayParams p;

        for (size_t t = 0; t < 4; ++t)
            p.taps[t] = { flag (t * perTap), flag (t * perTap + 1), value (t * perTap + 2), value (t * perTap + 3), value (t * perTap + 4), value (t * perTap + 5) };

        p.feedbackPct = value (shared);
        p.lowPassHz = value (shared + 1);
        p.highPassHz = value (shared + 2);
        p.wetDb = value (shared + 3);
        p.dryDb = value (shared + 4);
        p.bpm = bpm;
        return p;
    }

    fx::ReaDelayParams readReaParams (double bpm) const
    {
        fx::ReaDelayParams p;

        for (size_t t = 0; t < 4; ++t)
        {
            const size_t e = extra + t * perTapExtra;
            auto& tap = p.taps[t];
            tap.enabled = flag (t * perTap);
            tap.timeMs = value (t * perTap + 2);
            tap.musical = value (e);
            tap.feedback = value (e + 1) <= -149.0f ? 0.0 : std::pow (10.0, (double) value (e + 1) / 20.0);
            tap.lowPassHz = value (e + 2);
            tap.highPassHz = value (e + 3);
            tap.bits = (int) std::lround (value (e + 4));
            tap.width = value (e + 5);
            tap.volume = value (t * perTap + 4) <= -59.9f ? 0.0 : std::pow (10.0, (double) value (t * perTap + 4) / 20.0);
            tap.pan = value (t * perTap + 5);
        }

        p.wet = value (shared + 3) <= -95.9f ? 0.0 : std::pow (10.0, (double) value (shared + 3) / 20.0);
        p.dry = value (shared + 4) <= -95.9f ? 0.0 : std::pow (10.0, (double) value (shared + 4) / 20.0);
        p.bpm = bpm;
        return p;
    }

    void prepareDsp (double sr) override
    {
        sr_ = sr;
        delay_.prepare (sr);
        delay_.setParams (readParams (bpm_.load()));

        if (compat())
            prepareRea();
    }

    void prepareRea()
    {
        reaDelay_.prepare (sr_);
        reaDelay_.setParams (readReaParams (bpm_.load()));
        reaReady_ = true;
    }

    void resetDsp() override                    { delay_.reset(); if (reaReady_) reaDelay_.reset(); }

    void run (float* const* io, int n, int count, const te::PluginRenderContext& fc) override
    {
        if (compat())
        {
            bpm_ = edit.tempoSequence.getBpmAt (fc.editTime.getStart());

            if (! reaReady_)
                prepareRea();

            const double wet = std::clamp ((double) value (fxWet) / 100.0, 0.0, 1.0);
            const bool delta = flag (fxDelta), mix = delta || wet < 1.0;

            if (mix)
                saveDry (io, n, count);

            reaDelay_.setParams (readReaParams (bpm_.load()));
            reaDelay_.process (io, n, count);

            if (mix)
                mixDry (io, n, count, wet, delta);

            return;
        }

        bool synced = false;

        for (size_t t = 0; t < 4; ++t)
            synced = synced || flag (t * perTap + 1);

        if (synced)
            bpm_ = edit.tempoSequence.getBpmAt (fc.editTime.getStart());

        delay_.setParams (readParams (bpm_.load()));
        delay_.process (io, n, count);
    }

    fx::Delay delay_;
    fx::ReaDelay reaDelay_;
    bool reaReady_ = false;
    std::atomic<double> bpm_ { 120.0 };
};

//==============================================================================
class RovoxVerb : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxverb";

    RovoxVerb (te::PluginCreationInfo info) : StockPlugin (info, definitions()) {}

    juce::String getName() const override           { return "Rovox Verb"; }
    juce::String getPluginType() override           { return xmlTypeName; }
    bool noTail() override                          { return false; }
    double getTailLength() const override           { return compat() ? fx::ReaVerb::tailSeconds (readReaParams()) : fx::Verb::tailSeconds (readParams()); }

private:
    // parameter order: the Rovox reverb (8), then the ReaVerbate mode: reaper_compat, fx_wet, fx_delta, and the normalized knob positions of ReaVerbate
    static constexpr size_t compatFlag = 8, fxWet = 9, fxDelta = 10, reaFirst = 11;

    static std::vector<ParamDef> definitions()
    {
        return { num ("room_size", "Room size", 0.0f, 1.0f, 0.5f, "", 3),
                 num ("damping", "Damping", 0.0f, 1.0f, 0.5f, "", 3),
                 num ("pre_delay", "Pre-delay", 0.0f, 250.0f, 0.0f, "ms", 1, 40.0f),
                 num ("width", "Width", 0.0f, 1.0f, 1.0f, "", 2),
                 num ("low_cut", "Low cut", 20.0f, 1000.0f, 20.0f, "Hz", 0, 100.0f),
                 num ("high_cut", "High cut", 1000.0f, 20000.0f, 20000.0f, "Hz", 0, 6000.0f),
                 num ("wet", "Wet", -96.0f, 12.0f, -12.0f, "dB", 1),
                 num ("dry", "Dry", -96.0f, 12.0f, 0.0f, "dB", 1),
                 toggle ("reaper_compat", "ReaVerbate mode", false),         // imported ReaVerbate: its exact reverberator (src/core/StockFx.h)
                 num ("fx_wet", "FX wet mix", 0.0f, 100.0f, 100.0f, "%", 1),      // generic Wet / Delta of an imported REAPER FX
                 toggle ("fx_delta", "FX delta (processed - input)", false),
                 num ("rea_wet", "Wet (ReaVerbate mode, knob position, amplitude 2 n)", 0.0f, 1.0f, 0.25f, "", 4),
                 num ("rea_dry", "Dry (ReaVerbate mode, knob position, amplitude 2 n)", 0.0f, 1.0f, 0.5f, "", 4),
                 num ("rea_room", "Room size (ReaVerbate mode, knob position)", 0.0f, 1.0f, 0.294118f, "", 4),
                 num ("rea_damp", "Dampening (ReaVerbate mode, knob position)", 0.0f, 1.0f, 0.5f, "", 4),
                 num ("rea_width", "Width (ReaVerbate mode, knob position, width = 2 n - 1)", 0.0f, 1.0f, 1.0f, "", 4),
                 num ("rea_delay", "Delay (ReaVerbate mode, knob position, 500 ms n)", 0.0f, 1.0f, 0.0f, "", 4),
                 num ("rea_lp", "Lowpass (ReaVerbate mode, knob position, 20 kHz n; 1 = off)", 0.0f, 1.0f, 1.0f, "", 4),
                 num ("rea_hp", "Hipass (ReaVerbate mode, knob position, 20 kHz n; 0 = off)", 0.0f, 1.0f, 0.0f, "", 4) };
    }

    bool compat() const                         { return flag (compatFlag); }

    fx::VerbParams readParams() const
    {
        return { value (0), value (1), value (2), value (3), value (4), value (5), value (6), value (7) };
    }

    fx::ReaVerbParams readReaParams() const
    {
        return { value (reaFirst), value (reaFirst + 1), value (reaFirst + 2), value (reaFirst + 3), value (reaFirst + 4), value (reaFirst + 5),
                 value (reaFirst + 6), value (reaFirst + 7) };
    }

    void prepareDsp (double sr) override
    {
        sr_ = sr;
        verb_.prepare (sr);
        verb_.setParams (readParams());

        if (compat())
            prepareRea();
    }

    void prepareRea()
    {
        reaVerb_.prepare (sr_);
        reaVerb_.setParams (readReaParams());
        reaReady_ = true;
    }

    void resetDsp() override                    { verb_.reset(); if (reaReady_) reaVerb_.reset(); }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        if (compat())
        {
            if (! reaReady_)
                prepareRea();

            const double wet = std::clamp ((double) value (fxWet) / 100.0, 0.0, 1.0);
            const bool delta = flag (fxDelta), mix = delta || wet < 1.0;

            if (mix)
                saveDry (io, n, count);

            reaVerb_.setParams (readReaParams());
            reaVerb_.process (io, n, count);

            if (mix)
                mixDry (io, n, count, wet, delta);

            return;
        }

        verb_.setParams (readParams());
        verb_.process (io, n, count);
    }

    fx::Verb verb_;
    fx::ReaVerb reaVerb_;
    bool reaReady_ = false;
};
//==============================================================================
/** Imported ReaFir in EQ mode: the curve (frequency:gain-dB points, in the "curve" property of the plugin state) as an exact gain when it is flat, else as a
    zero-phase FIR whose latency the host compensates (src/core/StockFx.h). */
class RovoxFir : public StockPlugin
{
public:
    static inline const char* xmlTypeName = "rovoxfir";

    RovoxFir (te::PluginCreationInfo info) : StockPlugin (info, definitions())
    {
        firParams_.points = parseCurve (toStd (state["curve"].toString()));
    }

    juce::String getName() const override           { return "Rovox FIR EQ"; }
    juce::String getPluginType() override           { return xmlTypeName; }
    double getLatencySeconds() override             { return (double) latencySamples() / sampleRate; }
    bool noTail() override                          { return fir_.isFlat(); }
    double getTailLength() const override           { return (double) fir_.latencySamples() / sr_; }

    static std::vector<fx::ReaFirPoint> parseCurve (const std::string& text)
    {
        std::vector<fx::ReaFirPoint> out;
        std::istringstream in (text);
        std::string item;

        while (in >> item)
        {
            const auto colon = item.find (':');

            if (colon == std::string::npos)
                continue;

            fx::ReaFirPoint pt;
            pt.freqHz = std::atof (item.substr (0, colon).c_str());
            pt.gainDb = std::atof (item.substr (colon + 1).c_str());

            if (pt.freqHz > 0.0)
                out.push_back (pt);
        }

        return out;
    }

private:
    static std::vector<ParamDef> definitions()
    {
        return { num ("fft_size", "FFT size", 64.0f, 65536.0f, 4096.0f, "", 0, 4096.0f),
                 num ("gain", "Output gain", -96.0f, 12.0f, 0.0f, "dB", 2) };
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        StockPlugin::valueTreePropertyChanged (tree, property);

        if (tree == state && property == juce::Identifier ("curve"))
        {
            auto points = parseCurve (toStd (state["curve"].toString()));
            const juce::SpinLock::ScopedLockType lock (lock_);
            pending_ = std::move (points);
            dirty_ = true;
        }
    }

    fx::ReaFirParams currentParams()
    {
        if (dirty_.exchange (false))
        {
            const juce::SpinLock::ScopedLockType lock (lock_);
            firParams_.points = pending_;
        }

        firParams_.fftSize = (int) std::lround (value (0));
        firParams_.outputGain = dbToLinOrZero (value (1));
        return firParams_;
    }

    static double dbToLinOrZero (double db)         { return db <= -95.9 ? 0.0 : std::pow (10.0, db / 20.0); }

    int latencySamples() const override             { return fir_.latencySamples(); }

    void prepareDsp (double sr) override
    {
        sr_ = sr;
        fir_.prepare (sr);
        applied_ = currentParams();
        fir_.setParams (applied_);
    }

    void resetDsp() override                        { fir_.reset(); }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        const auto p = currentParams();

        if (! (p == applied_))
        {
            applied_ = p;
            fir_.setParams (p);
        }

        fir_.process (io, n, count);
    }

    fx::ReaFir fir_;
    fx::ReaFirParams firParams_, applied_;
    std::vector<fx::ReaFirPoint> pending_;
    juce::SpinLock lock_;
    std::atomic<bool> dirty_ { false };
};

} // namespace

//==============================================================================
const std::vector<StockInfo>& stockPlugins()
{
    static const std::vector<StockInfo> all {
        { "rovox:eq", "eq", "Rovox EQ", RovoxEq::xmlTypeName, "ReaEQ",
          "Parametric EQ, up to 8 bands (low/high shelf, peak, low/high pass, notch, band pass), freq in Hz, gain in dB, bandwidth in octaves. Similar to ReaEQ.",
          { "stock", "eq", "filter" } },
        { "rovox:comp", "comp", "Rovox Comp", RovoxComp::xmlTypeName, "ReaComp",
          "Feed-forward compressor: threshold, ratio, attack, release, knee, lookahead (pre-comp), makeup / auto makeup, RMS size, wet mix. Reports gain reduction. Similar to ReaComp.",
          { "stock", "dynamics", "compressor" } },
        { "rovox:gate", "gate", "Rovox Gate", RovoxGate::xmlTypeName, "ReaGate",
          "Noise gate: threshold, attack, hold, release, hysteresis, range. Similar to ReaGate.",
          { "stock", "dynamics", "gate" } },
        { "rovox:limit", "limit", "Rovox Limit", RovoxLimit::xmlTypeName, "ReaLimit",
          "True-peak lookahead brickwall limiter: input gain, ceiling (dBTP), release. Reports its latency (delay compensation). Similar to ReaLimit.",
          { "stock", "dynamics", "limiter" } },
        { "rovox:delay", "delay", "Rovox Delay", RovoxDelay::xmlTypeName, "ReaDelay",
          "Up to 4 delay taps (ms or tempo-synced beats), feedback of tap 1 with low/high-pass, per-tap gain and pan, wet/dry. Similar to ReaDelay.",
          { "stock", "delay", "time" } },
        { "rovox:verb", "verb", "Rovox Verb", RovoxVerb::xmlTypeName, "ReaVerbate",
          "Algorithmic reverb (damped combs and allpasses): room size, damping, pre-delay, width, low/high cut, wet/dry. Similar to ReaVerbate.",
          { "stock", "reverb", "space" } },
        { "rovox:fir", "fir", "Rovox FIR EQ", RovoxFir::xmlTypeName, "ReaFir",
          "FFT-style EQ from a frequency/gain curve (property `curve`, points \"hz:db\"): a flat curve is an exact gain, any other curve a zero-phase FIR with reported latency. Imported ReaFir in EQ mode; its dynamics modes are not modelled.",
          { "stock", "eq", "filter" } },
        { "rovox:drums", "drums", "Rovox Drums", drumsXmlType, "MT-PowerDrumKit",
          "General MIDI drum sampler (instrument): plays MIDI drum parts written for MT-PowerDrumKit unchanged (kick 35/36, snare 38/40, side stick 37, hats 42/44/46 with choke, toms, cymbals, cowbell, tambourine) "
          "with velocity layers and round robin. Samples: DRSKit (CC BY 4.0), loaded from ROVOX_DRUMKITS_PATH or the object store; plugin state reports the kit and unmapped notes.",
          { "instrument", "drums", "sampler" } } };

    return all;
}

const StockInfo* findStockPlugin (const std::string& key)
{
    const auto k = lowerOf (key);

    for (auto& s : stockPlugins())
        if (k == s.uid || k == s.shortId || k == lowerOf (s.name) || k == s.xmlType)
            return &s;

    return nullptr;
}

const StockInfo* stockForReaperName (const std::string& fxName)
{
    const auto k = lowerOf (fxName);

    for (auto& s : stockPlugins())
        if (k.find (lowerOf (s.similarTo)) != std::string::npos)
            return &s;

    return nullptr;
}

bool isStockInstrument (const StockInfo& s)
{
    return s.xmlType == drumsXmlType;
}

json stockCatalogRecord (const StockInfo& s)
{
    const bool instrument = isStockInstrument (s);
    return { { "plugin_uid", s.uid }, { "format", "Rovox" }, { "name", s.name }, { "vendor", "Rovox" }, { "version", "1" }, { "kind", instrument ? "instrument" : "effect" },
             { "categories", s.categories }, { "description", s.description },
             { "io", { { "in", instrument ? 0 : 2 }, { "out", 2 }, { "midi_in", instrument } } }, { "latency_samples", nullptr }, { "params_count", nullptr },
             { "file", "builtin" }, { "connector", "local" }, { "server_use_ok", true }, { "builtin", true } };
}

void registerStockPlugins (te::Engine& engine)
{
    auto& pm = engine.getPluginManager();
    pm.createBuiltInType<RovoxEq>();
    pm.createBuiltInType<RovoxComp>();
    pm.createBuiltInType<RovoxGate>();
    pm.createBuiltInType<RovoxLimit>();
    pm.createBuiltInType<RovoxDelay>();
    pm.createBuiltInType<RovoxVerb>();
    pm.createBuiltInType<RovoxFir>();
    registerDrumsPlugin (engine);
    registerPanStage (engine);
    registerFadeStage (engine);
}

json stockPluginExtras (te::Plugin& p)
{
    if (auto* sp = dynamic_cast<StockPlugin*> (&p))
        return sp->extras();

    return nullptr;
}

} // namespace rovox
