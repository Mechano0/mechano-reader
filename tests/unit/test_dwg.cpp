// DWG preview: the AutoCAD version and the saved preview picture, hostile input refused.
#include <objbase.h>
#include <cstring>
#include "../mzr_test.h"
#include "core/preview/Dwg.h"

using namespace mzr;
using namespace mzr::preview;

namespace {

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}

// 24-bit DIB (BITMAPINFOHEADER + pixels, no file header), w x h, one colour.
std::vector<uint8_t> MakeDib(int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b) {
    const uint32_t row = static_cast<uint32_t>((w * 3 + 3) & ~3);
    std::vector<uint8_t> v(40 + row * static_cast<uint32_t>(h), 0);
    Put32(v, 0, 40);
    Put32(v, 4, static_cast<uint32_t>(w));
    Put32(v, 8, static_cast<uint32_t>(h));
    v[12] = 1;                      // planes
    v[14] = 24;                     // bits per pixel
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            const size_t p = 40 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3;
            v[p] = b; v[p + 1] = g; v[p + 2] = r;
        }
    return v;
}

constexpr uint8_t kSentinel[16] = {0x1F, 0x25, 0x6D, 0x07, 0xD4, 0x36, 0x28, 0x28,
                                   0x9D, 0x57, 0xCA, 0x3F, 0x9D, 0x44, 0x10, 0x2B};

// Minimal DWG: version code, image seeker at 0x0D, image block with one BMP entry.
std::vector<uint8_t> MakeDwg(const char* code, const std::vector<uint8_t>& dib) {
    std::vector<uint8_t> v(0x100, 0);
    std::memcpy(v.data(), code, 6);
    const uint32_t block = 0x80;
    Put32(v, 0x0D, block);
    std::memcpy(v.data() + block, kSentinel, 16);
    const uint32_t entries = block + 21, dibAt = static_cast<uint32_t>(v.size());
    v[block + 20] = 1;              // one entry
    v[entries] = 2;                 // code 2 = BMP
    Put32(v, entries + 1, dibAt);
    Put32(v, entries + 5, static_cast<uint32_t>(dib.size()));
    v.insert(v.end(), dib.begin(), dib.end());
    return v;
}

} // namespace

TEST_CASE("dwg: kind by file name and version names") {
    CHECK(KindForName(L"plan.DWG") == Kind::Dwg);
    CHECK(KindForName(L"مخطط الدور الأرضي.dwg") == Kind::Dwg);
    CHECK(DwgVersionName("AC1032") == L"AutoCAD 2018 - 2025");
    CHECK(DwgVersionName("AC1015") == L"AutoCAD 2000 - 2002");
    CHECK(DwgVersionName("XX9999").empty());
}

TEST_CASE("dwg: saved BMP preview picture is decoded") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto r = ReadDwg(MakeDwg("AC1032", MakeDib(32, 24, 255, 0, 0)));
    CHECK(r.ok());
    if (r) {
        CHECK(r.value().code == "AC1032");
        CHECK(r.value().version == L"AutoCAD 2018 - 2025");
        CHECK(r.value().hasPicture);
        CHECK(r.value().picture.width == 32 && r.value().picture.height == 24);
        // BGRA: red pixel
        CHECK(r.value().picture.pixels.size() == 32u * 24u * 4u && r.value().picture.pixels[2] == 255 &&
              r.value().picture.pixels[0] == 0);
    }
    CoUninitialize();
}

TEST_CASE("dwg: no picture, old version and hostile input never crash") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Not a DWG at all.
    CHECK(!ReadDwg({}).ok());
    CHECK(!ReadDwg(std::vector<uint8_t>{'P', 'K', 3, 4, 0, 0, 0, 0}).ok());
    CHECK(!ReadDwg(std::vector<uint8_t>{'A', 'C', 'x', '0', '1', '2'}).ok());
    // R12: no image block, version still known.
    std::vector<uint8_t> r12(0x40, 0);
    std::memcpy(r12.data(), "AC1009", 6);
    auto a = ReadDwg(r12);
    CHECK(a.ok() && !a.value().hasPicture && a.value().version == L"AutoCAD R11 / R12");
    // Image seeker pointing outside outside the file, or at garbage.
    auto bad = MakeDwg("AC1027", MakeDib(8, 8, 0, 0, 255));
    auto outside = bad; Put32(outside, 0x0D, 0xFFFFFFF0u);
    CHECK(ReadDwg(outside).ok() && !ReadDwg(outside).value().hasPicture);
    auto garbage = bad; garbage[0x80] ^= 0xFF;          // broken sentinel
    CHECK(ReadDwg(garbage).ok() && !ReadDwg(garbage).value().hasPicture);
    auto huge = bad; Put32(huge, 0x80 + 21 + 5, 0x7FFFFFFFu);   // entry size larger than the file
    CHECK(ReadDwg(huge).ok() && !ReadDwg(huge).value().hasPicture);
    // Picture header claiming a giant image: refused by the image decoder limits.
    auto giant = MakeDib(8, 8, 0, 0, 0); Put32(giant, 4, 100000); Put32(giant, 8, 100000);
    CHECK(ReadDwg(MakeDwg("AC1032", giant)).ok() && !ReadDwg(MakeDwg("AC1032", giant)).value().hasPicture);
    // Cut in the middle of the picture (only the start of a big file was read).
    auto cut = MakeDwg("AC1032", MakeDib(64, 64, 1, 2, 3)); cut.resize(cut.size() / 2);
    CHECK(ReadDwg(cut).ok() && !ReadDwg(cut).value().hasPicture);
    // Every single-byte change of a valid file: no crash, no hang.
    const auto good = MakeDwg("AC1018", MakeDib(16, 16, 9, 9, 9));
    int ok = 0;
    for (size_t i = 0; i < good.size(); i += 3) {
        auto m = good; m[i] = static_cast<uint8_t>(m[i] + 0x5A);
        if (ReadDwg(m).ok()) ++ok;
    }
    CHECK(ok > 0);
    CoUninitialize();
}
