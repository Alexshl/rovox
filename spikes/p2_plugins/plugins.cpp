// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Spike P2: offline render through LV2 plugins in a headless container.
//   rovox_spikes plugins <work-dir> [--lv2-path P] [--vst3-path P] [--list] [--in-process-scan]
//                        [--effect SUBSTR]... [--instrument SUBSTR]... [--latent SUBSTR]
// Checks: (1) scan (child process by default), (2) load without editor, (3) effect applied + deterministic,
// (4) instrument is not silent (incl. note at t=0), (5) PDC with a latency-reporting plugin, (6) 8 tracks x 3 plugins.
// Writes <work-dir>/p2_report.json.

#include "SpikeUtil.h"

#include <array>
#include <cstdio>
#include <deque>

using namespace spike;
using namespace te::literals;

namespace
{
constexpr double sr = 48000.0;

struct Options
{
    juce::File workDir;
    juce::String lv2Path, vst3Path;
    bool list = false, inProcessScan = false;
    juce::StringArray effects, instruments;
    juce::String latent = "Limiter";
};

struct Loaded
{
    te::ExternalPlugin::Ptr plugin;
    double loadMs = 0;
    int latencySamples = 0;
    juce::String error;
};

/** KnownPluginList::getTypes() returns a copy, so keep our own stable storage for the returned description. */
const juce::PluginDescription* findDesc (const juce::KnownPluginList& list, const juce::String& substr)
{
    static std::deque<juce::PluginDescription> store;

    const auto types = list.getTypes();

    for (const bool exact : { true, false })
        for (const auto& d : types)
            if (exact ? d.name.equalsIgnoreCase (substr) : d.name.containsIgnoreCase (substr))
            {
                store.push_back (d);
                return &store.back();
            }

    return nullptr;
}

Loaded addPlugin (te::PluginList& list, te::Edit& edit, const juce::PluginDescription& d, int index)
{
    Loaded l;
    const auto t0 = nowMs();

    if (auto p = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, d))
    {
        list.insertPlugin (p, index, nullptr);
        p->initialiseFully();
        l.plugin = dynamic_cast<te::ExternalPlugin*> (p.get());
    }

    l.loadMs = nowMs() - t0;

    if (l.plugin == nullptr)
        l.error = "createNewPlugin failed";
    else if (auto* inst = l.plugin->getAudioPluginInstance())
        l.latencySamples = inst->getLatencySamples();
    else
        l.error = l.plugin->getLoadError().isNotEmpty() ? l.plugin->getLoadError() : juce::String ("no plugin instance");

    return l;
}

std::unique_ptr<te::Edit> makeEdit (te::Engine& engine, int numTracks)
{
    auto edit = te::Edit::createSingleTrackEdit (engine, te::Edit::EditRole::forRendering);
    edit->ensureNumberOfAudioTracks (numTracks);
    return edit;
}

std::vector<float> makeTestSignal (double seconds)
{
    // 110 Hz + 1 kHz + 6 kHz partials with a slow amplitude wobble: loud enough (about -3 dBFS peak) to trigger dynamics
    const int n = (int) (seconds * sr);
    std::vector<float> s ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = 0.75 + 0.25 * std::sin (juce::MathConstants<double>::twoPi * 0.5 * t);
        s[(size_t) i] = (float) (env * (0.35 * std::sin (juce::MathConstants<double>::twoPi * 110 * t)
                                        + 0.25 * std::sin (juce::MathConstants<double>::twoPi * 1000 * t)
                                        + 0.15 * std::sin (juce::MathConstants<double>::twoPi * 6000 * t)));
    }

    return s;
}

void addMidiNotes (te::MidiClip& clip, std::initializer_list<std::pair<int, double>> notesAtBeat, double lengthBeats)
{
    for (auto [pitch, beat] : notesAtBeat)
        clip.getSequence().addNote (pitch, te::BeatPosition::fromBeats (beat), te::BeatDuration::fromBeats (lengthBeats), 100, 0, nullptr);
}

juce::var scanReport (te::Engine& engine, const Options& o, juce::KnownPluginList& known, std::vector<std::pair<juce::String, double>>& perPlugin)
{
    auto& pm = engine.getPluginManager();
    juce::Array<juce::var> perFormat;

    for (const auto& [formatName, searchPath] : { std::pair<juce::String, juce::String> { "LV2", o.lv2Path }, { "VST3", o.vst3Path } })
    {
        juce::AudioPluginFormat* format = nullptr;

        for (auto* f : pm.pluginFormatManager.getFormats())
            if (f->getName() == formatName)
                format = f;

        if (format == nullptr)
        {
            perFormat.add (makeObject ({ { "format", formatName }, { "error", "format not compiled in" } }));
            continue;
        }

        const auto typesBefore = known.getNumTypes();
        const auto t0 = nowMs();
        juce::PluginDirectoryScanner scanner (known, *format, juce::FileSearchPath (searchPath), true,
                                              o.workDir.getChildFile ("scan-deadman-" + formatName + ".txt"), false);
        juce::String name;

        for (;;)
        {
            const auto s0 = nowMs();
            const bool more = scanner.scanNextFile (true, name);
            perPlugin.emplace_back (name, nowMs() - s0);

            if (! more)
                break;
        }

        juce::Array<juce::var> failed;

        for (auto& f : scanner.getFailedFiles())
            failed.add (f);

        perFormat.add (makeObject ({ { "format", formatName }, { "search_path", searchPath }, { "out_of_process", pm.usesSeparateProcessForScanning() },
                                     { "scan_ms", nowMs() - t0 }, { "found_types", known.getNumTypes() - typesBefore },
                                     { "failed_files", failed } }));
    }

    return makeObject ({ { "formats", perFormat }, { "total_types", known.getNumTypes() }, { "blacklisted", known.getBlacklistedFiles().size() } });
}

// ---- effect: applied and deterministic ----
juce::var testEffect (te::Engine& engine, const Options& o, const juce::PluginDescription& d, const std::vector<float>& signal)
{
    const auto in = o.workDir.getChildFile ("fx_in.wav");
    writeWav (in, { signal }, sr);
    const double seconds = (double) signal.size() / sr;

    auto render = [&] (bool bypass, const juce::String& tag, Loaded& loaded) -> Wav
    {
        auto edit = makeEdit (engine, 1);
        auto* track = te::getAudioTracks (*edit)[0];
        te::insertWaveClip (*track, {}, in, { .time = { 0_tp, te::TimeDuration::fromSeconds (seconds) } }, te::DeleteExistingClips::no);
        loaded = addPlugin (track->pluginList, *edit, d, 0);

        if (loaded.plugin != nullptr && bypass)
            loaded.plugin->setEnabled (false);

        const auto out = o.workDir.getChildFile ("fx_" + tag + ".wav");
        const auto err = renderBlocking (*edit, makeRenderParams (*edit, out, sr, seconds));
        Wav w;

        if (err.isNotEmpty() || ! readWav (out, w))
            std::printf ("  render error (%s): %s\n", tag.toRawUTF8(), err.toRawUTF8());

        loaded.plugin = nullptr;    // must not outlive the Edit that owns it
        return w;
    };

    Loaded l1, l2, l3;
    auto a = render (false, "a", l1);
    auto b = render (false, "b", l2);
    auto byp = render (true, "bypass", l3);

    if (a.ch.empty() || b.ch.empty() || byp.ch.empty())
        return makeObject ({ { "plugin", d.name }, { "error", l1.error.isNotEmpty() ? l1.error : juce::String ("render failed") } });

    const int n = std::min ({ a.numSamples(), b.numSamples(), byp.numSamples() });
    double maxDiffAB = 0, maxDiffBypass = 0;

    for (int i = 0; i < n; ++i)
    {
        maxDiffAB = std::max (maxDiffAB, (double) std::abs (a.ch[0][(size_t) i] - b.ch[0][(size_t) i]));
        maxDiffBypass = std::max (maxDiffBypass, (double) std::abs (a.ch[0][(size_t) i] - byp.ch[0][(size_t) i]));
    }

    return makeObject ({ { "plugin", d.name }, { "uri", d.fileOrIdentifier }, { "load_ms", l1.loadMs },
                         { "load_error", l1.error }, { "latency_samples", l1.latencySamples },
                         { "rms_db_fx", rmsDb (a.ch[0].data(), n) }, { "rms_db_bypass", rmsDb (byp.ch[0].data(), n) },
                         { "rms_delta_db", rmsDb (a.ch[0].data(), n) - rmsDb (byp.ch[0].data(), n) },
                         { "max_abs_diff_vs_bypass", maxDiffBypass },
                         { "max_abs_diff_two_renders", maxDiffAB },
                         { "diff_two_renders_dbfs", toDb (maxDiffAB) },
                         { "applied", maxDiffBypass > 1.0e-4 },
                         { "deterministic", toDb (maxDiffAB) < -120.0 } });
}

// ---- instrument: not silent from the first note ----
enum class WarmUp { none, sleep1500ms, primeBlocks };

const char* toString (WarmUp w)
{
    return w == WarmUp::none ? "none" : (w == WarmUp::sleep1500ms ? "sleep_1500ms_wall_clock" : "prime_200_silent_blocks");
}

/** Runs the plugin instance by hand for N silent blocks (also what a host does before the first real block). */
void primeBlocks (te::ExternalPlugin& plugin, int numBlocks)
{
    if (auto* inst = plugin.getAudioPluginInstance())
    {
        inst->prepareToPlay (sr, 512);
        juce::AudioBuffer<float> buf (std::max (2, std::max (inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels())), 512);
        juce::MidiBuffer midi;

        for (int i = 0; i < numBlocks; ++i)
        {
            buf.clear();
            inst->processBlock (buf, midi);
        }
    }
}

juce::var testInstrument (te::Engine& engine, const Options& o, const juce::PluginDescription& d)
{
    // 120 bpm: 1 beat = 0.5 s. Notes (first, +2 beats, +4, +6): kick 36, middle C 60, snare 38, kick 36 again.
    juce::Array<juce::var> variants;

    struct Variant { bool atZero; WarmUp warm; };

    for (const auto v : { Variant { true, WarmUp::none }, Variant { false, WarmUp::none },
                          Variant { true, WarmUp::sleep1500ms }, Variant { true, WarmUp::primeBlocks } })
    {
        auto edit = makeEdit (engine, 1);
        auto* track = te::getAudioTracks (*edit)[0];
        auto clip = te::insertMIDIClip (*track, "notes", { 0_tp, 8_tp });
        const double firstBeat = v.atZero ? 0.0 : 2.0;
        const std::array<int, 4> pitches { 36, 60, 38, 36 };
        addMidiNotes (*clip, { { pitches[0], firstBeat }, { pitches[1], firstBeat + 2.0 }, { pitches[2], firstBeat + 4.0 }, { pitches[3], firstBeat + 6.0 } }, 1.0);

        auto loaded = addPlugin (track->pluginList, *edit, d, 0);

        if (loaded.plugin != nullptr && v.warm == WarmUp::sleep1500ms)
            juce::Thread::sleep (1500);
        else if (loaded.plugin != nullptr && v.warm == WarmUp::primeBlocks)
            primeBlocks (*loaded.plugin, 200);

        const double seconds = 8.0;
        const auto out = o.workDir.getChildFile ("inst_tmp.wav");
        float peak = 0, rms = 0;
        const auto r0 = nowMs();
        const auto err = loaded.error.isNotEmpty() ? loaded.error : renderBlocking (*edit, makeRenderParams (*edit, out, sr, seconds), &peak, &rms);
        const auto renderMs = nowMs() - r0;
        Wav w;
        juce::Array<juce::var> windows;
        double best = -200;

        if (err.isEmpty() && readWav (out, w))
        {
            int idx = 0;

            for (double beat : { firstBeat, firstBeat + 2.0, firstBeat + 4.0, firstBeat + 6.0 })
            {
                const int start = (int) (beat * 0.5 * sr);
                const auto db = rmsDb (w.ch[0].data() + start, std::min ((int) (0.1 * sr), w.numSamples() - start));
                windows.add (makeObject ({ { "note", pitches[(size_t) idx++] }, { "onset_s", beat * 0.5 }, { "rms_first_100ms_db", db } }));
                best = std::max (best, db);
            }
        }

        loaded.plugin = nullptr;
        variants.add (makeObject ({ { "first_note_at_s", firstBeat * 0.5 }, { "warm_up", toString (v.warm) },
                                    { "load_ms", loaded.loadMs }, { "render_ms", renderMs }, { "error", err },
                                    { "peak_linear", peak }, { "windows", windows },
                                    { "best_window_db", best },
                                    { "first_note_audible_gt_minus40db", windows.size() > 0 && (double) windows[0]["rms_first_100ms_db"] > -40.0 },
                                    { "preflight_not_silent", best > -40.0 } }));
    }

    return makeObject ({ { "plugin", d.name }, { "uri", d.fileOrIdentifier }, { "variants", variants } });
}

// ---- PDC: impulse at 1.000 s through a latency-reporting plugin ----
juce::var testPdcCase (te::Engine& engine, const Options& o, const juce::PluginDescription& d, bool prime)
{
    const int impulseAt = (int) sr;     // 1.000 s
    std::vector<float> imp ((size_t) (3 * sr), 0.0f);
    imp[(size_t) impulseAt] = 0.5f;
    const auto in = o.workDir.getChildFile ("pdc_in.wav");
    writeWav (in, { imp }, sr);

    auto findPeaks = [] (const Wav& w)
    {
        std::vector<int> peaks;

        for (int i = 0; i < w.numSamples(); ++i)
            if (std::abs (w.ch[0][(size_t) i]) > 0.05f)
                peaks.push_back (i);

        return peaks;
    };

    auto toVar = [] (const std::vector<int>& v)
    {
        juce::Array<juce::var> a;

        for (int i : v)
            a.add (i);

        return a;
    };

    juce::var single, two;

    {   // single track: impulse -> plugin
        auto edit = makeEdit (engine, 1);
        auto* t = te::getAudioTracks (*edit)[0];
        te::insertWaveClip (*t, {}, in, { .time = { 0_tp, 3_td } }, te::DeleteExistingClips::no);
        auto l = addPlugin (t->pluginList, *edit, d, 0);
        const int latencyAtLoad = l.latencySamples;

        if (prime && l.plugin != nullptr)
            primeBlocks (*l.plugin, 8);

        const int latencyAfterPrime = l.plugin != nullptr && l.plugin->getAudioPluginInstance() != nullptr ? l.plugin->getAudioPluginInstance()->getLatencySamples() : -1;
        const auto out = o.workDir.getChildFile ("pdc_single.wav");
        const auto err = renderBlocking (*edit, makeRenderParams (*edit, out, sr, 3.0));
        Wav w;

        if (err.isEmpty() && readWav (out, w))
        {
            auto peaks = findPeaks (w);
            single = makeObject ({ { "latency_samples_at_load", latencyAtLoad }, { "latency_samples_before_render", latencyAfterPrime },
                                   { "latency_samples_after_render", l.plugin != nullptr && l.plugin->getAudioPluginInstance() != nullptr ? l.plugin->getAudioPluginInstance()->getLatencySamples() : -1 },
                                   { "output_peak_samples_over_0.05", toVar (peaks) },
                                   { "shift_vs_1.000s_samples", peaks.empty() ? 0 : peaks.front() - impulseAt } });
        }
        else
            single = makeObject ({ { "error", err } });

        l.plugin = nullptr;
    }

    {   // two tracks: dry impulse + impulse through the plugin; PDC aligns them if the output has one cluster at 1.000 s
        auto edit = makeEdit (engine, 2);
        auto tracks = te::getAudioTracks (*edit);
        te::insertWaveClip (*tracks[0], {}, in, { .time = { 0_tp, 3_td } }, te::DeleteExistingClips::no);
        te::insertWaveClip (*tracks[1], {}, in, { .time = { 0_tp, 3_td } }, te::DeleteExistingClips::no);
        auto l = addPlugin (tracks[1]->pluginList, *edit, d, 0);

        if (prime && l.plugin != nullptr)
            primeBlocks (*l.plugin, 8);

        const auto out = o.workDir.getChildFile ("pdc_two.wav");
        const auto err = renderBlocking (*edit, makeRenderParams (*edit, out, sr, 3.0));
        Wav w;

        if (err.isEmpty() && readWav (out, w))
        {
            auto peaks = findPeaks (w);
            two = makeObject ({ { "output_peak_samples_over_0.05", toVar (peaks) },
                                { "first_sample_over_threshold", peaks.empty() ? -1 : peaks.front() },
                                { "cluster_spread_samples", peaks.empty() ? 0 : peaks.back() - peaks.front() } });
        }
        else
            two = makeObject ({ { "error", err } });

        l.plugin = nullptr;
    }

    return makeObject ({ { "plugin", d.name }, { "primed_before_render", prime }, { "single_track", single }, { "two_tracks_dry_vs_plugin", two } });
}

// ---- load: 8 audio tracks x 3 plugins + 1 instrument track + master plugin ----
juce::var testLoad (te::Engine& engine, const Options& o, const std::vector<const juce::PluginDescription*>& fx,
                    const juce::PluginDescription* inst, const juce::PluginDescription* master)
{
    const double seconds = 30.0;
    const auto in = o.workDir.getChildFile ("load_in.wav");
    writeWav (in, { makeTestSignal (seconds) }, sr);

    auto edit = makeEdit (engine, 9);
    auto tracks = te::getAudioTracks (*edit);
    juce::Array<juce::var> loads;
    int plugins = 0;
    const auto t0 = nowMs();

    for (int i = 0; i < 8; ++i)
    {
        te::insertWaveClip (*tracks[(size_t) i], {}, in, { .time = { 0_tp, te::TimeDuration::fromSeconds (seconds) } }, te::DeleteExistingClips::no);

        for (int k = 0; k < 3; ++k)
        {
            auto l = addPlugin (tracks[(size_t) i]->pluginList, *edit, *fx[(size_t) ((i + k) % (int) fx.size())], k);
            ++plugins;
            loads.add (makeObject ({ { "track", i }, { "plugin", fx[(size_t) ((i + k) % (int) fx.size())]->name }, { "load_ms", l.loadMs }, { "error", l.error } }));
        }
    }

    if (inst != nullptr)
    {
        auto clip = te::insertMIDIClip (*tracks[8], "notes", { 0_tp, 60_tp });

        for (int b = 0; b < 40; ++b)
            addMidiNotes (*clip, { { 36 + (b % 3) * 12, b * 1.0 } }, 0.5);

        auto l = addPlugin (tracks[8]->pluginList, *edit, *inst, 0);
        ++plugins;
        loads.add (makeObject ({ { "track", 8 }, { "plugin", inst->name }, { "load_ms", l.loadMs }, { "error", l.error } }));
    }

    if (master != nullptr)
    {
        addPlugin (edit->getMasterPluginList(), *edit, *master, 0);
        ++plugins;
    }

    const auto buildMs = nowMs() - t0;
    const auto rssBefore = peakRssKb();
    const auto out = o.workDir.getChildFile ("load_out.wav");
    const auto r0 = nowMs();
    const auto err = renderBlocking (*edit, makeRenderParams (*edit, out, sr, seconds));
    const auto renderMs = nowMs() - r0;

    return makeObject ({ { "tracks", 9 }, { "plugin_instances", plugins }, { "build_and_load_ms", buildMs },
                         { "render_s", seconds }, { "render_ms", renderMs }, { "realtime_factor", seconds * 1000.0 / renderMs },
                         { "error", err }, { "peak_rss_mb_before_render", rssBefore / 1024.0 }, { "peak_rss_mb", peakRssKb() / 1024.0 },
                         { "loads", loads } });
}
}

int runPlugins (int argc, char** argv)
{
    Options o;

    if (argc < 2)
    {
        std::fprintf (stderr, "usage: rovox_spikes plugins <work-dir> [--lv2-path P] [--vst3-path P] [--list] [--in-process-scan] [--effect S]... [--instrument S]... [--latent S]\n");
        return 2;
    }

    o.workDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    o.workDir.createDirectory();
    o.lv2Path = "/usr/lib/lv2";
    o.vst3Path = "/usr/lib/vst3:/opt/vst3";

    for (int i = 2; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const juce::String next = i + 1 < argc ? juce::String (argv[i + 1]) : juce::String();

        if (a == "--lv2-path")             { o.lv2Path = next; ++i; }
        else if (a == "--vst3-path")       { o.vst3Path = next; ++i; }
        else if (a == "--list")            o.list = true;
        else if (a == "--in-process-scan") o.inProcessScan = true;
        else if (a == "--effect")          { o.effects.add (next); ++i; }
        else if (a == "--instrument")      { o.instruments.add (next); ++i; }
        else if (a == "--latent")          { o.latent = next; ++i; }
    }

    auto behaviour = std::make_unique<SpikeBehaviour>();
    behaviour->scanOutOfProcess = ! o.inProcessScan;
    behaviour->useCgroup = false;
    te::Engine engine ("rovox_p2_plugins", nullptr, std::move (behaviour));

    auto& known = engine.getPluginManager().knownPluginList;
    known.clear();
    std::vector<std::pair<juce::String, double>> perPlugin;

    auto report = std::make_unique<juce::DynamicObject>();
    const auto scan = scanReport (engine, o, known, perPlugin);
    report->setProperty ("scan", scan);
    std::printf ("scan: %s\n", juce::JSON::toString (scan, true).toRawUTF8());

    juce::StringArray listing;

    for (const auto& d : known.getTypes())
        listing.add (d.name + "  |  " + d.manufacturerName + "  |  in:" + juce::String (d.numInputChannels) + " out:" + juce::String (d.numOutputChannels)
                     + (d.isInstrument ? "  INSTRUMENT" : "") + "  |  " + d.fileOrIdentifier);

    listing.sort (true);

    if (o.list)
    {
        for (auto& l : listing)
            std::printf ("%s\n", l.toRawUTF8());

        return 0;
    }

    juce::Array<juce::var> effectResults, instrumentResults, pdcResults;
    std::vector<const juce::PluginDescription*> effectDescs;
    const auto signal = makeTestSignal (5.0);

    for (auto& name : o.effects)
        if (auto* d = findDesc (known, name))
        {
            effectDescs.push_back (d);
            auto r = testEffect (engine, o, *d, signal);
            effectResults.add (r);
            std::printf ("effect: %s\n", juce::JSON::toString (r, true).toRawUTF8());
        }
        else
            std::printf ("effect '%s' not found\n", name.toRawUTF8());

    const juce::PluginDescription* firstInstrument = nullptr;

    for (auto& name : o.instruments)
        if (auto* d = findDesc (known, name))
        {
            firstInstrument = firstInstrument != nullptr ? firstInstrument : d;
            auto r = testInstrument (engine, o, *d);
            instrumentResults.add (r);
            std::printf ("instrument: %s\n", juce::JSON::toString (r, true).toRawUTF8());
        }
        else
            std::printf ("instrument '%s' not found\n", name.toRawUTF8());

    const juce::PluginDescription* latent = findDesc (known, o.latent);

    if (latent != nullptr)
    {
        for (const bool prime : { false, true })
        {
            auto r = testPdcCase (engine, o, *latent, prime);
            pdcResults.add (r);
            std::printf ("pdc: %s\n", juce::JSON::toString (r, true).toRawUTF8());
        }
    }
    else
        std::printf ("latent plugin '%s' not found\n", o.latent.toRawUTF8());

    juce::var load;

    if (! effectDescs.empty())
    {
        load = testLoad (engine, o, effectDescs, firstInstrument, latent);
        std::printf ("load: %s\n", juce::JSON::toString (load, true).toRawUTF8());
    }

    report->setProperty ("effects", effectResults);
    report->setProperty ("instruments", instrumentResults);
    report->setProperty ("pdc", pdcResults);
    report->setProperty ("load_8x3", load);
    report->setProperty ("peak_rss_mb", peakRssKb() / 1024.0);
    o.workDir.getChildFile ("p2_report.json").replaceWithText (juce::JSON::toString (juce::var (report.release()), false));
    std::printf ("wrote %s\n", o.workDir.getChildFile ("p2_report.json").getFullPathName().toRawUTF8());
    return 0;
}
