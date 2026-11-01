// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Audio import (task 130): probe -> plan -> apply. Probing, hashing and copying into media/ happen while planning
// (heavy, no Edit access); apply only builds tracks and clips and is a single undo step.
#pragma once

#include "Project.h"

#include <map>
#include <optional>
#include <vector>

namespace rovox
{

struct AudioInfo
{
    int channels = 0, bitDepth = 0;
    double sampleRate = 0;
    std::int64_t frames = 0;
    std::optional<double> bwfTimeReferenceS;    // BWF `bext` TimeReference in seconds, if present

    double durationS() const    { return sampleRate > 0 ? (double) frames / sampleRate : 0.0; }
    json toJson() const;
};

/** Reads format/length/BWF metadata with JUCE's reader. Fails for non-audio or unreadable files. */
Result<AudioInfo> probeAudio (const fs::path&);

/** Copies (hard link when the filesystem allows) `src` to `<mediaDir>/<sha256>.<ext>` unless already there; atomic. */
Result<fs::path> stageMedia (const fs::path& src, const std::string& sha256, const fs::path& mediaDir);

struct ImportOptions
{
    std::string preset = "auto";                // auto | zoom_r20 | generic
    std::string align = "start";                // start | bwf | none
    std::string takesMode = "separate";         // separate | latest | lanes (lanes falls back to separate with a warning)
    std::map<std::string, std::string> nameMap; // source key ("REC01") or planned name -> track name
};

struct ImportWarning
{
    std::string code, message, file;
    json toJson() const;
};

struct ImportClipPlan
{
    std::string origName, sha256, mediaPath;    // mediaPath relative to the project dir ("media/<sha>.wav")
    AudioInfo info;
    double startS = 0;
    json toJson() const;
};

struct ImportTrackPlan
{
    std::string name, sourceKey, color, roleGuess;
    std::vector<ImportClipPlan> clips;
    json toJson() const;
};

struct ImportPlan
{
    std::string planId, detectedSource = "generic";
    std::vector<ImportTrackPlan> tracks;
    std::vector<ImportWarning> warnings;

    json toJson() const;
    static Result<ImportPlan> fromJson (const json&);
};

/** The options and source list of an `import.plan` request (used by the command and by the MCP job). `params` must have been validated. */
ImportOptions importOptionsFrom (const json& params);
std::vector<fs::path> importSourcesFrom (const json& params);

/** Pure planning: expands folders (a `.zprj` folder or any directory) and files, probes, hashes and stages media. */
Result<ImportPlan> planImport (const std::vector<fs::path>& sources, const fs::path& mediaDir, const ImportOptions&);

/** Names of the WAV files listed between SEQD and SEQE in R20PRJDATA.r20prj (the project's sequence), empty if unreadable. */
std::vector<std::string> readR20SequenceFiles (const fs::path& r20prj);

/** Creates tracks and clips from a plan. Message thread, inside a command (one undo step). */
json applyImportPlan (Project&, const ImportPlan&, const std::map<std::string, std::string>& rename);

/** Registers a media file in the manifest (idempotent by sha256). */
void registerMedia (Project&, const ImportClipPlan&);

} // namespace rovox
