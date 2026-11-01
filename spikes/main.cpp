// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Multi-call binary for the throwaway spikes P2..P5.
//   rovox_spikes plugins <work-dir> [lv2-path]         P2: LV2 scan, effects, instrument, PDC, load
//   rovox_spikes stream  <work-dir> [--port N] ...      P3: browser-stream (null clock device + Opus + WebSocket)
//   rovox_spikes mcp     [--selftest N]                 P4: MCP over stdio with two open projects
//   rovox_spikes bench   <work-dir> [options]           P5: resource benchmark

#include <tracktion_engine/tracktion_engine.h>

#include <cstdio>
#include <cstring>

int runPlugins (int argc, char** argv);
int runStream (int argc, char** argv);
int runMcp (int argc, char** argv);
int runBench (int argc, char** argv);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Out-of-process plugin scan: Tracktion re-launches this executable with a "PluginScan" argument.
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    if (args.joinIntoString (" ").contains ("PluginScan"))
    {
        if (tracktion::PluginManager::startChildProcessPluginScan (args.joinIntoString (" ")))
        {
            juce::MessageManager::getInstance()->runDispatchLoop();
            return 0;
        }
    }

    if (argc >= 2)
    {
        const std::string cmd = argv[1];

        if (cmd == "plugins")  return runPlugins (argc - 1, argv + 1);
        if (cmd == "stream")   return runStream (argc - 1, argv + 1);
        if (cmd == "mcp")      return runMcp (argc - 1, argv + 1);
        if (cmd == "bench")    return runBench (argc - 1, argv + 1);
    }

    std::fprintf (stderr, "usage: rovox_spikes plugins|stream|mcp|bench ...\n");
    return 2;
}
