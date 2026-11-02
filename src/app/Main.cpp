// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// rovox: serve | render | import | upload | plugins | mcp | bench   (task 170; mcp and the MCP endpoint of serve are task 200). Every subcommand that changes a project
// goes through the CommandBus, the same layer the server, MCP and the web UI use.

#include "Cli.h"
#include "PluginScan.h"
#include "Version.h"

#include <CLI/CLI.hpp>

#include <iostream>

int main (int argc, char** argv)
{
    // A scan child is the same executable started by the engine; it must not parse our command line.
    if (rovox::runPluginScanChildIfRequested (argc, argv))
        return 0;

    if (argc == 2 && std::string (argv[1]) == "--version")
    {
        std::cout << "Rovox " << rovox::rovoxVersion() << " (" << rovox::rovoxGitSha() << ")\n"
                  << "JUCE " << juce::SystemStats::getJUCEVersion().toStdString() << "\n"
                  << "Tracktion Engine " << rovox::tracktionVersion() << "\n";
        return 0;
    }

    CLI::App app { "Rovox: headless DAW server. Mix and render with checks, over MCP, HTTP or the command line." };
    app.require_subcommand (1);
    app.set_help_all_flag ("--help-all", "Show help for all subcommands");
    rovox::cli::Options o;

    app.add_option ("--data-dir,--projects", o.dataDir, "Writable data directory (default: $ROVOX_DATA_DIR, else ~/.local/share/rovox)");

    auto* serve = app.add_subcommand ("serve", "Run the server: HTTP API, uploads, autosave. REAPER JS effects: mount the REAPER Effects folder read-only at /jsfx "
                                           "(docker run -v \"$HOME/Library/Application Support/REAPER/Effects:/jsfx:ro\") or set ROVOX_JSFX_PATH (colon separated)");
    serve->add_option ("--port", o.port, "Listen port")->capture_default_str();
    serve->add_option ("--bind", o.bind, "Listen address")->capture_default_str();
    serve->add_option ("--audio", o.audio, "Audio mode: none | browser | device")->capture_default_str();
    serve->add_option ("--token", o.token, "Require 'Authorization: Bearer <token>' (default: $ROVOX_TOKEN)");
    serve->add_option ("--plugins", o.pluginsDir, "Extra plugin directory (VST3/LV2)");
    serve->add_option ("--web-dir", o.webDir, "Built web UI (default: $ROVOX_WEB_DIR, else /usr/share/rovox/web)");
    serve->add_option ("--allow-path", o.allowPaths, "Directory the API and MCP may read besides the data dir, e.g. a mounted /in (repeatable; default $ROVOX_ALLOWED_PATHS, ':' separated)");
    serve->add_flag ("--generate-token", o.generateToken, "With --bind beyond loopback and no token: generate one and print it (the container image does this by default)");
    serve->add_option ("--config", o.config, "Config file (reserved)");
    serve->add_option ("--allow-origin", o.allowOrigins, "Browser Origin accepted by /mcp besides localhost (repeatable)");
    serve->add_option ("--tool-names", o.toolNames, "MCP tool names: dots (track.list) | underscore (track_list)")->capture_default_str();
    serve->add_flag ("--reaper-compat", o.reaperCompat, "Also offer the tool names of the old REAPER MCP bridge (list_projects, update_tracks ...)");
    serve->add_option ("--source-url", o.sourceUrl, "Where the running source code can be obtained (AGPL section 13; default $ROVOX_SOURCE_URL)");
    serve->callback ([&] { std::exit (rovox::cli::runServe (o)); });

    auto* render = app.add_subcommand ("render", "Render a project to an audio file with preflight and post-render checks");
    render->add_option ("project", o.target, "A .rvx directory, a project id or a project name")->required();
    render->add_option ("--out,-o", o.out, "Output file (default: renders/<name>.<format> inside the project)");
    render->add_option ("--name", o.renderName, "Render name (default: from --out, else 'mix')");
    render->add_option ("--range", o.range, "Bars, e.g. 2:40 (bar 40 excluded)");
    render->add_option ("--start-s", o.startS, "Start in seconds");
    render->add_option ("--end-s", o.endS, "End in seconds");
    render->add_option ("--format", o.format, "wav | flac | mp3 (default: from --out extension, else wav)");
    render->add_option ("--bit-depth", o.bitDepth, "wav: 16|24|32(float), flac: 16|24")->capture_default_str();
    render->add_option ("--bitrate", o.bitrate, "mp3 bitrate in kbps")->capture_default_str();
    render->add_option ("--sample-rate", o.sampleRate, "Output sample rate (default: project)");
    render->add_option ("--channels", o.channels, "1 or 2")->capture_default_str();
    render->add_option ("--tail-s", o.tailS, "Tail after the range end, cut at the first silent block")->capture_default_str();
    render->add_option ("--target-lufs", o.targetLufs, "Loudness target in LUFS (with --check: verify, with --normalize: apply gain)");
    render->add_option ("--tolerance-lu", o.toleranceLu, "Accepted deviation from the target")->capture_default_str();
    render->add_flag ("--normalize", o.normalize, "Apply gain to reach --target-lufs (never past the true-peak ceiling)");
    render->add_option ("--true-peak-ceiling", o.truePeakCeiling, "True-peak ceiling in dBTP for --normalize")->capture_default_str();
    render->add_flag ("--limiter", o.limiter, "Master true-peak limiter (default: on with --target-lufs, else off)");
    render->add_flag ("--no-limiter", o.noLimiter, "Turn the limiter off");
    render->add_option ("--ceiling-dbtp", o.truePeakCeiling, "Same as --true-peak-ceiling");
    render->add_flag ("--check", o.check, "Fail (exit 2) on offline media, unloaded plugins, silent instruments or output, clipping");
    render->add_flag ("--json", o.json, "Print the full result as JSON");
    render->add_option ("--scope", o.scope, "master | tracks | stems")->capture_default_str();
    render->add_option ("--track", o.trackIds, "Track id for --scope tracks|stems (repeatable)");
    render->add_option ("--seed", o.seed, "Seed recorded in the render metadata");
    render->add_flag ("--preflight-only", o.preflightOnly, "Only run the preflight checks");
    render->callback ([&] { std::exit (rovox::cli::runRender (o)); });

    auto* import = app.add_subcommand ("import", "Import audio files, a folder, a Zoom R20 .zprj folder or a REAPER .RPP");
    import->add_option ("paths", o.paths, "Files, folders or one .RPP")->required();
    import->add_option ("--project", o.project, "Import into this project (id, name or .rvx path)");
    import->add_option ("--new", o.newName, "Create a new project with this name");
    import->add_option ("--preset", o.preset, "auto | zoom_r20 | generic")->capture_default_str();
    import->add_option ("--align", o.align, "start | bwf | none")->capture_default_str();
    import->add_option ("--takes", o.takes, "separate | latest | lanes")->capture_default_str();
    import->add_option ("--rename", o.rename, "Track names, e.g. \"REC01=Guitar,REC02=Bass\"");
    import->add_flag ("--no-copy-media", o.noCopyMedia, "RPP: reference media in place instead of copying it into the project");
    import->add_flag ("--json", o.json, "Print the result as JSON");
    import->callback ([&] { std::exit (rovox::cli::runImport (o)); });

    auto* upload = app.add_subcommand ("upload", "Upload files or a folder to a running server (resumable)");
    upload->add_option ("paths", o.paths, "Files or folders")->required();
    upload->add_option ("--server", o.server, "Server URL, e.g. http://127.0.0.1:4810")->required();
    upload->add_option ("--token", o.token, "Bearer token (default: $ROVOX_TOKEN)");
    upload->add_option ("--chunk-mb", o.chunkMb, "Chunk size")->capture_default_str();
    upload->callback ([&] { std::exit (rovox::cli::runUpload (o)); });

    auto* plugins = app.add_subcommand ("plugins", "List installed LV2 / VST3 plugins (scanned in a child process)");
    plugins->add_option ("--path", o.pluginsDir, "Extra search path");
    plugins->callback ([&] { std::exit (rovox::cli::runPlugins (o)); });

    auto* mcp = app.add_subcommand ("mcp", "MCP over stdio: the same tools as 'serve', for Claude Code and other local clients");
    mcp->add_option ("--plugins", o.pluginsDir, "Extra plugin directory (VST3/LV2)");
    mcp->add_option ("--tool-names", o.toolNames, "MCP tool names: dots (track.list) | underscore (track_list)")->capture_default_str();
    mcp->add_flag ("--reaper-compat", o.reaperCompat, "Also offer the tool names of the old REAPER MCP bridge (list_projects, update_tracks ...)");
    mcp->add_option ("--source-url", o.sourceUrl, "Where the running source code can be obtained (default $ROVOX_SOURCE_URL)");
    mcp->callback ([&] { std::exit (rovox::cli::runMcp (o)); });

    // helper processes of the server (task 410): a plugin is loaded and tested, or a project with plugins is rendered, outside the server process
    auto* probe = app.add_subcommand ("plugin-probe", "internal: load one plugin in this process and print what it does as JSON");
    probe->group ("");
    probe->add_option ("description", o.probeFile, "XML file with the plugin description")->required();
    probe->add_flag ("--presets-only", o.presetsOnly, "only list the programs");
    probe->callback ([&] { std::exit (rovox::cli::runPluginProbe (o)); });

    auto* renderChild = app.add_subcommand ("render-child", "internal: render a project copy and print PROGRESS / RESULT lines");
    renderChild->group ("");
    renderChild->add_option ("snapshot", o.snapshotDir, "project copy directory")->required();
    renderChild->add_option ("request", o.requestFile, "request.json")->required();
    renderChild->callback ([&] { std::exit (rovox::cli::runRenderChildCommand (o)); });

    auto* bench = app.add_subcommand ("bench", "Resource benchmark (the spike binary rovox_spikes bench covers it for now)");
    bench->callback ([] { std::cerr << "rovox bench is not available yet; see spikes/p5_bench.\n"; std::exit (2); });

    CLI11_PARSE (app, argc, argv);
    return 0;
}
