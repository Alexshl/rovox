// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "PluginScan.h"

#include <cstdlib>

namespace rovox
{

bool runPluginScanChildIfRequested (int argc, char** argv)
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    const auto line = args.joinIntoString (" ");

    if (! line.contains ("PluginScan"))
        return false;

    juce::ScopedJuceInitialiser_GUI juceInit;

    if (! te::PluginManager::startChildProcessPluginScan (line))
        return false;

    juce::MessageManager::getInstance()->runDispatchLoop();
    return true;
}

namespace
{
std::string defaultSearchPath (const std::string& format, const std::string& extra)
{
    const char* env = std::getenv (format == "LV2" ? "LV2_PATH" : "VST3_PATH");
    std::string path = env != nullptr && *env != 0 ? env : (format == "LV2" ? "/usr/lib/lv2:/usr/local/lib/lv2" : "/usr/lib/vst3:/usr/local/lib/vst3");
    path += format == "LV2" ? ":/plugins/lv2" : ":/plugins/vst3";

    if (! extra.empty())
        path += ":" + extra;

    return path;
}
} // namespace

json scanPlugins (EngineHost& host, const std::vector<std::string>& formats, const std::string& extraPath)
{
    auto& pm = host.engine().getPluginManager();
    json out { { "plugins", json::array() }, { "formats", json::array() } };
    juce::KnownPluginList known;

    for (auto& name : formats)
    {
        juce::AudioPluginFormat* format = nullptr;

        for (auto* f : pm.pluginFormatManager.getFormats())
            if (f->getName() == toJuce (name))
                format = f;

        if (format == nullptr)
        {
            out["formats"].push_back ({ { "format", name }, { "error", "format not compiled in" } });
            continue;
        }

        const auto searchPath = defaultSearchPath (name, extraPath);
        const auto before = known.getNumTypes();
        juce::PluginDirectoryScanner scanner (known, *format, juce::FileSearchPath (toJuce (searchPath)), true,
                                              toFile (host.dataDir() / "cache" / ("scan-deadman-" + name + ".txt")), false);
        juce::String scanned;

        while (scanner.scanNextFile (true, scanned))
        {}

        json failed = json::array();

        for (auto& f : scanner.getFailedFiles())
            failed.push_back (toStd (f));

        out["formats"].push_back ({ { "format", name }, { "search_path", searchPath }, { "found", known.getNumTypes() - before },
                                    { "failed_files", failed }, { "out_of_process", pm.usesSeparateProcessForScanning() } });
    }

    for (auto& d : known.getTypes())
        out["plugins"].push_back ({ { "name", toStd (d.name) }, { "format", toStd (d.pluginFormatName) }, { "uid", toStd (d.fileOrIdentifier) },
                                    { "manufacturer", toStd (d.manufacturerName) }, { "version", toStd (d.version) }, { "instrument", d.isInstrument },
                                    { "inputs", d.numInputChannels }, { "outputs", d.numOutputChannels } });

    return out;
}

} // namespace rovox
