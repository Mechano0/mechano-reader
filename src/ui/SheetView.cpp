#include "ui/SheetView.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace mzr::ui {
namespace {

constexpr wchar_t kClass[] = L"MechanoZipReader.Sheet";
constexpr COLORREF kGrid = RGB(218, 220, 224);
constexpr COLORREF kHeaderBg = RGB(240, 241, 243);
constexpr COLORREF kHeaderLine = RGB(200, 202, 206);
constexpr COLORREF kHeaderText = RGB(80, 80, 80);

COLORREF Rgb(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

bool HasRtlText(const std::wstring& s) {
    for (wchar_t ch : s) if ((ch >= 0x0590 && ch <= 0x08FF) || (ch >= 0xFB1D && ch <= 0xFEFC)) return true;
    return false;
}

std::wstring ColumnName(uint32_t c) {
    std::wstring n;
    for (uint32_t x = c + 1; x > 0; x = (x - 1) / 26) n.insert(n.begin(), static_cast<wchar_t>(L'A' + (x - 1) % 26));
    return n;
}

void Line(HDC dc, int x0, int y0, int x1, int y1) { MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1); }

} // namespace

SheetView::~SheetView() { ClearFonts(); }

bool SheetView::Create(HWND parent, HINSTANCE inst) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ProcStatic;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    wnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_HSCROLL | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, inst, this);
    if (!wnd_) return false;
    dpi_ = static_cast<int>(GetDpiForWindow(wnd_));
    if (dpi_ <= 0) dpi_ = 96;
    return true;
}

void SheetView::ClearFonts() {
    for (auto& [k, f] : fonts_) DeleteObject(f);
    fonts_.clear();
    if (headerFont_) { DeleteObject(headerFont_); headerFont_ = nullptr; }
}

void SheetView::SetSheet(std::shared_ptr<const preview::Sheet> sheet) {
    sheet_ = std::move(sheet);
    scrollX_ = scrollY_ = 0;
    mergeIndex_.clear();
    if (sheet_ && !sheet_->merges.empty() && sheet_->rows && sheet_->cols) {
        mergeIndex_.assign(static_cast<size_t>(sheet_->rows) * sheet_->cols, -1);
        for (size_t i = 0; i < sheet_->merges.size(); ++i) {
            const auto& m = sheet_->merges[i];
            for (uint32_t r = m.r0; r <= m.r1 && r < sheet_->rows; ++r)
                for (uint32_t c = m.c0; c <= m.c1 && c < sheet_->cols; ++c)
                    mergeIndex_[static_cast<size_t>(r) * sheet_->cols + c] = static_cast<int32_t>(i);
        }
    }
    Measure();
    UpdateScrollBars();
    if (wnd_) InvalidateRect(wnd_, nullptr, FALSE);
}

void SheetView::SetZoom(double zoom) {
    zoom = std::clamp(zoom, 0.25, 8.0);
    if (std::abs(zoom - zoom_) < 1e-9) return;
    const double k = zoom / zoom_;                               // keep the same cells in view
    zoom_ = zoom;
    ClearFonts();
    scrollX_ = static_cast<int>(scrollX_ * k);
    scrollY_ = static_cast<int>(scrollY_ * k);
    Measure();
    UpdateScrollBars();
    if (wnd_) InvalidateRect(wnd_, nullptr, FALSE);
}

void SheetView::Measure() {
    colX_.assign(1, 0);
    rowY_.assign(1, 0);
    hdrH_ = Px(20);
    hdrW_ = Px(36);
    if (!sheet_) return;
    const auto& s = *sheet_;
    for (uint32_t c = 0; c < s.cols; ++c) {
        const float w = c < s.colWidth.size() ? s.colWidth[c] : s.defaultColWidth;
        // Excel: pixels = width * maximum digit width (7 px for Calibri 11) + 5 px padding.
        colX_.push_back(colX_.back() + (w <= 0 ? 0 : std::max(1, Px(w * 7.0 + 5.0))));
    }
    for (uint32_t r = 0; r < s.rows; ++r) {
        const float h = r < s.rowHeight.size() ? s.rowHeight[r] : s.defaultRowHeight;
        rowY_.push_back(rowY_.back() + (h <= 0 ? 0 : std::max(1, Px(h * 96.0 / 72.0))));
    }
    const int digits = static_cast<int>(std::to_wstring(std::max<uint32_t>(1, s.rows)).size());
    hdrW_ = Px(std::max(30, 10 + digits * 8));
}

void SheetView::UpdateScrollBars() {
    if (!wnd_) return;
    RECT rc;
    GetClientRect(wnd_, &rc);
    const int viewW = std::max(1, static_cast<int>(rc.right) - hdrW_), viewH = std::max(1, static_cast<int>(rc.bottom) - hdrH_);
    const int totalW = colX_.empty() ? 0 : colX_.back(), totalH = rowY_.empty() ? 0 : rowY_.back();
    scrollX_ = std::clamp(scrollX_, 0, std::max(0, totalW - viewW));
    scrollY_ = std::clamp(scrollY_, 0, std::max(0, totalH - viewH));
    // Right-to-left sheets: column A is on the right, so the thumb starts on the right too.
    const bool rtl = sheet_ && sheet_->rightToLeft;
    const int thumbX = rtl ? std::max(0, totalW - viewW) - scrollX_ : scrollX_;
    SCROLLINFO si{sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL, 0, std::max(0, totalW - 1), static_cast<UINT>(viewW), thumbX, 0};
    SetScrollInfo(wnd_, SB_HORZ, &si, TRUE);
    si = {sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL, 0, std::max(0, totalH - 1), static_cast<UINT>(viewH), scrollY_, 0};
    SetScrollInfo(wnd_, SB_VERT, &si, TRUE);
}

void SheetView::ScrollBy(int dx, int dy) {
    const int ox = scrollX_, oy = scrollY_;
    scrollX_ += dx;
    scrollY_ += dy;
    UpdateScrollBars();
    if (ox != scrollX_ || oy != scrollY_) InvalidateRect(wnd_, nullptr, FALSE);
}

int SheetView::ScreenX(int x, int w, int clientW) const {
    const int left = hdrW_ + x - scrollX_;
    return (sheet_ && sheet_->rightToLeft) ? clientW - left - w : left;
}

int SheetView::MergeAt(uint32_t r, uint32_t c) const {
    if (mergeIndex_.empty() || !sheet_ || r >= sheet_->rows || c >= sheet_->cols) return -1;
    return mergeIndex_[static_cast<size_t>(r) * sheet_->cols + c];
}

HFONT SheetView::FontFor(const preview::CellStyle& s) {
    const int h = -std::max(1, static_cast<int>(std::lround(s.size * dpi_ / 72.0 * zoom_)));
    const FontKey key{s.font, h, s.bold, s.italic, s.underline, s.strike};
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    if (fonts_.size() > 300) ClearFonts();                       // many sizes: start over
    const std::wstring& name = sheet_ && s.font < sheet_->fontNames.size() ? sheet_->fontNames[s.font] : std::wstring(L"Calibri");
    HFONT f = CreateFontW(h, 0, 0, 0, s.bold ? FW_BOLD : FW_NORMAL, s.italic, s.underline, s.strike, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, name.c_str());
    fonts_[key] = f;
    return f;
}

HFONT SheetView::HeaderFont() {
    if (!headerFont_)
        headerFont_ = CreateFontW(-std::max(1, Px(12)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    return headerFont_;
}

void SheetView::Paint(HDC dc, const RECT& rc) {
    const int W = rc.right, H = rc.bottom;
    HBRUSH white = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    FillRect(dc, &rc, white);
    if (!sheet_ || !sheet_->rows || !sheet_->cols) return;
    const auto& s = *sheet_;
    const bool rtl = s.rightToLeft;
    SetBkMode(dc, TRANSPARENT);

    // Visible range.
    const int viewW = W - hdrW_, viewH = H - hdrH_;
    auto firstIndex = [](const std::vector<int>& pos, int v) {
        const auto it = std::upper_bound(pos.begin(), pos.end(), v);
        return static_cast<uint32_t>(std::max<std::ptrdiff_t>(0, (it - pos.begin()) - 1));
    };
    const uint32_t c0 = std::min(firstIndex(colX_, scrollX_), s.cols - 1);
    const uint32_t r0 = std::min(firstIndex(rowY_, scrollY_), s.rows - 1);
    uint32_t c1 = c0, r1 = r0;
    while (c1 + 1 < s.cols && colX_[c1 + 1] < scrollX_ + viewW) ++c1;
    while (r1 + 1 < s.rows && rowY_[r1 + 1] < scrollY_ + viewH) ++r1;

    auto cellRect = [&](uint32_t r, uint32_t c, uint32_t r2, uint32_t c2) {
        const int x = colX_[c], w = colX_[c2 + 1] - colX_[c];
        const int sx = ScreenX(x, w, W);
        const int sy = hdrH_ + rowY_[r] - scrollY_;
        return RECT{sx, sy, sx + w, sy + rowY_[r2 + 1] - rowY_[r]};
    };
    // Content area clip (headers drawn last).
    HRGN clip = CreateRectRgn(rtl ? 0 : hdrW_, hdrH_, rtl ? W - hdrW_ : W, H);
    SelectClipRgn(dc, clip);

    // 1) gridlines
    if (s.showGridLines) {
        HPEN gp = CreatePen(PS_SOLID, 1, kGrid);
        HGDIOBJ op = SelectObject(dc, gp);
        for (uint32_t c = c0; c <= c1 + 1 && c <= s.cols; ++c) {
            const int x = ScreenX(colX_[c], 0, W) - (rtl ? 1 : 0);
            Line(dc, x, hdrH_, x, H);
        }
        for (uint32_t r = r0; r <= r1 + 1 && r <= s.rows; ++r) {
            const int y = hdrH_ + rowY_[r] - scrollY_;
            Line(dc, 0, y, W, y);
        }
        SelectObject(dc, op);
        DeleteObject(gp);
    }

    // Merges overlapping the view (drawn as one cell, even when the anchor is scrolled away).
    std::vector<size_t> visMerges;
    for (size_t i = 0; i < s.merges.size(); ++i) {
        const auto& m = s.merges[i];
        if (m.r1 < r0 || m.r0 > r1 || m.c1 < c0 || m.c0 > c1 || m.r0 >= s.rows || m.c0 >= s.cols) continue;
        visMerges.push_back(i);
    }
    auto styleAt = [&](uint32_t r, uint32_t c) -> const preview::CellStyle& {
        const size_t k = static_cast<size_t>(r) * s.cols + c;
        const uint16_t st = k < s.cellStyle.size() ? s.cellStyle[k] : 0;
        return s.styles[st < s.styles.size() ? st : 0];
    };

    // 2) fills
    for (uint32_t r = r0; r <= r1; ++r)
        for (uint32_t c = c0; c <= c1; ++c) {
            if (MergeAt(r, c) >= 0) continue;
            const auto& st = styleAt(r, c);
            if (st.fill == preview::kNoColor) continue;
            RECT cr = cellRect(r, c, r, c);
            HBRUSH b = CreateSolidBrush(Rgb(st.fill));
            FillRect(dc, &cr, b);
            DeleteObject(b);
        }
    for (size_t i : visMerges) {
        const auto& m = s.merges[i];
        const uint32_t mr1 = std::min(m.r1, s.rows - 1), mc1 = std::min(m.c1, s.cols - 1);
        RECT cr = cellRect(m.r0, m.c0, mr1, mc1);
        const auto& st = styleAt(m.r0, m.c0);
        HBRUSH b = st.fill == preview::kNoColor ? nullptr : CreateSolidBrush(Rgb(st.fill));
        RECT inner{cr.left + 1, cr.top + 1, cr.right, cr.bottom};
        FillRect(dc, &inner, b ? b : white);                       // hides the inner gridlines
        if (b) DeleteObject(b);
    }

    // 3) text
    auto drawText = [&](uint32_t r, uint32_t c, RECT cr) {
        const size_t k = static_cast<size_t>(r) * s.cols + c;
        if (k >= s.cells.size() || s.cells[k].empty()) return;
        const std::wstring& text = s.cells[k];
        const auto& st = styleAt(r, c);
        const uint8_t kind = k < s.cellKind.size() ? s.cellKind[k] : 0;
        HGDIOBJ of = SelectObject(dc, FontFor(st));
        const uint32_t numColor = k < s.cellColor.size() ? s.cellColor[k] : preview::kNoColor;
        SetTextColor(dc, Rgb(numColor != preview::kNoColor ? numColor : st.color != preview::kNoColor ? st.color : 0x000000));
        // Excel's "General" alignment: numbers right, text left, booleans / errors centre.
        int h = st.hAlign;
        if (h == 0) h = kind == 1 ? 3 : (kind == 2 || kind == 3) ? 2 : 1;
        if (h == 5 || h == 4) h = 1;
        if (h == 6) h = 2;
        if (rtl && (h == 1 || h == 3)) h = 4 - h;               // right-to-left sheet: start is on the right
        const int pad = Px(2) + st.indent * Px(9);
        RECT tr{cr.left + pad, cr.top, cr.right - pad, cr.bottom};
        UINT fmt = DT_NOPREFIX | (h == 2 ? DT_CENTER : h == 3 ? DT_RIGHT : DT_LEFT);
        if (HasRtlText(text)) fmt |= DT_RTLREADING;
        if (st.wrap) {
            RECT calc = tr;
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &calc, fmt | DT_WORDBREAK | DT_CALCRECT);
            const int th = calc.bottom - calc.top, ch = tr.bottom - tr.top;
            if (st.vAlign == 1) tr.top += std::max(0, (ch - th) / 2); else if (st.vAlign == 2) tr.top += std::max(0, ch - th);
            IntersectClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &tr, fmt | DT_WORDBREAK);
            SelectClipRgn(dc, clip);
        } else {
            SIZE sz{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
            const int avail = tr.right - tr.left;
            std::wstring shown = text;
            if (kind == 1 && sz.cx > avail && MergeAt(r, c) < 0) {   // a number that does not fit: ####
                SIZE hs{};
                GetTextExtentPoint32W(dc, L"#", 1, &hs);
                shown.assign(static_cast<size_t>(std::max(1, avail / std::max(1, static_cast<int>(hs.cx)))), L'#');
            } else if (kind != 1 && sz.cx > avail && MergeAt(r, c) < 0) {
                // Text runs on into empty neighbours (in the reading direction), like Excel.
                const bool towardsEnd = h == 1 || h == 2, towardsStart = h == 3 || h == 2;
                const bool growRight = rtl ? towardsStart : towardsEnd, growLeft = rtl ? towardsEnd : towardsStart;
                auto empty = [&](uint32_t cc) {
                    const size_t kk = static_cast<size_t>(r) * s.cols + cc;
                    return MergeAt(r, cc) < 0 && (kk >= s.cells.size() || s.cells[kk].empty());
                };
                int need = sz.cx - avail;
                // neighbours in content order: "after" = c+1..., "before" = c-1...
                const bool afterIsRight = !rtl;
                for (uint32_t cc = c + 1; need > 0 && cc < s.cols && ((afterIsRight && growRight) || (!afterIsRight && growLeft)) && empty(cc); ++cc) {
                    const int w = colX_[cc + 1] - colX_[cc];
                    if (afterIsRight) tr.right += w; else tr.left -= w;
                    need -= h == 2 ? w / 2 : w;
                }
                for (int cc = static_cast<int>(c) - 1; need > 0 && cc >= 0 && ((afterIsRight && growLeft) || (!afterIsRight && growRight)) && empty(static_cast<uint32_t>(cc)); --cc) {
                    const int w = colX_[static_cast<uint32_t>(cc) + 1] - colX_[static_cast<uint32_t>(cc)];
                    if (afterIsRight) tr.left -= w; else tr.right += w;
                    need -= h == 2 ? w / 2 : w;
                }
                cr = {std::min(cr.left, tr.left - pad), cr.top, std::max(cr.right, tr.right + pad), cr.bottom};
            }
            const UINT v = st.vAlign == 0 ? DT_TOP : st.vAlign == 1 ? DT_VCENTER : DT_BOTTOM;
            if (st.vAlign == 2) tr.bottom -= Px(2);
            IntersectClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
            DrawTextW(dc, shown.c_str(), static_cast<int>(shown.size()), &tr, fmt | DT_SINGLELINE | v);
            SelectClipRgn(dc, clip);
        }
        SelectObject(dc, of);
    };
    // Text that overflows from a cell left / right of the view must still show: widen the scan a little.
    const uint32_t cs = c0 > 8 ? c0 - 8 : 0, ce = std::min(s.cols - 1, c1 + 8);
    for (uint32_t r = r0; r <= r1; ++r)
        for (uint32_t c = cs; c <= ce; ++c) {
            if (MergeAt(r, c) >= 0) continue;
            drawText(r, c, cellRect(r, c, r, c));
        }
    for (size_t i : visMerges) {
        const auto& m = s.merges[i];
        drawText(m.r0, m.c0, cellRect(m.r0, m.c0, std::min(m.r1, s.rows - 1), std::min(m.c1, s.cols - 1)));
    }

    // 4) borders (not inside merged blocks)
    auto pen = [&](const preview::CellBorder& b) {
        return CreatePen(PS_SOLID, b.style == 3 ? std::max(2, Px(3)) : b.style == 2 ? std::max(2, Px(2)) : 1, Rgb(b.color));
    };
    auto edge = [&](const preview::CellBorder& b, int x0, int y0, int x1, int y1, bool vertical) {
        if (!b.style) return;
        HPEN p = pen(b);
        HGDIOBJ op = SelectObject(dc, p);
        Line(dc, x0, y0, x1, y1);
        if (b.style == 4) { if (vertical) Line(dc, x0 + 2, y0, x1 + 2, y1); else Line(dc, x0, y0 + 2, x1, y1 + 2); }
        SelectObject(dc, op);
        DeleteObject(p);
    };
    for (uint32_t r = r0; r <= r1; ++r)
        for (uint32_t c = c0; c <= c1; ++c) {
            const auto& st = styleAt(r, c);
            if (!st.left.style && !st.right.style && !st.top.style && !st.bottom.style) continue;
            const int mi = MergeAt(r, c);
            const preview::CellMerge* m = mi >= 0 ? &s.merges[static_cast<size_t>(mi)] : nullptr;
            const RECT cr = cellRect(r, c, r, c);
            // content "left" edge is the screen right edge in right-to-left sheets
            const int xs = rtl ? cr.right - 1 : cr.left, xe = rtl ? cr.left : cr.right - 1;
            if (!m || c == m->c0) edge(st.left, xs, cr.top, xs, cr.bottom, true);
            if (!m || c == m->c1) edge(st.right, xe, cr.top, xe, cr.bottom, true);
            if (!m || r == m->r0) edge(st.top, cr.left, cr.top, cr.right, cr.top, false);
            if (!m || r == m->r1) edge(st.bottom, cr.left, cr.bottom - 1, cr.right, cr.bottom - 1, false);
        }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // 5) headers: column letters on top, row numbers at the start side
    HGDIOBJ of = SelectObject(dc, HeaderFont());
    SetTextColor(dc, kHeaderText);
    HBRUSH hb = CreateSolidBrush(kHeaderBg);
    HPEN hp = CreatePen(PS_SOLID, 1, kHeaderLine);
    HGDIOBJ op = SelectObject(dc, hp);
    RECT top{0, 0, W, hdrH_};
    FillRect(dc, &top, hb);
    const int hx0 = rtl ? W - hdrW_ : 0;
    RECT side{hx0, hdrH_, hx0 + hdrW_, H};
    FillRect(dc, &side, hb);
    HRGN topClip = CreateRectRgn(rtl ? 0 : hdrW_, 0, rtl ? W - hdrW_ : W, hdrH_);
    SelectClipRgn(dc, topClip);
    for (uint32_t c = c0; c <= c1; ++c) {
        const int w = colX_[c + 1] - colX_[c];
        if (w <= 0) continue;
        const int x = ScreenX(colX_[c], w, W);
        RECT hr{x, 0, x + w, hdrH_};
        const std::wstring n = ColumnName(c);
        DrawTextW(dc, n.c_str(), -1, &hr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        Line(dc, rtl ? x : x + w - 1, 0, rtl ? x : x + w - 1, hdrH_);
    }
    HRGN sideClip = CreateRectRgn(hx0, hdrH_, hx0 + hdrW_, H);
    SelectClipRgn(dc, sideClip);
    for (uint32_t r = r0; r <= r1; ++r) {
        const int h = rowY_[r + 1] - rowY_[r];
        if (h <= 0) continue;
        const int y = hdrH_ + rowY_[r] - scrollY_;
        RECT hr{hx0, y, hx0 + hdrW_, y + h};
        const std::wstring n = std::to_wstring(r + 1);
        DrawTextW(dc, n.c_str(), -1, &hr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        Line(dc, hx0, y + h - 1, hx0 + hdrW_, y + h - 1);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(topClip);
    DeleteObject(sideClip);
    Line(dc, 0, hdrH_ - 1, W, hdrH_ - 1);
    const int sx = rtl ? W - hdrW_ : hdrW_ - 1;
    Line(dc, sx, 0, sx, H);
    SelectObject(dc, op);
    SelectObject(dc, of);
    DeleteObject(hp);
    DeleteObject(hb);
}

LRESULT CALLBACK SheetView::ProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<SheetView*>(cs->lpCreateParams)->wnd_ = h;
    }
    auto* self = reinterpret_cast<SheetView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Proc(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT SheetView::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd_, &ps);
        RECT rc;
        GetClientRect(wnd_, &rc);
        HDC mem = CreateCompatibleDC(dc);                       // double buffer: no flicker while scrolling
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ ob = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd_, &ps);
        return 0;
    }
    case WM_SIZE: UpdateScrollBars(); InvalidateRect(wnd_, nullptr, FALSE); return 0;
    case WM_DPICHANGED_AFTERPARENT:
        dpi_ = static_cast<int>(GetDpiForWindow(wnd_));
        ClearFonts();
        Measure();
        UpdateScrollBars();
        InvalidateRect(wnd_, nullptr, FALSE);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        const bool horz = m == WM_HSCROLL;
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(wnd_, horz ? SB_HORZ : SB_VERT, &si);
        const int line = Px(horz ? 64 : 20), page = static_cast<int>(si.nPage);
        int pos = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: break;
        }
        if (horz) {
            const bool rtl = sheet_ && sheet_->rightToLeft;
            const int maxX = std::max(0, (colX_.empty() ? 0 : colX_.back()) - page);
            const int target = rtl ? maxX - std::clamp(pos, 0, maxX) : pos;           // thumb position -> scroll offset
            ScrollBy(target - scrollX_, 0);
        } else {
            ScrollBy(0, pos - scrollY_);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (m == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL))      // Ctrl+wheel = zoom (the preview pane decides)
            return SendMessageW(GetParent(wnd_), m, w, l);
        const int d = GET_WHEEL_DELTA_WPARAM(w);
        const bool horz = m == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(w) & MK_SHIFT);
        const int step = Px(horz ? 64 : 60) * d / WHEEL_DELTA;
        if (horz) ScrollBy(((sheet_ && sheet_->rightToLeft) ? step : -step) * (m == WM_MOUSEHWHEEL ? -1 : 1), 0);
        else ScrollBy(0, -step);
        return 0;
    }
    case WM_LBUTTONDOWN: SetFocus(wnd_); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        RECT rc; GetClientRect(wnd_, &rc);
        const int pageH = std::max(1, static_cast<int>(rc.bottom) - hdrH_);
        const bool rtl = sheet_ && sheet_->rightToLeft;
        switch (w) {
        case VK_UP: ScrollBy(0, -Px(20)); return 0;
        case VK_DOWN: ScrollBy(0, Px(20)); return 0;
        case VK_LEFT: ScrollBy(rtl ? Px(64) : -Px(64), 0); return 0;
        case VK_RIGHT: ScrollBy(rtl ? -Px(64) : Px(64), 0); return 0;
        case VK_PRIOR: ScrollBy(0, -pageH); return 0;
        case VK_NEXT: ScrollBy(0, pageH); return 0;
        case VK_HOME: ScrollBy(-scrollX_, (GetKeyState(VK_CONTROL) < 0) ? -scrollY_ : 0); return 0;
        case VK_END: if (GetKeyState(VK_CONTROL) < 0) ScrollBy(0, rowY_.empty() ? 0 : rowY_.back()); return 0;
        default: break;
        }
        break;
    }
    default: break;
    }
    return DefWindowProcW(wnd_, m, w, l);
}

} // namespace mzr::ui
