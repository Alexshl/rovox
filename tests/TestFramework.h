// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Minimal test harness (no external dependency): ROVOX_TEST (suite, name) { ... } with CHECK / REQUIRE.
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace rovox::test
{

struct TestCase
{
    std::string suite, name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

struct Registrar
{
    Registrar (const char* suite, const char* name, std::function<void()> fn) { registry().push_back ({ suite, name, std::move (fn) }); }
};

struct RequireFailed {};

inline int& failureCount()
{
    static int n = 0;
    return n;
}

inline void reportFailure (const char* file, int line, const std::string& what)
{
    ++failureCount();
    std::cerr << "    FAILED " << file << ":" << line << ": " << what << "\n";
}

} // namespace rovox::test

#define ROVOX_TEST(suite, name) \
    static void rovox_test_##suite##_##name(); \
    static ::rovox::test::Registrar rovox_reg_##suite##_##name (#suite, #name, &rovox_test_##suite##_##name); \
    static void rovox_test_##suite##_##name()

#define CHECK(expr) \
    do { if (! (expr)) ::rovox::test::reportFailure (__FILE__, __LINE__, #expr); } while (false)

#define REQUIRE(expr) \
    do { if (! (expr)) { ::rovox::test::reportFailure (__FILE__, __LINE__, #expr); throw ::rovox::test::RequireFailed(); } } while (false)

#define CHECK_NEAR(a, b, tol) \
    do { const double a_ = (a), b_ = (b); \
         if (! (std::abs (a_ - b_) <= (tol))) { std::ostringstream s_; s_ << #a << " = " << a_ << ", expected " << b_ << " +- " << (tol); \
                                                ::rovox::test::reportFailure (__FILE__, __LINE__, s_.str()); } } while (false)
