#include "../mzr_test.h"
#include "core/security/ExtractGuard.h"

using namespace mzr;
using namespace mzr::security;

TEST_CASE("guard: normal extraction passes") {
    ExtractGuard g({}, 10ull << 30);
    CHECK(g.BeginEntry(1000).ok());
    CHECK(g.OnBytesWritten(5000).ok());
    CHECK(g.TotalWritten() == 5000);
}

TEST_CASE("guard: entry count limit") {
    BombLimits l; l.maxEntries = 2;
    ExtractGuard g(l, 0);
    CHECK(g.BeginEntry(1).ok());
    CHECK(g.BeginEntry(1).ok());
    auto r = g.BeginEntry(1);
    CHECK(!r.ok() && r.error().code == ErrorCode::TooManyEntries);
}

TEST_CASE("guard: free disk space limit") {
    ExtractGuard g({}, 1000);
    CHECK(g.BeginEntry(10).ok());
    CHECK(g.OnBytesWritten(900).ok());
    auto r = g.OnBytesWritten(200);
    CHECK(!r.ok() && r.error().code == ErrorCode::DiskSpace);
}

TEST_CASE("guard: zip bomb ratio stops extraction") {
    BombLimits l; l.ratioCheckMinBytes = 1 << 20; l.maxRatio = 1000;
    ExtractGuard g(l, 0);
    CHECK(g.BeginEntry(100).ok());                 // 100 bytes packed
    auto r = g.OnBytesWritten(2 << 20);            // 2 MB out => ratio ~20000
    CHECK(!r.ok() && r.error().code == ErrorCode::SuspiciousRatio);
}

TEST_CASE("guard: highly compressible but small files are not flagged") {
    BombLimits l; l.ratioCheckMinBytes = 1 << 20;
    ExtractGuard g(l, 0);
    CHECK(g.BeginEntry(10).ok());
    CHECK(g.OnBytesWritten(500'000).ok());          // big ratio but under the threshold
}

TEST_CASE("guard: overflow is detected") {
    ExtractGuard g2({}, 0);
    CHECK(g2.BeginEntry(UINT64_MAX).ok());
    CHECK(g2.OnBytesWritten(10).ok());
    auto r = g2.OnBytesWritten(UINT64_MAX);
    CHECK(!r.ok() && r.error().code == ErrorCode::SizeLimitExceeded);
}
