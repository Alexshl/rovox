// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Instrument packs and the SoundFont instrument rovox:sf2 (src/core/InstrumentPacks, src/engine/Sf2Plugin; FluidSynth). The tests use the tiny soundfont of
// sine samples in tests/fixtures/instruments (written by tools/make_test_sf2.py, own work), so they do not need a real soundfont. One test looks at the
// General MIDI soundfont that the Docker image ships (MuseScore General Lite); without it the test skips, and with ROVOX_REQUIRE_SF2=1 it fails.
#include "InstrumentPacks.h"
#include "PluginCatalog.h"
#include "Render.h"
#include "Sf2Plugin.h"
#include "Sha256.h"
#include "Spectrum.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <utility>

using namespace rovox;

namespace
{
const char* const sinePack = "rovox-test-sine";
const char* const unknownPack = "rovox-test-unknown";

/** Sets an environment variable for the life of the object. */
struct EnvVar
{
    EnvVar (const char* n, const std::string& value) : name (n)
    {
        if (const char* old = std::getenv (n))
            previous = old;

        ::setenv (n, value.c_str(), 1);
    }

    ~EnvVar()
    {
        if (previous)
            ::setenv (name, previous->c_str(), 1);
        else
            ::unsetenv (name);
    }

    const char* name;
    std::optional<std::string> previous;
};

fs::path fixturesDir()
{
    const char* env = std::getenv ("ROVOX_TEST_INSTRUMENTS_DIR");
    return env != nullptr && *env != 0 ? fs::path (env) : fs::path (ROVOX_SOURCE_FIXTURES) / "instruments";
}

/** The test packs are the only packs: the built-in General MIDI pack is pointed at a file that does not exist. */
struct TestPacks
{
    TestPacks() : path ("ROVOX_INSTRUMENTS_PATH", fixturesDir().string()), gm ("ROVOX_GM_SOUNDFONT", "/nonexistent/none.sf3") {}

    EnvVar path, gm;
};

json notes (std::initializer_list<std::pair<int, double>> list, double lengthQn)
{
    json out = json::array();

    for (auto& [pitch, qn] : list)
        out.push_back ({ { "pitch", pitch }, { "start_qn", qn }, { "length_qn", lengthQn }, { "velocity", 100 } });

    return out;
}

/** Frequency of the strongest partial in 16384 samples from `from` (Hann window, parabolic interpolation of the log magnitude). */
double peakHz (const std::vector<float>& x, size_t from, double sampleRate)
{
    constexpr int n = 16384;
    std::vector<double> reim ((size_t) n * 2, 0.0);

    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (n - 1));
        reim[(size_t) i * 2] = (double) x[from + (size_t) i] * w;
    }

    fftInPlace (reim, n);
    const auto mag = [&] (int k) { return std::hypot (reim[(size_t) k * 2], reim[(size_t) k * 2 + 1]); };
    int best = 1;

    for (int k = 1; k < n / 2 - 1; ++k)
        if (mag (k) > mag (best))
            best = k;

    const double a = std::log (mag (best - 1) + 1e-30), b = std::log (mag (best) + 1e-30), c = std::log (mag (best + 1) + 1e-30);
    const double delta = 0.5 * (a - c) / (a - 2.0 * b + c);
    return (best + delta) * sampleRate / n;
}

double centsOff (double hz, double expectedHz)
{
    return 1200.0 * std::log2 (hz / expectedHz);
}

struct Rendered
{
    fs::path file;
    std::vector<std::vector<float>> ch;
    double sampleRate = 0.0;
};

/** A track with the SF2 preset `uid` playing `noteList`, rendered to the master. */
Rendered renderSf2 (test::Env& env, const std::string& uid, const json& noteList, const std::string& name)
{
    const auto id = env.newProject ("sf2");
    const auto track = env.ok (id, "track.create", { { "name", "Sine" } })["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", uid } } } });
    REQUIRE (added.ok);
    CHECK (added.data["plugin"]["is_instrument"] == true);
    CHECK (added.data["plugin"]["type"] == sf2XmlType);
    REQUIRE (env.run (id, "item.create_midi", { { "track_id", track }, { "notes", noteList } }).ok);

    RenderRequest req;
    req.name = name;
    req.tailS = 0.5;
    req.scope = "master";
    auto rendered = renderProject (env.project (id), req);

    if (! rendered.ok())
        std::cerr << "    " << rendered.error().toJson().dump() << "\n";

    REQUIRE (rendered.ok());
    Rendered out;
    out.file = env.project (id).rendersDir() / (name + ".wav");
    REQUIRE (test::readWav (out.file, out.ch, out.sampleRate));
    REQUIRE (out.ch.size() == 2);
    return out;
}
} // namespace

ROVOX_TEST (instrument, preset_uids_round_trip)
{
    CHECK (sf2PresetUid ("my-pack", 0, 24) == "rovox:sf2/my-pack/0-24");
    const auto parsed = parseSf2PresetUid ("rovox:sf2/my-pack/128-3");
    REQUIRE (parsed.has_value());
    CHECK (parsed->pack == "my-pack");
    CHECK (parsed->bank == 128);
    CHECK (parsed->program == 3);
    CHECK (! parseSf2PresetUid ("rovox:sf2").has_value());
    CHECK (! parseSf2PresetUid ("rovox:sf2/pack").has_value());
    CHECK (! parseSf2PresetUid ("rovox:sf2/pack/x-1").has_value());
    CHECK (! parseSf2PresetUid ("rovox:drums").has_value());
    CHECK (isSf2Uid ("rovox:sf2") && isSf2Uid ("rovox:sf2/a/0-0") && ! isSf2Uid ("rovox:sf2x"));
}

ROVOX_TEST (instrument, packs_are_found_and_verified_and_a_changed_file_is_caught)
{
    TestPacks packs;
    const auto scan = InstrumentPacks::scan();
    CHECK (scan.problems.empty());
    std::map<std::string, InstrumentPack> byId;

    for (auto& p : scan.packs)
        byId[p.id] = p;

    REQUIRE (byId.count (sinePack) == 1);
    REQUIRE (byId.count (unknownPack) == 1);
    CHECK (byId.count (InstrumentPacks::builtinId) == 0);
    CHECK (byId[sinePack].serverUse());
    CHECK (! byId[unknownPack].serverUse());
    CHECK (byId[sinePack].license == "AGPL-3.0-or-later");
    REQUIRE (byId[sinePack].instruments.size() == 1);
    CHECK (byId[sinePack].instruments[0].name == "Test Sine");
    CHECK (InstrumentPacks::verify (byId[sinePack])["ok"] == true);
    CHECK (InstrumentPacks::verify (byId[unknownPack])["ok"] == true);

    // a copy whose soundfont was changed no longer verifies
    const auto copy = test::makeTempDir ("sf2-pack-copy");
    fs::create_directories (copy / sinePack);
    fs::copy_file (fixturesDir() / sinePack / "pack.json", copy / sinePack / "pack.json");
    fs::copy_file (fixturesDir() / sinePack / "sine.sf2", copy / sinePack / "sine.sf2");
    {
        std::fstream f (copy / sinePack / "sine.sf2", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp (-100, std::ios::end);
        f.put ('\x55');
    }

    EnvVar path ("ROVOX_INSTRUMENTS_PATH", copy.string());
    auto changed = InstrumentPacks::find (sinePack);
    REQUIRE (changed.has_value());
    const auto result = InstrumentPacks::verify (*changed);
    CHECK (result["ok"] == false);
    CHECK (! result["problems"].empty());

    // a pack.json without a licence is refused
    CHECK (! InstrumentPacks::parse (json { { "id", "x" }, { "files", { { "a.sf2", "00" } } } }, copy).ok());
    CHECK (! InstrumentPacks::parse (json { { "id", "../x" }, { "license", "MIT" }, { "files", { { "a.sf2", "00" } } } }, copy).ok());
}

ROVOX_TEST (instrument, the_catalog_lists_pack_presets_with_the_licence_of_the_pack)
{
    TestPacks packs;
    test::Env env;

    // at least one preset that may be used on a server
    auto ok = env.ok ("", "plugin.catalog", { { "kind", "instrument" }, { "server_use_ok", true }, { "query", "Test Sine" } });
    REQUIRE (ok["plugins"].size() >= 1);
    const auto& rec = ok["plugins"][0];
    CHECK (rec["plugin_uid"].get<std::string>() == sf2PresetUid (sinePack, 0, 0));
    CHECK (rec["server_use_ok"] == true);
    CHECK (rec["license"] == "AGPL-3.0-or-later");
    CHECK (rec["kind"] == "instrument");
    CHECK (rec["io"]["midi_in"] == true);
    CHECK (rec["pack"]["id"] == sinePack);

    // the pack with an unknown licence is listed for the operator, but not under server_use_ok
    auto all = env.ok ("", "plugin.catalog", { { "kind", "instrument" }, { "query", "Test Sine" } });
    bool unknownListed = false;

    for (auto& p : all["plugins"])
        if (p["plugin_uid"].get<std::string>() == sf2PresetUid (unknownPack, 0, 0))
        {
            unknownListed = true;
            CHECK (p["server_use_ok"] == "unknown");
        }

    CHECK (unknownListed);

    for (auto& p : ok["plugins"])
        CHECK (p["plugin_uid"].get<std::string>() != sf2PresetUid (unknownPack, 0, 0));
}

ROVOX_TEST (instrument, a_pack_with_an_unknown_licence_is_refused_for_a_restricted_tenant)
{
    TestPacks packs;
    test::Env env;
    auto& catalog = test::host().catalog();
    const auto freeRec = catalog.find (sf2PresetUid (sinePack, 0, 0));
    const auto unknown = catalog.find (sf2PresetUid (unknownPack, 0, 0));
    REQUIRE (freeRec.has_value());
    REQUIRE (unknown.has_value());

    // "sf2_restricted" has no tenant record: it is restricted to server_use_ok plugins (fail closed); the operator ("local") is not
    CHECK (PluginCatalog::licenceAllows ("sf2_restricted", freeRec));
    CHECK (! PluginCatalog::licenceAllows ("sf2_restricted", unknown));
    auto refused = PluginCatalog::requireLicence ("sf2_restricted", unknown, sf2PresetUid (unknownPack, 0, 0));
    REQUIRE (! refused.ok());
    CHECK (refused.error().code == errc::forbiddenLicense);
    CHECK (PluginCatalog::requireLicence ("local", unknown, "x").ok());
    CHECK (! catalog.find ("rovox:sf2/no-such-pack/0-0").has_value());

    // the operator can add it, an unknown pack is a NOT_FOUND
    const auto id = env.newProject ("sf2-licence");
    const auto track = env.ok (id, "track.create", { { "name", "T" } })["track_id"].get<std::string>();
    CHECK (env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", sf2PresetUid (unknownPack, 0, 0) } } } }).ok);
    auto missing = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "rovox:sf2/no-such-pack/0-0" } } } });
    CHECK (! missing.ok);
}

ROVOX_TEST (instrument, notes_sound_at_the_right_pitch_and_two_renders_are_identical)
{
    TestPacks packs;
    test::Env env;
    const auto uid = sf2PresetUid (sinePack, 0, 0);

    // 120 bpm: 2 quarter notes are 1 s; A3, A4, A5
    const auto noteList = notes ({ { 57, 0.0 }, { 69, 2.0 }, { 81, 4.0 } }, 1.5);
    const auto first = renderSf2 (env, uid, noteList, "sf2a");
    REQUIRE (std::abs (first.sampleRate - 44100.0) < 1.0);

    const double expected[] = { 220.0, 440.0, 880.0 };

    for (int i = 0; i < 3; ++i)
    {
        const size_t from = (size_t) std::llround ((i * 1.0 + 0.1) * first.sampleRate);
        REQUIRE (first.ch[0].size() > from + 16384);
        const auto hz = peakHz (first.ch[0], from, first.sampleRate);
        const auto cents = centsOff (hz, expected[i]);

        if (std::abs (cents) > 5.0)
            std::cerr << "    note " << i << ": " << hz << " Hz, " << cents << " cents off\n";

        CHECK (std::abs (cents) <= 5.0);
    }

    // not silent, finite, silent before the first note's release tail ends
    double peak = 0.0;

    for (size_t i = 0; i < first.ch[0].size(); ++i)
    {
        REQUIRE (std::isfinite (first.ch[0][i]) && std::isfinite (first.ch[1][i]));
        peak = std::max (peak, (double) std::abs (first.ch[0][i]));
    }

    CHECK (peak > 0.05);

    // the same project again: the same bytes
    const auto second = renderSf2 (env, uid, noteList, "sf2b");
    const auto a = sha256File (first.file), b = sha256File (second.file);
    REQUIRE (a.has_value() && b.has_value());
    CHECK (a == b);
}

ROVOX_TEST (instrument, the_plugin_state_names_the_pack_and_a_missing_pack_is_reported)
{
    TestPacks packs;
    test::Env env;
    const auto id = env.newProject ("sf2-state");
    const auto track = env.ok (id, "track.create", { { "name", "T" } })["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", sf2PresetUid (sinePack, 0, 0) } } } });
    REQUIRE (added.ok);
    auto* plugin = env.project (id).findPlugin (added.data["plugin_id"].get<std::string>());
    REQUIRE (plugin != nullptr);

    auto state = sf2PluginExtras (*plugin);
    REQUIRE (state.is_object());
    CHECK (state["status"] == "ready");
    CHECK (state["pack"] == sinePack);
    CHECK (state["preset"] == "Test Sine");
    CHECK (state["license"] == "AGPL-3.0-or-later");
    const auto identity = sf2PackIdentity (*plugin);
    REQUIRE (identity.is_object());
    CHECK (identity["id"] == sinePack);
    CHECK (identity["sha256"].get<std::string>().size() == 64);

    bool shown = false;

    const auto listed = env.ok (id, "fx.list", { { "target", track } });     // kept: a range-for over ["fx"] of a temporary dangles

    for (auto& fx : listed["fx"])
        shown = shown || (fx.contains ("sf2") && fx["sf2"]["status"] == "ready" && fx["loaded"] == true);

    CHECK (shown);

    // the pack disappears (another search path): the plugin says so instead of rendering silence without a word
    EnvVar elsewhere ("ROVOX_INSTRUMENTS_PATH", test::makeTempDir ("sf2-empty").string());
    auto gone = sf2PluginExtras (*plugin);
    CHECK (gone["status"] == "error");
    CHECK (gone["error"].get<std::string>().find (sinePack) != std::string::npos);
}

ROVOX_TEST (instrument, the_shipped_general_midi_soundfont_is_in_the_catalog)
{
    // the runtime image installs musescore-general-soundfont-small; the build image may not, and then this skips (ROVOX_REQUIRE_SF2=1 makes that a failure)
    const bool require = std::getenv ("ROVOX_REQUIRE_SF2") != nullptr;
    EnvVar path ("ROVOX_INSTRUMENTS_PATH", test::makeTempDir ("sf2-no-packs").string());
    std::error_code ec;

    if (! fs::exists (InstrumentPacks::builtinSoundfont(), ec))
    {
        std::cerr << (require ? "    the image should have MuseScore_General_Lite.sf3 but it is missing\n" : "    skipped: no MuseScore General Lite soundfont installed\n");
        CHECK (! require);
        return;
    }

    test::Env env;
    auto r = env.ok ("", "plugin.catalog", { { "kind", "instrument" }, { "server_use_ok", true }, { "query", "Acoustic Grand Piano" } });
    REQUIRE (r["plugins"].size() >= 1);
    const auto& rec = r["plugins"][0];
    CHECK (rec["plugin_uid"].get<std::string>() == sf2PresetUid (InstrumentPacks::builtinId, 0, 0));
    CHECK (rec["license"] == "MIT");
    CHECK (rec["server_use_ok"] == true);
    CHECK (rec["pack"]["id"] == InstrumentPacks::builtinId);
    CHECK (! rec["pack"]["attribution"].get<std::string>().empty());

    // the GPL TimGM6mb is never offered
    auto timgm = env.ok ("", "plugin.catalog", { { "kind", "instrument" }, { "query", "timgm" } });
    CHECK (timgm["plugins"].empty());

    // the pack verifies
    auto pack = InstrumentPacks::find (InstrumentPacks::builtinId);
    REQUIRE (pack.has_value());
    CHECK (InstrumentPacks::verify (*pack)["ok"] == true);
}
