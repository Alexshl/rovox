// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Job API for long operations (task 230): render, preview, analysis, import probe. A job has an id, a state, progress,
// cancellation and a stored result. Jobs run on their own worker threads, never on the message thread.
#pragma once

#include "Result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace rovox
{

enum class JobState { queued, running, succeeded, failed, cancelled };

const char* toString (JobState);
inline bool isFinished (JobState s) { return s == JobState::succeeded || s == JobState::failed || s == JobState::cancelled; }

struct JobInfo
{
    std::string id, kind, projectId, tenant = "local", pool, clientRequestId;
    JobState state = JobState::queued;
    double progress = 0;
    std::string stage;
    json result = nullptr;
    std::optional<Error> error;
    json origin = nullptr;
    std::string created, started, finished;

    json toJson() const;
    static std::optional<JobInfo> fromJson (const json&);
};

struct JobSpec
{
    std::string kind;                   // render | preview | analysis | import_plan | ...
    std::string projectId;
    std::string tenant = "local";
    std::string pool = "render";        // render | analysis | io
    int priority = 0;                   // higher runs first inside a pool (preview 10, render 0)
    std::string clientRequestId;        // idempotency: same (tenant, project, client_request_id) returns the same job
    json origin = nullptr;
};

class JobManager;

class JobContext
{
public:
    const std::string& id() const               { return id_; }
    /** Progress 0..1 and a short stage name. Cheap; throttled by the manager when it reports changes. */
    void progress (double fraction, const std::string& stage);
    bool cancelled() const                      { return cancel_->load(); }

private:
    friend class JobManager;
    JobContext (JobManager& m, std::string id, std::shared_ptr<std::atomic<bool>> c) : manager_ (m), id_ (std::move (id)), cancel_ (std::move (c)) {}

    JobManager& manager_;
    std::string id_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

using JobFn = std::function<Result<json> (JobContext&)>;

struct JobPools
{
    int render = 2, analysis = 2, io = 2;
};

class JobManager
{
public:
    /** Called (on the worker or submitting thread, never under the manager's lock) when a job is queued, starts, finishes or
        its progress moved by at least 5 %. */
    using ChangeFn = std::function<void (const JobInfo&)>;

    /** `dir`: where `<id>.json` records are kept ("" = memory only). Jobs found there that never finished become
        `failed` with SERVER_RESTARTED. */
    JobManager (std::filesystem::path dir, JobPools = {}, ChangeFn = nullptr);
    ~JobManager();

    struct Submitted { std::string id; bool created = true; };
    Submitted submit (JobSpec, JobFn);

    /** Queued jobs are cancelled at once; a running job sees cancelled() and normally finishes within a second.
        Returns false for an unknown or already finished job. */
    bool cancel (const std::string& id);

    /** `wait`: long-poll, returns as soon as the job is finished or the time is up (or `abort` becomes true). */
    std::optional<JobInfo> get (const std::string& id, std::chrono::milliseconds wait = std::chrono::milliseconds (0), const std::atomic<bool>* abort = nullptr);

    struct Filter { std::string project, tenant; std::optional<JobState> state; std::string kind; };
    std::vector<JobInfo> list (const Filter& = {}) const;

    /** Cancels everything and joins the workers. */
    void shutdown();

private:
    friend class JobContext;
    struct Entry;
    struct Pool;

    void worker (Pool&);
    void report (const JobInfo&);
    void persist (const JobInfo&);
    void setProgress (const std::string& id, double, const std::string&);
    void evictOldFinished();

    std::filesystem::path dir_;
    ChangeFn onChange_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;                    // job finished / changed
    std::map<std::string, std::shared_ptr<Entry>> jobs_;
    std::map<std::string, std::string> byRequest_;  // "tenant|project|client_request_id" -> job id
    std::map<std::string, std::unique_ptr<Pool>> pools_;
    bool stopping_ = false;
};

} // namespace rovox
