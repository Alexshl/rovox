// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Long operations as jobs (tasks 230, 240): render, preflight, preview, analysis, import planning.
// The message thread only takes a snapshot of the Edit (or reads a path) and hands the work to a JobManager worker;
// rendering, hashing and analysis never block other calls (spike P4 measured 1.8 s of head-of-line blocking).
#pragma once

#include "CommandBus.h"
#include "JobCallback.h"
#include "JobFiles.h"
#include "JobManager.h"
#include "Quota.h"
#include "Render.h"

namespace rovox
{

struct JobContextInfo
{
    std::string tenant = "local";
    std::string projectId;
    Origin origin = Origin::cli();
    std::string keyId;              // the calling key (usage journal, per-key accounting); empty = the local operator
    std::string quotaProfile;       // the key's own quota profile, empty = the tenant's
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
        tail_s, target_lufs, normalize, tolerance_lu, true_peak_ceiling_dbtp, ceiling_dbtp, limiter, check, fail_on, warn_on, seed, client_request_id, stall_timeout_s,
        callback {url, secret_ref} (checked against [callbacks] of the configuration file, see JobCallback.h). */
    Result<StartedJob> startRender (const JobContextInfo&, const json& args, bool preflightOnly);

    /** One job, N renders of the same project snapshot: args are the render.start arguments as defaults plus `variants` [{name, ...any render argument:
        range, scope, track_ids (a mute set = the other tracks), format, target_lufs, tail_s...}] (`clips` is an alias), at most maxBatchVariants. Names are unique.
        Progress is (variant + fraction) / N; the quota is admitted once for the summed audio of all variants (QUOTA_EXCEEDED creates nothing).
        A failing variant is listed with its error and the others still render; the job fails only when every variant failed.
        Result: {variants:[{index, name, status, files, usage{audio_s}, error?}], files (all, each with `variant`), count, failed}. */
    Result<StartedJob> startBatch (const JobContextInfo&, const json& args);

    static constexpr size_t maxBatchVariants = 50;
    /** Lifetime of the signed URL in a finished job's file entries (seconds). */
    static constexpr std::int64_t fileUrlTtlS = 24 * 3600;

    /** args: range {start,end}, track_ids, format (mp3 | wav | flac), analysis [loudness, spectrum_bands, per_bar, tracks_presence], embed_audio, client_request_id. */
    Result<StartedJob> startPreview (const JobContextInfo&, const json& args);

    /** args: target (master | trk_...), range, kinds [...]. Renders to a temporary file that is deleted; no clip is kept. */
    Result<StartedJob> startAnalysis (const JobContextInfo&, const json& args);

    /** args as the import.plan command. The plan is stored in the project exactly as the command does. */
    Result<StartedJob> startImportPlan (const JobContextInfo&, const json& args);

    /** TOML file whose [callbacks] table says which hosts a job callback may target (read at every render.start; no file = no callbacks). */
    /** The same file holds [quota_profiles] and [tenants.<id>] (see Quota.h). */
    void setConfigFile (fs::path file)      { quotas().setConfigFile (file); configFile_ = std::move (file); }
    JobCallbackDispatcher& callbacks()      { return callbacks_; }

    /** Keeps the newest `keep` previews younger than `maxAgeS` (task 240: 10 files, 24 h). Returns the number removed. */
    static int prunePreviews (const fs::path& dir, int keep = 10, double maxAgeS = 24 * 3600.0);

private:
    struct Prepared;
    /** Validates `callback` (null target when absent); an error refuses the whole request. */
    Result<std::optional<CallbackTarget>> checkCallback (const json& args, bool allowed);
    /** Copies the output files of a finished render / preview / batch result into the job's out/ directory and gives every file entry a signed `url`
        and `expires_at` (the former `url` stays as `api_url`). A file that cannot be published keeps its entry without a signed url. */
    void publishJobFiles (const std::string& tenant, const std::string& jobId, json& result);
    Result<std::shared_ptr<Prepared>> prepare (const JobContextInfo&, const json& args, const std::string& kind, bool wantTrackIds);
    Result<json> runAnalysisJob (JobContext&, Prepared&, bool keepClip);
    /** `meteredAudioS` >= 0 makes the job count against the tenant's quotas (concurrent jobs, CPU seconds per day, audio minutes per month): it is
        admitted before the job exists (QUOTA_EXCEEDED creates nothing), its CPU is measured around `fn` and the result gains `usage {cpu_s, audio_s}`
        (the same numbers as the usage journal record). */
    Result<StartedJob> submit (const JobContextInfo&, const std::string& kind, const std::string& pool, int priority, const json& args, JobFn,
                               json callbackState = nullptr, std::function<void (const JobInfo&)> onFinished = nullptr, double meteredAudioS = -1);

    ProjectManager& projects_;
    JobManager& jobs_;
    MessageThreadExecutor& executor_;
    fs::path configFile_;
    JobCallbackDispatcher callbacks_;
    JobFiles jobFiles_;
};

} // namespace rovox
