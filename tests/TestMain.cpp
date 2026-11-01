// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "TestFramework.h"
#include "TestSupport.h"

#include <cstring>

int main (int argc, char** argv)
{
    using namespace rovox::test;

    // Helper mode used by the 60-second render reproduction: renders in this (child) process so the parent can time it out.
    if (argc >= 2 && std::strcmp (argv[1], "--child") == 0)
        return runChildMode (argc, argv);

    const std::string filter = argc >= 2 ? argv[1] : "";
    int run = 0, failed = 0;

    for (auto& t : registry())
    {
        if (! filter.empty() && t.suite != filter && (t.suite + "." + t.name) != filter)
            continue;

        ++run;
        const auto before = failureCount();
        std::cerr << "[ RUN  ] " << t.suite << "." << t.name << "\n";

        try
        {
            t.fn();
        }
        catch (const RequireFailed&) {}
        catch (const std::exception& e)
        {
            reportFailure (__FILE__, __LINE__, std::string ("unexpected exception: ") + e.what());
        }

        const bool ok = failureCount() == before;
        failed += ok ? 0 : 1;
        std::cerr << (ok ? "[  OK  ] " : "[ FAIL ] ") << t.suite << "." << t.name << "\n";
    }

    std::cerr << run << " test(s) run, " << failed << " failed\n";
    return (run == 0 || failed > 0) ? 1 : 0;
}
