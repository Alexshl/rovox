// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "JsfxPlugin.h"

#include <ysfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>

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

std::vector<std::string> splitPath (const std::string& name)
{
    std::vector<std::string> parts;
    std::string cur;

    for (char c : name)
    {
        if (c == '/' || c == '\\')
        {
            if (! cur.empty() && cur != ".")
                parts.push_back (cur);

            cur.clear();
        }
        else
        {
            cur += c;
        }
    }

    if (! cur.empty() && cur != ".")
        parts.push_back (cur);

    return parts;
}

/** A child of `dir` named `name` (exact case first, then any case); wantFile selects regular files, otherwise directories. */
std::optional<fs::path> matchChild (const fs::path& dir, const std::string& name, bool wantFile)
{
    std::error_code ec;
    const auto exact = dir / name;

    if (wantFile ? fs::is_regular_file (exact, ec) : fs::is_directory (exact, ec))
        return exact;

    const auto wanted = lowerOf (name);

    for (fs::directory_iterator it (dir, fs::directory_options::skip_permission_denied, ec), end; ! ec && it != end; it.increment (ec))
        if (lowerOf (it->path().filename().string()) == wanted && (wantFile ? it->is_regular_file (ec) : it->is_directory (ec)))
            return it->path();

    return std::nullopt;
}

std::string relativeTo (const fs::path& file, const fs::path& root)
{
    return file.lexically_relative (root).generic_string();
}
} // namespace

std::vector<fs::path> jsfxRoots()
{
    const char* env = std::getenv ("ROVOX_JSFX_PATH");
    const std::string value = env != nullptr && *env != 0 ? env : "/jsfx";
    std::vector<fs::path> out;
    std::string cur;

    for (size_t i = 0; i <= value.size(); ++i)
    {
        if (i == value.size() || value[i] == ':')
        {
            if (! cur.empty())
                out.emplace_back (cur);

            cur.clear();
        }
        else
        {
            cur += value[i];
        }
    }

    return out;
}

std::optional<JsfxFile> findJsfx (const std::string& name)
{
    if (! name.empty() && (name[0] == '/' || name[0] == '\\'))
        return std::nullopt;

    const auto parts = splitPath (name);

    if (parts.empty() || std::find (parts.begin(), parts.end(), "..") != parts.end())
        return std::nullopt;

    for (auto& root : jsfxRoots())
    {
        std::error_code ec;

        if (! fs::is_directory (root, ec))
            continue;

        fs::path dir = root;
        bool ok = true;

        for (size_t i = 0; i + 1 < parts.size() && ok; ++i)
        {
            if (auto next = matchChild (dir, parts[i], false))
                dir = *next;
            else
                ok = false;
        }

        if (! ok)
            continue;

        for (const auto& candidate : { parts.back(), parts.back() + ".jsfx" })
            if (auto file = matchChild (dir, candidate, true))
                return JsfxFile { *file, root, relativeTo (*file, root) };
    }

    return std::nullopt;
}

//==============================================================================
namespace
{
std::mutex catalogMutex;
std::string catalogRoots;
std::vector<json> catalogRecords;
bool catalogValid = false;

/** First "desc:" line within the head of a text file, or nullopt if the file does not look like a JSFX. */
std::optional<std::string> descOf (const fs::path& file)
{
    std::ifstream in (file, std::ios::binary);
    std::string head (4096, '\0');
    in.read (head.data(), (std::streamsize) head.size());
    head.resize ((size_t) in.gcount());

    if (head.find ('\0') != std::string::npos)
        return std::nullopt;

    size_t pos = 0;

    while (pos < head.size())
    {
        auto eol = head.find ('\n', pos);
        auto line = head.substr (pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? head.size() : eol + 1;

        if (line.rfind ("desc:", 0) == 0)
        {
            line = line.substr (5);
            const auto a = line.find_first_not_of (" \t");
            const auto b = line.find_last_not_of (" \t\r");
            return a == std::string::npos ? std::string() : line.substr (a, b - a + 1);
        }
    }

    return std::nullopt;
}

void scanRoots()
{
    std::string key;

    for (auto& r : jsfxRoots())
        key += r.string() + ":";

    if (catalogValid && key == catalogRoots)
        return;

    catalogRecords.clear();
    catalogRoots = key;
    catalogValid = true;
    std::set<std::string> seen;

    for (auto& root : jsfxRoots())
    {
        std::error_code ec;

        for (fs::recursive_directory_iterator it (root, fs::directory_options::skip_permission_denied, ec), end; ! ec && it != end; it.increment (ec))
        {
            if (it.depth() > 6 || (it->is_directory (ec) && it->path().filename().string().rfind (".", 0) == 0))
            {
                if (it->is_directory (ec))
                    it.disable_recursion_pending();

                continue;
            }

            if (! it->is_regular_file (ec) || it->path().filename().string().rfind (".", 0) == 0)
                continue;

            const auto ext = lowerOf (it->path().extension().string());

            if ((! ext.empty() && ext != ".jsfx") || it->file_size (ec) > 2 * 1024 * 1024)
                continue;

            const auto rel = relativeTo (it->path(), root);
            const auto desc = descOf (it->path());

            if (! desc || ! seen.insert (lowerOf (rel)).second)
                continue;

            json categories = json::array ({ "jsfx" });

            if (auto folder = fs::path (rel).parent_path().generic_string(); ! folder.empty())
                categories.push_back (lowerOf (folder));

            catalogRecords.push_back ({ { "plugin_uid", "jsfx:" + rel }, { "format", "JSFX" }, { "name", desc->empty() ? rel : *desc }, { "vendor", "REAPER JS" },
                                        { "version", "1" }, { "kind", "effect" }, { "categories", categories }, { "description", "JSFX " + rel },
                                        { "io", { { "in", 2 }, { "out", 2 }, { "midi_in", false } } }, { "latency_samples", nullptr }, { "params_count", nullptr },
                                        { "file", it->path().string() }, { "connector", "local" }, { "server_use_ok", true }, { "builtin", true } });
        }
    }
}
} // namespace

std::vector<json> jsfxCatalogRecords()
{
    std::lock_guard<std::mutex> lock (catalogMutex);
    scanRoots();
    return catalogRecords;
}

std::optional<json> jsfxCatalogRecord (const std::string& key)
{
    auto name = isJsfxUid (key) ? key.substr (5) : key;
    auto found = findJsfx (name);

    if (! found)
        return std::nullopt;

    for (auto& r : jsfxCatalogRecords())
        if (r["plugin_uid"] == "jsfx:" + found->rel)
            return r;

    return std::nullopt;
}

void jsfxRescan()
{
    std::lock_guard<std::mutex> lock (catalogMutex);
    catalogValid = false;
}

bool isJsfxUid (const std::string& s)       { return s.rfind ("jsfx:", 0) == 0; }

//==============================================================================
namespace
{
struct YsfxDeleter     { void operator() (ysfx_t* p) const { if (p != nullptr) ysfx_free (p); } };
using YsfxPtr = std::unique_ptr<ysfx_t, YsfxDeleter>;

struct SliderDef
{
    uint32_t index = 0;
    std::string id, name, unit;
    float min = 0.0f, max = 1.0f, def = 0.0f, inc = 0.0f;
    std::vector<std::string> labels;        // enumeration labels (value - min = index)
};

std::string unitOfName (const std::string& name)
{
    const auto close = name.rfind (')'), open = name.rfind ('(');

    if (close == name.size() - 1 && open != std::string::npos && open < close && close - open - 1 <= 8)
        return name.substr (open + 1, close - open - 1);

    return {};
}

class JsfxPlugin : public te::Plugin
{
public:
    static inline const char* xmlTypeName = "rovoxjsfx";

    JsfxPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        file_ = toStd (state["jsfx_file"].toString());
        load();
    }

    ~JsfxPlugin() override
    {
        notifyListenersOfDeletion();

        for (auto& ap : params_)
            ap->detachFromCurrentValue();
    }

    juce::String getName() const override                   { return name_.isNotEmpty() ? name_ : juce::String ("JSFX"); }
    juce::String getPluginType() override                   { return xmlTypeName; }
    juce::String getVendor() override                       { return "REAPER JS"; }
    juce::String getShortName (int) override                { return getName(); }
    juce::String getSelectableDescription() override        { return getName(); }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return juce::jmin (numInputChannels, 2); }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }
    void deinitialise() override                            {}
    bool producesAudioWhenNoAudioInput() override           { return isAutomationNeeded(); }
    void reset() override                                   { if (ready_) resetPending_ = true; }
    double getLatencySeconds() override                     { return sampleRate > 0.0 ? pdcSamples_.load() / sampleRate : 0.0; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        ready_ = false;

        if (fx_ == nullptr)
            return;

        if (! ysfx_is_compiled (fx_.get()))
            return;

        blockSize_ = std::max (1, info.blockSizeSamples);

        for (auto& s : scratch_)
            s.assign ((size_t) blockSize_, 0.0f);

        ysfx_set_sample_rate (fx_.get(), info.sampleRate);
        ysfx_set_block_size (fx_.get(), (uint32_t) blockSize_);
        pushSliders (true);
        ysfx_init (fx_.get());

        // an empty cycle runs @slider and @block once, so that a pdc_delay that is set there is known before the first block
        const float* zeroIn[2] = { scratch_[0].data(), scratch_[1].data() };
        float* zeroOut[2] = { scratch_[0].data(), scratch_[1].data() };
        ysfx_process_float (fx_.get(), zeroIn, zeroOut, 2, 2, 0);
        pdcSamples_ = ysfx_get_pdc_delay (fx_.get());
        resetPending_ = false;
        ready_ = true;
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (! ready_ || fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        const int n = std::min (2, fc.destBuffer->getNumChannels());

        if (n == 0)
            return;

        juce::ScopedNoDenormals noDenormals;

        if (resetPending_.exchange (false))
        {
            ysfx_init (fx_.get());
            pdcSamples_ = ysfx_get_pdc_delay (fx_.get());
        }

        pushSliders (false);

        ysfx_time_info_t t {};
        t.tempo = edit.tempoSequence.getBpmAt (fc.editTime.getStart());
        t.playback_state = fc.isPlaying ? ysfx_playback_playing : ysfx_playback_paused;
        t.time_position = fc.editTime.getStart().inSeconds();
        t.beat_position = edit.tempoSequence.toBeats (fc.editTime.getStart()).inBeats();
        t.time_signature[0] = 4;
        t.time_signature[1] = 4;
        ysfx_set_time_info (fx_.get(), &t);

        float* io[2] = { nullptr, nullptr };

        for (int c = 0; c < n; ++c)
            io[c] = fc.destBuffer->getWritePointer (c, fc.bufferStartSample);

        const uint32_t codeOuts = ysfx_get_num_outputs (fx_.get());

        for (int pos = 0; pos < fc.bufferNumSamples; pos += blockSize_)
        {
            const int count = std::min (blockSize_, fc.bufferNumSamples - pos);
            const float* ins[2] = { nullptr, nullptr };
            float* outs[2] = { nullptr, nullptr };

            for (int c = 0; c < n; ++c)
            {
                std::copy (io[c] + pos, io[c] + pos + count, scratch_[(size_t) c].data());
                ins[c] = scratch_[(size_t) c].data();
                outs[c] = io[c] + pos;
            }

            ysfx_process_float (fx_.get(), ins, outs, (uint32_t) n, (uint32_t) n, (uint32_t) count);

            // channels the effect does not produce pass through (REAPER does the same), also for effects without @sample
            for (uint32_t c = codeOuts; c < (uint32_t) n; ++c)
                std::copy (ins[c], ins[c] + count, outs[c]);

            if (! hasSample_)
                for (int c = 0; c < n; ++c)
                    std::copy (ins[c], ins[c] + count, outs[c]);
        }

        pdcSamples_ = ysfx_get_pdc_delay (fx_.get());
    }

    json extras() const
    {
        json units = json::object();

        for (auto& d : defs_)
            units[d.id] = d.labels.empty() ? json (d.unit) : json ("choice: " + [&] { std::string all; for (auto& c : d.labels) all += (all.empty() ? "" : "|") + c; return all; }());

        return { { "units", units }, { "jsfx_file", file_ }, { "latency_samples", (int) std::lround (pdcSamples_.load()) }, { "loaded", fx_ != nullptr && loadError_.empty() },
                 { "load_error", loadError_ } };
    }

private:
    static void logReporter (intptr_t user, ysfx_log_level level, const char* message)
    {
        auto* self = reinterpret_cast<JsfxPlugin*> (user);

        if (self->capturing_ && level == ysfx_log_error && message != nullptr)
            self->errors_ += (self->errors_.empty() ? "" : "; ") + std::string (message);
    }

    void load()
    {
        if (file_.empty())
        {
            loadError_ = "no JSFX file given";
            return;
        }

        auto found = findJsfx (file_);

        if (! found)
        {
            loadError_ = "JSFX file not found under ROVOX_JSFX_PATH: " + file_;
            return;
        }

        std::unique_ptr<ysfx_config_t, void (*) (ysfx_config_t*)> config (ysfx_config_new(), ysfx_config_free);
        ysfx_set_import_root (config.get(), found->root.string().c_str());
        const auto dataRoot = (fs::temp_directory_path() / "rovox-jsfx-data").string();
        ysfx_set_data_root (config.get(), dataRoot.c_str());
        ysfx_register_builtin_audio_formats (config.get());
        ysfx_set_log_reporter (config.get(), &logReporter);
        ysfx_set_user_data (config.get(), reinterpret_cast<intptr_t> (this));
        fx_.reset (ysfx_new (config.get()));
        capturing_ = true;
        const bool ok = ysfx_load_file (fx_.get(), found->path.string().c_str(), 0);
        capturing_ = false;

        if (! ok)
        {
            loadError_ = errors_.empty() ? "cannot load " + found->rel : errors_;
            fx_.reset();
            return;
        }

        file_ = found->rel;                         // canonical case, so that the saved path is exact
        state.setProperty ("jsfx_file", toJuce (file_), nullptr);
        name_ = juce::String::fromUTF8 (ysfx_get_name (fx_.get()));
        hasSample_ = ysfx_has_section (fx_.get(), ysfx_section_sample);

        for (uint32_t i = 0; i < (uint32_t) ysfx_max_sliders; ++i)
        {
            ysfx_slider_range_t range {};

            if (! ysfx_slider_exists (fx_.get(), i) || ysfx_slider_is_path (fx_.get(), i) || ! ysfx_slider_get_range (fx_.get(), i, &range))
                continue;

            SliderDef d;
            d.index = i;
            d.id = "s" + std::to_string (i + 1);
            d.name = ysfx_slider_get_name (fx_.get(), i) != nullptr ? ysfx_slider_get_name (fx_.get(), i) : d.id;
            d.min = (float) range.min;
            d.max = (float) range.max;
            d.def = (float) range.def;
            d.inc = (float) range.inc;

            if (d.max <= d.min)
                d.max = d.min + 1.0f;

            if (ysfx_slider_is_enum (fx_.get(), i))
            {
                const auto size = ysfx_slider_get_enum_names (fx_.get(), i, nullptr, 0);

                for (uint32_t k = 0; k < size; ++k)
                    d.labels.emplace_back (ysfx_slider_get_enum_name (fx_.get(), i, k) != nullptr ? ysfx_slider_get_enum_name (fx_.get(), i, k) : "");
            }
            else
            {
                d.unit = unitOfName (d.name);
            }

            defs_.push_back (std::move (d));
        }

        auto* um = getUndoManager();

        for (auto& d : defs_)
        {
            values_.push_back (std::make_unique<juce::CachedValue<float>>());
            values_.back()->referTo (state, juce::Identifier (d.id), um, d.def);
            juce::NormalisableRange<float> range (d.min, d.max, d.inc > 0.0f ? d.inc : 0.0f);
            const int decimals = d.inc >= 1.0f || d.inc <= 0.0f ? (d.inc <= 0.0f ? 3 : 0) : std::min (4, (int) std::ceil (-std::log10 (d.inc)));
            auto* ap = addParam (toJuce (d.id), toJuce (d.name), range,
                                 [def = d, decimals] (float v)
                                 {
                                     if (! def.labels.empty())
                                         return toJuce (def.labels[(size_t) std::clamp ((int) std::lround (v - def.min), 0, (int) def.labels.size() - 1)]);

                                     return juce::String (v, decimals);
                                 },
                                 [def = d] (const juce::String& s)
                                 {
                                     for (size_t i = 0; i < def.labels.size(); ++i)
                                         if (s.trim().equalsIgnoreCase (toJuce (def.labels[i])))
                                             return def.min + (float) i;

                                     return s.getFloatValue();
                                 });
            ap->attachToCurrentValue (*values_.back());
            params_.push_back (ap);
        }

        lastPushed_.assign (defs_.size(), 0.0f);
        errors_.clear();
        capturing_ = true;
        const bool compiled = ysfx_compile (fx_.get(), ysfx_compile_no_gfx);
        capturing_ = false;

        if (! compiled)
            loadError_ = errors_.empty() ? "compile error in " + file_ : errors_;
    }

    /** Parameter values (automation included) into the effect's sliders; only changed ones, so that @slider runs when needed. */
    void pushSliders (bool all)
    {
        for (size_t i = 0; i < defs_.size(); ++i)
        {
            const float v = params_[i]->getCurrentValue();

            if (all || v != lastPushed_[i])
            {
                ysfx_slider_set_value (fx_.get(), defs_[i].index, (ysfx_real) v);
                lastPushed_[i] = v;
            }
        }
    }

    std::string file_, loadError_, errors_;
    juce::String name_;
    YsfxPtr fx_;
    std::vector<SliderDef> defs_;
    std::vector<std::unique_ptr<juce::CachedValue<float>>> values_;
    std::vector<te::AutomatableParameter::Ptr> params_;
    std::vector<float> lastPushed_;
    std::array<std::vector<float>, 2> scratch_;
    int blockSize_ = 512;
    bool hasSample_ = true;
    std::atomic<bool> ready_ { false }, capturing_ { false }, resetPending_ { false };
    std::atomic<double> pdcSamples_ { 0.0 };
};
} // namespace

//==============================================================================
void registerJsfxPlugin (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<JsfxPlugin>();
}

Result<te::Plugin::Ptr> createJsfxPlugin (te::Edit& edit, const std::string& name, const std::map<int, double>& sliders)
{
    auto found = findJsfx (isJsfxUid (name) ? name.substr (5) : name);

    if (! found)
        return makeError (errc::notFound, "JSFX file not found under ROVOX_JSFX_PATH: " + name, json { { "jsfx", name } });

    auto v = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, juce::String (JsfxPlugin::xmlTypeName));
    v.setProperty ("jsfx_file", toJuce (found->rel), nullptr);

    for (auto& [number, value] : sliders)
        if (number >= 1 && number <= ysfx_max_sliders)
            v.setProperty (juce::Identifier ("s" + std::to_string (number)), (float) value, nullptr);

    auto plugin = edit.getPluginCache().createNewPlugin (v);

    if (plugin == nullptr)
        return makeError (errc::preconditionFailed, "JSFX plugin could not be created");

    if (auto* js = dynamic_cast<JsfxPlugin*> (plugin.get()))
    {
        const auto e = js->extras();

        if (! e["loaded"].get<bool>())
            return makeError (errc::pluginLoadFailed, "JSFX " + found->rel + " did not load: " + e["load_error"].get<std::string>());
    }

    return plugin;
}

json jsfxPluginExtras (te::Plugin& p)
{
    if (auto* js = dynamic_cast<JsfxPlugin*> (&p))
        return js->extras();

    return nullptr;
}

} // namespace rovox
