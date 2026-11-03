#include "core/preview/Dwg.h"

#include <cstring>

namespace mzr::preview {

namespace {

// Start sentinel of the DWG "image data" block (Open Design Specification for .dwg, chapter "Image data").
constexpr uint8_t kImageSentinel[16] = {0x1F, 0x25, 0x6D, 0x07, 0xD4, 0x36, 0x28, 0x28,
                                        0x9D, 0x57, 0xCA, 0x3F, 0x9D, 0x44, 0x10, 0x2B};

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

ErrorInfo NotDwg(const wchar_t* why) {
    return ErrorInfo{ErrorCode::Corrupted, L"This is not a valid AutoCAD drawing (DWG).", why};
}

// The BMP entry is a DIB (BITMAPINFOHEADER + palette + pixels) without the 14-byte file
// header. Add the file header so the normal (WIC) image decoder can read it.
std::vector<uint8_t> DibToBmp(const uint8_t* dib, uint32_t size) {
    if (size < 40) return {};
    const uint32_t hdr = Le32(dib);
    if (hdr < 40 || hdr > size) return {};
    const uint16_t bits = static_cast<uint16_t>(dib[14] | (dib[15] << 8));
    const uint32_t compression = Le32(dib + 16);
    uint32_t colors = Le32(dib + 32);
    if (colors == 0 && bits && bits <= 8) colors = 1u << bits;
    if (colors > 256) return {};
    uint64_t offBits = 14ull + hdr + 4ull * colors;
    if (compression == 3 /*BI_BITFIELDS*/ && hdr == 40) offBits += 12;
    if (offBits - 14 > size) return {};
    std::vector<uint8_t> bmp(14 + static_cast<size_t>(size));
    bmp[0] = 'B';
    bmp[1] = 'M';
    const uint32_t total = static_cast<uint32_t>(bmp.size()), off = static_cast<uint32_t>(offBits);
    for (int i = 0; i < 4; ++i) {
        bmp[2 + i] = static_cast<uint8_t>(total >> (8 * i));
        bmp[10 + i] = static_cast<uint8_t>(off >> (8 * i));
    }
    std::memcpy(bmp.data() + 14, dib, size);
    return bmp;
}

} // namespace

std::wstring DwgVersionName(const std::string& code) {
    static const struct { const char* code; const wchar_t* name; } kVersions[] = {
        {"AC1012", L"AutoCAD R13"},          {"AC1014", L"AutoCAD R14"},
        {"AC1015", L"AutoCAD 2000 - 2002"},  {"AC1018", L"AutoCAD 2004 - 2006"},
        {"AC1021", L"AutoCAD 2007 - 2009"},  {"AC1024", L"AutoCAD 2010 - 2012"},
        {"AC1027", L"AutoCAD 2013 - 2017"},  {"AC1032", L"AutoCAD 2018 - 2025"},
        {"AC1009", L"AutoCAD R11 / R12"},    {"AC1006", L"AutoCAD R10"},
        {"AC1004", L"AutoCAD R9"},           {"AC1003", L"AutoCAD 2.6"},
    };
    for (const auto& v : kVersions)
        if (code == v.code) return v.name;
    return {};
}

Result<DwgInfo> ReadDwg(const std::vector<uint8_t>& data) {
    if (data.size() < 6 || std::memcmp(data.data(), "AC", 2) != 0) return NotDwg(L"no AC10xx signature");
    for (int i = 2; i < 6; ++i)
        if (data[static_cast<size_t>(i)] < '0' || data[static_cast<size_t>(i)] > '9') return NotDwg(L"bad version code");

    DwgInfo info;
    info.code.assign(reinterpret_cast<const char*>(data.data()), 6);
    info.version = DwgVersionName(info.code);

    // R13 and later: offset 0x0D holds the file address of the image data block.
    if (info.code < "AC1012" || data.size() < 0x11) return info;
    const uint64_t at = Le32(data.data() + 0x0D);
    // sentinel (16) + overall size (4) + entry count (1)
    if (at == 0 || at + 21 > data.size() || std::memcmp(data.data() + at, kImageSentinel, 16) != 0) return info;
    const uint32_t count = data[static_cast<size_t>(at + 20)];
    uint64_t p = at + 21;
    const uint8_t* png = nullptr; uint32_t pngSize = 0;
    const uint8_t* dib = nullptr; uint32_t dibSize = 0;
    for (uint32_t i = 0; i < count && i < 16; ++i, p += 9) {
        if (p + 9 > data.size()) break;
        const uint8_t code = data[static_cast<size_t>(p)];
        const uint64_t start = Le32(data.data() + p + 1), size = Le32(data.data() + p + 5);
        if (size == 0 || start + size > data.size()) continue;         // outside what was read: ignore
        if (code == 6) { png = data.data() + start; pngSize = static_cast<uint32_t>(size); }
        else if (code == 2) { dib = data.data() + start; dibSize = static_cast<uint32_t>(size); }
        // code 3 = WMF: not decoded (old metafile format, not worth the risk for a thumbnail)
    }
    // Small picture: decode at its own size (it is drawn scaled in the pane).
    if (png) {
        auto im = DecodeImage(std::vector<uint8_t>(png, png + pngSize));
        if (im) { info.picture = std::move(im.value()); info.hasPicture = true; return info; }
    }
    if (dib) {
        const auto bmp = DibToBmp(dib, dibSize);
        if (!bmp.empty()) {
            auto im = DecodeImage(bmp);
            if (im) { info.picture = std::move(im.value()); info.hasPicture = true; }
        }
    }
    return info;
}

} // namespace mzr::preview
