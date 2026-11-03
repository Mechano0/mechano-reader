#pragma once
// Mechano CAD core (F7, trial): a small, reusable 2D drawing library.
//   - ReadDxf:      ASCII DXF -> Drawing (Model + every Layout), streamed from a file.
//   - RenderView:   one view (Model or a Layout with its viewports) -> picture.
//   - ConvertDwgToDxf (CadConvert.h): DWG -> DXF through LibreDWG's dwg2dxf.exe in its own,
//     limited process (GPL code never runs inside our process).
// Every input is untrusted: sizes, counts, block nesting and text lengths are bounded, and a
// drawing that hits a limit is shown partially (Drawing::truncated) instead of failing.
// No dependency on the archive engine or the UI, so other Mechano tools can reuse it.
#include <cstdint>
#include <string>
#include <vector>
#include "core/preview/PreviewDecode.h"   // preview::Image (BGRA picture)
#include "utils/Result.h"

namespace mzr::cad {

struct Pt { double x = 0, y = 0; };

// A colour after BYLAYER / BYBLOCK are resolved. kForeground = AutoCAD colour 7
// (white on the dark model background, black on paper).
constexpr uint32_t kForeground = 0xFF000000u;

enum class PrimKind : uint8_t { Lines, Fill, HatchFill, Text, Dot };

struct Prim {
    PrimKind kind = PrimKind::Lines;
    uint32_t rgb = kForeground;            // 0xRRGGBB or kForeground
    uint32_t first = 0, count = 0;         // points (Lines / Fill / HatchFill: count >= 2; Fill paths split by NaN)
    // Text
    uint32_t text = 0;                     // index in View::texts
    float height = 0;                      // drawing units (cap height)
    float rotation = 0;                    // radians, counter-clockwise
    float widthFactor = 1;
    uint8_t hAlign = 0;                    // 0 left, 1 center, 2 right
    uint8_t vAlign = 0;                    // 0 baseline, 1 bottom, 2 middle, 3 top
};

// A Layout viewport: shows the Model view, clipped to a paper rectangle.
struct Viewport {
    Pt     paperCenter;                    // paper units
    double paperWidth = 0, paperHeight = 0;
    Pt     modelCenter;                    // model point at the viewport centre
    double scale = 1;                      // paper units per model unit
};

struct View {
    std::wstring name;                     // "Model", "Layout1", "A1 - Plan" ...
    bool paper = false;                    // a Layout (white paper) or the Model
    std::vector<Prim> prims;
    std::vector<Pt> pts;
    std::vector<std::wstring> texts;
    std::vector<Viewport> viewports;       // Layouts only
    double minX = 0, minY = 0, maxX = 0, maxY = 0;   // what to show (Model: outliers trimmed)
    bool empty() const { return prims.empty() && viewports.empty(); }
};

struct Drawing {
    std::vector<View> views;               // [0] = Model, then Layouts in tab order
    std::wstring acadVersion;              // "AC1032"
    uint64_t entities = 0;                 // entities read
    bool truncated = false;                // a limit was hit: part of the drawing is not shown
};

struct Limits {
    uint64_t maxFileBytes   = 1ull << 30;  // DXF file size
    uint64_t maxEntities    = 5'000'000;
    uint64_t maxPoints      = 6'000'000;   // after blocks are expanded (all views)
    uint32_t maxBlockDepth  = 16;          // INSERT inside INSERT ...
    uint64_t maxInsertCells = 10'000;      // MINSERT rows * columns
    size_t   maxTextChars   = 4096;
};

// Reads an ASCII DXF file (R12 ... 2018). Binary DXF is refused with a clear message.
Result<Drawing> ReadDxf(const std::wstring& path, const Limits& limits = {});
// Same, from text already in RAM (tests, small files).
Result<Drawing> ReadDxfText(const std::string& text, const Limits& limits = {});

// Draws view `index` so that the picture is about `targetWidth` pixels wide (the longer side
// is kept within preview::kDecodeSide). Must run on a worker thread.
Result<preview::Image> RenderView(const Drawing& d, uint32_t index, uint32_t targetWidth);

// AutoCAD colour index (1..255) -> 0xRRGGBB. 7 -> kForeground.
uint32_t AciToRgb(int aci);

} // namespace mzr::cad
