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
    bool serverUseOk = false;                           // only records whose server_use_ok is exactly true
    bool hideNonFree = false;                           // tenants other than "local" never see records whose server_use_ok is not true
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

    /** Reads the licences of the installed plugins: $ROVOX_LICENSES_FILE or /usr/share/rovox/licenses.json (written at image build time by
        tools/plugin-licenses.py from the Debian copyright files), overridden per key by <data>/catalog/licenses.json. */
    void reloadLicenses();
    /** True if the SPDX id (or an expression of ids joined by AND / OR) is on the free list of the server-use policy. */
    static bool isFreeLicense (const std::string& spdx);
    /** True only if the record's server_use_ok is exactly boolean true (the string "unknown" is not). */
    static bool serverUseOk (const json& record)    { return record.is_object() && record.contains ("server_use_ok") && record["server_use_ok"] == true; }

    /** The tenant's licence policy (`[tenants.<id>] licence_policy` in the config file): true = only plugins with server_use_ok. `local` defaults to
        `any`, every other tenant (also one without a record) to `server_use_ok`. */
    static bool licenceGated (const std::string& tenant);

    /** THE licence gate: a tenant with licence_policy `any` (default for "local") may use anything; any other tenant only a plugin that has a catalog
        record with server_use_ok == true (a plugin without a record has an unknown licence). Every path that instantiates a plugin for a tenant goes through this. */
    static bool licenceAllows (const std::string& tenant, const std::optional<json>& record)
    {
        return ! licenceGated (tenant) || (record.has_value() && serverUseOk (*record));
    }
    /** FORBIDDEN_LICENSE unless licenceAllows(); `what` names the plugin in the message when there is no record. */
    static Result<void> requireLicence (const std::string& tenant, const std::optional<json>& record, const std::string& what);

    /** Licence gate for a project being opened: every external plugin ("vst" nodes) in the saved edit state must have a catalog record (matched by
        its file or URI) that passes licenceAllows() for the tenant; the error lists the offending plugins. A tenant with policy `any` ("local" by default) is never restricted. */
    Result<void> requireLicencesInState (const std::string& tenant, const juce::ValueTree& editState) const;

    /** Record by plugin_uid ("lv2:<uri>") or, failing that, by exact name (then the first match). nullopt if unknown. */
    std::optional<json> find (const std::string& uidOrName) const;
    static std::optional<juce::PluginDescription> descriptionOf (const json& record);

    /** Loads the plugin in a child process once per catalog fingerprint. A crash blacklists it (PLUGIN_CRASHED), a load failure
        gives PLUGIN_LOAD_FAILED. Returns the probe info. */
    Result<json> ensureProbed (const std::string& pluginUid, const std::string& tenant = "local");

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
    json withLicense (json record) const;       // mutex_ held
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
    json licenses_ = json::object();        // plugin_uid or bundle name -> {license, source_url, package}
    std::string fingerprint_, scannedAt_;
    std::string state_ = "never";           // never | scanning | done
};

} // namespace rovox
