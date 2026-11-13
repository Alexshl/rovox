// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RecipesBuiltin.h"

#include <algorithm>
#include <cmath>
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
