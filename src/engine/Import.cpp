// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Import.h"

#include "Command.h"
#include "Ids.h"
#include "RppImport.h"
#include "Sha256.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace rovox
{

json AudioInfo::toJson() const
{
    json j { { "channels", channels }, { "sample_rate", sampleRate }, { "bit_depth", bitDepth }, { "frames", frames }, { "duration_s", durationS() } };

    if (bwfTimeReferenceS.has_value())
        j["bwf_time_reference_s"] = *bwfTimeReferenceS;

    return j;
}

json ImportWarning::toJson() const
{
    json j { { "code", code }, { "message", message } };

    if (! file.empty())
        j["file"] = file;

    return j;
}

json ImportClipPlan::toJson() const
{
    json j = info.toJson();
    j["file"] = origName;
    j["sha256"] = sha256;
    j["media_path"] = mediaPath;
    j["start_s"] = startS;
    return j;
}

json ImportTrackPlan::toJson() const
{
    json c = json::array();

    for (auto& clip : clips)
        c.push_back (clip.toJson());

    return { { "name", name }, { "source_key", sourceKey }, { "color", color }, { "role_guess", roleGuess }, { "clips", c } };
}

json ImportPlan::toJson() const
{
    json t = json::array(), w = json::array();

    for (auto& x : tracks)      t.push_back (x.toJson());
    for (auto& x : warnings)    w.push_back (x.toJson());

    return { { "plan_id", planId }, { "detected_source", detectedSource }, { "tracks", t }, { "warnings", w } };
}

Result<ImportPlan> ImportPlan::fromJson (const json& j)
{
    try
    {
        ImportPlan p;
        p.planId = j.at ("plan_id");
        p.detectedSource = j.value ("detected_source", "generic");

        for (auto& t : j.at ("tracks"))
        {
            ImportTrackPlan tp;
            tp.name = t.at ("name");
            tp.sourceKey = t.value ("source_key", "");
            tp.color = t.value ("color", "");
            tp.roleGuess = t.value ("role_guess", "");

            for (auto& c : t.at ("clips"))
            {
                ImportClipPlan cp;
                cp.origName = c.value ("file", "");
                cp.sha256 = c.at ("sha256");
                cp.mediaPath = c.at ("media_path");
                cp.startS = c.value ("start_s", 0.0);
                cp.info.channels = c.value ("channels", 0);
                cp.info.sampleRate = c.value ("sample_rate", 0.0);
                cp.info.bitDepth = c.value ("bit_depth", 0);
                cp.info.frames = c.value ("frames", (std::int64_t) 0);
                tp.clips.push_back (std::move (cp));
            }

            p.tracks.push_back (std::move (tp));
        }

        for (auto& w : j.value ("warnings", json::array()))
            p.warnings.push_back ({ w.value ("code", ""), w.value ("message", ""), w.value ("file", "") });

        return p;
    }
    catch (const std::exception& e)
    {
        return makeError (errc::invalidParams, std::string ("invalid import plan: ") + e.what());
    }
}

//==============================================================================
Result<AudioInfo> probeAudio (const fs::path& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (toFile (file)));

    if (reader == nullptr)
        return makeError (errc::invalidParams, "not a readable audio file: " + file.string());

    AudioInfo info;
    info.channels = (int) reader->numChannels;
    info.sampleRate = reader->sampleRate;
    info.bitDepth = (int) reader->bitsPerSample;
    info.frames = reader->lengthInSamples;

    if (const auto ref = reader->metadataValues.getValue (juce::WavAudioFormat::bwavTimeReference, ""); ref.isNotEmpty() && info.sampleRate > 0)
        info.bwfTimeReferenceS = (double) ref.getLargeIntValue() / info.sampleRate;

    return info;
}

Result<fs::path> stageMedia (const fs::path& src, const std::string& sha256, const fs::path& mediaDir)
{
    std::error_code ec;
    fs::create_directories (mediaDir, ec);

    auto ext = src.extension().string();
    std::transform (ext.begin(), ext.end(), ext.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    const auto target = mediaDir / (sha256 + ext);

    if (fs::exists (target))
        return target;

    auto tmp = target;
    tmp += ".part";
    fs::remove (tmp, ec);
    fs::create_hard_link (src, tmp, ec);

    if (ec)
    {
        ec.clear();
        fs::copy_file (src, tmp, fs::copy_options::overwrite_existing, ec);
    }

    if (ec)
        return makeError (errc::io, "cannot copy " + src.string() + " into the project: " + ec.message());

    fs::rename (tmp, target, ec);

    if (ec)
        return makeError (errc::io, "cannot finalise media file: " + ec.message());

    return target;
}

std::vector<std::string> readR20SequenceFiles (const fs::path& r20prj)
{
    std::ifstream in (r20prj, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const auto data = ss.str();
    std::vector<std::string> names;
    const auto begin = data.find ("SEQD"), end = data.find ("SEQE");

    if (begin == std::string::npos || end == std::string::npos || end < begin)
        return names;

    std::string current;

    for (size_t i = begin + 4; i < end; ++i)
    {
        const auto c = data[i];

        if (c >= 32 && c < 127)
        {
            current += c;
        }
        else
        {
            if (current.size() > 4)
                names.push_back (current);

            current.clear();
        }
    }

    if (current.size() > 4)
        names.push_back (current);

    return names;
}

//==============================================================================
namespace
{
const char* const palette[] = { "#e5484d", "#f76b15", "#ffb224", "#46a758", "#12a594", "#0091ff", "#3e63dd", "#8e4ec6", "#d6409f", "#978365", "#6e56cf", "#00a2c7" };

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

bool isAudioExtension (const fs::path& p)
{
    const auto e = lower (p.extension().string());
    return e == ".wav" || e == ".wave" || e == ".aif" || e == ".aiff" || e == ".flac" || e == ".ogg";
}

std::string guessRole (const std::string& name)
{
    const auto n = lower (name);
    static const std::pair<const char*, const char*> table[] = {
        { "vox", "vocal" }, { "vocal", "vocal" }, { "voc", "vocal" }, { "gtr", "guitar" }, { "guitar", "guitar" }, { "bass", "bass" },
        { "flute", "flute" }, { "drum", "drums" }, { "kick", "drums" }, { "snare", "drums" }, { "hat", "drums" }, { "piano", "keys" }, { "keys", "keys" } };

    for (auto& [needle, role] : table)
        if (n.find (needle) != std::string::npos)
            return role;

    return {};
}

struct Candidate
{
    fs::path path;
    std::string stem;
};

bool isSidecar (const fs::path& p)
{
    const auto n = lower (p.filename().string());
    return n == ".ds_store" || n == "r20prjdata.r20prj" || n == "thumbs.db" || n.rfind ("._", 0) == 0;
}
} // namespace

Result<ImportPlan> planImport (const std::vector<fs::path>& sources, const fs::path& mediaDir, const ImportOptions& opts)
{
    ImportPlan plan;
    plan.planId = newId ("ipl");
    std::vector<fs::path> files;
    std::optional<fs::path> r20prj;
    std::error_code ec;

    for (auto& s : sources)
    {
        if (! fs::exists (s, ec))
            return makeError (errc::notFound, "no such file or folder: " + s.string());

        if (fs::is_directory (s, ec))
        {
            if (lower (s.extension().string()) == ".zprj" || fs::exists (s / "R20PRJDATA.r20prj"))
                plan.detectedSource = "zoom_r20";

            if (fs::exists (s / "R20PRJDATA.r20prj"))
                r20prj = s / "R20PRJDATA.r20prj";

            std::vector<fs::path> inDir;

            for (auto& e : fs::directory_iterator (s, ec))
                if (e.is_regular_file())
                    inDir.push_back (e.path());

            std::sort (inDir.begin(), inDir.end());
            files.insert (files.end(), inDir.begin(), inDir.end());
        }
        else
        {
            files.push_back (s);
        }
    }

    if (opts.preset == "zoom_r20")
        plan.detectedSource = "zoom_r20";
    else if (opts.preset == "generic")
        plan.detectedSource = "generic";

    const std::regex r20Name (R"(^REC(\d+)_(\d+)\.WAV$)", std::regex::icase);

    struct Entry { fs::path path; std::string key, stem; int track = -1, take = 0; };
    std::vector<Entry> entries;

    for (auto& f : files)
    {
        if (isSidecar (f))
            continue;

        if (! isAudioExtension (f))
        {
            plan.warnings.push_back ({ "NOT_AUDIO", "skipped, not an audio file", f.filename().string() });
            continue;
        }

        Entry e;
        e.path = f;
        e.stem = f.stem().string();
        std::smatch m;
        const auto name = f.filename().string();

        if (plan.detectedSource == "zoom_r20" && std::regex_match (name, m, r20Name))
        {
            e.track = std::stoi (m[1]);
            e.take = std::stoi (m[2]);
        }

        entries.push_back (std::move (e));
    }

    if (r20prj.has_value())
    {
        std::set<std::string> present;

        for (auto& e : entries)
            present.insert (lower (e.path.filename().string()));

        const auto listed = readR20SequenceFiles (*r20prj);
        std::set<std::string> listedLower;

        for (auto& n : listed)
        {
            if (! listedLower.insert (lower (n)).second)
                continue;           // R20PRJDATA lists a file once per use in the sequence

            if (! present.count (lower (n)))
                plan.warnings.push_back ({ "MISSING_FILE", "listed in R20PRJDATA.r20prj but not present in the folder", n });
        }

        if (! listedLower.empty())
            for (auto& e : entries)
                if (! listedLower.count (lower (e.path.filename().string())))
                    plan.warnings.push_back ({ "NOT_IN_SEQUENCE", "present in the folder but not referenced by R20PRJDATA.r20prj", e.path.filename().string() });
    }

    if (entries.empty())
        return makeError (errc::invalidParams, "no audio files to import", json { { "warnings", [&] { json w = json::array(); for (auto& x : plan.warnings) w.push_back (x.toJson()); return w; }() } });

    if (opts.takesMode == "lanes")
        plan.warnings.push_back ({ "TAKES_LANES_UNSUPPORTED", "takes_mode lanes is not implemented; importing takes as separate tracks", "" });

    // R20: group takes of one track number; other files are one track each
    std::map<int, std::vector<size_t>> byTrack;

    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].track >= 0)
            byTrack[entries[i].track].push_back (i);

    struct Group { std::string key, name; std::vector<size_t> members; };
    std::vector<Group> groups;
    std::set<size_t> used;

    // keep the order of first appearance: R20 tracks sorted by number first, then free files by name
    for (auto& [trk, members] : byTrack)
    {
        std::sort (members.begin(), members.end(), [&] (size_t a, size_t b) { return entries[a].take < entries[b].take; });

        char key[16];
        std::snprintf (key, sizeof (key), "REC%02d", trk);

        if (opts.takesMode == "latest" && members.size() > 1)
        {
            groups.push_back ({ key, key, { members.back() } });
        }
        else if (members.size() == 1 || opts.takesMode == "latest")
        {
            groups.push_back ({ key, key, members });
        }
        else
        {
            for (auto i : members)
                groups.push_back ({ std::string (key) + "_" + std::to_string (entries[i].take), std::string (key) + " take " + std::to_string (entries[i].take), { i } });
        }

        for (auto i : members)
            used.insert (i);
    }

    for (size_t i = 0; i < entries.size(); ++i)
        if (! used.count (i))
            groups.push_back ({ entries[i].stem, entries[i].stem, { i } });

    // probe, hash, stage
    std::vector<ImportTrackPlan> tracks;
    int colorIndex = 0;

    for (auto& g : groups)
    {
        ImportTrackPlan tp;
        tp.sourceKey = g.key;
        tp.name = g.name;

        if (auto it = opts.nameMap.find (g.key); it != opts.nameMap.end())
            tp.name = it->second;
        else if (auto it2 = opts.nameMap.find (g.name); it2 != opts.nameMap.end())
            tp.name = it2->second;

        tp.color = palette[colorIndex++ % (int) std::size (palette)];
        tp.roleGuess = guessRole (tp.name);

        for (auto i : g.members)
        {
            const auto& e = entries[i];
            auto info = probeAudio (e.path);

            if (! info.ok())
            {
                plan.warnings.push_back ({ "UNREADABLE_AUDIO", info.error().message, e.path.filename().string() });
                continue;
            }

            auto sha = sha256File (e.path);

            if (! sha)
                return makeError (errc::io, "cannot read " + e.path.string());

            auto staged = stageMedia (e.path, *sha, mediaDir);

            if (! staged.ok())
                return staged.error();

            ImportClipPlan cp;
            cp.origName = e.path.filename().string();
            cp.sha256 = *sha;
            cp.mediaPath = "media/" + staged.value().filename().string();
            cp.info = info.value();
            tp.clips.push_back (std::move (cp));
        }

        if (! tp.clips.empty())
            tracks.push_back (std::move (tp));
    }

    // alignment
    double minBwf = 1e300;

    for (auto& t : tracks)
        for (auto& c : t.clips)
            if (c.info.bwfTimeReferenceS.has_value())
                minBwf = std::min (minBwf, *c.info.bwfTimeReferenceS);

    for (auto& t : tracks)
        for (auto& c : t.clips)
        {
            if (opts.align == "bwf" && c.info.bwfTimeReferenceS.has_value())
                c.startS = *c.info.bwfTimeReferenceS - minBwf;
            else if (opts.align == "none" && c.info.bwfTimeReferenceS.has_value())
                c.startS = *c.info.bwfTimeReferenceS;
        }

    // sample-rate consistency
    std::set<double> rates;

    for (auto& t : tracks)
        for (auto& c : t.clips)
            rates.insert (c.info.sampleRate);

    if (rates.size() > 1)
        plan.warnings.push_back ({ "MIXED_SAMPLE_RATES", "the files have different sample rates; Tracktion resamples on playback and render", "" });

    plan.tracks = std::move (tracks);
    return plan;
}

//==============================================================================
void registerMedia (Project& p, const ImportClipPlan& c)
{
    if (p.manifest().findMedia (c.sha256) != nullptr)
        return;

    MediaEntry m;
    m.mediaId = newId ("med");
    m.path = c.mediaPath;
    m.sha256 = c.sha256;
    m.origName = c.origName;
    m.channels = c.info.channels;
    m.sampleRate = c.info.sampleRate;
    m.bitDepth = c.info.bitDepth;
    m.frames = c.info.frames;
    p.manifest().media.push_back (std::move (m));
}

json applyImportPlan (Project& p, const ImportPlan& plan, const std::map<std::string, std::string>& rename)
{
    auto& edit = p.edit();
    const bool empty = te::getAudioTracks (edit).isEmpty();

    if (empty && ! plan.tracks.empty() && ! plan.tracks[0].clips.empty())
        p.manifest().sampleRate = plan.tracks[0].clips[0].info.sampleRate;      // an empty project adopts the rate of its first file

    json tracksOut = json::array();

    for (auto& tp : plan.tracks)
    {
        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

        if (track == nullptr)
            throw std::runtime_error ("track limit reached");

        std::string name = tp.name;

        if (auto it = rename.find (tp.sourceKey); it != rename.end())
            name = it->second;
        else if (auto it2 = rename.find (tp.name); it2 != rename.end())
            name = it2->second;

        track->setName (toJuce (name));

        if (auto c = juce::Colour::fromString ("ff" + juce::String (tp.color).fromFirstOccurrenceOf ("#", false, false)); tp.color.size() == 7)
            track->setColour (c);

        json items = json::array();

        for (auto& cp : tp.clips)
        {
            registerMedia (p, cp);
            const auto file = toFile (p.dir() / cp.mediaPath);
            const auto start = te::TimePosition::fromSeconds (cp.startS);
            const auto length = te::TimeDuration::fromSeconds (cp.info.durationS());
            auto clip = te::insertWaveClip (*track, toJuce (fs::path (cp.origName).stem().string()), file, { .time = { start, length } }, te::DeleteExistingClips::no);

            if (clip == nullptr)
                throw std::runtime_error ("clip could not be created for " + cp.origName);

            ChangeCollector::assignMissingIds (clip->state);
            items.push_back (Project::idOf (clip->state));
        }

        ChangeCollector::assignMissingIds (track->state);
        tracksOut.push_back ({ { "track_id", Project::idOf (track->state) }, { "name", name }, { "item_ids", items } });
    }

    return { { "tracks", tracksOut } };
}

//==============================================================================
namespace
{
json obj (std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required = {})
{
    json properties = json::object();

    for (auto& [k, v] : props)
        properties[k] = v;

    return { { "type", "object" }, { "properties", properties }, { "required", json (std::vector<std::string> (required.begin(), required.end())) }, { "additionalProperties", false } };
}

json str()      { return { { "type", "string" } }; }
} // namespace

ImportOptions importOptionsFrom (const json& params)
{
    ImportOptions o;
    o.preset = params.value ("preset", "auto");
    o.align = params.value ("align", "start");
    o.takesMode = params.value ("takes_mode", "separate");

    if (params.contains ("name_map"))
        for (auto it = params["name_map"].begin(); it != params["name_map"].end(); ++it)
            o.nameMap[it.key()] = it.value().get<std::string>();

    return o;
}

std::vector<fs::path> importSourcesFrom (const json& params)
{
    std::vector<fs::path> sources;

    for (auto& s : params["paths"])
        sources.emplace_back (s.get<std::string>());

    return sources;
}

void registerImportCommands (CommandRegistry& r)
{
    registerRppCommands (r);

    r.add (makeProjectCommand ("import.plan",
        obj ({ { "paths", { { "type", "array" }, { "items", str() }, { "minItems", 1 } } },
               { "preset", { { "type", "string" }, { "enum", { "auto", "zoom_r20", "generic" } } } },
               { "align", { { "type", "string" }, { "enum", { "start", "bwf", "none" } } } },
               { "takes_mode", { { "type", "string" }, { "enum", { "separate", "latest", "lanes" } } } },
               { "name_map", { { "type", "object" } } } }, { "paths" }),
        obj ({ { "plan_id", str() } }), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto o = importOptionsFrom (params);
            auto sources = importSourcesFrom (params);

            auto plan = planImport (sources, p.mediaDir(), o);

            if (! plan.ok())
                return plan.error();

            auto j = plan.value().toJson();
            p.importPlans()[plan.value().planId] = j;
            return j;
        }));

    r.add (makeProjectCommand ("import.apply",
        obj ({ { "plan_id", str() }, { "rename", { { "type", "object" } } } }, { "plan_id" }),
        obj ({ { "tracks", { { "type", "array" } } } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto it = p.importPlans().find (params["plan_id"].get<std::string>());

            if (it == p.importPlans().end())
                return makeError (errc::notFound, "no import plan " + params["plan_id"].get<std::string>());

            auto plan = ImportPlan::fromJson (it->second);

            if (! plan.ok())
                return plan.error();

            std::map<std::string, std::string> rename;

            if (params.contains ("rename"))
                for (auto i = params["rename"].begin(); i != params["rename"].end(); ++i)
                {
                    if (! i.value().is_string())
                        return makeError (errc::invalidParams, "rename values must be strings", json { { "path", "/rename/" + i.key() } });

                    rename[i.key()] = i.value().get<std::string>();
                }

            // the media files are in place already; the Edit is only touched here
            for (auto& t : plan.value().tracks)
                for (auto& c : t.clips)
                    if (! fs::exists (p.dir() / c.mediaPath))
                        return makeError (errc::preconditionFailed, "staged media is missing: " + c.mediaPath);

            auto out = applyImportPlan (p, plan.value(), rename);
            out["warnings"] = plan.value().toJson()["warnings"];
            p.importPlans().erase (it);
            return out;
        }));
}

} // namespace rovox
