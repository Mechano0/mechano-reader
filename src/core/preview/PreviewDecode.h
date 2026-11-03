#pragma once
// Preview decoders: turn the bytes of ONE archive entry (already in RAM) into
// something the Preview pane can show. Pure data in / data out: no window, no
// disk, no network. Every input is untrusted.
#include <cstdint>
#include <string>
#include <vector>
#include "utils/Result.h"

namespace mzr::preview {

enum class Kind { None, Image, Text, Pdf, Video, Audio, Docx, Xlsx, Pptx, Dwg, Dxf, Revit, Ifc, Navisworks };

// Max size of a video / audio file that is previewed (it is extracted to the
// private temp folder first, because the media engine needs to seek).
constexpr uint64_t kMaxMediaBytes = 2048ull << 20;   // 2 GB

// Chosen by extension (the decoder still checks the content).
Kind KindForName(const std::wstring& name);

// Max bytes read from the archive for each kind.
constexpr uint64_t kMaxImageBytes = 64ull << 20;   // 64 MB compressed image file
constexpr uint64_t kMaxTextBytes  = 4ull << 20;    // first 4 MB of a text file
constexpr uint32_t kMaxImageSide  = 16384;         // refuse bigger images (decompression bomb)
constexpr uint32_t kDecodeSide    = 2560;          // keep at most this many pixels per side in RAM

struct Image {
    uint32_t width = 0, height = 0;                // decoded (possibly reduced) size
    uint32_t originalWidth = 0, originalHeight = 0;
    std::vector<uint8_t> pixels;                   // 32bpp premultiplied BGRA, stride = width * 4
};

// Decodes the first frame with Windows Imaging Component (built-in codecs only:
// PNG, JPEG, BMP, GIF, TIFF, ICO, JPEG XR, WebP/HEIC if Windows has them).
// EXIF orientation is applied so phone photos are upright.
Result<Image> DecodeImage(const std::vector<uint8_t>& data, uint32_t maxSide = kDecodeSide);

// UTF-8 / UTF-16 (BOM) / ANSI text -> UTF-16 with CRLF line ends.
// Returns an error for binary data (NUL bytes) so it is not shown as garbage.
Result<std::wstring> DecodeText(const std::vector<uint8_t>& data);

} // namespace mzr::preview
