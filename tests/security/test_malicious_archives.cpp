// Security tests with REAL hostile archives (built on the fly with entry names
// that a malicious tool would write). Extraction must never write outside the
// destination, never create links, and must stop zip bombs.
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/filesystem/FileSystem.h"

using namespace mzr;
using namespace mzr::archive;
using mzr_test::FileExists;
using mzr_test::TempDir;
using mzr_test::WriteFileBytes;

namespace {

// Build a ZIP whose entry names are chosen by the "attacker".
bool MakeHostileZip(const std::wstring& zip, const std::wstring& payload,
                    const std::vector<std::wstring>& names, const wchar_t* format = L"zip") {
    std::vector<SourceItem> items;
    for (const auto& n : names) {
        SourceItem s;
        s.diskPath = payload;
        s.archivePath = n;
        s.size = 7;
        items.push_back(s);
    }
    CompressionSettings cs;
    cs.format = format;
    CancelToken c;
    return CreateArchive(zip, items, cs, nullptr, c, nullptr).ok();
}

} // namespace

TEST_CASE("malicious zip: traversal entries are blocked, safe ones extracted") {
    TempDir d(L"slip");
    WriteFileBytes(d / L"payload.txt", "PWNED!!");
    const std::wstring zip = d / L"evil.zip";
    CHECK(MakeHostileZip(zip, d / L"payload.txt",
                         {L"..\\..\\escaped1.txt", L"good\\..\\..\\escaped2.txt", L"C:\\escaped3.txt",
                          L"\\\\server\\share\\escaped4.txt", L"ok\\safe.txt"}));
    auto arc = Archive::Open(zip, {}, nullptr);
    CHECK(arc.ok());
    if (!arc.ok()) return;

    const std::wstring dest = d / L"dest\\inner";
    ExtractOptions eo;
    eo.destination = dest;
    CancelToken cancel;
    auto rep = arc.value()->Extract({}, eo, nullptr, cancel);

    // Nothing may appear above the destination.
    CHECK(!FileExists(d / L"escaped1.txt"));
    CHECK(!FileExists(d / L"dest\\escaped1.txt"));
    CHECK(!FileExists(d / L"escaped2.txt"));
    CHECK(!FileExists(d / L"dest\\escaped2.txt"));
    CHECK(!FileExists(L"C:\\escaped3.txt"));
    // The safe entry still works, and the blocked ones are reported.
    CHECK(mzr_test::ReadFileBytes(dest + L"\\ok\\safe.txt") == "PWNED!!");
    CHECK(rep.skipped >= 2);
    CHECK(!rep.issues.empty());
}

TEST_CASE("malicious zip: reserved names and ADS are neutralised") {
    TempDir d(L"names");
    WriteFileBytes(d / L"payload.txt", "content");
    const std::wstring zip = d / L"names.zip";
    CHECK(MakeHostileZip(zip, d / L"payload.txt", {L"CON.txt", L"doc.txt:hidden.exe", L"trail. . "}));
    auto arc = Archive::Open(zip, {}, nullptr);
    CHECK(arc.ok());
    if (!arc.ok()) return;
    ExtractOptions eo;
    eo.destination = d / L"out";
    CancelToken cancel;
    auto rep = arc.value()->Extract({}, eo, nullptr, cancel);
    CHECK(rep.filesDone == 3);
    CHECK(FileExists(d / L"out\\_CON.txt"));
    CHECK(FileExists(d / L"out\\doc.txt_hidden.exe"));
    CHECK(FileExists(d / L"out\\trail"));
}

TEST_CASE("zip bomb: ratio limit stops extraction and removes the partial file") {
    TempDir d(L"bomb");
    // 64 MB of zeros compresses ~1000:1 with Deflate.
    WriteFileBytes(d / L"zeros.bin", std::string(64 * 1024 * 1024, '\0'));
    CompressionSettings cs;
    cs.level = CompressionLevel::Maximum;
    CancelToken cancel;
    CHECK(CreateArchive(d / L"bomb.zip", fs::CollectSources({d / L"zeros.bin"}, L""), cs, nullptr, cancel, nullptr).ok());
    DeleteFileW((d / L"zeros.bin").c_str());

    auto arc = Archive::Open(d / L"bomb.zip", {}, nullptr);
    CHECK(arc.ok());
    if (!arc.ok()) return;
    security::BombLimits lim;
    lim.ratioCheckMinBytes = 8 * 1024 * 1024;   // test-sized thresholds
    lim.maxRatio = 100;
    ExtractOptions eo;
    eo.destination = d / L"out";
    auto rep = arc.value()->Extract({}, eo, nullptr, cancel, lim);
    bool flagged = false;
    for (const auto& i : rep.issues) if (i.friendly.find(L"zip bomb") != std::wstring::npos) flagged = true;
    CHECK(flagged);
    CHECK(!FileExists(d / L"out\\zeros.bin"));

    // Total-size limit also works.
    security::BombLimits lim2;
    lim2.maxTotalBytes = 1024 * 1024;
    auto rep2 = arc.value()->Extract({}, eo, nullptr, cancel, lim2);
    CHECK(!rep2.issues.empty());
    CHECK(!FileExists(d / L"out\\zeros.bin"));
}

TEST_CASE("existing symlink in destination is never written through") {
    TempDir d(L"link");
    WriteFileBytes(d / L"payload.txt", "new data");
    WriteFileBytes(d / L"victim.txt", "original");
    const std::wstring zip = d / L"l.zip";
    CHECK(MakeHostileZip(zip, d / L"payload.txt", {L"target.txt"}));
    mzr::fs::CreateDirectories(d / L"out");
    // Symlink creation needs Developer Mode or admin; skip quietly if not allowed.
    if (!CreateSymbolicLinkW((d / L"out\\target.txt").c_str(), (d / L"victim.txt").c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        std::printf("      (symlink creation not permitted here: test skipped)\n");
        return;
    }
    auto arc = Archive::Open(zip, {}, nullptr);
    CHECK(arc.ok());
    if (!arc.ok()) return;
    ExtractOptions eo;
    eo.destination = d / L"out";
    eo.overwrite = OverwriteMode::Overwrite;
    CancelToken cancel;
    arc.value()->Extract({}, eo, nullptr, cancel);
    CHECK(mzr_test::ReadFileBytes(d / L"victim.txt") == "original");      // untouched
    CHECK(mzr_test::ReadFileBytes(d / L"out\\target (2).txt") == "new data");
}
