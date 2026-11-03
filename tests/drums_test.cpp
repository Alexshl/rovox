// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The built-in drum sampler (src/core/DrumKit, DrumKitLibrary, src/engine/DrumsPlugin): GM note mapping, velocity layers, round robin, hi-hat choke,
// voice stealing, sample-accurate triggering, kit lookup (local directory, object store download into a cache, missing kit) and the plugin in an edit
// (fx.add, render, RPP import of MT-PowerDrumKit). The kit is made here from synthetic tones: no sample audio is stored in the repository.
#include "DrumKit.h"
#include "DrumKitLibrary.h"
#include "DrumsPlugin.h"
#include "RppImport.h"
#include "Render.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <cmath>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

using namespace rovox;

namespace
{
constexpr double kSr = 44100.0;

struct Tone
{
    double hz = 440.0, seconds = 0.1, decay = 0.04;
};

std::vector<float> renderTone (const Tone& t)
{
    std::vector<float> s ((size_t) std::llround (t.seconds * kSr));

    for (size_t i = 0; i < s.size(); ++i)
        s[i] = (float) (0.5 * std::cos (2.0 * M_PI * t.hz * (double) i / kSr) * std::exp (-(double) i / (kSr * t.decay)));

    return s;
}

struct SynthPiece
{
    std::string id;
    std::vector<int> notes;
    int layers, roundRobin;
    double seconds, decay;
    std::string group;
    std::vector<std::string> chokes;
    int maxVoices;
};

/** A complete General MIDI kit of tones; every sample has its own frequency. */
struct SynthKit
{
    json manifest;
    std::map<std::string, Tone> tones;

    SynthKit()
    {
        const std::vector<SynthPiece> defs {
            { "kick", { 35, 36 }, 2, 2, 0.3, 0.04, "", {}, 3 },       { "snare", { 38, 40 }, 2, 2, 0.3, 0.04, "", {}, 3 },
            { "sidestick", { 37 }, 1, 1, 0.15, 0.04, "", {}, 3 },     { "hh_closed", { 42 }, 1, 2, 0.08, 0.03, "", { "hat_open" }, 3 },
            { "hh_pedal", { 44 }, 1, 1, 0.08, 0.03, "", { "hat_open" }, 3 }, { "hh_open", { 46 }, 1, 1, 1.0, 0.5, "hat_open", {}, 2 },
            { "tom_low", { 41, 43 }, 1, 1, 0.3, 0.05, "", {}, 3 },    { "tom_mid", { 45, 47 }, 1, 1, 0.3, 0.05, "", {}, 3 },
            { "tom_high", { 48, 50 }, 1, 1, 0.3, 0.05, "", {}, 3 },   { "crash", { 49, 57 }, 1, 1, 1.5, 0.5, "", {}, 2 },
            { "ride", { 51, 59 }, 1, 1, 1.5, 0.5, "", {}, 2 },        { "ride_bell", { 53 }, 1, 1, 1.0, 0.3, "", {}, 2 },
            { "china", { 52 }, 1, 1, 1.0, 0.3, "", {}, 2 },           { "splash", { 55 }, 1, 1, 0.8, 0.2, "", {}, 2 },
            { "cowbell", { 56 }, 1, 1, 0.2, 0.05, "", {}, 2 },        { "tambourine", { 54 }, 1, 1, 0.2, 0.05, "", {}, 2 } };

        json pieces = json::array();
        int counter = 0;

        for (auto& d : defs)
        {
            json layers = json::array();

            for (int l = 0; l < d.layers; ++l)
            {
                json samples = json::array();

                for (int r = 0; r < d.roundRobin; ++r)
                {
                    const auto path = d.id + "/l" + std::to_string (l) + "_" + std::to_string (r) + ".flac";
                    tones[path] = { 150.0 + 37.0 * (double) counter++, d.seconds, d.decay };
                    samples.push_back (path);
                }

                const int lo = d.layers == 1 ? 1 : (l == 0 ? 1 : 64), hi = d.layers == 1 ? 127 : (l == 0 ? 63 : 127);
                layers.push_back ({ { "vel_min", lo }, { "vel_max", hi }, { "samples", samples } });
            }

            pieces.push_back ({ { "id", d.id }, { "name", d.id }, { "notes", d.notes }, { "max_voices", d.maxVoices }, { "choke_group", d.group },
                                { "chokes", d.chokes }, { "layers", layers } });
        }

        manifest = { { "format", "rovox-drumkit" }, { "version", 1 }, { "id", "drskit" }, { "name", "Synthetic test kit" }, { "license", "none" },
                     { "sample_rate", 44100 }, { "pieces", pieces } };
    }

    DrumSampleMap samples() const
    {
        DrumSampleMap map;

        for (auto& [path, t] : tones)
        {
            const auto s = renderTone (t);
            map[path] = DrumSampleData::fromFloat (s.data(), s.data(), s.size());
        }

        return map;
    }

    /** Writes kit.json and the FLAC files below `dir` (the kit directory itself); returns the kit.json text. */
    std::string write (const fs::path& dir) const
    {
        fs::create_directories (dir);
        json m = manifest;
        json files = json::object();

        for (auto& [path, t] : tones)
        {
            const auto s = renderTone (t);
            const auto file = dir / path;
            fs::create_directories (file.parent_path());
            auto f = toFile (file);
            f.deleteFile();
            juce::FlacAudioFormat flac;
            std::unique_ptr<juce::OutputStream> stream (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer (flac.createWriterFor (stream.get(), kSr, 2u, 16, {}, 0));
            stream.release();
            juce::AudioBuffer<float> buffer (2, (int) s.size());
            buffer.copyFrom (0, 0, s.data(), (int) s.size());
            buffer.copyFrom (1, 0, s.data(), (int) s.size());
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
            writer.reset();
            files[path] = fs::file_size (file);
        }

        m["files"] = files;
        const auto text = m.dump (1);
        std::ofstream (dir / "kit.json", std::ios::binary) << text;
        return text;
    }
};

DrumKit makeKit (const SynthKit& k)
{
    auto m = DrumKitManifest::fromJson (k.manifest);
    REQUIRE (m.ok());
    auto kit = DrumKit::create (m.value());
    REQUIRE (kit.ok());
    return kit.value();
}

std::unique_ptr<DrumSampler> makeSampler (const SynthKit& k)
{
    auto s = std::make_unique<DrumSampler> (kSr);
    auto r = s->setKit (makeKit (k), k.samples());
    REQUIRE (r.ok());
    return s;
}

double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double sum = 0.0;
    to = std::min (to, v.size());

    for (size_t i = from; i < to; ++i)
        sum += (double) v[i] * v[i];

    return to > from ? std::sqrt (sum / (double) (to - from)) : 0.0;
}

/** Points the library at `root` (kit directories below it) for the life of the object. */
struct SearchPathScope
{
    explicit SearchPathScope (std::vector<fs::path> paths) { DrumKitLibrary::instance().setSearchPaths (std::move (paths)); }
    ~SearchPathScope() { DrumKitLibrary::instance().setSearchPaths (std::nullopt); }
};

class MemoryStore : public ObjectStore
{
public:
    Result<ObjectMeta> head (const std::string& key) override
    {
        auto g = get (key);
        return g.ok() ? Result<ObjectMeta> (ObjectMeta { key, "e", g.value().size() }) : Result<ObjectMeta> (g.error());
    }

    Result<std::string> get (const std::string& key) override
    {
        std::lock_guard<std::mutex> lock (mutex);

        if (down)
            return makeError (errc::busy, "store is down");

        ++gets[key];
        auto it = objects.find (key);

        if (it == objects.end())
            return makeError (errc::notFound, "no " + key);

        return it->second;
    }

    Result<void> getFile (const std::string& key, const fs::path& dest) override
    {
        auto body = get (key);

        if (! body.ok())
            return body.error();

        std::error_code ec;
        fs::create_directories (dest.parent_path(), ec);
        std::ofstream (dest, std::ios::binary) << body.value();
        return {};
    }

    Result<void> put (const std::string& key, const std::string& body, const std::string&) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        objects[key] = body;
        return {};
    }

    Result<void> putFile (const std::string& key, const fs::path& file, const std::string& type) override
    {
        std::ifstream in (file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return put (key, ss.str(), type);
    }

    Result<ListResult> list (const std::string& prefix, const std::string& delimiter) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        ListResult out;
        std::set<std::string> folded;

        for (auto& [k, v] : objects)
        {
            if (k.rfind (prefix, 0) != 0)
                continue;

            const auto rest = k.substr (prefix.size());

            if (! delimiter.empty() && rest.find (delimiter) != std::string::npos)
                folded.insert (prefix + rest.substr (0, rest.find (delimiter) + 1));
            else
                out.objects.push_back ({ k, "e", v.size() });
        }

        out.prefixes.assign (folded.begin(), folded.end());
        return out;
    }

    Result<void> remove (const std::string& key) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        objects.erase (key);
        return {};
    }

    std::string describe() const override { return "memory"; }

    std::mutex mutex;
    std::map<std::string, std::string> objects;
    std::map<std::string, int> gets;
    bool down = false;
};

json notesJson (std::initializer_list<std::tuple<int, double, int>> list)
{
    json out = json::array();

    for (auto& [pitch, qn, vel] : list)
        out.push_back ({ { "pitch", pitch }, { "start_qn", qn }, { "length_qn", 0.25 }, { "velocity", vel } });

    return out;
}

const char* const mtRpp = R"RPP(<REAPER_PROJECT 0.1 "7.0/macOS-arm64" 1700000000
  TEMPO 120 4 4 0
  <TRACK {BBBBBBBB-0000-0000-0000-000000000001}
    NAME "drums MIDI"
    <FXCHAIN
      <AU "AUi: MT-PowerDrumKit (MANDA AUDIO)" "MANDA AUDIO: MT-PowerDrumKit" "" 1635085685 1297371204 1297370445
        AAAAAAAA
      >
      FXID {CCCCCCCC-0000-0000-0000-000000000001}
    >
    <ITEM
      POSITION 0
      LENGTH 2
      <SOURCE MIDI
        HASDATA 1 960 QN
        E 0 90 24 6e
        E 240 80 24 00
        E 720 90 26 64
        E 240 80 26 00
      >
    >
  >
>
)RPP";
} // namespace

//==============================================================================
ROVOX_TEST (drums, gm_notes_map_to_the_pieces_and_unmapped_notes_fall_back)
{
    SynthKit synth;
    auto kit = makeKit (synth);
    const std::vector<std::pair<int, const char*>> expected {
        { 35, "kick" }, { 36, "kick" }, { 38, "snare" }, { 40, "snare" }, { 37, "sidestick" }, { 42, "hh_closed" }, { 44, "hh_pedal" }, { 46, "hh_open" },
        { 41, "tom_low" }, { 43, "tom_low" }, { 45, "tom_mid" }, { 47, "tom_mid" }, { 48, "tom_high" }, { 50, "tom_high" }, { 49, "crash" }, { 57, "crash" },
        { 51, "ride" }, { 59, "ride" }, { 53, "ride_bell" }, { 52, "china" }, { 55, "splash" }, { 56, "cowbell" }, { 54, "tambourine" } };

    for (auto& [note, id] : expected)
    {
        bool fallback = true;
        const int piece = kit.pieceForNote (note, &fallback);
        REQUIRE (piece >= 0);
        CHECK (kit.manifest().pieces[(size_t) piece].id == id);
        CHECK (! fallback);
    }

    // the nearest mapped note decides (the lower one on a tie)
    struct Case { int note; const char* id; };

    for (auto c : { Case { 39, "snare" }, Case { 0, "kick" }, Case { 34, "kick" }, Case { 58, "crash" }, Case { 127, "ride" }, Case { 81, "ride" }, Case { 60, "ride" } })
    {
        bool fallback = false;
        const int piece = kit.pieceForNote (c.note, &fallback);
        REQUIRE (piece >= 0);
        CHECK (kit.manifest().pieces[(size_t) piece].id == c.id);
        CHECK (fallback);
    }

    // a kit can say which piece plays a note it does not map
    auto withFallback = synth.manifest;
    withFallback["fallbacks"] = { { "39", "sidestick" }, { "0", "tom_low" } };
    auto fm = DrumKitManifest::fromJson (withFallback);
    REQUIRE (fm.ok());
    auto fkit = DrumKit::create (fm.value());
    REQUIRE (fkit.ok());
    bool fallbackFlag = false;
    CHECK (fkit.value().manifest().pieces[(size_t) fkit.value().pieceForNote (39, &fallbackFlag)].id == "sidestick");
    CHECK (fallbackFlag);
    CHECK (fkit.value().manifest().pieces[(size_t) fkit.value().pieceForNote (0)].id == "tom_low");
    CHECK (fkit.value().manifest().pieces[(size_t) fkit.value().pieceForNote (38)].id == "snare");     // a mapped note is never redirected
    withFallback["fallbacks"] = { { "39", "no_such_piece" } };
    CHECK (! DrumKitManifest::fromJson (withFallback).ok());

    // the sampler reports the unmapped notes it was asked to play
    auto sampler = makeSampler (synth);
    std::vector<float> l (512, 0.0f), r (512, 0.0f);
    const DrumNoteEvent events[] = { { 0, 39, 100 }, { 10, 39, 100 }, { 20, 38, 100 }, { 30, 0, 100 } };
    sampler->render (l.data(), r.data(), 512, events, 4);
    const auto unmapped = sampler->unmappedNotes();
    CHECK (unmapped.size() == 2);
    CHECK (unmapped["39"]["count"] == 2);
    CHECK (unmapped["39"]["played_as"] == "snare");
    CHECK (unmapped["0"]["played_as"] == "kick");
    CHECK (! unmapped.contains ("38"));
}

ROVOX_TEST (drums, velocity_selects_the_layer_and_round_robin_rotates_per_layer)
{
    SynthKit synth;
    auto kit = makeKit (synth);
    const int kick = kit.pieceById ("kick");
    CHECK (kit.layerFor (kick, 1) == 0);
    CHECK (kit.layerFor (kick, 63) == 0);
    CHECK (kit.layerFor (kick, 64) == 1);
    CHECK (kit.layerFor (kick, 127) == 1);

    // round robin: the samples of a layer in turn, each layer on its own
    std::vector<int> soft, loud;

    for (int i = 0; i < 5; ++i)
    {
        auto h = kit.select (36, 30);
        CHECK (h.layer == 0);
        soft.push_back (h.sample);

        if (i < 3)
        {
            auto hl = kit.select (36, 110);
            CHECK (hl.layer == 1);
            loud.push_back (hl.sample);
        }
    }

    CHECK (soft == std::vector<int> ({ 0, 1, 0, 1, 0 }));
    CHECK (loud == std::vector<int> ({ 0, 1, 0 }));
    kit.resetRoundRobin();
    CHECK (kit.select (36, 30).sample == 0);

    // louder inside a layer, never above 1
    auto quiet = kit.select (36, 64), strong = kit.select (36, 127);
    CHECK (quiet.gain < strong.gain);
    CHECK_NEAR (strong.gain, 1.0, 1e-9);

    // a gap between layers goes to the nearest one
    auto gap = synth.manifest;
    gap["pieces"][0]["layers"] = json::array ({ { { "vel_min", 1 }, { "vel_max", 40 }, { "samples", json::array ({ "kick/l0_0.flac" }) } },
                                                { { "vel_min", 80 }, { "vel_max", 127 }, { "samples", json::array ({ "kick/l1_0.flac" }) } } });
    auto m = DrumKitManifest::fromJson (gap);
    REQUIRE (m.ok());
    auto gapKit = DrumKit::create (m.value());
    REQUIRE (gapKit.ok());
    CHECK (gapKit.value().layerFor (0, 50) == 0);
    CHECK (gapKit.value().layerFor (0, 70) == 1);

    // the sampler plays the selected sample: a loud hit has the tone of the loud layer
    auto sampler = makeSampler (synth);
    std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
    const DrumNoteEvent events[] = { { 0, 36, 100 } };
    sampler->render (l.data(), r.data(), 4096, events, 1);
    CHECK (sampler->lastHit().layer == 1);
    CHECK (rms (l, 0, 2000) > 0.01);
}

ROVOX_TEST (drums, manifests_with_mistakes_are_refused)
{
    SynthKit synth;
    auto twice = synth.manifest;
    twice["pieces"][1]["notes"] = json::array ({ 36 });
    CHECK (! DrumKitManifest::fromJson (twice).ok());

    auto escape = synth.manifest;
    escape["pieces"][0]["layers"][0]["samples"][0] = "../../etc/passwd";
    CHECK (! DrumKitManifest::fromJson (escape).ok());

    auto noLayers = synth.manifest;
    noLayers["pieces"][0]["layers"] = json::array();
    CHECK (! DrumKitManifest::fromJson (noLayers).ok());

    auto badVel = synth.manifest;
    badVel["pieces"][0]["layers"][0]["vel_min"] = 100;
    CHECK (! DrumKitManifest::fromJson (badVel).ok());
    CHECK (! DrumKitManifest::fromJson (json::object()).ok());

    // a sample the kit names but the loader did not provide
    auto samples = synth.samples();
    samples.erase ("kick/l0_0.flac");
    DrumSampler sampler (kSr);
    CHECK (! sampler.setKit (makeKit (synth), samples).ok());

    // and the manifest survives its own JSON
    auto again = DrumKitManifest::fromJson (makeKit (synth).manifest().toJson());
    REQUIRE (again.ok());
    CHECK (again.value().pieces.size() == makeKit (synth).manifest().pieces.size());
}

ROVOX_TEST (drums, hits_start_at_the_sample_offset_of_their_midi_event)
{
    SynthKit synth;
    auto sampler = makeSampler (synth);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    const DrumNoteEvent events[] = { { 100, 36, 100 }, { 3001, 38, 100 }, { 3001, 42, 90 } };
    sampler->render (l.data(), r.data(), 8192, events, 3);

    CHECK (rms (l, 0, 100) < 1e-9);                         // nothing before the first event
    CHECK (std::abs (l[100]) > 0.1f);                       // cos starts at full level: the very first sample is the hit
    CHECK (std::abs (l[99]) < 1e-9f);
    CHECK (std::abs (l[3001]) > 0.2f);                      // two hits on the same sample add up

    // the same events split over two blocks give the same audio
    auto split = makeSampler (synth);
    std::vector<float> l2 (8192, 0.0f), r2 (8192, 0.0f);
    const DrumNoteEvent first[] = { { 100, 36, 100 } };
    const DrumNoteEvent second[] = { { 3001 - 2048, 38, 100 }, { 3001 - 2048, 42, 90 } };
    split->render (l2.data(), r2.data(), 2048, first, 1);
    split->render (l2.data() + 2048, r2.data() + 2048, 8192 - 2048, second, 2);
    int different = 0;

    for (size_t i = 0; i < 8192; ++i)
        different += std::abs (l[i] - l2[i]) > 0.0f ? 1 : 0;

    CHECK (different == 0);
}

ROVOX_TEST (drums, closed_and_pedal_hat_choke_the_open_hat)
{
    SynthKit synth;
    const auto run = [&] (int chokeNote)
    {
        auto sampler = makeSampler (synth);
        std::vector<float> l (44100, 0.0f), r (44100, 0.0f);
        std::vector<DrumNoteEvent> events { { 0, 46, 110 } };

        if (chokeNote > 0)
            events.push_back ({ 4410, chokeNote, 110 });

        sampler->render (l.data(), r.data(), 44100, events.data(), (int) events.size());
        return l;
    };

    const auto free = run (0), closed = run (42), pedal = run (44), other = run (38);
    const double tail = rms (free, 20000, 40000);
    CHECK (tail > 0.02);                                         // the open hat rings by itself (decay 0.5 s)
    CHECK (rms (closed, 20000, 40000) < 0.001);                  // the closed hat cuts it
    CHECK (rms (pedal, 20000, 40000) < 0.001);                   // so does the pedal hat
    CHECK (rms (other, 20000, 40000) > 0.5 * tail);              // a snare does not
    CHECK (rms (closed, 0, 4000) > 0.05);                        // up to the choke it sounded
}

ROVOX_TEST (drums, the_oldest_voice_of_a_piece_is_stolen_at_the_voice_limit)
{
    SynthKit synth;
    auto sampler = makeSampler (synth);
    const int kick = sampler->kit().pieceById ("kick");
    std::vector<float> l (100, 0.0f), r (100, 0.0f);
    const DrumNoteEvent events[] = { { 0, 36, 100 }, { 10, 36, 100 }, { 20, 36, 100 }, { 30, 36, 100 } };      // max_voices of the kick is 3
    sampler->render (l.data(), r.data(), 100, events, 4);

    CHECK (sampler->activeVoices (kick) == 3);
    CHECK (sampler->fadingVoices() == 1);                        // the first hit is on its way out

    // other pieces have their own limit
    const DrumNoteEvent snares[] = { { 0, 38, 100 } };
    sampler->render (l.data(), r.data(), 100, snares, 1);
    CHECK (sampler->activeVoices (kick) == 3);
    CHECK (sampler->activeVoices (sampler->kit().pieceById ("snare")) == 1);

    // after the fade the stolen voice is gone
    std::vector<float> l2 (2000, 0.0f), r2 (2000, 0.0f);
    sampler->render (l2.data(), r2.data(), 2000, nullptr, 0);
    CHECK (sampler->fadingVoices() == 0);
    CHECK (sampler->activeVoices (kick) == 3);

    // a flood of hits never exceeds the limit of the piece
    std::vector<DrumNoteEvent> flood;

    for (int i = 0; i < 40; ++i)
        flood.push_back ({ i * 5, 36, 100 });

    std::vector<float> l3 (400, 0.0f), r3 (400, 0.0f);
    sampler->render (l3.data(), r3.data(), 400, flood.data(), (int) flood.size());
    CHECK (sampler->activeVoices (kick) == 3);
}

//==============================================================================
ROVOX_TEST (drums, library_finds_a_local_kit_downloads_from_the_object_store_and_reports_a_missing_one)
{
    SynthKit synth;
    const auto root = test::makeTempDir ("drumkit-src");
    const auto text = synth.write (root / "drskit");
    const auto sha = sha256Hex (text);

    // 1. a local directory (ROVOX_DRUMKITS_PATH)
    {
        DrumKitLibrary lib;
        lib.setSearchPaths (std::vector<fs::path> { root });
        auto found = lib.resolve ("DRSKit");
        REQUIRE (found.ok());
        CHECK (found.value().source == "ROVOX_DRUMKITS_PATH");
        CHECK (fs::exists (found.value().dir / "kit.json"));
        CHECK (! lib.resolve ("nope").ok());
        CHECK (! lib.resolve ("../drskit").ok());

        // a directory that is the kit itself works too
        DrumKitLibrary direct;
        direct.setSearchPaths (std::vector<fs::path> { root / "drskit" });
        CHECK (direct.resolve ("drskit").ok());
    }

    // 2. the bucket: <prefix>assets/drumkits/drskit/<sha>/..., LATEST names the sha; downloaded once into the cache
    auto store = std::make_shared<MemoryStore>();
    const std::string base = "pfx/assets/drumkits/drskit/";
    store->objects[base + "LATEST"] = sha + "\n";
    store->objects[base + sha + "/kit.json"] = text;

    for (auto& [path, tone] : synth.tones)
    {
        std::ifstream in (root / "drskit" / path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        store->objects[base + sha + "/" + path] = ss.str();
    }

    const auto cache = test::makeTempDir ("drumkit-cache");
    DrumKitLibrary lib;
    lib.setSearchPaths (std::vector<fs::path> {});
    lib.setCacheDir (cache);
    lib.setObjectStore (store, "pfx");
    std::vector<std::string> progress;
    auto first = lib.resolve ("drskit", [&] (const std::string& m) { progress.push_back (m); });
    REQUIRE (first.ok());
    CHECK (first.value().source == "object store");
    CHECK (first.value().sha == sha);
    CHECK (first.value().dir == cache / "drskit" / sha);
    CHECK (fs::exists (first.value().dir / "kick" / "l0_0.flac"));
    CHECK (! progress.empty());
    const int downloads = store->gets[base + sha + "/kick/l0_0.flac"];
    CHECK (downloads == 1);

    // the second time nothing is downloaded again
    auto second = lib.resolve ("drskit");
    REQUIRE (second.ok());
    CHECK (second.value().source == "cache");
    CHECK (store->gets[base + sha + "/kick/l0_0.flac"] == downloads);

    // the store is unreachable: the cached kit still works, also from a fresh library
    store->down = true;
    DrumKitLibrary offline;
    offline.setSearchPaths (std::vector<fs::path> {});
    offline.setCacheDir (cache);
    offline.setObjectStore (store, "pfx");
    auto cached = offline.resolve ("drskit");
    REQUIRE (cached.ok());
    CHECK (cached.value().source == "cache");
    store->down = false;

    // a kit.json that does not match its sha is refused and leaves nothing in the cache
    auto bad = std::make_shared<MemoryStore>();
    bad->objects = store->objects;
    bad->objects[base + sha + "/kit.json"] = text + " ";
    const auto cache2 = test::makeTempDir ("drumkit-cache2");
    DrumKitLibrary tampered;
    tampered.setSearchPaths (std::vector<fs::path> {});
    tampered.setCacheDir (cache2);
    tampered.setObjectStore (bad, "pfx");
    CHECK (! tampered.resolve ("drskit").ok());
    CHECK (! fs::exists (cache2 / "drskit" / sha));

    // 3. nothing anywhere: a clear error that says what to do
    DrumKitLibrary none;
    none.setSearchPaths (std::vector<fs::path> {});
    none.setCacheDir (test::makeTempDir ("drumkit-cache3"));
    auto missing = none.resolve ("drskit");
    REQUIRE (! missing.ok());
    CHECK (missing.error().code == errc::notFound);
    CHECK (missing.error().message.find ("ROVOX_DRUMKITS_PATH") != std::string::npos);
    CHECK (missing.error().message.find ("build_drskit.py") != std::string::npos);
}

//==============================================================================
ROVOX_TEST (drums, the_plugin_renders_a_midi_pattern_at_the_right_times_and_reports_unmapped_notes)
{
    SynthKit synth;
    const auto root = test::makeTempDir ("drumkit-plugin");
    synth.write (root / "drskit");
    SearchPathScope scope ({ root });

    test::Env env;
    const auto id = env.newProject ("drums");
    const auto track = env.ok (id, "track.create", { { "name", "Drums" } })["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "builtin", "rovox:drums" } });
    REQUIRE (added.ok);
    CHECK (added.data["plugin"]["is_instrument"] == true);
    CHECK (added.data["plugin"]["type"] == drumsXmlType);
    const auto pluginId = added.data["plugin_id"].get<std::string>();

    // the same plugin through the catalog
    auto catalog = env.ok ("", "plugin.catalog", { { "query", "drums" } });
    bool listed = false;

    for (auto& p : catalog["plugins"])
        listed = listed || (p["plugin_uid"] == "rovox:drums" && p["kind"] == "instrument");

    CHECK (listed);

    // 120 bpm: a quarter note is 0.5 s
    REQUIRE (env.run (id, "item.create_midi", { { "track_id", track },
                                                { "notes", notesJson ({ { 36, 0.0, 110 }, { 38, 1.0, 100 }, { 42, 2.0, 90 }, { 39, 3.0, 100 }, { 46, 4.0, 100 }, { 42, 5.0, 100 } }) } }).ok);

    RenderRequest req;
    req.name = "drums";
    req.tailS = 1.0;
    req.scope = "master";
    auto rendered = renderProject (env.project (id), req);

    if (! rendered.ok())
        std::cerr << "    " << rendered.error().toJson().dump() << "\n";

    REQUIRE (rendered.ok());
    std::vector<std::vector<float>> ch;
    double sr = 0.0;
    REQUIRE (test::readWav (env.project (id).rendersDir() / "drums.wav", ch, sr));
    REQUIRE (ch.size() == 2);
    REQUIRE (std::abs (sr - kSr) < 1.0);

    const double times[] = { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5 };

    for (double t : times)
    {
        const auto expected = (size_t) std::llround (t * sr);
        size_t onset = 0;
        const size_t from = expected > 200 ? expected - 200 : 0;

        for (size_t i = from; i < ch[0].size(); ++i)
            if (std::abs (ch[0][i]) > 0.03f)
            {
                onset = i;
                break;
            }

        // the hat at 2.0 s follows nothing audible; the tones decay within 0.2 s so the onsets are clean (the open hat rings, but it is the last but one)
        if (t < 2.4)
            CHECK (onset >= expected && onset <= expected + 1);

        if (t < 0.6)
            CHECK (rms (ch[0], from, expected) < 0.001);
    }

    // the choke: the open hat (2.0 s) would still be at 0.1 more than 0.15 s after the closed hat at 2.5 s; the choke removes it
    REQUIRE (ch[0].size() > (size_t) (sr * 2.9));
    CHECK (rms (ch[0], (size_t) (sr * 2.65), (size_t) (sr * 2.85)) < 0.002);
    CHECK (rms (ch[0], (size_t) (sr * 2.0 + 2000), (size_t) (sr * 2.0 + 6000)) > 0.02);

    // stereo output, no NaN
    for (size_t i = 0; i < ch[0].size(); ++i)
        REQUIRE (std::isfinite (ch[0][i]) && std::isfinite (ch[1][i]));

    // the plugin state: kit and attribution (the render ran on its own copy of the plugin; the unmapped-note report is checked on the sampler above)
    auto* plugin = env.project (id).findPlugin (pluginId);
    REQUIRE (plugin != nullptr);
    auto state = drumsPluginExtras (*plugin, 5.0);
    if (state["status"] != "ready")
        std::cerr << "    plugin state: " << state.dump() << "\n";

    CHECK (state["status"] == "ready");
    CHECK (state["kit"] == "drskit");
    CHECK (state["name"] == "Synthetic test kit");
    CHECK (state["pieces"] == 16);
    CHECK (state["unmapped_notes"].is_object());
    CHECK (std::string (state["attribution"].get<std::string>()).find ("CC BY 4.0") != std::string::npos);
    const auto listedFx = env.ok (id, "fx.list", { { "target", track } });
    bool shown = false;

    for (auto& fx : listedFx["fx"])
        shown = shown || (fx.contains ("drums") && fx["drums"]["status"] == "ready");

    CHECK (shown);
}

ROVOX_TEST (drums, without_a_kit_the_plugin_says_so_and_the_render_does_not_crash)
{
    SearchPathScope scope ({});
    test::Env env;
    const auto id = env.newProject ("nokit");
    const auto track = env.ok (id, "track.create", { { "name", "Drums" } })["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "rovox:drums" } } } });
    REQUIRE (added.ok);
    auto* plugin = env.project (id).findPlugin (added.data["plugin_id"].get<std::string>());
    REQUIRE (plugin != nullptr);
    auto state = drumsPluginExtras (*plugin, 10.0);
    CHECK (state["status"] == "error");
    CHECK (state["error"].get<std::string>().find ("not available") != std::string::npos);
    CHECK (state["error"].get<std::string>().find ("ROVOX_DRUMKITS_PATH") != std::string::npos);
    const auto fx = env.ok (id, "fx.list", { { "target", track } });
    bool reported = false;

    for (auto& f : fx["fx"])
        reported = reported || (f.contains ("load_error") && f["loaded"] == false);

    CHECK (reported);

    REQUIRE (env.run (id, "item.create_midi", { { "track_id", track }, { "notes", notesJson ({ { 36, 0.0, 100 } }) } }).ok);
    RenderRequest req;
    req.name = "nokit";
    auto r = renderProject (env.project (id), req);
    CHECK (! r.ok());                       // the preflight refuses: plugin_not_loaded / silent_track
    req.failOn.clear();
    req.warnOn.clear();
    auto forced = renderProject (env.project (id), req);

    if (forced.ok())
    {
        std::vector<std::vector<float>> ch;
        double sr = 0.0;

        if (test::readWav (env.project (id).rendersDir() / "nokit.wav", ch, sr))
            CHECK (rms (ch[0], 0, ch[0].size()) < 1e-9);
    }
}

ROVOX_TEST (drums, rpp_import_maps_mt_powerdrumkit_to_the_drum_sampler)
{
    SynthKit synth;
    const auto root = test::makeTempDir ("drumkit-rpp");
    synth.write (root / "drskit");
    SearchPathScope scope ({ root });
    const auto dir = test::makeTempDir ("rpp-drums");
    std::ofstream (dir / "song.RPP") << mtRpp;

    test::Env env;
    const auto id = env.newProject ("mt");
    auto r = env.run (id, "import.rpp", { { "path", (dir / "song.RPP").string() } });
    REQUIRE (r.ok);
    REQUIRE (r.data["mapped"].size() == 1);
    CHECK (r.data["mapped"][0]["mapped_to"] == "Rovox Drums");
    CHECK (r.data["mapped"][0]["name"] == "AUi: MT-PowerDrumKit (MANDA AUDIO)");
    CHECK (r.data["unsupported"].size() == 0);
    CHECK (r.data["imported"]["midi_items"] == 1);

    const auto track = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
    bool has = false;
    const auto chain = env.ok (id, "fx.list", { { "target", track } });

    for (auto& fx : chain["fx"])
        has = has || fx["type"] == drumsXmlType;

    if (! has)
        std::cerr << "    fx.list: " << chain.dump() << "\n";

    CHECK (has);

    RenderRequest req;
    req.name = "mt";
    req.tailS = 0.5;
    auto rendered = renderProject (env.project (id), req);

    if (! rendered.ok())
        std::cerr << "    " << rendered.error().toJson().dump() << "\n";

    REQUIRE (rendered.ok());
    std::vector<std::vector<float>> ch;
    double sr = 0.0;
    REQUIRE (test::readWav (env.project (id).rendersDir() / "mt.wav", ch, sr));
    CHECK (rms (ch[0], 0, (size_t) (sr * 0.2)) > 0.02);             // the kick at 0 (note 0x24 = 36)
    CHECK (rms (ch[0], (size_t) (sr * 0.5), (size_t) (sr * 0.6)) > 0.02);      // the snare at 0.5 s
}
