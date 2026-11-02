// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "StockPlugins.h"

#include "Limiter.h"
#include "StockFx.h"

#include <algorithm>
#include <atomic>
#include <memory>

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
        static const std::vector<std::string> types { "low_shelf", "high_shelf", "peak", "low_pass", "high_pass", "notch", "band_pass" };
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
            out.push_back (num (p + "gain", n + "gain", -30.0f, 30.0f, 0.0f, "dB", 2));
            out.push_back (num (p + "bw", n + "bandwidth", 0.05f, 6.0f, d[b].bw, "oct", 2));
        }

        out.push_back (num ("out_gain", "Output gain", -24.0f, 24.0f, 0.0f, "dB", 2));
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

        if (! (p == eq_.params()))
            eq_.setParams (p);

        eq_.process (io, n, count);
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
    static std::vector<ParamDef> definitions()
    {
        return { num ("threshold", "Threshold", -60.0f, 0.0f, -18.0f, "dB", 1),
                 num ("ratio", "Ratio", 1.0f, 100.0f, 4.0f, ":1", 2, 6.0f),
                 num ("attack", "Attack", 0.01f, 500.0f, 10.0f, "ms", 2, 30.0f),
                 num ("release", "Release", 1.0f, 5000.0f, 100.0f, "ms", 1, 300.0f),
                 num ("knee", "Knee", 0.0f, 30.0f, 6.0f, "dB", 1),
                 num ("lookahead", "Pre-comp (lookahead)", 0.0f, 20.0f, 0.0f, "ms", 2),
                 num ("makeup", "Makeup gain", -12.0f, 36.0f, 0.0f, "dB", 1),
                 toggle ("auto_makeup", "Auto makeup", false),
                 num ("rms", "RMS size (0 = peak)", 0.0f, 200.0f, 0.0f, "ms", 1, 20.0f),
                 num ("mix", "Wet (parallel mix)", 0.0f, 100.0f, 100.0f, "%", 0) };
    }

    fx::CompParams readParams() const
    {
        return { value (0), value (1), value (2), value (3), value (4), value (5), value (6), flag (7), value (8), value (9) / 100.0 };
    }

    void prepareDsp (double sr) override        { sr_ = sr; comp_.prepare (sr); comp_.setParams (readParams()); }
    void resetDsp() override                    { comp_.reset(); }
    int latencySamples() const override         { return (int) std::lround (value (5) * 0.001 * sampleRate); }
    json meters() const override                { return { { "gain_reduction_db", (double) grDb_.load() } }; }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        comp_.setParams (readParams());
        comp_.process (io, n, count);
        grDb_ = (float) comp_.lastGainReductionDb();
    }

    fx::Comp comp_;
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

private:
    static std::vector<ParamDef> definitions()
    {
        return { num ("threshold", "Threshold", -80.0f, 0.0f, -40.0f, "dB", 1),
                 num ("attack", "Attack", 0.01f, 500.0f, 1.0f, "ms", 2, 10.0f),
                 num ("hold", "Hold", 0.0f, 2000.0f, 50.0f, "ms", 1, 100.0f),
                 num ("release", "Release", 1.0f, 5000.0f, 100.0f, "ms", 1, 300.0f),
                 num ("hysteresis", "Hysteresis", 0.0f, 24.0f, 3.0f, "dB", 1),
                 num ("range", "Range (closed attenuation)", -100.0f, 0.0f, -80.0f, "dB", 1) };
    }

    fx::GateParams readParams() const
    {
        return { value (0), value (1), value (2), value (3), value (4), value (5) };
    }

    void prepareDsp (double sr) override        { sr_ = sr; gate_.prepare (sr); gate_.setParams (readParams()); }
    void resetDsp() override                    { gate_.reset(); }
    json meters() const override                { return { { "gate_gain_db", (double) gainDb_.load() }, { "open", open_.load() } }; }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        gate_.setParams (readParams());
        gate_.process (io, n, count);
        gainDb_ = (float) gate_.gainDb();
        open_ = gate_.isOpen();
    }

    fx::Gate gate_;
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
    static std::vector<ParamDef> definitions()
    {
        return { num ("input_gain", "Input gain", 0.0f, 24.0f, 0.0f, "dB", 1),
                 num ("ceiling", "Ceiling (true peak)", -20.0f, 0.0f, -1.0f, "dBTP", 2),
                 num ("release", "Release", 1.0f, 2000.0f, 80.0f, "ms", 1, 100.0f) };
    }

    void prepareDsp (double sr) override        { sr_ = sr; limiter_.prepare (sr, 2); limiter_.setParams (value (0), value (1), value (2), 0.1); }
    void resetDsp() override                    { limiter_.reset(); }
    int latencySamples() const override         { return limiter_.latencySamples(); }
    json meters() const override                { return { { "gain_reduction_db", (double) grDb_.load() } }; }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        limiter_.setParams (value (0), value (1), value (2), 0.1);
        limiter_.process (io, n, count);
        grDb_ = (float) limiter_.lastGainReductionDb();
    }

    StreamingLimiter limiter_;
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
    double getTailLength() const override           { return fx::Delay::tailSeconds (readParams (bpm_.load())); }

private:
    static constexpr size_t perTap = 6, shared = 4 * perTap;

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
            out.push_back (num (p + "time", n + "time", 1.0f, 8000.0f, times[t], "ms", 2, 500.0f));
            out.push_back (num (p + "beats", n + "length", 0.0625f, 16.0f, 0.5f * (float) (t + 1), "beats", 4, 1.0f));
            out.push_back (num (p + "gain", n + "gain", -60.0f, 12.0f, 0.0f, "dB", 1));
            out.push_back (num (p + "pan", n + "pan", -1.0f, 1.0f, t == 1 ? -0.5f : t == 2 ? 0.5f : 0.0f, "", 2));
        }

        out.push_back (num ("feedback", "Feedback (tap 1)", 0.0f, 95.0f, 30.0f, "%", 1));
        out.push_back (num ("lowpass", "Feedback low-pass", 200.0f, 20000.0f, 20000.0f, "Hz", 0, 4000.0f));
        out.push_back (num ("highpass", "Feedback high-pass", 20.0f, 2000.0f, 20.0f, "Hz", 0, 200.0f));
        out.push_back (num ("wet", "Wet", -96.0f, 12.0f, -6.0f, "dB", 1));
        out.push_back (num ("dry", "Dry", -96.0f, 12.0f, 0.0f, "dB", 1));
        return out;
    }

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

    void prepareDsp (double sr) override        { sr_ = sr; delay_.prepare (sr); delay_.setParams (readParams (bpm_.load())); }
    void resetDsp() override                    { delay_.reset(); }

    void run (float* const* io, int n, int count, const te::PluginRenderContext& fc) override
    {
        bool synced = false;

        for (size_t t = 0; t < 4; ++t)
            synced = synced || flag (t * perTap + 1);

        if (synced)
            bpm_ = edit.tempoSequence.getBpmAt (fc.editTime.getStart());

        delay_.setParams (readParams (bpm_.load()));
        delay_.process (io, n, count);
    }

    fx::Delay delay_;
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
    double getTailLength() const override           { return fx::Verb::tailSeconds (readParams()); }

private:
    static std::vector<ParamDef> definitions()
    {
        return { num ("room_size", "Room size", 0.0f, 1.0f, 0.5f, "", 3),
                 num ("damping", "Damping", 0.0f, 1.0f, 0.5f, "", 3),
                 num ("pre_delay", "Pre-delay", 0.0f, 250.0f, 0.0f, "ms", 1, 40.0f),
                 num ("width", "Width", 0.0f, 1.0f, 1.0f, "", 2),
                 num ("low_cut", "Low cut", 20.0f, 1000.0f, 20.0f, "Hz", 0, 100.0f),
                 num ("high_cut", "High cut", 1000.0f, 20000.0f, 20000.0f, "Hz", 0, 6000.0f),
                 num ("wet", "Wet", -96.0f, 12.0f, -12.0f, "dB", 1),
                 num ("dry", "Dry", -96.0f, 12.0f, 0.0f, "dB", 1) };
    }

    fx::VerbParams readParams() const
    {
        return { value (0), value (1), value (2), value (3), value (4), value (5), value (6), value (7) };
    }

    void prepareDsp (double sr) override        { sr_ = sr; verb_.prepare (sr); verb_.setParams (readParams()); }
    void resetDsp() override                    { verb_.reset(); }

    void run (float* const* io, int n, int count, const te::PluginRenderContext&) override
    {
        verb_.setParams (readParams());
        verb_.process (io, n, count);
    }

    fx::Verb verb_;
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
          { "stock", "reverb", "space" } } };

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

json stockCatalogRecord (const StockInfo& s)
{
    return { { "plugin_uid", s.uid }, { "format", "Rovox" }, { "name", s.name }, { "vendor", "Rovox" }, { "version", "1" }, { "kind", "effect" },
             { "categories", s.categories }, { "description", s.description },
             { "io", { { "in", 2 }, { "out", 2 }, { "midi_in", false } } }, { "latency_samples", nullptr }, { "params_count", nullptr },
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
}

json stockPluginExtras (te::Plugin& p)
{
    if (auto* sp = dynamic_cast<StockPlugin*> (&p))
        return sp->extras();

    return nullptr;
}

} // namespace rovox
