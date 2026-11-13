// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Cli.h"

#include "CommandBus.h"
#include "DrumKitLibrary.h"
#include "EngineHost.h"
#include "EventWebhooks.h"
#include "FsObjectStore.h"
#include "Gateway.h"
#include "HttpServer.h"
#include "Ids.h"
#include "Import.h"
#include "JobManager.h"
#include "KeyStore.h"
#include "McpHttp.h"
#include "McpStdio.h"
#include "PluginCatalog.h"
#include "PluginHost.h"
#include "PluginScan.h"
#include "ProjectManager.h"
#include "Quota.h"
#include "RemoteStorage.h"
#include "RequestGuard.h"
#include "Render.h"
#include "RenderJobs.h"
#include "Sha1.h"
#include "Sha256.h"

#include <httplib.h>

#include <toml++/toml.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <csignal>
#include <random>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <unistd.h>

namespace rovox::cli
{

namespace
{
std::atomic<bool> stopRequested { false };

void onSignal (int)     { stopRequested = true; }

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

std::vector<std::string> split (const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss (s);
    std::string item;

    while (std::getline (ss, item, sep))
        if (! item.empty())
            out.push_back (item);

    return out;
}

/** JUCE and Tracktion log to stderr, so stdout carries only what the command prints (important for --json). */
EngineConfig engineConfig (const Options& o, bool scanOutOfProcess)
{
    EngineConfig c;
    c.dataDir = resolveDataDir (o.dataDir);
    c.pluginsDir = o.pluginsDir;
    c.scanPluginsOutOfProcess = scanOutOfProcess;

    if (auto mode = parseAudioMode (o.audio))
        c.audio = *mode;

    return c;
}

int fail (const std::string& message)
{
    std::cerr << "error: " << message << "\n";
    return 1;
}

std::string fmtDb (double v)
{
    char b[32];
    std::snprintf (b, sizeof (b), "%.1f", v);
    return b;
}

Result<std::string> openProject (ProjectManager& pm, const std::string& target)
{
    auto id = pm.open (target);

    if (! id.ok() && id.error().code == errc::busy)
    {
        std::cerr << "warning: " << id.error().message << "\n         continuing read-only\n";
        id = pm.open (target, { .readOnly = true });
    }

    return id;
}

CommandContext cliContext (const std::string& project)
{
    CommandContext ctx;
    ctx.projectId = project;
    ctx.origin = Origin::cli();
    return ctx;
}

void printFindings (const json& list, const char* prefix)
{
    for (auto& f : list)
    {
        std::cout << "  " << prefix << f.value ("check", "?");

        if (f.contains ("name"))
            std::cout << " [" << f["name"].get<std::string>() << "]";

        std::cout << ": " << f.value ("message", "") << "\n";
    }
}
} // namespace

//==============================================================================
int runRender (const Options& o)
{
    EngineHost host (engineConfig (o, false));
    EventHub hub;
    ProjectManager pm (host, hub);

    auto id = openProject (pm, o.target);

    if (! id.ok())
        return fail (id.error().message);

    auto* project = pm.find (id.value());

    RenderRequest req;
    const fs::path outPath = o.out;
    auto ext = lower (outPath.extension().string());

    if (! ext.empty() && ext[0] == '.')
        ext.erase (0, 1);

    req.format.container = ! o.format.empty() ? o.format : (ext == "flac" || ext == "mp3" ? ext : "wav");
    req.name = ! o.renderName.empty() ? o.renderName : (! o.out.empty() ? outPath.stem().string() : "mix");
    req.format.bitDepth = o.bitDepth;
    req.format.bitrateKbps = o.bitrate;
    req.format.sampleRate = o.sampleRate;
    req.format.channels = o.channels;
    req.tailS = o.tailS;
    req.scope = o.scope;
    req.trackIds = o.trackIds;
    req.preflightOnly = o.preflightOnly;

    if (! o.seed.empty())
        req.seed = o.seed;

    if (o.startS >= 0)  req.range.startS = o.startS;
    if (o.endS >= 0)    req.range.endS = o.endS;

    if (! o.range.empty())
    {
        const auto parts = split (o.range, ':');

        if (parts.size() != 2)
            return fail ("--range must look like 2:40 (bars)");

        req.range.startBar = std::atoi (parts[0].c_str());
        req.range.endBar = std::atoi (parts[1].c_str());
    }

    if (o.targetLufs != 0.0)
    {
        req.loudness.targetLufs = o.targetLufs;
        req.loudness.toleranceLu = o.toleranceLu;
        req.loudness.truePeakCeilingDb = o.truePeakCeiling;
        req.loudness.mode = o.normalize ? "normalize" : (o.check ? "check" : "none");
    }

    if (o.limiter || o.noLimiter)
    {
        req.loudness.limiter = o.limiter && ! o.noLimiter;
        req.loudness.truePeakCeilingDb = o.truePeakCeiling;
    }

    if (! o.check)
    {
        // without --check the findings are reported but never stop the render
        req.warnOn.insert (req.failOn.begin(), req.failOn.end());
        req.failOn.clear();
    }

    auto result = renderProject (*project, req);
    json report;
    int exitCode = 0;

    if (result.ok())
    {
        report = result.value();

        if (! o.out.empty() && report.contains ("files"))
        {
            std::error_code ec;
            auto& files = report["files"];

            for (size_t i = 0; i < files.size(); ++i)
            {
                const auto from = files[i]["abs_path"].get<std::string>();
                auto to = files.size() == 1 ? outPath : outPath.parent_path() / fs::path (files[i]["path"].get<std::string>()).filename();
                fs::create_directories (to.parent_path().empty() ? fs::path (".") : to.parent_path(), ec);
                auto tmp = to;
                tmp += ".part";
                fs::copy_file (from, tmp, fs::copy_options::overwrite_existing, ec);

                if (! ec)
                    fs::rename (tmp, to, ec);

                if (ec)
                    return fail ("cannot write " + to.string() + ": " + ec.message());

                files[i]["path"] = to.string();
                files[i]["abs_path"] = fs::absolute (to).string();
            }

            report["file"] = files[0];
        }
    }
    else
    {
        report = { { "status", "failed" }, { "error", result.error().toJson() } };
        exitCode = result.error().code == errc::preflightFailed ? 2 : 1;
    }

    if (o.json)
    {
        std::cout << report.dump (2) << "\n";
        return exitCode;
    }

    if (! result.ok())
    {
        std::cout << "Render FAILED (" << result.error().code << "): " << result.error().message << "\n";

        if (result.error().details.contains ("details"))
            printFindings (result.error().details["details"], "FAIL ");

        if (result.error().details.contains ("preflight"))
            printFindings (result.error().details["preflight"]["warnings"], "warn ");

        return exitCode;
    }

    if (o.preflightOnly)
    {
        std::cout << "Preflight passed.\n";
        printFindings (report["preflight"]["warnings"], "warn ");
        return 0;
    }

    const auto& f = report["file"];
    const auto& s = report["stats"];
    std::cout << "Rendered " << f["path"].get<std::string>() << "  (" << f["container"].get<std::string>() << ", " << f["channels"] << " ch, "
              << (int) f["sample_rate"].get<double>() << " Hz, " << fmtDb (f["duration_s"].get<double>()) << " s, " << f["bytes"].get<std::int64_t>() / 1024 << " KiB)\n"
              << "  sha256 " << f["sha256"].get<std::string>() << "\n"
              << "  integrated " << fmtDb (s["integrated_lufs"]) << " LUFS | short-term max " << fmtDb (s["short_term_max_lufs"]) << " LUFS | true peak "
              << fmtDb (s["true_peak_dbtp"]) << " dBTP | peak " << fmtDb (s["sample_peak_dbfs"]) << " dBFS | rms " << fmtDb (s["rms_dbfs"])
              << " dBFS | clipped samples " << s["clipped_samples"] << "\n";

    if (s["silence_ranges"].size() > 0)
        std::cout << "  silent ranges: " << s["silence_ranges"].size() << "\n";

    if (report["loudness"].value ("mode", "none") != "none")
    {
        const auto& l = report["loudness"];
        std::cout << "  loudness " << l["mode"].get<std::string>() << ": target " << fmtDb (l["target_lufs"]) << " LUFS, before " << fmtDb (l["before_lufs"])
                  << ", after " << fmtDb (l["after_lufs"]) << " (gain " << fmtDb (l["gain_db"]) << " dB)"
                  << (l.value ("limited_by_true_peak", false) ? ", held back by the true-peak ceiling" : "") << "\n";

        if (l.contains ("limiter"))
            std::cout << "  limiter: ceiling " << fmtDb (l["limiter"]["ceiling_dbtp"]) << " dBTP, max gain reduction " << fmtDb (l["limiter"]["max_gain_reduction_db"])
                      << " dB, >1 dB for " << fmtDb (l["limiter"]["limited_over_1db_percent"]) << " % of the time, " << l["limiter"]["passes"] << " pass(es)\n";
    }

    for (auto& t : report["tracks"])
        std::cout << "  instrument track " << t.value ("name", "") << ": " << (t.value ("audible", false) ? "audible" : "SILENT") << "\n";

    std::cout << "  preflight " << (report["preflight"]["passed"].get<bool>() ? "passed" : "FAILED") << ", " << report["preflight"]["warnings"].size() << " warning(s)\n";
    printFindings (report["preflight"]["warnings"], "warn ");
    return exitCode;
}

//==============================================================================
int runImport (const Options& o)
{
    EngineHost host (engineConfig (o, false));
    EventHub hub;
    ProjectManager pm (host, hub);
    CommandRegistry registry;
    registerBuiltinCommands (registry);
    MessageThreadExecutor executor;
    CommandBus bus (pm, registry, executor);

    std::string projectId;

    if (! o.project.empty())
    {
        auto id = openProject (pm, o.project);

        if (! id.ok())
            return fail (id.error().message);

        projectId = id.value();
    }
    else
    {
        const fs::path first = o.paths.front();
        CreateOptions co;
        co.name = ! o.newName.empty() ? o.newName : (first.stem().empty() ? first.filename().string() : first.stem().string());
        auto id = pm.create (co);

        if (! id.ok())
            return fail (id.error().message);

        projectId = id.value();
    }

    const auto ctx = cliContext (projectId);
    json summary { { "project_id", projectId }, { "path", pm.find (projectId)->dir().string() }, { "name", pm.find (projectId)->name() } };
    const auto firstExt = lower (fs::path (o.paths.front()).extension().string());

    if (o.paths.size() == 1 && firstExt == ".rpp")
    {
        auto r = bus.runNow ("import.rpp", { { "path", fs::absolute (o.paths.front()).string() }, { "copy_media", ! o.noCopyMedia } }, ctx);

        if (! r.ok)
            return fail (r.error->message);

        summary["rpp"] = r.data;
    }
    else
    {
        json params { { "paths", json::array() }, { "preset", o.preset }, { "align", o.align }, { "takes_mode", o.takes } };

        for (auto& p : o.paths)
            params["paths"].push_back (fs::absolute (p).string());

        if (! o.rename.empty())
        {
            json names = json::object();

            for (auto& pair : split (o.rename, ','))
                if (const auto eq = pair.find ('='); eq != std::string::npos)
                    names[pair.substr (0, eq)] = pair.substr (eq + 1);

            params["name_map"] = names;
        }

        auto plan = bus.runNow ("import.plan", params, ctx);

        if (! plan.ok)
            return fail (plan.error->message);

        auto applied = bus.runNow ("import.apply", { { "plan_id", plan.data["plan_id"] } }, ctx);

        if (! applied.ok)
            return fail (applied.error->message);

        summary["detected_source"] = plan.data["detected_source"];
        summary["tracks"] = applied.data["tracks"];
        summary["warnings"] = plan.data["warnings"];
        summary["plan"] = plan.data["tracks"];
    }

    if (auto saved = pm.close (projectId, SaveMode::save); ! saved.ok())
        return fail (saved.error().message);

    if (o.json)
    {
        std::cout << summary.dump (2) << "\n";
        return 0;
    }

    std::cout << "Imported into project " << summary["name"].get<std::string>() << " (" << projectId << ")\n  " << summary["path"].get<std::string>() << "\n";

    if (summary.contains ("tracks"))
        for (size_t i = 0; i < summary["tracks"].size(); ++i)
        {
            const auto& plan = summary["plan"][i];
            std::cout << "  " << summary["tracks"][i]["name"].get<std::string>() << ": " << plan["clips"].size() << " clip(s), "
                      << fmtDb (plan["clips"][0]["duration_s"].get<double>()) << " s, " << plan["clips"][0]["channels"] << " ch\n";
        }

    if (summary.contains ("rpp"))
    {
        const auto& r = summary["rpp"];
        std::cout << "  tracks " << r["imported"]["tracks"] << ", items " << r["imported"]["items"] << ", midi items " << r["imported"]["midi_items"]
                  << ", markers " << r["imported"]["markers"] << ", regions " << r["imported"]["regions"] << ", sends " << r["imported"]["sends"] << "\n"
                  << "  unsupported: " << r["unsupported"].size() << ", missing media: " << r["missing_media"].size() << "\n";

        for (auto& m : r["missing_media"])
            std::cout << "  MISSING " << m["path"].get<std::string>() << "\n";
    }

    for (auto& w : summary.value ("warnings", json::array()))
        std::cout << "  warning " << w["code"].get<std::string>() << ": " << w["message"].get<std::string>()
                  << (w.contains ("file") ? " (" + w["file"].get<std::string>() + ")" : std::string()) << "\n";

    return 0;
}

//==============================================================================
namespace
{
std::string instanceOwnerId()
{
    char host[256] = {};
    ::gethostname (host, sizeof (host) - 1);
    std::random_device rd;
    static const char hex[] = "0123456789abcdef";
    std::string suffix;

    for (int i = 0; i < 6; ++i)
        suffix.push_back (hex[rd() % 16]);

    return std::string (host[0] != 0 ? host : "rovox") + ":" + std::to_string ((long) ::getpid()) + ":" + suffix;
}

std::shared_ptr<RemoteStorage> makeRemoteStorage (const StorageConfig& cfg)
{
    auto store = makeS3ObjectStore (cfg.store);
    DrumKitLibrary::instance().setObjectStore (store, cfg.prefix);      // drum kits live under <prefix>assets/drumkits/
    return std::make_shared<RemoteStorage> (store, StorageKeys { cfg.prefix }, store->describe(), instanceOwnerId(), cfg.leaseTtlS);
}

/** The root of `--storage fs`: ROVOX_STORAGE_FS_ROOT, else `[storage] fs_root` of the config file. Empty if neither is set. */
std::string fsStorageRoot (const Options& o)
{
    if (const char* env = std::getenv ("ROVOX_STORAGE_FS_ROOT"); env != nullptr && env[0] != 0)
        return env;

    const char* envConfig = std::getenv ("ROVOX_CONFIG");
    const std::string file = ! o.config.empty() ? o.config : (envConfig != nullptr ? std::string (envConfig) : std::string());

    if (file.empty())
        return {};

    try
    {
        return toml::parse_file (file)["storage"]["fs_root"].value_or (std::string());
    }
    catch (const toml::parse_error&)
    {
        return {};
    }
}

/** Storage from --storage / the S3_* environment. Local storage gives a null pointer (and no error). The bucket is listed once so that wrong
    credentials stop the server at start-up instead of showing up as a failed save later; an unreachable endpoint only warns. */
Result<std::shared_ptr<RemoteStorage>> setupStorage (const Options& o, ProjectManager* pm)
{
    std::shared_ptr<RemoteStorage> storage;
    std::string where;

    if (const char* env = std::getenv ("ROVOX_STORAGE"); lower (o.storage.empty() && env != nullptr ? env : o.storage) == "fs")
    {
        const auto root = fsStorageRoot (o);

        if (root.empty())
            return makeError (errc::preconditionFailed, "storage is fs but no directory is set: set ROVOX_STORAGE_FS_ROOT or [storage] fs_root in the config file");

        auto store = makeFsObjectStore (root);
        DrumKitLibrary::instance().setObjectStore (store, "rovox/");
        storage = std::make_shared<RemoteStorage> (store, StorageKeys { "rovox/" }, store->describe(), instanceOwnerId(), 60);
        where = "directory " + root;
    }
    else
    {
        auto cfg = StorageConfig::fromEnvironment (o.storage);

        if (! cfg.ok())
            return cfg.error();

        if (! cfg.value().s3)
            return std::shared_ptr<RemoteStorage>();

        storage = makeRemoteStorage (cfg.value());
        where = "s3 bucket " + cfg.value().store.bucket + " at " + cfg.value().store.endpoint;
    }

    if (auto r = storage->refreshHeads(); ! r.ok())
    {
        if (r.error().code == errc::preconditionFailed || r.error().code == errc::invalidParams)
            return r.error();

        std::cerr << "warning: object storage not reachable yet: " << r.error().message << "\n";
    }

    storage->start();

    if (pm != nullptr)
        pm->setStorage (storage);

    std::cout << "storage: " << where << " (source of truth; the data directory is a working cache)" << std::endl;
    return storage;
}

/** Flushes the upload queue (bounded) and releases the writer leases. */
void shutdownStorage (const std::shared_ptr<RemoteStorage>& storage)
{
    if (storage == nullptr)
        return;

    std::cout << "flushing uploads to the object store" << std::endl;

    if (! storage->shutdown (std::chrono::seconds (30)))
        std::cerr << "warning: some snapshots were not uploaded within 30 s; they stay in the data directory (autosave/) and go up with the next start or `rovox storage push`\n";
}
} // namespace

//==============================================================================
namespace
{
/** One flock per data directory: a second `serve` on the same /data refuses with an explanation. */
int lockDataDir (const fs::path& dataDir, std::string& holderPid)
{
    std::error_code ec;
    fs::create_directories (dataDir, ec);
    const int fd = ::open ((dataDir / "serve.lock").c_str(), O_CREAT | O_RDWR, 0644);

    if (fd < 0)
        return -1;

    if (::flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
        char buf[32] = {};
        [[maybe_unused]] auto n = ::read (fd, buf, sizeof (buf) - 1);
        holderPid = buf;
        ::close (fd);
        return -2;
    }

    const auto pid = std::to_string ((long) ::getpid());
    [[maybe_unused]] auto r1 = ::ftruncate (fd, 0);
    [[maybe_unused]] auto r2 = ::write (fd, pid.c_str(), pid.size());
    return fd;
}

/** Jobs + MCP on top of the command layer; shared by `serve` and `mcp`. */
struct McpStack
{
    McpStack (EngineHost& host, ProjectManager& pm, CommandBus& bus, EventHub& hub, MessageThreadExecutor& executor, mcp::McpConfig cfg)
        : jobs (host.dataDir() / "jobs", pools (host), [&hub] (const JobInfo& i)
          {
              auto summary = i.toJson();
              summary.erase ("result");
              hub.publish (i.tenant, i.projectId, { { "type", "job.updated" }, { "job", summary } });
          }),
          service (pm, jobs, executor),
          server (pm, bus, hub, jobs, service, std::move (cfg))
    {}

    ~McpStack() { jobs.shutdown(); }        // before the server goes: running jobs publish events

    static JobPools pools (EngineHost& host)
    {
        JobPools p;
        p.render = host.cpuBudget().cpus >= 2 ? 2 : 1;
        return p;
    }

    JobManager jobs;
    JobService service;
    mcp::McpServer server;
};

mcp::McpConfig mcpConfig (const Options& o)
{
    mcp::McpConfig c;
    c.underscoreNames = o.toolNames == "underscore";
    c.reaperCompat = o.reaperCompat || std::getenv ("ROVOX_REAPER_COMPAT") != nullptr;
    c.allowedOrigins = o.allowOrigins;
    c.configFile = o.config;

    if (! o.sourceUrl.empty())
        c.sourceUrl = o.sourceUrl;
    else if (const char* env = std::getenv ("ROVOX_SOURCE_URL"))
        c.sourceUrl = env;

    return c;
}

std::string newToken()
{
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::string t;

    for (int i = 0; i < 32; ++i)
        t.push_back (hex[rd() % 16]);

    return t;
}

/** --web-dir, $ROVOX_WEB_DIR, ../share/rovox/web next to the executable, /usr/share/rovox/web; the first that has an index.html. */
fs::path resolveWebDir (const Options& o)
{
    std::vector<fs::path> candidates;

    if (! o.webDir.empty())
        candidates.emplace_back (o.webDir);

    if (const char* env = std::getenv ("ROVOX_WEB_DIR"))
        candidates.emplace_back (env);

    candidates.push_back (toPath (juce::File::getSpecialLocation (juce::File::currentExecutableFile)).parent_path().parent_path() / "share" / "rovox" / "web");
    candidates.emplace_back ("/usr/share/rovox/web");

    for (auto& c : candidates)
        if (fs::exists (c / "index.html"))
            return c;

    return {};
}

class ServeTimer : public juce::Timer
{
public:
    /** `onStop`: runs once on its own thread after SIGTERM/SIGINT (the message loop keeps running meanwhile: jobs may need it), then the loop is stopped. */
    ServeTimer (ProjectManager& p, UploadStore* u, mcp::McpServer* m = nullptr, std::function<void()> onStop = nullptr)
        : projects (p), uploads (u), mcpServer (m), stopHook (std::move (onStop)) {}

    ~ServeTimer() override { joinStop(); }

    void joinStop()
    {
        if (stopThread.joinable())
            stopThread.join();
    }

    void timerCallback() override
    {
        if (stopRequested)
        {
            stopTimer();

            if (! stopHook)
            {
                juce::MessageManager::getInstance()->stopDispatchLoop();
                return;
            }

            stopThread = std::thread ([this]
            {
                stopHook();
                juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
            });
            return;
        }

        ticks++;

        if (ticks % 25 == 0)                // 5 s
        {
            projects.sweepExpired();
            if (uploads != nullptr)
                uploads->cleanup (std::time (nullptr));

            if (mcpServer != nullptr)
                mcpServer->sweepSessions();
        }

        if (ticks % 300 == 0)               // 60 s
            projects.autosaveDirty();
    }

    ProjectManager& projects;
    UploadStore* uploads;
    mcp::McpServer* mcpServer;
    std::function<void()> stopHook;
    std::thread stopThread;
    int ticks = 0;
};

/** Gives running jobs `o.drainTimeoutS` to finish; the rest end `interrupted`. Dirty projects are autosaved afterwards by closeAll (to <project>/autosave/, never over the project files). */
void drainJobs (JobManager& jobs, const Options& o, std::ostream& log)
{
    const auto timeout = std::chrono::milliseconds (std::llround (std::max (0.0, o.drainTimeoutS) * 1000.0));
    log << "rovox draining: running jobs have " << o.drainTimeoutS << " s to finish" << std::endl;
    const int cut = jobs.drain (timeout);

    if (cut > 0)
        log << "rovox drain: " << cut << " job" << (cut == 1 ? "" : "s") << " interrupted" << std::endl;
}
} // namespace

namespace
{
/** Paths the API and MCP may touch: the data dir plus --allow-path / $ROVOX_ALLOWED_PATHS. */
void restrictToAllowedPaths (EngineConfig& cfg, const Options& o)
{
    cfg.restrictPaths = true;

    for (auto& p : o.allowPaths)
        cfg.allowedRoots.emplace_back (p);

    if (const char* env = std::getenv ("ROVOX_ALLOWED_PATHS"))
        for (auto& p : split (env, ':'))
            cfg.allowedRoots.emplace_back (p);
}

/** --allow-host plus $ROVOX_ALLOWED_HOSTS (comma separated). */
std::vector<std::string> allowedHosts (const Options& o)
{
    auto hosts = o.allowHosts;

    if (const char* env = std::getenv ("ROVOX_ALLOWED_HOSTS"))
        for (auto& h : split (env, ','))
            if (! h.empty())
                hosts.push_back (h);

    return hosts;
}

/** The configuration file: --config, else $ROVOX_CONFIG, else <data>/config/rovox.toml. */
fs::path configFileFor (const Options& o, const fs::path& dataDir)
{
    const char* envConfig = std::getenv ("ROVOX_CONFIG");
    return ! o.config.empty() ? fs::path (o.config) : (envConfig != nullptr ? fs::path (envConfig) : dataDir / "config" / "rovox.toml");
}

/** What a client presents to a server: --token, else $ROVOX_KEY, else $ROVOX_TOKEN. */
std::string clientToken (const Options& o)
{
    if (! o.token.empty())
        return o.token;

    for (const char* name : { "ROVOX_KEY", "ROVOX_TOKEN" })
        if (const char* env = std::getenv (name); env != nullptr && env[0] != 0)
            return env;

    return {};
}
} // namespace

int runServe (const Options& o)
{
    auto cfg = engineConfig (o, true);
    std::string holder;
    const int lockFd = lockDataDir (cfg.dataDir, holder);

    if (lockFd == -2)
        return fail ("another rovox serve is already using " + cfg.dataDir.string() + " (pid " + holder + "). Stop it, or use a different --data-dir.");

    if (lockFd < 0)
        return fail ("cannot use data directory " + cfg.dataDir.string() + " (is it writable?)");

    restrictToAllowedPaths (cfg, o);

    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT, onSignal);

    {
        std::error_code ec;
        fs::remove_all (cfg.dataDir / "cache" / "render-child", ec);       // project copies of renders that a crash left behind
    }

    EngineHost host (cfg);
    EventHub hub;
    ProjectManager pm (host, hub);
    CommandRegistry registry;
    registerBuiltinCommands (registry);
    MessageThreadExecutor executor;
    CommandBus bus (pm, registry, executor);

    // event webhooks of [tenants.<id>.webhooks]: declared before the MCP stack, whose jobs publish events until they are shut down
    const auto configFile = configFileFor (o, cfg.dataDir);
    quotas().configure (cfg.dataDir, configFile);
    KeyStore keys (cfg.dataDir / "auth" / "keys.json");
    EventWebhooks webhooks (hub, { .configFile = configFile,
                                   .tenantRoot = [&pm] (const std::string& tenant) { return pm.projectsRoot (tenant); },
                                   .hostRoot = cfg.dataDir });

    auto storage = setupStorage (o, &pm);

    if (! storage.ok())
        return fail (storage.error().message);

    ServerOptions so;
    so.bind = o.bind;
    so.port = o.port;
    so.token = ! o.token.empty() ? o.token : (std::getenv ("ROVOX_TOKEN") != nullptr ? std::getenv ("ROVOX_TOKEN") : "");

    const bool loopback = guard::isLoopbackBind (o.bind);
    bool generated = false;

    if (! loopback && so.token.empty() && (o.generateToken || std::getenv ("ROVOX_GENERATE_TOKEN") != nullptr))
    {
        so.token = newToken();
        generated = true;
    }

    if (! loopback && so.token.empty() && ! keys.inUse())
        return fail ("--bind " + o.bind + " exposes the server beyond this machine: set --token (or ROVOX_TOKEN) or create an API key. Refusing to start without it.");

    Authenticator authenticator (&keys, so.token, configFile);
    authenticator.setSessionKeyFile (cfg.dataDir / "auth" / "session.key");
    authenticator.setRequireCredentials (! loopback);   // keys.json removed later must not turn an exposed server into an open one
    so.authenticator = &authenticator;
    so.sessionKeyFile = cfg.dataDir / "auth" / "session.key";

    // live numbers for /metrics; the objects below are created after the server, so they are reached through pointers set once they exist
    std::atomic<JobManager*> metricsJobs { nullptr };
    std::atomic<Gateway*> metricsGateway { nullptr };
    so.metrics.wsClients = [&metricsGateway] { auto* g = metricsGateway.load(); return g != nullptr ? g->clientCount() : 0; };
    so.metrics.jobsByState = [&metricsJobs]
    {
        std::map<std::string, int> counts;

        for (auto st : { JobState::queued, JobState::running, JobState::succeeded, JobState::failed, JobState::cancelled, JobState::interrupted })
            counts[toString (st)] = 0;

        if (auto* jobs = metricsJobs.load())
            for (auto& j : jobs->list())
                ++counts[toString (j.state)];

        return counts;
    };
    so.metrics.tenants = [&metricsJobs]
    {
        std::set<std::string> tenants { "local" };

        if (auto* jobs = metricsJobs.load())
            for (auto& j : jobs->list())
                tenants.insert (j.tenant);

        return std::vector<std::string> (tenants.begin(), tenants.end());
    };

    so.webDir = resolveWebDir (o);
    so.allowedOrigins = o.allowOrigins;
    so.allowedHosts = allowedHosts (o);

    if (! o.sourceUrl.empty())
        so.sourceUrl = o.sourceUrl;
    else if (const char* env = std::getenv ("ROVOX_SOURCE_URL"))
        so.sourceUrl = env;

    HttpServer server (host, pm, bus, hub, so);
    GatewayOptions gatewayOptions;
    gatewayOptions.token = so.token;
    gatewayOptions.authenticator = &authenticator;
    gatewayOptions.allowedOrigins = o.allowOrigins;
    gatewayOptions.allowedHosts = so.allowedHosts;
    gatewayOptions.loopbackBind = loopback;
    gatewayOptions.sourceUrl = so.sourceUrl;
    Gateway gateway (host, pm, bus, hub, executor, gatewayOptions);
    server.setGateway (&gateway);

    auto mcpCfg = mcpConfig (o);
    mcpCfg.keyStore = &keys;
    mcpCfg.uploadBatchStatus = [&server] (const std::string& id, const std::string& tenant) { return server.uploads().batchStatus (id, tenant); };
    mcpCfg.uploadFinish = [&server] (const std::string& id, const std::string& tenant) { return server.uploads().finish (id, tenant); };
    McpStack mcpStack (host, pm, bus, hub, executor, mcpCfg);
    metricsJobs = &mcpStack.jobs;
    metricsGateway = &gateway;
    server.mount ([&] (httplib::Server& svr) { mcp::installMcpRoutes (svr, mcpStack.server, { .authenticator = &authenticator, .allowedOrigins = o.allowOrigins, .allowedHosts = so.allowedHosts, .loopbackBind = loopback }); });

    if (auto started = server.start(); ! started.ok())
        return fail (started.error().message);

    std::cout << "rovox serving on http://" << o.bind << ":" << server.port() << "  (data " << cfg.dataDir.string() << ", audio " << toString (cfg.audio)
              << ", " << host.cpuBudget().cpus << " cpu" << (host.cpuBudget().cpus == 1 ? "" : "s") << (authenticator.open() ? "" : (so.token.empty() ? ", API key required" : ", token required")) << ")" << std::endl;

    if (generated)
        std::cout << "generated access token (set ROVOX_TOKEN to choose your own): " << so.token << "\n"
                  << "open http://<host>:" << server.port() << "/?token=" << so.token << std::endl;

    if (so.webDir.empty())
        std::cout << "web UI not found (build web/ and pass --web-dir); the API, MCP and /ws are available" << std::endl;

    // the plugin catalog is scanned in the background (the scan itself runs in a child process); plugin.catalog reports the scan state meanwhile
    std::atomic<bool> catalogScanDone { false };
    std::thread catalogScan ([&host, &catalogScanDone] { host.catalog().rescan(); catalogScanDone = true; });

    // SIGTERM: /readyz goes 503 first, then running jobs get the drain timeout; the loop stops only when that is over
    ServeTimer timer (pm, &server.uploads(), &mcpStack.server, [&]
    {
        server.setDraining (true);
        drainJobs (mcpStack.jobs, o, std::cout);
    });
    timer.startTimer (200);
    juce::MessageManager::getInstance()->runDispatchLoop();
    timer.stopTimer();
    timer.joinStop();

    std::cout << "rovox shutting down" << std::endl;
    host.catalog().cancelScan();            // a scan of many plugins must not hold up the shutdown ...

    // ... but lilv cannot be interrupted inside a bundle (ZynAddSubFX takes seconds on a busy machine), so the wait for the current bundle is bounded
    for (int i = 0; i < 20 && ! catalogScanDone; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (100));

    const bool scanStillRunning = ! catalogScanDone;

    if (! scanStillRunning)
        catalogScan.join();

    mcpStack.server.shutdown();
    gateway.shutdown();
    server.stop();
    pm.closeAll (SaveMode::autosave);       // dirty projects land in autosave/, ready for crash-style recovery on the next start
    shutdownStorage (storage.value());      // ... and, with object storage, in the bucket (bounded wait)
    ::close (lockFd);

    if (scanStillRunning)
    {
        // everything is saved: leave without tearing the engine down under the scan thread (the process manager's stop timeout is 10 s)
        mcpStack.jobs.shutdown();
        std::cout.flush();
        std::cerr.flush();
        ::_exit (0);
    }

    return 0;
}

//==============================================================================
namespace
{
/** `mcp --connect`: stdio to Streamable HTTP. Each stdin line is POSTed to /mcp; JSON and SSE replies come back as one JSON per stdout line
    (SSE events are forwarded as they arrive, so progress reaches the client during a call); a GET /mcp stream forwards the notifications of the
    server. Requests run concurrently (at most 8 in flight) so a long wait_s call does not hold up cancel, ping or other calls. A session the server
    no longer knows (restart, idle expiry) is replaced transparently: the client's initialize is replayed and the request retried once.
    The session is deleted at the end of input. Logs go to stderr. */
class McpProxy
{
public:
    explicit McpProxy (const Options& o)
        : url (o.connectUrl),
          token (clientToken (o))
    {}

    int run()
    {
        std::string line;

        while (std::getline (std::cin, line))
        {
            if (line.find_first_not_of (" \t\r") == std::string::npos)
                continue;

            auto request = json::parse (line, nullptr, false);
            const bool isRequest = request.is_object() && request.contains ("id") && request.contains ("method");
            const auto method = request.is_object() ? request.value ("method", "") : std::string();

            // a cancel for a call that is still running may overtake that call on the way to the server (the call is POSTed from its own thread):
            // it is repeated until the call is over, the server ignores a cancel for a request it does not know (or no longer waits for)
            if (method == "notifications/cancelled" && request.contains ("params") && request["params"].is_object() && request["params"].contains ("requestId"))
            {
                const auto target = request["params"]["requestId"].dump();
                bool running = false;

                {
                    std::lock_guard<std::mutex> lock (flightMutex);
                    running = activeIds.count (target) != 0;

                    if (running)
                        ++inFlight;
                }

                if (running)
                {
                    std::thread ([this, line, target]
                    {
                        for (;;)
                        {
                            handle (line);
                            std::unique_lock<std::mutex> lock (flightMutex);

                            if (flightCv.wait_for (lock, std::chrono::milliseconds (500), [&] { return activeIds.count (target) == 0; }))
                                break;
                        }

                        std::lock_guard<std::mutex> lock (flightMutex);
                        --inFlight;
                        flightCv.notify_all();
                    }).detach();
                    continue;
                }
            }

            // notifications, initialize and ping are quick and keep their order; calls go to a worker each
            if (! isRequest || method == "initialize" || method == "ping")
            {
                handle (line);
                continue;
            }

            const auto idKey = request["id"].dump();

            {
                std::unique_lock<std::mutex> lock (flightMutex);
                flightCv.wait (lock, [&] { return inFlight < maxInFlight; });
                ++inFlight;
                activeIds.insert (idKey);
            }

            std::thread ([this, line, idKey]
            {
                handle (line);
                std::lock_guard<std::mutex> lock (flightMutex);
                activeIds.erase (idKey);
                --inFlight;
                flightCv.notify_all();
            }).detach();
        }

        {
            std::unique_lock<std::mutex> lock (flightMutex);
            flightCv.wait (lock, [&] { return inFlight == 0; });
        }

        stopListenerThread();
        const auto session = currentSession();

        if (! session.empty())
            makeClient (10)->Delete ("/mcp", headers (session, false));

        return 0;
    }

private:
    struct Reply
    {
        std::string error, contentType, sessionId, body;
        int status = 0;
    };

    static constexpr int maxInFlight = 8;

    std::unique_ptr<httplib::Client> makeClient (int readTimeoutS) const
    {
        auto c = std::make_unique<httplib::Client> (url);
        c->set_connection_timeout (10, 0);
        c->set_read_timeout (readTimeoutS, 0);
        return c;
    }

    httplib::Headers headers (const std::string& session, bool sse) const
    {
        httplib::Headers h { { "Accept", sse ? "text/event-stream" : "application/json, text/event-stream" } };

        if (! token.empty())
            h.emplace ("Authorization", "Bearer " + token);

        if (! session.empty())
            h.emplace ("Mcp-Session-Id", session);

        return h;
    }

    std::string currentSession()
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        return sessionId;
    }

    void emit (const std::string& line)
    {
        std::lock_guard<std::mutex> lock (outMutex);
        std::cout << line << "\n";
        std::cout.flush();
    }

    void emitJson (const std::string& text)
    {
        auto j = json::parse (text, nullptr, false);

        if (! j.is_discarded())
            emit (j.dump());
        else
            std::cerr << "mcp proxy: ignoring a reply that is not JSON\n";
    }

    // data of every complete SSE event (lines starting "data:" joined by newlines), the rest stays in `pending`
    void drainSse (std::string& pending)
    {
        for (;;)
        {
            const auto end = pending.find ("\n\n");

            if (end == std::string::npos)
                return;

            std::string event = pending.substr (0, end), data;
            pending.erase (0, end + 2);
            std::istringstream lines (event);
            std::string line;

            while (std::getline (lines, line))
            {
                if (! line.empty() && line.back() == '\r')
                    line.pop_back();

                if (line.rfind ("data:", 0) == 0)
                    data += (data.empty() ? "" : "\n") + line.substr (line.size() > 5 && line[5] == ' ' ? 6 : 5);
            }

            if (! data.empty())
                emitJson (data);
        }
    }

    void feedSse (std::string& pending, const char* data, size_t n)
    {
        pending.append (data, n);
        std::string normalised;

        for (size_t i = 0; i < pending.size(); ++i)
            if (! (pending[i] == '\r' && i + 1 < pending.size() && pending[i + 1] == '\n'))
                normalised += pending[i];

        pending = std::move (normalised);
        drainSse (pending);
    }

    /** POSTs one message. An SSE reply is forwarded event by event while it arrives; anything else is returned in `body`. */
    Reply post (const std::string& line, const std::string& session)
    {
        Reply r;
        auto client = makeClient (600);
        httplib::Request req;
        req.method = "POST";
        req.path = "/mcp";
        req.headers = headers (session, false);
        req.set_header ("Content-Type", "application/json");
        req.body = line;
        bool sse = false;
        std::string pending;

        req.response_handler = [&] (const httplib::Response& res)
        {
            r.status = res.status;
            r.contentType = res.get_header_value ("Content-Type");
            r.sessionId = res.get_header_value ("Mcp-Session-Id");
            sse = res.status >= 200 && res.status < 300 && r.contentType.find ("text/event-stream") != std::string::npos;
            return true;
        };
        req.content_receiver = [&] (const char* data, size_t n, uint64_t, uint64_t)
        {
            if (sse)
                feedSse (pending, data, n);
            else
                r.body.append (data, n);

            return true;
        };

        auto res = client->send (req);

        if (! res)
        {
            r.error = httplib::to_string (res.error());
            return r;
        }

        if (sse)
        {
            pending += "\n\n";
            drainSse (pending);
        }

        return r;
    }

    /** Replaces a session the server no longer knows by replaying the client's initialize. True if a usable session exists afterwards. */
    bool reinitialize (const std::string& stale)
    {
        std::lock_guard<std::mutex> lock (reinitMutex);
        std::string initLine;

        {
            std::lock_guard<std::mutex> state (stateMutex);

            if (sessionId != stale)
                return true;                        // another request has already done it

            initLine = initializeLine;
        }

        if (initLine.empty())
            return false;

        auto r = post (initLine, "");

        if (! r.error.empty() || r.status != 200 || r.sessionId.empty())
        {
            std::cerr << "mcp proxy: cannot start a new session: " << (r.error.empty() ? "HTTP " + std::to_string (r.status) : r.error) << "\n";
            return false;
        }

        {
            std::lock_guard<std::mutex> state (stateMutex);
            sessionId = r.sessionId;
        }

        post (R"({"jsonrpc":"2.0","method":"notifications/initialized"})", r.sessionId);

        if (! stale.empty())
            makeClient (10)->Delete ("/mcp", headers (stale, false));

        std::cerr << "mcp proxy: the server dropped the session; started a new one\n";
        kickListener();
        return true;
    }

    void failRequest (const json& id, const std::string& message)
    {
        std::cerr << "mcp proxy: " << message << "\n";

        if (! id.is_null())
            emit (json { { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", -32603 }, { "message", message } } } }.dump());
    }

    void handle (const std::string& line)
    {
        auto request = json::parse (line, nullptr, false);
        const std::string method = request.is_object() ? request.value ("method", "") : std::string();
        const bool isInit = method == "initialize";
        const json id = request.is_object() && request.contains ("id") ? request["id"] : json (nullptr);

        Reply r;

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto session = isInit ? std::string() : currentSession();
            r = post (line, session);

            if (r.error.empty() && r.status == 404 && attempt == 0 && ! isInit && ! session.empty() && method != "notifications/cancelled" && reinitialize (session))
                continue;

            break;
        }

        if (! r.error.empty())
        {
            failRequest (id, "cannot reach " + url + ": " + r.error);
            return;
        }

        if (isInit && r.status == 200 && ! r.sessionId.empty())
        {
            std::string old;

            {
                std::lock_guard<std::mutex> state (stateMutex);
                old = sessionId;
                sessionId = r.sessionId;
                initializeLine = line;
            }

            if (! old.empty() && old != r.sessionId)
                makeClient (10)->Delete ("/mcp", headers (old, false));

            kickListener();
        }

        if (r.status == 202 || r.body.empty())
            return;

        if (r.status >= 200 && r.status < 300)
        {
            emitJson (r.body);
            return;
        }

        auto body = json::parse (r.body, nullptr, false);

        if (body.is_object() && body.contains ("error") && ! id.is_null())
        {
            body["id"] = id;
            emit (body.dump());
        }
        else
        {
            failRequest (id, "server answered HTTP " + std::to_string (r.status));
        }
    }

    //==========================================================================
    // GET /mcp stream: runs for as long as the proxy does, reconnecting with backoff and after a new session

    void kickListener()
    {
        {
            std::lock_guard<std::mutex> lock (listenMutex);

            if (! listener.joinable())
                listener = std::thread ([this] { listenLoop(); });
            else if (listenClient != nullptr)
                listenClient->stop();
        }

        listenCv.notify_all();
    }

    void stopListenerThread()
    {
        std::thread t;

        {
            std::lock_guard<std::mutex> lock (listenMutex);
            stopListener = true;

            if (listenClient != nullptr)
                listenClient->stop();

            t = std::move (listener);
        }

        listenCv.notify_all();

        if (t.joinable())
            t.join();
    }

    void listenLoop()
    {
        int backoffS = 1, lastLogged = 0;

        for (;;)
        {
            const auto session = currentSession();
            int status = 0;

            if (! session.empty())
            {
                {
                    std::lock_guard<std::mutex> lock (listenMutex);

                    if (stopListener)
                        return;

                    listenClient = makeClient (3600);
                }

                std::string pending;
                auto res = listenClient->Get ("/mcp", headers (session, true), [&] (const char* data, size_t n)
                {
                    feedSse (pending, data, n);
                    return ! stopListener.load();
                });

                {
                    std::lock_guard<std::mutex> lock (listenMutex);
                    listenClient.reset();
                }

                status = res != nullptr ? res->status : -1;

                if (status == 200)
                    backoffS = 1;
            }

            if (stopListener)
                return;

            if (currentSession() != session)
                continue;                           // a new session: reconnect at once

            if (status == 404)
            {
                reinitialize (session);
            }
            else if (status != 0 && status != 200 && status != lastLogged)
            {
                lastLogged = status;
                std::cerr << "mcp proxy: server notifications are not available" << (status > 0 ? " (HTTP " + std::to_string (status) + ")" : "") << "\n";
            }

            const auto wait = status == 200 || status == 0 ? std::chrono::seconds (0) + std::chrono::milliseconds (200) : std::chrono::milliseconds (backoffS * 1000);

            if (status != 200 && status != 0)
                backoffS = std::min (backoffS * 2, 30);

            std::unique_lock<std::mutex> lock (listenMutex);
            listenCv.wait_for (lock, wait, [&] { return stopListener.load(); });

            if (stopListener)
                return;
        }
    }

    const std::string url, token;
    std::mutex outMutex, stateMutex, reinitMutex, flightMutex, listenMutex;
    std::condition_variable flightCv, listenCv;
    int inFlight = 0;
    std::set<std::string> activeIds;                // JSON of the ids of the calls being served
    std::string sessionId, initializeLine;
    std::atomic<bool> stopListener { false };
    std::thread listener;
    std::unique_ptr<httplib::Client> listenClient;
};
} // namespace

int runMcpProxy (const Options& o)
{
    McpProxy proxy (o);
    return proxy.run();
}

//==============================================================================
int runMcp (const Options& o)
{
    if (! o.connectUrl.empty())
        return runMcpProxy (o);

    // stdout carries the protocol and nothing else; JUCE and Tracktion log to stderr
    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT, onSignal);

    auto cfg = engineConfig (o, true);
    std::string holder;
    const int lockFd = lockDataDir (cfg.dataDir, holder);

    if (lockFd == -2)
        return fail ("another rovox process (pid " + holder + ") is already using " + cfg.dataDir.string() + ". Stop it, use `rovox mcp --connect` to share it, or use a different --data-dir.");

    if (lockFd < 0)
        return fail ("cannot use data directory " + cfg.dataDir.string() + " (is it writable?)");

    restrictToAllowedPaths (cfg, o);

    // stdio is the local operator; `--tenant` is the operator's statement that this session works for that tenant (never a tool parameter)
    Principal principal = Principal::local();

    if (! o.tenant.empty())
    {
        if (! isValidTenantId (o.tenant))
            return fail ("--tenant must be 1-64 characters of a-z, 0-9, '_' or '-'");

        principal.tenant = o.tenant;
        principal.scopes = ScopeSet::all();
        principal.scopes.remove (Scope::admin);
        principal.authMode = "operator";
    }

    quotas().configure (cfg.dataDir, configFileFor (o, cfg.dataDir));

    EngineHost host (cfg);
    EventHub hub;
    ProjectManager pm (host, hub);
    CommandRegistry registry;
    registerBuiltinCommands (registry);
    MessageThreadExecutor executor;
    CommandBus bus (pm, registry, executor);
    auto storage = setupStorage (o, &pm);

    if (! storage.ok())
        return fail (storage.error().message);

    KeyStore keys (cfg.dataDir / "auth" / "keys.json");
    auto mcpCfg = mcpConfig (o);
    mcpCfg.keyStore = &keys;
    McpStack stack (host, pm, bus, hub, executor, mcpCfg);

    std::atomic<bool> readerDone { false };
    std::thread reader ([&]
    {
        mcp::StdioTransport (stack.server, std::cin, std::cout, principal).run();
        readerDone = true;
        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    });

    ServeTimer timer (pm, nullptr, &stack.server, [&] { drainJobs (stack.jobs, o, std::cerr); });     // stdout carries the protocol
    timer.startTimer (200);
    juce::MessageManager::getInstance()->runDispatchLoop();
    timer.stopTimer();
    timer.joinStop();

    stack.server.shutdown();

    if (readerDone)
        reader.join();
    else
        reader.detach();            // stopped by a signal while blocked on stdin

    stack.jobs.shutdown();
    pm.closeAll (SaveMode::autosave);       // dirty projects land in autosave/, as with `serve`
    shutdownStorage (storage.value());
    return 0;
}

//==============================================================================
// key management: the file is the interface, no running server is needed (and none is touched)
namespace
{
KeyStore openKeyStore (const Options& o)
{
    return KeyStore (resolveDataDir (o.dataDir) / "auth" / "keys.json");
}

/** The key goes to stdout and nowhere else; the human hint goes to stderr. */
int printCreated (const KeyStore::Created& c, const Options& o, const char* verb)
{
    if (o.json)
        std::cout << json { { "key", c.key }, { "record", c.record } }.dump (2) << std::endl;
    else
        std::cout << c.key << std::endl;

    std::cerr << verb << " " << c.record.value ("id", "") << " (tenant " << c.record.value ("tenant", "") << "). This is the only time the key is shown: store it now.\n";
    return 0;
}

std::string keyStatus (const json& r, std::int64_t now)
{
    if (r.contains ("revoked_at"))
        return "revoked";

    if (r.contains ("expires_at") && now >= r["expires_at"].get<std::int64_t>())
        return "expired";

    return "active";
}

std::string joinNames (const json& names)
{
    std::string out;

    for (auto& n : names)
        out += (out.empty() ? "" : ",") + n.get<std::string>();

    return out;
}
} // namespace

int runKeyCreate (const Options& o)
{
    std::vector<std::string> names;
    std::stringstream ss (o.keyScopes);

    for (std::string item; std::getline (ss, item, ',');)
        if (! item.empty())
            names.push_back (item);

    auto scopes = ScopeSet::parse (names);

    if (! scopes.ok())
        return fail (scopes.error().message);

    auto keys = openKeyStore (o);
    KeyStore::CreateSpec spec;
    spec.tenant = o.tenant;
    spec.scopes = scopes.value();
    spec.label = o.keyLabel;
    spec.quotaProfile = o.keyQuotaProfile;

    if (! o.keyExpires.empty())
    {
        auto expiry = parseExpiry (o.keyExpires, keys.now());

        if (! expiry.ok())
            return fail (expiry.error().message);

        spec.expiresAt = expiry.value();
    }

    auto created = keys.create (spec, "cli");

    if (! created.ok())
        return fail (created.error().message);

    return printCreated (created.value(), o, "created key");
}

int runKeyList (const Options& o)
{
    auto keys = openKeyStore (o);
    json out = json::array();

    for (auto& r : keys.list())
        if (o.tenant.empty() || r.value ("tenant", "") == o.tenant)
            out.push_back (r);

    if (o.json)
    {
        std::cout << out.dump (2) << std::endl;
        return 0;
    }

    const auto now = keys.now();

    for (auto& r : out)
        std::cout << r.value ("id", "") << "  " << r.value ("tenant", "") << "  " << joinNames (r["scopes"]) << "  " << keyStatus (r, now) << "  " << r.value ("label", "") << "\n";

    return 0;
}

int runKeyRevoke (const Options& o)
{
    auto keys = openKeyStore (o);
    auto r = keys.revoke (o.keyId, "cli");

    if (! r.ok())
        return fail (r.error().message);

    std::cout << "revoked " << o.keyId << std::endl;
    return 0;
}

int runKeyRotate (const Options& o)
{
    auto keys = openKeyStore (o);
    auto r = keys.rotate (o.keyId, o.keyGraceS, "cli");

    if (! r.ok())
        return fail (r.error().message);

    return printCreated (r.value(), o, "rotated: new key");
}

//==============================================================================
int runUpload (const Options& o)
{
    httplib::Client client (o.server);
    client.set_connection_timeout (10, 0);
    client.set_read_timeout (120, 0);
    const std::string token = clientToken (o);
    httplib::Headers auth;

    if (! token.empty())
        auth.emplace ("Authorization", "Bearer " + token);

    struct Item { fs::path file; std::string rel; std::uint64_t size; };
    std::vector<Item> items;

    for (auto& p : o.paths)
    {
        const fs::path path = p;
        std::error_code ec;

        if (fs::is_directory (path, ec))
        {
            for (fs::recursive_directory_iterator it (path, ec), end; it != end; it.increment (ec))
                if (it->is_regular_file())
                    items.push_back ({ it->path(), (path.filename() / fs::relative (it->path(), path)).generic_string(), (std::uint64_t) it->file_size() });
        }
        else if (fs::is_regular_file (path, ec))
        {
            items.push_back ({ path, path.filename().string(), (std::uint64_t) fs::file_size (path) });
        }
        else
        {
            return fail ("no such file or folder: " + p);
        }
    }

    json files = json::array();

    for (auto& i : items)
        files.push_back ({ { "relpath", i.rel }, { "size", i.size }, { "sha256", sha256File (i.file).value_or ("") } });

    auto created = client.Post ("/api/upload-batches", auth, json { { "files", files } }.dump(), "application/json");

    if (! created || created->status != 201)
        return fail (created ? "server refused the batch: " + created->body : "cannot reach " + o.server);

    const auto batch = json::parse (created->body);
    const size_t chunk = (size_t) std::max (1, o.chunkMb) << 20;

    for (size_t n = 0; n < items.size(); ++n)
    {
        const auto url = batch["files"][n]["url"].get<std::string>();
        auto head = client.Get (url, auth);      // HEAD is served by the GET route in httplib: same headers, no body

        std::uint64_t offset = 0;

        if (head && head->status == 200)
            offset = std::stoull (head->get_header_value ("Upload-Offset"));

        std::ifstream in (items[n].file, std::ios::binary);
        in.seekg ((std::streamoff) offset);
        std::string buf;

        while (offset < items[n].size)
        {
            buf.resize ((size_t) std::min<std::uint64_t> (chunk, items[n].size - offset));
            in.read (buf.data(), (std::streamsize) buf.size());
            Sha1 h;
            h.update (buf.data(), buf.size());
            const auto d = h.digest();

            auto headers = auth;
            headers.emplace ("Tus-Resumable", "1.0.0");
            headers.emplace ("Upload-Offset", std::to_string (offset));
            headers.emplace ("Upload-Checksum", "sha1 " + base64Encode (d.data(), d.size()));
            auto r = client.Patch (url, headers, buf, "application/offset+octet-stream");

            if (! r || r->status != 204)
                return fail ("upload of " + items[n].rel + " failed at offset " + std::to_string (offset) + (r ? " (HTTP " + std::to_string (r->status) + ")" : " (connection lost; run the same command again to resume)"));

            offset += buf.size();
            std::cerr << "\r" << items[n].rel << ": " << offset * 100 / std::max<std::uint64_t> (1, items[n].size) << "%   " << std::flush;
        }

        std::cerr << "\r" << items[n].rel << ": done        \n";
    }

    std::cout << json { { "batch_id", batch["batch_id"] }, { "files", batch["files"] } }.dump (2) << "\n";
    return 0;
}

int runStoragePush (const Options& o)
{
    auto cfg = StorageConfig::fromEnvironment (o.storage.empty() ? "s3" : o.storage);

    if (! cfg.ok())
        return fail (cfg.error().message);

    if (! cfg.value().s3)
        return fail ("storage push needs object storage: set S3_ENDPOINT, S3_BUCKET, S3_ACCESS_KEY and S3_SECRET_KEY");

    auto storage = makeRemoteStorage (cfg.value());
    const auto dataDir = resolveDataDir (o.dataDir);
    std::vector<std::pair<std::string, fs::path>> roots { { "local", dataDir / "projects" } };
    std::error_code ec;

    for (auto& t : fs::directory_iterator (dataDir / "tenants", ec))
        if (t.is_directory())
            roots.push_back ({ t.path().filename().string(), t.path() / "projects" });

    json pushed = json::array(), skipped = json::array(), failed = json::array();
    int renders = 0;

    for (auto& [tenant, root] : roots)
        for (auto& entry : fs::directory_iterator (root, ec))
        {
            const auto dir = entry.path() / "project.rvx";

            if (! fs::exists (dir / manifestFileName, ec))
                continue;

            ProjectHead head;
            const auto summary = summarizeEdit (dir / editFileName);
            head.tracks = summary.tracks;
            head.durationS = summary.durationS;
            auto job = readSnapshot (dir, dir, head);

            if (! job.ok())
            {
                failed.push_back ({ { "project", entry.path().filename().string() }, { "error", job.error().message } });
                continue;
            }

            auto& snap = job.value();

            if (snap.tenant != tenant)
            {
                failed.push_back ({ { "project_id", snap.projectId }, { "error", "the manifest says tenant " + snap.tenant + " but the project lives under " + tenant } });
                continue;
            }

            const json item { { "project_id", snap.projectId }, { "name", snap.head.name }, { "tenant_id", tenant }, { "revision", snap.revision } };
            auto remote = storage->fetchHead (tenant, snap.projectId);

            if (remote.ok() && remote.value().revision >= snap.revision)
            {
                skipped.push_back (item);
            }
            else if (auto r = storage->push (snap); r.ok())
            {
                pushed.push_back (item);
            }
            else
            {
                auto f = item;
                f["error"] = r.error().message;
                failed.push_back (f);
                continue;
            }

            auto up = storage->pushRenders (tenant, snap.projectId, dir);

            if (up.ok())
                renders += up.value();
            else
                failed.push_back ({ { "project_id", snap.projectId }, { "error", up.error().message } });
        }

    const json report { { "pushed", pushed }, { "up_to_date", skipped }, { "failed", failed }, { "renders_uploaded", renders } };
    std::cout << report.dump (2) << "\n";
    return failed.empty() ? 0 : 1;
}

int runPluginProbe (const Options& o)
{
    return runPluginProbeChild (o.probeFile, o.presetsOnly);
}

int runRenderChildCommand (const Options& o)
{
    return runRenderChild (engineConfig (o, false), o.snapshotDir, o.requestFile);
}

int runPlugins (const Options& o)
{
    EngineHost host (engineConfig (o, true));
    std::cout << scanPlugins (host, { "LV2", "VST3" }, o.pluginsDir).dump (2) << "\n";
    return 0;
}

} // namespace rovox::cli
