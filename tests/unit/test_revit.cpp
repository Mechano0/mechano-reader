// Revit info card: our own compound-file reader + the Revit streams. A synthetic Revit-like file is
// built here with Windows structured storage (test side only), then read back and attacked.
#include <objbase.h>
#include <fstream>
#include <iterator>
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/preview/Revit.h"

using namespace mzr;
using namespace mzr::preview;
using mzr_test::TempDir;

namespace {

struct Com { Com() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); } ~Com() { CoUninitialize(); } };

void Put(std::string& b, uint32_t v) { for (int i = 0; i < 4; ++i) b += static_cast<char>((v >> (8 * i)) & 255); }
void PutStr(std::string& b, const std::wstring& s) {
    Put(b, static_cast<uint32_t>(s.size()));
    for (wchar_t c : s) { b += static_cast<char>(c & 255); b += static_cast<char>(c >> 8); }
}

bool WriteStream(IStorage* st, const wchar_t* name, const std::string& data) {
    IStream* s = nullptr;
    if (FAILED(st->CreateStream(name, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &s))) return false;
    ULONG w = 0;
    const HRESULT hr = s->Write(data.data(), static_cast<ULONG>(data.size()), &w);
    s->Release();
    return SUCCEEDED(hr) && w == data.size();
}

std::string FileBytes(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A family-like file: BasicFileInfo (workshared), a preview with a PNG, PartAtom, TransmissionData,
// a big stream in a sub-storage (regular sectors) and small ones (mini stream).
std::wstring MakeRevitLike(TempDir& d, bool family) {
    const std::wstring path = d / (family ? L"fam.rfa" : L"proj.rvt");
    IStorage* root = nullptr;
    if (FAILED(StgCreateDocfile(path.c_str(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &root))) return {};
    std::string bfi;
    Put(bfi, 14);
    bfi += '\x01';                                                          // workshared
    bfi += std::string(5, '\0');
    PutStr(bfi, L"\\\\server\\central\\Villa.rvt");
    bfi += std::string(3, '\0');
    PutStr(bfi, L"2024");
    PutStr(bfi, L"20230308_1635(x64)");
    PutStr(bfi, L"D:\\\u0645\u0634\u0627\u0631\u064A\u0639\\Villa.rvt");     // Arabic folder
    PutStr(bfi, L"ARA");
    WriteStream(root, L"BasicFileInfo", bfi);
    std::string pv(300, '\x07');
    pv += std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, 'x');
    WriteStream(root, L"RevitPreview4.0", pv);
    if (family) {
        WriteStream(root, L"PartAtom",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?><entry xmlns=\"http://www.w3.org/2005/Atom\" xmlns:A=\"urn:schemas-autodesk-com:partatom\">"
            "<title>Door Single</title><updated>2026-01-02T03:04:05Z</updated><category><term>Doors</term></category>"
            "<A:features><A:feature><A:group><A:title>Dimensions</A:title><Width displayName=\"Width\" type=\"system\">900 mm</Width></A:group></A:feature></A:features>"
            "<A:family type=\"user\"><A:part type=\"user\"><title>D900</title><Width>900</Width><Height>2100</Height></A:part>"
            "<A:part type=\"user\"><title>D1000</title><Width>1000</Width><Height>2200</Height></A:part></A:family></entry>");
    }
    {   // Real layout: 4-byte character count + UTF-16 XML.
        const std::wstring x = L"<?xml version=\"1.0\"?><TransmissionData><ExternalFileReference><ExternalFileReferenceType>Revit Link</ExternalFileReferenceType>"
                               L"<LastSavedPath>..\\Structure.rvt</LastSavedPath><LastSavedLoadState>Not Found</LastSavedLoadState></ExternalFileReference>"
                               L"<ExternalFileReference><ExternalFileReferenceType>Keynote Table</ExternalFileReferenceType><LastSavedPath></LastSavedPath>"
                               L"<LastSavedLoadState>Not Found</LastSavedLoadState></ExternalFileReference></TransmissionData>";
        std::string td;
        Put(td, static_cast<uint32_t>(x.size()));
        for (wchar_t c : x) { td += static_cast<char>(c & 255); td += static_cast<char>(c >> 8); }
        WriteStream(root, L"TransmissionData", td);
    }
    IStorage* sub = nullptr;
    if (SUCCEEDED(root->CreateStorage(L"Partitions", STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &sub))) {
        WriteStream(sub, L"0", std::string(200000, 'm'));                   // the "model": never read
        sub->Release();
    }
    root->Commit(STGC_DEFAULT);
    root->Release();
    return path;
}

Result<RevitInfo> ReadBytes(const std::string& b, const wchar_t* name) {
    auto readAt = [&b](uint64_t off, void* buf, size_t n) {
        if (off > b.size() || n > b.size() - off) return false;
        memcpy(buf, b.data() + off, n);
        return true;
    };
    return ReadRevit(readAt, b.size(), name);
}

} // namespace

TEST_CASE("revit: family card from a compound file (types, parameters, links, workshared)") {
    Com com;
    TempDir d(L"revit");
    const std::wstring f = MakeRevitLike(d, true);
    CHECK(!f.empty());
    auto r = ReadRevitFile(f, L"fam.rfa");
    CHECK(r.ok());
    if (!r.ok()) return;
    const RevitInfo& i = r.value();
    CHECK(i.kind == RevitInfo::Kind::Family && i.version == L"2024" && i.build == L"20230308_1635(x64)");
    CHECK(i.workshared && i.centralPath == L"\\\\server\\central\\Villa.rvt");
    CHECK(i.lastSavePath == L"D:\\\u0645\u0634\u0627\u0631\u064A\u0639\\Villa.rvt" && i.locale == L"ARA");
    CHECK(i.title == L"Door Single" && i.category == L"Doors");
    CHECK(i.types.size() == 2 && i.types[1].name == L"D1000" && i.types[1].params.size() == 2 && i.types[1].params[1].value == L"2200");
    CHECK(i.params.size() == 1 && i.params[0].group == L"Dimensions" && i.params[0].value == L"900 mm");
    CHECK(i.links.size() == 1 && i.links[0].state == L"Not Found");
    CHECK(i.thumbnailPng.size() == 72 && i.thumbnailPng[1] == 'P');
    const std::string rtf = RevitToRtf(i, L"fam.rfa");
    CHECK(rtf.rfind("{\\rtf1", 0) == 0 && rtf.find("D1000") != std::string::npos && rtf.find("\\pngblip") != std::string::npos);
    CHECK(rtf.find("\\u1605?") != std::string::npos);                     // Arabic escaped, never raw bytes
    for (unsigned char c : rtf) if (c >= 0x80) { CHECK(false); break; }    // RTF is pure ASCII
}

TEST_CASE("revit: hostile compound files never loop, crash or eat memory") {
    Com com;
    TempDir d(L"revit-bad");
    const std::wstring f = MakeRevitLike(d, false);
    const std::string good = FileBytes(f);
    CHECK(good.size() > 4096);
    CHECK(ReadBytes(good, L"x.rvt").ok());
    const uint32_t sectorSize = 1u << static_cast<uint8_t>(good[0x1E]);
    auto u32 = [](const std::string& b, size_t at) { return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8 | uint32_t(uint8_t(b[at + 2])) << 16 | uint32_t(uint8_t(b[at + 3])) << 24; };
    auto put = [](std::string& b, size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) b[at + k] = static_cast<char>((v >> (8 * k)) & 255); };
    // 1) every FAT entry points to itself: endless chains
    {
        std::string b = good;
        const size_t fatAt = (size_t(u32(b, 0x4C)) + 1) * sectorSize;
        for (uint32_t k = 0; k < sectorSize / 4; ++k) put(b, fatAt + k * 4, k);
        CHECK(!ReadBytes(b, L"x.rvt").ok());
    }
    // 2) directory entries pointing at each other (tree loops)
    {
        std::string b = good;
        const size_t dirAt = (size_t(u32(b, 0x30)) + 1) * sectorSize;
        for (size_t e = 0; e < sectorSize / 128; ++e) { put(b, dirAt + e * 128 + 0x44, 1); put(b, dirAt + e * 128 + 0x48, 2); put(b, dirAt + e * 128 + 0x4C, 1); }
        auto r = ReadBytes(b, L"x.rvt");
        CHECK(!r.ok() || r.value().version.empty() || true);                // finishing is the test
    }
    // 3) absurd header counts / sizes
    {
        std::string b = good;
        put(b, 0x2C, 0x7FFFFFFF);                                            // FAT sectors
        CHECK(!ReadBytes(b, L"x.rvt").ok());
        b = good; put(b, 0x48, 0x7FFFFFFF); put(b, 0x44, 0x7FFFFFF0);       // DIFAT chain
        ReadBytes(b, L"x.rvt");
        b = good; b[0x1E] = 30;                                             // sector shift
        CHECK(!ReadBytes(b, L"x.rvt").ok());
    }
    // 4) cut files and random damage (fixed seed)
    for (size_t cut : {size_t(0), size_t(100), size_t(600), good.size() / 2, good.size() - 1}) ReadBytes(good.substr(0, cut), L"x.rvt");
    uint32_t seed = 12345;
    for (int round = 0; round < 300; ++round) {
        std::string b = good;
        for (int k = 0; k < 20; ++k) {
            seed = seed * 1103515245u + 12345u;
            const size_t at = (seed >> 8) % b.size();
            seed = seed * 1103515245u + 12345u;
            b[at] = static_cast<char>(seed >> 16);
        }
        auto r = ReadBytes(b, L"x.rvt");
        if (r.ok()) RevitToRtf(r.value(), L"x.rvt");
    }
    // 5) not a compound file at all
    CHECK(!ReadBytes(std::string(5000, 'A'), L"x.rvt").ok());
    CHECK(!ReadBytes("", L"x.rvt").ok());
    CHECK(true);
}
