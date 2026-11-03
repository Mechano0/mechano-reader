#include "ui/MediaBar.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ui/MediaPlayer.h"
#include "ui/Theme.h"

namespace mzr::ui {

namespace {
constexpr wchar_t kClass[] = L"MechanoZipReader.MediaBar";
// Segoe MDL2 Assets glyphs (in every Windows 10 / 11).
constexpr wchar_t kPlay[] = L"\xE768", kPause[] = L"\xE769", kStop[] = L"\xE71A", kRewind[] = L"\xEB9E",
                  kForward[] = L"\xEB9D", kVol[] = L"\xE767", kMute[] = L"\xE74F", kPrev[] = L"\xE892",
                  kNext[] = L"\xE893", kFull[] = L"\xE740";
const COLORREF kVlcAccent = theme::kYellow;     // VLC orange, in the palette yellow

std::wstring Time(double s) {
    if (s < 0) s = 0;
    const long t = static_cast<long>(s);
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(b, L"%ld:%02ld", t / 60, t % 60);
    return b;
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Disc(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF edge) {
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, edge);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

bool In(const RECT& r, POINT p) { return PtInRect(&r, p) != FALSE; }
} // namespace

COLORREF MediaBar::AimpBack() { return theme::kPlayerBack; }
COLORREF MediaBar::AimpText() { return theme::kPlayerText; }
COLORREF MediaBar::AimpDim() { return theme::kPlayerDim; }
COLORREF MediaBar::AimpAccent() { return theme::kCoral; }

bool MediaBar::Create(HWND parent, HINSTANCE inst, MediaPlayer* player, HWND notifyWnd, UINT notifyMsg) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = ProcStatic;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    notify_ = notifyWnd;
    player_ = player;
    notifyMsg_ = notifyMsg;
    dpi_ = static_cast<int>(GetDpiForWindow(parent));
    if (dpi_ <= 0) dpi_ = 96;
    wnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD, 0, 0, 0, 0, parent, nullptr, inst, this);
    if (!wnd_) return false;
    NONCLIENTMETRICSW m{sizeof(m)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0, dpi_);
    textFont_ = CreateFontIndirectW(&m.lfMessageFont);
    LOGFONTW lf{};
    lf.lfHeight = -Dpi(15);
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe MDL2 Assets");
    iconFont_ = CreateFontIndirectW(&lf);
    lf.lfHeight = -Dpi(18);
    bigIconFont_ = CreateFontIndirectW(&lf);
    return true;
}

int MediaBar::PreferredHeight() const { return style_ == Style::Aimp ? Dpi(86) : Dpi(62); }

void MediaBar::SetStyle(Style s) {
    if (s == style_) return;
    style_ = s;
    hot_ = drag_ = None;
    Layout();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void MediaBar::Refresh() {
    if (wnd_ && IsWindowVisible(wnd_)) InvalidateRect(wnd_, nullptr, FALSE);
}

LRESULT CALLBACK MediaBar::ProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    MediaBar* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<MediaBar*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->wnd_ = h;
    } else {
        self = reinterpret_cast<MediaBar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->Proc(m, w, l) : DefWindowProcW(h, m, w, l);
}

void MediaBar::Layout() {
    RECT rc;
    GetClientRect(wnd_, &rc);
    const int pad = Dpi(style_ == Style::Aimp ? 14 : 6), tw = Dpi(54);
    // row 1: time | seek | time
    const int r1 = Dpi(style_ == Style::Aimp ? 6 : 4), r1h = Dpi(22);
    timeL_ = {rc.left + pad, r1, rc.left + pad + tw, r1 + r1h};
    timeR_ = {rc.right - pad - tw, r1, rc.right - pad, r1 + r1h};
    seekRc_ = {timeL_.right + Dpi(6), r1, std::max<LONG>(timeL_.right + Dpi(10), timeR_.left - Dpi(6)), r1 + r1h};
    const RECT none{};
    if (style_ == Style::Vlc) {
        // row 2: buttons left, full screen + volume right
        const int r2 = Dpi(30), bh = Dpi(28), bw = Dpi(34), g = Dpi(2);
        int x = rc.left + pad;
        playRc_ = {x, r2, x + Dpi(40), r2 + bh};  x = playRc_.right + Dpi(8);
        stopRc_ = {x, r2, x + bw, r2 + bh};       x = stopRc_.right + Dpi(8);
        backRc_ = {x, r2, x + bw, r2 + bh};       x = backRc_.right + g;
        fwdRc_ = {x, r2, x + bw, r2 + bh};
        volRc_ = {rc.right - pad - Dpi(76), r2 + Dpi(4), rc.right - pad, r2 + bh - Dpi(4)};
        muteRc_ = {volRc_.left - Dpi(4) - bw, r2, volRc_.left - Dpi(4), r2 + bh};
        fullRc_ = {muteRc_.left - Dpi(10) - bw, r2, muteRc_.left - Dpi(10), r2 + bh};
        prevRc_ = nextRc_ = none;
    } else {
        // row 2: transport centred, volume slider right
        const int r2 = Dpi(36), bh = Dpi(40), bw = Dpi(36), big = Dpi(46), g = Dpi(4);
        const int width = rc.right - rc.left;
        // wide: transport centred + volume slider; narrower: no slider; very narrow: no 10 s buttons either
        const int full = 5 * bw + big + 5 * g, small = 3 * bw + big + 3 * g;
        const bool slider = width >= full + Dpi(260);
        const bool tens = slider || width >= full + bw + 2 * pad + Dpi(12);
        const int total = tens ? full : small;
        int x = slider ? rc.left + (width - total) / 2 : rc.left + pad;
        const int cy = r2 + bh / 2;
        auto put = [&](RECT& r, int w) { r = {x, cy - w / 2, x + w, cy + w / 2}; x = r.right + g; };
        put(prevRc_, bw);
        if (tens) put(backRc_, bw); else backRc_ = none;
        put(playRc_, big);
        put(stopRc_, bw);
        if (tens) put(fwdRc_, bw); else fwdRc_ = none;
        put(nextRc_, bw);
        if (slider) {
            volRc_ = {rc.right - pad - Dpi(116), cy - Dpi(8), rc.right - pad - Dpi(40), cy + Dpi(8)};
            muteRc_ = {volRc_.left - Dpi(6) - bw, cy - bw / 2, volRc_.left - Dpi(6), cy + bw / 2};
        } else {
            volRc_ = none;                                  // the wheel still changes the volume
            muteRc_ = {rc.right - pad - bw, cy - bw / 2, rc.right - pad, cy + bw / 2};
        }
        fullRc_ = none;
    }
}

MediaBar::Part MediaBar::HitTest(POINT p) const {
    RECT seekHit = seekRc_;
    InflateRect(&seekHit, 0, Dpi(3));
    if (In(seekHit, p)) return Seek;
    if (In(timeR_, p)) return TimeR;
    if (In(playRc_, p)) return Play;
    if (In(stopRc_, p)) return Stop;
    if (In(backRc_, p)) return Back;
    if (In(fwdRc_, p)) return Fwd;
    if (In(prevRc_, p)) return Prev;
    if (In(nextRc_, p)) return Next;
    if (In(fullRc_, p)) return Full;
    if (In(muteRc_, p)) return Mute;
    if (!IsRectEmpty(&volRc_)) {
        RECT volHit = volRc_;
        InflateRect(&volHit, Dpi(4), Dpi(6));
        if (In(volHit, p)) return Volume;
    }
    return None;
}

void MediaBar::SeekTo(int x) {
    const double d = player_ ? player_->Duration() : 0;
    if (d <= 0) return;
    const int w = std::max(1, static_cast<int>(seekRc_.right - seekRc_.left));
    const double f = std::clamp(static_cast<double>(x - seekRc_.left) / w, 0.0, 1.0);
    dragPos_ = f * d;
    player_->Seek(dragPos_);
    InvalidateRect(wnd_, nullptr, FALSE);
}

void MediaBar::VolumeTo(int x) {
    if (!player_ || IsRectEmpty(&volRc_)) return;
    const int w = std::max(1, static_cast<int>(volRc_.right - volRc_.left));
    player_->SetVolume(static_cast<float>(std::clamp(static_cast<double>(x - volRc_.left) / w, 0.0, 1.0)));
    InvalidateRect(wnd_, nullptr, FALSE);
}

void MediaBar::Click(Part p) {
    if (p == TimeR) { remaining_ = !remaining_; InvalidateRect(wnd_, nullptr, FALSE); return; }
    if (p == Prev || p == Next || p == Full) {           // done by the main window
        PostMessageW(notify_, notifyMsg_, p == Prev ? PreviousFile : p == Next ? NextFile : FullScreen, 0);
        return;
    }
    if (!player_ || !player_->IsOpen()) return;
    switch (p) {
    case Play: player_->TogglePlay(); break;
    case Stop: player_->Stop(); break;
    case Back: player_->Seek(player_->Position() - 10.0); break;
    case Fwd: player_->Seek(player_->Position() + 10.0); break;
    case Mute: player_->ToggleMute(); break;
    default: break;
    }
    PostMessageW(notify_, notifyMsg_, StateChanged, 0);
    InvalidateRect(wnd_, nullptr, FALSE);
}

void MediaBar::Paint(HDC dc) {
    RECT rc;
    GetClientRect(wnd_, &rc);
    const bool aimp = style_ == Style::Aimp;
    const COLORREF face = aimp ? AimpBack() : theme::kBack;
    const COLORREF text = aimp ? AimpText() : theme::kText;
    const COLORREF gray = aimp ? RGB(0x6E, 0x63, 0x5D) : theme::kTextDim;
    const COLORREF dim = aimp ? AimpDim() : text;
    const COLORREF groove = aimp ? theme::kPlayerLine : theme::kLine;
    const COLORREF hotBg = aimp ? RGB(0x48, 0x3E, 0x3A) : theme::kHover;
    const COLORREF accent = aimp ? AimpAccent() : kVlcAccent;
    Fill(dc, rc, face);
    if (!aimp) {
        RECT top{rc.left, rc.top, rc.right, rc.top + 1};
        Fill(dc, top, theme::kLine);
    }
    SetBkMode(dc, TRANSPARENT);

    const bool open = player_ && player_->IsOpen();
    const double d = open ? player_->Duration() : 0;
    const double pos = drag_ == Seek && dragPos_ >= 0 ? dragPos_ : (open ? player_->Position() : 0);

    // ---- times (the right one: total, or remaining after a click)
    SelectObject(dc, textFont_);
    SetTextColor(dc, dim);
    std::wstring l = Time(pos), r = remaining_ && d > 0 ? L"-" + Time(d - pos) : Time(d);
    DrawTextW(dc, l.c_str(), -1, &timeL_, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SetTextColor(dc, hot_ == TimeR ? accent : dim);
    DrawTextW(dc, r.c_str(), -1, &timeR_, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    // ---- seek bar: groove, played part in the accent colour, round knob
    const int cy = (seekRc_.top + seekRc_.bottom) / 2;
    const bool seekHot = hot_ == Seek || drag_ == Seek;
    const int gh = aimp ? (seekHot ? Dpi(5) : Dpi(3)) : (seekHot ? Dpi(6) : Dpi(4));
    RECT g{seekRc_.left, cy - gh / 2, seekRc_.right, cy - gh / 2 + gh};
    Fill(dc, g, groove);
    if (d > 0) {
        const int x = seekRc_.left + static_cast<int>(std::clamp(pos / d, 0.0, 1.0) * (seekRc_.right - seekRc_.left));
        RECT played{g.left, g.top, x, g.bottom};
        Fill(dc, played, accent);
        if (aimp) {
            if (seekHot) Disc(dc, x, cy, Dpi(6), text, text);    // AIMP: the knob shows on hover
        } else {
            Disc(dc, x, cy, Dpi(7), accent, theme::kYellowDark);
        }
    }

    // ---- buttons (flat; a soft background when hovered)
    auto button = [&](const RECT& br, const wchar_t* glyph, Part p, bool enabled) {
        if (IsRectEmpty(&br)) return;
        if (hot_ == p && enabled) {
            if (aimp) {
                Disc(dc, (br.left + br.right) / 2, (br.top + br.bottom) / 2, (br.right - br.left) / 2 - 1, hotBg, hotBg);
            } else {
                Fill(dc, br, hotBg);
                FrameRect(dc, &br, theme::Brush(theme::kPink));
            }
        }
        SelectObject(dc, iconFont_);
        SetTextColor(dc, enabled ? (aimp && hot_ != p ? dim : text) : gray);
        if (aimp && hot_ == p && enabled) SetTextColor(dc, text);
        RECT t = br;
        DrawTextW(dc, glyph, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    };
    const bool playing = open && player_->IsPlaying();
    if (aimp) {
        // the big round play / pause button in the middle
        const int pcx = (playRc_.left + playRc_.right) / 2, pcy = (playRc_.top + playRc_.bottom) / 2;
        const int pr = (playRc_.right - playRc_.left) / 2;
        const COLORREF disc = !open ? gray : hot_ == Play ? RGB(0xF5, 0x99, 0x97) : accent;
        Disc(dc, pcx, pcy, pr, disc, disc);
        SelectObject(dc, bigIconFont_);
        SetTextColor(dc, theme::kText);              // dark glyph on coral (readable)
        RECT t = playRc_;
        if (!playing) OffsetRect(&t, Dpi(2), 0);           // the triangle looks centred
        DrawTextW(dc, playing ? kPause : kPlay, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        button(prevRc_, kPrev, Prev, true);
        button(nextRc_, kNext, Next, true);
    } else {
        button(playRc_, playing ? kPause : kPlay, Play, open);
        button(fullRc_, kFull, Full, true);
    }
    button(stopRc_, kStop, Stop, open);
    button(backRc_, kRewind, Back, d > 0);
    button(fwdRc_, kForward, Fwd, d > 0);
    const bool muted = !open || player_->IsMuted();
    button(muteRc_, muted ? kMute : kVol, Mute, open);

    // ---- volume
    if (IsRectEmpty(&volRc_)) return;
    const float vol = open ? player_->Volume() : 0.f;
    const std::wstring ps = std::to_wstring(static_cast<int>(std::lround(vol * 100))) + L"%";
    if (aimp) {
        // AIMP: a thin line with a round knob, percent on the right
        const int vy = (volRc_.top + volRc_.bottom) / 2;
        const int vx = volRc_.left + static_cast<int>(vol * (volRc_.right - volRc_.left));
        RECT line{volRc_.left, vy - Dpi(1), volRc_.right, vy + Dpi(2)};
        Fill(dc, line, groove);
        RECT lvl{volRc_.left, line.top, vx, line.bottom};
        Fill(dc, lvl, muted ? gray : accent);
        Disc(dc, vx, vy, Dpi(hot_ == Volume || drag_ == Volume ? 6 : 5), muted ? dim : text, muted ? dim : text);
        SelectObject(dc, textFont_);
        SetTextColor(dc, dim);
        RECT pct{volRc_.right + Dpi(12), volRc_.top - Dpi(4), volRc_.right + Dpi(44), volRc_.bottom + Dpi(4)};
        DrawTextW(dc, muted ? L"off" : ps.c_str(), -1, &pct, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        return;
    }
    // VLC: the wedge, filled up to the level, with the percent
    const POINT wedge[3] = {{volRc_.left, volRc_.bottom}, {volRc_.right, volRc_.bottom}, {volRc_.right, volRc_.top}};
    HRGN all = CreatePolygonRgn(wedge, 3, WINDING);
    SetDCBrushColor(dc, groove);
    FillRgn(dc, all, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int vx = volRc_.left + static_cast<int>(vol * (volRc_.right - volRc_.left));
    HRGN level = CreateRectRgn(volRc_.left, volRc_.top, vx, volRc_.bottom + 1);
    CombineRgn(level, level, all, RGN_AND);
    SetDCBrushColor(dc, muted ? gray : accent);
    FillRgn(dc, level, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    DeleteObject(level);
    DeleteObject(all);
    SelectObject(dc, textFont_);
    SetTextColor(dc, text);
    RECT pct{volRc_.left - Dpi(2), volRc_.top - Dpi(3), volRc_.left + Dpi(40), volRc_.top + Dpi(14)};
    DrawTextW(dc, ps.c_str(), -1, &pct, DT_SINGLELINE | DT_LEFT | DT_TOP | DT_NOPREFIX);
}

LRESULT MediaBar::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: Layout(); InvalidateRect(wnd_, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd_, &ps);
        RECT rc;
        GetClientRect(wnd_, &rc);
        // double buffered: the bar repaints 4x a second while playing
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (drag_ == Seek) { SeekTo(p.x); return 0; }
        if (drag_ == Volume) { VolumeTo(p.x); return 0; }
        const Part h = HitTest(p);
        if (h != hot_) { hot_ = h; InvalidateRect(wnd_, nullptr, FALSE); }
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, wnd_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_ != None) { hot_ = None; InvalidateRect(wnd_, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        // the wheel anywhere on the bar: volume up / down 5 % (VLC and AIMP do the same)
        if (player_ && player_->IsOpen()) {
            const int steps = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
            if (steps) {
                player_->SetVolume(std::clamp(player_->Volume() + 0.05f * steps, 0.f, 1.f));
                PostMessageW(notify_, notifyMsg_, StateChanged, 0);
                InvalidateRect(wnd_, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const Part h = HitTest(p);
        if (h == Seek || h == Volume) {
            // jump straight to the clicked point, then keep following the mouse
            drag_ = h;
            SetCapture(wnd_);
            if (h == Seek) SeekTo(p.x); else VolumeTo(p.x);
        } else if (h != None) {
            Click(h);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (drag_ != None) {
            drag_ = None;
            dragPos_ = -1;
            ReleaseCapture();
            PostMessageW(notify_, notifyMsg_, StateChanged, 0);
            InvalidateRect(wnd_, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        drag_ = None;
        dragPos_ = -1;
        return 0;
    case WM_DESTROY:
        for (HFONT* f : {&textFont_, &iconFont_, &bigIconFont_}) {
            if (*f) DeleteObject(*f);
            *f = nullptr;
        }
        return 0;
    default: break;
    }
    return DefWindowProcW(wnd_, m, w, l);
}

}  // namespace mzr::ui
