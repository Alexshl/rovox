// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "FadeStage.h"
#include "PanLaw.h"
#include "PanStage.h"
#include "ReaperFx.h"
#include "RppImport.h"
#include "RppParser.h"
#include "Render.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <cmath>
#include <cstdlib>
#include <fstream>

using namespace rovox;

namespace
{
const char* const syntheticRpp = R"RPP(<REAPER_PROJECT 0.1 "7.0/macOS-arm64" 1700000000
  RECORD_PATH "Media" ""
  TEMPO 120 4 4 0
  <TEMPOENVEX
    ACT 0 -1
    PT 8 140 1
    PT 16 90 0
  >
  MARKER 1 0 Intro 0 0 1 B {968C993E-C7FE-744F-A589-0B8931C8BDCB} 0 2
  MARKER 2 10.5 "Verse 1" 0 0 1 B {F59ED0C6-6290-F749-8C4E-E0E2B2A91987} 0 2
  MARKER 3 20 "Outro" 0 0 1
  MARKER 4 4 "Region A" 1 0 1
  MARKER 4 8 "" 1 0 1
  <WEIRDBLOCK some args
    UNKNOWNLINE 1 2 3
    <NESTED
      X 1
    >
  >
  <TRACK {AAAAAAAA-0000-0000-0000-000000000001}
    NAME "Band"
    PEAKCOL 16576
    VOLPAN 0.5 0 -1 -1 1
    ISBUS 1 1
  >
  <TRACK {AAAAAAAA-0000-0000-0000-000000000002}
    NAME "Guitar"
    PEAKCOL 33554431
    VOLPAN 0.7943282347242815 -0.5 -1 -1 1
    MUTESOLO 0 0 0
    ISBUS 0 0
    <FXCHAIN
      WNDRECT 1 2 3 4
      <VST "VST3: ReaComp (Cockos)" reacomp.vst3 0 "" 1919247213
        AAAAAAAA
        AAAAAA
      >
      <VST "VST: ReaEQ (Cockos)" reaeq.vst.dylib 0 "" 1919247729
        AAAAAAAA
      >
      <JS loudness_meter ""
        0 0
      >
    >
    <ITEM
      POSITION 1
      LENGTH 2
      SOFFS 0.5 0
      FADEIN 1 0.1 0 1 0 0 0
      FADEOUT 1 0.2 0 1 0 0 0
      MUTE 0 0
      NAME "g1"
      VOLPAN 0.5 0 1 -1
      <SOURCE WAVE
        FILE "guitar.wav"
      >
    >
    <ITEM
      POSITION 5
      LENGTH 1.5
      SOFFS 0 0
      NAME ""
      <SOURCE WAVE
        FILE "Media/guitar.wav"
      >
    >
  >
  <TRACK {AAAAAAAA-0000-0000-0000-000000000003}
    NAME "Drums MIDI"
    ISBUS 0 -1
    AUXRECV 1 0 0.5 0 0 0 0 0 0 -1:U 0 -1 ''
    <ITEM
      POSITION 0
      LENGTH 4
      <SOURCE MIDI
        HASDATA 1 960 QN
        E 0 90 24 60
        E 480 80 24 00
        E 480 90 26 60
        E 480 80 26 00
      >
    >
    <ITEM
      POSITION 8
      LENGTH 2
      <SOURCE MIDI
        HASDATA 1 960 QN
        E 0 90 2a 50
        E 960 80 2a 00
      >
    >
  >
  <TRACK {AAAAAAAA-0000-0000-0000-000000000004}
    NAME "Missing"
    <ITEM
      POSITION 0
      LENGTH 3
      <SOURCE WAVE
        FILE "/Users/someone/else/nothere.wav"
      >
    >
    <ITEM
      POSITION 3
      LENGTH 1
      <SOURCE WAVE
        FILE "guitar.wav"
      >
    >
  >
>
)RPP";

fs::path writeRppFixture (const fs::path& dir)
{
    fs::create_directories (dir / "Media");
    test::writeWav (dir / "Media" / "guitar.wav", { test::sine (330, -14, 4.0, 44100) }, 44100, 24);
    std::ofstream (dir / "song.RPP") << syntheticRpp;
    return dir / "song.RPP";
}
} // namespace

ROVOX_TEST (rpp, tokenizer_and_tree)
{
    CHECK (tokenizeRppLine (R"(NAME "a b" 'c d' `e f` g)") == std::vector<std::string> ({ "NAME", "a b", "c d", "e f", "g" }));
    CHECK (tokenizeRppLine ("E 480 90 3c 60") .size() == 5);

    auto tree = parseRpp (syntheticRpp);
    REQUIRE (tree.ok());
    CHECK (tree.value().name == "REAPER_PROJECT");
    CHECK (tree.value().blocks ("TRACK").size() == 4);
    CHECK (tree.value().blocks ("WEIRDBLOCK").size() == 1);           // unknown blocks are kept, not rejected
    CHECK (tree.value().lines ("MARKER").size() == 5);
    CHECK (tree.value().blocks ("TRACK")[1]->blocks ("ITEM").size() == 2);
    CHECK (tree.value().blocks ("TRACK")[1]->block ("FXCHAIN")->toText().find ("reacomp.vst3") != std::string::npos);

    CHECK (! parseRpp ("<REAPER_PROJECT 0.1\n  <TRACK\n").ok());      // unbalanced
    CHECK (! parseRpp (">\n").ok());
    CHECK (! parseRpp ("hello").ok());
}

ROVOX_TEST (rpp, synthetic_project_maps_to_tracks_items_markers_tempo_and_sends)
{
    test::Env env;
    const auto rpp = writeRppFixture (test::makeTempDir ("rpp"));
    const auto id = env.newProject ("from rpp");

    auto r = env.run (id, "import.rpp", { { "path", rpp.string() } });
    REQUIRE (r.ok);
    const auto& rep = r.data;

    CHECK (rep["imported"]["tracks"] == 4);
    CHECK (rep["imported"]["folders"] == 1);
    CHECK (rep["imported"]["items"] == 4);
    CHECK (rep["imported"]["midi_items"] == 2);
    CHECK (rep["imported"]["markers"] == 3);
    CHECK (rep["imported"]["regions"] == 1);
    CHECK (rep["imported"]["sends"] == 1);

    // REAPER stock FX are mapped to the Rovox stock effects with default settings, anything else is listed as unsupported; the raw chunk stays on the track
    REQUIRE (rep["mapped"].size() == 2);
    CHECK (rep["mapped"][0]["name"] == "VST3: ReaComp (Cockos)");
    CHECK (rep["mapped"][0]["mapped_to"] == "Rovox Comp");
    CHECK (rep["mapped"][1]["name"] == "VST: ReaEQ (Cockos)");
    CHECK (rep["mapped"][1]["mapped_to"] == "Rovox EQ");
    CHECK (rep["mapped"][0]["status"] == "mapped (default params, original settings not translated)");
    CHECK (! rep["mapped"][0]["track_id"].get<std::string>().empty());
    REQUIRE (rep["unsupported"].size() == 1);
    CHECK (rep["unsupported"][0]["name"] == "loudness_meter");
    CHECK (! rep["unsupported"][0]["track_id"].get<std::string>().empty());

    // the absolute macOS path is not found; the guitar.wav ones are (relative to the RPP, via RECORD_PATH, by basename)
    REQUIRE (rep["missing_media"].size() == 1);
    CHECK (rep["missing_media"][0]["path"] == "/Users/someone/else/nothere.wav");

    auto& p = env.project (id);
    auto audio = te::getAudioTracks (p.edit());
    REQUIRE (audio.size() == 3);
    CHECK (audio[0]->getName() == "Guitar");
    CHECK_NEAR (audio[0]->getVolumePlugin()->getVolumeDb(), -2.0, 0.01);
    CHECK_NEAR (audio[0]->getVolumePlugin()->getPan(), -0.5, 1e-4);
    CHECK (audio[0]->state.hasProperty ("rovox:unsupported_fx"));
    CHECK (toStd (audio[0]->state["rovox:unsupported_fx"].toString()).find ("reacomp.vst3") != std::string::npos);

    // the Rovox effects sit in the chain before the fader and keep their original chunk
    {
        auto fx = env.ok (id, "fx.list", { { "target", Project::idOf (audio[0]->state) } })["fx"];
        REQUIRE (fx.size() >= 3);
        // the item has REAPER's default fade (a curve, not the engine's linear ramp): a hidden stage applies it before the FX, it is not listed
        CHECK (fx[0]["name"] == "Rovox Comp");
        CHECK (fx[1]["name"] == "Rovox EQ");
        CHECK (fx[2]["is_fader"] == true);
        for (auto& f : fx)
            CHECK (f["type"] != "rovoxfades");
        CHECK (fx[0]["index"] == 0);
        auto* mapped = p.findPlugin (rep["mapped"][1]["plugin_id"].get<std::string>());
        REQUIRE (mapped != nullptr);
        CHECK (toStd (mapped->state["rovox:reaper_fx"].toString()).find ("reaeq.vst.dylib") != std::string::npos);
    }

    // folder nesting: Guitar and Drums are children of the folder, Missing is not
    CHECK (audio[0]->state.getParent().hasType (te::IDs::FOLDERTRACK));
    CHECK (audio[1]->state.getParent().hasType (te::IDs::FOLDERTRACK));
    CHECK (! audio[2]->state.getParent().hasType (te::IDs::FOLDERTRACK));

    // items: position, length, source offset, fades, gain
    auto* clip = dynamic_cast<te::WaveAudioClip*> (audio[0]->getClips()[0]);
    REQUIRE (clip != nullptr);
    CHECK_NEAR (clip->getPosition().getStart().inSeconds(), 1.0, 1e-9);
    CHECK_NEAR (clip->getPosition().getLength().inSeconds(), 2.0, 1e-9);
    CHECK_NEAR (clip->getPosition().getOffset().inSeconds(), 0.5, 1e-9);
    CHECK_NEAR (itemFadeSeconds (*clip, true), 0.1, 1e-6);
    CHECK_NEAR (itemFadeSeconds (*clip, false), 0.2, 1e-6);
    CHECK_NEAR (clip->getGainDB(), -6.02, 0.05);
    CHECK (clip->getName() == "g1");
    CHECK (fs::exists (p.dir() / p.manifest().media.front().path));      // copied into media/ by default

    // MIDI
    auto* midi = dynamic_cast<te::MidiClip*> (audio[1]->getClips()[0]);
    REQUIRE (midi != nullptr);
    REQUIRE (midi->getSequence().getNotes().size() == 2);
    CHECK (midi->getSequence().getNotes()[0]->getNoteNumber() == 0x24);
    CHECK_NEAR (midi->getSequence().getNotes()[1]->getStartBeat().inBeats(), 1.0, 1e-9);
    CHECK_NEAR (midi->getSequence().getNotes()[1]->getLengthBeats().inBeats(), 0.5, 1e-9);

    // markers, region, tempo
    auto markers = env.ok (id, "marker.list")["markers"];
    CHECK (markers.size() == 4);
    CHECK (p.edit().tempoSequence.getNumTempos() == 3);
    CHECK_NEAR (p.edit().tempoSequence.getTempo (0)->getBpm(), 120.0, 1e-9);
    CHECK_NEAR (p.edit().tempoSequence.getTempo (1)->getBpm(), 140.0, 1e-9);

    // send: AUXRECV is on the receiver (Drums, index 2) from source index 1 (Guitar)
    bool sendOnGuitar = false, returnOnDrums = false;

    for (auto* pl : audio[0]->pluginList.getPlugins())
        sendOnGuitar = sendOnGuitar || dynamic_cast<te::AuxSendPlugin*> (pl) != nullptr;

    for (auto* pl : audio[1]->pluginList.getPlugins())
        returnOnDrums = returnOnDrums || dynamic_cast<te::AuxReturnPlugin*> (pl) != nullptr;

    CHECK (sendOnGuitar);
    CHECK (returnOnDrums);

    // ids on everything, sends get snd_
    std::set<std::string> prefixes;

    for (auto pl : te::getAllPlugins (p.edit(), false))
        prefixes.insert (Project::idOf (pl->state).substr (0, 3));

    CHECK (prefixes.count ("snd") == 1);

    // one undo removes the whole import
    REQUIRE (env.run (id, "undo").ok);
    CHECK (te::getAllTracks (p.edit()).size() == 0 || te::getAudioTracks (p.edit()).isEmpty());
    CHECK (p.edit().getMarkerManager().getMarkers().isEmpty());

    // the missing file must surface at render time, not as a silent render
    REQUIRE (env.run (id, "redo").ok);
    test::pumpMessages (50);
    RenderRequest req;
    req.tailS = 0;
    req.failOn.erase ("silent_track");
    auto render = renderProject (p, req);
    REQUIRE (! render.ok());
    CHECK (render.error().code == errc::preflightFailed);
    bool offline = false;

    for (auto& f : render.error().details["details"])
        offline = offline || f["check"] == "offline_media";

    CHECK (offline);
}

ROVOX_TEST (rpp, no_copy_media_references_files_in_place_and_broken_files_change_nothing)
{
    test::Env env;
    const auto rpp = writeRppFixture (test::makeTempDir ("rpp2"));
    const auto id = env.newProject ("external");
    REQUIRE (env.run (id, "import.rpp", { { "path", rpp.string() }, { "copy_media", false } }).ok);
    auto& p = env.project (id);
    REQUIRE (! p.manifest().media.empty());
    CHECK (p.manifest().media[0].external);
    CHECK (fs::path (p.manifest().media[0].path).is_absolute());
    CHECK (fs::is_empty (p.mediaDir()));

    const auto bad = test::makeTempDir ("rpp3") / "bad.RPP";
    std::ofstream (bad) << "<REAPER_PROJECT 0.1\n  <TRACK\n    NAME \"x\"\n";
    const auto before = test::snapshot (env, id);
    auto r = env.run (id, "import.rpp", { { "path", bad.string() } });
    CHECK (! r.ok);
    CHECK (test::snapshot (env, id) == before);
    CHECK (! env.run (id, "import.rpp", { { "path", "/nonexistent.RPP" } }).ok);
}

//==============================================================================
namespace
{
/** One track with six REAPER stock FX (BYPASS comes before a block, FXID after it) plus a second ReaComp that has no entry in the sidecar. */
const char* const fxRpp = R"RPP(<REAPER_PROJECT 0.1 "7.0/macOS-arm64" 1700000000
  TEMPO 120 4 4 0
  <TRACK {AAAAAAAA-0000-0000-0000-0000000000F1}
    NAME "Vox"
    ISBUS 0 0
    <FXCHAIN
      BYPASS 0 0 0
      <VST "VST: ReaEQ (Cockos)" reaeq.vst.dylib 0 "" 1919247729
        AAAA
      >
      FLOATPOS 0 0 0 0
      FXID {11111111-1111-1111-1111-111111111111}
      WAK 0 0
      BYPASS 0 0 0
      <VST "VST: ReaComp (Cockos)" reacomp.vst.dylib 0 "" 1919247213
        AAAA
      >
      FXID {22222222-2222-2222-2222-222222222222}
      WAK 0 0
      BYPASS 1 0 0
      <VST "VST: ReaVerbate (Cockos)" reaverbate.vst.dylib 0 "" 1919252066
        AAAA
      >
      FXID {33333333-3333-3333-3333-333333333333}
      BYPASS 0 0 0
      <VST "VST: ReaLimit (Cockos)" realimit.vst.dylib 0 "" 1919709556
        AAAA
      >
      FXID {44444444-4444-4444-4444-444444444444}
      BYPASS 0 0 0
      <VST "VST: ReaGate (Cockos)" reagate.vst.dylib 0 "" 1919247975
        AAAA
      >
      FXID {55555555-5555-5555-5555-555555555555}
      BYPASS 0 0 0
      <VST "VST: ReaDelay (Cockos)" readelay.vst.dylib 0 "" 1919247468
        AAAA
      >
      FXID {66666666-6666-6666-6666-666666666666}
      BYPASS 0 0 0
      <VST "VST: ReaComp (Cockos)" reacomp.vst.dylib 0 "" 1919247213
        AAAA
      >
      FXID {77777777-7777-7777-7777-777777777777}
    >
  >
>
)RPP";

json p (int index, const char* name, double normalized, const char* formatted)
{
    return { { "index", index }, { "name", name }, { "normalized", normalized }, { "formatted", formatted } };
}

/** The generic block every REAPER FX ends with: Bypass ("normal" / "bypassed"), Wet (percent) and Delta. Its "Wet" is not the effect's own "Wet". */
json withGeneric (json params, int firstIndex, const char* bypass = "normal", const char* wetPercent = "100")
{
    params.push_back (p (firstIndex, "Bypass", std::string (bypass) == "normal" ? 0.0 : 1.0, bypass));
    params.push_back (p (firstIndex + 1, "Wet", std::atof (wetPercent) / 100.0, wetPercent));
    params.push_back (p (firstIndex + 2, "Delta", 0.0, "normal"));
    return params;
}

json fxJson (int index, const char* name, const char* guid, bool enabled, json params, json config = json::object())
{
    const int count = (int) params.size();
    return { { "index", index }, { "name", name }, { "guid", guid }, { "enabled", enabled }, { "config", config }, { "params", withGeneric (params, count) } };
}

/** What tools/reaper/rovox_export_fx.lua writes, in the shape of a real REAPER 7 export (unit-less formatted values, the trailing
    Bypass / Wet / Delta block, ReaEQ band config) with synthetic values. The limiter's GUID differs from the RPP's (found by track index + order). */
json fxSidecar()
{
    auto eq = fxJson (0, "VST: ReaEQ (Cockos)", "{11111111-1111-1111-1111-111111111111}", true,
                      { p (0, "Freq-Low Shelf", 0.4, "250.0"), p (1, "Gain-Low Shelf", 0.0, "-inf"), p (2, "BW-Low Shelf", 0.1, "0.80"),
                        p (3, "Freq-Band 2", 0.5, "580.9"), p (4, "Gain-Band 2", 0.4, "-1.1"), p (5, "BW-Band 2", 0.3, "2.00"),
                        p (6, "Freq-Band 3", 0.5, "1000.0"), p (7, "Gain-Band 3", 0.6, "2.5"), p (8, "BW-Band 3", 0.3, "1.50"),
                        p (9, "Freq-High Shelf 4", 0.8, "9000.0"), p (10, "Gain-High Shelf 4", 0.5, "1.5"), p (11, "BW-High Shelf 4", 0.1, "0.80"),
                        p (12, "Freq-High Pass 5", 0.2, "100.0"), p (13, "Gain-High Pass 5", 0.5, "0.0"), p (14, "BW-High Pass 5", 0.3, "2.00"),
                        p (15, "Global Gain", 0.5, "0.5") },
                      { { "BANDENABLED0", "1" }, { "BANDENABLED1", "1" }, { "BANDENABLED2", "1" }, { "BANDENABLED3", "1" }, { "BANDENABLED4", "0" },
                        { "BANDTYPE0", "0" }, { "BANDTYPE1", "8" }, { "BANDTYPE2", "8" }, { "BANDTYPE3", "1" }, { "BANDTYPE4", "4" } });
    auto comp = fxJson (1, "VST: ReaComp (Cockos)", "{22222222-2222-2222-2222-222222222222}", true,
                        { p (0, "Threshold", 0.4, "-24.5"), p (1, "Ratio", 0.1, "3.00"), p (2, "Attack", 0.1, "15.0"), p (3, "Release", 0.2, "250"),
                          p (4, "Pre-comp", 0.0, "2.00"), p (5, "resvd", 0.0, "0"), p (6, "Lowpass", 1.0, "20000"), p (7, "Hipass", 0.0, "0"),
                          p (8, "SignIn", 0.0, "0"), p (9, "AudIn", 0.0, "0"), p (10, "Dry", 0.0, "-inf"), p (11, "Wet", 0.5, "-3.0"),
                          p (12, "Filter Preview", 0.0, "0"), p (13, "RMS size", 0.1, "10.0"), p (14, "Knee", 0.2, "4.0"), p (15, "Auto Make Up Gain", 0.0, "0"),
                          p (16, "Auto Release", 1.0, "1"), p (17, "Legacy Attack/Knee Options", 0.25, "0.25"), p (18, "Deprecated Broken Anti-Alias", 0.0, "0"),
                          p (19, "Multichannel Mode", 0.0, "0"), p (20, "Metering Index", 0.0, "0") });
    auto verb = fxJson (2, "VST: ReaVerbate (Cockos)", "{33333333-3333-3333-3333-333333333333}", false,
                        { p (0, "Wet", 0.5, "-9.0"), p (1, "Dry", 0.0, "-inf"), p (2, "Room size", 0.29, "80"), p (3, "Dampening", 0.3, "30"),
                          p (4, "Width", 0.75, "0.75"), p (5, "Delay", 0.1, "15"), p (6, "Lowpass", 0.9, "9500"), p (7, "Hipass", 0.1, "120") });
    auto limit = fxJson (3, "VST: ReaLimit (Cockos)", "{99999999-9999-9999-9999-999999999999}", true,
                         { p (0, "Threshold", 0.5, "-6.0"), p (1, "Ceiling", 0.99, "-0.3"), p (2, "Release", 0.3, "120") });
    auto gate = fxJson (4, "VST: ReaGate (Cockos)", "{55555555-5555-5555-5555-555555555555}", true,
                        { p (0, "Threshold", 0.5, "-35.0"), p (1, "Attack", 0.1, "2"), p (2, "Release", 0.2, "150"), p (3, "Pre-open", 0.0, "0"),
                          p (4, "Hold", 0.1, "40"), p (5, "Lowpass", 1.0, "20000"), p (6, "Hipass", 0.0, "0"), p (7, "SignIn", 0.0, "0"), p (8, "AudIn", 0.0, "0"),
                          p (9, "Dry", 0.2, "-24.0"), p (10, "Wet", 1.0, "+0.0"), p (11, "Noise level", 0.0, "-inf"), p (12, "Hysteresis", 0.1, "4.0"),
                          p (13, "Preview Filter", 0.0, "0"), p (14, "RMS size", 0.0, "0"), p (15, "Send MIDI", 0.0, "0"), p (16, "Midi Note", 0.5, "69"),
                          p (17, "Midi Channel", 0.0, "1"), p (18, "Invert Wet", 0.0, "disabled"), p (19, "Multichannel Mode", 0.0, "0"), p (20, "Metering Index", 0.0, "0") });
    auto delay = fxJson (5, "VST: ReaDelay (Cockos)", "{66666666-6666-6666-6666-666666666666}", true,
                         { p (0, "Wet", 0.5, "-6.0 dB"), p (1, "Dry", 0.5, "0.0 dB"),
                           p (2, "Tap 1 enabled", 1.0, "On"), p (3, "Length 1 (time)", 0.1, "375 ms"), p (4, "Feedback 1", 0.5, "-6.0 dB"),
                           p (5, "Lowpass 1", 0.5, "5.00 kHz"), p (6, "Volume 1", 0.5, "-2.0 dB"), p (7, "Pan 1", 0.4, "20% L"),
                           p (8, "Tap 2 enabled", 1.0, "On"), p (9, "Length 2 (musical)", 0.00390625, "1.00"), p (10, "Volume 2", 0.5, "-3.0 dB") });
    // a JS effect (no Rovox counterpart); its text holds a Latin-1 byte, written below as a raw 0xE9
    auto jsfx = fxJson (6, "JS: De-esser", "{88888888-8888-8888-8888-888888888888}", true, json::array ({ p (0, "Time Constants", 0.5, "A: 3 LATIN1s - R: 50 ms") }));
    return { { "version", 1 }, { "tracks", json::array ({ { { "index", 0 }, { "name", "Vox" }, { "guid", "{AAAAAAAA-0000-0000-0000-0000000000F1}" },
                                                             { "fx", json::array ({ eq, comp, verb, limit, gate, delay, jsfx }) } } }) },
             { "master", { { "name", "MASTER" }, { "fx", json::array() } } } };
}

/** The sidecar as the exporter writes it before its UTF-8 fix: with a raw Latin-1 byte (0xE9) in a text. */
std::string fxSidecarText (int indent = -1)
{
    auto text = fxSidecar().dump (indent);
    const auto at = text.find ("LATIN1");
    REQUIRE (at != std::string::npos);
    text.replace (at, 6, "\xe9");
    return text;
}

json findParamIn (const json& fxInfo, const std::string& ident)
{
    for (auto& q : fxInfo["params"])
        if (q["ident"] == ident)
            return q;

    return nullptr;
}

double valueOf (test::Env& env, const std::string& project, const json& mapped, const char* ident)
{
    auto info = env.ok (project, "fx.params", { { "plugin_id", mapped["plugin_id"] } })["fx"];
    auto param = findParamIn (info, ident);
    REQUIRE (! param.is_null());
    return param["value"].get<double>();
}

bool translatedHas (const json& mapped, const std::string& to, double value, double tol = 1.0e-3)
{
    for (auto& t : mapped["translated"])
        if (t["to"] == to && std::abs (t["value"].get<double>() - value) <= tol)
            return true;

    return false;
}
} // namespace

ROVOX_TEST (rpp, reaper_value_text_is_parsed_with_its_unit)
{
    auto v = parseReaperValue ("1.50 kHz");
    CHECK (v.ok && v.unit == "khz" && std::abs (v.value - 1.5) < 1e-9);
    v = parseReaperValue ("-12.0 dB");
    CHECK (v.ok && v.unit == "db" && std::abs (v.value + 12.0) < 1e-9);
    v = parseReaperValue ("-inf dB");
    CHECK (v.ok && v.infinite && v.value < 0 && v.unit == "db");
    v = parseReaperValue ("3.0:1");
    CHECK (v.ok && v.unit == ":1" && std::abs (v.value - 3.0) < 1e-9);
    v = parseReaperValue ("inf:1");
    CHECK (v.ok && v.infinite && v.value > 0);
    v = parseReaperValue ("250 ms");
    CHECK (v.ok && v.unit == "ms");
    v = parseReaperValue ("On");
    CHECK (! v.ok);
    CHECK (normaliseReaperGuid ("{1A2B3C4D-0000-ABCD-0000-000000000001}") == normaliseReaperGuid ("1a2b3c4d-0000-abcd-0000-000000000001"));

    // the Rovox EQ design formula and its inverse
    const double q = 2.0, bw = bandwidthOctForQ (q), p2 = std::pow (2.0, bw);
    CHECK_NEAR (std::sqrt (p2) / (p2 - 1.0), q, 1e-6);
}

ROVOX_TEST (rpp, sidecar_translates_the_rea_effects_into_rovox_parameters)
{
    test::Env env;
    const auto dir = test::makeTempDir ("rpp-fx");
    std::ofstream (dir / "fxsong.RPP") << fxRpp;
    std::ofstream (dir / "fxsong.rovoxfx.json", std::ios::binary) << fxSidecarText (1);          // next to the RPP, named like the project

    const auto id = env.newProject ("fx sidecar");
    auto r = env.run (id, "import.rpp", { { "path", (dir / "fxsong.RPP").string() } });

    if (! r.ok && r.error)
        std::cerr << "    import.rpp: " << r.error->message << "\n";

    REQUIRE (r.ok);
    const auto& mapped = r.data["mapped"];
    REQUIRE (mapped.size() == 7);
    CHECK (r.data["unsupported"].empty());
    CHECK (r.data["warnings"].empty());          // the Latin-1 byte in the sidecar is repaired, not a reason to drop the file

    const auto& eq = mapped[0];
    const auto& comp = mapped[1];
    const auto& verb = mapped[2];
    const auto& limit = mapped[3];
    const auto& gate = mapped[4];
    const auto& delay = mapped[5];
    const auto& plain = mapped[6];

    for (auto* m : { &eq, &comp, &verb, &limit, &gate, &delay })
    {
        CHECK ((*m)["status"] == "mapped (params translated)");
        CHECK ((*m)["translated"].is_array() && ! (*m)["translated"].empty());
        CHECK ((*m)["untranslated"].is_array());
    }

    // the second ReaComp has no sidecar entry: defaults, as before
    CHECK (plain["status"] == "mapped (default params, original settings not translated)");
    CHECK (! plain.contains ("translated"));
    CHECK_NEAR (valueOf (env, id, plain, "threshold"), -18.0, 1e-3);

    // ReaEQ: types from the band names (cross-checked with BANDTYPE), band 5 (high pass) off by BANDENABLED4, the other slots off
    CHECK_NEAR (valueOf (env, id, eq, "b1_type"), 0.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b1_freq"), 250.0, 1e-2);
    CHECK_NEAR (valueOf (env, id, eq, "b1_gain"), -150.0, 1e-3);         // -inf: amplitude 0 (a gain of -150 dB or less)
    CHECK_NEAR (valueOf (env, id, eq, "b1_bw"), 0.8, 1e-3);
    CHECK_NEAR (valueOf (env, id, eq, "b1_on"), 1.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b2_type"), 2.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b2_freq"), 580.9, 1e-1);
    CHECK_NEAR (valueOf (env, id, eq, "b2_gain"), -1.1, 1e-3);
    CHECK_NEAR (valueOf (env, id, eq, "b2_bw"), 2.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, eq, "b3_type"), 2.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b3_gain"), 2.5, 1e-3);
    CHECK_NEAR (valueOf (env, id, eq, "b3_bw"), 1.5, 1e-3);
    CHECK_NEAR (valueOf (env, id, eq, "b4_type"), 1.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b4_freq"), 9000.0, 1.0);
    CHECK_NEAR (valueOf (env, id, eq, "b4_gain"), 1.5, 1e-3);
    CHECK_NEAR (valueOf (env, id, eq, "b4_on"), 1.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b5_type"), 4.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b5_on"), 0.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b6_on"), 0.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "b8_on"), 0.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, eq, "out_gain"), 0.5, 1e-3);
    CHECK (eq["untranslated"].empty());

    // ReaComp (detector, envelope and knee as ReaComp does): unit-less ratio 3.00, release 250 (ms), pre-comp, own wet -3 dB / dry -inf; Auto Release is
    // listed as untranslated
    CHECK_NEAR (valueOf (env, id, comp, "threshold"), -24.5, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "ratio"), 3.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "attack"), 15.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "release"), 250.0, 1e-2);
    CHECK_NEAR (valueOf (env, id, comp, "knee"), 4.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "lookahead"), 2.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "rms"), 10.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "wet"), -3.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "dry"), -150.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, comp, "auto_makeup"), 0.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, comp, "legacy_knee"), 0.25, 1e-6);
    CHECK_NEAR (valueOf (env, id, comp, "reaper_compat"), 1.0, 1e-6);
    REQUIRE (comp["untranslated"].size() == 1);          // only the auto release: open filters, off flags and reserved slots are no settings
    CHECK (comp["untranslated"][0]["param"] == "Auto Release");

    // ReaLimit (matched by track index and order, the GUID differs): threshold boosts, the release is the slow-stage rate in dB per second
    CHECK_NEAR (valueOf (env, id, limit, "threshold"), -6.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, limit, "ceiling"), -0.3, 1e-3);
    CHECK_NEAR (valueOf (env, id, limit, "release_rate"), 120.0, 1e-2);
    CHECK_NEAR (valueOf (env, id, limit, "reaper_compat"), 1.0, 1e-6);

    // ReaGate (gain trajectory as ReaGate does); its dry level is a dry gain
    CHECK_NEAR (valueOf (env, id, gate, "threshold"), -35.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, gate, "attack"), 2.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, gate, "hold"), 40.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, gate, "release"), 150.0, 1e-2);
    CHECK_NEAR (valueOf (env, id, gate, "rea_hysteresis"), 4.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, gate, "dry"), -24.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, gate, "wet"), 0.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, gate, "reaper_compat"), 1.0, 1e-6);
    CHECK (gate["untranslated"].empty());

    // ReaVerbate (also bypassed in the RPP)
    CHECK_NEAR (valueOf (env, id, verb, "wet"), -9.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, verb, "dry"), -96.0, 1e-3);          // -inf dB, clamped to the Rovox floor
    CHECK_NEAR (valueOf (env, id, verb, "room_size"), 0.8, 1e-3);
    CHECK_NEAR (valueOf (env, id, verb, "damping"), 0.3, 1e-3);
    CHECK_NEAR (valueOf (env, id, verb, "width"), 0.75, 1e-3);
    CHECK_NEAR (valueOf (env, id, verb, "pre_delay"), 15.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, verb, "high_cut"), 9500.0, 1e-1);
    CHECK_NEAR (valueOf (env, id, verb, "low_cut"), 120.0, 1e-2);
    CHECK (verb["untranslated"].empty());

    // ReaDelay (tap loops as ReaDelay does): tap 1 in ms with feedback -6 dB, tap 2 musical (1 eighth note), taps 3 and 4 off
    CHECK_NEAR (valueOf (env, id, delay, "reaper_compat"), 1.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, delay, "t1_time"), 375.0, 1e-2);
    CHECK_NEAR (valueOf (env, id, delay, "t1_on"), 1.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, delay, "t1_fb"), -6.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, delay, "t1_lp"), 5000.0, 1e-1);
    CHECK_NEAR (valueOf (env, id, delay, "t1_gain"), -2.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, delay, "t1_pan"), -0.2, 1e-3);
    CHECK_NEAR (valueOf (env, id, delay, "t2_on"), 1.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, delay, "t2_musical"), 1.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, delay, "t2_gain"), -3.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, delay, "t3_on"), 0.0, 1e-6);
    CHECK_NEAR (valueOf (env, id, delay, "wet"), -6.0, 1e-3);
    CHECK_NEAR (valueOf (env, id, delay, "dry"), 0.0, 1e-3);

    // bypass carries over: the ReaVerbate is bypassed in the RPP (and disabled in the sidecar), the others are enabled
    CHECK (verb["enabled"] == false);
    CHECK (env.ok (id, "fx.params", { { "plugin_id", verb["plugin_id"] } })["fx"]["enabled"] == false);
    CHECK (env.ok (id, "fx.params", { { "plugin_id", eq["plugin_id"] } })["fx"]["enabled"] == true);
    CHECK (env.ok (id, "fx.params", { { "plugin_id", plain["plugin_id"] } })["fx"]["enabled"] == true);
}

ROVOX_TEST (rpp, generic_block_and_units_of_the_real_reaper_shape)
{
    // the trailing Bypass / Wet / Delta block is told apart from the effect's own Wet by position
    auto entry = [] (json params, int first, const char* bypass, const char* wet)
    {
        return parseReaperFxEntry ({ { "name", "x" }, { "params", withGeneric (params, first, bypass, wet) } });
    };

    // ReaComp: dry -inf / wet +0 dB / FX wet 50 %: the FX-wide Wet is the Rovox fx_wet, the own Wet is the dB one
    auto r = translateReaperFx ("comp", entry ({ p (0, "Threshold", 0, "-20.0"), p (1, "Dry", 0, "-inf"), p (2, "Wet", 0, "+0.0") }, 3, "bypassed", "50"));
    CHECK (r.bypassed);
    CHECK (r.untranslated.empty());
    double fxWet = -1.0, wet = -999.0;

    for (auto& t : r.translated)
    {
        if (t.to == "fx_wet")   fxWet = t.value;
        if (t.to == "wet")      wet = t.value;
    }

    CHECK_NEAR (fxWet, 50.0, 1e-6);
    CHECK_NEAR (wet, 0.0, 1e-6);

    // the reverb carries the FX wet itself, like the others
    auto g = translateReaperFx ("verb", entry ({ p (0, "Room size", 0, "50") }, 1, "normal", "40"));
    CHECK (! g.bypassed);
    CHECK (g.untranslated.empty());
    double verbFxWet = -1.0;

    for (auto& t : g.translated)
        if (t.to == "fx_wet")
            verbFxWet = t.value;

    CHECK_NEAR (verbFxWet, 40.0, 1e-6);

    // ReaEQ: BANDTYPE decides the type (it tells the kinds of "Band" apart), the band name is the fallback and is cross-checked
    auto eqEntry = parseReaperFxEntry ({ { "name", "ReaEQ" }, { "config", { { "BANDTYPE0", "1" }, { "BANDENABLED0", "1" } } },
                                         { "params", json::array ({ p (0, "Freq-Low Shelf", 0, "200.0"), p (1, "Gain-Low Shelf", 0, "3.0"), p (2, "BW-Low Shelf", 0, "0.80") }) } });
    auto e = translateReaperFx ("eq", eqEntry);
    bool typeOk = false;

    for (auto& t : e.translated)
        typeOk = typeOk || (t.to == "b1_type" && t.value == 1.0 && t.note.find ("disagrees") != std::string::npos);

    CHECK (typeOk);

    // text that is not UTF-8 is repaired as Latin-1; valid UTF-8 is left alone
    CHECK (repairUtf8 ("A: 3 \xe9s") == "A: 3 \xc3\xa9s");
    CHECK (repairUtf8 ("caf\xc3\xa9") == "caf\xc3\xa9");
    CHECK (repairUtf8 ("\xc3") == "\xc3\x83");
}

ROVOX_TEST (rpp, bypass_carries_over_without_a_sidecar_and_an_explicit_sidecar_path_is_accepted)
{
    test::Env env;
    const auto dir = test::makeTempDir ("rpp-fx2");
    std::ofstream (dir / "song.RPP") << fxRpp;

    // no sidecar: every Rea effect keeps its defaults, the bypassed one stays bypassed
    const auto id = env.newProject ("fx plain");
    auto r = env.run (id, "import.rpp", { { "path", (dir / "song.RPP").string() } });
    REQUIRE (r.ok);
    REQUIRE (r.data["mapped"].size() == 7);

    for (auto& m : r.data["mapped"])
    {
        CHECK (m["status"] == "mapped (default params, original settings not translated)");
        CHECK (! m.contains ("translated"));
    }

    CHECK (r.data["mapped"][2]["enabled"] == false);
    CHECK (env.ok (id, "fx.params", { { "plugin_id", r.data["mapped"][2]["plugin_id"] } })["fx"]["enabled"] == false);
    CHECK_NEAR (valueOf (env, id, r.data["mapped"][0], "b2_freq"), 400.0, 1e-3);

    // explicit fx_sidecar, a name that is not looked up beside the RPP
    std::ofstream (dir / "elsewhere.json", std::ios::binary) << fxSidecarText();
    const auto id2 = env.newProject ("fx explicit");
    auto r2 = env.run (id2, "import.rpp", { { "path", (dir / "song.RPP").string() }, { "fx_sidecar", (dir / "elsewhere.json").string() } });

    if (! r2.ok && r2.error)
        std::cerr << "    import.rpp: " << r2.error->message << "\n";

    REQUIRE (r2.ok);
    CHECK (r2.data["mapped"][1]["status"] == "mapped (params translated)");
    CHECK_NEAR (valueOf (env, id2, r2.data["mapped"][1], "threshold"), -24.5, 1e-3);

    // a broken sidecar is a warning, not a failure
    std::ofstream (dir / "broken.json") << "{ nope";
    const auto id3 = env.newProject ("fx broken");
    auto r3 = env.run (id3, "import.rpp", { { "path", (dir / "song.RPP").string() }, { "fx_sidecar", (dir / "broken.json").string() } });
    REQUIRE (r3.ok);
    CHECK (r3.data["mapped"][1]["status"] == "mapped (default params, original settings not translated)");
    bool warned = false;

    for (auto& w : r3.data["warnings"])
        warned = warned || w["code"] == "FX_SIDECAR_INVALID";

    CHECK (warned);
}

ROVOX_TEST (rpp, local_reaper_projects_if_present)
{
    const char* home = std::getenv ("HOME");
    const fs::path dir = fs::path (home != nullptr ? home : "/nonexistent") / "MyFiles" / "REAPER Media";

    if (! fs::is_directory (dir))
    {
        std::cerr << "    (skipped: no local REAPER projects)\n";
        return;
    }

    test::Env env;

    for (auto& e : fs::directory_iterator (dir))
    {
        if (e.path().extension() != ".RPP" && e.path().extension() != ".rpp")
            continue;

        const auto id = env.newProject (e.path().stem().string());
        auto r = env.run (id, "import.rpp", { { "path", e.path().string() }, { "copy_media", false } });
        REQUIRE (r.ok);
        std::cerr << "    " << e.path().filename().string() << ": " << r.data["imported"].dump() << " unsupported " << r.data["unsupported"].size()
                  << " missing " << r.data["missing_media"].size() << " warnings " << r.data["warnings"].size() << "\n";
        env.pm.close (id, SaveMode::discard);
    }
}

//==============================================================================
// Pan modes / pan law (REAPER semantics) and item play rate
namespace
{
std::string panRpp (const std::string& trackBody, const std::string& itemBody, int projectPanMode = 3)
{
    return "<REAPER_PROJECT 0.1 \"7.0/macOS-arm64\" 1700000000\n  PANLAW 1\n  PANMODE " + std::to_string (projectPanMode) + "\n  TEMPO 120 4 4 0\n"
           "  <TRACK {AAAAAAAA-0000-0000-0000-0000000000A1}\n    NAME \"T\"\n" + trackBody + "\n    ISBUS 0 0\n"
           "    <ITEM\n      POSITION 0\n      LENGTH 1\n" + itemBody + "\n      <SOURCE WAVE\n        FILE \"item.wav\"\n      >\n    >\n  >\n>\n";
}

/** Imports the RPP into a new project (media in the RPP's folder), renders the master and returns the planar result. */
std::vector<std::vector<float>> importAndRender (test::Env& env, const fs::path& dir, const std::string& rppText, std::string& projectId, double* sr = nullptr)
{
    std::ofstream (dir / "t.RPP") << rppText;
    projectId = env.newProject ("pan");
    REQUIRE (env.run (projectId, "import.rpp", { { "path", (dir / "t.RPP").string() } }).ok);
    RenderRequest rq;
    rq.tailS = 0.0;
    auto rr = renderProject (env.project (projectId), rq);

    if (! rr.ok())
        std::cerr << "    render failed: " << rr.error().toJson().dump() << "\n";

    REQUIRE (rr.ok());
    std::vector<std::vector<float>> ch;
    double rate = 0;
    REQUIRE (test::readWav (env.project (projectId).dir() / rr.value()["file"]["path"].get<std::string>(), ch, rate));

    if (sr != nullptr)
        *sr = rate;

    if (std::getenv ("ROVOX_TEST_KEEP_RENDER") != nullptr)
        fs::copy_file (env.project (projectId).dir() / rr.value()["file"]["path"].get<std::string>(), std::string (std::getenv ("ROVOX_TEST_KEEP_RENDER")), fs::copy_options::overwrite_existing);

    return ch;
}

double peakOf (const std::vector<float>& v, size_t from, size_t to)
{
    double m = 0;

    for (size_t i = from; i < std::min(to, v.size()); ++i)
        m = std::max (m, (double) std::abs (v[i]));

    return m;
}
} // namespace

ROVOX_TEST (rpp, pan_law_maths_per_mode)
{
    double l, r;
    // 0 dB law (REAPER default): the near side stays at full level, the far side fades out linearly
    pan::monoGains (0.5, 0.0, l, r);
    CHECK_NEAR (l, 0.5, 1e-9);
    CHECK_NEAR (r, 1.0, 1e-9);
    pan::monoGains (-0.7, 0.0, l, r);
    CHECK_NEAR (l, 1.0, 1e-9);
    CHECK_NEAR (r, 0.3, 1e-9);

    // -3 dB: constant power, centre at -3 dB, hard pan at full level
    pan::monoGains (0.0, -3.0103, l, r);
    CHECK_NEAR (l, std::sqrt (0.5), 1e-4);
    CHECK_NEAR (r, std::sqrt (0.5), 1e-4);
    pan::monoGains (1.0, -3.0103, l, r);
    CHECK_NEAR (l, 0.0, 1e-9);
    CHECK_NEAR (r, 1.0, 1e-9);

    // -6 dB: centre at 0.5; -4.5 dB: centre at -4.5 dB
    pan::monoGains (0.0, -6.0206, l, r);
    CHECK_NEAR (l, 0.5, 1e-4);
    pan::monoGains (0.0, -4.5, l, r);
    CHECK_NEAR (20.0 * std::log10 (l), -4.5, 1e-3);
    CHECK_NEAR (pan::lawDbFromReaper (1.0), 0.0, 1e-9);
    CHECK_NEAR (pan::lawDbFromReaper (0.5), -6.0206, 1e-3);

    // stereo balance (mode 3): balance without law for stereo, pan with law for mono
    auto m = pan::matrix (pan::Mode::balance, -0.7, 1.0, -3.0, false);
    CHECK_NEAR (m.ll, 1.0, 1e-9);
    CHECK_NEAR (m.rr, 0.3, 1e-9);
    CHECK_NEAR (m.lr, 0.0, 1e-9);
    m = pan::matrix (pan::Mode::balance, 0.0, 1.0, -3.0, true);
    CHECK_NEAR (m.ll, std::sqrt (0.5), 1e-3);

    // classic 3.x: the law applies to stereo too
    m = pan::matrix (pan::Mode::classic, 0.0, 1.0, -3.0, false);
    CHECK_NEAR (m.ll, std::sqrt (0.5), 1e-3);
    CHECK_NEAR (m.rr, std::sqrt (0.5), 1e-3);

    // stereo pan: centre is a pass-through, hard right puts both channels on the right
    m = pan::matrix (pan::Mode::stereoPan, 0.0, 1.0, 0.0, false);
    CHECK_NEAR (m.ll, 1.0, 1e-9);
    CHECK_NEAR (m.rr, 1.0, 1e-9);
    CHECK_NEAR (m.lr + m.rl, 0.0, 1e-9);
    m = pan::matrix (pan::Mode::stereoPan, 1.0, 1.0, 0.0, false);
    CHECK_NEAR (m.ll, 0.0, 1e-9);
    CHECK_NEAR (m.lr, 0.0, 1e-9);
    CHECK_NEAR (m.rl, 1.0, 1e-9);
    CHECK_NEAR (m.rr, 1.0, 1e-9);

    // dual pan: (-1, +1) is the identity, a swap exchanges the channels
    m = pan::matrix (pan::Mode::dualPan, -1.0, 1.0, 0.0, false);
    CHECK_NEAR (m.ll, 1.0, 1e-9);
    CHECK_NEAR (m.rr, 1.0, 1e-9);
    CHECK_NEAR (m.lr, 0.0, 1e-9);
    m = pan::matrix (pan::Mode::dualPan, 1.0, -1.0, 0.0, false);
    CHECK_NEAR (m.lr, 1.0, 1e-9);
    CHECK_NEAR (m.rl, 1.0, 1e-9);
    CHECK_NEAR (m.ll, 0.0, 1e-9);

    CHECK (pan::modeFromReaper (3) == pan::Mode::balance);
    CHECK (pan::modeFromReaper (0) == pan::Mode::classic);
    CHECK (pan::modeFromReaper (5) == pan::Mode::stereoPan);
    CHECK (pan::modeFromReaper (6) == pan::Mode::dualPan);
}

ROVOX_TEST (rpp, pan_mode_and_law_are_imported_and_new_tracks_get_reaper_defaults)
{
    test::Env env;
    const auto dir = test::makeTempDir ("panimport");
    test::writeWav (dir / "item.wav", { test::sine (1000, -12, 1.0, 48000) }, 48000, 24);
    std::string rppText = panRpp ("    VOLPAN 1 0.2 -1 -1 1\n    PANMODE 6", "", 0);
    const auto pos = rppText.find ("PANLAW 1");
    rppText.replace (pos, 8, "PANLAW 0.70794578438414");
    std::ofstream (dir / "t.RPP") << rppText;
    const auto id = env.newProject ("panimport");
    REQUIRE (env.run (id, "import.rpp", { { "path", (dir / "t.RPP").string() } }).ok);
    auto& p = env.project (id);
    CHECK (projectPanMode (p.edit()) == 0);
    CHECK_NEAR (projectPanLawDb (p.edit()), -3.0, 0.01);
    auto audio = te::getAudioTracks (p.edit());
    REQUIRE (audio.size() == 1);
    CHECK (audio[0]->getVolumePlugin()->state.getProperty ("rovox:panMode", -1).operator int() == 6);

    // a new project and a new track: balance / mono pan, 0 dB law
    const auto id2 = env.newProject ("fresh");
    CHECK (projectPanMode (env.project (id2).edit()) == 3);
    CHECK_NEAR (projectPanLawDb (env.project (id2).edit()), 0.0, 1e-9);
    const auto tid = env.ok (id2, "track.create", { { "name", "x" }, { "pan", 0.5 } })["track_id"].get<std::string>();
    auto tracks = te::getAudioTracks (env.project (id2).edit());
    REQUIRE (tracks.size() == 1);
    CHECK (tracks[0]->getVolumePlugin()->getPan() > 0.49f);
    CHECK (toStd (tracks[0]->getVolumePlugin()->state[te::IDs::type].toString()) == "rovoxvolume");
    (void) tid;
}

ROVOX_TEST (rpp, stereo_item_balance_leaves_the_near_channel_alone_and_fades_the_far_one)
{
    test::Env env;
    const auto dir = test::makeTempDir ("panstereo");
    test::writeWav (dir / "item.wav", { test::sine (1000, -6, 1.0, 48000), test::sine (1000, -9, 1.0, 48000) }, 48000, 24);
    std::string id;
    auto out = importAndRender (env, dir, panRpp ("    VOLPAN 1 -0.7 -1 -1 1", ""), id);
    REQUIRE (out.size() >= 2);
    const double l = peakOf (out[0], 4800, 43200), r = peakOf (out[1], 4800, 43200);
    CHECK_NEAR (20.0 * std::log10 (l), -6.0, 0.15);                         // REAPER: unchanged
    CHECK_NEAR (20.0 * std::log10 (r), -9.0 + 20.0 * std::log10 (0.3), 0.15);    // REAPER: x 0.3
}

ROVOX_TEST (rpp, mono_item_pan_right_keeps_the_right_channel_and_halves_the_left)
{
    test::Env env;
    const auto dir = test::makeTempDir ("panmono");
    test::writeWav (dir / "item.wav", { test::sine (1000, -12, 1.0, 48000) }, 48000, 24);
    std::string id;
    auto out = importAndRender (env, dir, panRpp ("    VOLPAN 1 0.5 -1 -1 1", ""), id);
    REQUIRE (out.size() >= 2);
    const double l = peakOf (out[0], 4800, 43200), r = peakOf (out[1], 4800, 43200);
    CHECK_NEAR (20.0 * std::log10 (r), -12.0, 0.15);
    CHECK_NEAR (20.0 * std::log10 (l), -12.0 - 6.0206, 0.15);
}

ROVOX_TEST (rpp, item_playrate_scales_speed_and_source_offset_and_preserve_pitch_selects_a_stretcher)
{
    test::Env env;
    const auto dir = test::makeTempDir ("playrate");
    test::writeWav (dir / "item.wav", { test::sine (440, -12, 4.0, 48000) }, 48000, 24);

    // resample (pitch follows speed): 2 x, source offset 0.5 s
    std::string id;
    double sr = 48000.0;
    auto out = importAndRender (env, dir, panRpp ("    VOLPAN 1 0 -1 -1 1", "      SOFFS 0.5\n      PLAYRATE 2 0 0 -1 0 0.0025"), id, &sr);
    {
        auto& p = env.project (id);
        auto clips = te::getAudioTracks (p.edit())[0]->getClips();
        REQUIRE (clips.size() == 1);
        auto* wc = dynamic_cast<te::WaveAudioClip*> (clips[0]);
        REQUIRE (wc != nullptr);
        CHECK_NEAR (wc->getSpeedRatio(), 2.0, 1e-9);
        CHECK_NEAR (wc->getPosition().getOffset().inSeconds(), 0.25, 1e-9);
        CHECK_NEAR (wc->getPosition().getLength().inSeconds(), 1.0, 1e-9);
        CHECK (wc->getTimeStretchMode() == te::TimeStretcher::disabled);
    }

    // zero crossings of the render: 440 Hz at double speed is 880 Hz
    REQUIRE (! out.empty());
    int crossings = 0;

    for (size_t i = 4801; i < 43200; ++i)
        if (out[0][i - 1] < 0.0f && out[0][i] >= 0.0f)
            ++crossings;

    CHECK_NEAR ((double) crossings / ((43200 - 4801) / sr), 880.0, 15.0);

    // preserve pitch: a stretcher is selected
    auto out2 = importAndRender (env, dir, panRpp ("    VOLPAN 1 0 -1 -1 1", "      PLAYRATE 1.05833333333333 1 0 -1 0 0.0025"), id, &sr);
    auto clips = te::getAudioTracks (env.project (id).edit())[0]->getClips();
    REQUIRE (clips.size() == 1);
    auto* wc = dynamic_cast<te::WaveAudioClip*> (clips[0]);
    REQUIRE (wc != nullptr);
    CHECK_NEAR (wc->getSpeedRatio(), 1.05833333333333, 1e-9);
    CHECK (wc->getTimeStretchMode() != te::TimeStretcher::disabled);

    // the render stretches: the pitch stays at 440 Hz while the source runs 5.8 % faster
    REQUIRE (! out2.empty());
    int crossings2 = 0;

    for (size_t i = 9601; i < 38400; ++i)
        if (out2[0][i - 1] < 0.0f && out2[0][i] >= 0.0f)
            ++crossings2;

    CHECK_NEAR ((double) crossings2 / ((38400 - 9601) / sr), 440.0, 15.0);
}
