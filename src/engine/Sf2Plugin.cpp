// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Sf2Plugin.h"

#include "InstrumentPacks.h"

#include <fluidsynth.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <vector>

namespace rovox
{

std::string sf2PresetUid (const std::string& pack, int bank, int program)
{
    return "rovox:sf2/" + pack + "/" + std::to_string (bank) + "-" + std::to_string (program);
}

bool isSf2Uid (const std::string& uid)
{
    return uid == "rovox:sf2" || uid.rfind ("rovox:sf2/", 0) == 0;
}

std::optional<Sf2Preset> parseSf2PresetUid (const std::string& uid)
{
    const std::string prefix = "rovox:sf2/";

    if (uid.rfind (prefix, 0) != 0)
        return std::nullopt;

    const auto rest = uid.substr (prefix.size());
    const auto slash = rest.rfind ('/');

    if (slash == std::string::npos || slash == 0)
        return std::nullopt;

    const auto preset = rest.substr (slash + 1);
    const auto dash = preset.find ('-');

    if (dash == std::string::npos || dash == 0 || dash + 1 >= preset.size())
        return std::nullopt;

    const auto digits = [] (const std::string& s) { return ! s.empty() && s.size() <= 5 && std::all_of (s.begin(), s.end(), [] (unsigned char c) { return std::isdigit (c) != 0; }); };
    const auto bank = preset.substr (0, dash), program = preset.substr (dash + 1);

    if (! digits (bank) || ! digits (program))
        return std::nullopt;

    return Sf2Preset { rest.substr (0, slash), std::stoi (bank), std::stoi (program) };
}

namespace
{
const char* const defaultPackProperty = "";

struct MidiEvent
{
    enum Kind { noteOff, noteOn, controller, pitchBend, programChange, allNotesOff };

    int offset, kind, a, b;
};

//==============================================================================
class RovoxSf2 : public te::Plugin
{
public:
    static inline const char* xmlTypeName = sf2XmlType;

    RovoxSf2 (te::PluginCreationInfo info) : te::Plugin (info), listener_ (*this)
    {
        auto* um = getUndoManager();
        gain_.referTo (state, "gain", um, 0.0f);
        gainParam_ = addParam ("gain", "Output gain", juce::NormalisableRange<float> (-48.0f, 12.0f),
                               [] (float v) { return juce::String (v, 1) + " dB"; },
                               [] (const juce::String& s) { return s.getFloatValue(); });
        gainParam_->attachToCurrentValue (gain_);
        state.addListener (&listener_);
    }

    ~RovoxSf2() override
    {
        state.removeListener (&listener_);
        notifyListenersOfDeletion();
        gainParam_->detachFromCurrentValue();
        std::lock_guard<std::mutex> lock (mutex_);
        destroySynthLocked();
    }

    juce::String getName() const override                   { return "Rovox SF2"; }
    juce::String getPluginType() override                   { return xmlTypeName; }
    juce::String getVendor() override                       { return "Rovox"; }
    juce::String getShortName (int) override                { return "SF2"; }
    juce::String getSelectableDescription() override        { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override      { return 2; }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                          { return true; }
    bool isSynth() override                                 { return true; }
    bool producesAudioWhenNoAudioInput() override           { return true; }
    bool noTail() override                                  { return false; }
    double getTailLength() const override                   { return 2.0; }
    void deinitialise() override                            {}

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        std::lock_guard<std::mutex> lock (mutex_);
        rate_ = info.sampleRate > 0.0 ? info.sampleRate : 44100.0;
        const int block = std::max (1, info.blockSizeSamples);
        scratchL_.assign ((size_t) block, 0.0f);
        scratchR_.assign ((size_t) block, 0.0f);
        events_.reserve (2048);
        rebuildLocked();        // a fresh synth for every playback: nothing of an earlier render (voices, controllers) is left
    }

    void reset() override
    {
        std::unique_lock<std::mutex> lock (mutex_, std::try_to_lock);

        if (lock.owns_lock() && synth_ != nullptr)
            fluid_synth_all_notes_off (synth_, 0);
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        for (int c = 0; c < fc.destBuffer->getNumChannels(); ++c)
            fc.destBuffer->clear (c, fc.bufferStartSample, fc.bufferNumSamples);

        std::unique_lock<std::mutex> lock (mutex_, std::try_to_lock);

        if (! lock.owns_lock() || synth_ == nullptr || scratchL_.empty())
            return;

        juce::ScopedNoDenormals noDenormals;
        collectEvents (fc);
        const float g = std::pow (10.0f, gainParam_->getCurrentValue() / 20.0f);
        const int total = fc.bufferNumSamples;
        int done = 0;
        size_t next = 0;

        while (done < total)
        {
            // events at the current position first, then render up to the next event (or the end of the block, or the scratch size)
            while (next < events_.size() && events_[next].offset <= done)
                apply (events_[next++]);

            int count = std::min (total - done, (int) scratchL_.size());

            if (next < events_.size())
                count = std::min (count, std::max (1, events_[next].offset - done));

            fluid_synth_write_float (synth_, count, scratchL_.data(), 0, 1, scratchR_.data(), 0, 1);

            if (fc.destBuffer->getNumChannels() >= 2)
            {
                float* l = fc.destBuffer->getWritePointer (0, fc.bufferStartSample + done);
                float* r = fc.destBuffer->getWritePointer (1, fc.bufferStartSample + done);

                for (int i = 0; i < count; ++i)
                {
                    l[i] = scratchL_[(size_t) i] * g;
                    r[i] = scratchR_[(size_t) i] * g;
                }
            }
            else
            {
                float* m = fc.destBuffer->getWritePointer (0, fc.bufferStartSample + done);

                for (int i = 0; i < count; ++i)
                    m[i] = 0.5f * (scratchL_[(size_t) i] + scratchR_[(size_t) i]) * g;
            }

            done += count;
        }
    }

    json extras()
    {
        const auto pack = currentPack();
        const int bank = currentBank(), program = currentProgram();
        json j { { "pack", pack }, { "bank", bank }, { "program", program }, { "status", "ready" } };
        auto found = pack.empty() ? std::nullopt : InstrumentPacks::find (pack);

        if (! found)
        {
            j["status"] = "error";
            j["error"] = pack.empty() ? std::string ("no instrument pack is chosen (property pack)") : "instrument pack '" + pack + "' is not installed (ROVOX_INSTRUMENTS_PATH)";
            return j;
        }

        std::error_code ec;
        const auto file = found->soundfontPath();
        j["soundfont"] = file.filename().string();
        j["license"] = found->license;
        j["attribution"] = found->attribution;
        j["source_url"] = found->sourceUrl;
        j["pack_version"] = found->version;

        for (auto& i : found->instruments)
            if (i.bank == bank && i.program == program)
                j["preset"] = i.name;

        std::lock_guard<std::mutex> lock (mutex_);

        if (! fs::is_regular_file (file, ec))
        {
            j["status"] = "error";
            j["error"] = "soundfont " + file.string() + " is missing";
        }
        else if (! loadError_.empty())
        {
            j["status"] = "error";
            j["error"] = loadError_;
        }

        return j;
    }

private:
    struct Listener : juce::ValueTree::Listener
    {
        explicit Listener (RovoxSf2& o) : owner (o) {}

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& id) override
        {
            if (tree == owner.state && (id == juce::Identifier ("pack") || id == juce::Identifier ("bank") || id == juce::Identifier ("program")))
            {
                std::lock_guard<std::mutex> lock (owner.mutex_);

                if (owner.rate_ > 0.0)      // before initialise() the first load happens there
                    owner.rebuildLocked();
            }
        }

        RovoxSf2& owner;
    };

    std::string currentPack() const         { return toStd (state.getProperty ("pack", juce::String (defaultPackProperty)).toString()); }
    int currentBank() const                 { return std::clamp ((int) state.getProperty ("bank", 0), 0, 16383); }
    int currentProgram() const              { return std::clamp ((int) state.getProperty ("program", 0), 0, 127); }

    void collectEvents (const te::PluginRenderContext& fc)
    {
        events_.clear();

        if (fc.bufferForMidiMessages == nullptr)
            return;

        if (fc.bufferForMidiMessages->isAllNotesOff)
            events_.push_back ({ 0, MidiEvent::allNotesOff, 0, 0 });

        for (auto& m : *fc.bufferForMidiMessages)
        {
            MidiEvent e { std::clamp (juce::roundToInt ((m.getTimeStamp() + fc.midiBufferOffset) * rate_), 0, fc.bufferNumSamples - 1), 0, 0, 0 };

            if (m.isNoteOn())                   { e.kind = MidiEvent::noteOn; e.a = m.getNoteNumber(); e.b = (int) m.getVelocity(); }
            else if (m.isNoteOff())             { e.kind = MidiEvent::noteOff; e.a = m.getNoteNumber(); }
            else if (m.isController())          { e.kind = MidiEvent::controller; e.a = m.getControllerNumber(); e.b = m.getControllerValue(); }
            else if (m.isPitchWheel())          { e.kind = MidiEvent::pitchBend; e.a = m.getPitchWheelValue(); }
            else if (m.isProgramChange())       { e.kind = MidiEvent::programChange; e.a = m.getProgramChangeNumber(); }
            else if (m.isAllNotesOff() || m.isAllSoundOff()) { e.kind = MidiEvent::allNotesOff; }
            else                                continue;

            if (events_.size() < events_.capacity())        // beyond the reserved capacity events are dropped, never allocated for
                events_.push_back (e);
        }

        // insertion sort: stable and allocation free (blocks hold a handful of events)
        for (size_t i = 1; i < events_.size(); ++i)
            for (size_t j = i; j > 0 && events_[j - 1].offset > events_[j].offset; --j)
                std::swap (events_[j - 1], events_[j]);
    }

    void apply (const MidiEvent& e)
    {
        switch (e.kind)
        {
            case MidiEvent::noteOn:         fluid_synth_noteon (synth_, 0, e.a, e.b); break;
            case MidiEvent::noteOff:        fluid_synth_noteoff (synth_, 0, e.a); break;
            case MidiEvent::controller:     fluid_synth_cc (synth_, 0, e.a, e.b); break;
            case MidiEvent::pitchBend:      fluid_synth_pitch_bend (synth_, 0, e.a); break;
            case MidiEvent::programChange:  fluid_synth_program_change (synth_, 0, e.a); break;
            case MidiEvent::allNotesOff:    fluid_synth_all_notes_off (synth_, 0); break;
            default: break;
        }
    }

    void destroySynthLocked()
    {
        if (synth_ != nullptr)
            delete_fluid_synth (synth_);

        if (settings_ != nullptr)
            delete_fluid_settings (settings_);

        synth_ = nullptr;
        settings_ = nullptr;
    }

    /** Builds the synth for the chosen pack and preset (mutex_ held). A failure leaves no synth (the plugin is silent) and is reported by extras(). */
    void rebuildLocked()
    {
        destroySynthLocked();
        loadError_.clear();
        const auto pack = currentPack();

        if (pack.empty())
        {
            loadError_ = "no instrument pack is chosen (property pack)";
            return;
        }

        auto found = InstrumentPacks::find (pack);

        if (! found)
        {
            loadError_ = "instrument pack '" + pack + "' is not installed (ROVOX_INSTRUMENTS_PATH)";
            return;
        }

        const auto file = found->soundfontPath();
        std::error_code ec;

        if (! fs::is_regular_file (file, ec))
        {
            loadError_ = "soundfont " + file.string() + " is missing";
            return;
        }

        settings_ = new_fluid_settings();

        if (settings_ == nullptr)
        {
            loadError_ = "FluidSynth could not create its settings";
            return;
        }

        // determinism: one synth, driven only from here (no audio driver, no worker threads), no reverb or chorus
        fluid_settings_setnum (settings_, "synth.sample-rate", std::clamp (rate_, 8000.0, 96000.0));
        fluid_settings_setint (settings_, "synth.reverb.active", 0);
        fluid_settings_setint (settings_, "synth.chorus.active", 0);
        fluid_settings_setint (settings_, "synth.cpu-cores", 1);
        fluid_settings_setint (settings_, "synth.midi-channels", 16);
        fluid_settings_setint (settings_, "synth.dynamic-sample-loading", 0);
        fluid_settings_setint (settings_, "synth.lock-memory", 0);
        fluid_settings_setnum (settings_, "synth.gain", 0.5);
        synth_ = new_fluid_synth (settings_);

        if (synth_ == nullptr)
        {
            destroySynthLocked();
            loadError_ = "FluidSynth could not create a synthesizer";
            return;
        }

        const int sfont = fluid_synth_sfload (synth_, file.string().c_str(), 1);

        if (sfont == FLUID_FAILED)
        {
            destroySynthLocked();
            loadError_ = "FluidSynth could not load the soundfont " + file.string();
            return;
        }

        if (fluid_synth_program_select (synth_, 0, (unsigned) sfont, (unsigned) currentBank(), (unsigned) currentProgram()) == FLUID_FAILED)
        {
            destroySynthLocked();
            loadError_ = "the soundfont of pack '" + pack + "' has no preset " + std::to_string (currentBank()) + ":" + std::to_string (currentProgram());
        }
    }

    juce::CachedValue<float> gain_;
    te::AutomatableParameter::Ptr gainParam_;
    Listener listener_;

    std::mutex mutex_;                  // the synth, rate, scratch and events; the audio thread only try-locks it
    fluid_settings_t* settings_ = nullptr;
    fluid_synth_t* synth_ = nullptr;
    double rate_ = 0.0;
    std::string loadError_;
    std::vector<float> scratchL_, scratchR_;
    std::vector<MidiEvent> events_;
};
} // namespace

void registerSf2Plugin (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<RovoxSf2>();
}

Result<te::Plugin::Ptr> createSf2Plugin (te::Edit& edit, const Sf2Preset& preset)
{
    if (! InstrumentPacks::find (preset.pack))
        return makeError (errc::notFound, "instrument pack '" + preset.pack + "' is not installed (rovox packs list shows the installed ones)", json { { "pack", preset.pack } });

    auto v = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, juce::String (RovoxSf2::xmlTypeName));
    v.setProperty ("pack", toJuce (preset.pack), nullptr);
    v.setProperty ("bank", preset.bank, nullptr);
    v.setProperty ("program", preset.program, nullptr);
    auto plugin = edit.getPluginCache().createNewPlugin (v);

    if (plugin == nullptr)
        return makeError (errc::preconditionFailed, "SF2 plugin could not be created");

    return plugin;
}

json sf2PluginExtras (te::Plugin& p)
{
    if (auto* s = dynamic_cast<RovoxSf2*> (&p))
        return s->extras();

    return nullptr;
}

json sf2PackIdentity (te::Plugin& p)
{
    auto extras = sf2PluginExtras (p);

    if (! extras.is_object() || ! extras.contains ("pack_version"))
        return nullptr;

    auto found = InstrumentPacks::find (extras["pack"].get<std::string>());

    if (! found)
        return nullptr;

    return { { "id", found->id }, { "version", found->version }, { "sha256", InstrumentPacks::contentSha256 (*found) }, { "license", found->license }, { "source", found->sourceUrl } };
}

} // namespace rovox
