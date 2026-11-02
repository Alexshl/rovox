// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RenderJobs.h"

#include "Ids.h"
#include "Import.h"
#include "JsonSchema.h"
#include "Loudness.h"
#include "PluginHost.h"
#include "ProjectManager.h"
#include "Spectrum.h"
#include "TimePos.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace rovox
{

namespace
{
double round1 (double v)    { return std::round (v * 10.0) / 10.0; }
double round2 (double v)    { return std::round (v * 100.0) / 100.0; }

Error bad (const std::string& what, const std::string& path = {})
{
    return makeError (errc::invalidParams, what, path.empty() ? json (nullptr) : json { { "path", path } });
}

std::set<std::string> toSet (const json& a)
{
    std::set<std::string> s;

    for (auto& v : a)
        if (v.is_string())
            s.insert (v.get<std::string>());

    return s;
}

/** Everything a worker needs from the project, captured on the message thread. */
struct Captured
{
    std::string projectId, tenant;
    bool ephemeral = false;
    fs::path dir;
    std::uint64_t revision = 0;
    double sampleRate = 44100.0, startS = 0, endS = 0;
    std::vector<std::pair<std::string, std::string>> audioTracks;      // id, name
    std::vector<std::pair<int, double>> barStarts;                     // bar number, absolute start (clamped to the range start)
};

/** The bucket is the source of truth for renders too: a finished render is uploaded from the job's worker thread and its object key goes into the
    result. A failed upload does not fail the render (the file is in the cache); it is reported as storage_error and retried by `rovox storage push`. */
void uploadRenders (RemoteStorage* storage, const std::string& tenant, const std::string& projectId, json& result)
{
    if (storage == nullptr || ! result.is_object() || ! result.contains ("files") || ! result["files"].is_array())
        return;

    json keys = json::array(), errors = json::array();

    for (auto& f : result["files"])
    {
        const fs::path file = f.value ("abs_path", "");

        if (file.empty())
            continue;

        auto key = storage->uploadRender (tenant, projectId, file);

        if (key.ok())
        {
            f["key"] = key.value();
            keys.push_back (key.value());
        }
        else
        {
            errors.push_back (key.error().message);
        }
    }

    if (result["files"].size() > 0 && result.contains ("file") && result["file"].is_object())
        result["file"] = result["files"][0];

    result["storage"] = { { "backend", "s3" }, { "keys", keys } };

    if (! errors.empty())
        result["storage_error"] = errors;
}
} // namespace

struct JobService::Prepared
{
    Captured cap;
    RenderRequest req;
    std::shared_ptr<te::Edit> snapshot;                 // null when the render runs in a child process (the project has external plugins)
    fs::path childDir;                                  // project copy for the child process; empty = no external plugins
    std::shared_ptr<void> childCleanup;
    std::shared_ptr<void> activeToken;         // decrements Project::activeJobs on the message thread when released
    json args;
    std::vector<std::string> kinds;
    bool embedAudio = false;
    std::string target = "master";
};

JobService::JobService (ProjectManager& p, JobManager& j, MessageThreadExecutor& e) : projects_ (p), jobs_ (j), executor_ (e) {}

//==============================================================================
namespace
{
Result<void> parseRangeInto (Project& p, const json& args, RenderRequest& req)
{
    if (! args.contains ("range"))
        return {};

    const auto& r = args["range"];

    if (! r.is_object())
        return bad ("range must be {start, end} (TimePos)", "/range");

    if (r.contains ("start"))
    {
        auto s = timePosToSeconds (p, r["start"]);

        if (! s.ok())
            return Error { s.error().code, s.error().message, json { { "path", "/range/start" } } };

        req.range.startS = s.value();
    }

    if (r.contains ("end"))
    {
        auto s = timePosToSeconds (p, r["end"]);

        if (! s.ok())
            return Error { s.error().code, s.error().message, json { { "path", "/range/end" } } };

        req.range.endS = s.value();
    }
    else if (r.contains ("length"))
    {
        auto l = durationToSeconds (p, r["length"], req.range.startS.value_or (0.0));

        if (! l.ok())
            return Error { l.error().code, l.error().message, json { { "path", "/range/length" } } };

        req.range.endS = req.range.startS.value_or (0.0) + l.value();
    }

    return {};
}

Result<void> parseRenderArgs (Project& p, const json& args, RenderRequest& req)
{
    if (auto v = parseRangeInto (p, args, req); ! v.ok())
        return v;

    req.name = args.value ("name", req.name);
    req.scope = args.value ("scope", req.scope);

    if (args.contains ("track_ids"))
        for (auto& t : args["track_ids"])
            req.trackIds.push_back (t.get<std::string>());

    req.stemsThroughMaster = args.value ("stems_through_master", false);

    if (args.contains ("format"))
    {
        const auto& f = args["format"];
        req.format.container = f.value ("container", req.format.container);
        req.format.bitDepth = f.value ("bit_depth", req.format.bitDepth);
        req.format.bitrateKbps = f.value ("bitrate_kbps", req.format.bitrateKbps);
        req.format.sampleRate = f.value ("sample_rate", req.format.sampleRate);
        req.format.channels = f.value ("channels", req.format.channels);
    }

    req.tailS = args.value ("tail_s", req.tailS);
    req.clipTolerance = args.value ("clip_tolerance_samples", (std::int64_t) 0);
    req.stallTimeoutS = args.value ("stall_timeout_s", req.stallTimeoutS);

    if (args.contains ("seed"))
        req.seed = args["seed"].get<std::string>();

    const bool check = args.value ("check", false);

    if (args.contains ("target_lufs"))
    {
        req.loudness.targetLufs = args["target_lufs"].get<double>();
        req.loudness.toleranceLu = args.value ("tolerance_lu", req.loudness.toleranceLu);
        req.loudness.truePeakCeilingDb = args.value ("true_peak_ceiling_dbtp", req.loudness.truePeakCeilingDb);
        req.loudness.truePeakCeilingDb = args.value ("ceiling_dbtp", req.loudness.truePeakCeilingDb);
        req.loudness.mode = args.value ("normalize", true) ? "normalize" : (check ? "check" : "none");
    }

    if (args.contains ("limiter") && args["limiter"].is_boolean())
        req.loudness.limiter = args["limiter"].get<bool>();

    if (args.contains ("fail_on"))
        req.failOn = toSet (args["fail_on"]);

    if (args.contains ("warn_on"))
        req.warnOn = toSet (args["warn_on"]);

    // decision of M1: without `check` the findings are reported but never stop the render
    if (! check && ! args.contains ("fail_on"))
    {
        req.warnOn.insert (req.failOn.begin(), req.failOn.end());
        req.failOn.clear();
    }

    return {};
}
} // namespace

Result<std::shared_ptr<JobService::Prepared>> JobService::prepare (const JobContextInfo& who, const json& args, const std::string& kind, bool wantTrackIds)
{
    auto fut = executor_.run ([&]() -> Result<std::shared_ptr<Prepared>>
    {
        auto* project = projects_.find (who.projectId, who.tenant);

        if (project == nullptr)
            return makeError (errc::notFound, "no open project " + who.projectId);

        auto prep = std::make_shared<Prepared>();
        prep->args = args;

        if (auto v = parseRenderArgs (*project, args, prep->req); ! v.ok())
            return v.error();

        auto range = resolveRange (project->edit(), prep->req.range);

        if (! range.ok())
            return range.error();

        auto& cap = prep->cap;
        cap.projectId = project->id();
        cap.tenant = project->tenant();
        cap.ephemeral = project->manifest().ephemeral;
        cap.dir = project->dir();
        cap.revision = project->revision();
        cap.sampleRate = project->manifest().sampleRate;
        cap.startS = range.value().getStart().inSeconds();
        cap.endS = range.value().getEnd().inSeconds();
        prep->req.range.startS = cap.startS;
        prep->req.range.endS = cap.endS;
        prep->req.range.startBar.reset();
        prep->req.range.endBar.reset();

        if (wantTrackIds)
            for (auto* t : te::getAudioTracks (project->edit()))
                cap.audioTracks.emplace_back (Project::idOf (t->state), toStd (t->getName()));

        // bar boundaries of the range, for per-bar loudness
        auto& ts = project->edit().tempoSequence;
        const int firstBar = ts.toBarsAndBeats (range.value().getStart()).bars + 1;

        for (int bar = firstBar; bar < firstBar + 2000; ++bar)
        {
            te::tempo::BarsAndBeats bb;
            bb.bars = bar - 1;
            const double s = std::max (cap.startS, ts.toTime (bb).inSeconds());

            if (s >= cap.endS - 1.0e-6)
                break;

            cap.barStarts.emplace_back (bar, s);
        }

        // Projects with external plugins: the render runs in a child process, so a crashing plugin cannot take the server down (task 410).
        // The server never creates a second in-process copy of those plugins for a render; previews and analysis still analyse in-process,
        // but only after the same render passed in the child (the guard).
        if (editHasExternalPlugins (project->edit()))
        {
            prep->childDir = writeProjectSnapshot (*project, "job-" + newUlid());

            if (prep->childDir.empty())
                return makeError (errc::io, "cannot prepare the project copy for the render process");

            prep->childCleanup = std::shared_ptr<void> (static_cast<void*> (prep.get()), [dir = prep->childDir] (void*) { std::error_code ec; fs::remove_all (dir, ec); });
        }

        if (prep->childDir.empty() || kind != "render")
            prep->snapshot = std::shared_ptr<te::Edit> (makeRenderSnapshot (*project).release(), [ex = &executor_] (te::Edit* e)
            {
                ex->run ([e] { delete e; });        // an Edit must be destroyed on the message thread
            });

        ++project->activeJobs();
        const auto id = project->id();
        const auto tenant = project->tenant();
        prep->activeToken = std::shared_ptr<void> (static_cast<void*> (prep.get()), [ex = &executor_, pm = &projects_, id, tenant] (void*)
        {
            ex->run ([pm, id, tenant]
            {
                if (auto* p = pm->find (id, tenant))
                    --p->activeJobs();
            });
        });

        return prep;
    });

    if (fut.wait_for (std::chrono::seconds (60)) != std::future_status::ready)
        return makeError (errc::busy, "the message thread did not answer in time");

    try
    {
        return fut.get();
    }
    catch (const std::exception& e)
    {
        return makeError (errc::busy, e.what());
    }
}

Result<StartedJob> JobService::submit (const JobContextInfo& who, const std::string& kind, const std::string& pool, int priority, const json& args, JobFn fn)
{
    JobSpec spec;
    spec.kind = kind;
    spec.projectId = who.projectId;
    spec.tenant = who.tenant;
    spec.pool = pool;
    spec.priority = priority;
    spec.clientRequestId = args.value ("client_request_id", "");
    spec.origin = who.origin.toJson();
    auto s = jobs_.submit (std::move (spec), std::move (fn));
    return StartedJob { s.id, s.created };
}

//==============================================================================
Result<StartedJob> JobService::startRender (const JobContextInfo& who, const json& args, bool preflightOnly)
{
    // idempotency first: a repeated request must not take another snapshot
    if (args.contains ("client_request_id"))
    {
        for (auto& j : jobs_.list ({ .project = who.projectId, .tenant = who.tenant }))
            if (j.clientRequestId == args.value ("client_request_id", "") && (j.kind == "render" || j.kind == "preflight"))
                return StartedJob { j.id, false };
    }

    auto prep = prepare (who, args, "render", false);

    if (! prep.ok())
        return prep.error();

    auto p = prep.value();
    p->req.preflightOnly = preflightOnly;

    return submit (who, preflightOnly ? "preflight" : "render", "render", 0, args, [this, p, who, preflightOnly] (JobContext& jc) -> Result<json>
    {
        auto req = p->req;
        req.hooks.shouldCancel = [&jc] { return jc.cancelled(); };
        req.hooks.onProgress = [&jc] (double f, const std::string& stage) { jc.progress (f, stage); };

        RenderContext ctx;
        ctx.rendersDir = p->cap.dir / "renders";
        ctx.projectId = p->cap.projectId;
        ctx.sourceRevision = p->cap.revision;
        ctx.projectSampleRate = p->cap.sampleRate;

        auto result = p->childDir.empty() ? renderEdit (*p->snapshot, ctx, req)
                                          : renderInChild (p->childDir, projects_.host().dataDir(), ctx, req, [this, who, &jc] (const json& info)
                                            {
                                                json event = info;
                                                event["type"] = "plugin.crashed";
                                                event["project_id"] = who.projectId;
                                                event["job_id"] = jc.id();
                                                projects_.hub().publish (who.tenant, who.projectId, event);
                                            });

        if (result.ok() && ! preflightOnly && ! p->cap.ephemeral)
            uploadRenders (projects_.storage(), who.tenant, who.projectId, result.value());

        return result;
    });
}

//==============================================================================
namespace
{
/** Collects the analysis of one render pass from the audio tap. */
struct AnalysisTap
{
    AnalysisTap (double sr, const std::vector<std::pair<int, double>>& bars, double rangeStart, bool wantSpectrum, bool wantBars, size_t numOutputs)
        : sampleRate (sr), barStarts (bars), start (rangeStart), spectrumOn (wantSpectrum), barsOn (wantBars), sumSquares (numOutputs, 0.0), frames (numOutputs, 0)
    {
        if (wantSpectrum)
            spectrum = std::make_unique<SpectrumAnalyser> (sr);
    }

    void operator() (int output, const float* const* planar, int channels, int n)
    {
        if (output < 0 || (size_t) output >= sumSquares.size())
            return;

        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                sumSquares[(size_t) output] += (double) planar[c][i] * planar[c][i];

        frames[(size_t) output] += (std::int64_t) n * channels;

        if (output != 0)
            return;

        if (spectrum)
        {
            mono.resize ((size_t) n);

            for (int i = 0; i < n; ++i)
            {
                float v = 0;

                for (int c = 0; c < channels; ++c)
                    v += planar[c][i];

                mono[(size_t) i] = v / (float) channels;
            }

            spectrum->add (mono.data(), n);
        }

        if (barsOn && ! barStarts.empty())
            feedBars (planar, channels, n);
    }

    void feedBars (const float* const* planar, int channels, int n)
    {
        std::vector<const float*> ptrs ((size_t) channels);

        for (int pos = 0; pos < n;)
        {
            if (! current)
            {
                current = std::make_unique<StatsAccumulator> (channels, sampleRate);
                currentFrames = 0;
            }

            // end of the current bar in frames from the start of the range
            const double barEndS = barIndex + 1 < barStarts.size() ? barStarts[barIndex + 1].second - start : 1.0e12;
            const std::int64_t barEndFrame = (std::int64_t) std::llround (barEndS * sampleRate);
            const int take = (int) std::min<std::int64_t> (n - pos, std::max<std::int64_t> (1, barEndFrame - framesDone));

            for (int c = 0; c < channels; ++c)
                ptrs[(size_t) c] = planar[c] + pos;

            current->add (ptrs.data(), take);
            currentFrames += take;
            framesDone += take;
            pos += take;

            if (framesDone >= barEndFrame && barIndex + 1 < barStarts.size())
                closeBar();
        }
    }

    void closeBar()
    {
        if (! current)
            return;

        const auto stats = current->finish();
        bars.push_back ({ { "bar", barStarts[barIndex].first }, { "start_s", round2 (barStarts[barIndex].second) }, { "lufs", round1 (stats.integratedLufs) },
                          { "true_peak_dbtp", round1 (stats.truePeakDbtp) } });
        current.reset();
        ++barIndex;
    }

    json finish()
    {
        if (barsOn && current && barIndex < barStarts.size())
            closeBar();

        return bars;
    }

    double sampleRate;
    std::vector<std::pair<int, double>> barStarts;
    double start;
    bool spectrumOn, barsOn;
    std::vector<double> sumSquares;
    std::vector<std::int64_t> frames;
    std::unique_ptr<SpectrumAnalyser> spectrum;
    std::unique_ptr<StatsAccumulator> current;
    std::vector<float> mono;
    size_t barIndex = 0;
    std::int64_t framesDone = 0, currentFrames = 0;
    json bars = json::array();
};

std::mutex previewPruneMutex;
} // namespace

int JobService::prunePreviews (const fs::path& dir, int keep, double maxAgeS)
{
    std::lock_guard<std::mutex> lock (previewPruneMutex);
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;

    for (auto& f : fs::directory_iterator (dir, ec))
    {
        const auto name = f.path().filename().string();

        if (f.is_regular_file() && name.rfind ("pv_", 0) == 0 && f.path().extension() != ".json")
            files.emplace_back (f.last_write_time(), f.path());
    }

    std::sort (files.begin(), files.end(), [] (auto& a, auto& b) { return a.first > b.first; });
    int removed = 0;
    const auto now = fs::file_time_type::clock::now();

    for (size_t i = 0; i < files.size(); ++i)
    {
        const double age = std::chrono::duration<double> (now - files[i].first).count();

        if ((int) i >= keep || age > maxAgeS)
        {
            fs::remove (files[i].second, ec);
            auto sidecar = files[i].second;
            sidecar.replace_extension (".render.json");
            fs::remove (sidecar, ec);
            ++removed;
        }
    }

    return removed;
}

Result<json> JobService::runAnalysisJob (JobContext& jc, Prepared& p, bool keepClip)
{
    const bool wantLoudness = std::find (p.kinds.begin(), p.kinds.end(), "loudness") != p.kinds.end();
    const bool wantSpectrum = std::find (p.kinds.begin(), p.kinds.end(), "spectrum_bands") != p.kinds.end();
    const bool wantBars = std::find (p.kinds.begin(), p.kinds.end(), "per_bar") != p.kinds.end();
    const bool wantPresence = std::find (p.kinds.begin(), p.kinds.end(), "tracks_presence") != p.kinds.end();
    (void) wantLoudness;

    const auto previewsDir = p.cap.dir / "previews";
    const auto tmpDir = previewsDir / (".tmp-" + jc.id());
    std::error_code ec;
    fs::create_directories (keepClip ? previewsDir : tmpDir, ec);

    struct Cleanup { fs::path dir; ~Cleanup() { std::error_code e; fs::remove_all (dir, e); } } cleanup { tmpDir };

    auto req = p.req;
    req.hooks.shouldCancel = [&jc] { return jc.cancelled(); };

    const double presenceShare = wantPresence ? 0.4 : 0.0;
    req.hooks.onProgress = [&jc, presenceShare] (double f, const std::string& stage) { jc.progress (f * (1.0 - presenceShare), stage); };

    AnalysisTap tap (req.format.sampleRate > 0 ? req.format.sampleRate : p.cap.sampleRate, p.cap.barStarts, p.cap.startS, wantSpectrum, wantBars, 1);
    req.hooks.audioTap = [&tap] (int o, const float* const* planar, int ch, int n) { tap (o, planar, ch, n); };

    RenderContext ctx;
    ctx.rendersDir = keepClip ? previewsDir : tmpDir;
    ctx.projectId = p.cap.projectId;
    ctx.sourceRevision = p.cap.revision;
    ctx.projectSampleRate = p.cap.sampleRate;

    if (! p.childDir.empty())
    {
        // the guard: the same render in a child process, kept only to learn whether a plugin survives it
        auto guard = p.req;
        guard.name = "guard";
        guard.format = RenderFormat { "wav", 16, 128, p.cap.sampleRate, 2 };
        guard.loudness.mode = "none";
        guard.failOn.clear();
        guard.warnOn.clear();
        guard.tailS = 0.0;
        guard.hooks = {};
        guard.hooks.shouldCancel = [&jc] { return jc.cancelled(); };
        RenderContext gctx = ctx;
        gctx.rendersDir = tmpDir / "guard";
        fs::create_directories (gctx.rendersDir, ec);
        jc.progress (0.0, "rendering in a separate process (plugin check)");

        auto g = renderInChild (p.childDir, projects_.host().dataDir(), gctx, guard, [this, &p, &jc] (const json& info)
        {
            json event = info;
            event["type"] = "plugin.crashed";
            event["project_id"] = p.cap.projectId;
            event["job_id"] = jc.id();
            projects_.hub().publish (p.cap.tenant, p.cap.projectId, event);
        });

        if (! g.ok())
            return g.error();
    }

    const auto clipName = req.name;
    auto result = renderEdit (*p.snapshot, ctx, req);

    if (! result.ok())
        return result.error();

    const auto& r = result.value();
    const auto& stats = r["stats"];
    json out;
    out["source_revision"] = p.cap.revision;
    out["range"] = { { "start_s", round2 (p.cap.startS) }, { "end_s", round2 (p.cap.endS) }, { "length_s", round2 (p.cap.endS - p.cap.startS) } };
    out["loudness"] = { { "integrated_lufs", stats["integrated_lufs"] }, { "short_term_max_lufs", stats["short_term_max_lufs"] }, { "true_peak_dbtp", stats["true_peak_dbtp"] },
                        { "sample_peak_dbfs", stats["sample_peak_dbfs"] }, { "rms_dbfs", stats["rms_dbfs"] },
                        { "crest_db", round1 (stats["sample_peak_dbfs"].get<double>() - stats["rms_dbfs"].get<double>()) }, { "clipped_samples", stats["clipped_samples"] } };

    if (r.contains ("loudness") && r["loudness"].value ("mode", "none") != "none")
        out["loudness_processing"] = r["loudness"];

    if (wantSpectrum && tap.spectrum)
        out["spectrum_bands_db"] = tap.spectrum->finish().toJson();

    if (wantBars)
        out["per_bar"] = tap.finish();

    json warnings = json::array();

    for (auto& f : r["preflight"]["warnings"])
        warnings.push_back (f.value ("message", ""));

    if (keepClip)
    {
        const auto& file = r["file"];
        const auto name = fs::path (file["path"].get<std::string>()).filename().string();
        out["clip"] = { { "uri", "rovox://project/" + p.cap.projectId + "/previews/" + name }, { "url", "/api/projects/" + p.cap.projectId + "/previews/" + name },
                        { "path", "previews/" + name }, { "abs_path", file["abs_path"] }, { "duration_s", file["duration_s"] }, { "container", file["container"] },
                        { "bytes", file["bytes"] }, { "sample_rate", file["sample_rate"] }, { "channels", file["channels"] }, { "source_revision", p.cap.revision } };
        prunePreviews (previewsDir);
    }

    // tracks_presence: render every audio track as a stem of the range and compare the energy
    if (wantPresence)
    {
        if (p.cap.audioTracks.empty())
        {
            out["tracks"] = json::array();
        }
        else
        {
            auto stemReq = p.req;
            stemReq.scope = "stems";
            stemReq.name = "presence";
            stemReq.trackIds.clear();

            for (auto& t : p.cap.audioTracks)
                stemReq.trackIds.push_back (t.first);

            stemReq.tailS = 0.0;
            stemReq.loudness.mode = "none";
            stemReq.failOn.clear();
            stemReq.warnOn.clear();
            stemReq.format = RenderFormat { "wav", 16, 128, p.cap.sampleRate, 2 };
            stemReq.hooks = {};
            stemReq.hooks.shouldCancel = [&jc] { return jc.cancelled(); };
            stemReq.hooks.onProgress = [&jc, presenceShare] (double f, const std::string& stage) { jc.progress ((1.0 - presenceShare) + presenceShare * f, "presence: " + stage); };

            AnalysisTap stemTap (p.cap.sampleRate, {}, p.cap.startS, false, false, p.cap.audioTracks.size());
            stemReq.hooks.audioTap = [&stemTap] (int o, const float* const* planar, int ch, int n) { stemTap (o, planar, ch, n); };

            const auto stemDir = tmpDir / "stems";
            fs::create_directories (stemDir, ec);
            RenderContext sctx = ctx;
            sctx.rendersDir = stemDir;
            auto stems = renderEdit (*p.snapshot, sctx, stemReq);

            if (! stems.ok())
                return stems.error();

            double total = 0;

            for (size_t i = 0; i < stemTap.sumSquares.size(); ++i)
                total += stemTap.frames[i] > 0 ? stemTap.sumSquares[i] / (double) stemTap.frames[i] : 0.0;

            json tracks = json::array();

            for (size_t i = 0; i < p.cap.audioTracks.size(); ++i)
            {
                const double ms = stemTap.frames[i] > 0 ? stemTap.sumSquares[i] / (double) stemTap.frames[i] : 0.0;
                tracks.push_back ({ { "track_id", p.cap.audioTracks[i].first }, { "name", p.cap.audioTracks[i].second }, { "rms_dbfs", round1 (ms > 1.0e-12 ? 10.0 * std::log10 (ms) : silenceDb) },
                                    { "share_pct", total > 0 ? round1 (100.0 * ms / total) : 0.0 } });
            }

            out["tracks"] = tracks;
        }
    }

    out["warnings"] = warnings;
    return out;
}

Result<StartedJob> JobService::startPreview (const JobContextInfo& who, const json& args)
{
    if (args.contains ("client_request_id"))
        for (auto& j : jobs_.list ({ .project = who.projectId, .tenant = who.tenant, .kind = "preview" }))
            if (j.clientRequestId == args.value ("client_request_id", ""))
                return StartedJob { j.id, false };

    json a = args;
    a["name"] = "pv_" + newUlid();
    a["scope"] = args.contains ("track_ids") ? "tracks" : "master";
    a["check"] = false;
    a["tail_s"] = 0.0;

    const auto container = args.value ("format", "mp3");

    if (container != "mp3" && container != "wav" && container != "flac")
        return bad ("format must be mp3, wav or flac", "/format");

    a["format"] = { { "container", container }, { "bitrate_kbps", 128 }, { "bit_depth", 16 }, { "channels", 2 } };
    a.erase ("target_lufs");

    // a missing range previews the first 30 s of the project
    if (! a.contains ("range"))
        a["range"] = { { "start", { { "s", 0.0 } } }, { "end", { { "s", 30.0 } } } };

    auto prep = prepare (who, a, "preview", true);

    if (! prep.ok())
        return prep.error();

    auto p = prep.value();

    if (p->cap.endS > p->cap.startS + 300.0)
        return bad ("a preview is at most 300 s long; use render.start for the full mix", "/range");

    p->req.hooks = {};
    p->target = "master";
    p->kinds = args.contains ("analysis") ? args["analysis"].get<std::vector<std::string>>() : std::vector<std::string> { "loudness", "spectrum_bands", "per_bar" };
    p->kinds.push_back ("loudness");
    p->embedAudio = args.value ("embed_audio", false);
    p->req.failOn.clear();

    return submit (who, "preview", "render", 10, args, [this, p] (JobContext& jc) -> Result<json> { return runAnalysisJob (jc, *p, true); });
}

Result<StartedJob> JobService::startAnalysis (const JobContextInfo& who, const json& args)
{
    json a = args;
    const std::string target = args.value ("target", "master");
    a["name"] = "analysis";
    a["check"] = false;
    a["tail_s"] = 0.0;
    a["scope"] = target == "master" ? "master" : "tracks";

    if (target != "master")
        a["track_ids"] = json::array ({ target });

    a["format"] = { { "container", "wav" }, { "bit_depth", 16 }, { "channels", 2 } };
    a.erase ("target_lufs");

    auto prep = prepare (who, a, "analysis", target == "master");

    if (! prep.ok())
        return prep.error();

    auto p = prep.value();
    p->req.hooks = {};
    p->req.failOn.clear();
    p->kinds = args.contains ("kinds") ? args["kinds"].get<std::vector<std::string>>() : std::vector<std::string> { "loudness", "spectrum_bands" };
    p->kinds.push_back ("loudness");
    // presence only makes sense for the whole mix
    if (target != "master")
        p->kinds.erase (std::remove (p->kinds.begin(), p->kinds.end(), std::string ("tracks_presence")), p->kinds.end());

    return submit (who, "analysis", "analysis", 5, args, [this, p] (JobContext& jc) -> Result<json> { return runAnalysisJob (jc, *p, false); });
}

//==============================================================================
Result<StartedJob> JobService::startImportPlan (const JobContextInfo& who, const json& args)
{
    struct Info { fs::path mediaDir; };

    auto fut = executor_.run ([&]() -> Result<Info>
    {
        auto* project = projects_.find (who.projectId, who.tenant);

        if (project == nullptr)
            return makeError (errc::notFound, "no open project " + who.projectId);

        return Info { project->mediaDir() };
    });

    if (fut.wait_for (std::chrono::seconds (60)) != std::future_status::ready)
        return makeError (errc::busy, "the message thread did not answer in time");

    auto info = fut.get();

    if (! info.ok())
        return info.error();

    const auto options = importOptionsFrom (args);
    const auto sources = importSourcesFrom (args);

    for (auto& source : sources)
        if (auto allowed = projects_.host().checkPath (source); ! allowed.ok())
            return allowed.error();

    const auto mediaDir = info.value().mediaDir;
    const auto tenant = who.tenant, projectId = who.projectId;

    return submit (who, "import_plan", "io", 0, args, [this, options, sources, mediaDir, tenant, projectId] (JobContext& jc) -> Result<json>
    {
        jc.progress (0.05, "probing, hashing and staging media");
        auto plan = planImport (sources, mediaDir, options);

        if (! plan.ok())
            return plan.error();

        if (jc.cancelled())
            return makeError (errc::cancelled, "cancelled");

        auto j = plan.value().toJson();
        auto stored = executor_.run ([&]() -> Result<void>
        {
            auto* project = projects_.find (projectId, tenant);

            if (project == nullptr)
                return makeError (errc::notFound, "the project was closed while the plan was being made");

            project->importPlans()[plan.value().planId] = j;
            return {};
        });

        if (stored.wait_for (std::chrono::seconds (60)) != std::future_status::ready)
            return makeError (errc::busy, "the message thread did not answer in time");

        auto s = stored.get();

        if (! s.ok())
            return s.error();

        return j;
    });
}

} // namespace rovox
