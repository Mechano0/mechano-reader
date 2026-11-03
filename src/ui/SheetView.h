#pragma once
// Excel preview grid that looks like Excel: column widths, row heights, fills, fonts, borders,
// alignment, merged cells, text overflowing into empty neighbours, "####" for numbers that do
// not fit, right-to-left sheets (column A on the right). Only the visible cells are painted,
// so a 10 000-row sheet scrolls as fast as a small one. Read-only, no editing.
#include <windows.h>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include "core/preview/Ooxml.h"

namespace mzr::ui {

class SheetView {
public:
    ~SheetView();
    bool Create(HWND parent, HINSTANCE inst);
    HWND hwnd() const { return wnd_; }
    void SetSheet(std::shared_ptr<const preview::Sheet> sheet);   // nullptr = empty
    void SetZoom(double zoom);                                     // 1.0 = 100 %

private:
    static LRESULT CALLBACK ProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(UINT, WPARAM, LPARAM);
    void Measure();                      // column / row pixel positions for the current zoom
    void UpdateScrollBars();
    void ScrollBy(int dx, int dy);
    void Paint(HDC dc, const RECT& rc);
    HFONT FontFor(const preview::CellStyle& s);
    HFONT HeaderFont();
    void ClearFonts();
    int  Px(double v) const { return static_cast<int>(v * dpi_ / 96.0 * zoom_ + 0.5); }

    // Left edge on screen of the content x range [x, x+w) (mirrored for right-to-left sheets).
    int ScreenX(int x, int w, int clientW) const;
    int MergeAt(uint32_t r, uint32_t c) const;   // -1 none, else index in merges

    HWND wnd_ = nullptr;
    std::shared_ptr<const preview::Sheet> sheet_;
    double zoom_ = 1.0;
    int dpi_ = 96;
    std::vector<int> colX_, rowY_;       // prefix sums (size cols+1 / rows+1), pixels at the current zoom
    int hdrW_ = 0, hdrH_ = 0;            // row-number column width, column-letter row height
    int scrollX_ = 0, scrollY_ = 0;
    std::vector<int32_t> mergeIndex_;    // rows*cols when there are merges: index in merges or -1
    using FontKey = std::tuple<uint16_t, int, bool, bool, bool, bool>;
    std::map<FontKey, HFONT> fonts_;
    HFONT headerFont_ = nullptr;
};

} // namespace mzr::ui
