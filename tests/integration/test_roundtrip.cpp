// Integration: Create archive -> add files -> save -> reopen -> extract -> verify
// (brief section 29), for ZIP and 7z, with Arabic names, plus add/delete/rename/test.
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/filesystem/FileSystem.h"

using namespace mzr;
using namespace mzr::archive;
using mzr_test::ReadFileBytes;
using mzr_test::TempDir;
using mzr_test::WriteFileBytes;

namespace {

std::string Pattern(size_t n, char seed) {
    std::string s(n, '\0');
    uint32_t x = static_cast<uint8_t>(seed) * 2654435761u + 1;
    for (auto& c : s) { x = x * 1664525u + 1013904223u; c = static_cast<char>(x >> 24); }
    return s;
}

void MakeSourceTree(const TempDir& d) {
    WriteFileBytes(d / L"src\\project\\readme.txt", "hello archive\r\n");
    WriteFileBytes(d / L"src\\project\\data\\random.bin", Pattern(300 * 1024, 'r'));      // incompressible
    WriteFileBytes(d / L"src\\project\\data\\zeros.bin", std::string(2 * 1024 * 1024, '\0'));
    WriteFileBytes(d / L"src\\project\\مستندات\\تقرير المشروع.txt", "تقرير");
    WriteFileBytes(d / L"src\\project\\empty.txt", "");
    CreateDirectoryW((d / L"src\\project\\empty folder").c_str(), nullptr);
}

const EntryRecord* FindEntry(const ArchiveIndex& idx, const std::wstring& path) {
    for (const auto& e : idx.entries()) {
        std::wstring p = e.path;
        for (auto& c : p) if (c == L'/') c = L'\\';
        if (p == path) return &e;
    }
    return nullptr;
}

void RoundTrip(const wchar_t* format) {
    TempDir d(format);
    MakeSourceTree(d);
    const std::wstring arcPath = d / (std::wstring(L"out.") + format);

    CompressionSettings cs;
    cs.format = format;
    CancelToken cancel;
    OperationReport rep;
    auto sources = fs::CollectSources({d / L"src\\project"}, L"");
    CHECK(sources.size() == 9);    // project, readme, data, random, zeros, مستندات, تقرير, empty.txt, empty folder
    auto created = CreateArchive(arcPath, sources, cs, nullptr, cancel, &rep);
    CHECK(created.ok());
    CHECK(rep.issues.empty());

    auto opened = Archive::Open(arcPath, {}, nullptr);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto& arc = *opened.value();
    auto idx = arc.Index();
    CHECK(idx->totalFiles() == 5);
    CHECK(idx->FindFolder(L"project\\مستندات") != ArchiveIndex::kNoEntry);
    CHECK(idx->FindFolder(L"project\\empty folder") != ArchiveIndex::kNoEntry);
    const auto* zeros = FindEntry(*idx, L"project\\data\\zeros.bin");
    CHECK(zeros && zeros->size == 2 * 1024 * 1024 && zeros->packSize < 64 * 1024);   // compressed well

    // Test integrity.
    auto t = arc.Test(nullptr, cancel);
    CHECK(t.issues.empty());
    CHECK(t.filesDone == 5);

    // Extract all and compare bytes.
    ExtractOptions eo;
    eo.destination = d / L"x1";
    auto r = arc.Extract({}, eo, nullptr, cancel);
    CHECK(r.issues.empty());
    CHECK(r.filesDone == 5);
    CHECK(ReadFileBytes(d / L"x1\\project\\data\\random.bin") == Pattern(300 * 1024, 'r'));
    CHECK(ReadFileBytes(d / L"x1\\project\\مستندات\\تقرير المشروع.txt") == "تقرير");
    CHECK(ReadFileBytes(d / L"x1\\project\\empty.txt").empty());
    CHECK(fs::IsDirectory(d / L"x1\\project\\empty folder"));

    // Extract again with Rename policy: no overwrite, a " (2)" copy appears.
    auto r2 = arc.Extract({}, eo, nullptr, cancel);
    CHECK(r2.issues.empty());
    CHECK(mzr_test::FileExists(d / L"x1\\project\\readme (2).txt"));

    // Extract only one selected file.
    ExtractOptions eo2;
    eo2.destination = d / L"x2";
    const auto* readme = FindEntry(*idx, L"project\\readme.txt");
    CHECK(readme != nullptr);
    if (readme) {
        auto r3 = arc.Extract({readme->arcIndex}, eo2, nullptr, cancel);
        CHECK(r3.filesDone == 1);
        CHECK(!mzr_test::FileExists(d / L"x2\\project\\data\\random.bin"));
        CHECK(ReadFileBytes(d / L"x2\\project\\readme.txt") == "hello archive\r\n");
    }

    // Update: add a file, delete one, rename one.
    WriteFileBytes(d / L"new\\added.txt", "added later");
    UpdatePlan plan;
    plan.add = fs::CollectSources({d / L"new\\added.txt"}, L"project");
    if (zeros) plan.remove.push_back(zeros->arcIndex);
    if (readme) plan.rename.emplace_back(readme->arcIndex, L"project\\README-renamed.txt");
    OperationReport ur;
    auto up = arc.Update(plan, cs, nullptr, cancel, &ur);
    CHECK(up.ok());
    auto idx2 = arc.Index();
    CHECK(idx2->totalFiles() == 5);
    CHECK(FindEntry(*idx2, L"project\\added.txt") != nullptr);
    CHECK(FindEntry(*idx2, L"project\\data\\zeros.bin") == nullptr);
    CHECK(FindEntry(*idx2, L"project\\README-renamed.txt") != nullptr);
    CHECK(FindEntry(*idx2, L"project\\readme.txt") == nullptr);

    ExtractOptions eo3;
    eo3.destination = d / L"x3";
    auto r4 = arc.Extract({}, eo3, nullptr, cancel);
    CHECK(r4.issues.empty());
    CHECK(ReadFileBytes(d / L"x3\\project\\added.txt") == "added later");
    CHECK(ReadFileBytes(d / L"x3\\project\\README-renamed.txt") == "hello archive\r\n");
}

} // namespace

TEST_CASE("roundtrip: zip") { RoundTrip(L"zip"); }
TEST_CASE("roundtrip: 7z") { RoundTrip(L"7z"); }

TEST_CASE("password: non-English password is refused for ZIP with a clear message, accepted for 7z") {
    TempDir d(L"pw7");
    WriteFileBytes(d / L"secret.txt", "سر");
    CompressionSettings zip;
    zip.password = L"كلمة-سر";
    CancelToken cancel;
    auto z = CreateArchive(d / L"a.zip", fs::CollectSources({d / L"secret.txt"}, L""), zip, nullptr, cancel, nullptr);
    CHECK(!z.ok());
    CHECK(!z.ok() && z.error().friendly.find(L"7z") != std::wstring::npos);
    CHECK(!mzr_test::FileExists(d / L"a.zip"));

    CompressionSettings sz;
    sz.format = L"7z";
    sz.password = L"كلمة-سر";
    sz.encryptHeaders = true;                 // file names hidden too
    CHECK(CreateArchive(d / L"a.7z", fs::CollectSources({d / L"secret.txt"}, L""), sz, nullptr, cancel, nullptr).ok());
    int asked = 0;
    auto o = Archive::Open(d / L"a.7z", {}, [&](const std::wstring&) { ++asked; return std::optional<std::wstring>(L"كلمة-سر"); });
    CHECK(o.ok());
    CHECK(asked == 1);                         // asked once at open (encrypted headers), remembered for extract
    if (o.ok()) {
        ExtractOptions eo; eo.destination = d / L"out";
        auto r = o.value()->Extract({}, eo, nullptr, cancel);
        CHECK(r.issues.empty());
        CHECK(asked == 1);
        CHECK(ReadFileBytes(d / L"out\\secret.txt") == "سر");
    }
    // Wrong password on encrypted headers: open fails with a friendly message.
    auto w = Archive::Open(d / L"a.7z", {}, [](const std::wstring&) { return std::optional<std::wstring>(L"nope"); });
    CHECK(!w.ok() && w.error().code == ErrorCode::WrongPassword);
}

TEST_CASE("password protected zip (AES-256)") {
    TempDir d(L"pw");
    WriteFileBytes(d / L"secret.txt", "top secret");
    CompressionSettings cs;
    cs.password = L"Secret-123!";
    CancelToken cancel;
    auto c = CreateArchive(d / L"p.zip", fs::CollectSources({d / L"secret.txt"}, L""), cs, nullptr, cancel, nullptr);
    CHECK(c.ok());
    if (!c.ok()) std::wprintf(L"      create error: %ls | %ls\n", c.error().friendly.c_str(), c.error().details.c_str());

    // Right password.
    auto ok = Archive::Open(d / L"p.zip", {}, [](const std::wstring&) { return std::optional<std::wstring>(L"Secret-123!"); });
    CHECK(ok.ok());
    if (ok.ok()) {
        CHECK(ok.value()->Index()->info().hasEncrypted);
        ExtractOptions eo; eo.destination = d / L"good";
        auto r = ok.value()->Extract({}, eo, nullptr, cancel);
        CHECK(r.issues.empty() && !r.wrongPassword);
        CHECK(ReadFileBytes(d / L"good\\secret.txt") == "top secret");
    }

    // Wrong password: reported, and no damaged file is left behind.
    auto bad = Archive::Open(d / L"p.zip", {}, [](const std::wstring&) { return std::optional<std::wstring>(L"wrong"); });
    CHECK(bad.ok());
    if (bad.ok()) {
        ExtractOptions eo; eo.destination = d / L"bad";
        auto r = bad.value()->Extract({}, eo, nullptr, cancel);
        CHECK(r.wrongPassword);
        CHECK(!mzr_test::FileExists(d / L"bad\\secret.txt"));
    }

    // User cancels the password prompt.
    auto none = Archive::Open(d / L"p.zip", {}, [](const std::wstring&) { return std::optional<std::wstring>(); });
    if (none.ok()) {
        ExtractOptions eo; eo.destination = d / L"none";
        auto r = none.value()->Extract({}, eo, nullptr, cancel);
        CHECK(r.cancelled);
    }
}

TEST_CASE("corrupted and non-archive files fail gracefully") {
    TempDir d(L"bad");
    WriteFileBytes(d / L"fake.zip", "this is not a zip file at all");
    auto r = Archive::Open(d / L"fake.zip", {}, nullptr);
    CHECK(!r.ok());
    CHECK(!r.ok() && !r.error().friendly.empty());

    // Real archive with bytes damaged in the middle: opens, but extraction reports damage, no crash.
    WriteFileBytes(d / L"f.txt", Pattern(200 * 1024, 'c'));
    CompressionSettings cs; cs.level = CompressionLevel::Fast;
    CancelToken cancel;
    CHECK(CreateArchive(d / L"c.zip", fs::CollectSources({d / L"f.txt"}, L""), cs, nullptr, cancel, nullptr).ok());
    std::string bytes = ReadFileBytes(d / L"c.zip");
    for (size_t i = 5000; i < 5200 && i < bytes.size(); ++i) bytes[i] = static_cast<char>(bytes[i] ^ 0x5A);
    WriteFileBytes(d / L"c.zip", bytes);
    auto c = Archive::Open(d / L"c.zip", {}, nullptr);
    CHECK(c.ok());
    if (c.ok()) {
        ExtractOptions eo; eo.destination = d / L"out";
        auto rep = c.value()->Extract({}, eo, nullptr, cancel);
        CHECK(!rep.issues.empty());
        CHECK(!mzr_test::FileExists(d / L"out\\f.txt"));       // partial file removed
        auto t = c.value()->Test(nullptr, cancel);
        CHECK(!t.issues.empty());
    }

    // Truncated archive.
    WriteFileBytes(d / L"t.zip", bytes.substr(0, bytes.size() / 2));
    auto tr = Archive::Open(d / L"t.zip", {}, nullptr);
    if (tr.ok()) {
        ExtractOptions eo; eo.destination = d / L"tout";
        auto rep = tr.value()->Extract({}, eo, nullptr, cancel);
        CHECK(!rep.issues.empty() || rep.filesDone == 0);
    }
}

TEST_CASE("smart extract strip + cancel") {
    TempDir d(L"cancel");
    WriteFileBytes(d / L"a.bin", Pattern(4 * 1024 * 1024, 'a'));
    CancelToken cancel;
    CompressionSettings cs;
    CHECK(CreateArchive(d / L"a.zip", fs::CollectSources({d / L"a.bin"}, L""), cs, nullptr, cancel, nullptr).ok());
    auto arc = Archive::Open(d / L"a.zip", {}, nullptr);
    CHECK(arc.ok());
    if (arc.ok()) {
        CancelToken pre;
        pre.Cancel();
        ExtractOptions eo; eo.destination = d / L"out";
        auto rep = arc.value()->Extract({}, eo, nullptr, pre);
        CHECK(rep.cancelled);
        CHECK(!mzr_test::FileExists(d / L"out\\a.bin"));
    }
}
