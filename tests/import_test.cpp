// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Import.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <chrono>
#include <cstdlib>
#include <fstream>

using namespace rovox;

ROVOX_TEST (import, sha256_known_vectors)
{
    CHECK (sha256Hex ("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK (sha256Hex ("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK (sha256Hex (std::string (1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

ROVOX_TEST (import, synthetic_r20_folder_becomes_named_tracks_aligned_at_zero)
{
    test::Env env;
    const auto folder = test::makeR20Folder (test::makeTempDir ("r20"), "ZOOM0001.zprj", 8, 2.0);
    const auto id = env.newProject ("r20");

    auto plan = env.ok (id, "import.plan", { { "paths", { folder.string() } } });
    CHECK (plan["detected_source"] == "zoom_r20");
    REQUIRE (plan["tracks"].size() == 8);

    for (int i = 0; i < 8; ++i)
    {
        char n[8];
        std::snprintf (n, sizeof (n), "REC%02d", i + 1);
        CHECK (plan["tracks"][(size_t) i]["name"] == n);
    }

    // sidecar files (DATA/, peaks/, R20PRJDATA.r20prj) are not warnings
    for (auto& w : plan["warnings"])
        CHECK (w["code"] != "NOT_AUDIO");

    const auto rev = env.project (id).revision();
    auto applied = env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] }, { "rename", { { "REC01", "Гітара" }, { "REC02", "Бас" } } } });
    CHECK (env.project (id).revision() == rev + 1);
    REQUIRE (applied["tracks"].size() == 8);
    CHECK (applied["tracks"][0]["name"] == "Гітара");
    CHECK (applied["tracks"][1]["name"] == "Бас");
    CHECK (applied["tracks"][2]["name"] == "REC03");

    auto tracks = env.ok (id, "track.list")["tracks"];
    REQUIRE (tracks.size() == 8);

    // every clip at 0.0 s, length equal to the file sample for sample
    for (int i = 0; i < 8; ++i)
    {
        const auto frames = (std::int64_t) std::llround ((2.0 + (i + 1) * 0.0137) * 44100.0);
        REQUIRE (tracks[(size_t) i]["items"].size() == 1);
        CHECK (tracks[(size_t) i]["items"][0]["start_s"].get<double>() == 0.0);
        CHECK (std::llround (tracks[(size_t) i]["items"][0]["length_s"].get<double>() * 44100.0) == frames);
    }

    CHECK (env.project (id).manifest().media.size() == 8);
    CHECK (env.project (id).manifest().media[0].path.rfind ("media/", 0) == 0);

    // one undo removes the whole import
    REQUIRE (env.run (id, "undo").ok);
    CHECK (env.ok (id, "track.list")["tracks"].empty());

    // media is content-addressed inside the project: deleting the source does not break it
    REQUIRE (env.run (id, "redo").ok);
    fs::remove_all (folder);
    CHECK (fs::exists (env.project (id).dir() / env.project (id).manifest().media[0].path));
    const auto sha = sha256File (env.project (id).dir() / env.project (id).manifest().media[0].path);
    REQUIRE (sha.has_value());
    CHECK (env.project (id).manifest().media[0].path.find (*sha) != std::string::npos);
}

ROVOX_TEST (import, takes_non_audio_and_missing_files_are_warnings)
{
    test::Env env;
    const auto dir = test::makeTempDir ("takes");
    test::writeWav (dir / "REC06_01.WAV", { test::sine (440, -20, 1.0, 44100) }, 44100);
    test::writeWav (dir / "REC06_02.WAV", { test::sine (440, -20, 1.5, 44100) }, 44100);
    test::writeWav (dir / "Punk Rock Verse BPM130.WAV", { test::sine (330, -20, 1.0, 44100), test::sine (331, -20, 1.0, 44100) }, 44100);
    std::ofstream (dir / "notes.txt") << "hello";
    std::ofstream (dir / ".DS_Store") << "x";
    std::ofstream (dir / "R20PRJDATA.r20prj", std::ios::binary) << "ZOOM R20    PROJECT DATA VER0001SEQD" << '\0' << "REC06_01.WAV" << '\0' << "REC06_09.WAV" << '\0' << "SEQE";

    const auto id = env.newProject();

    auto plan = env.ok (id, "import.plan", { { "paths", { dir.string() } }, { "preset", "zoom_r20" } });
    std::set<std::string> codes;

    for (auto& w : plan["warnings"])
        codes.insert (w["code"]);

    CHECK (codes.count ("NOT_AUDIO") == 1);
    CHECK (codes.count ("MISSING_FILE") == 1);
    CHECK (codes.count ("NOT_IN_SEQUENCE") == 1);
    REQUIRE (plan["tracks"].size() == 3);                            // two takes of REC06 as separate tracks + the free-named stereo file
    CHECK (plan["tracks"][0]["name"] == "REC06 take 1");
    CHECK (plan["tracks"][1]["name"] == "REC06 take 2");
    CHECK (plan["tracks"][2]["name"] == "Punk Rock Verse BPM130");
    CHECK (plan["tracks"][2]["clips"][0]["channels"] == 2);

    auto latest = env.ok (id, "import.plan", { { "paths", { dir.string() } }, { "preset", "zoom_r20" }, { "takes_mode", "latest" } });
    REQUIRE (latest["tracks"].size() == 2);
    CHECK (latest["tracks"][0]["name"] == "REC06");
    CHECK (latest["tracks"][0]["clips"][0]["file"] == "REC06_02.WAV");

    // a folder with nothing importable is an error, not an empty success
    const auto empty = test::makeTempDir ("empty");
    std::ofstream (empty / "readme.txt") << "x";
    auto r = env.run (id, "import.plan", { { "paths", { empty.string() } } });
    CHECK (! r.ok);
}

ROVOX_TEST (import, bwf_time_reference_alignment)
{
    const auto dir = test::makeTempDir ("bwf");
    test::writeWav (dir / "a.wav", { test::sine (440, -20, 1.0, 48000) }, 48000);
    ImportOptions o;
    o.align = "bwf";
    auto plan = planImport ({ dir / "a.wav" }, dir / "media", o);
    REQUIRE (plan.ok());
    CHECK (plan.value().tracks[0].clips[0].startS == 0.0);       // no bext chunk: stays at 0
    CHECK (plan.value().tracks[0].clips[0].info.sampleRate == 48000.0);
}

ROVOX_TEST (import, local_r20_projects_if_present)
{
    const char* home = std::getenv ("HOME");
    const char* envPath = std::getenv ("ROVOX_LOCAL_ZPRJ");
    const fs::path dir = envPath != nullptr ? fs::path (envPath) : fs::path (home != nullptr ? home : "/nonexistent") / "MyFiles" / "linija-nadreza" / "mukola20260513.zprj";

    if (! fs::is_directory (dir))
    {
        std::cerr << "    (skipped: no local .zprj at " << dir << ")\n";
        return;
    }

    test::Env env;
    const auto id = env.newProject ("local r20");
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto plan = env.ok (id, "import.plan", { { "paths", { dir.string() } } });
    const auto t1 = Clock::now();
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    const auto t2 = Clock::now();
    const auto planS = std::chrono::duration<double> (t1 - t0).count(), applyS = std::chrono::duration<double> (t2 - t1).count();
    std::uintmax_t bytes = 0;

    for (auto& e : fs::directory_iterator (dir))
        bytes += e.is_regular_file() ? e.file_size() : 0;

    std::cerr << "    " << dir.filename().string() << ": " << plan["tracks"].size() << " tracks, " << bytes / (1 << 20) << " MB; plan (probe + hash + stage) " << planS
              << " s, apply " << applyS << " s\n";
    CHECK (plan["detected_source"] == "zoom_r20");
    CHECK (plan["tracks"].size() >= 8);
    CHECK (applyS < 1.0);
    CHECK (env.ok (id, "track.list")["tracks"].size() == plan["tracks"].size());

    for (auto& t : env.ok (id, "track.list")["tracks"])
        for (auto& c : t["items"])
            CHECK (c["length_s"].get<double>() > 0.0);
}
