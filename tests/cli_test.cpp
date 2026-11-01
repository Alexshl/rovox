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
    CHECK (bounce->get_header_value ("Set-Cookie").find ("rovox_token=tok123") != std::string::npos);

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

    // a change made over HTTP as an MCP agent arrives with that origin
    c.Post ("/api/projects/" + projectId + "/commands", bearer,
            json { { "name", "track.update" }, { "params", { { "track_id", trackId }, { "name", "Agent was here" } } },
                   { "origin", { { "kind", "mcp" }, { "session", "mcp_9" }, { "label", "Claude" } } } }.dump(), "application/json");
    json agentEvent;
    REQUIRE (ws.waitFor ("event", agentEvent));
    CHECK (agentEvent["origin"]["kind"] == "mcp");
    CHECK (agentEvent["origin"]["label"] == "Claude");

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
    kill (server.pid, SIGTERM);
    CHECK (waitExit (server, 10) == 0);
}
