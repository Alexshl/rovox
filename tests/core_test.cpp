// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Cgroup.h"
#include "Ids.h"
#include "TestFramework.h"

#include <set>

using namespace rovox;

ROVOX_TEST (core, ulid_sorted_and_unique)
{
    std::set<std::string> seen;
    std::string previous;

    for (int i = 0; i < 20000; ++i)
    {
        auto id = newId ("trk");
        CHECK (isValidId (id, "trk"));
        CHECK (id > previous);
        previous = id;
        seen.insert (id);
    }

    CHECK (seen.size() == 20000);
    CHECK (! isValidId ("trk_short", "trk"));
    CHECK (! isValidId (newId ("itm"), "trk"));
}

ROVOX_TEST (core, tenant_ids)
{
    CHECK (isValidTenantId ("local"));
    CHECK (isValidTenantId ("a_b-9"));
    CHECK (! isValidTenantId ("../x"));
    CHECK (! isValidTenantId ("A"));
    CHECK (! isValidTenantId (""));
    CHECK (! isValidTenantId (std::string (65, 'a')));
}

ROVOX_TEST (core, cgroup_cpu_max)
{
    CHECK_NEAR (parseCpuMax ("max 100000"), 0.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("200000 100000"), 2.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("150000 100000\n"), 1.5, 1e-9);
    CHECK_NEAR (parseCpuMax (""), 0.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("garbage"), 0.0, 1e-9);
    CHECK (cpuBudget().cpus >= 1);
}
