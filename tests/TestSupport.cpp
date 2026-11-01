// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "TestSupport.h"
#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <unistd.h>

namespace rovox::test
{

namespace
{
fs::path testRoot()
{
    // never destroyed: the atexit cleanup below must still be able to use it
    static const std::string* root = []
    {
        auto base = fs::temp_directory_path() / ("rovox-tests-" + std::to_string ((long) ::getpid()));
        fs::remove_all (base);
        fs::create_directories (base);
        auto* s = new std::string (base.string());
        std::atexit ([] { std::error_code ec; fs::remove_all (testRoot(), ec); });
        return s;
    }();

    return *root;
}
} // namespace

EngineHost& host()
{
    static EngineHost h ([]
    {
        EngineConfig c;
        c.dataDir = testRoot() / "data";

        // the transport suite plays through the browser-stream device (a timer thread, no sound card)
        if (const char* audio = std::getenv ("ROVOX_TEST_AUDIO"))
            if (auto mode = parseAudioMode (audio))
                c.audio = *mode;

        return c;
    }());

    return h;
}

fs::path makeTempDir (const std::string& tag)
{
    static int counter = 0;
    auto d = testRoot() / (tag + "-" + std::to_string (++counter));
    fs::create_directories (d);
    return d;
}

void writeWav (const fs::path& file, const std::vector<std::vector<float>>& channels, double sampleRate, int bits)
{
    auto f = toFile (file);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (f.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, (unsigned) channels.size(), bits, {}, 0));
    stream.release();

    juce::AudioBuffer<float> buffer ((int) channels.size(), (int) channels[0].size());

    for (size_t c = 0; c < channels.size(); ++c)
        buffer.copyFrom ((int) c, 0, channels[c].data(), (int) channels[c].size());

    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

std::vector<float> sine (double hz, double dbfs, double seconds, double sampleRate)
{
    std::vector<float> s ((size_t) std::llround (seconds * sampleRate));
    const double amp = std::pow (10.0, dbfs / 20.0);

    for (size_t i = 0; i < s.size(); ++i)
        s[i] = (float) (amp * std::sin (2.0 * M_PI * hz * (double) i / sampleRate));

    return s;
}

bool readWav (const fs::path& file, std::vector<std::vector<float>>& channels, double& sampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (toFile (file)));

    if (r == nullptr)
        return false;

    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    sampleRate = r->sampleRate;
    channels.assign (r->numChannels, {});

    for (unsigned c = 0; c < r->numChannels; ++c)
        channels[c].assign (b.getReadPointer ((int) c), b.getReadPointer ((int) c) + b.getNumSamples());

    return true;
}

Env::Env() : pm (host(), hub), bus (pm, registry, executor)
{
    registerBuiltinCommands (registry);
}

CommandResult Env::runAs (const Origin& origin, const std::string& project, const std::string& name, const json& params)
{
    CommandContext ctx;
    ctx.projectId = project;
    ctx.origin = origin;
    return bus.runNow (name, params, ctx);
}

CommandResult Env::run (const std::string& project, const std::string& name, const json& params, std::optional<std::uint64_t> ifRevision)
{
    CommandContext ctx;
    ctx.projectId = project;
    ctx.origin = { Origin::Kind::mcp, "mcp_1", "Claude" };
    ctx.ifRevision = ifRevision;
    return bus.runNow (name, params, ctx);
}

json Env::ok (const std::string& project, const std::string& name, const json& params)
{
    auto r = run (project, name, params);

    if (! r.ok)
        std::cerr << "    command " << name << " failed: " << (r.error ? r.error->toJson().dump() : std::string ("?")) << "\n";

    if (! r.ok)
        throw RequireFailed();

    return r.data;
}

std::string Env::newProject (const std::string& name, bool ephemeral, std::optional<int> ttl)
{
    CreateOptions o;
    o.name = name;
    o.ephemeral = ephemeral;
    o.ttlS = ttl;
    auto id = pm.create (o);

    if (! id.ok())
    {
        std::cerr << "    create failed: " << id.error().message << "\n";
        throw RequireFailed();
    }

    return id.value();
}

Project& Env::project (const std::string& id)
{
    return *pm.find (id);
}

fs::path makeR20Folder (const fs::path& parent, const std::string& name, int numTracks, double seconds)
{
    const auto dir = parent / name;
    fs::create_directories (dir / "DATA");
    fs::create_directories (dir / "peaks");
    std::string seq = "SEQD";

    for (int i = 1; i <= numTracks; ++i)
    {
        char fname[32];
        std::snprintf (fname, sizeof (fname), "REC%02d_01.WAV", i);
        const double secs = seconds + i * 0.0137;       // a different, non-round length per file
        writeWav (dir / fname, { sine (220.0 * i, -18.0, secs, 44100.0) }, 44100.0, 24);
        seq += std::string (1, '\0') + fname;
    }

    seq += std::string (1, '\0') + "SEQE";
    std::ofstream (dir / "R20PRJDATA.r20prj", std::ios::binary) << "ZOOM R20    PROJECT DATA VER0001" << seq;
    std::ofstream (dir / "DATA" / "REC01_01.ZWVH", std::ios::binary) << "junk";
    std::ofstream (dir / "peaks" / "REC01_01.WAV.reapeaks", std::ios::binary) << "junk";
    return dir;
}

json snapshot (Env& env, const std::string& projectId)
{
    auto& p = env.project (projectId);
    auto tracks = env.ok (projectId, "track.list");
    auto markers = env.ok (projectId, "marker.list");
    json plugins = json::array();

    for (auto pl : te::getAllPlugins (p.edit(), true))
        plugins.push_back ({ { "id", Project::idOf (pl->state) }, { "type", toStd (pl->getPluginType()) } });

    std::sort (plugins.begin(), plugins.end(), [] (const json& a, const json& b) { return a["id"] < b["id"]; });
    tracks.erase ("revision");
    return { { "tracks", tracks["tracks"] }, { "markers", markers["markers"] }, { "plugins", plugins }, { "name", p.name() } };
}

void pumpMessages (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

} // namespace rovox::test
