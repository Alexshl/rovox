// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// rovox: serve | render | import | upload | plugins | mcp | bench   (task 170; mcp and the MCP endpoint of serve are task 200). Every subcommand that changes a project
// goes through the CommandBus, the same layer the server, MCP and the web UI use.

#include "Bench.h"
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

    app.add_option ("--storage", o.storage, "Project storage: s3 (S3_ENDPOINT, S3_BUCKET, S3_ACCESS_KEY, S3_SECRET_KEY, S3_REGION, S3_PREFIX; each also as <NAME>_FILE), local, or fs (a directory: ROVOX_STORAGE_FS_ROOT or [storage] fs_root) (default: $ROVOX_STORAGE, "
                                           "else s3 when S3_ENDPOINT and S3_BUCKET are set)")->check (CLI::IsMember ({ "s3", "local", "fs" }));

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
    serve->add_option ("--allow-origin", o.allowOrigins, "Browser Origin accepted by /api, /ws and /mcp besides localhost and the server's own host (repeatable); its host is also an accepted Host");
    serve->add_option ("--allow-host", o.allowHosts, "Host header accepted on a loopback bind besides localhost, e.g. daw.example.com behind a reverse proxy (repeatable; default $ROVOX_ALLOWED_HOSTS, ',' separated)");
    serve->add_option ("--tool-names", o.toolNames, "MCP tool names: dots (track.list) | underscore (track_list)")->capture_default_str();
    serve->add_flag ("--reaper-compat", o.reaperCompat, "Also offer the tool names of the old REAPER MCP bridge (list_projects, update_tracks ...)");
    serve->add_option ("--source-url", o.sourceUrl, "Where the running source code can be obtained (AGPL section 13; default $ROVOX_SOURCE_URL)");
    serve->add_option ("--storage", o.storage, "s3 | local | fs (see the global option)")->check (CLI::IsMember ({ "s3", "local", "fs" }));
    serve->add_option ("--drain-timeout", o.drainTimeoutS, "On SIGTERM: seconds running jobs get to finish before they end 'interrupted' (keep it below the process manager's stop timeout; Docker 10 s)")
         ->capture_default_str()->check (CLI::Range (0.0, 3600.0));
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
    upload->add_option ("--token", o.token, "Bearer token or API key (default: $ROVOX_KEY, then $ROVOX_TOKEN)");
    upload->add_option ("--chunk-mb", o.chunkMb, "Chunk size")->capture_default_str();
    upload->callback ([&] { std::exit (rovox::cli::runUpload (o)); });

    auto* plugins = app.add_subcommand ("plugins", "List installed LV2 / VST3 plugins (scanned in a child process)");
    plugins->add_option ("--path", o.pluginsDir, "Extra search path");
    plugins->callback ([&] { std::exit (rovox::cli::runPlugins (o)); });

    auto* packs = app.add_subcommand ("packs", "Instrument packs (SoundFonts played by rovox:sf2, from $ROVOX_INSTRUMENTS_PATH, default /instruments): list, verify");
    packs->require_subcommand (1);
    auto* packsList = packs->add_subcommand ("list", "List the installed packs with licence and server_use_ok");
    packsList->add_flag ("--json", o.json, "Print JSON");
    packsList->callback ([&] { std::exit (rovox::cli::runPacksList (o)); });
    auto* packsVerify = packs->add_subcommand ("verify", "Check the files and sha256 of the packs (exit 2 if something is wrong)");
    packsVerify->add_option ("id", o.packId, "Only this pack");
    packsVerify->add_flag ("--json", o.json, "Print JSON");
    packsVerify->callback ([&] { std::exit (rovox::cli::runPacksVerify (o)); });

    auto* storage = app.add_subcommand ("storage", "Object storage (S3-compatible) administration");
    storage->require_subcommand (1);
    auto* storagePush = storage->add_subcommand ("push", "Upload every local project (snapshot, media, renders) to the bucket; projects that are already up to date are skipped");
    storagePush->add_option ("--storage", o.storage, "s3 | local (see the global option)")->check (CLI::IsMember ({ "s3", "local" }));
    storagePush->callback ([&] { std::exit (rovox::cli::runStoragePush (o)); });

    auto* mcp = app.add_subcommand ("mcp", "MCP over stdio: the same tools as 'serve', for Claude Code and other local clients");
    mcp->add_option ("--connect", o.connectUrl, "Proxy to a running 'rovox serve' (stdio to HTTP), e.g. http://127.0.0.1:4810, instead of opening the projects in this process");
    mcp->add_option ("--token", o.token, "With --connect: Bearer token or API key (default: $ROVOX_KEY, then $ROVOX_TOKEN)");
    mcp->add_option ("--tenant", o.tenant, "Run this session as tenant <id> with every scope except admin (for an embedding executor; default: local, the operator)");
    mcp->add_option ("--allow-path", o.allowPaths, "Directory the tools may read besides the data dir (repeatable; default $ROVOX_ALLOWED_PATHS, ':' separated)");
    mcp->add_option ("--plugins", o.pluginsDir, "Extra plugin directory (VST3/LV2)");
    mcp->add_option ("--tool-names", o.toolNames, "MCP tool names: dots (track.list) | underscore (track_list)")->capture_default_str();
    mcp->add_flag ("--reaper-compat", o.reaperCompat, "Also offer the tool names of the old REAPER MCP bridge (list_projects, update_tracks ...)");
    mcp->add_option ("--source-url", o.sourceUrl, "Where the running source code can be obtained (default $ROVOX_SOURCE_URL)");
    mcp->add_option ("--storage", o.storage, "s3 | local | fs (see the global option)")->check (CLI::IsMember ({ "s3", "local", "fs" }));
    mcp->add_option ("--drain-timeout", o.drainTimeoutS, "On SIGTERM: seconds running jobs get to finish before they end 'interrupted'")->capture_default_str()->check (CLI::Range (0.0, 3600.0));
    mcp->callback ([&] { std::exit (rovox::cli::runMcp (o)); });

    auto* key = app.add_subcommand ("key", "API keys: create, list, revoke, rotate (works on <data>/auth/keys.json; a running server notices within a second)");
    key->require_subcommand (1);
    auto* keyCreate = key->add_subcommand ("create", "Create a key; the full key is printed once on stdout (and never again)");
    keyCreate->add_option ("--tenant", o.tenant, "Tenant the key belongs to")->required();
    keyCreate->add_option ("--scopes", o.keyScopes, "Comma separated: catalog:read,projects:read,projects:write,render,delivery,admin (admin only for tenant local; read and write are shorthands)")->required();
    keyCreate->add_option ("--label", o.keyLabel, "Free text to recognise the key");
    keyCreate->add_option ("--expires", o.keyExpires, "30d (days from now) or an ISO date / time in UTC");
    keyCreate->add_option ("--quota-profile", o.keyQuotaProfile, "Quota profile of this key (default: the tenant's)");
    keyCreate->add_flag ("--json", o.json, "Print {key, record} as JSON");
    keyCreate->callback ([&] { std::exit (rovox::cli::runKeyCreate (o)); });

    auto* keyList = key->add_subcommand ("list", "List keys (never their secrets)");
    keyList->add_option ("--tenant", o.tenant, "Only this tenant");
    keyList->add_flag ("--json", o.json, "Print a JSON array");
    keyList->callback ([&] { std::exit (rovox::cli::runKeyList (o)); });

    auto* keyRevoke = key->add_subcommand ("revoke", "Revoke a key");
    keyRevoke->add_option ("id", o.keyId, "Key id (k_...)")->required();
    keyRevoke->callback ([&] { std::exit (rovox::cli::runKeyRevoke (o)); });

    auto* keyRotate = key->add_subcommand ("rotate", "Create a replacement with the same tenant and scopes; the new key is printed once");
    keyRotate->add_option ("id", o.keyId, "Key id (k_...)")->required();
    keyRotate->add_option ("--grace", o.keyGraceS, "Seconds the old key keeps working (default: 0, revoked at once)")->check (CLI::NonNegativeNumber);
    keyRotate->add_flag ("--json", o.json, "Print {key, record} as JSON");
    keyRotate->callback ([&] { std::exit (rovox::cli::runKeyRotate (o)); });

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

    auto* bench = app.add_subcommand ("bench", "Resource benchmark: open, offline render and real-time playback of a generated project");
    bench->add_option ("--scenario", o.benchScenario, "s1 (8 tracks, EQ + compressor) | s2 (+ synth, reverbs) | s3 (32 tracks) | s4 (3 projects, 2 rendering while 1 plays) | s5 (16 tracks 44.1 kHz in a 48 kHz project; ROVOX_BENCH_RESAMPLING)")
         ->check (CLI::IsMember ({ "s1", "s2", "s3", "s4" }))->capture_default_str();
    bench->add_option ("--seconds", o.benchSeconds, "Song length, and the length of the real-time playback")->capture_default_str();
    bench->add_flag ("--json", o.json, "Compact JSON only on stdout");
    bench->callback ([&] { std::exit (rovox::cli::runBench (o)); });

    CLI11_PARSE (app, argc, argv);
    return 0;
}
