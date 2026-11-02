// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The `.rvx` project container (task 110): a directory holding
//   edit.xml      Tracktion Edit ValueTree as XML, our properties live in the `rovox:` namespace
//   rovox.json    manifest: schema, project_id, revision, media index, render/delivery defaults
//   media/        immutable, content-addressed (sha256) audio; renders/, peaks/, autosave/
// Save order is edit.xml first, rovox.json last (the manifest pins the revision). An interrupted save leaves either the
// old pair or a new edit.xml with an old manifest; readProject recovers the second case.
#pragma once

#include "Tracktion.h"

#include <optional>
#include <vector>

namespace rovox
{

inline constexpr int currentSchema = 1;
inline constexpr const char* editFileName = "edit.xml";
inline constexpr const char* manifestFileName = "rovox.json";
inline constexpr int autosaveKeep = 20;

namespace prop
{
inline const juce::Identifier id ("rovox:id");
inline const juce::Identifier revision ("rovox:revision");
inline const juce::Identifier schema ("rovox:schema");
} // namespace prop

struct MediaEntry
{
    std::string mediaId, path, sha256, origName;
    int channels = 0, bitDepth = 0;
    double sampleRate = 0;
    std::int64_t frames = 0;
    bool external = false;      // referenced in place (RPP import with copy_media=false); path is absolute

    json toJson() const;
    static MediaEntry fromJson (const json&);
};

struct Manifest
{
    int schema = currentSchema;
    std::string projectId, name = "Untitled";
    std::uint64_t revision = 0;
    double sampleRate = 44100.0;
    std::string createdAt, savedAt;
    std::vector<MediaEntry> media;
    json renderPresets = json::object();
    json deliveryDefaults = json::object();
    std::string tenantId = "local";     // absent in old files = "local"
    bool ephemeral = false;             // absent = false
    std::optional<int> ttlS;

    json toJson() const;
    /** Applies schema migrations first, then parses. A newer schema than we know is an error. */
    static Result<Manifest> fromJson (json);
    const MediaEntry* findMedia (const std::string& sha256) const;
};

/** Migrates a manifest document from its own `schema` to currentSchema (0 -> 1: legacy key names). */
Result<json> migrateManifest (json doc);

/** Hook for ValueTree -> ValueTree migrations of edit.xml (none needed yet; schema 1 is the first). */
void migrateEditState (juce::ValueTree& state, int fromSchema);

std::string nowIso8601();

enum class SaveFault { none, afterTempFiles, betweenRenames };     // test hooks: simulate a crash at that point

struct SaveOptions
{
    bool fsync = true;
    SaveFault fault = SaveFault::none;
};

struct LoadedProject
{
    juce::ValueTree editState;
    Manifest manifest;
    std::vector<std::string> warnings;
    std::optional<std::uint64_t> newerAutosave;     // an autosave with a higher revision exists (crash recovery offer)
};

/** Writes edit.xml + rovox.json atomically (temp files, fsync, rename; manifest last). `editState` gets rovox:revision. */
Result<void> writeProject (const fs::path& dir, const juce::ValueTree& editState, const Manifest&, const SaveOptions& = {});

/** Reads a project directory, repairing the "edit.xml newer than rovox.json" state. */
Result<LoadedProject> readProject (const fs::path& dir);

/** autosave/<revision>.rvx/ with rotation (keeps the newest autosaveKeep). */
Result<void> writeAutosave (const fs::path& dir, const juce::ValueTree& editState, const Manifest&);
std::vector<std::uint64_t> listAutosaves (const fs::path& dir);      // ascending revisions
Result<LoadedProject> readAutosave (const fs::path& dir, std::uint64_t revision);

/** Track count and length of an edit.xml without loading an Edit (project.catalog of a closed project, the HEAD object of the bucket). */
struct EditSummary
{
    int tracks = 0;
    double durationS = 0;
};
EditSummary summarizeEdit (const fs::path& editXml);

/** Atomic text write: path.tmp, fsync, rename. */
Result<void> writeFileAtomic (const fs::path& path, const std::string& content, bool fsync = true);
void fsyncDirectory (const fs::path& dir);

} // namespace rovox
