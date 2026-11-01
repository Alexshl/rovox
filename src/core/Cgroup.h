// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <cstdint>
#include <string>

namespace rovox
{

/** Parses the content of cgroup v2 `cpu.max` ("max 100000" or "200000 100000").
    Returns the quota in CPUs (2.0 for "200000 100000"), or 0 if unlimited / unparsable. */
double parseCpuMax (const std::string& content);

/** CPU quota of this container from /sys/fs/cgroup/cpu.max, 0 if unlimited or unknown. */
double cgroupCpuQuota();

/** Memory limit in bytes from /sys/fs/cgroup/memory.max, 0 if unlimited or unknown. */
std::int64_t cgroupMemoryLimit();

/** What the engine should size its thread pools by: ceil (quota) if a quota exists, else the host CPU count (>= 1).
    JUCE reports the host CPU count inside `docker run --cpus=1`, which oversubscribes the audio graph. */
struct CpuBudget
{
    int cpus = 1;
    double quota = 0.0;     // 0 = no cgroup quota
};

CpuBudget cpuBudget();

} // namespace rovox
