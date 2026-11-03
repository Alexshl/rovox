// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "DrumKit.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace rovox
{

//==============================================================================
namespace
{
std::string stringOf (const json& j, const char* key, const std::string& def = "")
{
    return j.contains (key) && j[key].is_string() ? j[key].get<std::string>() : def;
}

Error bad (const std::string& message, const std::string& path)
{
    return makeError (errc::invalidParams, "drum kit manifest: " + message, json { { "path", path } });
}

bool safeRelativePath (const std::string& p)
{
    if (p.empty() || p[0] == '/' || p.find ('\\') != std::string::npos)
        return false;

    size_t start = 0;

    while (start <= p.size())
    {
        const auto end = p.find ('/', start);
        const auto part = p.substr (start, end == std::string::npos ? std::string::npos : end - start);

        if (part.empty() || part == "..")
            return false;

        if (end == std::string::npos)
            break;

        start = end + 1;
    }

    return true;
}
} // namespace

Result<DrumKitManifest> DrumKitManifest::fromJson (const json& j)
{
    if (! j.is_object())
        return bad ("not an object", "");

    if (j.contains ("format") && stringOf (j, "format") != "rovox-drumkit")
        return bad ("format must be \"rovox-drumkit\"", "/format");

    DrumKitManifest m;
    m.id = stringOf (j, "id");
    m.name = stringOf (j, "name", m.id);
    m.version = j.contains ("version") ? (j["version"].is_string() ? j["version"].get<std::string>() : j["version"].dump()) : "";
    m.license = stringOf (j, "license");
    m.attribution = stringOf (j, "attribution");
    m.source = stringOf (j, "source");
    m.sampleRate = j.value ("sample_rate", 44100);

    if (m.sampleRate < 8000 || m.sampleRate > 192000)
        return bad ("sample_rate out of range", "/sample_rate");

    if (! j.contains ("pieces") || ! j["pieces"].is_array() || j["pieces"].empty())
        return bad ("needs a non-empty \"pieces\" array", "/pieces");

    std::set<std::string> ids;
    std::map<int, std::string> noteOwner;
    int pi = 0;

    for (auto& pj : j["pieces"])
    {
        const auto at = "/pieces/" + std::to_string (pi++);

        if (! pj.is_object())
            return bad ("piece is not an object", at);

        DrumPieceDef p;
        p.id = stringOf (pj, "id");

        if (p.id.empty() || ! ids.insert (p.id).second)
            return bad ("piece id is missing or used twice: '" + p.id + "'", at + "/id");

        p.name = stringOf (pj, "name", p.id);
        p.category = stringOf (pj, "category");
        p.gainDb = pj.value ("gain_db", 0.0);
        p.pan = pj.value ("pan", 0.0);
        p.chokeGroup = stringOf (pj, "choke_group");
        p.maxVoices = pj.value ("max_voices", 4);

        if (p.pan < -1.0 || p.pan > 1.0)
            return bad ("pan must be -1..1", at + "/pan");

        if (p.maxVoices < 1 || p.maxVoices > 64)
            return bad ("max_voices must be 1..64", at + "/max_voices");

        if (pj.contains ("notes"))
        {
            if (! pj["notes"].is_array())
                return bad ("notes must be an array", at + "/notes");

            for (auto& n : pj["notes"])
            {
                if (! n.is_number_integer() || n.get<int>() < 0 || n.get<int>() > 127)
                    return bad ("notes are integers 0..127", at + "/notes");

                const int note = n.get<int>();

                if (auto it = noteOwner.find (note); it != noteOwner.end())
                    return bad ("note " + std::to_string (note) + " is mapped by '" + it->second + "' and '" + p.id + "'", at + "/notes");

                noteOwner[note] = p.id;
                p.notes.push_back (note);
            }
        }

        if (pj.contains ("chokes"))
        {
            if (! pj["chokes"].is_array())
                return bad ("chokes must be an array of group names", at + "/chokes");

            for (auto& c : pj["chokes"])
                if (c.is_string())
                    p.chokes.push_back (c.get<std::string>());
        }

        if (! pj.contains ("layers") || ! pj["layers"].is_array() || pj["layers"].empty())
            return bad ("piece '" + p.id + "' needs at least one layer", at + "/layers");

        int li = 0;

        for (auto& lj : pj["layers"])
        {
            const auto lat = at + "/layers/" + std::to_string (li++);
            DrumLayerDef l;
            l.velMin = lj.value ("vel_min", 1);
            l.velMax = lj.value ("vel_max", 127);

            if (l.velMin < 1 || l.velMax > 127 || l.velMin > l.velMax)
                return bad ("vel_min / vel_max must satisfy 1 <= min <= max <= 127", lat);

            if (! lj.contains ("samples") || ! lj["samples"].is_array() || lj["samples"].empty())
                return bad ("a layer needs at least one sample", lat + "/samples");

            for (auto& s : lj["samples"])
            {
                if (! s.is_string() || ! safeRelativePath (s.get<std::string>()))
                    return bad ("sample paths are relative paths without '..'", lat + "/samples");

                l.samples.push_back (s.get<std::string>());
            }

            p.layers.push_back (std::move (l));
        }

        std::stable_sort (p.layers.begin(), p.layers.end(), [] (auto& a, auto& b) { return a.velMin < b.velMin; });
        m.pieces.push_back (std::move (p));
    }

    if (j.contains ("files") && j["files"].is_object())
        for (auto& [k, v] : j["files"].items())
        {
            if (! safeRelativePath (k) || ! v.is_number_unsigned())
                return bad ("files maps relative paths to sizes", "/files");

            m.files[k] = v.get<std::uint64_t>();
        }

    if (j.contains ("fallbacks") && j["fallbacks"].is_object())
        for (auto& [k, v] : j["fallbacks"].items())
        {
            int note = -1;

            try { note = std::stoi (k); } catch (...) {}

            if (note < 0 || note > 127 || ! v.is_string() || ids.count (v.get<std::string>()) == 0)
                return bad ("fallbacks maps a note 0..127 to the id of a piece", "/fallbacks");

            m.fallbacks[note] = v.get<std::string>();
        }

    return m;
}

json DrumKitManifest::toJson() const
{
    json out { { "format", "rovox-drumkit" }, { "version", 1 }, { "id", id }, { "name", name }, { "license", license }, { "attribution", attribution },
               { "source", source }, { "sample_rate", sampleRate } };
    json ps = json::array();

    for (auto& p : pieces)
    {
        json layers = json::array();

        for (auto& l : p.layers)
            layers.push_back ({ { "vel_min", l.velMin }, { "vel_max", l.velMax }, { "samples", l.samples } });

        ps.push_back ({ { "id", p.id }, { "name", p.name }, { "category", p.category }, { "notes", p.notes }, { "gain_db", p.gainDb }, { "pan", p.pan },
                        { "choke_group", p.chokeGroup }, { "chokes", p.chokes }, { "max_voices", p.maxVoices }, { "layers", layers } });
    }

    out["pieces"] = ps;
    out["files"] = files;
    json fb = json::object();

    for (auto& [note, piece] : fallbacks)
        fb[std::to_string (note)] = piece;

    out["fallbacks"] = fb;
    return out;
}

std::vector<std::string> DrumKitManifest::samplePaths() const
{
    std::vector<std::string> out;
    std::set<std::string> seen;

    for (auto& p : pieces)
        for (auto& l : p.layers)
            for (auto& s : l.samples)
                if (seen.insert (s).second)
                    out.push_back (s);

    return out;
}

//==============================================================================
Result<DrumKit> DrumKit::create (DrumKitManifest m)
{
    if (m.pieces.empty())
        return makeError (errc::invalidParams, "drum kit has no pieces");

    DrumKit k;
    k.noteToPiece_.fill (-1);
    k.mapped_.fill (false);

    for (size_t i = 0; i < m.pieces.size(); ++i)
        for (int n : m.pieces[i].notes)
        {
            if (n < 0 || n > 127 || k.mapped_[(size_t) n])
                return makeError (errc::invalidParams, "drum kit maps note " + std::to_string (n) + " twice or out of range");

            k.mapped_[(size_t) n] = true;
            k.noteToPiece_[(size_t) n] = (int) i;
        }

    // notes the kit does not map go to the piece with the nearest mapped note (the lower one on a tie)
    std::vector<int> mappedNotes;

    for (int n = 0; n < 128; ++n)
        if (k.mapped_[(size_t) n])
            mappedNotes.push_back (n);

    if (mappedNotes.empty())
    {
        // a kit without notes: every piece is reachable only through the fallback of the first one
        for (int n = 0; n < 128; ++n)
            k.noteToPiece_[(size_t) n] = 0;
    }
    else
    {
        for (int n = 0; n < 128; ++n)
        {
            if (k.mapped_[(size_t) n])
                continue;

            int best = mappedNotes.front();

            for (int c : mappedNotes)
                if (std::abs (c - n) < std::abs (best - n))
                    best = c;

            k.noteToPiece_[(size_t) n] = k.noteToPiece_[(size_t) best];
        }
    }

    for (auto& [note, id] : m.fallbacks)
        for (size_t i = 0; i < m.pieces.size(); ++i)
            if (m.pieces[i].id == id && ! k.mapped_[(size_t) note])
                k.noteToPiece_[(size_t) note] = (int) i;

    for (auto& p : m.pieces)
        k.roundRobin_.emplace_back (p.layers.size(), 0);

    k.manifest_ = std::move (m);
    return k;
}

int DrumKit::pieceForNote (int note, bool* fallback) const
{
    if (note < 0 || note > 127 || manifest_.pieces.empty())
        return -1;

    if (fallback != nullptr)
        *fallback = ! mapped_[(size_t) note];

    return noteToPiece_[(size_t) note];
}

int DrumKit::layerFor (int piece, int velocity) const
{
    if (piece < 0 || piece >= numPieces())
        return 0;

    const auto& layers = manifest_.pieces[(size_t) piece].layers;
    const int v = std::clamp (velocity, 1, 127);
    int best = 0, bestDistance = 1000;

    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (v >= layers[i].velMin && v <= layers[i].velMax)
            return (int) i;

        const int d = v < layers[i].velMin ? layers[i].velMin - v : v - layers[i].velMax;

        if (d < bestDistance)
        {
            bestDistance = d;
            best = (int) i;
        }
    }

    return best;
}

DrumHit DrumKit::select (int note, int velocity)
{
    DrumHit h;
    h.piece = pieceForNote (note, &h.fallback);

    if (h.piece < 0)
        return h;

    const int v = std::clamp (velocity, 1, 127);
    h.layer = layerFor (h.piece, v);
    const auto& layer = manifest_.pieces[(size_t) h.piece].layers[(size_t) h.layer];
    int& next = roundRobin_[(size_t) h.piece][(size_t) h.layer];
    h.sample = next % (int) layer.samples.size();
    next = (h.sample + 1) % (int) layer.samples.size();

    // louder inside a layer: a wide layer (a kit with few layers) carries more of the dynamics than a narrow one
    const double width = (double) (layer.velMax - layer.velMin + 1);
    const double t = std::clamp ((double) (v - layer.velMin + 1) / width, 0.0, 1.0);
    h.gain = 1.0 - 0.5 * (width / 127.0) * (1.0 - t);
    return h;
}

void DrumKit::resetRoundRobin()
{
    for (auto& p : roundRobin_)
        std::fill (p.begin(), p.end(), 0);
}

int DrumKit::pieceById (const std::string& id) const
{
    for (size_t i = 0; i < manifest_.pieces.size(); ++i)
        if (manifest_.pieces[i].id == id)
            return (int) i;

    return -1;
}

//==============================================================================
std::shared_ptr<DrumSampleData> DrumSampleData::fromFloat (const float* left, const float* right, std::size_t frames)
{
    auto d = std::make_shared<DrumSampleData>();
    d->frames = frames;
    d->pcm.resize (frames * 2);

    auto toInt = [] (float v) { return (std::int16_t) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f); };

    for (std::size_t i = 0; i < frames; ++i)
    {
        d->pcm[i * 2] = toInt (left[i]);
        d->pcm[i * 2 + 1] = toInt (right != nullptr ? right[i] : left[i]);
    }

    return d;
}

//==============================================================================
DrumSampler::DrumSampler (double sampleRate)
    : sampleRate_ (sampleRate), stealFade_ (std::max (8, (int) std::lround (sampleRate * 0.004))), chokeFade_ (std::max (8, (int) std::lround (sampleRate * 0.008)))
{
    for (auto& a : unmappedPiece_)
        a.store (-1);
}

Result<void> DrumSampler::setKit (DrumKit kit, DrumSampleMap samples)
{
    std::vector<std::vector<std::vector<const DrumSampleData*>>> data;

    for (auto& p : kit.manifest().pieces)
    {
        data.emplace_back();

        for (auto& l : p.layers)
        {
            data.back().emplace_back();

            for (auto& s : l.samples)
            {
                auto it = samples.find (s);

                if (it == samples.end() || it->second == nullptr || it->second->frames == 0)
                    return makeError (errc::notFound, "drum kit sample missing or empty: " + s);

                data.back().back().push_back (it->second.get());
            }
        }
    }

    reset();
    kit_ = std::move (kit);
    samples_ = std::move (samples);
    data_ = std::move (data);
    pieceGain_.clear();
    pieceBalanceL_.clear();
    pieceBalanceR_.clear();

    for (auto& p : kit_.manifest().pieces)
    {
        pieceGain_.push_back ((float) std::pow (10.0, p.gainDb / 20.0));
        pieceBalanceL_.push_back ((float) (p.pan > 0.0 ? 1.0 - p.pan : 1.0));
        pieceBalanceR_.push_back ((float) (p.pan < 0.0 ? 1.0 + p.pan : 1.0));
    }

    return {};
}

void DrumSampler::reset()
{
    for (auto& v : voices_)
        v = Voice();

    counter_ = 0;
    kit_.resetRoundRobin();
    lastHit_ = DrumHit();

    for (size_t i = 0; i < 128; ++i)
    {
        unmappedCount_[i].store (0);
        unmappedPiece_[i].store (-1);
    }
}

void DrumSampler::allNotesOff()
{
    for (auto& v : voices_)
        if (v.data != nullptr && v.fadeTotal == 0)
            fadeOut (v, chokeFade_);
}

void DrumSampler::fadeOut (Voice& v, int samples)
{
    v.fadeTotal = v.fadeLeft = std::max (1, samples);
}

DrumSampler::Voice* DrumSampler::allocate()
{
    Voice* oldest = nullptr;

    for (auto& v : voices_)
    {
        if (v.data == nullptr)
            return &v;

        if (oldest == nullptr || v.age < oldest->age)
            oldest = &v;
    }

    return oldest;      // the pool is full: the oldest voice of all is replaced
}

void DrumSampler::trigger (int note, int velocity)
{
    if (kit_.numPieces() == 0 || velocity <= 0)
        return;

    const auto hit = kit_.select (note, velocity);

    if (hit.piece < 0)
        return;

    lastHit_ = hit;

    if (hit.fallback && note >= 0 && note < 128)
    {
        unmappedCount_[(size_t) note].fetch_add (1);
        unmappedPiece_[(size_t) note].store (hit.piece);
    }

    const auto& piece = kit_.manifest().pieces[(size_t) hit.piece];

    // choke: this hit cuts the voices of the groups it names
    if (! piece.chokes.empty())
        for (auto& v : voices_)
        {
            if (v.data == nullptr || v.fadeTotal != 0)
                continue;

            const auto& vg = kit_.manifest().pieces[(size_t) v.piece].chokeGroup;

            if (! vg.empty() && std::find (piece.chokes.begin(), piece.chokes.end(), vg) != piece.chokes.end())
                fadeOut (v, chokeFade_);
        }

    // voice limit of the piece: fade out the oldest ones
    for (;;)
    {
        int count = 0;
        Voice* oldest = nullptr;

        for (auto& v : voices_)
            if (v.data != nullptr && v.piece == hit.piece && v.fadeTotal == 0)
            {
                ++count;

                if (oldest == nullptr || v.age < oldest->age)
                    oldest = &v;
            }

        if (count < piece.maxVoices || oldest == nullptr)
            break;

        fadeOut (*oldest, stealFade_);
    }

    auto* v = allocate();

    if (v == nullptr)
        return;

    const float g = pieceGain_[(size_t) hit.piece] * (float) hit.gain;
    *v = Voice();
    v->data = data_[(size_t) hit.piece][(size_t) hit.layer][(size_t) hit.sample];
    v->gainL = g * pieceBalanceL_[(size_t) hit.piece] * (1.0f / 32768.0f);
    v->gainR = g * pieceBalanceR_[(size_t) hit.piece] * (1.0f / 32768.0f);
    v->piece = hit.piece;
    v->age = ++counter_;
}

void DrumSampler::mix (float* left, float* right, int from, int to)
{
    if (to <= from)
        return;

    for (auto& v : voices_)
    {
        if (v.data == nullptr)
            continue;

        const auto* pcm = v.data->pcm.data();
        const std::size_t frames = v.data->frames;
        int i = from;

        for (; i < to && v.pos < frames; ++i, ++v.pos)
        {
            float g = 1.0f;

            if (v.fadeTotal > 0)
            {
                if (v.fadeLeft <= 0)
                    break;

                g = (float) v.fadeLeft / (float) v.fadeTotal;
                --v.fadeLeft;
            }

            left[i] += (float) pcm[v.pos * 2] * v.gainL * g;
            right[i] += (float) pcm[v.pos * 2 + 1] * v.gainR * g;
        }

        if (v.pos >= frames || (v.fadeTotal > 0 && v.fadeLeft <= 0))
            v = Voice();
    }
}

void DrumSampler::render (float* left, float* right, int numSamples, const DrumNoteEvent* events, int numEvents)
{
    int pos = 0;

    for (int e = 0; e < numEvents; ++e)
    {
        const int at = std::clamp (events[e].offset, pos, numSamples);
        mix (left, right, pos, at);
        pos = at;
        trigger (events[e].note, events[e].velocity);
    }

    mix (left, right, pos, numSamples);
}

int DrumSampler::activeVoices (int piece) const
{
    int n = 0;

    for (auto& v : voices_)
        if (v.data != nullptr && v.piece == piece && v.fadeTotal == 0)
            ++n;

    return n;
}

int DrumSampler::fadingVoices() const
{
    int n = 0;

    for (auto& v : voices_)
        if (v.data != nullptr && v.fadeTotal > 0)
            ++n;

    return n;
}

int DrumSampler::totalVoices() const
{
    int n = 0;

    for (auto& v : voices_)
        if (v.data != nullptr)
            ++n;

    return n;
}

json DrumSampler::unmappedNotes() const
{
    json out = json::object();

    for (size_t n = 0; n < 128; ++n)
        if (const auto count = unmappedCount_[n].load(); count > 0)
        {
            const int piece = unmappedPiece_[n].load();
            out[std::to_string (n)] = { { "count", count }, { "name", gmDrumName ((int) n) },
                                        { "played_as", piece >= 0 && piece < kit_.numPieces() ? kit_.manifest().pieces[(size_t) piece].id : std::string() } };
        }

    return out;
}

//==============================================================================
std::string gmDrumName (int note)
{
    static const char* names[] = {
        "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare", "Hand Clap", "Electric Snare", "Low Floor Tom", "Closed Hi-Hat",
        "High Floor Tom", "Pedal Hi-Hat", "Low Tom", "Open Hi-Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1", "High Tom", "Ride Cymbal 1",
        "Chinese Cymbal", "Ride Bell", "Tambourine", "Splash Cymbal", "Cowbell", "Crash Cymbal 2", "Vibraslap", "Ride Cymbal 2", "Hi Bongo",
        "Low Bongo", "Mute Hi Conga", "Open Hi Conga", "Low Conga", "High Timbale", "Low Timbale", "High Agogo", "Low Agogo", "Cabasa", "Maracas",
        "Short Whistle", "Long Whistle", "Short Guiro", "Long Guiro", "Claves", "Hi Wood Block", "Low Wood Block", "Mute Cuica", "Open Cuica",
        "Mute Triangle", "Open Triangle" };

    if (note < 35 || note > 81)
        return {};

    return names[note - 35];
}

} // namespace rovox
