// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "TestFramework.h"
#include "TestSupport.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace rovox;

ROVOX_TEST (engine, host_uses_data_dir_for_prefs)
{
    auto& h = test::host();
    CHECK (fs::exists (h.dataDir() / "projects"));
    CHECK (toPath (h.engine().getPropertyStorage().getAppPrefsFolder()).string().find (h.dataDir().string()) == 0);
    CHECK (h.audioMode() == AudioMode::None);
    CHECK (h.engine().getEngineBehaviour().getEditLimits().maxNumTracks > 400);
    CHECK (h.engine().getDeviceManager().deviceManager.getCurrentAudioDevice() == nullptr);
}

ROVOX_TEST (engine, thread_budget_follows_cpus_and_is_never_zero)
{
    auto& h = test::host();
    CHECK (h.cpuBudget().cpus >= 1);
    CHECK (h.engine().getEngineBehaviour().getNumberOfCPUsToUseForAudio() == h.cpuBudget().cpus);
    // a zero here once became (size_t) -1 worker threads inside Tracktion's node player
    CHECK (h.engine().getEngineBehaviour().getNumberOfCPUsToUseForAudio() >= 1);
}

// The catalog is created lazily and first reached at once by the startup scan thread and by request/job threads. Two creators used to replace each
// other's catalog under the one using it (glibc aborted in PluginCatalog::rescan: "__owner == 0"): everybody must get the same object.
ROVOX_TEST (engine, catalog_is_created_once_when_threads_ask_at_the_same_time)
{
    for (int round = 0; round < 6; ++round)
    {
        EngineConfig c;
        c.dataDir = test::makeTempDir ("catalog-race");
        EngineHost h (c);

        std::atomic<bool> go { false };
        std::vector<PluginCatalog*> seen (8, nullptr);
        std::vector<std::thread> threads;

        for (size_t i = 0; i < seen.size(); ++i)
            threads.emplace_back ([&, i]
            {
                while (! go)
                    std::this_thread::yield();

                seen[i] = &h.catalog();
            });

        go = true;

        for (auto& t : threads)
            t.join();

        for (auto* p : seen)
            CHECK (p == seen[0]);
    }
}
