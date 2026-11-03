// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The drum sampler without the audio framework: the kit manifest (kit.json: pieces, GM note mapping, velocity layers, round-robin samples,
// choke groups, per-piece gain / pan), the selection of a sample for a hit, and the voice engine that mixes the hits (sample-accurate, per-piece
// voice limit with oldest-steal, choke). The `rovox:drums` plugin (src/engine/DrumsPlugin.cpp) only decodes the audio files and feeds MIDI.
//
// Manifest (kit.json), all keys but "pieces" optional:
//   { "format": "rovox-drumkit", "version": 1, "id": "drskit", "name": "DRSKit", "license": "CC-BY-4.0", "attribution": "...", "source": "https://...",
//     "sample_rate": 44100,
//     "pieces": [ { "id": "snare", "name": "Snare", "category": "snare", "notes": [38, 40], "gain_db": 0, "pan": 0.0,
//                   "choke_group": "", "chokes": [], "max_voices": 4,
//                   "layers": [ { "vel_min": 1, "vel_max": 64, "samples": ["snare/a1.flac", "snare/a2.flac"] }, ... ] } ],
//     "fallbacks": { "55": "crash_left" },                           // optional: which piece plays a note the kit does not map
//     "files": { "snare/a1.flac": 12345, ... } }                    // sizes in bytes, used to verify a download
#pragma once

#include "Result.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rovox
{

struct DrumLayerDef
{
    int velMin = 1, velMax = 127;
    std::vector<std::string> samples;       // round-robin set, paths relative to the kit directory
};

struct DrumPieceDef
{
    std::string id, name, category;         // category: kick snare hihat tom cymbal perc (informational)
    std::vector<int> notes;                 // MIDI notes (0..127) that play this piece
    double gainDb = 0.0, pan = 0.0;         // pan -1..1 (balance of the stereo sample)
    std::string chokeGroup;                 // this piece's voices belong to the group ...
    std::vector<std::string> chokes;        // ... and a hit of this piece cuts the voices of these groups
    int maxVoices = 4;                      // simultaneous voices of the piece; the oldest is faded out when exceeded
    std::vector<DrumLayerDef> layers;
};

struct DrumKitManifest
{
    std::string id, name, version, license, attribution, source;
    int sampleRate = 44100;
    std::vector<DrumPieceDef> pieces;
    std::map<std::string, std::uint64_t> files;     // relative path -> size (may be empty)
    std::map<int, std::string> fallbacks;           // note -> piece id for notes the kit does not map (else the nearest mapped note decides)

    static Result<DrumKitManifest> fromJson (const json&);
    json toJson() const;
    /** Every sample path of the pieces, each once, in order of appearance. */
    std::vector<std::string> samplePaths() const;
};

/** What one hit plays. */
struct DrumHit
{
    int piece = -1;             // index into the manifest's pieces, -1 = nothing
    int layer = 0, sample = 0;
    double gain = 1.0;          // from the velocity position inside the layer
    bool fallback = false;      // the note is not mapped by the kit, the piece is the nearest one
};

/** A validated manifest with the note table and the round-robin state. Not thread-safe (one per audio thread). */
class DrumKit
{
public:
    static Result<DrumKit> create (DrumKitManifest);

    const DrumKitManifest& manifest() const         { return manifest_; }
    int numPieces() const                           { return (int) manifest_.pieces.size(); }

    /** The piece for a MIDI note: the one that maps it, else the piece with the nearest mapped note (lower wins a tie). -1 only for an empty kit. */
    int pieceForNote (int note, bool* fallback = nullptr) const;
    /** Index of the layer for a velocity 1..127 (layers are sorted by vel_min; a gap goes to the nearest layer). */
    int layerFor (int piece, int velocity) const;
    /** Picks the piece, layer and sample for a hit and advances that layer's round robin. velocity 1..127. */
    DrumHit select (int note, int velocity);
    void resetRoundRobin();
    int pieceById (const std::string& id) const;

private:
    DrumKitManifest manifest_;
    std::array<int, 128> noteToPiece_ {};
    std::array<bool, 128> mapped_ {};
    std::vector<std::vector<int>> roundRobin_;      // [piece][layer] next sample
};

/** Decoded stereo audio at the sampler's rate, 16-bit interleaved (half the memory of float). */
struct DrumSampleData
{
    std::vector<std::int16_t> pcm;
    std::size_t frames = 0;

    static std::shared_ptr<DrumSampleData> fromFloat (const float* left, const float* right, std::size_t frames);
};

using DrumSampleMap = std::map<std::string, std::shared_ptr<const DrumSampleData>>;

/** One MIDI event in a block. velocity 0 (or note-off) is ignored: drum samples play to the end. */
struct DrumNoteEvent
{
    int offset = 0;             // sample position inside the block
    int note = 0;
    int velocity = 0;
};

/** Mixes the hits of a kit into a stereo buffer. Use from one thread (the audio thread); the readers at the bottom are safe from any thread. */
class DrumSampler
{
public:
    static constexpr int maxTotalVoices = 128;

    explicit DrumSampler (double sampleRate);

    /** Fails if the manifest names a sample that `samples` does not have. */
    Result<void> setKit (DrumKit, DrumSampleMap samples);

    /** Adds the audio of `numSamples` frames to left / right. `events` sorted by offset (stable); offsets are clamped into the block. */
    void render (float* left, float* right, int numSamples, const DrumNoteEvent* events, int numEvents);
    void allNotesOff();
    void reset();

    // --- for tests and the plugin state ---
    int activeVoices (int piece) const;             // voices of the piece that are not fading out
    int fadingVoices() const;
    int totalVoices() const;
    const DrumHit& lastHit() const                  { return lastHit_; }
    const DrumKit& kit() const                      { return kit_; }
    /** Hits per MIDI note for the notes the kit does not map (note -> {count, piece id}) since the last call to reset(). Thread-safe. */
    json unmappedNotes() const;

private:
    struct Voice
    {
        const DrumSampleData* data = nullptr;
        std::size_t pos = 0;
        float gainL = 0, gainR = 0;
        int piece = -1;
        std::uint64_t age = 0;
        int fadeLeft = 0, fadeTotal = 0;            // fadeTotal > 0: fading out
    };

    void trigger (int note, int velocity);
    void mix (float* left, float* right, int from, int to);
    void fadeOut (Voice&, int samples);
    Voice* allocate();

    double sampleRate_;
    int stealFade_, chokeFade_;
    DrumKit kit_;
    DrumSampleMap samples_;
    std::vector<std::vector<std::vector<const DrumSampleData*>>> data_;     // [piece][layer][sample]
    std::vector<float> pieceGain_, pieceBalanceL_, pieceBalanceR_;
    std::array<Voice, maxTotalVoices> voices_ {};
    std::uint64_t counter_ = 0;
    DrumHit lastHit_;
    std::array<std::atomic<std::uint32_t>, 128> unmappedCount_ {};
    std::array<std::atomic<int>, 128> unmappedPiece_ {};
};

/** The General MIDI percussion name of a note ("Acoustic Bass Drum" for 35), empty outside 35..81. */
std::string gmDrumName (int note);

} // namespace rovox
