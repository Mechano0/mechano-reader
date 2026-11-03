// Navisworks info card: a synthetic NWD-like binary with a PNG thumbnail, a version text and source file
// paths (ASCII and UTF-16, Arabic), then hostile data (never a crash, a hang or a huge allocation).
#include <cstring>
#include "../mzr_test.h"
#include "core/preview/Navisworks.h"
#include "core/preview/PreviewDecode.h"

using namespace mzr;
using namespace mzr::preview;

namespace {

void Ascii(std::vector<uint8_t>& b, const char* s) { b.insert(b.end(), s, s + strlen(s)); b.push_back(0); }
void Wide(std::vector<uint8_t>& b, const std::wstring& s) {
    for (wchar_t c : s) { b.push_back(static_cast<uint8_t>(c & 255)); b.push_back(static_cast<uint8_t>(c >> 8)); }
    b.push_back(0); b.push_back(0);
}
void Be(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void Chunk(std::vector<uint8_t>& b, const char* type, const std::vector<uint8_t>& data) {
    Be(b, static_cast<uint32_t>(data.size()));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), data.begin(), data.end());
    Be(b, 0);                                                    // CRC is not checked by the scanner
}
std::vector<uint8_t> Png(uint32_t w, uint32_t h) {
    std::vector<uint8_t> p = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    Be(ihdr, w); Be(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    Chunk(p, "IHDR", ihdr);
    Chunk(p, "IDAT", std::vector<uint8_t>(100, 0x55));
    Chunk(p, "IEND", {});
    return p;
}

std::vector<uint8_t> Junk(size_t n, uint32_t seed) {
    std::vector<uint8_t> b(n);
    for (auto& c : b) { seed = seed * 1103515245u + 12345u; c = static_cast<uint8_t>(seed >> 24); }
    return b;
}

} // namespace

TEST_CASE("navisworks: thumbnail, version and referenced files") {
    CHECK(KindForName(L"Model.NWD") == Kind::Navisworks && KindForName(L"a.nwc") == Kind::Navisworks && KindForName(L"a.nwf") == Kind::Navisworks);
    std::vector<uint8_t> b = Junk(3000, 7);
    b.push_back(0);
    Ascii(b, "Autodesk Navisworks Manage 2023");
    b.insert(b.end(), {1, 2, 3});
    const auto small = Png(16, 16);                              // too small: an icon, not the thumbnail
    b.insert(b.end(), small.begin(), small.end());
    const auto png = Png(256, 160);
    b.insert(b.end(), png.begin(), png.end());
    Ascii(b, "C:\\Projects\\Villa\\Arch.rvt");
    Wide(b, L"D:\\\u0645\u0634\u0631\u0648\u0639\\MEP model.nwc");
    Ascii(b, "xx\\\\server\\share\\Site.dwg  C:\\Lib\\Steel.ifc");
    Ascii(b, "C:\\Projects\\Villa\\Arch.RVT");                    // same file again (case)
    Ascii(b, "nothing.rvtx and .rvt alone");                    // not files
    const auto tail = Junk(2000, 9);
    b.insert(b.end(), tail.begin(), tail.end());

    auto r = ReadNavisworksBytes(b, L"Villa.nwd");
    CHECK(r.ok());
    if (!r.ok()) return;
    const NavisInfo& i = r.value();
    CHECK(i.kind == NavisInfo::Kind::Nwd && i.size == b.size() && !i.partial);
    CHECK(i.versions.size() == 1 && i.versions[0] == L"Autodesk Navisworks Manage 2023");
    CHECK(i.thumbnailWidth == 256 && i.thumbnailHeight == 160 && !i.thumbnailJpeg && i.thumbnail.size() == png.size());
    auto has = [&](const std::wstring& f) { for (const auto& x : i.files) if (x == f) return true; return false; };
    CHECK(has(L"C:\\Projects\\Villa\\Arch.rvt"));
    CHECK(has(L"D:\\\u0645\u0634\u0631\u0648\u0639\\MEP model.nwc"));
    CHECK(has(L"\\\\server\\share\\Site.dwg"));
    CHECK(has(L"C:\\Lib\\Steel.ifc"));
    CHECK(i.files.size() == 4);

    const std::string rtf = NavisworksToRtf(i, L"Villa.nwd");
    CHECK(rtf.find("\\pngblip") != std::string::npos && rtf.find("Arch.rvt") != std::string::npos);
    CHECK(rtf.find("Source files referenced (4)") != std::string::npos);
    for (unsigned char c : rtf) if (c >= 0x80) { CHECK(false); break; }
    CHECK(NavisworksToRtf(ReadNavisworksBytes(Junk(5000, 3), L"x.nwf").value(), L"x.nwf").find("No readable details") != std::string::npos);
}

TEST_CASE("navisworks: hostile data never crashes or hangs") {
    CHECK(!ReadNavisworksBytes({}, L"a.nwd").ok());
    CHECK(!ReadNavisworksBytes({1, 2, 3}, L"a.nwd").ok());
    CHECK(ReadNavisworksBytes(Junk(1 << 20, 1), L"a.nwc").ok());
    // PNG chunk claiming 4 GB, PNG cut in the middle, endless JPEG markers, JPEG with a SOS and no end.
    std::vector<uint8_t> b = Junk(100, 2);
    std::vector<uint8_t> bad = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 'I', 'H', 'D', 'R'};
    bad.resize(bad.size() + 40, 0);
    b.insert(b.end(), bad.begin(), bad.end());
    auto cut = Png(300, 300);
    cut.resize(cut.size() - 20);
    b.insert(b.end(), cut.begin(), cut.end());
    for (int k = 0; k < 2000; ++k) b.insert(b.end(), {0xFF, 0xD8, 0xFF, 0xFF});
    b.insert(b.end(), {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02});
    b.resize(b.size() + 100000, 0x11);
    auto r = ReadNavisworksBytes(b, L"a.nwd");
    CHECK(r.ok() && r.value().thumbnail.empty());
    // One huge text run and thousands of distinct paths: bounded.
    std::vector<uint8_t> t(3u << 20, 'a');
    for (int k = 0; k < 5000; ++k) Ascii(t, ("C:\\f\\" + std::to_string(k) + ".dwg").c_str());
    auto r2 = ReadNavisworksBytes(t, L"a.nwf");
    CHECK(r2.ok() && r2.value().files.size() <= 300 && r2.value().filesTruncated);
    // Reader failure is an error, not a crash.
    auto r3 = ReadNavisworks([](uint64_t, void*, size_t) { return false; }, 1000, L"a.nwd");
    CHECK(!r3.ok());
    // A "big" file (claimed 1 GB): only the start and end are read.
    uint64_t asked = 0;
    auto r4 = ReadNavisworks([&](uint64_t, void* p, size_t n) { asked += n; memset(p, 0, n); return true; }, 1ull << 30, L"a.nwd");
    CHECK(r4.ok() && r4.value().partial && asked <= (24ull << 20) + 8);
}
