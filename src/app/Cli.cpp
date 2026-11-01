// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Cli.h"

#include "CommandBus.h"
#include "EngineHost.h"
#include "HttpServer.h"
#include "Import.h"
#include "JobManager.h"
#include "McpHttp.h"
#include "McpStdio.h"
#include "PluginScan.h"
#include "ProjectManager.h"
#include "Render.h"
#include "RenderJobs.h"
#include "Sha1.h"
#include "Sha256.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
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

    if (! o.sourceUrl.empty())
        c.sourceUrl = o.sourceUrl;
    else if (const char* env = std::getenv ("ROVOX_SOURCE_URL"))
        c.sourceUrl = env;

    return c;
}

class ServeTimer : public juce::Timer
{
public:
    ServeTimer (ProjectManager& p, UploadStore* u, mcp::McpServer* m = nullptr) : projects (p), uploads (u), mcpServer (m) {}

    void timerCallback() override
    {
        if (stopRequested)
        {
            juce::MessageManager::getInstance()->stopDispatchLoop();
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
    int ticks = 0;
};
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

    if (cfg.audio == AudioMode::BrowserStream)
        return fail ("--audio browser is not part of milestone M1");

    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT, onSignal);

    EngineHost host (cfg);
    EventHub hub;
    ProjectManager pm (host, hub);
    CommandRegistry registry;
    registerBuiltinCommands (registry);
    MessageThreadExecutor executor;
    CommandBus bus (pm, registry, executor);

    ServerOptions so;
    so.bind = o.bind;
    so.port = o.port;
    so.token = ! o.token.empty() ? o.token : (std::getenv ("ROVOX_TOKEN") != nullptr ? std::getenv ("ROVOX_TOKEN") : "");

    const bool loopback = o.bind == "127.0.0.1" || o.bind == "localhost" || o.bind == "::1";

    if (! loopback && so.token.empty())
        return fail ("--bind " + o.bind + " exposes the server beyond this machine: set --token (or ROVOX_TOKEN). Refusing to start without it.");

    HttpServer server (host, pm, bus, hub, so);

    auto mcpCfg = mcpConfig (o);
    mcpCfg.uploadBatchStatus = [&server] (const std::string& id) { return server.uploads().batchStatus (id); };
    McpStack mcpStack (host, pm, bus, hub, executor, mcpCfg);
    server.mount ([&] (httplib::Server& svr) { mcp::installMcpRoutes (svr, mcpStack.server, { .allowedOrigins = o.allowOrigins, .loopbackBind = loopback }); });

    if (auto started = server.start(); ! started.ok())
        return fail (started.error().message);

    std::cout << "rovox serving on http://" << o.bind << ":" << server.port() << "  (data " << cfg.dataDir.string() << ", audio " << toString (cfg.audio)
              << ", " << host.cpuBudget().cpus << " cpu" << (host.cpuBudget().cpus == 1 ? "" : "s") << (so.token.empty() ? "" : ", token required") << ")" << std::endl;

    ServeTimer timer (pm, &server.uploads(), &mcpStack.server);
    timer.startTimer (200);
    juce::MessageManager::getInstance()->runDispatchLoop();
    timer.stopTimer();

    std::cout << "rovox shutting down" << std::endl;
    mcpStack.server.shutdown();
    server.stop();
    pm.closeAll (SaveMode::autosave);       // dirty projects land in autosave/, ready for crash-style recovery on the next start
    ::close (lockFd);
    return 0;
}

//==============================================================================
int runMcp (const Options& o)
{
    // stdout carries the protocol and nothing else; JUCE and Tracktion log to stderr
    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT, onSignal);

    EngineHost host (engineConfig (o, true));
    EventHub hub;
    ProjectManager pm (host, hub);
    CommandRegistry registry;
    registerBuiltinCommands (registry);
    MessageThreadExecutor executor;
    CommandBus bus (pm, registry, executor);
    McpStack stack (host, pm, bus, hub, executor, mcpConfig (o));

    std::atomic<bool> readerDone { false };
    std::thread reader ([&]
    {
        mcp::StdioTransport (stack.server, std::cin, std::cout).run();
        readerDone = true;
        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    });

    ServeTimer timer (pm, nullptr, &stack.server);
    timer.startTimer (200);
    juce::MessageManager::getInstance()->runDispatchLoop();
    timer.stopTimer();

    stack.server.shutdown();

    if (readerDone)
        reader.join();
    else
        reader.detach();            // stopped by a signal while blocked on stdin

    stack.jobs.shutdown();
    pm.closeAll (SaveMode::autosave);       // dirty projects land in autosave/, as with `serve`
    return 0;
}

//==============================================================================
int runUpload (const Options& o)
{
    httplib::Client client (o.server);
    client.set_connection_timeout (10, 0);
    client.set_read_timeout (120, 0);
    const std::string token = ! o.token.empty() ? o.token : (std::getenv ("ROVOX_TOKEN") != nullptr ? std::getenv ("ROVOX_TOKEN") : "");
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

int runPlugins (const Options& o)
{
    EngineHost host (engineConfig (o, true));
    std::cout << scanPlugins (host, { "LV2", "VST3" }, o.pluginsDir).dump (2) << "\n";
    return 0;
}

} // namespace rovox::cli
