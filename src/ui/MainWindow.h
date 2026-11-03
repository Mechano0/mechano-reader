#pragma once
// Main window: toolbar + search, archive list (virtual ListView) | preview /
// info pane with a draggable splitter, status bar with progress.
// All archive work runs on the Worker thread; results come back as messages.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/archive/ArchiveEngine.h"
#include "core/cad/Cad.h"
#include "core/preview/Dwg.h"
#include "core/preview/Ooxml.h"
#include "core/preview/Revit.h"
#include "core/preview/Ifc.h"
#include "core/preview/Navisworks.h"
#include "core/preview/AudioTags.h"
#include "core/preview/PreviewDecode.h"
#include "core/update/Updater.h"
#include "ui/MediaBar.h"
#include "ui/SheetView.h"
#include "ui/Theme.h"
#include "ui/MediaPlayer.h"
#include "workers/Worker.h"

struct IWICBitmap;

namespace mzr::ui {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    bool Create(HINSTANCE inst, int show);
    HWND hwnd() const { return hwnd_; }
    HACCEL accelerators() const { return accel_; }
    void OpenArchive(const std::wstring& path, uint32_t codePage = 0);
    // Explorer right-click menu ("MechanoZip.exe /extract-here a.zip" ...): the window shows the
    // progress of one task and closes when it is done (problems are shown in a dialog first).
    enum class ShellAction { None, ExtractHere, ExtractToFolder, AddDialog, AddZip, ZipEach };
    void RunShellAction(ShellAction a, std::vector<std::wstring> paths);
    // Called by the message loop before accelerators: Quick Look keys (Space / arrows).
    bool PreTranslate(MSG* m);

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PreviewProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK VideoProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK QuickLookProcStatic(HWND, UINT, WPARAM, LPARAM);

    // F1 Quick Look: Space opens the selected file in a big resizable window.
    // The preview pane itself is moved into that window (SetParent), so every
    // preview type, zoom and the media controls work there with no extra code.
    void ToggleQuickLook();
    void OpenQuickLook();
    void CloseQuickLook();
    void UpdateQuickLookTitle();
    HWND ql_ = nullptr;
    bool qlOpen_ = false;
    LRESULT WndProc(UINT, WPARAM, LPARAM);
    LRESULT PreviewProc(HWND, UINT, WPARAM, LPARAM);

    // setup / layout
    void CreateChildren();
    void CreateLateChildren();
    void ApplyFonts();
    void Layout();
    int  Dpi(int px) const { return MulDiv(px, dpi_, 96); }

    // view
    void RebuildView();                 // current folder or search results -> view_
    void SortView();
    void NavigateTo(uint32_t folderNode);
    void GoUp();
    void ActivateSelection();           // Enter / double click
    void UpdatePreview();
    void UpdateStatus();
    void UpdateTitleAndPath();
    std::vector<uint32_t> SelectedNodes() const;
    void OnGetDispInfo(NMLVDISPINFOW* di);
    int  IconFor(uint32_t node);
    void PaintPreview(HWND wnd, HDC dc);
    void PaintFileContent(HDC dc, const RECT& area);
    void PaintFooter(HDC dc, const RECT& area, uint32_t node);
    void RequestPreview(uint32_t node);
    void RequestDocPage(uint32_t page, uint32_t renderWidth = 0);   // renderWidth: re-render the same page sharper
    void ResetPreview();
    void LayoutPreviewChildren();
    void UpdatePreviewButtons();
    RECT PreviewContentRect() const;
    int  SeekRowH() const;
    void SetPreviewImage(preview::Image&& img);

    // Zoom (pictures, PDF pages, slides: drawn scaled + panned; text / Word / Excel: font zoom).
    // zoom_ is relative to "fit" (1.0 = whole file visible).
    bool CanZoom() const;
    void ZoomRange(double& lo, double& hi) const;
    void SetZoom(double z, const POINT* anchor = nullptr);       // anchor: pane point kept still
    void ApplyChildZoom();
    void ImagePlacement(const RECT& area, double& x0, double& y0, double& dw, double& dh);
    bool ImagePannable();
    std::wstring ZoomLabel();
    // Video / audio seek bar.
    void UpdateSeekBar();
    void OnSeekScroll(int code);

    // The "detail" column: one column whose content is chosen from a dropdown
    // in its header (Packed / Ratio / Modified / CRC32 / Type / Method).
    enum class Detail { Packed, Ratio, Modified, Crc, Type, Method, Count };
    void ShowDetailMenu();
    void SetDetail(Detail d);
    std::wstring TypeNameOf(const std::wstring& name);

    // commands
    void OnCommand(int id);
    void CmdOpen();
    void CmdNew(std::vector<std::wstring> files = {});
    void CmdAdd(std::vector<std::wstring> files = {});
    void CmdExtract(bool here);
    void StartExtract(std::vector<uint32_t> indices, archive::ExtractOptions eo);
    void StartCreate(std::vector<std::wstring> files, std::wstring path, archive::CompressionSettings cs);
    // App menu (≡ at the end of the toolbar): Restart + app updates (core/update, like Mechano DM).
    void ShowAppMenu();
    void RestartApp();
    void CheckForUpdates(bool manual);
    void AskToUpdate(bool manual);
    void StartUpdateDownload(bool thenRestart);
    void OnUpdateMessage(LPARAM lp);
    void SetMenuButtonText();
    bool IsInstalledCopy() const;
    std::wstring UpdateFolder() const;
    struct UpdateMsg;
    std::shared_ptr<update::Manifest> updManifest_;     // newer version found (signed)
    std::wstring updReadyFile_;                         // verified Setup, waiting to be run
    bool updChecking_ = false, updDownloading_ = false, updRestartWhenReady_ = false, updLaunched_ = false;
    int updPercent_ = -1;
    HWND menuBtn_ = nullptr;
    void ShellExtract(bool toFolder, std::wstring base = {});   // base: "" = next to the archive
    // F14 batch: several archives extracted (or several items zipped) one after the other in this
    // window, one summary at the end. Esc stops the rest.
    struct Batch {
        bool active = false;
        ShellAction action = ShellAction::None;     // ExtractHere / ExtractToFolder / ZipEach
        std::wstring destBase;                      // "" = next to each item
        std::vector<std::wstring> items;
        size_t next = 0, ok = 0;
        uint64_t files = 0, bytes = 0;
        std::vector<std::pair<std::wstring, std::wstring>> problems;   // item name, what went wrong
        bool stopped = false;
    };
    Batch batch_;
    void StartBatch(ShellAction a, std::vector<std::wstring> items, std::wstring destBase = {});
    void NextBatchItem();
    void FinishBatch();
    void AskBatch(std::vector<std::wstring> archives);     // several archives dropped / opened: what to do?
    ShellAction shellAction_ = ShellAction::None;   // waits for the archive to open
    bool shellClose_ = false;                       // close the window when the task is done
    void CmdExtractAndOpen();
    void CmdTest();
    void CmdDelete();
    void CmdEncodingMenu();
    void OnDropFiles(HDROP drop);
    // Phase 5: right-click menu, drag out to Explorer, shortcut list (F1).
    void ShowContextMenu(LPARAM screenPt);
    void CmdDragOut();
    void DoDragOut(const std::wstring& dir);
    void CopyToClipboard(const std::wstring& text);
    void ShowShortcuts();
    // Rename inside the archive: F2 / right-click menu / slow second click, edited in place.
    void BeginRename();
    void ApplyRename(uint32_t node, std::wstring newName);
    std::wstring selectAfterReload_;                       // name to select when the archive comes back
    bool draggingOut_ = false;

    // jobs
    bool BeginJob(const std::wstring& status);
    void EndJob();
    archive::PasswordProvider MakePasswordProvider();

    struct UiProgress;
    struct JobResult;
    struct PreviewResult;
    void OnPreviewResult(PreviewResult* r);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr, list_ = nullptr, preview_ = nullptr, status_ = nullptr, search_ = nullptr, pathLabel_ = nullptr;
    HWND pvOpenBtn_ = nullptr, pvExtractBtn_ = nullptr;
    std::vector<HWND> toolButtons_;
    HACCEL accel_ = nullptr;
    HFONT font_ = nullptr, boldFont_ = nullptr;
    int dpi_ = 96;
    int split_ = 0;                     // list width in px (0 = default 58%)
    bool dragging_ = false;
    int barH_ = 0;                      // toolbar band height (painted by WM_PAINT)
    RECT searchFrame_{};                // rounded frame around the search box

    // archive state (archive_ is only touched by the worker while a job runs)
    // shared_ptr so a queued preview keeps "its" archive alive even if a new one is opened.
    std::shared_ptr<archive::Archive> archive_;
    std::shared_ptr<const archive::ArchiveIndex> index_;
    uint32_t codePage_ = 0;
    uint32_t folder_ = 0;
    std::vector<uint32_t> view_;
    bool searchMode_ = false;
    int sortCol_ = 0;
    bool sortAsc_ = true;
    std::unordered_map<std::wstring, int> iconCache_;
    std::unordered_map<std::wstring, std::wstring> typeNameCache_;
    Detail detail_ = Detail::Modified;

    // preview state (UI thread only, except previewGen_ which the worker reads)
    // Image: picture, PDF page, PowerPoint slide.  Rich: Word.  Grid: Excel.
    enum class PvState { Empty, Loading, Image, Text, Rich, Grid, Media, Failed };
    PvState pvState_ = PvState::Empty;
    preview::Kind pvKind_ = preview::Kind::None;
    // PDF: the file stays in RAM while it is shown, so page changes do not touch the archive.
    std::shared_ptr<const std::vector<uint8_t>> docBytes_;
    uint32_t docPage_ = 0, docPages_ = 0;
    // Video / audio: played from a private temp copy (the media engine must seek).
    std::unique_ptr<MediaPlayer> media_;
    std::wstring mediaDir_;
    HWND pvVideo_ = nullptr;
    HWND pvBtnA_ = nullptr, pvBtnB_ = nullptr;              // ◀ ▶  or  Pause / Sound
    uint32_t pvNode_ = 0;                                   // node shown / being loaded
    std::shared_ptr<std::atomic<uint64_t>> previewGen_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<archive::CancelToken> previewCancel_;
    preview::Image pvImage_;
    HBITMAP pvScaled_ = nullptr;                            // pvImage_ scaled to the pane
    int pvScaledW_ = 0, pvScaledH_ = 0;
    std::wstring pvMessage_;                                // why there is no preview
    std::wstring pvDwgVersion_;                             // AutoCAD drawing: shown in the footer
    // F7 CAD view (DWG through the dwg2dxf helper, or DXF): parsed once, views rendered on demand.
    std::shared_ptr<const cad::Drawing> cad_;
    bool cadBusy_ = false;                                  // DWG: thumbnail shown, real drawing still converting
    std::wstring cadNote_;                                  // why the real drawing is not shown (footer)
    bool PagedImage() const;                                // PDF page, slide or CAD view with ◀ ▶
    bool pvTruncated_ = false;
    HWND pvText_ = nullptr;                                 // read-only text view
    HWND pvRich_ = nullptr;                                 // Word: read-only rich text (RTF we generate)
    HWND pvGrid_ = nullptr;                                 // Excel: sheetView_.hwnd()
    SheetView sheetView_;                                   // Excel: drawn like Excel (widths, colours, merges, RTL)
    std::shared_ptr<const preview::Sheet> sheet_;
    HFONT monoFont_ = nullptr;
    bool pvTextUiFont_ = false;                             // text view uses the UI font (Arabic) not Consolas

    // zoom / pan
    double zoom_ = 1.0;
    double viewCx_ = 0.5, viewCy_ = 0.5;                    // view centre, as a fraction of the picture
    IWICBitmap* wicSrc_ = nullptr;                          // pvImage_ wrapped once for WIC scaling
    double pvScaledS_ = 0;                                  // cache key of pvScaled_: scale + source rect
    RECT pvScaledSrc_{};
    HFONT zoomFont_ = nullptr;
    bool panning_ = false;
    POINT panLast_{};
    HWND pvZoomOut_ = nullptr, pvZoomPct_ = nullptr, pvZoomIn_ = nullptr;
    // seek bar: [« 10 s] [=====o=====] [10 s »]
    HWND pvBack_ = nullptr, pvSeek_ = nullptr, pvFwd_ = nullptr;
    bool seeking_ = false;
    // video: VLC-style control bar (audio keeps the simple row above)
    MediaBar mediaBar_;
    bool IsVideo() const { return pvState_ == PvState::Media && pvKind_ == preview::Kind::Video; }
    // audio: AIMP-style dark song panel (cover, title, artist, album, format, big time)
    void PaintAudioPanel(HDC dc, const RECT& area);
    void StepMediaFile(int dir);                            // ⏮ / ⏭: previous / next audio file in the list
    std::shared_ptr<const preview::AudioTags> audio_;
    HBITMAP coverBmp_ = nullptr;                            // audio_->cover as a DIB
    HFONT audioTitleFont_ = nullptr, audioTimeFont_ = nullptr, audioGlyphFont_ = nullptr;
    RECT audioTimeRc_{};                                    // click: elapsed <-> remaining
    bool audioRemaining_ = false;
    bool carrySound_ = false;                               // ⏮ / ⏭ keep the sound the user turned on
    float carryVolume_ = 1.0f;

    // job state
    std::shared_ptr<archive::CancelToken> cancel_;
    std::shared_ptr<UiProgress> progress_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> closing_{false};
    std::wstring statusMessage_;

    // Declared LAST so it is destroyed FIRST: the worker thread is joined
    // before anything a running job could touch goes away.
    workers::Worker worker_;
};

} // namespace mzr::ui
