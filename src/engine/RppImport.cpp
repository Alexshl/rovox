// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RppImport.h"

#include "Command.h"
#include "Ids.h"
#include "Import.h"
#include "RppParser.h"
#include "Sha256.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace rovox
{

json RppImportReport::toJson() const
{
    return { { "imported", { { "tracks", tracks }, { "folders", folders }, { "items", items }, { "midi_items", midiItems },
                             { "markers", markers }, { "regions", regions }, { "sends", sends } } },
             { "unsupported", unsupported }, { "missing_media", missingMedia }, { "warnings", warnings } };
}

namespace
{
std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

double toDouble (const std::vector<std::string>* l, size_t arg, double fallback)
{
    if (l != nullptr && l->size() > arg + 1)
        try { return std::stod ((*l)[arg + 1]); } catch (...) {}

    return fallback;
}

std::string toStringArg (const std::vector<std::string>* l, size_t arg)
{
    return (l != nullptr && l->size() > arg + 1) ? (*l)[arg + 1] : std::string();
}

double linearToDbSafe (double lin)  { return lin > 1.0e-6 ? 20.0 * std::log10 (lin) : -100.0; }

juce::Colour reaperColour (int value)
{
    // PEAKCOL / COLOR: low 24 bits, R in the lowest byte (the 0x1000000 flag only says "custom colour")
    return juce::Colour ((juce::uint8) (value & 0xff), (juce::uint8) ((value >> 8) & 0xff), (juce::uint8) ((value >> 16) & 0xff));
}

/** basename -> path index of the files below the RPP directory, to find media that moved or was recorded on another machine. */
class MediaFinder
{
public:
    MediaFinder (fs::path rppDir, std::string recordPath) : dir_ (std::move (rppDir)), record_ (std::move (recordPath)) {}

    std::optional<fs::path> find (std::string file)
    {
        std::replace (file.begin(), file.end(), '\\', '/');
        const fs::path p (file);
        std::error_code ec;

        if (p.is_absolute() && fs::is_regular_file (p, ec))
            return p;

        for (const auto& base : { dir_, record_.empty() ? dir_ : (fs::path (record_).is_absolute() ? fs::path (record_) : dir_ / record_) })
            if (! p.is_absolute() && fs::is_regular_file (base / p, ec))
                return base / p;

        buildIndex();
        auto it = index_.find (lower (p.filename().string()));
        return it == index_.end() ? std::nullopt : std::optional<fs::path> (it->second);
    }

    /** Where a missing file is expected, for the offline clip. */
    fs::path expected (std::string file) const
    {
        std::replace (file.begin(), file.end(), '\\', '/');
        const fs::path p (file);
        return p.is_absolute() ? p : dir_ / p;
    }

private:
    void buildIndex()
    {
        if (indexed_)
            return;

        indexed_ = true;
        std::error_code ec;
        int seen = 0;

        for (fs::recursive_directory_iterator it (dir_, fs::directory_options::skip_permission_denied, ec), end; it != end && seen < 50000; it.increment (ec))
        {
            if (it.depth() > 4)
            {
                it.disable_recursion_pending();
                continue;
            }

            if (it->is_regular_file (ec))
            {
                ++seen;
                index_.try_emplace (lower (it->path().filename().string()), it->path());
            }
        }
    }

    fs::path dir_;
    std::string record_;
    bool indexed_ = false;
    std::map<std::string, fs::path> index_;
};

struct MidiNoteData
{
    int note, velocity;
    double startBeats, lengthBeats;
};

std::vector<MidiNoteData> parseMidiSource (const RppNode& source, double& ppq)
{
    ppq = 960.0;

    if (auto* h = source.line ("HASDATA"); h != nullptr && h->size() > 2)
        try { ppq = std::stod ((*h)[2]); } catch (...) {}

    std::vector<MidiNoteData> notes;
    std::map<std::pair<int, int>, std::pair<double, int>> open;     // (channel, note) -> (start ticks, velocity)
    double ticks = 0;

    for (auto& e : source.entries)
    {
        if (e.isBlock() || e.tokens.size() < 5 || (e.tokens[0] != "E" && e.tokens[0] != "e"))
            continue;

        try
        {
            ticks += std::stod (e.tokens[1]);
            const int status = std::stoi (e.tokens[2], nullptr, 16), d1 = std::stoi (e.tokens[3], nullptr, 16), d2 = std::stoi (e.tokens[4], nullptr, 16);
            const int kind = status & 0xf0, channel = status & 0x0f;

            if (kind == 0x90 && d2 > 0)
            {
                open[{ channel, d1 }] = { ticks, d2 };
            }
            else if (kind == 0x80 || (kind == 0x90 && d2 == 0))
            {
                if (auto it = open.find ({ channel, d1 }); it != open.end())
                {
                    notes.push_back ({ d1, it->second.second, it->second.first / ppq, std::max (0.01, (ticks - it->second.first) / ppq) });
                    open.erase (it);
                }
            }
        }
        catch (...) {}
    }

    return notes;
}

struct Importer
{
    Project& project;
    const fs::path rppFile;
    const RppImportOptions options;
    RppImportReport report;
    std::map<fs::path, std::pair<std::string, AudioInfo>> hashed;      // staged media by source path
    std::unique_ptr<MediaFinder> finder;
    std::vector<te::Track*> allTracks;                                  // in RPP order, for AUXRECV

    struct Resolved
    {
        fs::path file;
        bool found = false;
        AudioInfo info;
        double lengthFromFileS = 0;
    };

    Resolved stage (const std::string& fileTok, const std::string& trackId)
    {
        Resolved r;
        auto found = finder->find (fileTok);

        if (! found)
        {
            r.file = finder->expected (fileTok);
            report.missingMedia.push_back ({ { "path", fileTok }, { "expected", r.file.string() }, { "track_id", trackId } });
            return r;
        }

        auto probe = probeAudio (*found);

        if (! probe.ok())
        {
            r.file = *found;
            report.warnings.push_back ({ { "code", "UNREADABLE_MEDIA" }, { "message", probe.error().message } });
            report.missingMedia.push_back ({ { "path", fileTok }, { "expected", found->string() }, { "track_id", trackId }, { "reason", "unreadable" } });
            return r;
        }

        r.found = true;
        r.info = probe.value();

        if (! options.copyMedia)
        {
            r.file = *found;

            if (hashed.find (*found) == hashed.end())
            {
                MediaEntry m;
                m.mediaId = newId ("med");
                m.path = found->string();
                m.origName = found->filename().string();
                m.channels = r.info.channels;
                m.sampleRate = r.info.sampleRate;
                m.bitDepth = r.info.bitDepth;
                m.frames = r.info.frames;
                m.external = true;
                project.manifest().media.push_back (m);
                hashed[*found] = { "", r.info };
            }

            return r;
        }

        auto it = hashed.find (*found);

        if (it == hashed.end())
        {
            const auto sha = sha256File (*found).value_or ("");
            auto staged = stageMedia (*found, sha, project.mediaDir());

            if (! staged.ok())
            {
                report.warnings.push_back ({ { "code", "COPY_FAILED" }, { "message", staged.error().message } });
                r.file = *found;
                return r;
            }

            ImportClipPlan plan;
            plan.origName = found->filename().string();
            plan.sha256 = sha;
            plan.mediaPath = "media/" + staged.value().filename().string();
            plan.info = r.info;
            registerMedia (project, plan);
            it = hashed.emplace (*found, std::make_pair (staged.value().string(), r.info)).first;
        }

        r.file = it->second.first;
        return r;
    }

    void importMarkers (const RppNode& root, te::Edit& edit)
    {
        struct M { int idx; double pos; std::string name; int flags; };
        std::vector<M> all;

        for (auto* l : root.lines ("MARKER"))
            all.push_back ({ (int) toDouble (l, 0, 0), toDouble (l, 1, 0), toStringArg (l, 2), (int) toDouble (l, 3, 0) });

        auto& mm = edit.getMarkerManager();
        std::set<int> consumed;

        for (size_t i = 0; i < all.size(); ++i)
        {
            if (consumed.count ((int) i))
                continue;

            const auto& m = all[i];
            double duration = 0;
            const bool region = (m.flags & 1) != 0;

            if (region)
                for (size_t j = i + 1; j < all.size(); ++j)
                    if (all[j].idx == m.idx && (all[j].flags & 1) != 0 && ! consumed.count ((int) j))
                    {
                        duration = std::max (0.0, all[j].pos - m.pos);
                        consumed.insert ((int) j);
                        break;
                    }

            auto marker = mm.createMarker (mm.getNextUniqueID(), te::TimePosition::fromSeconds (m.pos), te::TimeDuration::fromSeconds (duration), nullptr);

            if (marker == nullptr)
                continue;

            marker->setName (toJuce (m.name));
            ChangeCollector::assignMissingIds (marker->state);
            ++(region ? report.regions : report.markers);
        }
    }

    void importTempo (const RppNode& root, te::Edit& edit)
    {
        if (auto* t = root.line ("TEMPO"))
        {
            const double bpm = toDouble (t, 0, 120.0);
            const int num = (int) toDouble (t, 1, 4), den = (int) toDouble (t, 2, 4);

            if (auto* tempo = edit.tempoSequence.getTempo (0))
                tempo->setBpm (bpm);

            if (auto* sig = edit.tempoSequence.getTimeSig (0))
                sig->setStringTimeSig (juce::String (num) + "/" + juce::String (den));
        }

        if (auto* env = root.block ("TEMPOENVEX"))
            for (auto* pt : env->lines ("PT"))
            {
                const double at = toDouble (pt, 0, 0.0), bpm = toDouble (pt, 1, 0.0);

                if (at <= 0.0 || bpm <= 0.0)
                    continue;

                if (auto tempo = edit.tempoSequence.insertTempo (te::TimePosition::fromSeconds (at)))
                    tempo->setBpm (bpm);
            }
    }

    void importItem (const RppNode& item, te::AudioTrack& track, const std::string& trackId)
    {
        const double position = item.number ("POSITION", 0, 0.0), length = item.number ("LENGTH", 0, 0.0);
        const double soffs = item.number ("SOFFS", 0, 0.0);
        const bool muted = item.number ("MUTE", 0, 0.0) != 0.0;
        const auto* nameLine = item.line ("NAME");
        std::string name = nameLine != nullptr && nameLine->size() > 1 ? (*nameLine)[1] : std::string();

        if (length <= 0.0)
            return;

        const RppNode* source = nullptr;

        for (auto& e : item.entries)
            if (e.isBlock() && e.children[0].name == "SOURCE" && source == nullptr)
                source = &e.children[0];

        if (source == nullptr)
            return;

        if (item.blocks ("SOURCE").size() > 1)
            report.warnings.push_back ({ { "code", "TAKES_IGNORED" }, { "message", "item has several takes; only the first is imported" }, { "track_id", trackId } });

        if (item.number ("PLAYRATE", 0, 1.0) != 1.0)
            report.warnings.push_back ({ { "code", "PLAYRATE_IGNORED" }, { "message", "item play rate is not applied" }, { "track_id", trackId } });

        const auto kind = source->args.empty() ? std::string() : source->args[0];
        const auto start = te::TimePosition::fromSeconds (position);
        const auto len = te::TimeDuration::fromSeconds (length);

        if (kind == "MIDI")
        {
            double ppq = 960.0;
            auto notes = parseMidiSource (*source, ppq);
            auto clip = te::insertMIDIClip (track, toJuce (name.empty() ? "MIDI" : name), { start, start + len });

            if (clip == nullptr)
                return;

            for (auto& n : notes)
                clip->getSequence().addNote (n.note, te::BeatPosition::fromBeats (n.startBeats), te::BeatDuration::fromBeats (n.lengthBeats), n.velocity, 0, nullptr);

            if (soffs != 0.0)
                report.warnings.push_back ({ { "code", "MIDI_OFFSET_IGNORED" }, { "message", "source offset of a MIDI item is not applied" }, { "track_id", trackId } });

            clip->setMuted (muted);
            ChangeCollector::assignMissingIds (clip->state);
            ++report.midiItems;
            return;
        }

        if (kind != "WAVE" && kind != "MP3" && kind != "FLAC" && kind != "VORBIS")
        {
            report.unsupported.push_back ({ { "kind", "item_source" }, { "name", kind }, { "track_id", trackId } });
            return;
        }

        const auto fileTok = toStringArg (source->line ("FILE"), 0);

        if (fileTok.empty())
            return;

        auto media = stage (fileTok, trackId);
        auto clip = te::insertWaveClip (track, toJuce (name.empty() ? fs::path (fileTok).stem().string() : name), toFile (media.file),
                                        { .time = { start, start + len }, .offset = te::TimeDuration::fromSeconds (soffs) }, te::DeleteExistingClips::no);

        if (clip == nullptr)
            return;

        const double volume = item.number ("VOLPAN", 0, 1.0);

        if (std::abs (volume - 1.0) > 1e-6)
            clip->setGainDB ((float) linearToDbSafe (volume));

        if (const double fi = toDouble (item.line ("FADEIN"), 1, 0.0); fi > 0.0)    clip->setFadeIn (te::TimeDuration::fromSeconds (fi));
        if (const double fo = toDouble (item.line ("FADEOUT"), 1, 0.0); fo > 0.0)   clip->setFadeOut (te::TimeDuration::fromSeconds (fo));

        clip->setMuted (muted);
        ChangeCollector::assignMissingIds (clip->state);
        ++report.items;
    }

    Result<void> run()
    {
        std::ifstream in (rppFile, std::ios::binary);

        if (! in)
            return makeError (errc::notFound, "cannot read " + rppFile.string());

        std::stringstream ss;
        ss << in.rdbuf();
        auto parsed = parseRpp (ss.str());

        if (! parsed.ok())
            return parsed.error();

        const auto& root = parsed.value();
        auto& edit = project.edit();
        std::string recordPath;

        if (auto* rp = root.line ("RECORD_PATH"))
            recordPath = rp->size() > 1 ? (*rp)[1] : std::string();

        finder = std::make_unique<MediaFinder> (rppFile.parent_path(), recordPath);
        importTempo (root, edit);

        std::vector<te::Track*> folderStack;
        std::map<te::Track*, te::Track*> lastChild;
        int colorFallback = 0;
        (void) colorFallback;

        struct Pending { size_t source, receiver; double volume; int mode; };
        std::vector<Pending> sends;
        const auto trackBlocks = root.blocks ("TRACK");

        for (size_t index = 0; index < trackBlocks.size(); ++index)
        {
            const auto& node = *trackBlocks[index];
            const auto* nameLine = node.line ("NAME");
            const std::string name = nameLine != nullptr && nameLine->size() > 1 ? (*nameLine)[1] : "Track " + std::to_string (index + 1);
            const int busFlag = (int) node.number ("ISBUS", 0, 0), busDepth = (int) node.number ("ISBUS", 1, 0);

            te::Track* parent = folderStack.empty() ? nullptr : folderStack.back();
            te::Track* preceding = lastChild.count (parent) ? lastChild[parent] : nullptr;
            const te::TrackInsertPoint where (parent, preceding);
            te::Track::Ptr created;

            if (busFlag == 1 && busDepth == 1)
            {
                created = edit.insertNewFolderTrack (where, nullptr, false);
                ++report.folders;
            }
            else
            {
                created = edit.insertNewAudioTrack (where, nullptr);
            }

            if (created == nullptr)
                return makeError (errc::preconditionFailed, "track limit reached while importing track '" + name + "'");

            te::Track* track = created.get();
            lastChild[parent] = track;
            allTracks.push_back (track);
            track->setName (toJuce (name));
            ChangeCollector::assignMissingIds (track->state);
            const auto trackId = Project::idOf (track->state);
            ++report.tracks;

            if (const auto* c = node.line ("PEAKCOL"); c != nullptr && c->size() > 1)
                try { const int v = std::stoi ((*c)[1]); if (v != 0) track->setColour (reaperColour (v)); } catch (...) {}

            track->setMute (node.number ("MUTESOLO", 0, 0) != 0);
            track->setSolo (node.number ("MUTESOLO", 1, 0) != 0);

            if (auto* at = dynamic_cast<te::AudioTrack*> (track))
            {
                if (auto* vp = at->getVolumePlugin())
                {
                    vp->setVolumeDb ((float) linearToDbSafe (node.number ("VOLPAN", 0, 1.0)));
                    vp->setPan ((float) std::clamp (node.number ("VOLPAN", 1, 0.0), -1.0, 1.0));
                }

                for (auto* item : node.blocks ("ITEM"))
                    importItem (*item, *at, trackId);
            }
            else if (! node.blocks ("ITEM").empty())
            {
                report.warnings.push_back ({ { "code", "FOLDER_ITEMS_DROPPED" }, { "message", "items on a folder track are not imported" }, { "track_id", trackId } });
            }

            for (auto* chain : node.blocks ("FXCHAIN"))
            {
                for (auto& e : chain->entries)
                    if (e.isBlock())
                    {
                        const auto& fx = e.children[0];
                        report.unsupported.push_back ({ { "kind", "fx" }, { "name", fx.args.empty() ? fx.name : fx.args[0] }, { "format", fx.name }, { "track_id", trackId } });
                    }

                track->state.setProperty ("rovox:unsupported_fx", toJuce (chain->toText()), nullptr);
            }

            for (auto* recv : node.lines ("AUXRECV"))
                sends.push_back ({ (size_t) toDouble (recv, 0, -1), index, toDouble (recv, 2, 1.0), (int) toDouble (recv, 1, 0) });

            if (busDepth < 0)
                for (int k = 0; k < -busDepth && ! folderStack.empty(); ++k)
                    folderStack.pop_back();

            if (busFlag == 1 && busDepth == 1)
                folderStack.push_back (track);
        }

        // sends: stored on the receiver in RPP; Tracktion wants a send on the source and a return on the receiver sharing a bus number
        std::map<size_t, int> busOfReceiver;

        for (auto& s : sends)
        {
            if (s.source >= allTracks.size() || s.receiver >= allTracks.size())
                continue;

            auto* src = dynamic_cast<te::AudioTrack*> (allTracks[s.source]);
            auto* dst = dynamic_cast<te::AudioTrack*> (allTracks[s.receiver]);

            if (src == nullptr || dst == nullptr)
            {
                report.warnings.push_back ({ { "code", "SEND_ON_FOLDER" }, { "message", "sends from or to folder tracks are not imported" } });
                continue;
            }

            if (! busOfReceiver.count (s.receiver))
            {
                const int bus = (int) busOfReceiver.size();
                busOfReceiver[s.receiver] = bus;
                auto ret = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {});
                dst->pluginList.insertPlugin (ret, 0, nullptr);

                if (auto* r = dynamic_cast<te::AuxReturnPlugin*> (ret.get()))
                    r->busNumber = bus;

                ChangeCollector::assignMissingIds (ret->state);
            }

            auto send = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});
            auto* vol = src->getVolumePlugin();
            int at = src->pluginList.size();

            if (vol != nullptr)
                at = src->pluginList.indexOf (vol) + (s.mode == 0 ? 1 : 0);      // post-fader send sits after the volume plugin

            src->pluginList.insertPlugin (send, std::max (0, at), nullptr);

            if (auto* a = dynamic_cast<te::AuxSendPlugin*> (send.get()))
            {
                a->busNumber = busOfReceiver[s.receiver];
                a->setGainDb ((float) linearToDbSafe (s.volume));
            }

            ChangeCollector::assignMissingIds (send->state);
            ++report.sends;
        }

        importMarkers (root, edit);

        if (auto* mfx = root.block ("MASTERFXLIST"))
            for (auto& e : mfx->entries)
                if (e.isBlock())
                    report.unsupported.push_back ({ { "kind", "fx" }, { "name", e.children[0].args.empty() ? e.children[0].name : e.children[0].args[0] }, { "track_id", "master" } });

        return {};
    }
};
} // namespace

Result<RppImportReport> importRpp (Project& project, const fs::path& rppFile, const RppImportOptions& options)
{
    Importer imp { project, rppFile, options, {}, {}, nullptr, {} };

    if (auto r = imp.run(); ! r.ok())
        return r.error();

    return imp.report;
}

void registerRppCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("import.rpp",
        { { "type", "object" }, { "properties", { { "path", { { "type", "string" }, { "minLength", 1 } } }, { "copy_media", { { "type", "boolean" } } } } },
          { "required", { "path" } }, { "additionalProperties", false } },
        { { "type", "object" } }, true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            if (auto allowed = p.host().checkPath (params["path"].get<std::string>()); ! allowed.ok())
                return allowed.error();

            auto report = importRpp (p, params["path"].get<std::string>(), { .copyMedia = params.value ("copy_media", true) });

            if (! report.ok())
                return report.error();

            return report.value().toJson();
        }));
}

} // namespace rovox
