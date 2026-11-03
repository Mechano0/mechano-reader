// Phase 7 security audit: regression tests for the fixed findings (see docs/SECURITY-AUDIT-PHASE7.md).
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/filesystem/FileSystem.h"
#include "core/security/ExtractGuard.h"
#include "core/security/PathSanitizer.h"

using namespace mzr;
using namespace mzr::archive;
using mzr_test::FileExists;
using mzr_test::ReadFileBytes;
using mzr_test::TempDir;
using mzr_test::WriteFileBytes;

namespace {

// Data that does not compress (simple LCG), so the archive is about as big as the data.
std::string NoiseBytes(size_t n, uint32_t seed) {
    std::string s(n, '\0');
    uint32_t x = seed;
    for (auto& c : s) { x = x * 1664525u + 1013904223u; c = static_cast<char>(x >> 24); }
    return s;
}

size_t CountFiles(const std::wstring& dir, const wchar_t* pattern) {
    size_t n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

} // namespace

// ---- S2: unknown packed size (solid 7z) is NOT treated as "1 byte"

TEST_CASE("guard: unknown packed size uses the archive size, not 1 byte (S2)") {
    security::BombLimits l;
    l.ratioCheckMinBytes = 1 << 20;
    l.maxRatio = 1000;
    {
        security::ExtractGuard g(l, 0, 10ull << 20);      // 10 MB archive
        CHECK(g.BeginEntry(0).ok());                        // packed size unknown
        CHECK(g.OnBytesWritten(5ull << 20).ok());           // 5 MB out: ratio 0.5, fine
    }
    {
        security::ExtractGuard g(l, 0, 100);                // 100-byte archive
        CHECK(g.BeginEntry(0).ok());
        auto r = g.OnBytesWritten(2ull << 20);              // 2 MB out of 100 bytes: bomb
        CHECK(!r.ok() && r.error().code == ErrorCode::SuspiciousRatio);
    }
}

TEST_CASE("solid 7z: a big non-first file is not reported as a zip bomb (S2)") {
    TempDir d(L"p7-solid");
    WriteFileBytes(d / L"src\\a-small.txt", "first file of the solid block");
    WriteFileBytes(d / L"src\\b-big.bin", NoiseBytes(3u << 20, 7));
    CompressionSettings cs;
    cs.format = L"7z";
    CancelToken cancel;
    CHECK(CreateArchive(d / L"solid.7z", fs::CollectSources({d / L"src\\a-small.txt", d / L"src\\b-big.bin"}, L""), cs,
                        nullptr, cancel, nullptr).ok());
    auto a = Archive::Open(d / L"solid.7z", {}, nullptr);
    CHECK(a.ok());
    if (!a.ok()) return;
    // The case this test is about: 7-Zip gives no packed size for the second file of the block.
    bool sawUnknownPack = false;
    for (const auto& e : a.value()->Index()->entries())
        if (e.path.find(L"b-big") != std::wstring::npos) sawUnknownPack = e.packSize == 0;
    CHECK(sawUnknownPack);
    // Same rule as the app, scaled down: the ratio test starts after 1 MB instead of 1 GB.
    security::BombLimits l;
    l.ratioCheckMinBytes = 1 << 20;
    ExtractOptions eo;
    eo.destination = d / L"out";
    auto r = a.value()->Extract({}, eo, nullptr, cancel, l);
    CHECK(r.issues.empty());
    CHECK(r.filesDone == 2);
    CHECK(ReadFileBytes(d / L"out\\b-big.bin") == NoiseBytes(3u << 20, 7));
}

// ---- S3 + S6: risky names the way Windows sees them

TEST_CASE("risky file names: trailing dot / space and hidden characters do not hide .exe (S3, S6)") {
    using security::IsRiskyFileName;
    CHECK(IsRiskyFileName(L"setup.exe"));
    CHECK(IsRiskyFileName(L"SETUP.EXE"));
    CHECK(IsRiskyFileName(L"invoice.exe."));
    CHECK(IsRiskyFileName(L"invoice.exe "));
    CHECK(IsRiskyFileName(L"invoice.exe. . "));
    CHECK(IsRiskyFileName(L"folder\\run.bat"));
    // Built with code points (a raw RLO in the source would reorder the code itself in editors).
    CHECK(IsRiskyFileName(std::wstring(L"invoice") + wchar_t(0x202E) + L"fdp.exe"));   // shows as "invoiceexe.pdf"
    CHECK(IsRiskyFileName(std::wstring(L"tool.ex") + wchar_t(0x200E) + L"e"));         // hidden mark inside the extension
    CHECK(IsRiskyFileName(L"console.msc"));
    CHECK(IsRiskyFileName(L"desktop.scf"));
    CHECK(IsRiskyFileName(L"help.chm"));
    CHECK(IsRiskyFileName(L"module.psm1"));
    CHECK(IsRiskyFileName(L"open.settingcontent-ms"));
    CHECK(IsRiskyFileName(L"disk.img"));
    CHECK(!IsRiskyFileName(L"report.pdf"));
    CHECK(!IsRiskyFileName(L"photo.jpg."));
    CHECK(!IsRiskyFileName(L"exe"));
    CHECK(!IsRiskyFileName(L"تقرير.docx"));
    CHECK(!IsRiskyFileName(L""));
}

// ---- S4: a failed overwrite keeps the user's original file

TEST_CASE("overwrite: a failed entry keeps the original file, a good one replaces it (S4)") {
    TempDir d(L"p7-overwrite");
    WriteFileBytes(d / L"secret.txt", "NEW CONTENT FROM THE ARCHIVE");
    CompressionSettings cs;
    cs.password = L"Right-123";
    CancelToken cancel;
    CHECK(CreateArchive(d / L"p.zip", fs::CollectSources({d / L"secret.txt"}, L""), cs, nullptr, cancel, nullptr).ok());

    const std::wstring out = d / L"out";
    WriteFileBytes(out + L"\\secret.txt", "ORIGINAL USER FILE");

    // Wrong password + Overwrite: before the fix the original was truncated, then deleted.
    {
        auto a = Archive::Open(d / L"p.zip", {}, [](const std::wstring&) { return std::optional<std::wstring>(L"wrong"); });
        CHECK(a.ok());
        if (a.ok()) {
            ExtractOptions eo;
            eo.destination = out;
            eo.overwrite = OverwriteMode::Overwrite;
            auto r = a.value()->Extract({}, eo, nullptr, cancel);
            CHECK(r.wrongPassword);
            CHECK(r.filesDone == 0);
        }
        CHECK(ReadFileBytes(out + L"\\secret.txt") == "ORIGINAL USER FILE");
        CHECK(CountFiles(out, L"~mzr*.tmp") == 0);
    }

    // Right password + Overwrite on a READ-ONLY original: replaced.
    SetFileAttributesW((out + L"\\secret.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    {
        auto a = Archive::Open(d / L"p.zip", {}, [](const std::wstring&) { return std::optional<std::wstring>(L"Right-123"); });
        CHECK(a.ok());
        if (a.ok()) {
            ExtractOptions eo;
            eo.destination = out;
            eo.overwrite = OverwriteMode::Overwrite;
            auto r = a.value()->Extract({}, eo, nullptr, cancel);
            CHECK(r.issues.empty());
            CHECK(r.filesDone == 1);
        }
        CHECK(ReadFileBytes(out + L"\\secret.txt") == "NEW CONTENT FROM THE ARCHIVE");
        CHECK(CountFiles(out, L"~mzr*.tmp") == 0);
        CHECK(CountFiles(out, L"*") == 1);
    }
    SetFileAttributesW((out + L"\\secret.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

// ---- S7: random, always-new temp folders

TEST_CASE("temp folders: random names, always a new folder (S7)") {
    const std::wstring h1 = fs::RandomHex(), h2 = fs::RandomHex();
    CHECK(h1.size() == 16 && h2.size() == 16 && h1 != h2);
    CHECK(fs::RandomHex(4).size() == 8);
    const std::wstring a = fs::NewTempSubfolder(L"test-"), b = fs::NewTempSubfolder(L"test-");
    CHECK(!a.empty() && !b.empty() && a != b);
    CHECK(fs::IsDirectory(a) && fs::IsDirectory(b));
    CHECK(a.find(fs::TempFolder()) == 0);
    TempDir::Remove(a);
    TempDir::Remove(b);
}
