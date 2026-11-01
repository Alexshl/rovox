// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Runs a helper process (plugin probe, isolated render) and reports how it ended. Used to keep third-party plugin code
// out of the server process: whatever the child does (abort, segfault, hang), the caller only sees a SubprocessResult.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rovox
{

struct SubprocessOptions
{
    std::vector<std::string> argv;                          // argv[0] is the executable path
    std::map<std::string, std::string> env;                 // added to the inherited environment
    double timeoutS = 0;                                    // 0 = none; on expiry the child is killed (SIGKILL)
    std::function<void (const std::string&)> onStdoutLine;  // complete lines, without the newline
    std::function<bool()> shouldCancel;                     // polled about every 50 ms; true kills the child
};

struct SubprocessResult
{
    bool started = false;
    int exitCode = -1;              // valid if signal == 0
    int signal = 0;                 // terminating signal (SIGSEGV, SIGABRT, ...), 0 if the child exited normally
    bool timedOut = false, cancelled = false;
    std::string stdoutText;         // everything the child printed to stdout, kept only if onStdoutLine is not set
    std::string stderrTail;         // last 4 KB of stderr

    bool crashed() const noexcept   { return signal != 0 && ! timedOut && ! cancelled; }
    bool succeeded() const noexcept { return started && signal == 0 && exitCode == 0; }
    std::string describe() const;   // "exited with code 2" | "killed by signal 11 (SIGSEGV)" | "timed out" ...
};

SubprocessResult runSubprocess (const SubprocessOptions&);

} // namespace rovox
