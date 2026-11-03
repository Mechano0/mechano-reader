#include "../mzr_test.h"
#include "core/archive/SmartExtract.h"

using namespace mzr::archive;

static std::shared_ptr<const ArchiveIndex> Make(std::initializer_list<const wchar_t*> paths) {
    std::vector<EntryRecord> v;
    uint32_t i = 0;
    for (auto p : paths) { EntryRecord e; e.arcIndex = i++; e.path = p; v.push_back(e); }
    return ArchiveIndex::Build({}, std::move(v));
}

TEST_CASE("smart extract: single root folder goes straight in") {
    auto idx = Make({L"project/a.txt", L"project/b/c.txt"});
    auto t = ComputeSmartTarget(*idx, L"C:\\dl\\project.zip", L"C:\\dl");
    CHECK(t.destination == L"C:\\dl");
    CHECK(!t.createdSubfolder);
}

TEST_CASE("smart extract: single file goes straight in") {
    auto idx = Make({L"report.pdf"});
    CHECK(ComputeSmartTarget(*idx, L"C:\\dl\\report.zip", L"C:\\dl\\").destination == L"C:\\dl");
}

TEST_CASE("smart extract: loose files get their own folder") {
    auto idx = Make({L"a.txt", L"b.txt", L"img/c.png"});
    auto t = ComputeSmartTarget(*idx, L"C:\\dl\\photos 2024.zip", L"C:\\dl");
    CHECK(t.destination == L"C:\\dl\\photos 2024");
    CHECK(t.createdSubfolder);
}

TEST_CASE("archive base names") {
    CHECK(ArchiveBaseName(L"C:\\x\\data.tar.gz") == L"data");
    CHECK(ArchiveBaseName(L"C:\\x\\Backup.TAR.XZ") == L"Backup");
    CHECK(ArchiveBaseName(L"C:\\x\\a.b.zip") == L"a.b");
    CHECK(ArchiveBaseName(L"C:\\x\\big.7z.001") == L"big");
    CHECK(ArchiveBaseName(L"C:\\x\\movie.part1.rar") == L"movie");
    CHECK(ArchiveBaseName(L"C:\\x\\مشروع.zip") == L"مشروع");
    CHECK(ArchiveBaseName(L"C:\\x\\noext") == L"noext");
}
