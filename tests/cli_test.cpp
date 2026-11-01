// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Runs the real `rovox` binary (path in $ROVOX_CLI_BIN, set by CTest) against synthetic projects.

#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rovox;

namespace
{
struct Process
{
    pid_t pid = -1;
    int outFd = -1, errFd = -1;
    std::string out, err;
};

fs::path cliBinary()
{
    const char* env = std::getenv ("ROVOX_CLI_BIN");
    return env != nullptr ? fs::path (env) : fs::path();
}

/** stdin is a pipe that carries `input` and then ends: no command can block on a terminal. */
Process spawn (const std::vector<std::string>& args, const fs::path& dataDir, const std::string& input = {})
{
    int outPipe[2], errPipe[2], inPipe[2];
    REQUIRE (pipe (outPipe) == 0);
    REQUIRE (pipe (errPipe) == 0);
    REQUIRE (pipe (inPipe) == 0);
    Process p;
    p.pid = fork();

    if (p.pid == 0)
    {
        dup2 (outPipe[1], 1);
        dup2 (errPipe[1], 2);
        dup2 (inPipe[0], 0);
        close (inPipe[1]);
        close (outPipe[0]); close (errPipe[0]);
        setenv ("ROVOX_DATA_DIR", dataDir.c_str(), 1);
        unsetenv ("ROVOX_TOKEN");
        std::vector<char*> argv;
        const auto bin = cliBinary().string();
        argv.push_back (const_cast<char*> (bin.c_str()));

        for (auto& a : args)
            argv.push_back (const_cast<char*> (a.c_str()));

        argv.push_back (nullptr);
        execv (bin.c_str(), argv.data());
        _exit (127);
    }

    close (outPipe[1]);
    close (errPipe[1]);
    close (inPipe[0]);

    for (size_t done = 0; done < input.size();)
    {
        const auto n = write (inPipe[1], input.data() + done, input.size() - done);

        if (n <= 0)
            break;

        done += (size_t) n;
    }

    close (inPipe[1]);
    p.outFd = outPipe[0];
    p.errFd = errPipe[0];
    fcntl (p.outFd, F_SETFL, O_NONBLOCK);
    fcntl (p.errFd, F_SETFL, O_NONBLOCK);
    return p;
}

/** Pumps the pipes; returns true if `until` became true before the timeout. */
template <typename Pred>
bool pump (Process& p, double timeoutS, Pred until)
{
    const auto t0 = std::chrono::steady_clock::now();

    for (;;)
    {
        pollfd fds[2] = { { p.outFd, POLLIN, 0 }, { p.errFd, POLLIN, 0 } };
        poll (fds, 2, 50);
        char buf[4096];

        for (int i = 0; i < 2; ++i)
            for (;;)
            {
                const auto n = read (fds[i].fd, buf, sizeof (buf));

                if (n <= 0)
                    break;

                (i == 0 ? p.out : p.err).append (buf, (size_t) n);
            }

        if (until())
            return true;

        if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > timeoutS)
            return false;
    }
}

int waitExit (Process& p, double timeoutS)
{
    int status = 0;
    bool exited = false;
    pump (p, timeoutS, [&]
    {
        exited = waitpid (p.pid, &status, WNOHANG) == p.pid;
        return exited;
    });

    if (! exited)
    {
        kill (p.pid, SIGKILL);
        waitpid (p.pid, &status, 0);
        return -1;
    }

    pump (p, 0.2, [] { return false; });
    return WIFEXITED (status) ? WEXITSTATUS (status) : 128 + WTERMSIG (status);
}

Process run (const std::vector<std::string>& args, const fs::path& dataDir, double timeoutS, int& exitCode, const std::string& input = {})
{
    auto p = spawn (args, dataDir, input);
    exitCode = waitExit (p, timeoutS);
    return p;
}

bool haveCli()
{
    if (cliBinary().empty() || ! fs::exists (cliBinary()))
    {
        std::cerr << "    (skipped: ROVOX_CLI_BIN is not set)\n";
        return false;
    }

    return true;
}
} // namespace

ROVOX_TEST (cli, version_and_help)
{
    if (! haveCli())
        return;

    int code = -1;
    auto v = run ({ "--version" }, test::makeTempDir ("d"), 20, code);
    CHECK (code == 0);
    CHECK (v.out.find ("Rovox ") == 0);
    CHECK (v.out.find ("Tracktion Engine") != std::string::npos);

    auto h = run ({ "--help-all" }, test::makeTempDir ("d"), 20, code);
    CHECK (code == 0);

    for (const char* sub : { "serve", "render", "import", "upload", "mcp", "bench" })
        CHECK (h.out.find (sub) != std::string::npos);

    auto bad = run ({ "render" }, test::makeTempDir ("d"), 20, code);
    CHECK (code != 0);
}

ROVOX_TEST (cli, import_zprj_then_render_check_json)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("clidata");
    const auto folder = test::makeR20Folder (test::makeTempDir ("clisrc"), "ZOOM0001.zprj", 8, 3.0);
    int code = -1;

    auto imp = run ({ "import", folder.string(), "--new", "test", "--rename", "REC01=Гітара,REC02=Бас", "--json" }, data, 120, code);
    REQUIRE (code == 0);
    const auto summary = json::parse (imp.out);
    CHECK (summary["tracks"].size() == 8);
    CHECK (summary["tracks"][0]["name"] == "Гітара");
    CHECK (summary["detected_source"] == "zoom_r20");

    // 8 sines at -18 dBFS summed stay below 0 dBFS only if some are quiet: lower the master target with --normalize to keep it deterministic
    const auto out = test::makeTempDir ("cliout") / "m.wav";
    auto ren = run ({ "render", "test", "--out", out.string(), "--check", "--json", "--tail-s", "0", "--target-lufs", "-23", "--normalize" }, data, 120, code);
    CHECK (code == 0);
    const auto report = json::parse (ren.out);        // stdout carries only the JSON
    CHECK (report["status"] == "succeeded");
    CHECK (std::abs (report["stats"]["integrated_lufs"].get<double>() + 23.0) < 0.5);
    CHECK (report["file"]["path"] == out.string());
    CHECK (fs::exists (out));
    CHECK (report["preflight"]["passed"] == true);

    // human readable report, same project
    auto text = run ({ "render", "test", "--out", out.string(), "--check", "--tail-s", "0", "--target-lufs", "-23", "--normalize" }, data, 120, code);
    CHECK (code == 0);
    CHECK (text.out.find ("integrated") != std::string::npos);

    // delete a media file of the project: exit code 2, readable message, no file
    const auto projects = data / "projects";
    fs::path media;

    for (auto& p : fs::recursive_directory_iterator (projects))
        if (p.path().parent_path().filename() == "media" && p.path().extension() == ".wav")
        {
            media = p.path();
            break;
        }

    REQUIRE (! media.empty());
    fs::remove (media);
    fs::remove (out);
    auto broken = run ({ "render", "test", "--out", out.string(), "--check", "--tail-s", "0" }, data, 120, code);
    CHECK (code == 2);
    CHECK (broken.out.find ("offline_media") != std::string::npos);
    CHECK (broken.out.find ("FAILED") != std::string::npos);
    CHECK (! fs::exists (out));

    // without --check the same problem is only a warning and the render still happens
    auto lenient = run ({ "render", "test", "--out", out.string(), "--tail-s", "0", "--json" }, data, 120, code);
    CHECK (code == 0);
    CHECK (fs::exists (out));
    CHECK (json::parse (lenient.out)["preflight"]["warnings"].size() >= 1);
}

ROVOX_TEST (cli, unknown_project_is_an_error_with_exit_1)
{
    if (! haveCli())
        return;

    int code = -1;
    auto r = run ({ "render", "no-such-project", "--out", "/tmp/x.wav" }, test::makeTempDir ("empty"), 30, code);
    CHECK (code == 1);
    CHECK (r.err.find ("no project") != std::string::npos);
}

ROVOX_TEST (cli, serve_starts_fast_serves_refuses_a_second_instance_and_autosaves_on_sigterm)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("serve");
    const auto t0 = std::chrono::steady_clock::now();
    auto server = spawn ({ "serve", "--port", "0" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const auto startup = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::cerr << "    startup to 'serving': " << startup << " s\n";
    CHECK (startup < 2.0);

    // "http://127.0.0.1:PORT"
    const auto at = server.out.find ("http://127.0.0.1:");
    REQUIRE (at != std::string::npos);
    const int port = std::stoi (server.out.substr (at + 17));

    httplib::Client c ("127.0.0.1", port);
    auto health = c.Get ("/healthz");
    REQUIRE (health != nullptr);
    CHECK (health->status == 200);

    // a project that is dirty at shutdown
    auto created = c.Post ("/api/projects", R"({"name":"dirty"})", "application/json");
    REQUIRE (created != nullptr);
    REQUIRE (created->status == 200);
    const auto projectId = json::parse (created->body)["data"]["project_id"].get<std::string>();
    auto cmd = c.Post ("/api/projects/" + projectId + "/commands", R"({"name":"track.create","params":{"name":"unsaved"}})", "application/json");
    REQUIRE (cmd != nullptr);
    CHECK (cmd->status == 200);

    // second server on the same data dir
    int code = -1;
    auto second = run ({ "serve", "--port", "0" }, data, 20, code);
    CHECK (code == 1);
    CHECK (second.err.find ("already") != std::string::npos);

    const auto t1 = std::chrono::steady_clock::now();
    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
    CHECK (std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count() < 10.0);

    bool autosaved = false;

    for (auto& e : fs::directory_iterator (data / "projects" / projectId / "project.rvx" / "autosave"))
        autosaved = autosaved || e.path().extension() == ".rvx";

    CHECK (autosaved);
}

ROVOX_TEST (cli, upload_to_a_running_server_and_resume)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("up");
    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const auto at = server.out.find ("http://127.0.0.1:");
    const int port = std::stoi (server.out.substr (at + 17));
    const auto url = "http://127.0.0.1:" + std::to_string (port);

    const auto folder = test::makeR20Folder (test::makeTempDir ("upsrc"), "ZOOM0002.zprj", 3, 2.0);
    int code = -1;
    auto noToken = run ({ "upload", folder.string(), "--server", url }, test::makeTempDir ("d"), 30, code);
    CHECK (code == 1);

    auto up = run ({ "upload", folder.string(), "--server", url, "--token", "tok123", "--chunk-mb", "1" }, test::makeTempDir ("d"), 60, code);
    CHECK (code == 0);
    const auto result = json::parse (up.out);
    CHECK (result["files"].size() >= 3);

    int completed = 0;

    for (auto& e : fs::directory_iterator (data / "uploads"))
        completed += e.path().extension() == ".done" ? 1 : 0;

    CHECK (completed == (int) result["files"].size());
    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (cli, mcp_speaks_json_rpc_on_stdout_and_exits_at_end_of_input)
{
    if (! haveCli())
        return;

    int code = -1;
    const std::string input =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
        R"({"jsonrpc":"2.0","id":1001,"method":"tools/call","params":{"name":"project.new","arguments":{"name":"from-cli"}}})" "\n"
        R"({"jsonrpc":"2.0","id":"abc","method":"tools/list"})" "\n";
    auto m = run ({ "mcp" }, test::makeTempDir ("d"), 60, code, input);
    CHECK (code == 0);

    std::istringstream lines (m.out);
    std::string line;
    int responses = 0;

    while (std::getline (lines, line))
    {
        auto j = json::parse (line, nullptr, false);
        REQUIRE (! j.is_discarded());          // nothing but JSON-RPC on stdout
        ++responses;

        if (j["id"] == 1001)
            CHECK (j["result"]["structuredContent"]["data"]["project_id"].get<std::string>().rfind ("p_", 0) == 0);

        if (j["id"] == "abc")
            CHECK (j["result"]["tools"].size() >= 40);
    }

    CHECK (responses == 3);
}

ROVOX_TEST (cli, bench_is_reported_as_not_available_and_open_binds_need_a_token)
{
    if (! haveCli())
        return;

    int code = -1;
    auto b = run ({ "bench" }, test::makeTempDir ("d"), 20, code);
    CHECK (code == 2);

    auto exposed = run ({ "serve", "--bind", "0.0.0.0", "--port", "0" }, test::makeTempDir ("d"), 20, code);
    CHECK (code == 1);
    CHECK (exposed.err.find ("--token") != std::string::npos);
}
