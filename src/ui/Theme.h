#pragma once
// The app's look: Eslam's palette (2026-11-02)
//   #F6BD60 yellow   #F7EDE2 cream   #F5CAC3 pink   #84A59D sage   #F28482 coral
// Cream is the background, pink the selection / hover, coral the main action and the player accent,
// sage the icons and lines, yellow the video bar and highlights. Text is a warm dark brown (never
// white on the light fills, so it stays readable). File contents (Word, Excel, PDF, text) keep a
// white "paper" background.
// Also: flat owner-drawn buttons (icon + label, hover), the Windows 11 title bar colours, a
// custom-painted status bar and list header.
#include <windows.h>
#include <commctrl.h>

namespace mzr::ui::theme {

// ---- palette
constexpr COLORREF kYellow = RGB(0xF6, 0xBD, 0x60);
constexpr COLORREF kCream  = RGB(0xF7, 0xED, 0xE2);
constexpr COLORREF kPink   = RGB(0xF5, 0xCA, 0xC3);
constexpr COLORREF kSage   = RGB(0x84, 0xA5, 0x9D);
constexpr COLORREF kCoral  = RGB(0xF2, 0x84, 0x82);

// ---- derived
constexpr COLORREF kBack      = kCream;                      // window, panes
constexpr COLORREF kBar       = RGB(0xF1, 0xE3, 0xD4);       // toolbar band, status bar, list header
constexpr COLORREF kPaper     = RGB(0xFF, 0xFB, 0xF7);       // the file list, search box
constexpr COLORREF kWhite     = RGB(0xFF, 0xFF, 0xFF);       // file contents (documents)
constexpr COLORREF kText      = RGB(0x3B, 0x32, 0x2C);       // warm dark brown
constexpr COLORREF kTextDim   = RGB(0x7C, 0x6D, 0x63);       // secondary text (4.5:1 on cream)
constexpr COLORREF kLine      = RGB(0xE4, 0xD3, 0xC2);       // hairlines
constexpr COLORREF kIcon      = RGB(0x5E, 0x82, 0x7A);       // sage, darkened for icons
constexpr COLORREF kHover     = RGB(0xFA, 0xDF, 0xDA);       // light pink
constexpr COLORREF kSelect    = kPink;
constexpr COLORREF kCoralDark = RGB(0xD9, 0x6A, 0x68);       // pressed / border of coral
constexpr COLORREF kYellowDark = RGB(0xD9, 0x9E, 0x3E);
// the dark song panel (AIMP style) in the same warm family
constexpr COLORREF kPlayerBack = RGB(0x33, 0x2C, 0x29);
constexpr COLORREF kPlayerText = kCream;
constexpr COLORREF kPlayerDim  = RGB(0xB3, 0xA3, 0x97);
constexpr COLORREF kPlayerLine = RGB(0x55, 0x4B, 0x46);

HBRUSH Brush(COLORREF c);                                    // cached, never deleted by the caller
void Fill(HDC dc, const RECT& r, COLORREF c);
void RoundRectFill(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border);

// Segoe MDL2 Assets (Windows 10 / 11) at `px` pixels; cached.
HFONT IconFont(int px);

// ---- flat buttons
enum class ButtonStyle { Toolbar, Primary, Secondary };
// Turns a BUTTON into an owner-drawn flat one (keeps its id, text and WM_COMMAND).
// glyph: an MDL2 character drawn left of the text (0 = none).
void MakeFlatButton(HWND button, ButtonStyle style, wchar_t glyph = 0);
// Call from the parent's WM_DRAWITEM; true if it was one of our buttons.
bool DrawItem(const DRAWITEMSTRUCT* di);
// Width a toolbar button needs for its icon + text.
int ButtonWidth(HWND button, int dpi);

// ---- window chrome
void ApplyTitleBar(HWND top);                                // Windows 11: cream caption, sage border
void ThemeStatusBar(HWND status);                            // painted by us (cream, dim text)
// List view: colours + selection through NM_CUSTOMDRAW (returns the custom-draw result).
void ThemeListView(HWND list);
LRESULT ListCustomDraw(NMLVCUSTOMDRAW* cd);
// The list's header: painted by us. sortColumn < 0 = no arrow; dropColumn = the column with a ⌄.
LRESULT HeaderCustomDraw(NMCUSTOMDRAW* cd, int sortColumn, bool ascending, int dropColumn);
void SetHeaderInfo(int sortColumn, bool ascending, int dropColumn);   // what the header draws (list subclass)

} // namespace mzr::ui::theme
