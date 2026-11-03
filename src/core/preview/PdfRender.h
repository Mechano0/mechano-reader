#pragma once
// PDF preview through the PDF renderer built into Windows (Windows.Data.Pdf):
// no JavaScript, no forms, no network, nothing extra to ship.
// Must be called on an MTA thread (the preview worker), never on the UI thread.
#include <cstdint>
#include <memory>
#include <vector>
#include "core/preview/PreviewDecode.h"
#include "utils/Result.h"

namespace mzr::preview {

constexpr uint64_t kMaxPdfBytes = 128ull << 20;    // 128 MB PDF file

struct PdfPage {
    uint32_t pageCount = 0;
    uint32_t pageIndex = 0;                         // 0-based
    Image image;                                    // rendered page, white background
};

// Renders one page so that its width is about `targetWidth` pixels.
Result<PdfPage> RenderPdfPage(const std::vector<uint8_t>& pdf, uint32_t pageIndex, uint32_t targetWidth);

} // namespace mzr::preview
