// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "PluginCatalog.h"
#include "StockPlugins.h"
#include "JsfxPlugin.h"

#include "PluginHost.h"
#include "ProjectFormat.h"
#include "Sha256.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rovox
{

namespace
{
std::vector<std::string> splitPath (const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss (s);
    std::string item;

    while (std::getline (ss, item, ':'))
        if (! item.empty())
            out.push_back (item);

    return out;
}

std::string lowerCase (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

std::string searchPathFor (const std::string& format)
{
    const char* env = std::getenv (format == "LV2" ? "LV2_PATH" : "VST3_PATH");
    return env != nullptr && *env != 0 ? env : (format == "LV2" ? "/usr/lib/lv2:/usr/local/lib/lv2" : "/usr/lib/vst3:/usr/local/lib/vst3");
}

std::string iso (std::time_t t = std::time (nullptr))
{
    char buf[32];
    std::tm tm {};
    gmtime_r (&t, &tm);
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
} // namespace

std::string pluginUidFor (const juce::PluginDescription& d)
{
    auto uid = lowerCase (toStd (d.pluginFormatName)) + ":" + toStd (d.fileOrIdentifier);

    if (d.pluginFormatName != "LV2")
        uid += "#" + toStd (juce::String::toHexString (d.uniqueId));

    return uid;
}

PluginCatalog::PluginCatalog (EngineHost& h) : host_ (h), dir_ (h.dataDir() / "catalog")
{
    std::error_code ec;
    fs::create_directories (dir_, ec);
    loadFiles();
}

json PluginCatalog::loadJson (const char* file) const
{
    std::ifstream in (dir_ / file);

    if (! in)
        return json::object();

    auto j = json::parse (in, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

void PluginCatalog::saveJson (const char* file, const json& j) const
{
    writeFileAtomic (dir_ / file, j.dump (1), false);
}

void PluginCatalog::loadFiles()
{
    std::lock_guard<std::mutex> lock (mutex_);
    auto doc = loadJson ("plugins.json");
    plugins_ = doc.value ("plugins", json::object());
    fingerprint_ = doc.value ("fingerprint", "");
    scannedAt_ = doc.value ("scanned_at", "");
    state_ = plugins_.empty() ? "never" : "done";
    blacklist_ = loadJson ("blacklist.json");
    probe_ = loadJson ("probe.json");
    tags_ = loadJson ("tags.json");
    presetCache_ = loadJson ("presets-cache.json");
}

void PluginCatalog::saveCatalog()
{
    saveJson ("plugins.json", { { "plugins", plugins_ }, { "fingerprint", fingerprint_ }, { "scanned_at", scannedAt_ } });
}

std::string PluginCatalog::fingerprint() const
{
    Sha256 h;

    for (auto* format : { "LV2", "VST3" })
    {
        for (auto& root : splitPath (searchPathFor (format)))
        {
            std::error_code ec;
            h.update (root.data(), root.size());

            if (! fs::is_directory (root, ec))
                continue;

            fs::recursive_directory_iterator it (root, fs::directory_options::skip_permission_denied, ec), end;

            for (; it != end; it.increment (ec))
            {
                if (ec)
                    break;

                if (it.depth() > 3)
                {
                    it.disable_recursion_pending();
                    continue;
                }

                const auto name = it->path().string();
                std::error_code e2;
                const auto mtime = (long long) it->last_write_time (e2).time_since_epoch().count();
                const auto size = it->is_regular_file (e2) ? (long long) it->file_size (e2) : 0LL;
                const auto line = name + "|" + std::to_string (mtime) + "|" + std::to_string (size) + "\n";
                h.update (line.data(), line.size());
            }
        }
    }

    return h.hexDigest();
}

json PluginCatalog::rescan (bool force)
{
    std::lock_guard<std::mutex> scanLock (scanMutex_);
    const auto t0 = std::chrono::steady_clock::now();
    const auto fp = fingerprint();

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (! force && fp == fingerprint_ && ! plugins_.empty())
            return { { "unchanged", true }, { "plugins", plugins_.size() }, { "scanned_at", scannedAt_ },
                     { "elapsed_ms", std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count() } };

        state_ = "scanning";
    }

    const auto cancelledResult = [&]
    {
        std::lock_guard<std::mutex> lock (mutex_);
        state_ = plugins_.empty() ? "never" : "done";
        return json { { "cancelled", true } };
    };

    if (cancelled_)
        return cancelledResult();

    // Scanned into the engine's own list: with `serve` that list has the out-of-process scanner (a plugin that crashes while it is scanned
    // is blacklisted by the scan child, not by this process), and Tracktion finds the description again there when it loads a plugin.
    auto& pm = host_.engine().getPluginManager();
    auto& known = pm.knownPluginList;
    known.clear();
    json formats = json::array(), failed = json::array();

    for (const std::string name : { "LV2", "VST3" })
    {
        juce::AudioPluginFormat* format = nullptr;

        for (auto* f : pm.pluginFormatManager.getFormats())
            if (f->getName() == toJuce (name))
                format = f;

        if (format == nullptr)
        {
            formats.push_back ({ { "format", name }, { "error", "format not compiled in" } });
            continue;
        }

        const auto before = known.getNumTypes();
        juce::PluginDirectoryScanner scanner (known, *format, juce::FileSearchPath (toJuce (searchPathFor (name))), true,
                                              toFile (host_.dataDir() / "cache" / ("scan-deadman-" + name + ".txt")), false);
        juce::String scanned;

        while (! cancelled_ && scanner.scanNextFile (true, scanned))
        {}

        if (cancelled_)
            return cancelledResult();

        for (auto& f : scanner.getFailedFiles())
        {
            failed.push_back (toStd (f));
            blacklist ("scan:" + toStd (f), "the scan failed or crashed on this file");
        }

        formats.push_back ({ { "format", name }, { "search_path", searchPathFor (name) }, { "found", known.getNumTypes() - before },
                             { "out_of_process", pm.usesSeparateProcessForScanning() } });
    }

    json fresh = json::object();

    for (auto& d : known.getTypes())
    {
        const auto uid = pluginUidFor (d);
        json rec {
            { "plugin_uid", uid }, { "format", toStd (d.pluginFormatName) }, { "name", toStd (d.name) }, { "vendor", toStd (d.manufacturerName) },
            { "version", toStd (d.version) }, { "kind", d.isInstrument ? "instrument" : "effect" }, { "categories", json::array() },
            { "io", { { "in", d.numInputChannels }, { "out", d.numOutputChannels }, { "midi_in", d.isInstrument } } },
            { "latency_samples", nullptr }, { "params_count", nullptr }, { "file", toStd (d.fileOrIdentifier) }, { "connector", "local" },
            { "server_use_ok", "unknown" }, { "desc_xml", d.createXml()->toString().toStdString() }
        };

        if (d.category.isNotEmpty())
            rec["categories"].push_back (toStd (d.category));

        fresh[uid] = std::move (rec);
    }

    std::lock_guard<std::mutex> lock (mutex_);
    plugins_ = std::move (fresh);
    fingerprint_ = fp;
    scannedAt_ = iso();
    state_ = "done";
    probe_ = json::object();            // the probe results belong to the files that were just scanned
    saveJson ("probe.json", probe_);
    saveCatalog();
    return { { "unchanged", false }, { "plugins", plugins_.size() }, { "formats", formats }, { "failed_files", failed }, { "scanned_at", scannedAt_ },
             { "elapsed_ms", std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count() } };
}

size_t PluginCatalog::size() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return plugins_.size();
}

json PluginCatalog::scanState() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return { { "state", state_ }, { "scanned_at", scannedAt_.empty() ? json (nullptr) : json (scannedAt_) }, { "plugins", plugins_.size() } };
}

void PluginCatalog::registerWithEngine()
{
    json records;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        records = plugins_;
    }

    auto& list = host_.engine().getPluginManager().knownPluginList;

    for (auto& [uid, rec] : records.items())
        if (auto d = descriptionOf (rec))
            list.addType (*d);
}

json PluginConnector::describe() const
{
    json j { { "id", id() }, { "available", available() }, { "description", description() } };

    if (! available())
        j["reason"] = unavailableReason();

    return j;
}

namespace
{
struct LocalConnector : PluginConnector
{
    std::string id() const override             { return "local"; }
    std::string description() const override    { return "Plugins installed on this server (LV2, VST3), tested and rendered in child processes"; }
    bool available() const override             { return true; }
};

struct RemoteNodeConnector : PluginConnector
{
    std::string id() const override             { return "remote-node"; }
    std::string description() const override    { return "Plugins on a Rovox node on another machine (for example macOS AU / VST3 instruments)"; }
    bool available() const override             { return false; }
    std::string unavailableReason() const override  { return "not implemented in this version: the connector interface exists, the node protocol does not"; }
};

struct WineConnector : PluginConnector
{
    std::string id() const override             { return "wine"; }
    std::string description() const override    { return "Windows plugins through Wine and yabridge"; }
    bool available() const override             { return false; }
    std::string unavailableReason() const override  { return "not implemented in this version; activation and licensing of such plugins stay with their vendors and the user"; }
};
} // namespace

std::vector<std::unique_ptr<PluginConnector>> makeConnectors()
{
    std::vector<std::unique_ptr<PluginConnector>> out;
    out.push_back (std::make_unique<LocalConnector>());
    out.push_back (std::make_unique<RemoteNodeConnector>());
    out.push_back (std::make_unique<WineConnector>());
    return out;
}

json PluginCatalog::connectors() const
{
    json out = json::array();

    for (auto& c : makeConnectors())
        out.push_back (c->describe());

    return out;
}

std::optional<std::string> PluginCatalog::blacklistReason (const std::string& uid) const
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (blacklist_.contains (uid))
        return blacklist_[uid].value ("reason", "blacklisted");

    return std::nullopt;
}

void PluginCatalog::blacklist (const std::string& uid, const std::string& reason)
{
    std::lock_guard<std::mutex> lock (mutex_);
    blacklist_[uid] = { { "reason", reason }, { "at", iso() } };
    saveJson ("blacklist.json", blacklist_);
}

void PluginCatalog::unblacklist (const std::string& uid)
{
    std::lock_guard<std::mutex> lock (mutex_);
    blacklist_.erase (uid);
    saveJson ("blacklist.json", blacklist_);
}

std::string PluginCatalog::presetId (const std::string& uid, const std::string& name)
{
    return "pr_" + sha256Hex (uid + "\n" + name).substr (0, 16);
}

fs::path PluginCatalog::userPresetDir (const std::string& uid) const
{
    return host_.dataDir() / "presets" / sha256Hex (uid).substr (0, 16);
}

std::optional<json> PluginCatalog::find (const std::string& key) const
{
    if (auto* stock = findStockPlugin (key))
        return stockCatalogRecord (*stock);

    if (isJsfxUid (key))
        return jsfxCatalogRecord (key);

    std::lock_guard<std::mutex> lock (mutex_);

    if (plugins_.contains (key))
        return plugins_[key];

    const auto wanted = lowerCase (key);

    for (auto& [uid, rec] : plugins_.items())
        if (lowerCase (rec.value ("name", "")) == wanted)
            return rec;

    return std::nullopt;
}

std::optional<juce::PluginDescription> PluginCatalog::descriptionOf (const json& rec)
{
    if (auto xml = juce::parseXML (toJuce (rec.value ("desc_xml", ""))))
    {
        juce::PluginDescription d;

        if (d.loadFromXml (*xml))
            return d;
    }

    return std::nullopt;
}

Result<json> PluginCatalog::ensureProbed (const std::string& uid)
{
    auto rec = find (uid);

    if (! rec)
        return makeError (errc::notFound, "no plugin " + uid + " in the catalog (plugin.catalog lists what is installed)");

    const auto realUid = rec->value ("plugin_uid", uid);

    if (rec->value ("builtin", false))
        return json { { "builtin", true }, { "name", rec->value ("name", "") }, { "latency_samples", nullptr } };

    if (auto why = blacklistReason (realUid))
        return makeError (errc::pluginBlacklisted, "plugin '" + rec->value ("name", realUid) + "' is blacklisted: " + *why, json { { "plugin_uid", realUid } });

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (probe_.contains (realUid) && probe_[realUid].value ("ok", false))
            return probe_[realUid]["info"];
    }

    auto desc = descriptionOf (*rec);

    if (! desc)
        return makeError (errc::internal, "the catalog entry of " + realUid + " is damaged; run plugin.rescan with force");

    auto out = probePluginInChild (*desc, host_.dataDir());

    if (out.crashed || out.timedOut)
    {
        blacklist (realUid, out.error);
        return makeError (errc::pluginCrashed, "plugin '" + rec->value ("name", realUid) + "' " + out.error + " while it was tested in a separate process; it is blacklisted. The server was not affected",
                          json { { "plugin_uid", realUid } });
    }

    if (! out.ok)
        return makeError (errc::pluginLoadFailed, "plugin '" + rec->value ("name", realUid) + "' could not be loaded: " + out.error, json { { "plugin_uid", realUid } });

    std::lock_guard<std::mutex> lock (mutex_);
    probe_[realUid] = { { "ok", true }, { "at", iso() }, { "info", out.info } };
    plugins_[realUid]["latency_samples"] = out.info.value ("latency_samples", 0);
    plugins_[realUid]["params_count"] = out.info.value ("num_params", 0);
    saveJson ("probe.json", probe_);
    return out.info;
}

Result<json> PluginCatalog::setTags (const std::string& uid, const std::vector<std::string>& tags)
{
    auto rec = find (uid);

    if (! rec)
        return makeError (errc::notFound, "no plugin " + uid);

    std::lock_guard<std::mutex> lock (mutex_);
    tags_[rec->value ("plugin_uid", uid)] = tags;
    saveJson ("tags.json", tags_);
    return json { { "plugin_uid", rec->value ("plugin_uid", uid) }, { "tags", tags } };
}

Result<json> PluginCatalog::presetsOf (const std::string& uid)
{
    auto rec = find (uid);

    if (! rec)
        return makeError (errc::notFound, "no plugin " + uid);

    const auto realUid = rec->value ("plugin_uid", uid);
    json presets = json::array();
    bool cached = false;

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (presetCache_.contains (realUid) && presetCache_[realUid].value ("fingerprint", "") == fingerprint_)
        {
            presets = presetCache_[realUid]["presets"];
            cached = true;
        }
    }

    if (! cached)
    {
        if (blacklistReason (realUid))
            return json::array();

        auto desc = descriptionOf (*rec);

        if (! desc)
            return makeError (errc::internal, "damaged catalog entry");

        auto out = probePluginInChild (*desc, host_.dataDir(), true);

        if (out.crashed || out.timedOut)
        {
            blacklist (realUid, out.error);
            return makeError (errc::pluginCrashed, "plugin '" + rec->value ("name", realUid) + "' " + out.error + " while its presets were listed; it is blacklisted", json { { "plugin_uid", realUid } });
        }

        if (! out.ok)
            return makeError (errc::pluginLoadFailed, "plugin '" + rec->value ("name", realUid) + "' could not be loaded: " + out.error);

        int index = 0;

        for (auto& n : out.info.value ("programs", json::array()))
            presets.push_back ({ { "preset_id", presetId (realUid, n.get<std::string>()) }, { "name", n }, { "source", "factory" }, { "program_index", index++ }, { "tags", json::array() } });

        std::lock_guard<std::mutex> lock (mutex_);
        presetCache_[realUid] = { { "fingerprint", fingerprint_ }, { "presets", presets } };
        saveJson ("presets-cache.json", presetCache_);
    }

    std::error_code ec;

    for (auto& f : fs::directory_iterator (userPresetDir (realUid), ec))
    {
        if (f.path().extension() != ".rvxpreset")
            continue;

        std::ifstream in (f.path());
        auto doc = json::parse (in, nullptr, false);

        if (! doc.is_discarded())
            presets.push_back ({ { "preset_id", presetId (realUid, doc.value ("name", "")) }, { "name", doc.value ("name", "") }, { "source", "user" },
                                 { "tags", doc.value ("tags", json::array()) } });
    }

    return presets;
}

json PluginCatalog::summary (const json& rec, bool withPresets)
{
    json j = rec;
    j.erase ("desc_xml");
    const auto uid = rec.value ("plugin_uid", "");

    {
        std::lock_guard<std::mutex> lock (mutex_);
        j["status"] = blacklist_.contains (uid) ? "blacklisted" : "ok";

        if (blacklist_.contains (uid))
            j["status_reason"] = blacklist_[uid].value ("reason", "");

        j["tags"] = tags_.contains (uid) ? tags_[uid] : json::array();
        j["probed"] = probe_.contains (uid);
    }

    if (withPresets && rec.value ("builtin", false))
    {
        j["presets"] = json::array();
    }
    else if (withPresets)
    {
        auto presets = presetsOf (uid);

        if (presets.ok())
            j["presets"] = presets.value();
        else
            j["presets_error"] = presets.error().message;
    }

    return j;
}

json PluginCatalog::query (const CatalogQuery& q)
{
    std::vector<json> matches;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto needle = lowerCase (q.query);
        json source = plugins_;

        for (auto& stock : stockPlugins())
            source[stock.uid] = stockCatalogRecord (stock);

        for (auto& rec : jsfxCatalogRecords())
            source[rec["plugin_uid"].get<std::string>()] = rec;

        for (auto& [uid, rec] : source.items())
        {
            if (! q.kind.empty() && rec.value ("kind", "") != q.kind)           continue;
            if (! q.format.empty() && lowerCase (rec.value ("format", "")) != lowerCase (q.format))  continue;

            const bool listed = blacklist_.contains (uid);
            const std::string status = listed ? "blacklisted" : "ok";

            if (! q.status.empty() && status != q.status)                       continue;

            const json myTags = tags_.contains (uid) ? tags_[uid] : json::array();

            if (! q.tag.empty() && std::find (myTags.begin(), myTags.end(), q.tag) == myTags.end())
                continue;

            if (! needle.empty())
            {
                auto hay = lowerCase(rec.value ("name", "") + " " + rec.value ("vendor", "") + " " + rec["categories"].dump() + " " + myTags.dump());

                if (hay.find (needle) == std::string::npos)
                    continue;
            }

            matches.push_back (rec);
        }
    }

    std::sort (matches.begin(), matches.end(), [] (const json& a, const json& b) { return lowerCase (a.value ("name", "")) < lowerCase (b.value ("name", "")); });
    const size_t limit = q.includePresets ? std::min<size_t> (q.limit, 10) : q.limit;
    json out = json::array();

    for (size_t i = q.offset; i < matches.size() && out.size() < limit; ++i)
        out.push_back (summary (matches[i], q.includePresets));

    const size_t next = q.offset + out.size();
    json result { { "plugins", out }, { "total", matches.size() }, { "next_cursor", next < matches.size() ? json (std::to_string (next)) : json (nullptr) },
                  { "scan", scanState() }, { "connectors", connectors() } };

    if (q.includePresets && q.limit > limit)
        result["note"] = "include_presets lists presets for at most 10 plugins per call (each is loaded in a child process); use next_cursor";

    return result;
}

} // namespace rovox
