// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Shared helpers for the engine tests: one EngineHost per process, temp dirs, synthetic WAV generation.
#pragma once

#include "CommandBus.h"
#include "EngineHost.h"
#include "EventHub.h"
#include "ProjectManager.h"

#include <vector>

namespace rovox::test
{

/** The process-wide engine (created on first use on the main thread, data dir in a fresh temp directory). */
EngineHost& host();

/** A fresh empty directory below the test temp root. */
fs::path makeTempDir (const std::string& tag);

/** Writes a WAV (channels given as planar float vectors) at `bits` (16/24/32 where 32 = float). */
void writeWav (const fs::path& file, const std::vector<std::vector<float>>& channels, double sampleRate, int bits = 24);

std::vector<float> sine (double hz, double dbfs, double seconds, double sampleRate);

/** Reads a whole WAV into planar float. Returns false if unreadable. */
bool readWav (const fs::path& file, std::vector<std::vector<float>>& channels, double& sampleRate);

/** Pumps the JUCE message loop on the calling (main) thread for `ms` milliseconds. */
void pumpMessages (int ms);

/** Everything a command test needs: hub, project manager, registry, executor and bus on the shared engine. */
struct Env
{
    Env();

    /** Runs a command on the message thread (the calling thread) as an MCP client "Claude". */
    CommandResult run (const std::string& project, const std::string& name, const json& params = json::object(),
                       std::optional<std::uint64_t> ifRevision = std::nullopt);
    CommandResult runAs (const Origin&, const std::string& project, const std::string& name, const json& params);
    /** Same, and requires success; returns the result data. */
    json ok (const std::string& project, const std::string& name, const json& params = json::object());
    std::string newProject (const std::string& name = "Test", bool ephemeral = false, std::optional<int> ttl = std::nullopt);
    Project& project (const std::string& id);

    EventHub hub;
    ProjectManager pm;
    CommandRegistry registry;
    MessageThreadExecutor executor;
    CommandBus bus;
};

/** A synthetic Zoom R20 project folder: REC01_01.WAV ... mono 24-bit 44.1 kHz with distinct lengths, a stub R20PRJDATA.r20prj, DATA/ and peaks/. */
fs::path makeR20Folder (const fs::path& parent, const std::string& name, int numTracks, double seconds);

/** Normalised view of a project for equality checks: tracks, items, plugins, markers with ids and parameters. */
json snapshot (Env&, const std::string& projectId);

/** Entry for `rovox_engine_tests --child <mode> ...` (see TestMain.cpp). */
int runChildMode (int argc, char** argv);

} // namespace rovox::test
