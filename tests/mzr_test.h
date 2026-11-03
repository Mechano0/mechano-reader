#pragma once
// Tiny self-contained test framework (no download needed).
// Usage:  TEST_CASE("name") { CHECK(a == b); }
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mzr_test {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& Registry() { static std::vector<Case> r; return r; }
inline int& Failures() { static int f = 0; return f; }
inline int& Checks() { static int c = 0; return c; }
struct Reg { Reg(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); } };
inline void Report(bool ok, const char* expr, const char* file, int line) {
    ++Checks();
    if (!ok) { ++Failures(); std::printf("  FAIL %s:%d  %s\n", file, line, expr); }
}
} // namespace mzr_test

#define MZR_CAT2(a, b) a##b
#define MZR_CAT(a, b) MZR_CAT2(a, b)
#define TEST_CASE(name)                                                              \
    static void MZR_CAT(mzr_tc_, __LINE__)();                                        \
    static mzr_test::Reg MZR_CAT(mzr_reg_, __LINE__)(name, &MZR_CAT(mzr_tc_, __LINE__)); \
    static void MZR_CAT(mzr_tc_, __LINE__)()
#define CHECK(expr) mzr_test::Report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
