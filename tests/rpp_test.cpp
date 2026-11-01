// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RppImport.h"
#include "RppParser.h"
#include "Render.h"
#include "TestFramework.h"
#include "TestSupport.h"

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

    // FX are not translated: listed, raw chunk kept on the track
    REQUIRE (rep["unsupported"].size() == 1);
    CHECK (rep["unsupported"][0]["name"] == "VST3: ReaComp (Cockos)");
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
    CHECK_NEAR (clip->getFadeIn().inSeconds(), 0.1, 1e-6);
    CHECK_NEAR (clip->getFadeOut().inSeconds(), 0.2, 1e-6);
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
