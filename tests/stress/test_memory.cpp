// Memory of a big archive, split into "7z.dll handler" and "our index".
// Runs only when MZR_BENCH_ZIP points to a big archive (benchmarks\bench.ps1 makes one).
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/archive/ArchiveIndex.h"

using namespace mzr;
using namespace mzr::archive;

namespace {
double PrivateMb() {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
    return pm.PrivateUsage / 1048576.0;
}
size_t StrBytes(const std::wstring& s) { return s.capacity() > 7 ? (s.capacity() + 1) * 2 + 16 : 0; }   // heap part
} // namespace

TEST_CASE("stress: memory of a big archive (only with MZR_BENCH_ZIP)") {
    wchar_t path[1024];
    if (!GetEnvironmentVariableW(L"MZR_BENCH_ZIP", path, 1024)) { CHECK(true); return; }
    LoadEngine();
    const double before = PrivateMb();
    auto arc = Archive::Open(path, {}, nullptr);
    CHECK(arc.ok());
    if (!arc) return;
    const double after = PrivateMb();
    auto idx = arc.value()->Index();
    size_t eb = idx->entries().capacity() * sizeof(EntryRecord), nb = idx->nodes().capacity() * sizeof(ArchiveIndex::Node);
    for (const auto& e : idx->entries()) eb += StrBytes(e.path);
    for (const auto& n : idx->nodes()) nb += n.children.capacity() * 4;
    std::printf("      entries=%zu  private: +%.1f MB total | index: entries %.1f MB (%zu B/rec), nodes %.1f MB (%zu B/node) | rest (7z.dll + heap) %.1f MB\n",
                idx->entries().size(), after - before, eb / 1048576.0, sizeof(EntryRecord), nb / 1048576.0,
                sizeof(ArchiveIndex::Node), after - before - (eb + nb) / 1048576.0);
}
