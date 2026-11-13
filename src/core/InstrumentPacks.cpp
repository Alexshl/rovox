// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "InstrumentPacks.h"

#include "Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
const char* const gmProgramNames[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)",
    "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot" };

const char* const gmFamilies[16] = { "piano", "chromatic percussion", "organ", "guitar", "bass", "strings", "ensemble", "brass",
                                     "reed", "pipe", "synth lead", "synth pad", "synth effects", "ethnic", "percussive", "sound effects" };

std::string lowerCase (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

bool isSoundfontName (const std::string& name)
{
    const auto ext = lowerCase (fs::path (name).extension().string());
    return ext == ".sf2" || ext == ".sf3";
}

bool validId (const std::string& id)
{
    if (id.empty() || id.size() > 64 || ! std::isalnum ((unsigned char) id[0]))
        return false;

    return std::all_of (id.begin(), id.end(), [] (unsigned char c) { return std::isalnum (c) || c == '-' || c == '_' || c == '.'; })
           && id.find ("..") == std::string::npos;
}

bool validFileName (const std::string& name)
{
    return ! name.empty() && name.find ('/') == std::string::npos && name.find ('\\') == std::string::npos && name != "." && name != ".." && name.find ('\0') == std::string::npos;
}

std::optional<json> readJson (const fs::path& file)
{
    std::ifstream in (file, std::ios::binary);

    if (! in)
        return std::nullopt;

    auto j = json::parse (in, nullptr, false);

    if (j.is_discarded() || ! j.is_object())
        return std::nullopt;

    return j;
}

InstrumentPack builtinPack()
{
    InstrumentPack p;
    const auto file = InstrumentPacks::builtinSoundfont();
    p.id = InstrumentPacks::builtinId;
    p.version = "1";
    p.license = "MIT";
    p.serverUseOk = true;
    p.attribution = "MuseScore General Lite soundfont by S. Christian Collins and the MuseScore project, MIT License";
    p.sourceUrl = "https://musescore.org/en/handbook/3/soundfonts-and-sfz-files";
    p.licenseNotes = "Shipped with the Rovox image (Debian package musescore-general-soundfont-small); its licence file is in /usr/share/doc/musescore-general-soundfont-small.";
    p.dir = file.parent_path();
    p.builtin = true;
    p.files[file.filename().string()] = "";

    for (int i = 0; i < 128; ++i)
        p.instruments.push_back ({ gmProgramNames[i], 0, i, 0, 127, { "gm", gmFamilies[i / 8] } });

    return p;
}
} // namespace

fs::path InstrumentPack::soundfontPath() const
{
    for (auto& [name, sha] : files)
        if (isSoundfontName (name))
            return dir / name;

    return {};
}

std::vector<fs::path> InstrumentPacks::searchPaths()
{
    std::vector<fs::path> out;
    const char* env = std::getenv ("ROVOX_INSTRUMENTS_PATH");
    std::stringstream ss (env != nullptr && *env != 0 ? env : "/instruments");
    std::string part;

    while (std::getline (ss, part, ':'))
        if (! part.empty())
            out.emplace_back (part);

    return out;
}

fs::path InstrumentPacks::builtinSoundfont()
{
    const char* env = std::getenv ("ROVOX_GM_SOUNDFONT");
    return env != nullptr && *env != 0 ? fs::path (env) : fs::path ("/usr/share/sounds/sf3/MuseScore_General_Lite.sf3");
}

Result<InstrumentPack> InstrumentPacks::parse (const json& m, const fs::path& dir)
{
    if (! m.is_object())
        return makeError (errc::invalidParams, "pack.json is not a JSON object");

    InstrumentPack p;
    p.dir = dir;
    p.id = m.value ("id", "");

    if (! validId (p.id))
        return makeError (errc::invalidParams, "pack.json: id must be letters, digits, '-', '_' or '.' (got '" + p.id + "')");

    p.version = m.contains ("version") && m["version"].is_string() ? m["version"].get<std::string>() : "1";
    p.sha256 = lowerCase (m.contains ("sha256") && m["sha256"].is_string() ? m["sha256"].get<std::string>() : "");
    p.license = m.contains ("license") && m["license"].is_string() ? m["license"].get<std::string>() : "";
    p.attribution = m.contains ("attribution") && m["attribution"].is_string() ? m["attribution"].get<std::string>() : "";
    p.sourceUrl = m.contains ("source_url") && m["source_url"].is_string() ? m["source_url"].get<std::string>() : "";
    p.licenseNotes = m.contains ("license_notes") && m["license_notes"].is_string() ? m["license_notes"].get<std::string>() : "";
    p.serverUseOk = m.contains ("server_use_ok") && m["server_use_ok"].is_boolean() ? m["server_use_ok"] : json ("unknown");

    if (p.license.empty())
        return makeError (errc::invalidParams, "pack.json: license (an SPDX id) is required");

    if (! m.contains ("files") || ! m["files"].is_object() || m["files"].empty())
        return makeError (errc::invalidParams, "pack.json: files must list the soundfont file with its sha256");

    for (auto& [name, sha] : m["files"].items())
    {
        if (! validFileName (name) || ! sha.is_string())
            return makeError (errc::invalidParams, "pack.json: bad entry in files: '" + name + "'");

        p.files[name] = lowerCase (sha.get<std::string>());
    }

    if (p.soundfontPath().empty())
        return makeError (errc::invalidParams, "pack.json: files has no .sf2 / .sf3 soundfont");

    if (m.contains ("instruments"))
    {
        if (! m["instruments"].is_array())
            return makeError (errc::invalidParams, "pack.json: instruments must be an array");

        for (auto& i : m["instruments"])
        {
            if (! i.is_object() || ! i.contains ("name") || ! i["name"].is_string())
                return makeError (errc::invalidParams, "pack.json: every instrument needs a name");

            PackInstrument pi;
            pi.name = i["name"].get<std::string>();
            pi.bank = i.contains ("bank") && i["bank"].is_number_integer() ? i["bank"].get<int>() : 0;
            pi.program = i.contains ("gm_program") && i["gm_program"].is_number_integer() ? i["gm_program"].get<int>() : 0;

            if (pi.bank < 0 || pi.bank > 16383 || pi.program < 0 || pi.program > 127)
                return makeError (errc::invalidParams, "pack.json: instrument '" + pi.name + "' has a bank or gm_program out of range");

            if (i.contains ("range"))
            {
                if (! i["range"].is_array() || i["range"].size() != 2 || ! i["range"][0].is_number_integer() || ! i["range"][1].is_number_integer())
                    return makeError (errc::invalidParams, "pack.json: instrument '" + pi.name + "' range must be [low, high]");

                pi.lowNote = std::clamp (i["range"][0].get<int>(), 0, 127);
                pi.highNote = std::clamp (i["range"][1].get<int>(), 0, 127);
            }

            if (i.contains ("tags") && i["tags"].is_array())
                for (auto& t : i["tags"])
                    if (t.is_string())
                        pi.tags.push_back (t.get<std::string>());

            p.instruments.push_back (std::move (pi));
        }
    }

    if (p.instruments.empty())
        p.instruments.push_back ({ p.id, 0, 0, 0, 127, {} });

    return p;
}

json InstrumentPacks::toJson (const InstrumentPack& p)
{
    json instruments = json::array();

    for (auto& i : p.instruments)
        instruments.push_back ({ { "name", i.name }, { "bank", i.bank }, { "gm_program", i.program }, { "range", { i.lowNote, i.highNote } }, { "tags", i.tags } });

    json files = json::object();

    for (auto& [name, sha] : p.files)
        files[name] = sha;

    return { { "id", p.id }, { "version", p.version }, { "sha256", contentSha256 (p) }, { "license", p.license }, { "server_use_ok", p.serverUseOk },
             { "attribution", p.attribution }, { "source_url", p.sourceUrl }, { "license_notes", p.licenseNotes }, { "files", files },
             { "instruments", instruments }, { "builtin", p.builtin }, { "path", p.dir.string() } };
}

PackScan InstrumentPacks::scan()
{
    PackScan out;
    std::vector<std::string> seen;

    for (auto& root : searchPaths())
    {
        std::error_code ec;
        std::vector<fs::path> dirs;

        for (fs::directory_iterator it (root, ec), end; ! ec && it != end; it.increment (ec))
            if (it->is_directory (ec))
                dirs.push_back (it->path());

        std::sort (dirs.begin(), dirs.end());

        for (auto& dir : dirs)
        {
            const auto file = dir / "pack.json";

            if (! fs::exists (file, ec))
                continue;

            auto manifest = readJson (file);

            if (! manifest)
            {
                out.problems.push_back ({ { "path", file.string() }, { "error", "pack.json is not valid JSON" } });
                continue;
            }

            auto parsed = parse (*manifest, dir);

            if (! parsed.ok())
            {
                out.problems.push_back ({ { "path", file.string() }, { "error", parsed.error().message } });
                continue;
            }

            if (parsed.value().id != dir.filename().string())
            {
                out.problems.push_back ({ { "path", file.string() }, { "error", "pack id '" + parsed.value().id + "' differs from its directory name" } });
                continue;
            }

            if (std::find (seen.begin(), seen.end(), parsed.value().id) != seen.end())
                continue;

            seen.push_back (parsed.value().id);
            out.packs.push_back (std::move (parsed.value()));
        }
    }

    std::error_code ec;

    if (fs::exists (builtinSoundfont(), ec) && std::find (seen.begin(), seen.end(), std::string (builtinId)) == seen.end())
        out.packs.push_back (builtinPack());

    return out;
}

std::optional<InstrumentPack> InstrumentPacks::find (const std::string& id)
{
    for (auto& p : scan().packs)
        if (p.id == id)
            return p;

    return std::nullopt;
}

json InstrumentPacks::verify (const InstrumentPack& p)
{
    json problems = json::array();
    std::error_code ec;

    if (p.license.empty())
        problems.push_back ("no licence");

    for (auto& [name, sha] : p.files)
    {
        const auto file = p.dir / name;

        if (! fs::is_regular_file (file, ec))
        {
            problems.push_back ("missing file " + name);
            continue;
        }

        if (p.builtin)
        {
            if (fs::file_size (file, ec) == 0)
                problems.push_back ("empty file " + name);

            continue;
        }

        const auto actual = sha256File (file);

        if (! actual)
            problems.push_back ("cannot read " + name);
        else if (*actual != sha)
            problems.push_back ("sha256 of " + name + " is " + *actual + ", the pack says " + (sha.empty() ? "nothing" : sha));
    }

    if (! p.builtin)
    {
        const auto main = p.soundfontPath();

        if (! p.sha256.empty() && ! main.empty() && p.files.count (main.filename().string()) != 0 && p.files.at (main.filename().string()) != p.sha256)
            problems.push_back ("pack sha256 is not the sha256 of " + main.filename().string());

        if (p.sha256.empty())
            problems.push_back ("no sha256");
    }

    return { { "id", p.id }, { "ok", problems.empty() }, { "problems", problems } };
}

std::string InstrumentPacks::contentSha256 (const InstrumentPack& p)
{
    if (! p.sha256.empty() || ! p.builtin)
        return p.sha256;

    static std::mutex mutex;
    static std::map<std::string, std::string> cache;
    const auto file = p.soundfontPath();
    std::error_code sizeEc, timeEc;
    const auto size = fs::file_size (file, sizeEc);
    const auto time = fs::last_write_time (file, timeEc).time_since_epoch().count();
    const auto key = file.string() + "|" + std::to_string (sizeEc ? 0 : size) + "|" + std::to_string (timeEc ? 0 : time);

    {
        std::lock_guard<std::mutex> lock (mutex);

        if (auto it = cache.find (key); it != cache.end())
            return it->second;
    }

    auto hash = sha256File (file).value_or ("");
    std::lock_guard<std::mutex> lock (mutex);
    cache[key] = hash;
    return hash;
}

} // namespace rovox
