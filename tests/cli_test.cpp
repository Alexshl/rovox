// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Runs the real `rovox` binary (path in $ROVOX_CLI_BIN, set by CTest) against synthetic projects.

#include "KeyStore.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
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

/** stdin is a pipe that carries `input` and then ends (unless `keepStdin` takes over the write end): no command can block on a terminal. */
Process spawn (const std::vector<std::string>& args, const fs::path& dataDir, const std::string& input = {}, int* keepStdin = nullptr)
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

    if (keepStdin != nullptr)
    {
        *keepStdin = inPipe[1];             // the caller feeds more lines and closes it
        fcntl (inPipe[1], F_SETFD, FD_CLOEXEC);     // a process spawned later must not keep this pipe open: the EOF would never reach the child
    }
    else
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

        if (! j.contains ("id"))
            continue;                                // notifications (resources/list_changed) are valid JSON-RPC but no response

        ++responses;

        if (j["id"] == 1001)
            CHECK (j["result"]["structuredContent"]["data"]["project_id"].get<std::string>().rfind ("p_", 0) == 0);

        if (j["id"] == "abc")
            CHECK (j["result"]["tools"].size() >= 40);
    }

    CHECK (responses == 3);
}

ROVOX_TEST (cli, bench_runs_a_scenario_and_prints_json)
{
    if (! haveCli())
        return;

    int code = -1;
    const auto data = test::makeTempDir ("d");
    fs::create_directories (data / "bench");                        // someone else's directory with this name must survive
    std::ofstream (data / "bench" / "keep.txt") << "x";
    auto b = run ({ "bench", "--scenario", "s1", "--seconds", "2", "--json" }, data, 180, code);
    CHECK (code == 0);
    CHECK (fs::exists (data / "bench" / "keep.txt"));

    for (auto& e : fs::directory_iterator (data))
        CHECK (e.path().filename().string().rfind ("bench-", 0) != 0);      // the run's own directory is removed

    auto report = json::parse (b.out, nullptr, false);
    REQUIRE (! report.is_discarded());
    REQUIRE (report["results"].size() == 1);
    CHECK (report["results"][0]["scenario"] == "s1");
    CHECK (report["results"][0]["offline"]["error"] == "");
}

ROVOX_TEST (cli, mcp_connect_proxies_stdio_to_a_running_server)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("mcp-connect");
    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const auto at = server.out.find ("http://127.0.0.1:");
    REQUIRE (at != std::string::npos);
    const int port = std::stoi (server.out.substr (at + 17));

    httplib::Client c ("127.0.0.1", port);
    const httplib::Headers auth { { "Authorization", "Bearer tok123" } };
    auto created = c.Post ("/api/projects", auth, R"({"name":"via-proxy"})", "application/json");
    REQUIRE (created != nullptr);
    REQUIRE (created->status == 200);
    const auto projectId = json::parse (created->body)["data"]["project_id"].get<std::string>();

    int code = -1;
    const std::string input =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
        R"({"jsonrpc":"2.0","id":1001,"method":"tools/call","params":{"name":"track.create","arguments":{"project_id":")" + projectId + R"(","name":"from-stdio"}}})" "\n";
    auto m = run ({ "mcp", "--connect", "http://127.0.0.1:" + std::to_string (port), "--token", "tok123" }, test::makeTempDir ("d"), 60, code, input);
    CHECK (code == 0);

    std::istringstream lines (m.out);
    std::string line;
    int responses = 0;

    while (std::getline (lines, line))
    {
        auto j = json::parse (line, nullptr, false);
        REQUIRE (! j.is_discarded());
        ++responses;

        if (j["id"] == 1001)
            CHECK (! j["result"]["isError"].get<bool>());
    }

    CHECK (responses == 2);

    // the change made through stdio is visible in the server's own API
    auto got = c.Post ("/api/projects/" + projectId + "/commands", auth, R"({"name":"track.list","params":{}})", "application/json");
    REQUIRE (got != nullptr);
    CHECK (got->status == 200);
    CHECK (got->body.find ("from-stdio") != std::string::npos);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (cli, mcp_connect_survives_a_second_initialize)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("mcp-connect2");
    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const auto at = server.out.find ("http://127.0.0.1:");
    REQUIRE (at != std::string::npos);
    const int port = std::stoi (server.out.substr (at + 17));

    int code = -1;
    const std::string init1 = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n";
    const std::string init2 = R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n";
    auto m = run ({ "mcp", "--connect", "http://127.0.0.1:" + std::to_string (port), "--token", "tok123" }, test::makeTempDir ("d"), 60, code, init1 + init2);
    CHECK (code == 0);
    CHECK (m.out.find ("\"id\":1") != std::string::npos);
    CHECK (m.out.find ("\"id\":2") != std::string::npos);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (cli, mcp_stdio_refuses_a_data_dir_another_process_holds)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("mcp-lock");
    auto server = spawn ({ "serve", "--port", "0" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));

    int code = -1;
    auto m = run ({ "mcp" }, data, 20, code);
    CHECK (code == 1);
    CHECK (m.err.find ("already using") != std::string::npos);
    CHECK (m.err.find ("--data-dir") != std::string::npos);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (cli, mcp_stdio_enforces_the_path_policy_and_allow_path)
{
    if (! haveCli())
        return;

    const std::string head =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n";
    const auto allowed = test::makeTempDir ("mcp-allowed");
    const auto outside = test::makeTempDir ("mcp-outside");
    const auto call = [] (const fs::path& target)
    {
        return json { { "jsonrpc", "2.0" }, { "id", 7 }, { "method", "tools/call" },
                      { "params", { { "name", "project.open" }, { "arguments", { { "target", target.string() } } } } } }.dump() + "\n";
    };

    int code = -1;
    auto denied = run ({ "mcp" }, test::makeTempDir ("d"), 60, code, head + call (outside));
    CHECK (code == 0);
    CHECK (denied.out.find ("FORBIDDEN_PATH") != std::string::npos);

    // --allow-path lifts the refusal for that directory (the open then fails for another reason: nothing is there)
    auto permitted = run ({ "mcp", "--allow-path", allowed.string() }, test::makeTempDir ("d"), 60, code, head + call (allowed));
    CHECK (code == 0);
    CHECK (permitted.out.find ("FORBIDDEN_PATH") == std::string::npos);
    auto stillDenied = run ({ "mcp", "--allow-path", allowed.string() }, test::makeTempDir ("d"), 60, code, head + call (outside));
    CHECK (stillDenied.out.find ("FORBIDDEN_PATH") != std::string::npos);
}

ROVOX_TEST (cli, mcp_connect_recovers_after_the_server_restarts)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("mcp-restart");
    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const auto at = server.out.find ("http://127.0.0.1:");
    REQUIRE (at != std::string::npos);
    const int port = std::stoi (server.out.substr (at + 17));

    int stdinFd = -1;
    const std::string init =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n";
    auto proxy = spawn ({ "mcp", "--connect", "http://127.0.0.1:" + std::to_string (port), "--token", "tok123" }, test::makeTempDir ("d"), init, &stdinFd);
    REQUIRE (stdinFd >= 0);
    REQUIRE (pump (proxy, 20, [&] { return proxy.out.find ("\"id\":1") != std::string::npos; }));

    // the server restarts on the same port: it knows nothing of the session
    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
    auto again = spawn ({ "serve", "--port", std::to_string (port), "--token", "tok123" }, data);
    REQUIRE (pump (again, 20, [&] { return again.out.find ("serving on") != std::string::npos; }));

    const std::string list = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" "\n";
    REQUIRE (write (stdinFd, list.data(), list.size()) == (ssize_t) list.size());
    CHECK (pump (proxy, 20, [&] { return proxy.out.find ("\"id\":2") != std::string::npos; }));
    CHECK (proxy.out.find ("\"tools\"") != std::string::npos);

    // the schemas of the tools contain "error" as a property name: look for a JSON-RPC error object, not for the word
    {
        std::istringstream lines (proxy.out);
        std::string l;

        while (std::getline (lines, l))
        {
            auto j = json::parse (l, nullptr, false);

            if (j.is_object() && j.value ("id", 0) == 2)
                CHECK (! j.contains ("error"));
        }
    }

    close (stdinFd);
    CHECK (waitExit (proxy, 20) == 0);
    kill (again.pid, SIGTERM);
    CHECK (waitExit (again, 10) == 0);
}

ROVOX_TEST (cli, mcp_connect_forwards_a_cancel_while_a_call_waits)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("mcp-cancel");
    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const auto at = server.out.find ("http://127.0.0.1:");
    REQUIRE (at != std::string::npos);
    const int port = std::stoi (server.out.substr (at + 17));

    // events.poll waits 30 s for an event that never comes; the cancel behind it must reach the server while it waits
    int code = -1;
    const std::string input =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"events.poll","arguments":{"since_event_id":1000000,"wait_s":30}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":7}})" "\n";
    const auto t0 = std::chrono::steady_clock::now();
    auto m = run ({ "mcp", "--connect", "http://127.0.0.1:" + std::to_string (port), "--token", "tok123" }, test::makeTempDir ("d"), 60, code, input);
    CHECK (code == 0);
    CHECK (m.out.find ("\"id\":7") != std::string::npos);
    CHECK (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() < 15.0);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (cli, open_binds_need_a_token)
{
    if (! haveCli())
        return;

    int code = -1;
    auto exposed = run ({ "serve", "--bind", "0.0.0.0", "--port", "0" }, test::makeTempDir ("d"), 20, code);
    CHECK (code == 1);
    CHECK (exposed.err.find ("--token") != std::string::npos);
}

//==============================================================================
// Web UI surface (tasks 300, 330, 350): static files, cookie login, path policy, /source, WebSocket gateway
//==============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{
int portOf (const Process& p)
{
    const auto at = p.out.find ("http://127.0.0.1:");
    return at == std::string::npos ? -1 : std::stoi (p.out.substr (at + 17));
}

/** A raw WebSocket client: enough of RFC 6455 to talk to the gateway. */
struct WsClient
{
    int fd = -1;
    std::string buffer;

    ~WsClient()
    {
        if (fd >= 0)
            ::close (fd);
    }

    /** Returns the HTTP status of the upgrade response. */
    int open (int port, const std::vector<std::string>& headers, const std::string& path = "/ws")
    {
        fd = ::socket (AF_INET, SOCK_STREAM, 0);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons ((uint16_t) port);
        inet_pton (AF_INET, "127.0.0.1", &a.sin_addr);

        if (::connect (fd, (sockaddr*) &a, sizeof a) != 0)
            return -1;

        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string (port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";

        for (auto& h : headers)
            req += h + "\r\n";

        req += "\r\n";
        ::send (fd, req.data(), req.size(), 0);
        std::string head;
        char ch;

        while (head.find ("\r\n\r\n") == std::string::npos)
        {
            pollfd p { fd, POLLIN, 0 };

            if (poll (&p, 1, 5000) <= 0 || ::recv (fd, &ch, 1, 0) <= 0)
                return -2;

            head.push_back (ch);
        }

        lastHead = head;

        // drain the (short) body of a rejection
        if (head.find (" 101 ") == std::string::npos)
        {
            pollfd p { fd, POLLIN, 0 };

            while (poll (&p, 1, 100) > 0 && ::recv (fd, &ch, 1, 0) > 0) {}
        }

        return std::stoi (head.substr (9, 3));
    }

    void sendText (const std::string& text)
    {
        std::vector<uint8_t> f { 0x81 };
        const size_t n = text.size();

        if (n < 126)
        {
            f.push_back ((uint8_t) (0x80 | n));
        }
        else
        {
            f.push_back (0x80 | 126);
            f.push_back ((uint8_t) (n >> 8));
            f.push_back ((uint8_t) n);
        }

        const uint8_t mask[4] = { 1, 2, 3, 4 };
        f.insert (f.end(), mask, mask + 4);

        for (size_t i = 0; i < n; ++i)
            f.push_back ((uint8_t) text[i] ^ mask[i % 4]);

        ::send (fd, f.data(), f.size(), 0);
    }

    /** The header of a masked text frame that claims `length` bytes (the server refuses it before reading any payload). */
    void sendFrameHeaderOnly (uint64_t length)
    {
        std::vector<uint8_t> f { 0x81, 0x80 | 127 };

        for (int shift = 56; shift >= 0; shift -= 8)
            f.push_back ((uint8_t) (length >> shift));

        const uint8_t mask[4] = { 1, 2, 3, 4 };
        f.insert (f.end(), mask, mask + 4);
        ::send (fd, f.data(), f.size(), 0);
    }

    bool readExact (void* data, size_t n, int timeoutMs)
    {
        auto* p = (uint8_t*) data;

        while (n > 0)
        {
            pollfd pf { fd, POLLIN, 0 };

            if (poll (&pf, 1, timeoutMs) <= 0)
                return false;

            const auto r = ::recv (fd, p, n, 0);

            if (r <= 0)
                return false;

            p += r;
            n -= (size_t) r;
        }

        return true;
    }

    /** Next frame: opcode and payload. */
    bool readFrame (int& opcode, std::vector<uint8_t>& payload, int timeoutMs = 3000)
    {
        uint8_t h[2];

        if (! readExact (h, 2, timeoutMs))
            return false;

        opcode = h[0] & 0xF;
        uint64_t len = h[1] & 0x7F;

        if (len == 126)
        {
            uint8_t e[2];

            if (! readExact (e, 2, timeoutMs))
                return false;

            len = (uint64_t) e[0] << 8 | e[1];
        }
        else if (len == 127)
        {
            uint8_t e[8];

            if (! readExact (e, 8, timeoutMs))
                return false;

            len = 0;

            for (auto b : e)
                len = len << 8 | b;
        }

        payload.resize ((size_t) len);
        return len == 0 || readExact (payload.data(), (size_t) len, timeoutMs);
    }

    /** Next text frame with `"t": type`; other text frames stay queued for later waits (the event of a command arrives before its result). */
    bool waitFor (const std::string& type, json& out, int timeoutMs = 4000)
    {
        const auto t0 = std::chrono::steady_clock::now();

        for (;;)
        {
            for (auto it = backlog.begin(); it != backlog.end(); ++it)
                if (it->value ("t", "") == type)
                {
                    out = *it;
                    backlog.erase (it);
                    return true;
                }

            if (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count() >= timeoutMs)
                return false;

            int op = 0;
            std::vector<uint8_t> p;

            if (! readFrame (op, p, 100))
                continue;

            if (op == 2)
                binary.push_back (std::move (p));
            else if (op == 1)
            {
                auto j = json::parse (std::string (p.begin(), p.end()), nullptr, false);

                if (j.is_object())
                    backlog.push_back (std::move (j));
            }
        }
    }

    std::vector<json> backlog;
    std::string lastHead;
    std::vector<std::vector<uint8_t>> binary;
};
} // namespace

ROVOX_TEST (web, static_ui_cookie_login_path_policy_and_source)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("webdata");
    const auto web = test::makeTempDir ("webdir");
    std::ofstream (web / "index.html") << "<!doctype html><title>rovox</title>";
    fs::create_directories (web / "assets");
    std::ofstream (web / "assets" / "app.js") << "console.log(1)";

    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123", "--web-dir", web.string() }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);
    httplib::Client c ("127.0.0.1", port);

    // the shell is public and cross-origin isolated; the API is not
    auto index = c.Get ("/");
    REQUIRE (index != nullptr);
    CHECK (index->status == 200);
    CHECK (index->get_header_value ("Cross-Origin-Opener-Policy") == "same-origin");
    CHECK (index->get_header_value ("Cross-Origin-Embedder-Policy") == "require-corp");
    CHECK (index->get_header_value ("Cache-Control") == "no-cache");
    auto asset = c.Get ("/assets/app.js");
    REQUIRE (asset != nullptr);
    CHECK (asset->get_header_value ("Cache-Control").find ("immutable") != std::string::npos);
    CHECK (c.Get ("/api/projects")->status == 401);

    // login: wrong token, right token -> HttpOnly SameSite=Strict cookie
    CHECK (c.Post ("/api/login", R"({"token":"nope"})", "application/json")->status == 401);
    auto login = c.Post ("/api/login", R"({"token":"tok123"})", "application/json");
    REQUIRE (login != nullptr);
    CHECK (login->status == 200);
    const auto cookie = login->get_header_value ("Set-Cookie");
    CHECK (cookie.find ("HttpOnly") != std::string::npos);
    CHECK (cookie.find ("SameSite=Strict") != std::string::npos);
    httplib::Headers withCookie { { "Cookie", cookie.substr (0, cookie.find (';')) } };
    CHECK (c.Get ("/api/projects", withCookie)->status == 200);

    // a cookie is ambient authority: a state-changing request from a foreign origin is refused
    httplib::Headers foreign { { "Cookie", cookie.substr (0, cookie.find (';')) }, { "Origin", "http://evil.example" } };
    CHECK (c.Post ("/api/projects", foreign, R"({"name":"x"})", "application/json")->status == 403);

    // ?token= on the page URL sets the cookie and redirects
    auto bounce = c.Get ("/?token=tok123");
    REQUIRE (bounce != nullptr);
    CHECK (bounce->status == 302);
    CHECK (bounce->get_header_value ("Set-Cookie").find ("rovox_session=") != std::string::npos);
    CHECK (bounce->get_header_value ("Set-Cookie").find ("tok123") == std::string::npos);      // the cookie is signed, it never holds the token

    // AGPL section 13
    auto source = c.Get ("/source");
    REQUIRE (source != nullptr);
    CHECK (source->status == 200);
    CHECK (json::parse (source->body).contains ("commit"));
    CHECK (! json::parse (source->body)["source_url"].get<std::string>().empty());

    // path policy: nothing outside the data dir and the allowed roots
    httplib::Headers bearer { { "Authorization", "Bearer tok123" } };
    auto outside = c.Post ("/api/projects/p_x/commands", bearer, R"({"name":"project.open","params":{"target":"../../etc"}})", "application/json");
    REQUIRE (outside != nullptr);
    std::cerr << "    project.open ../../etc -> " << outside->status << " " << outside->body << "\n";
    CHECK (outside->status == 403);
    CHECK (outside->body.find ("FORBIDDEN_PATH") != std::string::npos);
    auto etc = c.Post ("/api/projects/p_x/commands", bearer, R"({"name":"project.open","params":{"target":"/etc"}})", "application/json");
    CHECK (etc->status == 403);

    auto created = c.Post ("/api/projects", bearer, R"({"name":"policy"})", "application/json");
    const auto projectId = json::parse (created->body)["data"]["project_id"].get<std::string>();
    auto plan = c.Post ("/api/projects/" + projectId + "/commands", bearer, R"({"name":"import.plan","params":{"paths":["/etc/passwd"]}})", "application/json");
    CHECK (plan->status == 403);

    // wrong tokens are rate limited: after ten in a minute the answer is 429 with Retry-After, a right token on a request keeps working
    int lastStatus = 0;

    for (int i = 0; i < 14; ++i)
    {
        auto wrong = c.Post ("/api/login", R"({"token":"guess"})", "application/json");
        lastStatus = wrong != nullptr ? wrong->status : -1;
    }

    CHECK (lastStatus == 429);
    auto limited = c.Post ("/api/login", R"({"token":"guess"})", "application/json");
    CHECK (limited->get_header_value ("Retry-After") == "30");
    CHECK (c.Get ("/api/projects", bearer)->status == 200);

    // no secret in any answer
    CHECK (source->body.find ("tok123") == std::string::npos);
    CHECK (c.Get ("/api/session")->body.find ("tok123") == std::string::npos);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (web, websocket_auth_origin_snapshot_events_and_audio)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("wsdata");
    const auto media = test::makeTempDir ("wsmedia");
    test::writeWav (media / "tone.wav", { test::sine (440, -12, 8.0, 44100) }, 44100);

    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123", "--audio", "browser", "--allow-path", media.string() }, data);
    REQUIRE (pump (server, 30, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);
    httplib::Client c ("127.0.0.1", port);
    httplib::Headers bearer { { "Authorization", "Bearer tok123" } };

    auto created = c.Post ("/api/projects", bearer, R"({"name":"ws"})", "application/json");
    const auto projectId = json::parse (created->body)["data"]["project_id"].get<std::string>();
    auto plan = c.Post ("/api/projects/" + projectId + "/commands", bearer,
                        json { { "name", "import.plan" }, { "params", { { "paths", json::array ({ (media / "tone.wav").string() }) } } } }.dump(), "application/json");
    REQUIRE (plan->status == 200);
    auto applied = c.Post ("/api/projects/" + projectId + "/commands", bearer,
                           json { { "name", "import.apply" }, { "params", { { "plan_id", json::parse (plan->body)["data"]["plan_id"] } } } }.dump(), "application/json");
    REQUIRE (applied->status == 200);

    // upgrade refusals
    { WsClient w; CHECK (w.open (port, {}) == 401); }
    { WsClient w; CHECK (w.open (port, { "Origin: http://evil.example", "Authorization: Bearer tok123" }) == 403); }
    { WsClient w; CHECK (w.open (port, {}, "/ws?token=wrong") == 401); }
    { WsClient w; CHECK (w.open (port, { "Host: rebind.example", "Origin: http://rebind.example", "Authorization: Bearer tok123" }) == 403); }

    WsClient ws;
    REQUIRE (ws.open (port, { "Origin: http://localhost:" + std::to_string (port), "Authorization: Bearer tok123" }) == 101);
    ws.sendText (R"({"t":"hello","client":"test/0"})");
    json welcome;
    REQUIRE (ws.waitFor ("welcome", welcome));
    CHECK (welcome["audio"]["mode"] == "browser-stream");
    CHECK (welcome["audio"]["available"] == true);
    const auto session = welcome["session"].get<std::string>();

    ws.sendText (json { { "t", "sub" }, { "project_id", projectId }, { "channels", json::array ({ "state", "transport", "meters" }) } }.dump());
    json snapshot;
    REQUIRE (ws.waitFor ("snapshot", snapshot));
    CHECK (snapshot["project_id"] == projectId);
    CHECK (snapshot["state"]["tracks"].size() == 1);
    CHECK (snapshot["meter_index"].size() == 2);
    CHECK (snapshot["meter_index"][0] == "master");
    const auto trackId = snapshot["state"]["tracks"][0]["track_id"].get<std::string>();

    // a command from the socket: origin ui, the answer carries the revision, the event carries the origin
    ws.sendText (json { { "t", "cmd" }, { "id", "c_1" }, { "name", "track.update" }, { "project_id", projectId },
                        { "params", { { "track_id", trackId }, { "volume_db", -4.5 } } } }.dump());
    json result;
    REQUIRE (ws.waitFor ("result", result));
    CHECK (result["id"] == "c_1");
    CHECK (result["ok"] == true);
    json event;
    REQUIRE (ws.waitFor ("event", event));
    CHECK (event["type"] == "project.changed");
    CHECK (event["origin"]["kind"] == "ui");
    CHECK (event["origin"]["session"] == session);

    // a change made over HTTP arrives as an agent: the body cannot claim to be the UI or pick a label
    c.Post ("/api/projects/" + projectId + "/commands", bearer,
            json { { "name", "track.update" }, { "params", { { "track_id", trackId }, { "name", "Agent was here" } } },
                   { "origin", { { "kind", "ui" }, { "session", "ws_1" }, { "label", "Claude" } } } }.dump(), "application/json");
    json agentEvent;
    REQUIRE (ws.waitFor ("event", agentEvent));
    CHECK (agentEvent["origin"]["kind"] == "mcp");
    CHECK (agentEvent["origin"]["label"] == "http");
    CHECK (agentEvent["origin"]["session"] != "ws_1");                  // the body's origin is ignored: neither kind, session nor label
    CHECK (agentEvent["origin"]["session"] != session);
    CHECK (agentEvent["origin"]["session"].get<std::string>().rfind ("http:", 0) == 0);
    CHECK (agentEvent["origin"]["label"] != "Claude");

    // audio: play, receive Opus packets, 0x03 positions and 0x02 meters
    ws.sendText (R"({"t":"audio","on":true})");
    ws.sendText (json { { "t", "cmd" }, { "id", "c_2" }, { "name", "transport.set" }, { "project_id", projectId }, { "params", { { "state", "playing" }, { "position_s", 1.0 } } } }.dump());
    json played;
    REQUIRE (ws.waitFor ("result", played));
    CHECK (played["ok"] == true);
    json never;
    ws.waitFor ("never-arrives", never, 1500);          // collects binary frames for 1.5 s

    int audio = 0, positions = 0, meters = 0;

    for (auto& f : ws.binary)
    {
        audio += f[0] == 1 ? 1 : 0;
        positions += f[0] == 3 ? 1 : 0;
        meters += f[0] == 2 ? 1 : 0;
    }

    std::cerr << "    in 1.5 s: " << audio << " audio, " << positions << " position, " << meters << " meter frames\n";
    CHECK (audio >= 40);
    CHECK (positions >= 25);
    CHECK (meters >= 25);

    ws.sendText (json { { "t", "cmd" }, { "id", "c_3" }, { "name", "transport.set" }, { "project_id", projectId }, { "params", { { "state", "stopped" } } } }.dump());

    // a frame above the message limit is answered with close code 1009 (message too big)
    {
        WsClient big;
        REQUIRE (big.open (port, { "Origin: http://localhost:" + std::to_string (port), "Authorization: Bearer tok123" }) == 101);
        big.sendFrameHeaderOnly (2u << 20);
        int opcode = -1;
        std::vector<uint8_t> payload;
        bool closed = false;

        for (int i = 0; i < 20 && big.readFrame (opcode, payload, 3000); ++i)
            if (opcode == 8)
            {
                closed = true;
                break;
            }

        REQUIRE (closed);
        REQUIRE (payload.size() >= 2);
        CHECK (((payload[0] << 8) | payload[1]) == 1009);
    }

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

//==============================================================================
// E11-02: authentication wiring (REST, WebSocket, MCP stdio)

namespace
{
KeyStore::CreateSpec keySpec (const std::string& tenant, ScopeSet scopes)
{
    KeyStore::CreateSpec s;
    s.label = "cli-test";
    s.tenant = tenant;
    s.scopes = scopes;
    return s;
}
} // namespace

ROVOX_TEST (web, keys_cookie_session_scopes_and_legacy_token_regression)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("authdata");
    KeyStore keys (data / "auth" / "keys.json");
    auto reader = keys.create (keySpec ("cli-acme", ScopeSet::of ({ Scope::catalogRead })));
    REQUIRE (reader.ok());
    const auto readerKey = reader.value().key;
    const auto readerId = reader.value().record["id"].get<std::string>();

    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);
    httplib::Client c ("127.0.0.1", port);

    // the legacy token still works as a header, a query parameter on GET and the old cookie
    CHECK (c.Get ("/api/projects")->status == 401);
    CHECK (c.Get ("/api/projects", httplib::Headers { { "Authorization", "Bearer tok123" } })->status == 200);
    CHECK (c.Get ("/api/projects?token=tok123")->status == 200);
    CHECK (c.Get ("/api/projects", httplib::Headers { { "Cookie", "rovox_token=tok123" } })->status == 200);
    CHECK (c.Get ("/api/projects", httplib::Headers { { "Authorization", "Bearer tok124" } })->status == 401);

    // the login cookie is signed and never holds the token
    auto login = c.Post ("/api/login", R"({"token":"tok123"})", "application/json");
    REQUIRE (login != nullptr);
    CHECK (login->status == 200);
    const auto setCookie = login->get_header_value ("Set-Cookie");
    CHECK (setCookie.rfind ("rovox_session=", 0) == 0);
    CHECK (setCookie.find ("tok123") == std::string::npos);
    const auto cookie = setCookie.substr (0, setCookie.find (';'));
    CHECK (c.Get ("/api/projects", httplib::Headers { { "Cookie", cookie } })->status == 200);

    // a forged or altered cookie is refused
    auto forged = cookie;
    forged.back() = forged.back() == '0' ? '1' : '0';
    CHECK (c.Get ("/api/projects", httplib::Headers { { "Cookie", forged } })->status == 401);
    CHECK (c.Get ("/api/projects", httplib::Headers { { "Cookie", "rovox_session=legacy.99999999999.00" } })->status == 401);

    // /api/session says who is asking, without secrets
    auto session = c.Get ("/api/session", httplib::Headers { { "Cookie", cookie } });
    REQUIRE (session != nullptr);
    const auto sj = json::parse (session->body);
    CHECK (sj["authenticated"] == true);
    CHECK (sj["tenant"].get<std::string>() == "local");
    CHECK (sj["key_id"].get<std::string>() == "legacy");
    CHECK (sj["auth_mode"].get<std::string>() == "legacy");
    CHECK (session->body.find ("tok123") == std::string::npos);

    // an API key: its own tenant and scopes, and a 403 for what it lacks
    httplib::Headers asReader { { "Authorization", "Bearer " + readerKey } };
    auto readerSession = c.Get ("/api/session", asReader);
    REQUIRE (readerSession != nullptr);
    const auto rj = json::parse (readerSession->body);
    CHECK (rj["tenant"].get<std::string>() == "cli-acme");
    CHECK (rj["key_id"].get<std::string>() == readerId);
    CHECK (rj["auth_mode"].get<std::string>() == "token");
    REQUIRE (rj["scopes"].size() == 1);
    CHECK (rj["scopes"][0].get<std::string>() == "catalog:read");
    CHECK (readerSession->body.find (readerKey) == std::string::npos);

    auto render = c.Post ("/api/projects/p_none/commands", asReader, R"({"name":"render.start","params":{}})", "application/json");
    REQUIRE (render != nullptr);
    CHECK (render->status == 403);
    CHECK (render->body.find ("FORBIDDEN_SCOPE") != std::string::npos);

    // the key logs in too: the cookie carries its id, not the key
    auto keyLogin = c.Post ("/api/login", json { { "token", readerKey } }.dump(), "application/json");
    REQUIRE (keyLogin != nullptr);
    CHECK (keyLogin->status == 200);
    const auto keyCookie = keyLogin->get_header_value ("Set-Cookie");
    CHECK (keyCookie.find (readerKey) == std::string::npos);
    CHECK (keyCookie.find ("rovox_session=" + readerId + ".") == 0);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (web, websocket_of_a_revoked_key_is_closed_within_a_second)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("wsrevoke");
    KeyStore keys (data / "auth" / "keys.json");
    auto created = keys.create (keySpec ("cli-wsrev", ScopeSet::of ({ Scope::catalogRead, Scope::projectsRead })));
    REQUIRE (created.ok());

    auto server = spawn ({ "serve", "--port", "0" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);

    WsClient ws;
    REQUIRE (ws.open (port, { "Authorization: Bearer " + created.value().key }) == 101);
    ws.sendText (R"({"t":"hello","client":"test/0"})");
    json welcome;
    REQUIRE (ws.waitFor ("welcome", welcome));

    REQUIRE (keys.revoke (created.value().record["id"].get<std::string>()).ok());
    const auto t0 = std::chrono::steady_clock::now();
    bool closed = false;

    while (! closed && std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() < 3.0)
    {
        int op = 0;
        std::vector<uint8_t> payload;

        if (ws.readFrame (op, payload, 100))
        {
            closed = op == 8;
        }
        else
        {
            char ch;
            closed = ::recv (ws.fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
        }
    }

    CHECK (closed);
    CHECK (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() <= 1.0);     // the ticket says one second

    // and the revoked key no longer opens a new socket
    { WsClient again; CHECK (again.open (port, { "Authorization: Bearer " + created.value().key }) == 401); }

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (web, per_key_rate_limit_answers_429_with_retry_after_and_leaves_other_keys_alone)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("ratelimit");
    fs::create_directories (data / "config");
    {
        std::ofstream out (data / "config" / "rovox.toml");
        out << "[quota_profiles.tiny]\nrequests_per_min = 5\n\n[tenants.cli-rl-a]\nquota_profile = \"tiny\"\n\n[tenants.cli-rl-b]\nquota_profile = \"tiny\"\n";
    }

    KeyStore keys (data / "auth" / "keys.json");
    auto a = keys.create (keySpec ("cli-rl-a", ScopeSet::of ({ Scope::catalogRead, Scope::projectsRead })));
    auto b = keys.create (keySpec ("cli-rl-b", ScopeSet::of ({ Scope::catalogRead, Scope::projectsRead })));
    REQUIRE (a.ok());
    REQUIRE (b.ok());

    auto server = spawn ({ "serve", "--port", "0" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);
    httplib::Client c ("127.0.0.1", port);
    const httplib::Headers keyA { { "Authorization", "Bearer " + a.value().key } }, keyB { { "Authorization", "Bearer " + b.value().key } };

    for (int i = 0; i < 5; ++i)
    {
        auto ok = c.Get ("/api/projects", keyA);
        REQUIRE (ok != nullptr);
        CHECK (ok->status == 200);
    }

    auto limited = c.Get ("/api/projects", keyA);                   // request N + 1 of the minute
    REQUIRE (limited != nullptr);
    CHECK (limited->status == 429);
    CHECK (limited->body.find ("RATE_LIMITED") != std::string::npos);
    const auto retryAfter = limited->get_header_value ("Retry-After");
    CHECK (! retryAfter.empty());
    CHECK (std::atoi (retryAfter.c_str()) >= 1);

    auto other = c.Get ("/api/projects", keyB);                     // key B has a bucket of its own
    REQUIRE (other != nullptr);
    CHECK (other->status == 200);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}

ROVOX_TEST (cli, mcp_tenant_flag_gives_every_scope_but_admin)
{
    if (! haveCli())
        return;

    const std::string input =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli-test"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"usage.summary","arguments":{}}})" "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"usage.report","arguments":{}}})" "\n";
    int code = -1;
    auto m = run ({ "mcp", "--tenant", "acme" }, test::makeTempDir ("tenantdata"), 60, code, input);
    CHECK (code == 0);

    std::map<int, json> byId;
    std::istringstream lines (m.out);
    std::string l;

    while (std::getline (lines, l))
    {
        auto j = json::parse (l, nullptr, false);

        if (j.is_object() && j.contains ("id") && j["id"].is_number_integer())
            byId[j["id"].get<int>()] = j;
    }

    REQUIRE (byId.count (2) == 1);
    REQUIRE (byId.count (3) == 1);
    CHECK (byId[2].dump().find ("acme") != std::string::npos);
    CHECK (byId[3].dump().find ("FORBIDDEN_SCOPE") != std::string::npos);

    // not a valid tenant id: refused before anything starts
    int bad = -1;
    run ({ "mcp", "--tenant", "Not Valid!" }, test::makeTempDir ("tenantbad"), 20, bad);
    CHECK (bad == 1);
}

//==============================================================================
// E11-07: key management CLI

namespace
{
std::string trimmed (std::string s)
{
    while (! s.empty() && std::isspace ((unsigned char) s.back()))
        s.pop_back();

    return s;
}

std::string slurp (const fs::path& p)
{
    std::ifstream in (p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** The status of GET /api/projects with `key`, polled until it is `want` or `timeoutS` passes; returns the last status. */
int statusWithKey (int port, const std::string& key, int want, double timeoutS)
{
    httplib::Client c ("127.0.0.1", port);
    const auto t0 = std::chrono::steady_clock::now();
    int status = -1;

    for (;;)
    {
        auto r = c.Get ("/api/projects", httplib::Headers { { "Authorization", "Bearer " + key } });
        status = r != nullptr ? r->status : -1;

        if (status == want || std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > timeoutS)
            return status;

        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
}
} // namespace

ROVOX_TEST (cli, key_create_list_rotate_revoke_show_the_secret_once_and_work_with_serve)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("keycli");     // its own data dir: auth/keys.json is shared state
    std::vector<std::string> allOutput;                 // stdout and stderr of every command after the key was shown
    int code = -1;

    // create: the key is the only line on stdout
    auto created = run ({ "key", "create", "--tenant", "cli-keyt", "--scopes", "catalog:read,projects:read", "--label", "ci", "--expires", "30d" }, data, 30, code);
    REQUIRE (code == 0);
    const auto key1 = trimmed (created.out);
    const auto parsed1 = parseApiKey (key1);
    REQUIRE (parsed1.has_value());
    CHECK (created.out.find ('\n') == created.out.size() - 1);
    CHECK (created.err.find (key1) == std::string::npos);
    CHECK (created.err.find (parsed1->id) != std::string::npos);

    // list: ids only
    auto listed = run ({ "key", "list", "--json", "--tenant", "cli-keyt" }, data, 30, code);
    CHECK (code == 0);
    allOutput.push_back (listed.out + listed.err);
    const auto records = json::parse (listed.out, nullptr, false);
    REQUIRE ((records.is_array() && records.size() == 1));
    CHECK (records[0].value ("id", std::string()) == parsed1->id);
    CHECK (records[0].value ("label", std::string()) == "ci");
    CHECK (records[0].contains ("expires_at"));
    CHECK (! records[0].contains ("hash"));

    auto text = run ({ "key", "list" }, data, 30, code);
    CHECK (code == 0);
    CHECK (text.out.find (parsed1->id) != std::string::npos);
    CHECK (text.out.find ("active") != std::string::npos);
    allOutput.push_back (text.out + text.err);

    // refused: admin for a tenant other than local, a bad scope, an unknown id
    auto adminKey = run ({ "key", "create", "--tenant", "cli-keyt", "--scopes", "admin" }, data, 30, code);
    CHECK (code == 1);
    CHECK (adminKey.out.empty());
    run ({ "key", "create", "--tenant", "cli-keyt", "--scopes", "nonsense" }, data, 30, code);
    CHECK (code == 1);
    run ({ "key", "revoke", "k_doesnotexist" }, data, 30, code);
    CHECK (code == 1);

    // the key works with a running serve, rotate swaps it, revoke ends it, each within the one-second reload
    auto server = spawn ({ "serve", "--port", "0" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);
    CHECK (statusWithKey (port, key1, 200, 3.0) == 200);

    auto rotated = run ({ "key", "rotate", parsed1->id, "--json" }, data, 30, code);
    REQUIRE (code == 0);
    const auto rotatedJson = json::parse (rotated.out, nullptr, false);
    REQUIRE (rotatedJson.is_object());
    const auto key2 = rotatedJson.value ("key", std::string());
    const auto parsed2 = parseApiKey (key2);
    REQUIRE (parsed2.has_value());
    CHECK (parsed2->id != parsed1->id);
    CHECK (rotated.err.find (key2) == std::string::npos);
    CHECK (statusWithKey (port, key1, 401, 3.0) == 401);
    CHECK (statusWithKey (port, key2, 200, 3.0) == 200);

    auto revoked = run ({ "key", "revoke", parsed2->id }, data, 30, code);
    CHECK (code == 0);
    allOutput.push_back (revoked.out + revoked.err);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK (statusWithKey (port, key2, 401, 3.0) == 401);
    CHECK (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() <= 2.5);

    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
    allOutput.push_back (server.out + server.err);

    // the secret never shows up again: not in later output, the key file, the audit journal or the server's logs
    allOutput.push_back (slurp (data / "auth" / "keys.json"));
    const auto audit = slurp (data / "auth" / "audit.jsonl");
    allOutput.push_back (audit);

    for (auto* k : { &key1, &key2 })
    {
        const auto secret = parseApiKey (*k)->secret;

        for (auto& o : allOutput)
        {
            CHECK (o.find (*k) == std::string::npos);
            CHECK (o.find (secret) == std::string::npos);
        }
    }

    // the audit journal: create, rotate (old key), create (new key), revoke; no hash, private file
    CHECK (audit.find ("sha256") == std::string::npos);
    CHECK (audit.find ("\"hash\"") == std::string::npos);
    std::istringstream lines (audit);
    std::vector<std::string> actions;

    for (std::string l; std::getline (lines, l);)
    {
        const auto j = json::parse (l, nullptr, false);
        REQUIRE (j.is_object());
        CHECK (j.value ("actor", std::string()) == "cli");
        CHECK (j.value ("tenant", std::string()) == "cli-keyt");
        actions.push_back (j.value ("action", std::string()));
    }

    CHECK (actions == std::vector<std::string> ({ "create", "rotate", "create", "revoke" }));
    CHECK ((fs::status (data / "auth" / "audit.jsonl").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
}

ROVOX_TEST (cli, readyz_is_public_and_metrics_needs_the_admin_scope)
{
    if (! haveCli())
        return;

    const auto data = test::makeTempDir ("metrics");
    int code = -1;
    auto created = run ({ "key", "create", "--tenant", "cli-metrics", "--scopes", "catalog:read,projects:read" }, data, 30, code);
    REQUIRE (code == 0);
    const auto key = trimmed (created.out);
    const auto parsed = parseApiKey (key);
    REQUIRE (parsed.has_value());

    auto server = spawn ({ "serve", "--port", "0", "--token", "tok123", "--drain-timeout", "2" }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);
    httplib::Client c ("127.0.0.1", port);

    auto ready = c.Get ("/readyz");
    REQUIRE (ready != nullptr);
    CHECK (ready->status == 200);
    CHECK (json::parse (ready->body).value ("ready", false));

    auto noAuth = c.Get ("/metrics");
    REQUIRE (noAuth != nullptr);
    CHECK (noAuth->status == 401);

    auto noAdmin = c.Get ("/metrics", httplib::Headers { { "Authorization", "Bearer " + key } });
    REQUIRE (noAdmin != nullptr);
    CHECK (noAdmin->status == 403);
    CHECK (json::parse (noAdmin->body)["error"].value ("code", "") == "FORBIDDEN_SCOPE");

    auto metrics = c.Get ("/metrics", httplib::Headers { { "Authorization", "Bearer tok123" } });
    REQUIRE (metrics != nullptr);
    CHECK (metrics->status == 200);
    CHECK (metrics->get_header_value ("Content-Type").rfind ("text/plain", 0) == 0);
    CHECK (metrics->body.find ("# TYPE rovox_jobs gauge") != std::string::npos);
    CHECK (metrics->body.find ("rovox_jobs{state=\"queued\"} 0") != std::string::npos);
    CHECK (metrics->body.find ("rovox_draining 0") != std::string::npos);
    CHECK (metrics->body.find ("rovox_ws_clients 0") != std::string::npos);
    CHECK (metrics->body.find ("rovox_tenant_cpu_seconds_today{tenant=\"local\"}") != std::string::npos);
    CHECK (metrics->body.find (parsed->id) == std::string::npos);       // no key ids, no secrets
    CHECK (metrics->body.find (parsed->secret) == std::string::npos);
    CHECK (metrics->body.find ("tok123") == std::string::npos);

    const auto t0 = std::chrono::steady_clock::now();
    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
    CHECK (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() < 5.0);      // nothing running: the drain does not wait for the timeout
}

ROVOX_TEST (cli, sigterm_during_a_render_interrupts_the_job_saves_the_project_and_exits_cleanly)
{
    if (! haveCli())
        return;

    // a render that outlasts the drain window: several long mono files (8 kHz keeps the files small), normalised to a target (two passes)
    const auto media = test::makeTempDir ("sigterm-media");
    json paths = json::array();

    for (int i = 0; i < 4; ++i)
    {
        const auto f = media / ("t" + std::to_string (i + 1) + ".wav");
        test::writeWav (f, { test::sine (220.0 * (i + 1), -20.0, 3000.0, 8000.0) }, 8000.0, 16);
        paths.push_back (f.string());
    }

    const auto data = test::makeTempDir ("sigterm-render");
    auto server = spawn ({ "serve", "--port", "0", "--drain-timeout", "1", "--allow-path", media.string() }, data);
    REQUIRE (pump (server, 20, [&] { return server.out.find ("serving on") != std::string::npos; }));
    const int port = portOf (server);
    REQUIRE (port > 0);

    httplib::Client c ("127.0.0.1", port);
    c.set_read_timeout (180);
    auto created = c.Post ("/api/projects", R"({"name":"render-then-term"})", "application/json");
    REQUIRE (created != nullptr);
    REQUIRE (created->status == 200);
    const auto projectId = json::parse (created->body)["data"]["project_id"].get<std::string>();

    const json planBody { { "name", "import.plan" }, { "params", { { "paths", paths } } } };
    auto planned = c.Post ("/api/projects/" + projectId + "/commands", planBody.dump(), "application/json");
    REQUIRE (planned != nullptr);
    REQUIRE (planned->status == 200);
    const auto planId = json::parse (planned->body)["data"]["plan_id"].get<std::string>();
    const json applyBody { { "name", "import.apply" }, { "params", { { "plan_id", planId } } } };
    auto applied = c.Post ("/api/projects/" + projectId + "/commands", applyBody.dump(), "application/json");
    REQUIRE (applied != nullptr);
    REQUIRE (applied->status == 200);

    // an MCP session over HTTP starts the render job
    const httplib::Headers base { { "Accept", "application/json, text/event-stream" }, { "MCP-Protocol-Version", "2025-06-18" } };
    auto init = c.Post ("/mcp", base, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"sigterm-test"}}})", "application/json");
    REQUIRE (init != nullptr);
    const auto sid = init->get_header_value ("Mcp-Session-Id");
    REQUIRE (! sid.empty());
    httplib::Headers session = base;
    session.emplace ("Mcp-Session-Id", sid);
    c.Post ("/mcp", session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "application/json");

    auto call = [&] (const std::string& tool, const json& args) -> json
    {
        const json body { { "jsonrpc", "2.0" }, { "id", 5 }, { "method", "tools/call" }, { "params", { { "name", tool }, { "arguments", args } } } };
        auto res = c.Post ("/mcp", session, body.dump(), "application/json");

        if (res == nullptr)
            return json (nullptr);

        const auto parsed = json::parse (res->body, nullptr, false);
        return parsed.is_object() && parsed.contains ("result") ? json (parsed["result"]["structuredContent"]) : json (nullptr);
    };

    const auto started = call ("render.start", { { "project_id", projectId }, { "name", "long" }, { "target_lufs", -16.0 }, { "tail_s", 0 } });
    REQUIRE (started.is_object());
    REQUIRE (started.value ("ok", false));
    const auto jobId = started["data"]["job_id"].get<std::string>();

    // SIGTERM only once the job runs (never a fixed sleep)
    bool running = false;

    for (int i = 0; i < 600 && ! running; ++i)
    {
        const auto job = call ("job.get", { { "job_id", jobId } });
        running = job.is_object() && job.contains ("data") && job["data"].value ("state", "") == "running";

        if (! running)
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }

    REQUIRE (running);
    kill (server.pid, SIGTERM);

    // /readyz drops at once, while the job still has its drain window
    bool notReady = false;

    for (int i = 0; i < 100 && ! notReady; ++i)
    {
        httplib::Client probe ("127.0.0.1", port);
        probe.set_connection_timeout (1);
        probe.set_read_timeout (1);

        if (auto ready = probe.Get ("/readyz"); ready != nullptr)
            notReady = ready->status == 503;

        if (! notReady)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    CHECK (notReady);
    CHECK (waitExit (server, 30) == 0);                  // a clean exit, not a crash or a kill

    // the job record says interrupted
    std::ifstream in (data / "jobs" / (jobId + ".json"));
    REQUIRE (in.good());
    const auto record = json::parse (in, nullptr, false);
    REQUIRE (record.is_object());
    CHECK (record.value ("state", "") == "interrupted");
    CHECK (record.contains ("error") && record["error"].value ("code", "") == "INTERRUPTED");

    // the project is kept: the imported (dirty) state is in autosave/
    bool autosaved = false;

    for (auto& e : fs::directory_iterator (data / "projects" / projectId / "project.rvx" / "autosave"))
        autosaved = autosaved || e.path().extension() == ".rvx";

    CHECK (autosaved);
}
