// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "EngineHost.h"

#include "PluginCatalog.h"

#include <algorithm>
#include <cstdlib>

namespace rovox
{

namespace
{
/** Prefs and Tracktion temp files go below the data dir instead of $HOME/.config/<app>. */
class DataDirPropertyStorage : public te::PropertyStorage
{
public:
    DataDirPropertyStorage (const fs::path& dataDir) : te::PropertyStorage ("rovox"), cacheDir_ (toFile (dataDir / "cache"))
    {
        prefsDir_ = cacheDir_.getChildFile ("prefs");
    }

    juce::File getAppPrefsFolder() override
    {
        prefsDir_.createDirectory();
        return prefsDir_;
    }

    juce::File getAppCacheFolder() override
    {
        cacheDir_.createDirectory();
        return cacheDir_;
    }

private:
    juce::File cacheDir_, prefsDir_;
};

class RovoxEngineBehaviour : public te::EngineBehaviour
{
public:
    RovoxEngineBehaviour (const EngineConfig& c, CpuBudget b) : config (c), budget (b) {}

    // JUCE reports the host CPU count inside `docker run --cpus=N`; follow the cgroup quota instead (spike P5: 40-80 % faster offline).
    int getNumberOfCPUsToUseForAudio() override     { return budget.cpus; }
    bool canScanPluginsOutOfProcess() override      { return config.scanPluginsOutOfProcess; }
    bool addSystemAudioIODeviceTypes() override     { return config.audio == AudioMode::Device; }
    bool autoInitialiseDeviceManager() override     { return false; }

    // Spike P5: the default (400 tracks) makes Edit::insertTrack return null and ensureNumberOfAudioTracks spin forever.
    te::EditLimits getEditLimits() override
    {
        te::EditLimits l;
        l.maxNumTracks = 4096;
        l.maxClipsInTrack = 100000;
        l.maxPluginsOnTrack = 64;
        l.maxNumMasterPlugins = 16;
        return l;
    }

private:
    EngineConfig config;
    CpuBudget budget;
};
} // namespace

std::optional<AudioMode> parseAudioMode (std::string_view s)
{
    if (s == "none")                                    return AudioMode::None;
    if (s == "browser" || s == "browser-stream")        return AudioMode::BrowserStream;
    if (s == "device")                                  return AudioMode::Device;
    return std::nullopt;
}

const char* toString (AudioMode m)
{
    switch (m)
    {
        case AudioMode::None:           return "none";
        case AudioMode::BrowserStream:  return "browser-stream";
        case AudioMode::Device:         return "device";
    }

    return "none";
}

fs::path resolveDataDir (const fs::path& explicitDir)
{
    if (! explicitDir.empty())
        return explicitDir;

    if (const char* env = std::getenv ("ROVOX_DATA_DIR"); env != nullptr && *env != 0)
        return env;

    if (const char* xdg = std::getenv ("XDG_DATA_HOME"); xdg != nullptr && *xdg != 0)
        return fs::path (xdg) / "rovox";

    if (const char* home = std::getenv ("HOME"); home != nullptr && *home != 0)
        return fs::path (home) / ".local" / "share" / "rovox";

    return fs::temp_directory_path() / "rovox";
}

namespace
{
/** lilv and the VST3 scanner read LV2_PATH / VST3_PATH when the plugin formats are created, so the search paths must be settled before the engine.
    The plugin directory (--plugins, /plugins in the container) is added to the system locations. */
void settlePluginSearchPaths (const fs::path& pluginsDir)
{
    auto settle = [&] (const char* var, const std::string& systemDefault, const std::vector<std::string>& extra)
    {
        std::string value = std::getenv (var) != nullptr && *std::getenv (var) != 0 ? std::getenv (var) : systemDefault;

        for (auto& e : extra)
            if (! e.empty() && value.find (e) == std::string::npos)
                value += ":" + e;

        ::setenv (var, value.c_str(), 1);
    };

    const std::string dir = pluginsDir.string();
    settle ("LV2_PATH", "/usr/lib/lv2:/usr/local/lib/lv2", { "/plugins/lv2", dir.empty() ? "" : dir + "/lv2", dir });
    settle ("VST3_PATH", "/usr/lib/vst3:/usr/local/lib/vst3", { "/plugins/vst3", dir.empty() ? "" : dir + "/vst3", dir });
}
} // namespace

EngineHost::EngineHost (const EngineConfig& c) : config_ (c), budget_ (rovox::cpuBudget())
{
    settlePluginSearchPaths (config_.pluginsDir);

    if (config_.dataDir.empty())
        config_.dataDir = resolveDataDir ({});

    std::error_code ec;
    fs::create_directories (config_.dataDir / "projects", ec);
    fs::create_directories (config_.dataDir / "uploads", ec);
    fs::create_directories (config_.dataDir / "cache", ec);

    engine_ = std::make_unique<te::Engine> (std::make_unique<DataDirPropertyStorage> (config_.dataDir),
                                            std::make_unique<te::UIBehaviour>(),
                                            std::make_unique<RovoxEngineBehaviour> (config_, budget_));

    if (config_.audio == AudioMode::Device)
        setAudioMode (AudioMode::Device);
    else if (config_.audio == AudioMode::BrowserStream)
        stream_ = std::make_unique<BrowserStream> (*engine_);
}

PluginCatalog& EngineHost::catalog()
{
    if (catalog_ == nullptr)
        catalog_ = std::make_unique<PluginCatalog> (*this);

    return *catalog_;
}

EngineHost::~EngineHost()
{
    catalog_.reset();
    if (stream_ != nullptr)
        engine_->getDeviceManager().closeDevices();         // no more callbacks into the stream

    stream_.reset();
    engine_.reset();
}

Result<fs::path> EngineHost::checkPath (const fs::path& p) const
{
    if (! config_.restrictPaths)
        return p;

    std::error_code ec;
    const auto resolved = fs::weakly_canonical (fs::absolute (p), ec);
    auto inside = [&] (const fs::path& root)
    {
        const auto r = fs::weakly_canonical (root, ec);
        auto [a, b] = std::mismatch (r.begin(), r.end(), resolved.begin(), resolved.end());
        return a == r.end();
    };

    if (! ec && (inside (config_.dataDir) || std::any_of (config_.allowedRoots.begin(), config_.allowedRoots.end(), inside)))
        return resolved;

    return makeError (errc::forbiddenPath, "path is outside the allowed directories: " + p.string(), json { { "path", p.string() } });
}

Result<void> EngineHost::setAudioMode (AudioMode mode)
{
    if (mode == AudioMode::BrowserStream)
        return config_.audio == AudioMode::BrowserStream ? Result<void>() : Result<void> (makeError (errc::preconditionFailed, "the engine was created without the browser-stream device"));

    if (mode == AudioMode::Device)
    {
        if (config_.audio != AudioMode::Device)
            return makeError (errc::preconditionFailed, "the engine was created without system audio device types");

        engine_->getDeviceManager().initialise (2, 2);
    }
    else if (config_.audio == AudioMode::Device)
    {
        engine_->getDeviceManager().closeDevices();
    }

    return {};
}

} // namespace rovox
