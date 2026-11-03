#include "ui/Theme.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <map>
#include <string>

namespace mzr::ui::theme {
namespace {

struct ButtonData {
    ButtonStyle style = ButtonStyle::Toolbar;
    wchar_t glyph = 0;
    bool hot = false, tracking = false;
};

int Scale(HWND h, int px) {
    const UINT dpi = GetDpiForWindow(h);
    return MulDiv(px, dpi ? static_cast<int>(dpi) : 96, 96);
}

LRESULT CALLBACK FlatProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    auto* d = reinterpret_cast<ButtonData*>(ref);
    switch (m) {
    case WM_MOUSEMOVE:
        if (!d->hot) { d->hot = true; InvalidateRect(h, nullptr, FALSE); }
        if (!d->tracking) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            d->tracking = TrackMouseEvent(&t) != FALSE;
        }
        break;
    case WM_MOUSELEAVE:
        d->tracking = false;
        if (d->hot) { d->hot = false; InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, FlatProc, id);
        delete d;
        break;
    default: break;
    }
    return DefSubclassProc(h, m, w, l);
}

// ---- status bar: painted by us
LRESULT CALLBACK StatusProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        Fill(mem, rc, kBar);
        RECT top{rc.left, rc.top, rc.right, rc.top + 1};
        Fill(mem, top, kLine);
        HGDIOBJ oldFont = SelectObject(mem, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
        SetBkMode(mem, TRANSPARENT);
        const int parts = static_cast<int>(SendMessageW(h, SB_GETPARTS, 0, 0));
        for (int i = 0; i < parts && i < 16; ++i) {
            RECT pr{};
            SendMessageW(h, SB_GETRECT, i, reinterpret_cast<LPARAM>(&pr));
            const LRESULT info = SendMessageW(h, SB_GETTEXTLENGTHW, i, 0);
            std::wstring t(LOWORD(info) + 1, L'\0');
            SendMessageW(h, SB_GETTEXTW, i, reinterpret_cast<LPARAM>(t.data()));
            t.resize(wcslen(t.c_str()));
            RECT tr = pr;
            tr.left += Scale(h, 8);
            tr.right -= Scale(h, 6);
            SetTextColor(mem, i == 0 ? kText : kTextDim);
            DrawTextW(mem, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (i + 1 < parts) {
                RECT sep{pr.right - 1, pr.top + Scale(h, 5), pr.right, pr.bottom - Scale(h, 4)};
                Fill(mem, sep, kLine);
            }
        }
        // size grip: three sage dots in the corner
        const int g = Scale(h, 2), s = Scale(h, 4);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c <= r; ++c) {
                const int x = rc.right - s * (c + 1) - g, y = rc.bottom - s * (3 - r) - g + s;
                RECT dot{x, y - g, x + g, y};
                Fill(mem, dot, kSage);
            }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case SB_SETTEXTW: {
        const LRESULT r = DefSubclassProc(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, StatusProc, id);
        break;
    default: break;
    }
    return DefSubclassProc(h, m, w, l);
}

// ---- list view: the header's custom draw arrives at the list (its parent)
int g_sortCol = -1, g_dropCol = -1;
bool g_sortAsc = true;

LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NOTIFY) {
        auto* hdr = reinterpret_cast<NMHDR*>(l);
        if (hdr->hwndFrom == ListView_GetHeader(h) && hdr->code == NM_CUSTOMDRAW)
            return HeaderCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(l), g_sortCol, g_sortAsc, g_dropCol);
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, ListProc, id);
    return DefSubclassProc(h, m, w, l);
}

} // namespace

HBRUSH Brush(COLORREF c) {
    static std::map<COLORREF, HBRUSH> cache;
    auto it = cache.find(c);
    if (it != cache.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    cache[c] = b;
    return b;
}

void Fill(HDC dc, const RECT& r, COLORREF c) { FillRect(dc, &r, Brush(c)); }

void RoundRectFill(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

HFONT IconFont(int px) {
    static std::map<int, HFONT> cache;
    auto it = cache.find(px);
    if (it != cache.end()) return it->second;
    LOGFONTW lf{};
    lf.lfHeight = -px;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe MDL2 Assets");
    HFONT f = CreateFontIndirectW(&lf);
    cache[px] = f;
    return f;
}

void MakeFlatButton(HWND button, ButtonStyle style, wchar_t glyph) {
    if (!button) return;
    auto* d = new ButtonData{style, glyph, false, false};
    LONG_PTR st = GetWindowLongPtrW(button, GWL_STYLE);
    st = (st & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW;
    SetWindowLongPtrW(button, GWL_STYLE, st);
    if (!SetWindowSubclass(button, FlatProc, 1, reinterpret_cast<DWORD_PTR>(d))) delete d;
    InvalidateRect(button, nullptr, TRUE);
}

bool DrawItem(const DRAWITEMSTRUCT* di) {
    if (!di || di->CtlType != ODT_BUTTON) return false;
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(di->hwndItem, FlatProc, 1, &ref) || !ref) return false;
    const auto* d = reinterpret_cast<const ButtonData*>(ref);
    HWND h = di->hwndItem;
    HDC dc = di->hDC;
    const RECT rc = di->rcItem;
    const bool pressed = (di->itemState & ODS_SELECTED) != 0;
    const bool disabled = (di->itemState & ODS_DISABLED) != 0;
    const bool hot = d->hot && !disabled;
    const int radius = Scale(h, 8);

    // what is behind the button
    Fill(dc, rc, d->style == ButtonStyle::Toolbar ? kBar : kBack);
    COLORREF fill = 0, border = 0, text = kText, icon = kIcon;
    bool shape = true;
    switch (d->style) {
    case ButtonStyle::Toolbar:
        shape = hot || pressed;
        fill = border = pressed ? kPink : kHover;
        break;
    case ButtonStyle::Primary:
        fill = border = disabled ? kLine : pressed ? kCoralDark : hot ? RGB(0xF5, 0x99, 0x97) : kCoral;
        icon = kText;
        break;
    case ButtonStyle::Secondary:
        fill = pressed ? kPink : hot ? kHover : kPaper;
        border = hot || pressed ? kPink : kLine;
        break;
    }
    if (shape) {
        RECT s = rc;
        InflateRect(&s, -1, -1);
        RoundRectFill(dc, s, radius, fill, border);
    }
    if (disabled) text = icon = RGB(0xB8, 0xAA, 0x9F);

    wchar_t buf[128];
    GetWindowTextW(h, buf, 128);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    HGDIOBJ old = SelectObject(dc, font);
    SIZE ts{};
    GetTextExtentPoint32W(dc, buf, lstrlenW(buf), &ts);
    const int iconPx = Scale(h, 16), gap = buf[0] ? Scale(h, 7) : 0;
    const int contentW = (d->glyph ? iconPx + gap : 0) + ts.cx;
    int x = rc.left + (rc.right - rc.left - contentW) / 2;
    SetBkMode(dc, TRANSPARENT);
    if (d->glyph) {
        SelectObject(dc, IconFont(iconPx));
        SetTextColor(dc, icon);
        RECT ir{x, rc.top, x + iconPx, rc.bottom};
        const wchar_t g[2] = {d->glyph, 0};
        DrawTextW(dc, g, 1, &ir, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        x += iconPx + gap;
        SelectObject(dc, font);
    }
    SetTextColor(dc, text);
    RECT tr{x, rc.top, x + ts.cx + 2, rc.bottom};
    DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = rc;
        InflateRect(&f, -Scale(h, 3), -Scale(h, 3));
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, kCoral);
        RoundRect(dc, f.left, f.top, f.right, f.bottom, radius, radius);
        SelectObject(dc, ob);
        SelectObject(dc, op);
    }
    SelectObject(dc, old);
    return true;
}

int ButtonWidth(HWND button, int dpi) {
    wchar_t buf[128];
    GetWindowTextW(button, buf, 128);
    HDC dc = GetDC(button);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
    SIZE ts{};
    GetTextExtentPoint32W(dc, buf, lstrlenW(buf), &ts);
    SelectObject(dc, old);
    ReleaseDC(button, dc);
    DWORD_PTR ref = 0;
    const bool glyph = GetWindowSubclass(button, FlatProc, 1, &ref) && ref && reinterpret_cast<ButtonData*>(ref)->glyph;
    return ts.cx + MulDiv(glyph ? 16 + 7 + 22 : 22, dpi, 96);
}

void ApplyTitleBar(HWND top) {
    // Windows 11 (build 22000+); older Windows ignores these attributes.
    const COLORREF caption = kBar, text = kText, border = kSage;
    DwmSetWindowAttribute(top, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(top, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    DwmSetWindowAttribute(top, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}

void ThemeStatusBar(HWND status) {
    if (status) SetWindowSubclass(status, StatusProc, 1, 0);
}

void ThemeListView(HWND list) {
    if (!list) return;
    ListView_SetBkColor(list, kPaper);
    ListView_SetTextBkColor(list, kPaper);
    ListView_SetTextColor(list, kText);
    SetWindowSubclass(list, ListProc, 1, 0);
}

void SetHeaderInfo(int sortColumn, bool ascending, int dropColumn) {
    g_sortCol = sortColumn;
    g_sortAsc = ascending;
    g_dropCol = dropColumn;
}

LRESULT ListCustomDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        const int item = static_cast<int>(cd->nmcd.dwItemSpec);
        const bool selected = ListView_GetItemState(cd->nmcd.hdr.hwndFrom, item, LVIS_SELECTED) != 0;
        const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
        cd->clrText = kText;
        cd->clrTextBk = selected ? kSelect : hot ? kHover : kPaper;
        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_HOT | CDIS_FOCUS);   // no blue system highlight
        return CDRF_NEWFONT;
    }
    default: return CDRF_DODEFAULT;
    }
}

LRESULT HeaderCustomDraw(NMCUSTOMDRAW* cd, int sortColumn, bool ascending, int dropColumn) {
    HWND h = cd->hdr.hwndFrom;
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        RECT rc;
        GetClientRect(h, &rc);
        Fill(cd->hdc, rc, kBar);
        RECT line{rc.left, rc.bottom - 1, rc.right, rc.bottom};
        Fill(cd->hdc, line, kLine);
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    }
    case CDDS_POSTPAINT: {
        // the empty part right of the last column (the header paints it white by itself)
        const int n = Header_GetItemCount(h);
        if (n <= 0) return CDRF_DODEFAULT;
        RECT last{};
        Header_GetItemRect(h, Header_OrderToIndex(h, n - 1), &last);
        RECT rc;
        GetClientRect(h, &rc);
        RECT rest{last.right, rc.top, rc.right, rc.bottom};
        if (rest.right > rest.left) {
            Fill(cd->hdc, rest, kBar);
            RECT line{rest.left, rc.bottom - 1, rest.right, rc.bottom};
            Fill(cd->hdc, line, kLine);
        }
        return CDRF_DODEFAULT;
    }
    case CDDS_ITEMPREPAINT: {
        const int i = static_cast<int>(cd->dwItemSpec);
        RECT r = cd->rc;
        const bool hot = (cd->uItemState & CDIS_HOT) != 0;
        Fill(cd->hdc, r, hot ? kHover : kBar);
        RECT line{r.left, r.bottom - 1, r.right, r.bottom};
        Fill(cd->hdc, line, kLine);
        RECT sep{r.right - 1, r.top + Scale(h, 5), r.right, r.bottom - Scale(h, 5)};
        Fill(cd->hdc, sep, kLine);
        wchar_t text[128]{};
        HDITEMW it{};
        it.mask = HDI_TEXT | HDI_FORMAT;
        it.pszText = text;
        it.cchTextMax = 128;
        Header_GetItem(h, i, &it);
        RECT tr = r;
        tr.left += Scale(h, 8);
        tr.right -= Scale(h, 8);
        const int icon = Scale(h, 10);
        if (i == dropColumn) {                                    // the "choose what this column shows" arrow
            RECT a{tr.right - icon - Scale(h, 2), r.top, tr.right + Scale(h, 2), r.bottom};
            SelectObject(cd->hdc, IconFont(icon));
            SetTextColor(cd->hdc, kIcon);
            SetBkMode(cd->hdc, TRANSPARENT);
            DrawTextW(cd->hdc, L"\xE70D", 1, &a, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            tr.right = a.left - Scale(h, 4);
        }
        if (i == sortColumn) {
            RECT a{tr.right - icon, r.top, tr.right, r.bottom};
            SelectObject(cd->hdc, IconFont(Scale(h, 9)));
            SetTextColor(cd->hdc, kCoralDark);
            SetBkMode(cd->hdc, TRANSPARENT);
            DrawTextW(cd->hdc, ascending ? L"\xE70E" : L"\xE70D", 1, &a, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            tr.right = a.left - Scale(h, 4);
        }
        SelectObject(cd->hdc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
        SetTextColor(cd->hdc, kText);
        SetBkMode(cd->hdc, TRANSPARENT);
        const UINT align = (it.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT ? DT_RIGHT : (it.fmt & HDF_JUSTIFYMASK) == HDF_CENTER ? DT_CENTER : DT_LEFT;
        DrawTextW(cd->hdc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | align);
        return CDRF_SKIPDEFAULT;
    }
    default: return CDRF_DODEFAULT;
    }
}

} // namespace mzr::ui::theme
