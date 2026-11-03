#pragma once
// DWG (AutoCAD drawing) preview: the small picture AutoCAD saves inside every
// DWG (R13 and later) plus the AutoCAD version. The drawing itself is NOT
// parsed (no free, license-compatible DWG engine), so nothing is executed and
// only a few bounded fields are read. Every input is untrusted.
#include <cstdint>
#include <string>
#include <vector>
#include "core/preview/PreviewDecode.h"
#include "utils/Result.h"

namespace mzr::preview {

// The preview picture is near the start of the file; only this much is read from the archive.
constexpr uint64_t kMaxDwgReadBytes = 16ull << 20;

struct DwgInfo {
    std::string  code;         // "AC1032"
    std::wstring version;      // "AutoCAD 2018 - 2025"; empty if the code is unknown
    bool hasPicture = false;   // a BMP or PNG preview was found and decoded
    Image picture;
};

// "AC1032" -> "AutoCAD 2018 - 2025". Empty for anything else.
std::wstring DwgVersionName(const std::string& code);

// Error only when the data is not a DWG at all. A DWG without a usable preview
// picture (R12 and older, WMF-only, damaged) returns hasPicture = false.
Result<DwgInfo> ReadDwg(const std::vector<uint8_t>& data);

} // namespace mzr::preview
