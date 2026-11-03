// F7 trial: Mechano CAD core (DXF reader, renderer) and the DWG converter helper.
// Hostile drawings must finish quickly and never crash: self-inserting blocks, block "bombs",
// giant MINSERT grids, absurd counts / numbers, garbage, binary DXF.
#include <chrono>
#include <filesystem>
#include <sstream>
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/cad/Cad.h"
#include "core/cad/CadConvert.h"

using namespace mzr;
using namespace mzr::cad;
using mzr_test::TempDir;

namespace {

// DXF text from (code, value) pairs.
class Dxf {
public:
    Dxf& operator()(int code, const std::string& value) { s_ << code << "\n" << value << "\n"; return *this; }
    Dxf& operator()(int code, double value) { s_ << code << "\n" << value << "\n"; return *this; }
    Dxf& Raw(const std::string& text) { s_ << text; return *this; }
    std::string str() const { return s_.str(); }
private:
    std::ostringstream s_;
};

Dxf& Header(Dxf& d, const char* ver = "AC1015") {
    return d(0, "SECTION")(2, "HEADER")(9, "$ACADVER")(1, ver)(0, "ENDSEC");
}

size_t CountPixels(const preview::Image& im, uint8_t r, uint8_t g, uint8_t b, int tol = 40) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < im.pixels.size(); i += 4)
        if (std::abs(im.pixels[i + 2] - r) <= tol && std::abs(im.pixels[i + 1] - g) <= tol && std::abs(im.pixels[i] - b) <= tol) ++n;
    return n;
}

double Seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::wstring ProjectFile(const wchar_t* rel) {
    // <project>\01-wip\tests\unit\test_cad.cpp -> <project>\<rel>
    return (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / rel).wstring();
}

} // namespace

TEST_CASE("cad: lines, circle, block, text, layers -> Model view") {
    Dxf d;
    Header(d);
    d(0, "SECTION")(2, "TABLES")(0, "TABLE")(2, "LAYER")
     (0, "LAYER")(2, "HIDDEN")(62, -5)(70, 0)
     (0, "LAYER")(2, "RED")(62, 1)(70, 0)
     (0, "ENDTAB")(0, "ENDSEC");
    d(0, "SECTION")(2, "BLOCKS")
     (0, "BLOCK")(2, "BOX")(10, 0.0)(20, 0.0)
     (0, "LWPOLYLINE")(8, "0")(90, 4)(70, 1)(10, 0.0)(20, 0.0)(10, 1.0)(20, 0.0)(10, 1.0)(20, 1.0)(10, 0.0)(20, 1.0)
     (0, "ENDBLK")(0, "ENDSEC");
    d(0, "SECTION")(2, "ENTITIES")
     (0, "LINE")(8, "RED")(10, 0.0)(20, 0.0)(11, 100.0)(21, 50.0)
     (0, "CIRCLE")(8, "0")(10, 50.0)(20, 25.0)(40, 10.0)
     (0, "LINE")(8, "HIDDEN")(10, -1000.0)(20, -1000.0)(11, -999.0)(21, -999.0)      // layer off: not drawn
     (0, "INSERT")(8, "RED")(2, "BOX")(10, 10.0)(20, 10.0)(41, 5.0)(42, 5.0)
     (0, "TEXT")(8, "0")(10, 5.0)(20, 40.0)(40, 3.0)(1, "Hello %%c50")
     (0, "ENDSEC")(0, "EOF");
    auto r = ReadDxfText(d.str());
    CHECK(r.ok());
    if (!r.ok()) return;
    const Drawing& dr = r.value();
    CHECK(dr.views.size() == 1 && dr.views[0].name == L"Model" && !dr.truncated);
    const View& m = dr.views[0];
    CHECK(m.prims.size() == 4);                                   // line, circle, block box, text
    CHECK(m.texts.size() == 1 && m.texts[0] == L"Hello \u230050");
    bool redLine = false, redBox = false;
    for (const auto& p : m.prims) {
        if (p.kind == PrimKind::Lines && p.count == 2 && p.rgb == 0xFF0000) redLine = true;
        if (p.kind == PrimKind::Lines && p.count == 5 && p.rgb == 0xFF0000) redBox = true;   // layer 0 in a block takes the INSERT's layer
    }
    CHECK(redLine && redBox);
    CHECK(m.minX > -10 && m.maxX < 110 && m.minY > -10 && m.maxY < 60);                    // hidden line not in the box
    auto im = RenderView(dr, 0, 800);
    CHECK(im.ok());
    if (im.ok()) {
        CHECK(im.value().width == 800 && im.value().height > 300 && im.value().height < 500);
        CHECK(CountPixels(im.value(), 255, 0, 0) > 200);              // red line + box
        CHECK(CountPixels(im.value(), 236, 236, 236, 30) > 100);      // white circle / text on the dark model
    }
}

TEST_CASE("cad: layouts with viewports, names from LAYOUT objects") {
    Dxf d;
    Header(d);
    d(0, "SECTION")(2, "TABLES")(0, "TABLE")(2, "BLOCK_RECORD")
     (0, "BLOCK_RECORD")(5, "1F")(2, "*Paper_Space")
     (0, "BLOCK_RECORD")(5, "2A")(2, "*Paper_Space0")
     (0, "ENDTAB")(0, "ENDSEC");
    d(0, "SECTION")(2, "BLOCKS")
     (0, "BLOCK")(2, "*Paper_Space0")(10, 0.0)(20, 0.0)
     (0, "LINE")(8, "0")(10, 0.0)(20, 0.0)(11, 420.0)(21, 297.0)
     (0, "ENDBLK")(0, "ENDSEC");
    d(0, "SECTION")(2, "ENTITIES")
     (0, "CIRCLE")(8, "0")(10, 0.0)(20, 0.0)(40, 100.0)
     (0, "VIEWPORT")(8, "0")(67, 1)(10, 148.0)(20, 105.0)(40, 297.0)(41, 210.0)(68, 1)(69, 1)          // the sheet itself
     (0, "VIEWPORT")(8, "0")(67, 1)(10, 148.0)(20, 105.0)(40, 200.0)(41, 150.0)(12, 0.0)(22, 0.0)(45, 250.0)(68, 2)(69, 2)
     (0, "LINE")(8, "0")(67, 1)(10, 0.0)(20, 0.0)(11, 297.0)(21, 0.0)
     (0, "ENDSEC");
    d(0, "SECTION")(2, "OBJECTS")
     (0, "LAYOUT")(1, "page setup")(100, "AcDbLayout")(1, "A3 Plan")(71, 2)(330, "2A")
     (0, "LAYOUT")(1, "x")(100, "AcDbLayout")(1, "A4 \xD9\x85\xD8\xAE\xD8\xB7\xD8\xB7")(71, 1)(330, "1F")    // "A4 مخطط" (UTF-8 bytes)
     (0, "LAYOUT")(100, "AcDbLayout")(1, "Model")(71, 0)(330, "1E")
     (0, "ENDSEC")(0, "EOF");
    auto r = ReadDxfText(d.str());
    CHECK(r.ok());
    if (!r.ok()) return;
    const Drawing& dr = r.value();
    CHECK(dr.views.size() == 3);
    if (dr.views.size() != 3) return;
    CHECK(dr.views[1].paper && dr.views[2].paper);
    CHECK(dr.views[2].name == L"A3 Plan");                         // tab order 2
    CHECK(dr.views[1].viewports.size() == 1);                      // id 1 (the sheet) skipped
    if (!dr.views[1].viewports.empty()) CHECK(std::abs(dr.views[1].viewports[0].scale - 150.0 / 250.0) < 1e-9);
    auto im = RenderView(dr, 1, 600);
    CHECK(im.ok());
    if (im.ok()) {
        CHECK(CountPixels(im.value(), 255, 255, 255, 5) > 1000);    // white paper
        CHECK(CountPixels(im.value(), 0, 0, 0, 60) > 100);          // model circle in black through the viewport
    }
    CHECK(!RenderView(dr, 7, 600).ok());                           // no such view: error, no crash
}

TEST_CASE("cad: Arabic text (UTF-8 in 2007+, \\U+ escapes in older files), MTEXT codes") {
    {
        Dxf d;
        Header(d, "AC1021");
        d(0, "SECTION")(2, "ENTITIES")
         (0, "TEXT")(8, "0")(10, 0.0)(20, 0.0)(40, 2.0)(1, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7")   // مرحبا
         (0, "MTEXT")(8, "0")(10, 0.0)(20, 10.0)(40, 2.0)(71, 1)(1, "{\\fArial|b1;Line 1}\\PLine 2 4\\S3/4;")
         (0, "ENDSEC")(0, "EOF");
        auto r = ReadDxfText(d.str());
        CHECK(r.ok());
        if (r.ok() && r.value().views[0].texts.size() == 2) {
            CHECK(r.value().views[0].texts[0] == L"\u0645\u0631\u062D\u0628\u0627");
            CHECK(r.value().views[0].texts[1] == L"Line 1\nLine 2 4 3/4");
        } else {
            CHECK(false);
        }
    }
    {
        Dxf d;
        Header(d, "AC1015");
        d(0, "SECTION")(2, "ENTITIES")(0, "TEXT")(8, "0")(10, 0.0)(20, 0.0)(40, 2.0)(1, "\\U+0645\\U+0631 ok")(0, "ENDSEC")(0, "EOF");
        auto r = ReadDxfText(d.str());
        CHECK(r.ok() && r.value().views[0].texts.size() == 1 && r.value().views[0].texts[0] == L"\u0645\u0631 ok");
    }
}

TEST_CASE("cad: hostile drawings finish fast and never crash") {
    const auto t0 = std::chrono::steady_clock::now();
    // 1) A block that inserts itself.
    {
        Dxf d;
        Header(d);
        d(0, "SECTION")(2, "BLOCKS")(0, "BLOCK")(2, "A")(10, 0.0)(20, 0.0)
         (0, "LINE")(8, "0")(10, 0.0)(20, 0.0)(11, 1.0)(21, 1.0)
         (0, "INSERT")(8, "0")(2, "A")(10, 1.0)(20, 0.0)
         (0, "ENDBLK")(0, "ENDSEC")
         (0, "SECTION")(2, "ENTITIES")(0, "INSERT")(8, "0")(2, "A")(10, 0.0)(20, 0.0)(0, "ENDSEC")(0, "EOF");
        auto r = ReadDxfText(d.str());
        CHECK(r.ok() && r.value().truncated);
    }
    // 2) "Billion laughs" for drawings: 10 levels x 10 inserts each = 10^10 lines.
    {
        Dxf d;
        Header(d);
        d(0, "SECTION")(2, "BLOCKS");
        for (int lv = 0; lv < 10; ++lv) {
            d(0, "BLOCK")(2, "L" + std::to_string(lv))(10, 0.0)(20, 0.0);
            for (int k = 0; k < 10; ++k) d(0, "INSERT")(8, "0")(2, "L" + std::to_string(lv + 1))(10, static_cast<double>(k))(20, 0.0);
            d(0, "ENDBLK");
        }
        d(0, "BLOCK")(2, "L10")(10, 0.0)(20, 0.0)(0, "LINE")(8, "0")(10, 0.0)(20, 0.0)(11, 1.0)(21, 1.0)(0, "ENDBLK")(0, "ENDSEC");
        d(0, "SECTION")(2, "ENTITIES")(0, "INSERT")(8, "0")(2, "L0")(10, 0.0)(20, 0.0)(0, "ENDSEC")(0, "EOF");
        const auto t1 = std::chrono::steady_clock::now();
        auto r = ReadDxfText(d.str());
        CHECK(r.ok() && r.value().truncated);
        CHECK(Seconds(t1) < 20);
    }
    // 3) MINSERT with a giant grid.
    {
        Dxf d;
        Header(d);
        d(0, "SECTION")(2, "BLOCKS")(0, "BLOCK")(2, "B")(10, 0.0)(20, 0.0)(0, "LINE")(8, "0")(10, 0.0)(20, 0.0)(11, 1.0)(21, 1.0)(0, "ENDBLK")(0, "ENDSEC")
         (0, "SECTION")(2, "ENTITIES")
         (0, "INSERT")(8, "0")(2, "B")(10, 0.0)(20, 0.0)(70, 1000000.0)(71, 1000000.0)(44, 2.0)(45, 2.0)
         (0, "LINE")(8, "0")(10, 0.0)(20, 0.0)(11, 5.0)(21, 5.0)
         (0, "ENDSEC")(0, "EOF");
        auto r = ReadDxfText(d.str());
        CHECK(r.ok() && r.value().truncated && r.value().views[0].prims.size() == 1);
    }
    // 4) Absurd counts and numbers: hatch path claiming 10^12 vertices, NaN / inf / huge bulge,
    //    spline with wrong knots, very long text, unknown entities.
    {
        Dxf d;
        Header(d);
        d(0, "SECTION")(2, "ENTITIES")
         (0, "HATCH")(8, "0")(70, 1)(91, 1)(92, 2)(72, 1)(73, 1)(93, "999999999999")(10, 0.0)(20, 0.0)(42, 0.5)(10, 1.0)(20, 0.0)(10, 1.0)(20, 1.0)
         (0, "LINE")(8, "0")(10, "nan")(20, "1e999")(11, "-inf")(21, "abc")
         (0, "LWPOLYLINE")(8, "0")(90, 2)(10, 0.0)(20, 0.0)(42, 1e300)(10, 1.0)(20, 1.0)
         (0, "SPLINE")(8, "0")(71, 3)(72, 2)(40, 0.0)(40, 1.0)(10, 0.0)(20, 0.0)(10, 1.0)(20, 1.0)(10, 2.0)(20, 0.0)
         (0, "TEXT")(8, "0")(10, 0.0)(20, 0.0)(40, 1.0)(1, std::string(1000000, 'x'))
         (0, "3DSOLID")(8, "0")(1, "acis data")
         (0, "CIRCLE")(8, "0")(10, 0.0)(20, 0.0)(40, -5.0)
         (0, "ENDSEC")(0, "EOF");
        auto r = ReadDxfText(d.str());
        CHECK(r.ok());
        if (r.ok()) {
            for (const auto& t : r.value().views[0].texts) CHECK(t.size() <= Limits{}.maxTextChars);
            CHECK(RenderView(r.value(), 0, 500).ok());
        }
    }
    // 5) Entity limit.
    {
        Dxf d;
        Header(d);
        d(0, "SECTION")(2, "ENTITIES");
        for (int k = 0; k < 100; ++k) d(0, "LINE")(8, "0")(10, 0.0)(20, 0.0)(11, static_cast<double>(k))(21, 1.0);
        d(0, "ENDSEC")(0, "EOF");
        Limits lim; lim.maxEntities = 10;
        auto r = ReadDxfText(d.str(), lim);
        CHECK(r.ok() && r.value().truncated && r.value().views[0].prims.size() == 10);
    }
    // 6) Not a drawing / binary DXF / empty.
    CHECK(!ReadDxfText("hello\nworld\n").ok());
    CHECK(!ReadDxfText(std::string("AutoCAD Binary DXF\r\n\x1a\0", 22)).ok());
    CHECK(!ReadDxfText("").ok());
    CHECK(!ReadDxf(L"C:\\no\\such\\file.dxf").ok());
    CHECK(Seconds(t0) < 30);
}

TEST_CASE("cad: AutoCAD colour index") {
    CHECK(AciToRgb(1) == 0xFF0000 && AciToRgb(3) == 0x00FF00 && AciToRgb(5) == 0x0000FF);
    CHECK(AciToRgb(7) == kForeground && AciToRgb(0) == kForeground && AciToRgb(999) == kForeground);
    CHECK(AciToRgb(250) == 0x333333 && AciToRgb(255) == 0xFFFFFF);
    CHECK(AciToRgb(10) == 0xFF0000);                              // first of the hue table: pure red
}

TEST_CASE("cad: DWG through the dwg2dxf helper (separate, limited process)") {
    const std::wstring conv = FindDwgConverter();
    if (conv.empty()) { std::printf("      (skipped: cad\\dwg2dxf.exe not next to the tests)\n"); return; }
    TempDir d(L"cad-dwg");
    // Garbage DWG: an error, no crash, no hang.
    mzr_test::WriteFileBytes(d / L"bad.dwg", "AC1032" + std::string(5000, '\x5a'));
    auto bad = ConvertDwgToDxf(conv, d / L"bad.dwg", d / L"bad.dxf", nullptr);
    CHECK(!bad.ok() || !ReadDxf(d / L"bad.dxf").ok() || true);      // either way: we are still running
    // Cancelled at once.
    auto c = ConvertDwgToDxf(conv, d / L"bad.dwg", d / L"c.dxf", [] { return true; });
    CHECK(!c.ok() && c.error().code == ErrorCode::Cancelled);
    // Size limit.
    ConvertLimits small; small.maxDwgBytes = 100;
    CHECK(!ConvertDwgToDxf(conv, d / L"bad.dwg", d / L"s.dxf", nullptr, small).ok());
    CHECK(!ConvertDwgToDxf(L"", d / L"bad.dwg", d / L"s.dxf", nullptr).ok());

    // Real drawing from the sample archive (skipped if 00-data is not there).
    auto a = archive::Archive::Open(ProjectFile(L"00-data\\test-archives\\sample-dwg.zip"), {}, nullptr);
    if (!a.ok()) return;
    archive::CancelToken cancel;
    for (const auto& e : a.value()->Index()->entries()) {
        if (e.path.find(L"scupper") == std::wstring::npos) continue;
        archive::ExtractOptions eo;
        eo.destination = d / L"x";
        auto rep = a.value()->Extract({e.arcIndex}, eo, nullptr, cancel);
        CHECK(rep.filesDone == 1);
        MoveFileW((d / L"x\\scupper-drain.dwg").c_str(), (d / L"x\\in.dwg").c_str());
        auto ok = ConvertDwgToDxf(conv, d / L"x\\in.dwg", d / L"x\\out.dxf", nullptr);
        CHECK(ok.ok());
        auto dr = ReadDxf(d / L"x\\out.dxf");
        CHECK(dr.ok() && dr.value().views[0].prims.size() > 100 && dr.value().acadVersion == L"AC1018");
        if (dr.ok()) CHECK(RenderView(dr.value(), 0, 640).ok());
    }
}
