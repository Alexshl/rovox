// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Where the drum kits come from. The kits are not part of the repository or of the Docker image; a kit is a directory with kit.json (see DrumKit.h)
// and its FLAC / WAV samples, found in this order:
//
//   1. $ROVOX_DRUMKITS_PATH (colon separated): <dir>/<kit id>/kit.json, or <dir>/kit.json when that manifest has the id (offline / development)
//   2. the object store: <S3_PREFIX>assets/drumkits/<kit id>/LATEST (text: the sha) and <S3_PREFIX>assets/drumkits/<kit id>/<sha>/{kit.json,<samples>},
//      downloaded once into <data dir>/cache/drumkits/<kit id>/<sha>/ (sha = SHA-256 of kit.json)
//   3. a kit already in that cache (when the store cannot be reached)
//
// Nothing here runs on the audio thread: the plugin resolves the kit in a loader thread.
#pragma once

#include "ObjectStore.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rovox
{

struct DrumKitLocation
{
    std::filesystem::path dir;      // contains kit.json
    std::string source;             // "ROVOX_DRUMKITS_PATH" | "object store" | "cache"
    std::string sha;                // empty for a local directory
};

class DrumKitLibrary
{
public:
    using Progress = std::function<void (const std::string&)>;

    static DrumKitLibrary& instance();

    /** <data dir>/cache/drumkits. Without it nothing is downloaded. */
    void setCacheDir (const std::filesystem::path&);
    /** The bucket and key prefix (StorageConfig::prefix); a null store turns the download off. */
    void setObjectStore (std::shared_ptr<ObjectStore>, std::string keyPrefix);
    /** Test hook: replaces $ROVOX_DRUMKITS_PATH; nullopt goes back to the environment. */
    void setSearchPaths (std::optional<std::vector<std::filesystem::path>>);

    /** The directories of $ROVOX_DRUMKITS_PATH (read on every call). */
    static std::vector<std::filesystem::path> environmentPaths();
    /** "<prefix>assets/drumkits/<kit id>/" */
    static std::string objectPrefix (const std::string& keyPrefix, const std::string& kitId);
    static std::string normaliseKitId (std::string);

    /** The kit directory, downloading it if needed (may take a while: call it from a worker thread). NOT_FOUND with the places searched if there is none. */
    Result<DrumKitLocation> resolve (const std::string& kitId, const Progress& = {});

private:
    Result<DrumKitLocation> download (ObjectStore&, const std::string& kitId, const std::string& keyPrefix, const std::string& sha, const std::filesystem::path& cache, const Progress&);
    std::optional<DrumKitLocation> fromCache (const std::filesystem::path& cache, const std::string& kitId, const std::string& sha);

    std::mutex mutex_;          // guards the settings
    std::mutex downloadMutex_;  // one download at a time
    std::filesystem::path cacheDir_;
    std::shared_ptr<ObjectStore> store_;
    std::string keyPrefix_;
    std::optional<std::vector<std::filesystem::path>> searchPaths_;
};

} // namespace rovox
