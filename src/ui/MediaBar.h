#pragma once
// Media control bar, drawn by itself (no child controls), in two looks:
//  Vlc  (video, light, VLC 3 style):
//     0:03 [=========o--------------------] 0:08
//     [▶] [■] [«10] [10»]                [⛶] [🔊] [◢◢◢◢◢ volume wedge]
//  Aimp (audio, dark, AIMP style, sits right under the dark song panel):
//     0:03 ━━━━━━━━━━━━━●──────────────────── 0:08
//                [⏮] [«10] ( ▶ ) [■] [10»] [⏭]          [🔊] ───●─── 80%
// A click anywhere on the seek bar jumps straight to that point; dragging scrubs; the wheel changes the
// volume; a click on the right time switches total / remaining (like VLC and AIMP).
// Talks to the MediaPlayer directly; actions it cannot do itself go to notifyWnd as notifyMsg with
// wParam = Action (previous / next file, full screen).
#include <windows.h>

namespace mzr::ui {

class MediaPlayer;

class MediaBar {
public:
    enum class Style { Vlc, Aimp };
    enum Action : WPARAM { StateChanged = 0, PreviousFile = 1, NextFile = 2, FullScreen = 3 };

    bool Create(HWND parent, HINSTANCE inst, MediaPlayer* player, HWND notifyWnd, UINT notifyMsg);
    HWND hwnd() const { return wnd_; }
    int  PreferredHeight() const;
    void SetStyle(Style s);
    Style style() const { return style_; }
    void Refresh();                  // call from the media timer / state changes

    // AIMP panel colours (the song panel above the bar uses the same ones).
    static COLORREF AimpBack();
    static COLORREF AimpText();
    static COLORREF AimpDim();
    static COLORREF AimpAccent();

private:
    enum Part { None = 0, Seek, Play, Stop, Back, Fwd, Mute, Volume, Prev, Next, Full, TimeR };
    static LRESULT CALLBACK ProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    void Layout();
    Part HitTest(POINT p) const;
    void SeekTo(int x);
    void VolumeTo(int x);
    void Click(Part p);
    int  Dpi(int px) const { return MulDiv(px, dpi_, 96); }

    HWND wnd_ = nullptr, notify_ = nullptr;
    MediaPlayer* player_ = nullptr;
    UINT notifyMsg_ = 0;
    int dpi_ = 96;
    Style style_ = Style::Vlc;
    HFONT textFont_ = nullptr, iconFont_ = nullptr, bigIconFont_ = nullptr;
    RECT seekRc_{}, timeL_{}, timeR_{}, playRc_{}, stopRc_{}, backRc_{}, fwdRc_{}, muteRc_{}, volRc_{},
         prevRc_{}, nextRc_{}, fullRc_{};
    Part hot_ = None, drag_ = None;
    double dragPos_ = -1;            // seconds shown while scrubbing
    bool tracking_ = false;
    bool remaining_ = false;         // right time shows "-0:05" instead of the total
};

} // namespace mzr::ui
