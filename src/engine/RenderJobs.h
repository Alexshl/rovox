// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Long operations as jobs (tasks 230, 240): render, preflight, preview, analysis, import planning.
// The message thread only takes a snapshot of the Edit (or reads a path) and hands the work to a JobManager worker;
// rendering, hashing and analysis never block other calls (spike P4 measured 1.8 s of head-of-line blocking).
#pragma once

#include "CommandBus.h"
#include "JobManager.h"
#include "Render.h"

namespace rovox
{

struct JobContextInfo
{
    std::string tenant = "local";
    std::string projectId;
    Origin origin = Origin::cli();
};

struct StartedJob
{
    std::string jobId;
    bool created = true;        // false: the client_request_id matched an existing job
};

class JobService
{
public:
    JobService (ProjectManager&, JobManager&, MessageThreadExecutor&);

    /** args: name, scope, track_ids, stems_through_master, range {start,end} (TimePos), format {container, bit_depth, bitrate_kbps, sample_rate, channels},
        tail_s, target_lufs, normalize, tolerance_lu, true_peak_ceiling_dbtp, ceiling_dbtp, limiter, check, fail_on, warn_on, seed, client_request_id, stall_timeout_s. */
    Result<StartedJob> startRender (const JobContextInfo&, const json& args, bool preflightOnly);

    /** args: range {start,end}, track_ids, format (mp3 | wav | flac), analysis [loudness, spectrum_bands, per_bar, tracks_presence], embed_audio, client_request_id. */
    Result<StartedJob> startPreview (const JobContextInfo&, const json& args);

    /** args: target (master | trk_...), range, kinds [...]. Renders to a temporary file that is deleted; no clip is kept. */
    Result<StartedJob> startAnalysis (const JobContextInfo&, const json& args);

    /** args as the import.plan command. The plan is stored in the project exactly as the command does. */
    Result<StartedJob> startImportPlan (const JobContextInfo&, const json& args);

    /** Keeps the newest `keep` previews younger than `maxAgeS` (task 240: 10 files, 24 h). Returns the number removed. */
    static int prunePreviews (const fs::path& dir, int keep = 10, double maxAgeS = 24 * 3600.0);

private:
    struct Prepared;
    Result<std::shared_ptr<Prepared>> prepare (const JobContextInfo&, const json& args, const std::string& kind, bool wantTrackIds);
    Result<json> runAnalysisJob (JobContext&, Prepared&, bool keepClip);
    Result<StartedJob> submit (const JobContextInfo&, const std::string& kind, const std::string& pool, int priority, const json& args, JobFn);

    ProjectManager& projects_;
    JobManager& jobs_;
    MessageThreadExecutor& executor_;
};

} // namespace rovox
