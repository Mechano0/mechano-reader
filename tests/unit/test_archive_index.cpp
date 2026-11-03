#include "../mzr_test.h"
#include "core/archive/ArchiveIndex.h"

using namespace mzr::archive;

static EntryRecord File(uint32_t i, const wchar_t* path, uint64_t size) {
    EntryRecord e; e.arcIndex = i; e.path = path; e.size = size; e.packSize = size / 2; return e;
}
static EntryRecord Dir(uint32_t i, const wchar_t* path) {
    EntryRecord e; e.arcIndex = i; e.path = path; e.isDir = true; return e;
}

TEST_CASE("index: implied folders, totals and sorting") {
    std::vector<EntryRecord> v{
        File(0, L"docs/b.txt", 100), File(1, L"docs/a.txt", 50), File(2, L"readme.md", 10),
        File(3, L"images/2024/x.png", 1000), Dir(4, L"docs/"), File(5, L"Docs/c.txt", 5)};
    auto idx = ArchiveIndex::Build({}, std::move(v));
    const auto& root = idx->node(ArchiveIndex::kRoot);
    CHECK(root.children.size() == 3);                 // docs, images, readme.md (Docs merged with docs)
    CHECK(idx->node(root.children[0]).isDir);         // folders first
    CHECK(idx->node(root.children[2]).name == L"readme.md");
    CHECK(idx->totalFiles() == 5);
    CHECK(idx->totalSize() == 1165);

    const uint32_t docs = idx->FindFolder(L"docs");
    CHECK(docs != ArchiveIndex::kNoEntry);
    CHECK(idx->node(docs).fileCount == 3);
    CHECK(idx->node(docs).entry != ArchiveIndex::kNoEntry);          // real folder entry attached
    CHECK(idx->node(idx->node(docs).children[0]).name == L"a.txt");    // sorted by name
    CHECK(idx->FindFolder(L"images\\2024") != ArchiveIndex::kNoEntry);
    CHECK(idx->FindFolder(L"nope") == ArchiveIndex::kNoEntry);
    CHECK(idx->PathOf(idx->FindFolder(L"images\\2024")) == L"images\\2024");
}

TEST_CASE("index: collect indices of a folder recursively, sorted") {
    std::vector<EntryRecord> v{File(0, L"a/1.txt", 1), File(1, L"b/2.txt", 1), File(2, L"a/sub/3.txt", 1), Dir(3, L"a")};
    auto idx = ArchiveIndex::Build({}, std::move(v));
    auto got = idx->CollectArcIndices({idx->FindFolder(L"a")});
    CHECK(got.size() == 3);
    CHECK(got.size() == 3 && got[0] == 0 && got[1] == 2 && got[2] == 3);
}

TEST_CASE("index: arabic names and odd separators") {
    std::vector<EntryRecord> v{File(0, L"مشروع\\مخطط.dwg", 10), File(1, L"./x//y.txt", 1), File(2, L"/abs.txt", 1)};
    auto idx = ArchiveIndex::Build({}, std::move(v));
    CHECK(idx->FindFolder(L"مشروع") != ArchiveIndex::kNoEntry);
    CHECK(idx->FindFolder(L"x") != ArchiveIndex::kNoEntry);
    CHECK(idx->node(ArchiveIndex::kRoot).children.size() == 3);
}

TEST_CASE("index: 100,000 entries build quickly") {
    std::vector<EntryRecord> v;
    v.reserve(100000);
    for (uint32_t i = 0; i < 100000; ++i) {
        EntryRecord e;
        e.arcIndex = i;
        e.path = L"folder" + std::to_wstring(i % 100) + L"\\sub" + std::to_wstring(i % 7) + L"\\file" + std::to_wstring(i) + L".txt";
        e.size = i;
        v.push_back(std::move(e));
    }
    const ULONGLONG t0 = GetTickCount64();
    auto idx = ArchiveIndex::Build({}, std::move(v));
    const ULONGLONG ms = GetTickCount64() - t0;
    std::printf("      (100k entries indexed in %llu ms)\n", ms);
    CHECK(idx->totalFiles() == 100000);
    CHECK(ms < 3000);
}
