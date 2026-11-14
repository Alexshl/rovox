// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RecipesBuiltin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

namespace rovox::mcp
{

namespace
{
/** render_mixdown: the master as one file, or one file per loudness target in `variants_lufs` (the name gets the target as a suffix). */
Result<json> renderMixdown (RecipeRunner& run, const json& params)
{
    std::vector<std::optional<double>> targets;

    if (params.contains ("variants_lufs") && params["variants_lufs"].is_array() && ! params["variants_lufs"].empty())
    {
        for (auto& v : params["variants_lufs"])
            targets.push_back (v.get<double>());
    }
    else if (params.contains ("target_lufs") && params["target_lufs"].is_number())
    {
        targets.push_back (params["target_lufs"].get<double>());
    }
    else
    {
        targets.push_back (std::nullopt);
    }

    const bool suffixed = params.contains ("variants_lufs") && params["variants_lufs"].is_array() && ! params["variants_lufs"].empty();
    const std::string name = params.value ("name", "mixdown");
    run.setTotal (targets.size());
    json renders = json::array();

    for (auto target : targets)
    {
        json args { { "project_id", params.value ("project_id", "") }, { "scope", "master" }, { "format", { { "container", params.value ("container", "wav") } } }, { "wait_s", 60 } };
        std::string fileName = name;

        if (target)
        {
            char suffix[32];
            std::snprintf (suffix, sizeof (suffix), "%g", *target);
            args["target_lufs"] = *target;
            args["check"] = true;

            if (suffixed)
                fileName += "_" + std::string (suffix) + "lufs";
        }

        args["name"] = fileName;
        auto r = run.step ("render.start", std::move (args));

        if (! r.ok())
            return r.error();

        // a cancelled run deletes the files written so far
        if (r.value().contains ("result") && r.value()["result"].is_object() && r.value()["result"].contains ("files") && r.value()["result"]["files"].is_array())
            for (auto& f : r.value()["result"]["files"])
                if (f.is_object() && f.contains ("path") && f["path"].is_string())
                    run.trackFile (f["path"].get<std::string>());

        renders.push_back (r.value());
    }

    return json { { "renders", renders } };
}


//==============================================================================
// accompaniment.v1: parts as notes in, minus/plus mixes, a click stem, a MIDI file and a manifest out.

/** 0.1 ms precision is far below the +-1 ms the manifest promises. */
double roundMs (double ms)
{
    return std::round (ms * 1000.0) / 1000.0;
}

/** "87.5" -> "87p5": a file-name part. */
std::string pctLabel (double pct)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%g", pct);
    std::string out = buf;
    std::replace (out.begin(), out.end(), '.', 'p');
    return out;
}

/** The notes of one part as item.create_midi notes: beats of the time signature's denominator become quarter notes. */
json notesToQn (const json& part, double qnPerBeat)
{
    json out = json::array();

    for (auto& n : part["notes"])
        out.push_back ({ { "pitch", n["pitch"] }, { "start_qn", n["start_beats"].get<double>() * qnPerBeat }, { "length_qn", n["length_beats"].get<double>() * qnPerBeat },
                         { "velocity", n.value ("velocity", 100) } });

    return out;
}

double partEndBeats (const json& part)
{
    double end = 0.0;

    for (auto& n : part["notes"])
        end = std::max (end, n["start_beats"].get<double>() + n["length_beats"].get<double>());

    return end;
}

/** A finished render job: waits through job.get (a step, so a cancel stops the wait) when render.batch returned before the job finished. */
Result<json> finishedJob (RecipeRunner& run, json job)
{
    for (;;)
    {
        const auto state = job.value ("state", "");

        if (state == "succeeded")
            return job;

        if (state == "failed" || state == "cancelled" || state == "interrupted")
        {
            if (job.contains ("error") && job["error"].is_object())
                return makeError (job["error"].value ("code", errc::internal), job["error"].value ("message", "the render failed"), job["error"].value ("details", json (nullptr)));

            return makeError (state == "cancelled" ? errc::cancelled : errc::internal, "the render job " + state);
        }

        auto next = run.step ("job.get", { { "job_id", job.value ("job_id", "") }, { "wait_s", 30 } });

        if (! next.ok())
            return next.error();

        job = next.value();
    }
}

Result<json> accompaniment (RecipeRunner& run, const json& params)
{
    const int numerator = params["time_signature"].value ("numerator", 4), denominator = params["time_signature"].value ("denominator", 4);
    const double qnPerBeat = 4.0 / denominator, barQn = numerator * qnPerBeat;
    const double bpm = params["bpm"].get<double>();
    const int countInBars = params.value ("count_in_bars", 1);
    const bool embeddedClick = params.value ("click", "separate") == "embedded";
    const std::string name = params.value ("name", "accompaniment");
    const auto& parts = params["parts"];

    // everything that can be wrong with the input is found before the first step: nothing is created, nothing is written
    if (! parts.is_object() || parts.empty())
        return makeError (errc::invalidParams, "parts must hold at least the chords", json { { "path", "/parts" } });

    for (auto& [role, part] : parts.items())
        if (! part.is_object() || ! part.contains ("notes") || ! part["notes"].is_array() || part["notes"].empty())
            return makeError (errc::invalidParams, "part " + role + " needs a non-empty notes array", json { { "path", "/parts/" + role + "/notes" } });

    std::vector<double> percents;
    std::set<std::string> labels;

    for (auto& v : params["tempo_variants_pct"])
    {
        const double pct = v.get<double>();

        if (bpm * pct / 100.0 < 20.0 || bpm * pct / 100.0 > 400.0)
            return makeError (errc::invalidParams, "bpm x tempo_variants_pct must stay within 20..400 bpm", json { { "path", "/tempo_variants_pct" } });

        if (! labels.insert (pctLabel (pct)).second)
            return makeError (errc::invalidParams, "tempo_variants_pct has a value twice", json { { "path", "/tempo_variants_pct" } });

        percents.push_back (pct);
    }

    std::set<std::string> outputs;

    for (auto& o : params["outputs"])
        outputs.insert (o.get<std::string>());

    const bool wantMidi = outputs.count ("midi") > 0, wantClip = ! embeddedClick && outputs.count ("click_wav") > 0;

    double endBeats = 0.0;

    for (auto& [role, part] : parts.items())
        endBeats = std::max (endBeats, partEndBeats (part));

    const int pieceBars = std::max (1, (int) std::ceil (endBeats * qnPerBeat / barQn - 1.0e-9));
    const double pieceQn = pieceBars * barQn, countInQn = countInBars * barQn, totalQn = countInQn + pieceQn;

    const std::vector<std::pair<std::string, std::string>> roles { { "melody", "soloist" }, { "chords", "chords" }, { "bass", "bass" }, { "drums", "drums" } };
    size_t used = 0;

    for (auto& r : roles)
        used += parts.contains (r.first) ? 1 : 0;

    const size_t perVariant = 1 + used + 1 + 1 + (wantMidi ? 1 : 0) + (used + 1);       // tempo.set, part items, click item, render, (midi), deletes
    run.setTotal (4 + percents.size() * perVariant);

    auto bail = [&run] (const Error& e) -> Result<json>
    {
        run.cleanup();
        return e;
    };

    const std::string projectName = "accompaniment " + name;
    auto made = run.step ("project.new", { { "name", projectName }, { "ephemeral", true }, { "ttl_s", params.value ("ttl_s", 3600) } });

    if (! made.ok())
        return bail (made.error());

    const std::string pid = made.value().value ("project_id", "");
    auto applied = run.step ("template.apply", { { "project_id", pid }, { "template_id", params.value ("template_id", "flute-piano") } });

    if (! applied.ok())
        return bail (applied.error());

    std::map<std::string, std::string> trackOfRole;
    std::string soloistTrack;

    for (auto& t : applied.value()["tracks"])
    {
        trackOfRole[t.value ("role", "")] = t.value ("track_id", "");

        if (t.value ("soloist", false))
            soloistTrack = t.value ("track_id", "");
    }

    for (auto& r : roles)
        if (parts.contains (r.first) && trackOfRole.count (r.second) == 0)
            return bail (makeError (errc::preconditionFailed, "the template has no track with role " + r.second + " for the part " + r.first,
                                    json { { "step", run.report().size() }, { "code", errc::preconditionFailed } }));

    auto clickTrack = run.step ("track.create", { { "project_id", pid }, { "name", "Click" }, { "volume_db", -6.0 } });

    if (! clickTrack.ok())
        return bail (clickTrack.error());

    const std::string clickId = clickTrack.value().value ("track_id", "");

    if (auto fx = run.step ("fx.add", { { "project_id", pid }, { "track_id", clickId }, { "builtin", "rovox:drums" } }); ! fx.ok())
        return bail (fx.error());

    // the click: a side stick (GM 37) on every beat, louder on the first of the bar; it runs through the count-in and the piece
    json clickNotes = json::array();
    const int beatsTotal = (int) std::lround (totalQn / qnPerBeat);

    for (int i = 0; i < beatsTotal; ++i)
        clickNotes.push_back ({ { "pitch", 37 }, { "start_qn", i * qnPerBeat }, { "length_qn", std::min (0.25, qnPerBeat) }, { "velocity", i % numerator == 0 ? 120 : 84 } });

    std::vector<std::string> mixTracks;        // every track of the template, the click when it is embedded
    std::vector<std::string> minusTracks;

    for (auto& t : applied.value()["tracks"])
    {
        const auto id = t.value ("track_id", "");
        mixTracks.push_back (id);

        if (id != soloistTrack)
            minusTracks.push_back (id);
    }

    if (embeddedClick)
    {
        mixTracks.push_back (clickId);
        minusTracks.push_back (clickId);
    }

    json variantsOut = json::array(), packsOut = json::array(), attributions = json::array(), midiOut = nullptr, templateOut = nullptr;
    std::set<std::string> packKeys;

    for (size_t vi = 0; vi < percents.size(); ++vi)
    {
        const double pct = percents[vi], variantBpm = bpm * pct / 100.0;
        const std::string label = pctLabel (pct);

        if (auto t = run.step ("tempo.set", { { "project_id", pid }, { "points", json::array ({ { { "bpm", variantBpm } } }) },
                                              { "time_signature", { { "numerator", numerator }, { "denominator", denominator } } } }); ! t.ok())
            return bail (t.error());

        std::vector<std::string> items;

        for (auto& r : roles)
        {
            if (! parts.contains (r.first))
                continue;

            auto item = run.step ("item.create_midi", { { "project_id", pid }, { "track_id", trackOfRole[r.second] }, { "name", r.first }, { "start", { { "qn", countInQn } } },
                                                        { "length", { { "qn", pieceQn } } }, { "notes", notesToQn (parts[r.first], qnPerBeat) } });

            if (! item.ok())
                return bail (item.error());

            items.push_back (item.value().value ("item_id", ""));
        }

        if (wantMidi && vi == 0)        // the notes do not depend on the tempo: one file, written before the click joins the project
        {
            auto exported = run.step ("midi.export_file", { { "project_id", pid }, { "name", name + "_" + label } });

            if (! exported.ok())
                return bail (exported.error());

            midiOut = exported.value();
        }

        auto click = run.step ("item.create_midi", { { "project_id", pid }, { "track_id", clickId }, { "name", "click" }, { "length", { { "qn", totalQn } } }, { "notes", clickNotes } });

        if (! click.ok())
            return bail (click.error());

        items.push_back (click.value().value ("item_id", ""));

        // the variants of one render.batch: [minus mp3/wav, plus mp3/wav, click wav]
        json variants = json::array();
        std::vector<std::string> roleOfVariant;

        auto addVariant = [&] (const std::string& kind, const std::string& container, const json& trackIds, const std::string& output)
        {
            if (outputs.count (output) == 0)
                return;

            json format { { "container", container } };

            if (container == "mp3")
                format["bitrate_kbps"] = 192;

            variants.push_back ({ { "name", name + "_" + kind + "_" + label }, { "scope", "tracks" }, { "track_ids", trackIds }, { "format", format } });
            roleOfVariant.push_back (kind + "_" + container);
        };

        addVariant ("minus", "mp3", minusTracks, "minus_mp3");
        addVariant ("minus", "wav", minusTracks, "minus_wav");
        addVariant ("plus", "mp3", mixTracks, "plus_mp3");
        addVariant ("plus", "wav", mixTracks, "plus_wav");

        if (wantClip)
            addVariant ("click", "wav", json::array ({ clickId }), "click_wav");

        json variantOut { { "tempo_pct", pct }, { "bpm", variantBpm }, { "intro_offset_ms", roundMs (countInQn * 60000.0 / variantBpm) }, { "files", json::object() } };

        if (! variants.empty())
        {
            json args { { "project_id", pid }, { "variants", variants }, { "wait_s", 60 } };

            if (params.contains ("seed") && ! params["seed"].is_null())
                args["seed"] = params["seed"].is_string() ? params["seed"] : json (params["seed"].dump());

            auto started = run.step ("render.batch", std::move (args));

            if (! started.ok())
                return bail (started.error());

            auto done = finishedJob (run, started.value());

            if (! done.ok())
                return bail (makeError (done.error().code, done.error().message, json { { "step", run.report().size() - 1 }, { "code", done.error().code } }));

            const auto& result = done.value()["result"];

            for (auto& v : result["variants"])
            {
                if (v.value ("status", "") != "succeeded")
                {
                    const auto code = v["error"].value ("code", errc::internal);
                    return bail (makeError (code, "render of " + v.value ("name", "") + " failed: " + v["error"].value ("message", ""), json { { "step", run.report().size() - 1 }, { "code", code } }));
                }

                if (v.contains ("metadata") && v["metadata"].is_object())
                {
                    if (templateOut.is_null() && v["metadata"].contains ("template"))
                        templateOut = v["metadata"]["template"];

                    for (auto& pack : v["metadata"].value ("instrument_packs", json::array()))
                        if (packKeys.insert (pack.value ("id", "") + "|" + pack.value ("version", "") + "|" + pack.value ("sha256", "")).second)
                        {
                            packsOut.push_back (pack);
                            attributions.push_back ({ { "pack", pack.value ("id", "") }, { "license", pack.value ("license", json (nullptr)) }, { "source", pack.value ("source", json (nullptr)) } });
                        }
                }
            }

            for (auto f : result["files"])
            {
                const int index = f.value ("variant", -1);

                if (index < 0 || (size_t) index >= roleOfVariant.size())
                    continue;

                f.erase ("abs_path");
                f.erase ("api_url");
                variantOut["files"][roleOfVariant[(size_t) index]] = std::move (f);
            }
        }

        // the grid of the tempo: every beat of the count-in and the piece
        json beats = json::array();

        for (int i = 0; i < beatsTotal; ++i)
            beats.push_back ({ { "t_ms", roundMs (i * qnPerBeat * 60000.0 / variantBpm) }, { "bar", i / numerator + 1 }, { "beat", i % numerator + 1 }, { "count_in", i < countInBars * numerator } });

        variantOut["beats"] = std::move (beats);
        variantsOut.push_back (std::move (variantOut));

        if (vi + 1 < percents.size())
            for (auto& id : items)
                if (auto del = run.step ("item.delete", { { "project_id", pid }, { "item_id", id } }); ! del.ok())
                    return bail (del.error());
    }

    return json { { "recipe", "accompaniment.v1" }, { "template", templateOut }, { "key", params.value ("key", "") }, { "bpm", bpm },
                  { "time_signature", { { "numerator", numerator }, { "denominator", denominator } } }, { "count_in_bars", countInBars },
                  { "bars", pieceBars }, { "click", embeddedClick ? "embedded" : "separate" }, { "intro_offset_ms", variantsOut[0]["intro_offset_ms"] },
                  { "variants", variantsOut }, { "midi", midiOut }, { "instrument_packs", packsOut }, { "attributions", attributions },
                  { "ephemeral_project_id", pid } };
}

json accompanimentNotes (const char* what)
{
    return { { "type", "object" }, { "description", what }, { "required", { "notes" } },
             { "properties", { { "notes", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 20000 },
                                            { "items", { { "type", "object" }, { "required", { "pitch", "start_beats", "length_beats" } },
                                                         { "properties", { { "pitch", { { "type", { "integer", "string" } }, { "description", "0..127 or a name such as C4" } } },
                                                                           { "start_beats", { { "type", "number" }, { "minimum", 0 } } },
                                                                           { "length_beats", { { "type", "number" }, { "minimum", 0.001 } } },
                                                                           { "velocity", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 127 } } } } } } } } } } } };
}

//==============================================================================
// eartraining.*.v1: N short seeded exercises in one ephemeral project, one render.batch of clip_### files, and the answers as the manifest.

constexpr std::uint64_t splitGolden = 0x9E3779B97F4A7C15ull;

std::uint64_t mix64 (std::uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint64_t fnv1a64 (const std::string& text)
{
    std::uint64_t h = 0xCBF29CE484222325ull;

    for (unsigned char c : text)
        h = (h ^ c) * 0x100000001B3ull;

    return h;
}

/** splitmix64: the same seed gives the same numbers on every platform (the standard distributions do not promise that). */
struct Rng
{
    explicit Rng (std::uint64_t s) : state (s) {}

    std::uint64_t next()
    {
        state += splitGolden;
        return mix64 (state);
    }

    /** An integer in [lo, hi]. */
    int range (int lo, int hi)    { return lo + (int) (next() % (std::uint64_t) (hi - lo + 1)); }
    size_t index (size_t n)       { return (size_t) (next() % (std::uint64_t) n); }

    std::uint64_t state;
};

std::uint64_t seedValue (const json& seed)
{
    if (seed.is_number_integer())
        return (std::uint64_t) seed.get<std::int64_t>();

    if (seed.is_string())
        return fnv1a64 (seed.get<std::string>());

    return 0;
}

std::string noteName (int midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string (names[((midi % 12) + 12) % 12]) + std::to_string (midi / 12 - 1);
}

struct EarNote
{
    int pitch;
    double startS, lengthS;
};

struct EarClip
{
    std::vector<EarNote> notes;
    double contentS = 0.0;
    json answer;
};

const std::map<std::string, int>& intervalNames()
{
    static const std::map<std::string, int> m { { "m2", 1 }, { "M2", 2 }, { "m3", 3 }, { "M3", 4 }, { "P4", 5 }, { "TT", 6 }, { "P5", 7 }, { "m6", 8 }, { "M6", 9 }, { "m7", 10 }, { "M7", 11 }, { "P8", 12 } };
    return m;
}

std::string intervalLabel (int semitones)
{
    for (auto& [name, v] : intervalNames())
        if (v == semitones)
            return name;

    return std::to_string (semitones) + " semitones";
}

const std::map<std::string, std::vector<int>>& chordShapes()
{
    static const std::map<std::string, std::vector<int>> m { { "major", { 0, 4, 7 } }, { "minor", { 0, 3, 7 } }, { "dim", { 0, 3, 6 } }, { "aug", { 0, 4, 8 } }, { "sus4", { 0, 5, 7 } },
                                                              { "maj7", { 0, 4, 7, 11 } }, { "dom7", { 0, 4, 7, 10 } }, { "min7", { 0, 3, 7, 10 } } };
    return m;
}

struct CadenceDef { std::vector<std::vector<int>> chords; };

const std::map<std::string, CadenceDef>& cadenceDefs()
{
    // semitone offsets from the tonic: I, then the cadence; low voicings for IV and V keep every chord close to the tonic chord
    static const std::vector<int> I { 0, 4, 7 }, IV { -7, -3, 0 }, V { -5, -1, 2 }, vi { -3, 0, 4 };
    static const std::map<std::string, CadenceDef> m { { "authentic", { { I, IV, V, I } } }, { "plagal", { { I, V, IV, I } } }, { "half", { { I, IV, V } } }, { "deceptive", { { I, IV, V, vi } } } };
    return m;
}

constexpr double earLeadS = 0.15, earTailS = 0.6, earSlotGapS = 2.0, earMaxClipS = 15.0;

/** `kind`: intervals | chords | cadences | dictation. Returns an error for an unusable spec; nothing has been made yet. */
Result<std::vector<EarClip>> makeEarClips (const std::string& kind, const json& params, std::uint64_t seed)
{
    const int count = params["count"].get<int>(), low = params["range"].value ("low", 48), high = params["range"].value ("high", 84);
    const double bpm = params["tempo"].get<double>(), beatS = 60.0 / bpm;
    const std::string direction = params.value ("direction", "asc");
    const auto& spec = params["items_spec"];

    auto bad = [] (const std::string& m, const std::string& path) { return makeError (errc::invalidParams, m, json { { "path", path } }); };

    if (low >= high || low < 0 || high > 127)
        return bad ("range needs 0 <= low < high <= 127", "/range");

    std::vector<std::string> names;
    std::vector<int> numbers;

    for (auto& v : spec)
    {
        if (kind == "intervals")
        {
            int semis = 0;

            if (v.is_number_integer())
                semis = v.get<int>();
            else if (v.is_string() && intervalNames().count (v.get<std::string>()))
                semis = intervalNames().at (v.get<std::string>());

            if (semis < 1 || semis > 24)
                return bad ("an interval is 1..24 semitones or a name such as M3", "/items_spec");

            numbers.push_back (semis);
        }
        else if (kind == "dictation")
        {
            if (! v.is_number_integer() || v.get<int>() < 1 || v.get<int>() > 8)
                return bad ("a dictation item is a scale degree 1..8", "/items_spec");

            numbers.push_back (v.get<int>());
        }
        else
        {
            const auto n = v.is_string() ? v.get<std::string>() : std::string();

            if (kind == "chords" ? chordShapes().count (n) == 0 : cadenceDefs().count (n) == 0)
                return bad ("unknown item " + v.dump() + " in items_spec", "/items_spec");

            names.push_back (n);
        }
    }

    if (names.empty() && numbers.empty())
        return bad ("items_spec must not be empty", "/items_spec");

    std::vector<EarClip> clips;

    for (int i = 0; i < count; ++i)
    {
        Rng rng (mix64 (mix64 (seed) ^ ((std::uint64_t) (i + 1) * splitGolden)));
        const auto itemSeed = mix64 (mix64 (seed) ^ ((std::uint64_t) (i + 1) * splitGolden));
        EarClip clip;
        json answer { { "index", i }, { "file", "clip_" + std::string (i < 9 ? "00" : (i < 99 ? "0" : "")) + std::to_string (i + 1) }, { "item_seed", std::to_string (itemSeed) }, { "tags", { kind } } };

        if (kind == "intervals" || kind == "chords")
        {
            const bool isInterval = kind == "intervals";
            std::vector<int> shape;
            std::string label;

            if (isInterval)
            {
                const int semis = numbers[rng.index (numbers.size())];
                shape = { 0, semis };
                label = intervalLabel (semis);
                answer["interval_semitones"] = semis;
            }
            else
            {
                label = names[rng.index (names.size())];
                shape = chordShapes().at (label);
                answer["chord"] = label;
            }

            const int top = shape.back();

            if (high - low < top)
                return bad ("range is narrower than the widest item", "/range");

            const int root = rng.range (low, high - top);
            std::vector<int> pitches;

            for (int off : shape)
                pitches.push_back (root + off);

            if (direction == "desc")
                std::reverse (pitches.begin(), pitches.end());

            if (direction == "harmonic")
                for (int p : pitches)
                    clip.notes.push_back ({ p, earLeadS, 2.0 });
            else
                for (size_t k = 0; k < pitches.size(); ++k)
                    clip.notes.push_back ({ pitches[k], earLeadS + (double) k * 1.0, 1.0 });

            clip.contentS = earLeadS + (direction == "harmonic" ? 2.0 : (double) pitches.size());
            answer["label"] = label;
            answer["direction"] = direction;
            answer["root"] = root;
            answer["notes"] = pitches;
            answer["note_names"] = json::array();

            for (int p : pitches)
                answer["note_names"].push_back (noteName (p));

            answer["tags"].push_back (direction);
        }
        else if (kind == "cadences")
        {
            const auto label = names[rng.index (names.size())];
            const auto& def = cadenceDefs().at (label);

            if (high - low < 16 + 7)
                return bad ("range needs at least 23 semitones for a cadence", "/range");

            const int tonic = rng.range (low + 7, high - 16);
            double at = earLeadS;
            json chords = json::array();

            for (auto& chord : def.chords)
            {
                json pitches = json::array();

                for (int off : chord)
                {
                    clip.notes.push_back ({ tonic + off, at, 1.2 });
                    pitches.push_back (tonic + off);
                }

                chords.push_back (pitches);
                at += 1.4;
            }

            clip.contentS = at;
            answer["cadence"] = label;
            answer["label"] = label;
            answer["tonic"] = tonic;
            answer["tonic_name"] = noteName (tonic);
            answer["chords"] = chords;
        }
        else
        {
            const int bars = params.value ("bars", 2);
            static const std::vector<std::vector<double>> cells { { 4 }, { 2, 2 }, { 1, 1, 1, 1 }, { 2, 1, 1 }, { 1, 1, 2 }, { 1.5, 0.5, 2 }, { 1, 2, 1 } };
            static const int major[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
            const int tonic = rng.range (low, std::max (low, high - 12));
            double beat = 0.0;
            json notes = json::array();

            for (int b = 0; b < bars; ++b)
                for (double len : cells[rng.index (cells.size())])
                {
                    const int degree = numbers[rng.index (numbers.size())], pitch = tonic + major[degree - 1];
                    clip.notes.push_back ({ pitch, earLeadS + beat * beatS, len * beatS * 0.95 });
                    notes.push_back ({ { "degree", degree }, { "pitch", pitch }, { "name", noteName (pitch) }, { "start_beats", beat }, { "length_beats", len } });
                    beat += len;
                }

            clip.contentS = earLeadS + beat * beatS;
            answer["bars"] = bars;
            answer["tonic"] = tonic;
            answer["tonic_name"] = noteName (tonic);
            answer["notes"] = notes;
        }

        if (clip.contentS + earTailS > earMaxClipS)
            return bad ("a clip would be longer than 15 s: raise the tempo or shorten the items", "/tempo");

        clip.answer = std::move (answer);
        clips.push_back (std::move (clip));
    }

    return clips;
}

Result<json> earTraining (const std::string& kind, RecipeRunner& run, const json& params)
{
    const double bpm = params["tempo"].get<double>(), qnPerS = bpm / 60.0;
    const std::string format = params.value ("format", "mp3");
    auto made = makeEarClips (kind, params, seedValue (params.value ("seed", json (0))));

    if (! made.ok())
        return made.error();

    auto clips = std::move (made.value());
    run.setTotal (5);

    auto bail = [&run] (const Error& e) -> Result<json>
    {
        run.cleanup();
        return e;
    };

    auto project = run.step ("project.new", { { "name", "ear-training " + params.value ("name", kind) }, { "ephemeral", true }, { "ttl_s", params.value ("ttl_s", 3600) } });

    if (! project.ok())
        return bail (project.error());

    const std::string pid = project.value().value ("project_id", "");
    auto applied = run.step ("template.apply", { { "project_id", pid }, { "template_id", params.value ("template_id", "ear-training") } });

    if (! applied.ok())
        return bail (applied.error());

    std::string trackId;

    for (auto& t : applied.value()["tracks"])
        if (t.value ("soloist", false) || trackId.empty())
            trackId = t.value ("track_id", "");

    if (trackId.empty())
        return bail (makeError (errc::preconditionFailed, "the template has no track to play the exercises"));

    if (auto t = run.step ("tempo.set", { { "project_id", pid }, { "points", json::array ({ { { "bpm", bpm } } }) }, { "time_signature", { { "numerator", 4 }, { "denominator", 4 } } } }); ! t.ok())
        return bail (t.error());

    // every exercise sits in its own slot of the one timeline; the slot gap swallows the release of the previous clip
    json notes = json::array(), variants = json::array();
    double slotS = 0.0;

    for (auto& clip : clips)
    {
        const double clipS = clip.contentS + earTailS;

        for (auto& n : clip.notes)
            notes.push_back ({ { "pitch", n.pitch }, { "start_qn", (slotS + n.startS) * qnPerS }, { "length_qn", n.lengthS * qnPerS }, { "velocity", 90 } });

        json variant { { "name", clip.answer["file"] }, { "range", { { "start", { { "qn", slotS * qnPerS } } }, { "end", { { "qn", (slotS + clipS) * qnPerS } } } } } };
        variants.push_back (std::move (variant));
        slotS += clipS + earSlotGapS;
    }

    if (auto item = run.step ("item.create_midi", { { "project_id", pid }, { "track_id", trackId }, { "name", "exercises" }, { "start", { { "qn", 0 } } },
                                                    { "length", { { "qn", slotS * qnPerS } } }, { "notes", notes } }); ! item.ok())
        return bail (item.error());

    json format_ { { "container", format } };

    if (format == "mp3")
        format_["bitrate_kbps"] = 192;

    json args { { "project_id", pid }, { "variants", variants }, { "scope", "master" }, { "format", format_ }, { "target_lufs", params.value ("target_lufs", -16.0) }, { "wait_s", 60 } };

    if (params.contains ("seed") && ! params["seed"].is_null())
        args["seed"] = params["seed"].is_string() ? params["seed"] : json (params["seed"].dump());

    auto started = run.step ("render.batch", std::move (args));

    if (! started.ok())
        return bail (started.error());

    auto done = finishedJob (run, started.value());

    if (! done.ok())
        return bail (makeError (done.error().code, done.error().message, json { { "step", run.report().size() - 1 }, { "code", done.error().code } }));

    json clipsOut = json::array();
    const auto& result = done.value()["result"];

    for (auto& v : result["variants"])
    {
        if (v.value ("status", "") != "succeeded")
        {
            const auto code = v["error"].value ("code", errc::internal);
            return bail (makeError (code, "render of " + v.value ("name", "") + " failed: " + v["error"].value ("message", ""), json { { "step", run.report().size() - 1 }, { "code", code } }));
        }
    }

    for (auto f : result["files"])
    {
        const int index = f.value ("variant", -1);
        f.erase ("abs_path");
        f.erase ("api_url");

        if (index >= 0 && (size_t) index < result["variants"].size() && result["variants"][(size_t) index].contains ("loudness"))
            f["loudness"] = result["variants"][(size_t) index]["loudness"];

        clipsOut.push_back (std::move (f));
    }

    json answers = json::array();

    for (auto& clip : clips)
        answers.push_back (clip.answer);

    return json { { "recipe", "eartraining." + kind + ".v1" }, { "seed", params.value ("seed", json (0)) }, { "count", (int) clips.size() }, { "tempo", bpm }, { "target_lufs", params.value ("target_lufs", -16.0) },
                  { "clips", clipsOut }, { "answers", answers }, { "ephemeral_project_id", pid } };
}

ProceduralRecipe earTrainingRecipe (const std::string& kind, const std::string& title, const std::string& itemsHelp, const json& itemsDefault, const json& itemSchema, bool hasDirection)
{
    ProceduralRecipe r;
    json props {
        { "count", { { "type", "integer" }, { "minimum", 1 }, { "maximum", (int) 50 }, { "default", 10 } } },
        { "items_spec", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 64 }, { "items", itemSchema }, { "description", itemsHelp }, { "default", itemsDefault } } },
        { "range", { { "type", "object" }, { "description", "MIDI pitch limits of the exercises" },
                     { "properties", { { "low", { { "type", "integer" }, { "minimum", 0 }, { "maximum", 126 } } }, { "high", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 127 } } } } }, { "additionalProperties", false } } },
        { "tempo", { { "type", "number" }, { "minimum", 30 }, { "maximum", 240 }, { "default", 80 } } },
        { "template_id", { { "type", "string" }, { "default", "ear-training" }, { "description", "the instrument: a template with a soloist track" } } },
        { "seed", { { "type", { "string", "integer" } }, { "description", "the same seed gives the same exercises and the same files" } } },
        { "format", { { "type", "string" }, { "enum", { "mp3", "wav" } }, { "default", "mp3" } } },
        { "target_lufs", { { "type", "number" }, { "minimum", -40 }, { "maximum", -6 }, { "default", -16 } } },
        { "name", { { "type", "string" }, { "minLength", 1 }, { "maxLength", 60 } } },
        { "ttl_s", { { "type", "integer" }, { "minimum", 60 }, { "maximum", 2592000 }, { "default", 3600 } } } };
    json defaults { { "count", 10 }, { "items_spec", itemsDefault }, { "range", { { "low", 48 }, { "high", 84 } } }, { "tempo", 80 }, { "template_id", "ear-training" }, { "seed", 0 }, { "format", "mp3" }, { "target_lufs", -16 }, { "ttl_s", 3600 } };

    if (hasDirection)
    {
        props["direction"] = { { "type", "string" }, { "enum", { "asc", "desc", "harmonic" } }, { "default", "asc" } };
        defaults["direction"] = "asc";
    }

    if (kind == "dictation")
    {
        props["bars"] = { { "type", "integer" }, { "minimum", 2 }, { "maximum", 4 }, { "default", 2 } };
        defaults["bars"] = 2;
    }

    r.definition = {
        { "id", "eartraining." + kind + ".v1" }, { "builtin", true }, { "procedural", true }, { "name", title },
        { "description", "Ear-training clips: count short exercises (each at most 15 s, a 150 ms silence first, loudness-normalized) from items_spec, chosen by a splitmix64 generator "
                         "seeded from seed (item_seed per clip), in one ephemeral project and one render.batch (clip_001.. with signed urls, count at most 50). The result holds the "
                         "answers[] (the correct answers, tags, item_seed) apart from the clips: nothing about the answer is written into the audio or its metadata. "
                         "The batch is admitted against the tenant's quota as a whole (QUOTA_EXCEEDED creates nothing). " + itemsHelp },
        { "params", { { "type", "object" }, { "properties", props } } },
        { "defaults", defaults },
        { "steps", json::array() } };
    r.run = [kind] (RecipeRunner& run, const json& params)
    {
        auto p = params;

        if (! p.contains ("range") || ! p["range"].is_object())
            p["range"] = json::object();

        if (! p["range"].contains ("low"))
            p["range"]["low"] = 48;

        if (! p["range"].contains ("high"))
            p["range"]["high"] = 84;

        return earTraining (kind, run, p);
    };
    return r;
}

std::vector<ProceduralRecipe> makeRecipes()
{
    std::vector<ProceduralRecipe> out;

    ProceduralRecipe mixdown;
    mixdown.definition = {
        { "id", "render_mixdown" }, { "builtin", true }, { "procedural", true }, { "name", "Render the master mixdown" },
        { "description", "Renders the master to one file, or to one file per loudness target in variants_lufs (name_<target>lufs). One render step each, with progress; cancelling removes the files already written." },
        { "params", { { "type", "object" }, { "required", { "project_id" } },
                      { "properties", { { "project_id", { { "type", "string" } } }, { "name", { { "type", "string" }, { "default", "mixdown" } } },
                                        { "container", { { "type", "string" }, { "enum", { "mp3", "wav", "flac" } }, { "default", "wav" } } },
                                        { "target_lufs", { { "type", "number" } } },
                                        { "variants_lufs", { { "type", "array" }, { "items", { { "type", "number" } } } } } } } } },
        { "defaults", { { "name", "mixdown" }, { "container", "wav" } } },
        { "steps", json::array() } };
    mixdown.run = renderMixdown;
    out.push_back (std::move (mixdown));

    ProceduralRecipe accompany;
    accompany.definition = {
        { "id", "accompaniment.v1" }, { "builtin", true }, { "procedural", true }, { "name", "Accompaniment (minus-one and play-along files)" },
        { "description", "From parts given as notes (melody, bass, chords, drums; beats of the time signature) builds an ephemeral project from a template (default flute-piano), "
                         "adds a count-in and a click, and renders per tempo variant the minus-one mix (the soloist muted), the plus mix and a click stem, plus a MIDI file. "
                         "bpm is in quarter notes; beats are of the denominator of time_signature. The result is the manifest: per variant intro_offset_ms (count-in length), "
                         "beats[] of the tempo grid and the files with signed urls; instrument_packs[], attributions[] and the template come from the render metadata. "
                         "click=separate keeps the click out of the mixes (click_wav); embedded mixes it into minus and plus. The project removes itself after ttl_s. "
                         "The notes come from the caller (the accompany sidecar); this recipe makes no music." },
        { "params", { { "type", "object" }, { "required", { "bpm", "parts" } },
                      { "properties", { { "template_id", { { "type", "string" }, { "default", "flute-piano" } } },
                                        { "name", { { "type", "string" }, { "minLength", 1 }, { "maxLength", 60 }, { "default", "accompaniment" } } },
                                        { "key", { { "type", "string" }, { "description", "for the manifest only, e.g. D minor" } } },
                                        { "bpm", { { "type", "number" }, { "minimum", 20 }, { "maximum", 400 } } },
                                        { "time_signature", { { "type", "object" }, { "required", { "numerator", "denominator" } },
                                                              { "properties", { { "numerator", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 32 } } },
                                                                                { "denominator", { { "type", "integer" }, { "enum", { 2, 4, 8, 16 } } } } } } } },
                                        { "count_in_bars", { { "type", "integer" }, { "minimum", 0 }, { "maximum", 8 }, { "default", 1 } } },
                                        { "click", { { "type", "string" }, { "enum", { "separate", "embedded" } }, { "default", "separate" } } },
                                        { "tempo_variants_pct", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 8 }, { "items", { { "type", "number" }, { "minimum", 25 }, { "maximum", 300 } } }, { "default", { 100 } } } },
                                        { "parts", { { "type", "object" }, { "required", { "chords" } },
                                                     { "properties", { { "melody", accompanimentNotes ("the soloist: muted in the minus mix") }, { "chords", accompanimentNotes ("required") },
                                                                       { "bass", accompanimentNotes ("") }, { "drums", accompanimentNotes ("GM drum map, played by rovox:drums") } } } } },
                                        { "seed", { { "type", { "string", "integer" } } } },
                                        { "outputs", { { "type", "array" }, { "minItems", 1 },
                                                       { "items", { { "type", "string" }, { "enum", { "minus_mp3", "minus_wav", "plus_mp3", "plus_wav", "click_wav", "midi" } } } },
                                                       { "default", { "minus_mp3", "plus_mp3", "click_wav", "midi" } } } },
                                        { "ttl_s", { { "type", "integer" }, { "minimum", 60 }, { "maximum", 2592000 }, { "default", 3600 } } } } } } },
        { "defaults", { { "template_id", "flute-piano" }, { "name", "accompaniment" }, { "time_signature", { { "numerator", 4 }, { "denominator", 4 } } }, { "count_in_bars", 1 },
                        { "click", "separate" }, { "tempo_variants_pct", { 100 } }, { "outputs", { "minus_mp3", "plus_mp3", "click_wav", "midi" } }, { "ttl_s", 3600 } } },
        { "steps", json::array() } };
    accompany.run = accompaniment;
    out.push_back (std::move (accompany));

    out.push_back (earTrainingRecipe ("intervals", "Ear training: intervals", "items_spec: the intervals to ask, semitones 1..24 or names (m2..P8). direction: asc, desc or harmonic.",
                                      json::array ({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }), { { "type", { "integer", "string" } } }, true));
    out.push_back (earTrainingRecipe ("chords", "Ear training: chords", "items_spec: chord qualities (major, minor, dim, aug, sus4, maj7, dom7, min7). direction: arpeggio up (asc), down (desc) or block (harmonic).",
                                      json::array ({ "major", "minor", "dim", "aug" }), { { "type", "string" }, { "enum", { "major", "minor", "dim", "aug", "sus4", "maj7", "dom7", "min7" } } }, true));
    out.push_back (earTrainingRecipe ("cadences", "Ear training: cadences", "items_spec: authentic, plagal, half, deceptive (a tonic chord first, then the cadence in a major key).",
                                      json::array ({ "authentic", "plagal", "half", "deceptive" }), { { "type", "string" }, { "enum", { "authentic", "plagal", "half", "deceptive" } } }, false));
    out.push_back (earTrainingRecipe ("dictation", "Ear training: melodic and rhythmic dictation", "items_spec: the scale degrees (1..8) a melody may use; bars 2..4 of 4/4 from a few rhythm cells, in a major key.",
                                      json::array ({ 1, 2, 3, 4, 5 }), { { "type", "integer" }, { "minimum", 1 }, { "maximum", 8 } }, false));
    return out;
}
} // namespace

const std::vector<ProceduralRecipe>& proceduralRecipes()
{
    static const std::vector<ProceduralRecipe> recipes = makeRecipes();
    return recipes;
}

const ProceduralRecipe* findProceduralRecipe (const std::string& id)
{
    for (auto& r : proceduralRecipes())
        if (r.definition["id"] == id)
            return &r;

    return nullptr;
}

} // namespace rovox::mcp
