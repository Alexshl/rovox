// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "DrumKitLibrary.h"

#include "DrumKit.h"
#include "RemoteStorage.h"
#include "Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
std::string trim (std::string s)
{
    while (! s.empty() && std::isspace ((unsigned char) s.back()))
        s.pop_back();

    size_t i = 0;

    while (i < s.size() && std::isspace ((unsigned char) s[i]))
        ++i;

    return s.substr (i);
}

bool isSha (const std::string& s)
{
    return s.size() == 64 && std::all_of (s.begin(), s.end(), [] (unsigned char c) { return std::isxdigit (c) != 0; });
}

std::optional<json> readJson (const fs::path& file)
{
    std::ifstream in (file, std::ios::binary);

    if (! in)
        return std::nullopt;

    std::stringstream ss;
    ss << in.rdbuf();
    auto j = json::parse (ss.str(), nullptr, false);
    return j.is_discarded() ? std::nullopt : std::optional<json> (j);
}
} // namespace

DrumKitLibrary& DrumKitLibrary::instance()
{
    static DrumKitLibrary library;
    return library;
}

void DrumKitLibrary::setCacheDir (const fs::path& dir)
{
    std::lock_guard<std::mutex> lock (mutex_);
    cacheDir_ = dir;
}

void DrumKitLibrary::setObjectStore (std::shared_ptr<ObjectStore> store, std::string keyPrefix)
{
    std::lock_guard<std::mutex> lock (mutex_);
    store_ = std::move (store);
    keyPrefix_ = StorageKeys::normalisePrefix (std::move (keyPrefix));
}

void DrumKitLibrary::setSearchPaths (std::optional<std::vector<fs::path>> paths)
{
    std::lock_guard<std::mutex> lock (mutex_);
    searchPaths_ = std::move (paths);
}

std::vector<fs::path> DrumKitLibrary::environmentPaths()
{
    std::vector<fs::path> out;
    const char* env = std::getenv ("ROVOX_DRUMKITS_PATH");

    if (env == nullptr)
        return out;

    std::stringstream ss (env);
    std::string part;

    while (std::getline (ss, part, ':'))
        if (! part.empty())
            out.emplace_back (part);

    return out;
}

std::string DrumKitLibrary::objectPrefix (const std::string& keyPrefix, const std::string& kitId)
{
    return keyPrefix + "assets/drumkits/" + kitId + "/";
}

std::string DrumKitLibrary::normaliseKitId (std::string id)
{
    std::transform (id.begin(), id.end(), id.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return trim (id);
}

std::optional<DrumKitLocation> DrumKitLibrary::fromCache (const fs::path& cache, const std::string& kitId, const std::string& sha)
{
    std::error_code ec;

    if (cache.empty())
        return std::nullopt;

    const auto base = cache / kitId;

    if (! sha.empty())
    {
        if (fs::exists (base / sha / "kit.json", ec))
            return DrumKitLocation { base / sha, "cache", sha };

        return std::nullopt;
    }

    std::optional<DrumKitLocation> best;
    fs::file_time_type bestTime {};

    for (fs::directory_iterator it (base, ec), end; ! ec && it != end; it.increment (ec))
    {
        const auto manifest = it->path() / "kit.json";

        if (it->path().filename().string().rfind ('.', 0) == 0 || ! fs::exists (manifest, ec))
            continue;

        const auto t = fs::last_write_time (manifest, ec);

        if (! best || t > bestTime)
        {
            best = DrumKitLocation { it->path(), "cache", it->path().filename().string() };
            bestTime = t;
        }
    }

    return best;
}

Result<DrumKitLocation> DrumKitLibrary::download (ObjectStore& store, const std::string& kitId, const std::string& keyPrefix, const std::string& sha,
                                                   const fs::path& cache, const Progress& progress)
{
    std::lock_guard<std::mutex> lock (downloadMutex_);

    if (auto cached = fromCache (cache, kitId, sha))      // somebody else finished it while we waited
        return *cached;

    const auto prefix = objectPrefix (keyPrefix, kitId) + sha + "/";
    auto manifestText = store.get (prefix + "kit.json");

    if (! manifestText.ok())
        return makeError (manifestText.error().code, "drum kit " + kitId + ": cannot read " + prefix + "kit.json: " + manifestText.error().message);

    if (sha256Hex (manifestText.value()) != sha)
        return makeError (errc::io, "drum kit " + kitId + ": kit.json in the object store does not match its sha " + sha);

    auto parsed = json::parse (manifestText.value(), nullptr, false);

    if (parsed.is_discarded())
        return makeError (errc::io, "drum kit " + kitId + ": kit.json is not valid JSON");

    auto manifest = DrumKitManifest::fromJson (parsed);

    if (! manifest.ok())
        return manifest.error();

    std::error_code ec;
    const auto part = cache / kitId / ("." + sha + ".part");
    fs::create_directories (part, ec);

    if (ec)
        return makeError (errc::io, "cannot create " + part.string() + ": " + ec.message());

    const auto paths = manifest.value().samplePaths();
    size_t done = 0;

    for (auto& rel : paths)
    {
        const auto dest = part / rel;
        const auto expected = manifest.value().files.find (rel);

        if (fs::exists (dest, ec) && expected != manifest.value().files.end() && fs::file_size (dest, ec) == expected->second)
        {
            ++done;                                 // left by an interrupted download
            continue;
        }

        fs::create_directories (dest.parent_path(), ec);

        if (auto r = store.getFile (prefix + rel, dest); ! r.ok())
            return makeError (r.error().code, "drum kit " + kitId + ": download of " + rel + " failed: " + r.error().message);

        if (expected != manifest.value().files.end() && fs::file_size (dest, ec) != expected->second)
        {
            fs::remove (dest, ec);
            return makeError (errc::io, "drum kit " + kitId + ": " + rel + " has the wrong size after download");
        }

        if (progress && (++done % 25 == 0 || done == paths.size()))
            progress ("downloaded " + std::to_string (done) + " of " + std::to_string (paths.size()) + " samples");
    }

    {
        std::ofstream out (part / "kit.json", std::ios::binary);
        out << manifestText.value();
    }

    const auto finalDir = cache / kitId / sha;
    fs::remove_all (finalDir, ec);
    fs::rename (part, finalDir, ec);

    if (ec)
        return makeError (errc::io, "cannot move the downloaded kit into place: " + ec.message());

    return DrumKitLocation { finalDir, "object store", sha };
}

Result<DrumKitLocation> DrumKitLibrary::resolve (const std::string& rawId, const Progress& progress)
{
    const auto kitId = normaliseKitId (rawId);

    if (kitId.empty() || kitId.find_first_of ("/\\") != std::string::npos || kitId.find ("..") != std::string::npos)
        return makeError (errc::invalidParams, "invalid drum kit id: '" + rawId + "'");

    std::vector<fs::path> paths;
    fs::path cache;
    std::shared_ptr<ObjectStore> store;
    std::string keyPrefix;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        paths = searchPaths_ ? *searchPaths_ : environmentPaths();
        cache = cacheDir_;
        store = store_;
        keyPrefix = keyPrefix_;
    }

    std::error_code ec;
    std::string searched;

    for (auto& dir : paths)
    {
        searched += (searched.empty() ? "" : ", ") + dir.string();

        if (fs::exists (dir / kitId / "kit.json", ec))
            return DrumKitLocation { dir / kitId, "ROVOX_DRUMKITS_PATH", "" };

        if (auto j = readJson (dir / "kit.json"); j && j->is_object() && normaliseKitId (j->value ("id", "")) == kitId)
            return DrumKitLocation { dir, "ROVOX_DRUMKITS_PATH", "" };
    }

    std::string storeError;

    if (store != nullptr && ! cache.empty())
    {
        const auto base = objectPrefix (keyPrefix, kitId);
        std::string sha;
        auto latest = store->get (base + "LATEST");

        if (latest.ok())
        {
            sha = trim (latest.value());
        }
        else if (latest.error().code == errc::notFound)
        {
            if (auto listed = store->list (base, "/"); listed.ok())
                for (auto& p : listed.value().prefixes)
                {
                    auto candidate = p.substr (base.size());
                    candidate = candidate.substr (0, candidate.find ('/'));

                    if (isSha (candidate) && candidate > sha)
                        sha = candidate;
                }
        }
        else
        {
            storeError = latest.error().message;
        }

        if (isSha (sha))
        {
            if (auto cached = fromCache (cache, kitId, sha))
                return *cached;

            auto got = download (*store, kitId, keyPrefix, sha, cache, progress);

            if (got.ok())
                return got.value();

            storeError = got.error().message;
        }
        else if (storeError.empty())
        {
            storeError = "no kit " + kitId + " under " + base + " in the object store";
        }
    }

    if (auto cached = fromCache (cache, kitId, ""))
        return *cached;

    std::string msg = "drum kit '" + kitId + "' is not available. ";
    msg += paths.empty() ? "ROVOX_DRUMKITS_PATH is not set" : "not in ROVOX_DRUMKITS_PATH (" + searched + ")";
    msg += store == nullptr ? "; no object storage is configured (S3_ENDPOINT / S3_BUCKET), so nothing can be downloaded" : "; object store: " + storeError;
    msg += ". Convert the kit with tools/drumkit/build_drskit.py and either upload it to <S3_PREFIX>assets/drumkits/" + kitId + "/<sha>/ or point ROVOX_DRUMKITS_PATH at the output directory.";
    return makeError (errc::notFound, msg);
}

} // namespace rovox
