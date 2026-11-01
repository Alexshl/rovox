// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// M4 commands (task 210 reserved names): item.create_midi, item.duplicate, midi.get / write / edit / transpose / quantize / import_file.
// Note times are quarter notes relative to the start of the item (960 ticks per quarter elsewhere); pitches are numbers (60 = C4) or names ("C4", "F#2").
// Writing MIDI is what lets an agent play the user's instruments: the notes go through whatever instrument plugin sits on the track.
#include "Command.h"

#include "Ids.h"
#include "ProjectManager.h"
#include "TimePos.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace rovox
{

namespace
{
json obj (std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required = {})
{
    json properties = json::object();

    for (auto& [k, v] : props)
        properties[k] = v;

    return { { "type", "object" }, { "properties", properties }, { "required", json (std::vector<std::string> (required.begin(), required.end())) },
             { "additionalProperties", false } };
}

json str()                          { return { { "type", "string" } }; }
json strMin (int n)                 { return { { "type", "string" }, { "minLength", n } }; }
json num()                          { return { { "type", "number" } }; }
json numMin (double lo)             { return { { "type", "number" }, { "minimum", lo } }; }
json boolean()                      { return { { "type", "boolean" } }; }
json anyObject()                    { return { { "type", "object" } }; }
json arrayOf (json items)           { return { { "type", "array" }, { "items", std::move (items) } }; }

json noteSchema()
{
    return obj ({ { "pitch", { { "description", "MIDI note number 0..127 or a name such as \"C4\" (60), \"F#2\", \"Bb3\"" } } }, { "start_qn", numMin (0) }, { "length_qn", numMin (0.001) },
                  { "velocity", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 127 } } } },
                { "pitch", "start_qn", "length_qn" });
}

const juce::Identifier noteIdProp ("rovox:id");

std::optional<int> parsePitch (const json& v)
{
    if (v.is_number_integer())
    {
        const int n = v.get<int>();
        return n >= 0 && n <= 127 ? std::optional<int> (n) : std::nullopt;
    }

    if (! v.is_string())
        return std::nullopt;

    const auto s = v.get<std::string>();

    if (s.size() < 2)
        return std::nullopt;

    static const int semitone[7] = { 9, 11, 0, 2, 4, 5, 7 };       // A B C D E F G
    const char letter = (char) std::toupper ((unsigned char) s[0]);

    if (letter < 'A' || letter > 'G')
        return std::nullopt;

    int n = semitone[letter - 'A'];
    size_t i = 1;

    while (i < s.size() && (s[i] == '#' || s[i] == 'b'))
        n += s[i++] == '#' ? 1 : -1;

    char* end = nullptr;
    const long octave = std::strtol (s.c_str() + i, &end, 10);

    if (end == s.c_str() + i || *end != 0)
        return std::nullopt;

    n += (int) (octave + 1) * 12;
    return n >= 0 && n <= 127 ? std::optional<int> (n) : std::nullopt;
}

std::string pitchName (int n)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string (names[n % 12]) + std::to_string (n / 12 - 1);
}

te::MidiClip* midiClip (Project& p, const std::string& id, Error& error)
{
    auto* clip = p.findClip (id);

    if (clip == nullptr)
    {
        error = makeError (errc::notFound, "no item " + id, json { { "item_id", id } });
        return nullptr;
    }

    auto* mc = dynamic_cast<te::MidiClip*> (clip);

    if (mc == nullptr)
        error = makeError (errc::invalidParams, "item " + id + " is an audio item; MIDI tools need a MIDI item (item.create_midi)", json { { "item_id", id } });

    return mc;
}

std::string noteId (te::MidiNote& n)
{
    if (! n.state.hasProperty (noteIdProp))
        n.state.setProperty (noteIdProp, juce::String (newId ("nt")), nullptr);       // an id is not a content change: no undo entry

    return n.state[noteIdProp].toString().toStdString();
}

te::MidiNote* findNote (te::MidiClip& clip, const std::string& id)
{
    for (auto* n : clip.getSequence().getNotes())
        if (noteId (*n) == id)
            return n;

    return nullptr;
}

json noteJson (te::MidiNote& n)
{
    return { { "note_id", noteId (n) }, { "pitch", n.getNoteNumber() }, { "name", pitchName (n.getNoteNumber()) }, { "start_qn", n.getStartBeat().inBeats() },
             { "length_qn", n.getLengthBeats().inBeats() }, { "velocity", n.getVelocity() } };
}

double beatsPerBar (Project& p, double atSeconds)
{
    return p.edit().tempoSequence.getTimeSigAt (te::TimePosition::fromSeconds (atSeconds)).numerator.get();
}

/** Length of the item in quarter notes. */
double clipLengthQn (Project& p, te::Clip& c)
{
    auto& ts = p.edit().tempoSequence;
    return ts.toBeats (c.getPosition().getEnd()).inBeats() - ts.toBeats (c.getPosition().getStart()).inBeats();
}

void setClipLengthQn (Project& p, te::Clip& c, double qn)
{
    auto& ts = p.edit().tempoSequence;
    const auto startBeats = ts.toBeats (c.getPosition().getStart());
    c.setLength (te::TimeDuration::fromSeconds (ts.toTime (startBeats + te::BeatDuration::fromBeats (qn)).inSeconds() - c.getPosition().getStart().inSeconds()), true);
}

json itemJson (Project& p, te::MidiClip& c)
{
    const double start = c.getPosition().getStart().inSeconds();
    json j { { "item_id", Project::idOf (c.state) }, { "kind", "midi" }, { "name", toStd (c.getName()) }, { "start_s", start }, { "length_s", c.getPosition().getLength().inSeconds() },
             { "start_bar_beat", barBeatString (p.edit(), start) }, { "length_qn", clipLengthQn (p, c) }, { "notes", c.getSequence().getNumNotes() } };

    if (auto* t = c.getClipTrack())
    {
        j["track_id"] = Project::idOf (t->state);
        bool instrument = false;

        for (auto* pl : t->pluginList.getPlugins())
            instrument = instrument || (pl->isSynth() && pl->isEnabled());

        if (! instrument)
            j["warning"] = "the track has no instrument plugin: these notes are silent until you add one (fx.add with a plugin_uid from plugin.catalog kind=instrument)";
    }

    return j;
}

/** Validates the notes first, adds them all after; so a bad note leaves the item untouched. */
Result<std::vector<std::array<double, 4>>> parseNotes (const json& notes, const std::string& path)
{
    std::vector<std::array<double, 4>> out;       // pitch, start, length, velocity

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        const auto where = path + "/" + std::to_string (i);
        auto pitch = n.contains ("pitch") ? parsePitch (n["pitch"]) : std::nullopt;

        if (! pitch)
            return makeError (errc::invalidParams, "pitch must be 0..127 or a note name such as C4", json { { "path", where + "/pitch" } });

        if (! n.contains ("start_qn") || ! n["start_qn"].is_number() || n["start_qn"].get<double>() < 0.0)
            return makeError (errc::invalidParams, "start_qn must be a number >= 0 (quarter notes from the start of the item)", json { { "path", where + "/start_qn" } });

        if (! n.contains ("length_qn") || ! n["length_qn"].is_number() || n["length_qn"].get<double>() <= 0.0)
            return makeError (errc::invalidParams, "length_qn must be a number > 0", json { { "path", where + "/length_qn" } });

        const int velocity = n.value ("velocity", 100);
        out.push_back ({ (double) *pitch, n["start_qn"].get<double>(), n["length_qn"].get<double>(), (double) std::clamp (velocity, 1, 127) });
    }

    return out;
}

json addNotes (Project& p, te::MidiClip& clip, const std::vector<std::array<double, 4>>& notes, bool extend)
{
    json ids = json::array();
    double maxEnd = 0;

    for (auto& n : notes)
    {
        auto* added = clip.getSequence().addNote ((int) n[0], te::BeatPosition::fromBeats (n[1]), te::BeatDuration::fromBeats (n[2]), (int) n[3], 0, &p.edit().getUndoManager());

        if (added != nullptr)
            ids.push_back (noteId (*added));

        maxEnd = std::max (maxEnd, n[1] + n[2]);
    }

    if (extend && maxEnd > clipLengthQn (p, clip) + 1.0e-9)
    {
        const double bar = beatsPerBar (p, clip.getPosition().getStart().inSeconds());
        setClipLengthQn (p, clip, std::ceil (maxEnd / bar - 1.0e-9) * bar);
    }

    return ids;
}

bool selectedNote (const json& params, te::MidiNote& n, std::set<std::string>& wanted)
{
    return wanted.empty() || wanted.count (noteId (n)) > 0;
}

std::set<std::string> noteIdSet (const json& params)
{
    std::set<std::string> s;

    for (auto& id : params.value ("note_ids", json::array()))
        s.insert (id.get<std::string>());

    return s;
}
} // namespace

void registerMidiCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("item.create_midi",
        obj ({ { "track_id", strMin (1) }, { "start_s", numMin (0) }, { "start", timePosSchema() }, { "length_s", numMin (0.001) }, { "length", durationSchema() },
               { "name", str() }, { "notes", arrayOf (noteSchema()) } }, { "track_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* track = p.findTrack (params["track_id"].get<std::string>());
            auto* clipTrack = dynamic_cast<te::AudioTrack*> (track);

            if (clipTrack == nullptr)
                return makeError (errc::notFound, "no track " + params["track_id"].get<std::string>(), json { { "track_id", params["track_id"] } });

            auto start = readTime (p, params, "start");

            if (! start.ok())
                return start.error();

            const double startS = start.value().value_or (0.0);
            std::optional<double> lengthS;

            if (params.contains ("length_s"))
            {
                lengthS = params["length_s"].get<double>();
            }
            else if (params.contains ("length"))
            {
                auto l = durationToSeconds (p, params["length"], startS);

                if (! l.ok())
                    return l.error();

                lengthS = l.value();
            }

            auto parsed = parseNotes (params.value ("notes", json::array()), "/notes");

            if (! parsed.ok())
                return parsed.error();

            if (! lengthS)
            {
                auto l = durationToSeconds (p, { { "bars", 1 } }, startS);
                lengthS = l.ok() ? l.value() : 2.0;
            }

            auto clip = clipTrack->insertMIDIClip (toJuce (params.value ("name", "MIDI")), te::TimeRange (te::TimePosition::fromSeconds (startS), te::TimeDuration::fromSeconds (*lengthS)), nullptr);

            if (clip == nullptr)
                return makeError (errc::internal, "the MIDI item could not be created");

            ChangeCollector::assignMissingIds (clip->state);
            auto ids = addNotes (p, *clip, parsed.value(), ! params.contains ("length_s") && ! params.contains ("length"));
            auto out = itemJson (p, *clip);
            out["note_ids"] = ids;
            return json { { "item_id", Project::idOf (clip->state) }, { "item", out } };
        }));

    r.add (makeProjectCommand ("item.duplicate",
        obj ({ { "item_id", strMin (1) }, { "to_track_id", strMin (1) }, { "start_s", numMin (0) }, { "start", timePosSchema() },
               { "count", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 } } } }, { "item_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* clip = p.findClip (params["item_id"].get<std::string>());

            if (clip == nullptr)
                return makeError (errc::notFound, "no item " + params["item_id"].get<std::string>(), json { { "item_id", params["item_id"] } });

            te::ClipTrack* target = clip->getClipTrack();

            if (params.contains ("to_track_id"))
            {
                target = dynamic_cast<te::ClipTrack*> (p.findTrack (params["to_track_id"].get<std::string>()));

                if (target == nullptr)
                    return makeError (errc::notFound, "no track " + params["to_track_id"].get<std::string>(), json { { "track_id", params["to_track_id"] } });
            }

            auto start = readTime (p, params, "start");

            if (! start.ok())
                return start.error();

            // default: right after the original (several copies follow each other)
            double at = start.value().value_or (clip->getPosition().getEnd().inSeconds());
            const double length = clip->getPosition().getLength().inSeconds();
            json copies = json::array();

            for (int i = 0; i < params.value ("count", 1); ++i)
            {
                auto copy = te::duplicateClip (*clip);

                if (copy == nullptr)
                    return makeError (errc::internal, "the item could not be duplicated");

                if (target != nullptr && copy->getClipTrack() != target)
                    copy->moveTo (*target);

                copy->setStart (te::TimePosition::fromSeconds (at), false, true);
                ChangeCollector::assignMissingIds (copy->state);
                json item { { "item_id", Project::idOf (copy->state) }, { "start_s", copy->getPosition().getStart().inSeconds() }, { "length_s", length },
                            { "start_bar_beat", barBeatString (p.edit(), copy->getPosition().getStart().inSeconds()) } };

                if (auto* t = copy->getClipTrack())
                    item["track_id"] = Project::idOf (t->state);

                copies.push_back (item);
                at += length;
            }

            return json { { "source_item_id", params["item_id"] }, { "items", copies }, { "item_id", copies.back()["item_id"] } };
        }));

    //==========================================================================
    r.add (makeProjectCommand ("midi.get", obj ({ { "item_id", strMin (1) } }, { "item_id" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* clip = midiClip (p, params["item_id"].get<std::string>(), error);

            if (clip == nullptr)
                return error;

            json notes = json::array();

            for (auto* n : clip->getSequence().getNotes())
                notes.push_back (noteJson (*n));

            std::sort (notes.begin(), notes.end(), [] (const json& a, const json& b)
            {
                return a["start_qn"] != b["start_qn"] ? a["start_qn"] < b["start_qn"] : a["pitch"] < b["pitch"];
            });

            return json { { "revision", p.revision() }, { "item", itemJson (p, *clip) }, { "notes", notes } };
        }));

    r.add (makeProjectCommand ("midi.write",
        obj ({ { "item_id", strMin (1) }, { "notes", arrayOf (noteSchema()) }, { "mode", { { "type", "string" }, { "enum", { "replace", "append" } } } }, { "extend", boolean() } },
            { "item_id", "notes" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* clip = midiClip (p, params["item_id"].get<std::string>(), error);

            if (clip == nullptr)
                return error;

            auto parsed = parseNotes (params["notes"], "/notes");

            if (! parsed.ok())
                return parsed.error();

            if (params.value ("mode", "replace") == "replace")
                clip->getSequence().clear (&p.edit().getUndoManager());

            auto ids = addNotes (p, *clip, parsed.value(), params.value ("extend", true));
            return json { { "item", itemJson (p, *clip) }, { "note_ids", ids } };
        }));

    r.add (makeProjectCommand ("midi.edit",
        obj ({ { "item_id", strMin (1) }, { "add", arrayOf (noteSchema()) }, { "delete", arrayOf (str()) },
               { "update", arrayOf (obj ({ { "note_id", strMin (1) }, { "pitch", {} }, { "start_qn", numMin (0) }, { "length_qn", numMin (0.001) },
                                          { "velocity", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 127 } } } }, { "note_id" })) } }, { "item_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* clip = midiClip (p, params["item_id"].get<std::string>(), error);

            if (clip == nullptr)
                return error;

            auto parsed = parseNotes (params.value ("add", json::array()), "/add");

            if (! parsed.ok())
                return parsed.error();

            for (auto& id : params.value ("delete", json::array()))
                if (findNote (*clip, id.get<std::string>()) == nullptr)
                    return makeError (errc::notFound, "no note " + id.get<std::string>(), json { { "note_id", id } });

            std::vector<std::pair<te::MidiNote*, json>> updates;

            for (auto& u : params.value ("update", json::array()))
            {
                auto* n = findNote (*clip, u["note_id"].get<std::string>());

                if (n == nullptr)
                    return makeError (errc::notFound, "no note " + u["note_id"].get<std::string>(), json { { "note_id", u["note_id"] } });

                if (u.contains ("pitch") && ! parsePitch (u["pitch"]))
                    return makeError (errc::invalidParams, "pitch must be 0..127 or a note name such as C4", json { { "note_id", u["note_id"] } });

                updates.emplace_back (n, u);
            }

            auto& um = p.edit().getUndoManager();
            json deleted = json::array();

            for (auto& id : params.value ("delete", json::array()))
                if (auto* n = findNote (*clip, id.get<std::string>()))
                {
                    clip->getSequence().removeNote (*n, &um);
                    deleted.push_back (id);
                }

            for (auto& [n, u] : updates)
            {
                if (u.contains ("pitch"))       n->setNoteNumber (*parsePitch (u["pitch"]), &um);
                if (u.contains ("velocity"))    n->setVelocity (u["velocity"].get<int>(), &um);

                if (u.contains ("start_qn") || u.contains ("length_qn"))
                    n->setStartAndLength (te::BeatPosition::fromBeats (u.value ("start_qn", n->getStartBeat().inBeats())),
                                          te::BeatDuration::fromBeats (u.value ("length_qn", n->getLengthBeats().inBeats())), &um);
            }

            auto ids = addNotes (p, *clip, parsed.value(), true);
            return json { { "item", itemJson (p, *clip) }, { "added", ids }, { "deleted", deleted }, { "updated", (int) updates.size() } };
        }));

    r.add (makeProjectCommand ("midi.transpose",
        obj ({ { "item_id", strMin (1) }, { "semitones", { { "type", "integer" }, { "minimum", -96 }, { "maximum", 96 } } }, { "note_ids", arrayOf (str()) } }, { "item_id", "semitones" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* clip = midiClip (p, params["item_id"].get<std::string>(), error);

            if (clip == nullptr)
                return error;

            auto wanted = noteIdSet (params);
            const int by = params["semitones"].get<int>();
            std::vector<te::MidiNote*> targets;

            for (auto* n : clip->getSequence().getNotes())
                if (selectedNote (params, *n, wanted))
                    targets.push_back (n);

            for (auto* n : targets)
                if (n->getNoteNumber() + by < 0 || n->getNoteNumber() + by > 127)
                    return makeError (errc::invalidParams, "transposing by " + std::to_string (by) + " moves note " + pitchName (n->getNoteNumber()) + " out of the MIDI range 0..127");

            for (auto* n : targets)
                n->setNoteNumber (n->getNoteNumber() + by, &p.edit().getUndoManager());

            return json { { "item", itemJson (p, *clip) }, { "transposed", (int) targets.size() }, { "semitones", by } };
        }));

    r.add (makeProjectCommand ("midi.quantize",
        obj ({ { "item_id", strMin (1) }, { "grid_qn", numMin (0.0625) }, { "strength", { { "type", "number" }, { "minimum", 0 }, { "maximum", 1 } } },
               { "quantize_length", boolean() }, { "note_ids", arrayOf (str()) } }, { "item_id", "grid_qn" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* clip = midiClip (p, params["item_id"].get<std::string>(), error);

            if (clip == nullptr)
                return error;

            auto wanted = noteIdSet (params);
            const double grid = params["grid_qn"].get<double>(), strength = params.value ("strength", 1.0);
            const bool lengths = params.value ("quantize_length", false);
            int moved = 0;
            std::vector<te::MidiNote*> targets;

            for (auto* n : clip->getSequence().getNotes())
                if (selectedNote (params, *n, wanted))
                    targets.push_back (n);

            for (auto* n : targets)
            {
                const double s = n->getStartBeat().inBeats(), l = n->getLengthBeats().inBeats();
                const double snappedStart = std::round (s / grid) * grid;
                const double newStart = s + (snappedStart - s) * strength;
                double newLength = l;

                if (lengths)
                {
                    const double snappedLength = std::max (grid, std::round (l / grid) * grid);
                    newLength = l + (snappedLength - l) * strength;
                }

                if (std::abs (newStart - s) > 1.0e-9 || std::abs (newLength - l) > 1.0e-9)
                {
                    n->setStartAndLength (te::BeatPosition::fromBeats (newStart), te::BeatDuration::fromBeats (newLength), &p.edit().getUndoManager());
                    ++moved;
                }
            }

            return json { { "item", itemJson (p, *clip) }, { "moved", moved } };
        }));

    r.add (makeProjectCommand ("midi.import_file",
        obj ({ { "track_id", strMin (1) }, { "file", strMin (1) }, { "start_s", numMin (0) }, { "start", timePosSchema() }, { "name", str() },
              { "midi_track", { { "type", "integer" }, { "minimum", 0 } } } }, { "track_id", "file" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* track = dynamic_cast<te::AudioTrack*> (p.findTrack (params["track_id"].get<std::string>()));

            if (track == nullptr)
                return makeError (errc::notFound, "no track " + params["track_id"].get<std::string>(), json { { "track_id", params["track_id"] } });

            auto allowed = p.host().checkPath (params["file"].get<std::string>());

            if (! allowed.ok())
                return allowed.error();

            juce::MidiFile file;
            juce::FileInputStream in (toFile (allowed.value()));

            if (! in.openedOk() || ! file.readFrom (in))
                return makeError (errc::invalidParams, "not a readable standard MIDI file: " + params["file"].get<std::string>(), json { { "path", "/file" } });

            const int ticks = file.getTimeFormat();

            if (ticks <= 0)
                return makeError (errc::unsupported, "SMPTE time code MIDI files are not supported (only ticks per quarter note)");

            // all tracks merged unless one is named
            juce::MidiMessageSequence merged;

            for (int t = 0; t < file.getNumTracks(); ++t)
                if (! params.contains ("midi_track") || params["midi_track"].get<int>() == t)
                    merged.addSequence (*file.getTrack (t), 0.0);

            merged.updateMatchedPairs();
            std::vector<std::array<double, 4>> notes;
            double endQn = 0;

            for (int i = 0; i < merged.getNumEvents(); ++i)
            {
                auto* ev = merged.getEventPointer (i);

                if (! ev->message.isNoteOn() || ev->noteOffObject == nullptr)
                    continue;

                const double s = ev->message.getTimeStamp() / ticks, e = ev->noteOffObject->message.getTimeStamp() / ticks;
                notes.push_back ({ (double) ev->message.getNoteNumber(), s, std::max (0.01, e - s), (double) std::max (1, (int) ev->message.getVelocity()) });
                endQn = std::max (endQn, e);
            }

            if (notes.empty())
                return makeError (errc::invalidParams, "the MIDI file has no notes");

            auto start = readTime (p, params, "start");

            if (! start.ok())
                return start.error();

            const double startS = start.value().value_or (0.0);
            auto& ts = p.edit().tempoSequence;
            const auto startBeats = ts.toBeats (te::TimePosition::fromSeconds (startS));
            const double bar = beatsPerBar (p, startS);
            const double lengthQn = std::ceil (endQn / bar - 1.0e-9) * bar;
            const double lengthS = ts.toTime (startBeats + te::BeatDuration::fromBeats (lengthQn)).inSeconds() - startS;
            auto clip = track->insertMIDIClip (toJuce (params.value ("name", toStd (toFile (allowed.value()).getFileNameWithoutExtension()))),
                                               te::TimeRange (te::TimePosition::fromSeconds (startS), te::TimeDuration::fromSeconds (lengthS)), nullptr);

            if (clip == nullptr)
                return makeError (errc::internal, "the MIDI item could not be created");

            ChangeCollector::assignMissingIds (clip->state);
            addNotes (p, *clip, notes, false);
            return json { { "item_id", Project::idOf (clip->state) }, { "item", itemJson (p, *clip) }, { "imported_notes", (int) notes.size() } };
        }));
}

} // namespace rovox
