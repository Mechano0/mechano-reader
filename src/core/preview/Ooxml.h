#pragma once
// Office Open XML preview (DOCX / XLSX / PPTX) WITHOUT Microsoft Office.
//
// An Office file is a ZIP: it is opened in RAM with the same 7z.dll engine,
// and its XML parts are read with pugixml. Treated as data only:
//   - no macros (VBA is never read), no OLE objects, no ActiveX
//   - external links / images (TargetMode="External") are never fetched
//   - every part is read with a byte limit (an Office "zip bomb" stops early)
//   - pugixml does not process DTDs or external entities (no XXE / billion laughs)
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/preview/PdfRender.h"     // PdfPage: page/slide image + count
#include "utils/Result.h"

namespace mzr::preview {

constexpr uint64_t kMaxOfficeBytes = 128ull << 20;   // the .docx/.xlsx/.pptx file itself
constexpr uint64_t kMaxXmlPart     = 48ull << 20;    // one XML part, uncompressed
constexpr uint64_t kMaxMediaPart   = 24ull << 20;    // one embedded picture
constexpr uint32_t kMaxSheetRows   = 10000;
constexpr uint32_t kMaxSheetCols   = 100;

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

// Word: rich text (RTF built by us: paragraphs, headings, bold/italic/underline,
// colours, alignment, right-to-left, lists, tables, PNG/JPEG pictures; EMF/WMF show as "[picture]").
// Tables and pictures wider than maxWidthTwips are shrunk to fit the view.
Result<std::string> DocxToRtf(const Bytes& file, uint32_t maxWidthTwips = 9360);

// Excel: one worksheet as display strings + the look Excel gives them.
constexpr uint32_t kNoColor = 0xFFFFFFFFu;          // colours below are 0x00RRGGBB

struct CellBorder {
    uint8_t style = 0;                    // 0 none, 1 thin/hair/dotted/dashed, 2 medium, 3 thick, 4 double
    uint32_t color = 0x000000;
};

struct CellStyle {
    uint32_t fill = kNoColor;             // background
    uint32_t color = kNoColor;            // text (kNoColor = automatic black)
    uint16_t font = 0;                    // index in Sheet::fontNames
    float    size = 11;                   // points
    bool bold = false, italic = false, underline = false, strike = false;
    uint8_t  hAlign = 0;                  // 0 general, 1 left, 2 center, 3 right, 4 fill, 5 justify, 6 center across
    uint8_t  vAlign = 2;                  // 0 top, 1 center, 2 bottom
    bool     wrap = false;
    uint8_t  indent = 0;
    CellBorder left, right, top, bottom;
};

struct CellMerge { uint32_t r0 = 0, c0 = 0, r1 = 0, c1 = 0; };   // inclusive, 0-based

struct Sheet {
    std::vector<std::wstring> sheetNames;
    uint32_t index = 0;
    uint32_t rows = 0, cols = 0;
    std::vector<std::wstring> cells;      // rows * cols, row-major
    bool truncated = false;               // more rows / columns than shown
    std::wstring note;                    // e.g. "This is a chart sheet."
    const std::wstring& At(uint32_t r, uint32_t c) const { return cells[static_cast<size_t>(r) * cols + c]; }

    // Look (Excel's own rules: General numbers right-aligned, text left, ...).
    std::vector<uint16_t> cellStyle;      // rows * cols: index in styles (0 = default)
    std::vector<uint8_t>  cellKind;       // rows * cols: 0 empty/text, 1 number, 2 boolean, 3 error
    std::vector<uint32_t> cellColor;      // rows * cols: colour from the number format ([Red]), kNoColor = none
    std::vector<CellStyle> styles;        // [0] = workbook default
    std::vector<std::wstring> fontNames;  // [0] = default font ("Calibri")
    std::vector<float> colWidth;          // cols: Excel character units (0 = hidden)
    std::vector<float> rowHeight;         // rows: points (0 = hidden)
    std::vector<CellMerge> merges;
    bool rightToLeft = false;             // Arabic sheets: column A on the right
    bool showGridLines = true;
    float defaultColWidth = 8.43f;        // characters
    float defaultRowHeight = 15.0f;       // points
};
Result<Sheet> ReadXlsxSheet(const Bytes& file, uint32_t sheetIndex);

// PowerPoint: one slide drawn to a picture (text boxes, shapes, pictures, tables).
Result<PdfPage> RenderPptxSlide(const Bytes& file, uint32_t slideIndex, uint32_t targetWidth);

// Excel serial date -> "2026-10-29" / "2026-10-29 14:05" (1900 date system).
std::wstring ExcelDate(double serial, bool withTime);
// Excel number format (built-in id or format code) applied to a number, the way Excel shows it:
// sections (pos;neg;zero), 0 # , . %, E+00, literal text, [Red] colours (returned in *color),
// dates / times (yyyy mm dd hh mm ss AM/PM). General = up to 11 significant digits.
std::wstring FormatExcelNumber(double v, int fmtId, const std::string& code, uint32_t* color = nullptr);
// Column letters -> 0-based index ("A" = 0, "AA" = 26). -1 if invalid.
int ColumnIndex(const std::wstring& ref);

} // namespace mzr::preview
