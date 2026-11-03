// Office preview: Word -> RTF, Excel -> grid, PowerPoint -> slide picture.
// The test files are built here (XML parts zipped with our own engine), so the
// tests do not depend on Microsoft Office being installed.
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/filesystem/FileSystem.h"
#include "core/preview/Ooxml.h"

using namespace mzr;
using namespace mzr::archive;
using namespace mzr::preview;
using mzr_test::TempDir;
using mzr_test::WriteFileBytes;

namespace {

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

using Parts = std::vector<std::pair<std::wstring, std::string>>;   // path inside the package -> content

// Writes the parts to <dir>\<name>\..., zips the top-level items and returns the zip bytes.
preview::Bytes MakePackage(TempDir& d, const std::wstring& name, const Parts& parts) {
    const std::wstring root = d / name;
    std::vector<std::wstring> top;
    for (const auto& [p, content] : parts) {
        WriteFileBytes(root + L"\\" + p, content);
        const std::wstring first = p.substr(0, p.find(L'\\'));
        const std::wstring full = root + L"\\" + first;
        if (std::find(top.begin(), top.end(), full) == top.end()) top.push_back(full);
    }
    CompressionSettings cs;
    CancelToken cancel;
    OperationReport rep;
    const std::wstring zip = d / (name + L".zip");
    CHECK(CreateArchive(zip, fs::CollectSources(top, L""), cs, nullptr, cancel, &rep).ok());
    std::ifstream f(zip, std::ios::binary);
    auto v = std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return v;
}

const char* kRootRels(const char* target) {
    static std::string s;
    s = std::string(R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
                    R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target=")") +
        target + R"("/></Relationships>)";
    return s.c_str();
}

constexpr const char* kW = R"(xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")";

} // namespace

TEST_CASE("office: column letters and Excel dates") {
    CHECK(ColumnIndex(L"A1") == 0);
    CHECK(ColumnIndex(L"Z9") == 25);
    CHECK(ColumnIndex(L"AA10") == 26);
    CHECK(ColumnIndex(L"") == -1);
    CHECK(ExcelDate(45000, false) == L"2023-03-15");
    CHECK(ExcelDate(1, false) == L"1900-01-01");
}

TEST_CASE("office: Word document becomes rich text") {
    ComInit com;
    TempDir d(L"office-docx");
    const std::string doc = std::string(R"(<?xml version="1.0" encoding="UTF-8"?><w:document )") + kW + R"(><w:body>)"
        R"(<w:p><w:r><w:t>Hello plain</w:t></w:r></w:p>)"
        R"(<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Bold words</w:t></w:r></w:p>)"
        R"(<w:p><w:pPr><w:bidi/><w:jc w:val="right"/></w:pPr><w:r><w:t>مرحبا</w:t></w:r></w:p>)"
        R"(<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Cell1</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Cell2</w:t></w:r></w:p></w:tc></w:tr></w:tbl>)"
        R"(</w:body></w:document>)";
    // An external picture link must be ignored (never fetched).
    const std::string docRels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="http://example.invalid/x.png" TargetMode="External"/>)"
        R"(</Relationships>)";
    auto bytes = MakePackage(d, L"doc", {{L"_rels\\.rels", kRootRels("word/document.xml")},
                                         {L"word\\document.xml", doc},
                                         {L"word\\_rels\\document.xml.rels", docRels}});
    auto rtf = DocxToRtf(bytes);
    CHECK(rtf.ok());
    if (!rtf.ok()) return;
    const std::string& s = rtf.value();
    CHECK(s.rfind("{\\rtf1", 0) == 0);
    CHECK(s.find("Hello plain") != std::string::npos);
    CHECK(s.find("Bold words") != std::string::npos);
    CHECK(s.find("\\b") != std::string::npos);
    CHECK(s.find("\\u1605?") != std::string::npos);      // م (U+0645) escaped
    CHECK(s.find("\\rtlpar") != std::string::npos);
    CHECK(s.find("\\cell") != std::string::npos);
    CHECK(s.find("example.invalid") == std::string::npos);
}

TEST_CASE("office: Word sample file (pictures, fit to width)") {
    // The real sample from 00-data (skipped if it is not there).
    ComInit com;
    // <project>\01-wip\tests\unit\test_office.cpp -> <project>\00-data\test-archives\sample-office.zip
    const std::wstring zip = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
                              L"00-data" / L"test-archives" / L"sample-office.zip").wstring();
    auto a = Archive::Open(zip, {}, nullptr);
    if (!a.ok()) return;
    CancelToken cancel;
    for (const auto& e : a.value()->Index()->entries()) {
        if (e.path.find(L".docx") == std::wstring::npos) continue;
        auto data = a.value()->ReadEntry(e.arcIndex, kMaxOfficeBytes, cancel);
        CHECK(data.ok());
        if (!data.ok()) return;
        auto rtf = DocxToRtf(std::make_shared<const std::vector<uint8_t>>(std::move(data.value())), 5000);
        CHECK(rtf.ok());
        if (!rtf.ok()) return;
        char dump[MAX_PATH];
        if (GetEnvironmentVariableA("MZR_DUMP_RTF", dump, MAX_PATH)) { std::ofstream(dump, std::ios::binary) << rtf.value(); }
        CHECK(rtf.value().find("\\pngblip") != std::string::npos);
        CHECK(rtf.value().find("\\picwgoal5000") != std::string::npos);   // 7200 twips wide -> fitted to 5000
        CHECK(rtf.value().find("\\cellx9000") == std::string::npos);      // 3 x 3000 table -> shrunk
    }
}

TEST_CASE("office: Excel sheet becomes a grid (strings, numbers, dates, percent)") {
    ComInit com;
    TempDir d(L"office-xlsx");
    const std::string ns = R"(xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships")";
    const std::string wb = R"(<?xml version="1.0" encoding="UTF-8"?><workbook )" + ns + R"(><sheets>)"
        R"(<sheet name="بيانات" sheetId="1" r:id="rId1"/><sheet name="Second" sheetId="2" r:id="rId2"/></sheets></workbook>)";
    const std::string wbRels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>)"
        R"(<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>)"
        R"(<Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)"
        R"(</Relationships>)";
    const std::string sst = R"(<?xml version="1.0" encoding="UTF-8"?><sst )" + ns + R"(><si><t>الاسم</t></si><si><r><t>Two </t></r><r><t>runs</t></r></si></sst>)";
    const std::string styles = R"(<?xml version="1.0" encoding="UTF-8"?><styleSheet )" + ns + R"(><cellXfs>)"
        R"(<xf numFmtId="0"/><xf numFmtId="14"/><xf numFmtId="9"/></cellXfs></styleSheet>)";
    const std::string sheet1 = R"(<?xml version="1.0" encoding="UTF-8"?><worksheet )" + ns + R"(><sheetData>)"
        R"(<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1"><v>42</v></c></row>)"
        R"(<row r="2"><c r="A2" s="1"><v>45000</v></c><c r="B2" s="2"><v>0.25</v></c></row>)"
        R"(<row r="3"><c r="C3" t="inlineStr"><is><t>inline</t></is></c><c r="D3" t="b"><v>1</v></c></row>)"
        R"(<row r="4"><c r="A4" t="s"><v>1</v></c><c r="B4"><v>0.30000000000000004</v></c></row>)"
        R"(</sheetData></worksheet>)";
    const std::string sheet2 = R"(<?xml version="1.0" encoding="UTF-8"?><worksheet )" + ns + R"(><sheetData><row r="1"><c r="A1"><v>7</v></c></row></sheetData></worksheet>)";
    auto bytes = MakePackage(d, L"book", {{L"_rels\\.rels", kRootRels("xl/workbook.xml")},
                                          {L"xl\\workbook.xml", wb},
                                          {L"xl\\_rels\\workbook.xml.rels", wbRels},
                                          {L"xl\\sharedStrings.xml", sst},
                                          {L"xl\\styles.xml", styles},
                                          {L"xl\\worksheets\\sheet1.xml", sheet1},
                                          {L"xl\\worksheets\\sheet2.xml", sheet2}});
    auto s = ReadXlsxSheet(bytes, 0);
    CHECK(s.ok());
    if (!s.ok()) return;
    const Sheet& g = s.value();
    CHECK(g.sheetNames.size() == 2 && g.sheetNames[0] == L"بيانات");
    CHECK(g.rows == 4 && g.cols == 4);
    if (g.rows != 4 || g.cols != 4) return;
    CHECK(g.At(0, 0) == L"الاسم");
    CHECK(g.At(0, 1) == L"42");
    // Built-in date format 14 follows the Windows short date (like Excel): only the parts are checked.
    CHECK(g.At(1, 0).find(L"2023") != std::wstring::npos && g.At(1, 0).find(L"15") != std::wstring::npos);
    CHECK(g.At(1, 1) == L"25%");
    CHECK(g.At(2, 2) == L"inline");
    CHECK(g.At(2, 3) == L"TRUE");
    CHECK(g.At(3, 0) == L"Two runs");
    CHECK(g.At(3, 1) == L"0.3");
    CHECK(g.At(2, 0).empty());

    auto s2 = ReadXlsxSheet(bytes, 1);
    CHECK(s2.ok() && s2.value().index == 1 && s2.value().rows == 1 && s2.value().At(0, 0) == L"7");
    auto s9 = ReadXlsxSheet(bytes, 99);                  // out of range: clamped to the last sheet
    CHECK(s9.ok() && s9.value().index == 1);
}

TEST_CASE("office: PowerPoint slide is drawn to a picture") {
    ComInit com;
    TempDir d(L"office-pptx");
    const std::string ns = R"(xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main")";
    const std::string pres = R"(<?xml version="1.0" encoding="UTF-8"?><p:presentation )" + ns +
        R"(><p:sldIdLst><p:sldId id="256" r:id="rId2"/><p:sldId id="257" r:id="rId3"/></p:sldIdLst><p:sldSz cx="12192000" cy="6858000"/></p:presentation>)";
    const std::string presRels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide1.xml"/>)"
        R"(<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide2.xml"/>)"
        R"(</Relationships>)";
    // A red rectangle filling the left half + a text box.
    const std::string slide = R"(<?xml version="1.0" encoding="UTF-8"?><p:sld )" + ns + R"(><p:cSld><p:spTree>)"
        R"(<p:sp><p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="6096000" cy="6858000"/></a:xfrm><a:prstGeom prst="rect"/>)"
        R"(<a:solidFill><a:srgbClr val="FF0000"/></a:solidFill></p:spPr></p:sp>)"
        R"(<p:sp><p:spPr><a:xfrm><a:off x="6500000" y="500000"/><a:ext cx="5000000" cy="1000000"/></a:xfrm></p:spPr>)"
        R"(<p:txBody><a:p><a:r><a:rPr sz="4000"/><a:t>Title مرحبا</a:t></a:r></a:p></p:txBody></p:sp>)"
        R"(</p:spTree></p:cSld></p:sld>)";
    const std::string slide2 = R"(<?xml version="1.0" encoding="UTF-8"?><p:sld )" + ns + R"(><p:cSld><p:spTree/></p:cSld></p:sld>)";
    auto bytes = MakePackage(d, L"deck", {{L"_rels\\.rels", kRootRels("ppt/presentation.xml")},
                                          {L"ppt\\presentation.xml", pres},
                                          {L"ppt\\_rels\\presentation.xml.rels", presRels},
                                          {L"ppt\\slides\\slide1.xml", slide},
                                          {L"ppt\\slides\\slide2.xml", slide2}});
    auto pg = RenderPptxSlide(bytes, 0, 800);
    CHECK(pg.ok());
    if (!pg.ok()) { fwprintf(stderr, L"  slide error: %ls | %ls\n", pg.error().friendly.c_str(), pg.error().details.c_str()); return; }
    const auto& img = pg.value().image;
    CHECK(pg.value().pageCount == 2 && pg.value().pageIndex == 0);
    CHECK(img.width == 800 && img.height == 450);
    CHECK(img.pixels.size() == static_cast<size_t>(img.width) * img.height * 4);
    if (img.pixels.size() != static_cast<size_t>(img.width) * img.height * 4) return;
    // Left half red (BGRA), right side mostly white with some dark text pixels.
    const size_t left = (static_cast<size_t>(200) * img.width + 100) * 4;
    CHECK(img.pixels[left + 2] > 200 && img.pixels[left + 1] < 60 && img.pixels[left] < 60);
    size_t dark = 0;
    for (uint32_t y = 0; y < 120; ++y)
        for (uint32_t x = 430; x < 800; ++x)
            if (img.pixels[(static_cast<size_t>(y) * img.width + x) * 4 + 1] < 128) ++dark;
    CHECK(dark > 50);

    auto p2 = RenderPptxSlide(bytes, 1, 400);
    CHECK(p2.ok() && p2.value().pageIndex == 1 && p2.value().image.width == 400);
}

TEST_CASE("office: damaged or wrong files are refused, not crashed on") {
    ComInit com;
    TempDir d(L"office-bad");
    auto junk = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{'P', 'K', 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK(!DocxToRtf(junk).ok());
    CHECK(!ReadXlsxSheet(junk, 0).ok());
    CHECK(!RenderPptxSlide(junk, 0, 400).ok());
    auto empty = std::make_shared<std::vector<uint8_t>>();
    CHECK(!DocxToRtf(empty).ok());
    CHECK(!DocxToRtf(nullptr).ok());
    // A real zip that is not an Office file.
    auto other = MakePackage(d, L"other", {{L"readme.txt", "hello"}});
    CHECK(!ReadXlsxSheet(other, 0).ok());
    CHECK(!RenderPptxSlide(other, 0, 400).ok());
    // Malformed XML with a DTD entity: refused or ignored, never expanded.
    const std::string bomb = R"(<?xml version="1.0"?><!DOCTYPE x [<!ENTITY a "AAAAAAAAAA"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;">]>)"
                             R"(<w:document )" + std::string(kW) + R"(><w:body><w:p><w:r><w:t>&b;</w:t></w:r></w:p></w:body></w:document>)";
    auto bp = MakePackage(d, L"bomb", {{L"_rels\\.rels", kRootRels("word/document.xml")}, {L"word\\document.xml", bomb}});
    auto r = DocxToRtf(bp);
    CHECK(!r.ok() || r.value().find("AAAAAAAAAA") == std::string::npos);
}

// Phase 7 (S1): a hostile file with very deep XML nesting used to overflow the stack and
// kill the app as soon as it was selected (preview runs automatically). Now refused.
TEST_CASE("office: deeply nested XML is refused, never a crash") {
    ComInit com;
    TempDir d(L"office-deep");
    auto nest = [](const char* open, const char* close, int n, const std::string& inner) {
        std::string s;
        s.reserve(static_cast<size_t>(n) * (strlen(open) + strlen(close)) + inner.size());
        for (int i = 0; i < n; ++i) s += open;
        s += inner;
        for (int i = 0; i < n; ++i) s += close;
        return s;
    };
    const std::string para = "<w:p><w:r><w:t>deep text</w:t></w:r></w:p>";
    // Word: 200 000 nested <w:customXml> (an 11 KB file did it before the fix).
    const std::string deepDoc = std::string(R"(<?xml version="1.0"?><w:document )") + kW + "><w:body>" +
                                nest("<w:customXml>", "</w:customXml>", 200000, para) + "</w:body></w:document>";
    auto deep = MakePackage(d, L"deepdoc", {{L"_rels\\.rels", kRootRels("word/document.xml")}, {L"word\\document.xml", deepDoc}});
    CHECK(!DocxToRtf(deep).ok());
    // Normal nesting (100 levels) still works.
    const std::string okDoc = std::string(R"(<?xml version="1.0"?><w:document )") + kW + "><w:body>" +
                              nest("<w:customXml>", "</w:customXml>", 100, para) + "</w:body></w:document>";
    auto fine = MakePackage(d, L"okdoc", {{L"_rels\\.rels", kRootRels("word/document.xml")}, {L"word\\document.xml", okDoc}});
    auto r = DocxToRtf(fine);
    CHECK(r.ok() && r.value().find("deep text") != std::string::npos);

    // PowerPoint: 100 000 nested groups.
    const std::string ns = R"(xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main")";
    const std::string pres = R"(<?xml version="1.0" encoding="UTF-8"?><p:presentation )" + ns +
        R"(><p:sldIdLst><p:sldId id="256" r:id="rId2"/></p:sldIdLst><p:sldSz cx="12192000" cy="6858000"/></p:presentation>)";
    const std::string presRels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide1.xml"/></Relationships>)";
    const std::string slide = R"(<?xml version="1.0" encoding="UTF-8"?><p:sld )" + ns + R"(><p:cSld><p:spTree>)" +
                              nest("<p:grpSp>", "</p:grpSp>", 100000, "") + R"(</p:spTree></p:cSld></p:sld>)";
    auto deck = MakePackage(d, L"deepdeck", {{L"_rels\\.rels", kRootRels("ppt/presentation.xml")},
                                             {L"ppt\\presentation.xml", pres},
                                             {L"ppt\\_rels\\presentation.xml.rels", presRels},
                                             {L"ppt\\slides\\slide1.xml", slide}});
    auto pg = RenderPptxSlide(deck, 0, 400);      // a blank slide or an error: both fine, no crash
    CHECK(pg.ok() || pg.error().code != ErrorCode::None);
}

// Excel look: number formats the way Excel shows them.
TEST_CASE("office: Excel number formats") {
    auto f = [](double v, int id, const char* code = "") { return FormatExcelNumber(v, id, code); };
    CHECK(f(1234567.891, 0) == L"1234567.891");
    CHECK(f(0.000012345, 0) == L"1.2345E-05");
    CHECK(f(123456789012345.0, 0) == L"1.23457E+14");
    CHECK(f(1234567.891, 4) == L"1,234,567.89");
    CHECK(f(1250, 3) == L"1,250");
    CHECK(f(-1500, 37) == L"(1,500)");
    CHECK(f(0.05, 9) == L"5%");
    CHECK(f(0.1234, 0, "0.0%") == L"12.3%");
    CHECK(f(0.000012345, 0, "0.00E+00") == L"1.23E-05");
    CHECK(f(1234567.891, 0, "\"$\"#,##0.00") == L"$1,234,567.89");
    CHECK(f(1234.5, 0, "[$\xE2\x82\xAC-2] #,##0.00") == L"€ 1,234.50");      // [$€-2]
    CHECK(f(45230.75, 15) == L"31-Oct-23");
    CHECK(f(45230.75, 20) == L"18:00");
    CHECK(f(45001, 0, "dd/mm/yyyy") == L"16/03/2023");
    CHECK(f(45001, 0, "yyyy-mm-dd hh:mm") == L"2023-03-16 00:00");
    CHECK(f(0.5, 0, "#.##") == L".5");
    CHECK(f(0, 0, "#,##0.00;-#,##0.00;\"-\"") == L"-");
    uint32_t color = 0;
    CHECK(FormatExcelNumber(-5000, 0, "#,##0.00;[Red]\\-#,##0.00", &color) == L"-5,000.00" && color == 0xFF0000);
    CHECK(FormatExcelNumber(5000, 0, "#,##0.00;[Red]\\-#,##0.00", &color) == L"5,000.00" && color == kNoColor);
    CHECK(f(1234567, 0, "#,##0,\"K\"") == L"1,235K");
    CHECK(f(-3, 0, "0") == L"-3");
    CHECK(f(1e300, 0, "#,##0").size() < 600);                                      // absurd: bounded, no crash
    CHECK(f(1e308, 0, std::string(100000, '0').c_str()).size() < 600);            // hostile format code: capped digits
    CHECK(f(45000, 0, ("[" + std::string(5000, 'h') + "]:mm").c_str()).size() < 100);
    CHECK(f(1.5, 0, ("0.0E+" + std::string(5000, '0')).c_str()).size() < 100);
}

TEST_CASE("office: Excel look (styles, theme colours, widths, merges, right-to-left)") {
    ComInit com;
    auto a = Archive::Open((std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
                            L"00-data" / L"test-archives" / L"sample-excel.zip").wstring(), {}, nullptr);
    if (!a.ok()) { std::printf("      (skipped: sample-excel.zip not found)\n"); return; }
    CancelToken cancel;
    for (const auto& e : a.value()->Index()->entries()) {
        if (e.path.find(L".xlsx") == std::wstring::npos) continue;
        auto data = a.value()->ReadEntry(e.arcIndex, kMaxOfficeBytes, cancel);
        CHECK(data.ok());
        if (!data.ok()) return;
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(data.value()));
        auto s1 = ReadXlsxSheet(bytes, 0);
        CHECK(s1.ok());
        if (!s1.ok()) return;
        const Sheet& s = s1.value();
        CHECK(s.rightToLeft && s.rows == 8 && s.cols == 6);
        CHECK(s.merges.size() == 2 && s.merges[0].c1 == 5);
        CHECK(std::abs(s.colWidth[0] - 30) < 0.01 && std::abs(s.rowHeight[0] - 30) < 0.01);
        auto st = [&](uint32_t r, uint32_t c) -> const CellStyle& { return s.styles[s.cellStyle[r * s.cols + c]]; };
        CHECK(st(0, 0).fill == 0x4472C4 && st(0, 0).bold && st(0, 0).color == 0xFFFFFF && std::abs(st(0, 0).size - 16) < 0.01);
        CHECK(st(1, 0).fill != kNoColor && st(1, 0).fill != 0x4472C4 && st(1, 0).hAlign == 2 && st(1, 0).left.style == 1);   // accent1 tint .8
        CHECK(s.At(2, 1) == L"1,250" && s.cellKind[2 * s.cols + 1] == 1);
        CHECK(s.At(2, 4) == L"16/03/2023" && s.At(2, 5) == L"5%");
        CHECK(s.At(5, 3) == L"-5,000.00" && s.cellColor[5 * s.cols + 3] == 0xFF0000);
        CHECK(st(6, 0).wrap && st(6, 0).vAlign == 0);
        CHECK(st(7, 3).top.style == 4 && st(7, 3).bottom.style == 2);
        auto s2 = ReadXlsxSheet(bytes, 1);
        CHECK(s2.ok());
        if (!s2.ok()) return;
        const Sheet& t = s2.value();
        CHECK(!t.rightToLeft && t.colWidth[2] == 0);                               // hidden column
        CHECK(t.styles[t.cellStyle[0]].fill == st(1, 0).fill);                      // same header style as sheet 1
        CHECK(t.At(1, 3) == L"$1,234,567.89" && t.At(4, 1) == L"(1,500)" && t.At(7, 0) == L"TRUE" && t.cellKind[7 * t.cols + 1] == 3);
        CHECK(t.fontNames.size() >= 2 && t.styles[t.cellStyle[5 * t.cols + 1]].indent == 2);
    }
}
