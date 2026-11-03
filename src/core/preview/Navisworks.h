#pragma once
// Navisworks files (.nwd published model, .nwc cache of one source file, .nwf working file that only
// lists appended files) WITHOUT Navisworks. The format is closed and undocumented, so this is a
// best-effort look: the file is scanned (whole file up to 24 MB, otherwise its first 16 MB and last
// 8 MB) for a saved thumbnail (PNG / JPEG, checked chunk by chunk) and for readable text (ASCII and
// UTF-16): the Navisworks version that wrote it and the source files it references (.rvt, .dwg,
// .ifc, .nwc ...). Every size, count and string is bounded; nothing is ever trusted or executed.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "utils/Result.h"

namespace mzr::preview {

constexpr uint64_t kMaxNavisBytes = 2ull << 30;     // bigger files are not previewed (a temp copy is made first)

struct NavisInfo {
    enum class Kind { Nwd, Nwc, Nwf } kind = Kind::Nwd;
    uint64_t size = 0;
    std::wstring container;                          // "Navisworks binary", "OLE compound file", "ZIP"
    std::vector<std::wstring> versions;              // texts that mention Navisworks ("Autodesk Navisworks 2023")
    std::vector<std::wstring> files;                 // referenced source / appended files (paths or names)
    std::vector<uint8_t> thumbnail;                  // PNG or JPEG bytes, empty if none
    bool thumbnailJpeg = false;
    uint32_t thumbnailWidth = 0, thumbnailHeight = 0;
    bool partial = false;                            // big file: only its start and end were scanned
    bool filesTruncated = false;
};

using NavisReadAt = std::function<bool(uint64_t offset, void* buffer, size_t bytes)>;

Result<NavisInfo> ReadNavisworks(NavisReadAt readAt, uint64_t fileSize, const std::wstring& fileName);
Result<NavisInfo> ReadNavisworksFile(const std::wstring& path, const std::wstring& fileName);
Result<NavisInfo> ReadNavisworksBytes(const std::vector<uint8_t>& data, const std::wstring& fileName);

// The info card as RTF for the read-only rich text view.
std::string NavisworksToRtf(const NavisInfo& info, const std::wstring& fileName);

} // namespace mzr::preview
