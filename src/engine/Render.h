// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Offline render with preflight and post-render checks (task 160). A render never silently produces a wrong file:
// offline media, plugins that failed to load, MIDI tracks whose instrument is silent, silent or clipping output
// and missed loudness targets are reported, and the failing ones stop the render before a file is published.
#pragma once

#include "Project.h"

#include <functional>
#include <set>

namespace rovox
{

struct RenderFormat
{
    std::string container = "wav";      // wav | flac | mp3
    int bitDepth = 24;                  // wav: 16 | 24 | 32 (32 = float); flac: 16 | 24
    int bitrateKbps = 320;              // mp3
    double sampleRate = 0;              // 0 = project sample rate
    int channels = 2;                   // 1 | 2
};

struct RenderRange
{
    std::optional<double> startS, endS;         // seconds; absent = project start / end
    std::optional<int> startBar, endBar;        // 1-based bars; endBar is exclusive. Uses the time signature at the start of the project
};

struct LoudnessOptions
{
    std::string mode = "none";          // none | check | normalize
    double targetLufs = -14.0;
    double toleranceLu = 1.0;
    double truePeakCeilingDb = -1.0;    // normalize never lets the true peak pass this; the target is then reported as not reached (= ceiling_dbtp of the limiter)
    std::optional<bool> limiter;        // master true-peak limiter; absent = on for normalize, off otherwise. normalize then iterates gain + limiter to the target
};

/** Optional hooks used by jobs: cancellation, progress and a tap on the final (post-gain) audio of each output, for analysis. */
struct RenderHooks
{
    std::function<bool()> shouldCancel;                                         // polled about every 100 ms and between phases
    std::function<void (double fraction, const std::string& stage)> onProgress; // fraction 0..1 of the whole render
    /** Called block by block with the audio that is written to the file (planar float). outputIndex follows the outputs of the request (one per stem). */
    std::function<void (int outputIndex, const float* const* planar, int channels, int frames)> audioTap;
};

struct RenderRequest
{
    std::string name = "mix";                   // renders/<name>.<ext>, overwritten atomically
    std::string scope = "master";               // master | tracks | stems
    std::vector<std::string> trackIds;          // for tracks / stems
    bool stemsThroughMaster = false;
    RenderRange range;
    double tailS = 2.0;                         // up to this long after the range end, cut at the first silent block
    RenderFormat format;
    LoudnessOptions loudness;
    std::set<std::string> failOn { "offline_media", "silent_output", "silent_track", "clipping", "plugin_not_loaded" };
    std::set<std::string> warnOn { "empty_track", "true_peak_over:-1.0" };
    std::int64_t clipTolerance = 0;             // clipped samples allowed before `clipping` fires
    std::optional<std::string> seed;            // recorded in the metadata; the built-in engine is deterministic without it
    bool preflightOnly = false;
    double stallTimeoutS = 60;                  // a render that makes no progress for this long is cancelled
    RenderHooks hooks;                          // a cancelled render returns CANCELLED and leaves no file behind
};

struct Finding
{
    std::string check, severity, trackId, name, message;
    json details = nullptr;

    json toJson() const;
};

struct PreflightReport
{
    std::vector<Finding> findings;
    json tracks = json::array();                // per-track presence info gathered by the instrument probes

    bool passed() const;
    json toJson() const;
};

struct RenderContext
{
    fs::path rendersDir;
    std::string projectId;
    std::uint64_t sourceRevision = 0;
    double projectSampleRate = 44100.0;
};

/** Resolves bars/seconds against the Edit; end defaults to the end of the content. */
Result<te::TimeRange> resolveRange (te::Edit&, const RenderRange&);

/** Checks of task 160 on the Edit itself. Runs on the thread that owns the Edit. `tempDir` holds the short probe renders of instrument tracks. */
PreflightReport runPreflight (te::Edit&, const RenderRequest&, te::TimeRange range, double sampleRate, const fs::path& tempDir);

/** Spike P2: LV2 plugins report latency only after the first processBlock, so PDC is wrong when the graph is built before any
    audio ran. Pushes `blocks` silent blocks through every external plugin instance. */
void primePlugins (te::Edit&, double sampleRate, int blocks = 8);

/** Renders an Edit that is not otherwise in use (a snapshot). Blocking. */
Result<json> renderEdit (te::Edit&, const RenderContext&, const RenderRequest&);

/** Snapshot of `project` (state copy, same media paths) rendered in place. Message thread; blocks until done.
    Changes made to the project during the call cannot happen (single thread), and a snapshot taken by a server job
    keeps `source_revision` stable by construction. */
Result<json> renderProject (Project&, const RenderRequest&);

/** Same, but the caller supplies the snapshot (for jobs that render on a worker thread). */
std::unique_ptr<te::Edit> makeRenderSnapshot (Project&);

/** The 60.000 s workaround, exposed for the test: a range this long is rendered one sample longer. */
bool needsSixtySecondWorkaround (double lengthS);

/** Writes a float WAV with the raw Tracktion render of a range; used by the render path and by the reproduction test. */
Result<void> renderRawWav (te::Edit&, const fs::path& dest, te::TimeRange range, double tailS, double sampleRate,
                           const juce::BigInteger& tracks, bool useMasterPlugins, double stallTimeoutS, bool applySixtySecondWorkaround = true,
                           const RenderHooks* hooks = nullptr, double progressFrom = 0.0, double progressTo = 1.0);

} // namespace rovox
