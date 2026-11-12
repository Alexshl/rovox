// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Subprocess.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <algorithm>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace rovox
{

std::string SubprocessResult::describe() const
{
    if (! started)      return "could not be started";
    if (timedOut)       return "timed out";
    if (cancelled)      return "cancelled";

    if (signal != 0)
    {
        const char* name = ::strsignal (signal);
        return "killed by signal " + std::to_string (signal) + (name != nullptr ? " (" + std::string (name) + ")" : std::string());
    }

    return "exited with code " + std::to_string (exitCode);
}

namespace
{
thread_local double childCpuThisThread = 0.0;
}

double childCpuSecondsThisThread()
{
    return childCpuThisThread;
}

SubprocessResult runSubprocess (const SubprocessOptions& o)
{
    SubprocessResult r;

    if (o.argv.empty())
        return r;

    int outPipe[2], errPipe[2];

    // O_CLOEXEC: a concurrently spawned child must not inherit these ends (it would hold the pipe open and delay our EOF);
    // the dup2 file actions below still hand the child its own fds 0/1/2
    if (::pipe2 (outPipe, O_CLOEXEC) != 0)
        return r;

    if (::pipe2 (errPipe, O_CLOEXEC) != 0)
    {
        ::close (outPipe[0]);
        ::close (outPipe[1]);
        return r;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_adddup2 (&actions, outPipe[1], 1);
    posix_spawn_file_actions_adddup2 (&actions, errPipe[1], 2);

    // own process group, so a timeout/cancel kills grandchildren too
    posix_spawnattr_t attr;
    posix_spawnattr_init (&attr);
    posix_spawnattr_setpgroup (&attr, 0);
    posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETPGROUP);

    std::vector<std::string> envStrings;

    for (char** e = environ; e != nullptr && *e != nullptr; ++e)
    {
        const std::string entry = *e;
        const auto eq = entry.find ('=');

        if (eq == std::string::npos || o.env.count (entry.substr (0, eq)) == 0)
            envStrings.push_back (entry);
    }

    for (auto& [k, v] : o.env)
        envStrings.push_back (k + "=" + v);

    std::vector<char*> argv, envp;

    for (auto& a : o.argv)
        argv.push_back (const_cast<char*> (a.c_str()));

    for (auto& e : envStrings)
        envp.push_back (const_cast<char*> (e.c_str()));

    argv.push_back (nullptr);
    envp.push_back (nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawn (&pid, o.argv[0].c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy (&actions);
    posix_spawnattr_destroy (&attr);
    ::close (outPipe[1]);
    ::close (errPipe[1]);

    if (rc != 0)
    {
        ::close (outPipe[0]);
        ::close (errPipe[0]);
        r.stderrTail = std::strerror (rc);
        return r;
    }

    r.started = true;
    const auto start = std::chrono::steady_clock::now();
    bool outOpen = true, errOpen = true;
    std::string lineBuf;
    int status = 0;
    rusage usage {};
    bool reaped = false, killed = false;
    auto killedAt = start;
    constexpr size_t maxStdoutBytes = 16u * 1024 * 1024, maxLineBytes = 1024 * 1024;
    constexpr double killGraceS = 0.5;    // after a kill, a grandchild holding the pipes open is not waited for
    bool truncated = false;

    auto kill9 = [&]
    {
        ::kill (-pid, SIGKILL);    // the child is its own group leader
        ::kill (pid, SIGKILL);
        if (! killed)
        {
            killed = true;
            killedAt = std::chrono::steady_clock::now();
        }
    };

    auto feed = [&] (const char* data, size_t n)
    {
        if (! o.onStdoutLine)
        {
            const size_t room = maxStdoutBytes - std::min (r.stdoutText.size(), maxStdoutBytes);
            r.stdoutText.append (data, std::min (n, room));
            truncated = truncated || n > room;

            return;
        }

        lineBuf.append (data, n);
        size_t nl;

        while ((nl = lineBuf.find ('\n')) != std::string::npos)
        {
            o.onStdoutLine (lineBuf.substr (0, nl));
            lineBuf.erase (0, nl + 1);
        }

        if (lineBuf.size() > maxLineBytes)
        {
            lineBuf.resize (maxLineBytes);
            lineBuf += " [truncated]";
            o.onStdoutLine (lineBuf);
            lineBuf.clear();
        }
    };

    while (outOpen || errOpen)
    {
        pollfd fds[2];
        nfds_t n = 0;
        int outIndex = -1, errIndex = -1;

        if (outOpen)    { outIndex = (int) n; fds[n++] = { outPipe[0], POLLIN, 0 }; }
        if (errOpen)    { errIndex = (int) n; fds[n++] = { errPipe[0], POLLIN, 0 }; }

        const int pr = ::poll (fds, n, 50);

        if (pr < 0 && errno != EINTR)
            break;

        char buf[8192];

        if (pr > 0)
        {
            if (outIndex >= 0 && (fds[outIndex].revents & (POLLIN | POLLHUP | POLLERR)))
            {
                const auto got = ::read (outPipe[0], buf, sizeof (buf));

                if (got > 0)    feed (buf, (size_t) got);
                else if (got == 0 || errno != EINTR) outOpen = false;
            }

            if (errIndex >= 0 && (fds[errIndex].revents & (POLLIN | POLLHUP | POLLERR)))
            {
                const auto got = ::read (errPipe[0], buf, sizeof (buf));

                if (got > 0)
                {
                    r.stderrTail.append (buf, (size_t) got);

                    if (r.stderrTail.size() > 4096)
                        r.stderrTail.erase (0, r.stderrTail.size() - 4096);
                }
                else if (got == 0 || errno != EINTR)
                {
                    errOpen = false;
                }
            }
        }

        if (! r.timedOut && ! r.cancelled)
        {
            if (o.timeoutS > 0 && std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() > o.timeoutS)
            {
                r.timedOut = true;
                kill9();
            }
            else if (o.shouldCancel && o.shouldCancel())
            {
                r.cancelled = true;
                kill9();
            }
        }

        if (killed && std::chrono::duration<double> (std::chrono::steady_clock::now() - killedAt).count() > killGraceS)
            break;
    }

    ::close (outPipe[0]);
    ::close (errPipe[0]);

    // the pipes closed, but a child may have closed them and kept running: keep honouring the timeout and cancellation
    while (! reaped)
    {
        const pid_t w = ::wait4 (pid, &status, WNOHANG, &usage);

        if (w == pid)
        {
            reaped = true;
            break;
        }

        if (w < 0 && errno != EINTR)
            break;

        if (! r.timedOut && ! r.cancelled)
        {
            if (o.timeoutS > 0 && std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() > o.timeoutS)
            {
                r.timedOut = true;
                kill9();
            }
            else if (o.shouldCancel && o.shouldCancel())
            {
                r.cancelled = true;
                kill9();
            }
        }

        ::usleep (20000);
    }

    if (truncated)
        r.stdoutText += "\n[output truncated]\n";

    if (o.onStdoutLine && ! lineBuf.empty())
        o.onStdoutLine (lineBuf);

    if (reaped)
    {
        r.cpuUserS = (double) usage.ru_utime.tv_sec + (double) usage.ru_utime.tv_usec * 1.0e-6;
        r.cpuSystemS = (double) usage.ru_stime.tv_sec + (double) usage.ru_stime.tv_usec * 1.0e-6;
        childCpuThisThread += r.cpuUserS + r.cpuSystemS;

        if (WIFSIGNALED (status))       r.signal = WTERMSIG (status);
        else if (WIFEXITED (status))    r.exitCode = WEXITSTATUS (status);
    }

    return r;
}

} // namespace rovox
