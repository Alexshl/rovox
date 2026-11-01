// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Cgroup.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace rovox
{

namespace
{
std::string readSmallFile (const char* path)
{
    std::ifstream in (path);
    std::stringstream ss;
    ss << in.rdbuf();
    auto s = ss.str();

    while (! s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();

    return s;
}
} // namespace

double parseCpuMax (const std::string& content)
{
    std::istringstream in (content);
    std::string quota, period;

    if (! (in >> quota >> period) || quota == "max")
        return 0.0;

    try
    {
        const auto q = std::stod (quota), p = std::stod (period);
        return (q > 0.0 && p > 0.0) ? q / p : 0.0;
    }
    catch (...)
    {
        return 0.0;
    }
}

double cgroupCpuQuota()
{
    return parseCpuMax (readSmallFile ("/sys/fs/cgroup/cpu.max"));
}

std::int64_t cgroupMemoryLimit()
{
    const auto s = readSmallFile ("/sys/fs/cgroup/memory.max");

    if (s.empty() || s == "max")
        return 0;

    try { return (std::int64_t) std::stoll (s); } catch (...) { return 0; }
}

CpuBudget cpuBudget()
{
    CpuBudget b;

    if (const char* env = std::getenv ("ROVOX_CPUS"); env != nullptr && std::atoi (env) > 0)
    {
        b.cpus = std::atoi (env);       // explicit override (hosts that cannot express a cgroup quota)
        return b;
    }

    b.quota = cgroupCpuQuota();
    b.cpus = b.quota > 0.0 ? std::max (1, (int) std::ceil (b.quota))
                           : std::max (1, (int) std::thread::hardware_concurrency());
    return b;
}

} // namespace rovox
