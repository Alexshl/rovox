// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "PluginHost.h"

#include "Ids.h"
#include "JsfxPlugin.h"
#include "PluginCatalog.h"
#include "ProjectFormat.h"

#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>

#include <unistd.h>

namespace rovox
{

fs::path hostExecutable()
{
    for (auto* name : { "ROVOX_HOST_BIN", "ROVOX_CLI_BIN" })
        if (const char* env = std::getenv (name); env != nullptr && *env != 0)
            return env;

    std::error_code ec;
    const auto self = fs::read_symlink ("/proc/self/exe", ec);

    if (! ec && self.filename() == "rovox")
        return self;

    return {};
}

//==============================================================================
namespace
{
std::mutex stdoutMutex;

void printLine (const std::string& line)
{
    std::lock_guard<std::mutex> lock (stdoutMutex);
    std::cout << line << std::endl;
}

std::string describeCrash (const SubprocessResult& r)
{
    if (r.timedOut)     return "hung";
    if (r.signal != 0)  return "crashed (" + r.describe() + ")";
    return r.describe();
}

std::atomic<int> tempCounter { 0 };
} // namespace

ProbeOutcome probePluginInChild (const juce::PluginDescription& desc, const fs::path& dataDir, bool presetsOnly, double timeoutS)
{
    ProbeOutcome out;
    const auto exe = hostExecutable();

    if (exe.empty())
    {
        out.error = "no plugin host executable is known (set ROVOX_HOST_BIN)";
        return out;
    }

    std::error_code ec;
    fs::create_directories (dataDir / "cache", ec);
    const auto file = dataDir / "cache" / ("probe-" + std::to_string ((long) ::getpid()) + "-" + std::to_string (++tempCounter) + ".xml");

    if (auto xml = desc.createXml())
        toFile (file).replaceWithText (xml->toString());

    SubprocessOptions o;
    o.argv = { exe.string(), "--data-dir", dataDir.string(), "plugin-probe", file.string() };

    if (presetsOnly)
        o.argv.push_back ("--presets-only");

    o.timeoutS = timeoutS;
    std::string last;
    o.onStdoutLine = [&last] (const std::string& line) { if (! line.empty() && line[0] == '{') last = line; };
    const auto r = runSubprocess (o);
    fs::remove (file, ec);

    if (! r.started)
    {
        out.error = "the plugin host process could not be started: " + r.stderrTail;
        return out;
    }

    if (r.timedOut || r.crashed())
    {
        out.crashed = r.crashed();
        out.timedOut = r.timedOut;
        out.error = describeCrash (r);
        return out;
    }

    auto doc = json::parse (last, nullptr, false);

    if (doc.is_discarded())
    {
        out.error = "the plugin host gave no answer (" + r.describe() + ")";
        return out;
    }

    out.ok = doc.value ("ok", false);
    out.error = doc.value ("error", "");
    out.info = doc;
    return out;
}

int runPluginProbeChild (const std::string& descXmlFile, bool presetsOnly)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto xml = juce::parseXML (juce::File (juce::CharPointer_UTF8 (descXmlFile.c_str())));
    juce::PluginDescription desc;

    if (xml == nullptr || ! desc.loadFromXml (*xml))
    {
        printLine (json { { "ok", false }, { "error", "unreadable plugin description" } }.dump());
        return 3;
    }

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    juce::String error;
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 512;
    std::unique_ptr<juce::AudioPluginInstance> inst = formats.createPluginInstance (desc, sampleRate, blockSize, error);

    if (inst == nullptr)
    {
        printLine (json { { "ok", false }, { "error", error.isEmpty() ? std::string ("the plugin could not be created") : toStd (error) } }.dump());
        return 3;
    }

    json programs = json::array();

    for (int i = 0; i < inst->getNumPrograms() && i < 2000; ++i)
        programs.push_back (toStd (inst->getProgramName (i)));

    json info { { "ok", true }, { "name", toStd (inst->getName()) }, { "num_params", inst->getParameters().size() }, { "inputs", inst->getTotalNumInputChannels() },
                { "outputs", inst->getTotalNumOutputChannels() }, { "programs", programs }, { "is_instrument", desc.isInstrument }, { "accepts_midi", inst->acceptsMidi() } };

    // The answer is printed first and the process then ends without destroying the instance: some plugins (avldrums, spike P2) crash when an
    // instance is destroyed while they still load samples in a worker thread, and nothing is gained by cleaning up a process that is about to exit.
    auto finish = [] (const json& answer)
    {
        printLine (answer.dump());
        std::cout.flush();
        std::_Exit (0);
    };

    if (presetsOnly)
        finish (info);

    inst->setRateAndBufferSizeDetails (sampleRate, blockSize);
    inst->prepareToPlay (sampleRate, blockSize);
    const int channels = std::max ({ 2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels() });
    juce::AudioBuffer<float> buf (channels, blockSize);

    // LV2 latency is only valid after the first processed block (spike P2): prime, then read it
    for (int i = 0; i < 8; ++i)
    {
        juce::MidiBuffer none;
        buf.clear();
        inst->processBlock (buf, none);
    }

    info["latency_samples"] = inst->getLatencySamples();

    if (desc.isInstrument)
    {
        std::vector<double> peaks ((size_t) inst->getTotalNumOutputChannels(), 0.0);
        double peak = 0;
        // Notes are repeated and the blocks paced like real time until a note is heard: instruments that load samples in a worker thread
        // (avldrums) are silent until that is done, however fast blocks are processed.
        constexpr int phaseLength = 40;
        const int notes[3] = { 36, 38, 60 };
        int sounded = 0;

        for (int block = 0; block < 300; ++block)
        {
            juce::MidiBuffer midi;
            const int phase = block % phaseLength, note = notes[(block / phaseLength) % 3];

            if (phase == 0)     midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
            if (phase == 25)    midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);

            buf.clear();
            inst->processBlock (buf, midi);

            for (int c = 0; c < inst->getTotalNumOutputChannels(); ++c)
            {
                const double p = buf.getMagnitude (c, 0, blockSize);
                peaks[(size_t) c] = std::max (peaks[(size_t) c], p);
                peak = std::max (peak, p);
            }

            if (peak > 3.16e-4)
            {
                if (phase >= 26 && ++sounded >= 1)
                    break;
            }
            else
            {
                juce::Thread::sleep (8);
            }
        }

        info["peak_dbfs"] = peak > 1.0e-9 ? 20.0 * std::log10 (peak) : -200.0;
        info["audible"] = peak > 3.16e-4;
        info["output_peaks"] = peaks;
    }

    finish (info);
    return 0;
}

//==============================================================================
bool editHasExternalPlugins (te::Edit& edit)
{
    // JSFX counts: it runs machine code that ysfx generates from third-party effect files, so it is rendered in the child process as well
    for (auto p : te::getAllPlugins (edit, true))
        if (dynamic_cast<te::ExternalPlugin*> (p) != nullptr || jsfxPluginExtras (*p).is_object())
            return true;

    return false;
}

fs::path writeProjectSnapshot (Project& p, const std::string& tag)
{
    const auto dir = p.host().dataDir() / "cache" / "render-child" / tag;
    std::error_code ec;
    fs::remove_all (dir, ec);
    fs::create_directories (dir, ec);

    p.collector().beginScope();         // flushState rewrites plugin state properties; not a user-visible change
    p.edit().flushState();
    p.collector().endScope();

    auto written = writeProject (dir, p.edit().state.createCopy(), p.manifest(), { .fsync = false });

    if (! written.ok())
        return {};

    fs::create_directories (p.mediaDir(), ec);
    fs::create_directory_symlink (p.mediaDir(), dir / "media", ec);
    return dir;
}

namespace
{
json requestToJson (const RenderRequest& r)
{
    return { { "name", r.name }, { "scope", r.scope }, { "track_ids", r.trackIds }, { "stems_through_master", r.stemsThroughMaster },
             { "range", { { "start_s", r.range.startS ? json (*r.range.startS) : json (nullptr) }, { "end_s", r.range.endS ? json (*r.range.endS) : json (nullptr) },
                          { "start_bar", r.range.startBar ? json (*r.range.startBar) : json (nullptr) }, { "end_bar", r.range.endBar ? json (*r.range.endBar) : json (nullptr) } } },
             { "tail_s", r.tailS },
             { "format", { { "container", r.format.container }, { "bit_depth", r.format.bitDepth }, { "bitrate_kbps", r.format.bitrateKbps }, { "sample_rate", r.format.sampleRate }, { "channels", r.format.channels } } },
             { "loudness", { { "mode", r.loudness.mode }, { "target_lufs", r.loudness.targetLufs }, { "tolerance_lu", r.loudness.toleranceLu }, { "true_peak_ceiling_db", r.loudness.truePeakCeilingDb },
                            { "limiter", r.loudness.limiter ? json (*r.loudness.limiter) : json (nullptr) } } },
             { "fail_on", r.failOn }, { "warn_on", r.warnOn }, { "clip_tolerance", r.clipTolerance }, { "seed", r.seed ? json (*r.seed) : json (nullptr) },
             { "preflight_only", r.preflightOnly }, { "stall_timeout_s", r.stallTimeoutS } };
}

RenderRequest requestFromJson (const json& j)
{
    RenderRequest r;
    r.name = j.value ("name", r.name);
    r.scope = j.value ("scope", r.scope);
    r.trackIds = j.value ("track_ids", std::vector<std::string>());
    r.stemsThroughMaster = j.value ("stems_through_master", false);

    if (j.contains ("range"))
    {
        const auto& g = j["range"];

        if (g["start_s"].is_number())   r.range.startS = g["start_s"].get<double>();
        if (g["end_s"].is_number())     r.range.endS = g["end_s"].get<double>();
        if (g["start_bar"].is_number()) r.range.startBar = g["start_bar"].get<int>();
        if (g["end_bar"].is_number())   r.range.endBar = g["end_bar"].get<int>();
    }

    r.tailS = j.value ("tail_s", r.tailS);
    const auto& f = j["format"];
    r.format = { f.value ("container", "wav"), f.value ("bit_depth", 24), f.value ("bitrate_kbps", 320), f.value ("sample_rate", 0.0), f.value ("channels", 2) };
    const auto& l = j["loudness"];
    r.loudness = { l.value ("mode", "none"), l.value ("target_lufs", -14.0), l.value ("tolerance_lu", 1.0), l.value ("true_peak_ceiling_db", -1.0) };
    if (l.contains ("limiter") && l["limiter"].is_boolean())
        r.loudness.limiter = l["limiter"].get<bool>();
    r.failOn = j.value ("fail_on", r.failOn);
    r.warnOn = j.value ("warn_on", r.warnOn);
    r.clipTolerance = j.value ("clip_tolerance", (std::int64_t) 0);

    if (j.contains ("seed") && j["seed"].is_string())
        r.seed = j["seed"].get<std::string>();

    r.preflightOnly = j.value ("preflight_only", false);
    r.stallTimeoutS = j.value ("stall_timeout_s", r.stallTimeoutS);
    return r;
}

std::vector<std::string> externalPluginNames (const fs::path& snapshotDir)
{
    std::vector<std::string> names;

    if (auto xml = juce::parseXML (toFile (snapshotDir / editFileName)))
    {
        std::function<void (const juce::XmlElement&)> walk = [&] (const juce::XmlElement& e)
        {
            if (e.hasTagName ("PLUGIN") && e.getStringAttribute ("type") != "volume" && e.hasAttribute ("filename"))
                names.push_back (toStd (e.getStringAttribute ("name")));

            for (auto* c : e.getChildIterator())
                walk (*c);
        };

        walk (*xml);
    }

    return names;
}
} // namespace

Result<json> renderInChild (const fs::path& snapshotDir, const fs::path& dataDir, const RenderContext& ctx, const RenderRequest& req,
                            const std::function<void (const json&)>& onCrash)
{
    const auto exe = hostExecutable();

    if (exe.empty())
        return makeError (errc::preconditionFailed, "no plugin host executable is known (set ROVOX_HOST_BIN): the render of a project with plugins runs in a separate process");

    const json doc { { "request", requestToJson (req) },
                     { "context", { { "renders_dir", ctx.rendersDir.string() }, { "project_id", ctx.projectId }, { "source_revision", ctx.sourceRevision }, { "sample_rate", ctx.projectSampleRate } } } };
    const auto requestFile = snapshotDir / "request.json";
    toFile (requestFile).replaceWithText (toJuce (doc.dump()));

    SubprocessOptions o;
    o.argv = { exe.string(), "--data-dir", dataDir.string(), "render-child", snapshotDir.string(), requestFile.string() };
    std::optional<json> result, failure;

    o.onStdoutLine = [&] (const std::string& line)
    {
        if (line.rfind ("PROGRESS ", 0) == 0)
        {
            if (req.hooks.onProgress)
            {
                const auto rest = line.substr (9);
                const auto sp = rest.find (' ');
                req.hooks.onProgress (std::atof (rest.substr (0, sp).c_str()), sp == std::string::npos ? "" : rest.substr (sp + 1));
            }
        }
        else if (line.rfind ("RESULT ", 0) == 0)
        {
            auto j = json::parse (line.substr (7), nullptr, false);

            if (! j.is_discarded())
                result = std::move (j);
        }
        else if (line.rfind ("ERROR ", 0) == 0)
        {
            auto j = json::parse (line.substr (6), nullptr, false);

            if (! j.is_discarded())
                failure = std::move (j);
        }
    };
    o.shouldCancel = req.hooks.shouldCancel;
    const auto r = runSubprocess (o);

    if (result)
        return *result;

    if (failure)
        return Error { failure->value ("code", errc::internal), failure->value ("message", "render failed"), failure->value ("details", json (nullptr)) };

    if (r.cancelled)
        return makeError (errc::cancelled, "cancelled");

    const auto plugins = externalPluginNames (snapshotDir);

    if (r.crashed() || r.timedOut)
    {
        json info { { "signal", r.signal }, { "timed_out", r.timedOut }, { "stderr_tail", r.stderrTail.substr (r.stderrTail.size() > 400 ? r.stderrTail.size() - 400 : 0) }, { "plugins", plugins } };

        if (onCrash)
            onCrash (info);

        return makeError (errc::pluginCrashed, "the render process " + describeCrash (r) + "; a plugin in this project most likely crashed. The server is not affected. Plugins in the project: "
                                                   + (plugins.empty() ? std::string ("none") : [&] { std::string s; for (auto& n : plugins) s += (s.empty() ? "" : ", ") + n; return s; }()),
                          info);
    }

    return makeError (errc::internal, "the render process " + r.describe() + (r.stderrTail.empty() ? "" : ": " + r.stderrTail.substr (r.stderrTail.size() > 300 ? r.stderrTail.size() - 300 : 0)));
}

int runRenderChild (const EngineConfig& cfg, const std::string& snapshotDir, const std::string& requestFile)
{
    std::ifstream in (requestFile);
    const auto doc = json::parse (in, nullptr, false);

    if (doc.is_discarded())
    {
        printLine ("ERROR " + json { { "code", errc::io }, { "message", "unreadable render request" } }.dump());
        return 1;
    }

    EngineHost host (cfg);
    host.catalog().registerWithEngine();
    auto loaded = readProject (snapshotDir);

    if (! loaded.ok())
    {
        printLine ("ERROR " + loaded.error().toJson().dump());
        return 1;
    }

    auto edit = createEditForProject (host.engine(), snapshotDir, loaded.value().editState, te::Edit::EditRole::forRendering);
    prepareEditForRender (*edit);
    auto req = requestFromJson (doc["request"]);
    RenderContext ctx;
    ctx.rendersDir = doc["context"].value ("renders_dir", "");
    ctx.projectId = doc["context"].value ("project_id", "");
    ctx.sourceRevision = doc["context"].value ("source_revision", (std::uint64_t) 0);
    ctx.projectSampleRate = doc["context"].value ("sample_rate", 44100.0);

    std::atomic<double> lastSent { -1.0 };
    req.hooks.onProgress = [&lastSent] (double f, const std::string& stage)
    {
        if (f - lastSent.load() >= 0.01 || f >= 1.0)
        {
            lastSent = f;
            printLine("PROGRESS " + std::to_string (f) + " " + stage);
        }
    };

    auto result = renderEdit (*edit, ctx, req);

    // The answer is printed first and the process ends without tearing the Edit down: the file is already published, and some plugins
    // (avldrums, spike P2) crash when an instance is destroyed while a worker thread still loads samples.
    printLine (result.ok() ? "RESULT " + result.value().dump() : "ERROR " + result.error().toJson().dump());
    std::cout.flush();
    std::_Exit (result.ok() ? 0 : 1);
}

//==============================================================================
Result<te::Plugin::Ptr> createExternalPlugin (Project& p, const json& param, json* probeInfo)
{
    auto& catalog = p.host().catalog();
    std::string key = param.value ("plugin_uid", "");
    std::optional<json> rec;

    if (! key.empty())
        rec = catalog.find (key);

    if (! rec && param.contains ("uid"))
    {
        const auto uid = param["uid"].get<std::string>();
        auto fmt = param.value ("format", "");
        std::transform (fmt.begin(), fmt.end(), fmt.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        rec = catalog.find (fmt + ":" + uid);

        if (! rec)
            rec = catalog.find (uid);
    }

    juce::PluginDescription d;

    if (rec)
    {
        const auto connector = rec->value ("connector", "local");

        for (auto& c : makeConnectors())
            if (c->id() == connector && ! c->available())
                return makeError (errc::preconditionFailed, "plugin " + rec->value ("name", "") + " lives behind the '" + connector + "' connector, which is unavailable: " + c->unavailableReason());

        auto probe = catalog.ensureProbed (rec->value ("plugin_uid", ""));

        if (! probe.ok())
            return probe.error();

        if (probeInfo != nullptr)
            *probeInfo = probe.value();

        auto fromCatalog = PluginCatalog::descriptionOf (*rec);

        if (! fromCatalog)
            return makeError (errc::internal, "damaged catalog entry for " + rec->value ("plugin_uid", ""));

        d = *fromCatalog;
        // Tracktion finds the description of a plugin again (on load, in a render copy) through the engine's own list of known plugins
        p.host().engine().getPluginManager().knownPluginList.addType (d);
    }
    else if (param.contains ("uid"))
    {
        // not in the catalog: created as given; if it cannot be loaded the plugin reports the error and the render preflight stops
        d.pluginFormatName = toJuce (param.value ("format", ""));
        d.fileOrIdentifier = toJuce (param.value ("file", param["uid"].get<std::string>()));
        d.uniqueId = toJuce (param["uid"].get<std::string>()).hashCode();
        d.name = toJuce (param.value ("name", param["uid"].get<std::string>()));
        d.isInstrument = false;
    }
    else
    {
        return makeError (errc::notFound, "no plugin " + key + " in the catalog (plugin.catalog lists what is installed)");
    }

    auto plugin = p.edit().getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, d);

    if (plugin == nullptr)
        return makeError (errc::preconditionFailed, "plugin could not be created");

    return plugin;
}

void resyncPluginStates (te::Edit& edit)
{
    for (auto p : te::getAllPlugins (edit, true))
    {
        auto* ext = dynamic_cast<te::ExternalPlugin*> (p);
        auto* inst = ext != nullptr ? ext->getAudioPluginInstance() : nullptr;

        if (inst == nullptr || ! ext->state.hasProperty (te::IDs::state))
            continue;

        juce::MemoryBlock chunk;
        inst->getStateInformation (chunk);

        if (chunk.toBase64Encoding() != ext->state[te::IDs::state].toString())
            ext->restorePluginStateFromValueTree (ext->state);
    }
}

} // namespace rovox
