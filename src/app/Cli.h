// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rovox::cli
{

struct Options
{
    std::string dataDir, storage;       // storage: s3 | local (default: $ROVOX_STORAGE, else s3 when S3_ENDPOINT and S3_BUCKET are set)

    // serve
    int port = 4810;
    std::string bind = "127.0.0.1", audio = "none", token, pluginsDir, config, webDir;
    std::vector<std::string> allowPaths;
    bool generateToken = false;

    // mcp / serve
    std::string toolNames = "dots", sourceUrl;
    bool reaperCompat = false;
    std::vector<std::string> allowOrigins;
    std::vector<std::string> allowHosts;
    std::string connectUrl;             // mcp --connect: URL of a running `rovox serve`
    double drainTimeoutS = 20;          // serve / mcp (in-process): how long running jobs may finish after SIGTERM/SIGINT before they end `interrupted`
    std::string tenant;                 // mcp (in-process): the tenant of this stdio session, every scope but admin (default: local, the operator)

    // key create | list | revoke | rotate (--tenant is `tenant` above)
    std::string keyId, keyScopes, keyLabel, keyExpires, keyQuotaProfile;
    std::int64_t keyGraceS = 0;

    // render
    std::string target, out, renderName, range, format, scope = "master", seed;
    double startS = -1, endS = -1, sampleRate = 0, tailS = 2.0, targetLufs = 0, toleranceLu = 1.0, truePeakCeiling = -1.0;
    int bitDepth = 24, bitrate = 320, channels = 2;
    bool limiter = false, noLimiter = false;
    bool normalize = false, check = false, json = false, preflightOnly = false;
    std::vector<std::string> trackIds;

    // bench
    std::string benchScenario = "s1";
    double benchSeconds = 20.0;

    // packs verify
    std::string packId;

    // hidden child commands (plugin-probe, render-child)
    std::string probeFile, snapshotDir, requestFile;
    bool presetsOnly = false;

    // import / upload
    std::vector<std::string> paths;
    std::string project, newName, preset = "auto", align = "start", takes = "separate", rename, server;
    bool noCopyMedia = false;
    int chunkMb = 8;
};

/** Exit codes: 0 success, 1 error, 2 checks failed. */
int runServe (const Options&);
int runRender (const Options&);
int runImport (const Options&);
int runUpload (const Options&);
int runPlugins (const Options&);
int runStoragePush (const Options&);
int runKeyCreate (const Options&);    // key management works on <data>/auth/keys.json directly; a running server picks the change up within a second
int runKeyList (const Options&);
int runKeyRevoke (const Options&);
int runKeyRotate (const Options&);
int runMcp (const Options&);          // in-process, or (with --connect) a stdio proxy to a running server
int runPacksList (const Options&);    // instrument packs (SoundFonts played by rovox:sf2): list, and check files and hashes (exit 2 on a problem)
int runPacksVerify (const Options&);
int runPluginProbe (const Options&);
int runRenderChildCommand (const Options&);

} // namespace rovox::cli
