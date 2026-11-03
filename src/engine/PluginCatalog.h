// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Plugin catalog (task 400): what the agent can load. Plugins are found by the (out-of-process) scan, probed in a child process
// before first use, and remembered in <data>/catalog/{plugins,blacklist,probe,tags}.json. Connectors say where a plugin runs;
// the MVP has `local` only (remote-node and wine are declared and report why they are unavailable).
#pragma once

#include "EngineHost.h"

#include <atomic>
#include <mutex>
#include <optional>

namespace rovox
{

/** "lv2:<uri>", "vst3:<file>#<hex id>": the catalog key of a plugin description. */
std::string pluginUidFor (const juce::PluginDescription&);

/** Where a plugin runs. `local` hosts plugins installed on this server (in child processes); `remote-node` (a Rovox node on the user's Mac for AU / VST3)
    and `wine` (Windows plugins through yabridge) are declared so that the catalog and the tools have the vocabulary, and report why they are unavailable. */
class PluginConnector
{
public:
    virtual ~PluginConnector() = default;
    virtual std::string id() const = 0;
    virtual std::string description() const = 0;
    virtual bool available() const = 0;
    virtual std::string unavailableReason() const    { return {}; }
    json describe() const;
};

std::vector<std::unique_ptr<PluginConnector>> makeConnectors();

struct CatalogQuery
{
    std::string kind, query, format, status, tag;       // kind: effect | instrument
    bool includePresets = false;
    size_t limit = 50, offset = 0;
};

class PluginCatalog
{
public:
    explicit PluginCatalog (EngineHost&);

    /** Scans the LV2 / VST3 search paths (blocking, any thread but one scan at a time). Unchanged search paths give the cached
        catalog at once unless `force`. */
    json rescan (bool force = false);

    /** Ends a running scan after the file it is on and stops later ones (the server is shutting down). The unfinished scan is not recorded:
        the catalog keeps what it had, and the next start scans again. */
    void cancelScan() noexcept      { cancelled_ = true; }

    json query (const CatalogQuery&);

    /** Record by plugin_uid ("lv2:<uri>") or, failing that, by exact name (then the first match). nullopt if unknown. */
    std::optional<json> find (const std::string& uidOrName) const;
    static std::optional<juce::PluginDescription> descriptionOf (const json& record);

    /** Loads the plugin in a child process once per catalog fingerprint. A crash blacklists it (PLUGIN_CRASHED), a load failure
        gives PLUGIN_LOAD_FAILED. Returns the probe info. */
    Result<json> ensureProbed (const std::string& pluginUid);

    std::optional<std::string> blacklistReason (const std::string& pluginUid) const;
    void blacklist (const std::string& pluginUid, const std::string& reason);
    void unblacklist (const std::string& pluginUid);

    Result<json> setTags (const std::string& pluginUid, const std::vector<std::string>& tags);

    /** Presets of a plugin: factory programs (listed by a child process, cached) and user presets saved with fx.preset_save. */
    Result<json> presetsOf (const std::string& pluginUid);
    static std::string presetId (const std::string& pluginUid, const std::string& name);
    fs::path userPresetDir (const std::string& pluginUid) const;

    /** Adds every cataloged plugin to the engine's own list of known plugins, which Tracktion consults when it loads a plugin of an Edit.
        A process that did not scan (the render child) needs this before it opens a project. */
    void registerWithEngine();

    json connectors() const;
    json scanState() const;

    size_t size() const;

private:
    json summary (const json& record, bool withPresets);
    void loadFiles();
    void saveCatalog();
    void saveJson (const char* file, const json&) const;
    json loadJson (const char* file) const;
    std::string fingerprint() const;

    EngineHost& host_;
    fs::path dir_;
    mutable std::mutex mutex_;
    std::mutex scanMutex_;
    std::atomic<bool> cancelled_ { false };
    json plugins_ = json::object();         // plugin_uid -> record
    json blacklist_ = json::object(), probe_ = json::object(), tags_ = json::object(), presetCache_ = json::object();
    std::string fingerprint_, scannedAt_;
    std::string state_ = "never";           // never | scanning | done
};

} // namespace rovox
