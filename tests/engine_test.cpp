// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "TestFramework.h"
#include "TestSupport.h"

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
