// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <string>
#include <vector>

namespace rovox::cli
{

struct Options
{
    std::string dataDir;

    // serve
    int port = 4810;
    std::string bind = "127.0.0.1", audio = "none", token, pluginsDir, config, webDir;
    std::vector<std::string> allowPaths;
    bool generateToken = false;

    // mcp / serve
    std::string toolNames = "dots", sourceUrl;
    bool reaperCompat = false;
    std::vector<std::string> allowOrigins;

    // render
    std::string target, out, renderName, range, format, scope = "master", seed;
    double startS = -1, endS = -1, sampleRate = 0, tailS = 2.0, targetLufs = 0, toleranceLu = 1.0, truePeakCeiling = -1.0;
    int bitDepth = 24, bitrate = 320, channels = 2;
    bool normalize = false, check = false, json = false, preflightOnly = false;
    std::vector<std::string> trackIds;

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
int runMcp (const Options&);

} // namespace rovox::cli
