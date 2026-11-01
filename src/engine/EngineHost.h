// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Cgroup.h"
#include "Tracktion.h"

#include <memory>
#include <optional>
#include <string_view>

namespace rovox
{

enum class AudioMode { None, BrowserStream, Device };

std::optional<AudioMode> parseAudioMode (std::string_view);
const char* toString (AudioMode);

struct EngineConfig
{
    fs::path dataDir;                       // writable root: projects/, uploads/, config/, cache/ (Tracktion prefs + temp files)
    AudioMode audio = AudioMode::None;
    fs::path pluginsDir;                    // extra search path for VST3/LV2 (mounted at /plugins in the container)
    bool scanPluginsOutOfProcess = false;   // true only in a process whose main() calls PluginScanChild::run first
};

/** Data directory: explicit value, else $ROVOX_DATA_DIR, else $XDG_DATA_HOME/rovox, else $HOME/.local/share/rovox.
    The Engine otherwise writes preferences and temp files to $HOME/.config, which does not exist or is read-only in containers. */
fs::path resolveDataDir (const fs::path& explicitDir);

/** Engine, with every default that does not suit a headless server overridden:
    prefs/cache under the data dir, cgroup-aware thread count, raised Edit limits, no system audio devices unless asked.
    Create on the message thread (the process main thread). One per process. */
class EngineHost
{
public:
    explicit EngineHost (const EngineConfig&);
    ~EngineHost();

    te::Engine& engine()                    { return *engine_; }
    const EngineConfig& config() const      { return config_; }
    fs::path dataDir() const                { return config_.dataDir; }
    fs::path projectsDir() const            { return config_.dataDir / "projects"; }
    CpuBudget cpuBudget() const             { return budget_; }
    AudioMode audioMode() const             { return config_.audio; }

    /** none: no device is ever opened; device: JUCE default device; browser-stream: not part of M1 (returns an error). */
    Result<void> setAudioMode (AudioMode);

private:
    // Declared first, destroyed last: JUCE needs to outlive everything below.
    juce::ScopedJuceInitialiser_GUI juceInit_;
    EngineConfig config_;
    CpuBudget budget_;
    std::unique_ptr<te::Engine> engine_;
};

} // namespace rovox
