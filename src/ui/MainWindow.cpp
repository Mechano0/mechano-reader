#include "ui/MainWindow.h"

#include <windowsx.h>
#include <commoncontrols.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <richedit.h>
#include <richole.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>
#include <thread>

#include "core/archive/SmartExtract.h"
#include "core/filesystem/FileSystem.h"
#include "core/preview/PdfRender.h"
#include "core/security/PathSanitizer.h"
#include "core/cad/CadConvert.h"
#include "ui/Dialogs.h"
#include "utils/Format.h"
#include "utils/Log.h"
#include "utils/Settings.h"
#include "assets/version.h"

namespace mzr::ui {
namespace {

using archive::ArchiveIndex;

constexpr wchar_t kMainClass[] = L"MechanoZipReader.Main";
constexpr wchar_t kPreviewClass[] = L"MechanoZipReader.Preview";
constexpr wchar_t kQuickLookClass[] = L"MechanoZipReader.QuickLook";

enum : UINT {
    WM_APP_OPENED = WM_APP + 1,   // lParam = OpenResult*
    WM_APP_JOBDONE,               // lParam = JobResult*
    WM_APP_PROGRESS,
    WM_APP_ASKPW,                 // wParam = const std::wstring* name, lParam = std::optional<std::wstring>*
    WM_APP_PREVIEW,               // lParam = PreviewResult*
    WM_APP_MEDIA,                 // media player state changed
    WM_APP_LATEINIT,              // window is on screen: create what the first picture does not need
    WM_APP_ICONS,                 // lParam = HIMAGELIST (system file icons, loaded on a background thread)
    WM_APP_BATCHNEXT,             // F14 batch: start the next item
    WM_APP_UPDATE,                // lParam = UpdateMsg* (app update check / download, background thread)
};

constexpr wchar_t kVideoClass[] = L"MechanoZipReader.Video";

enum : int {
    ID_OPEN = 1001, ID_NEW, ID_ADD, ID_EXTRACT_TO, ID_EXTRACT_HERE, ID_TEST, ID_DELETE, ID_UP, ID_ENCODING, ID_MENU,
    ID_APP_RESTART = 1200, ID_APP_CHECK, ID_APP_UPD_NOTIFY, ID_APP_UPD_AUTO, ID_APP_UPD_OFF, ID_APP_PAGE, ID_APP_SHORTCUTS,
    ID_SEARCH = 1050, ID_LIST, ID_PATH,
    ID_PV_OPEN = 1060, ID_PV_EXTRACT, ID_PV_A, ID_PV_B, ID_PV_ZOOMOUT, ID_PV_ZOOMFIT, ID_PV_ZOOMIN, ID_PV_BACK, ID_PV_FWD,
    ID_ACC_SELECTALL = 1080, ID_ACC_CANCEL, ID_ACC_FIND, ID_ACC_BACK, ID_ACC_ENTER, ID_ACC_REFRESH,
    ID_ACC_ZOOMIN, ID_ACC_ZOOMOUT, ID_ACC_ZOOMFIT, ID_ACC_HELP,
    ID_CTX_QUICKLOOK, ID_CTX_OPEN, ID_CTX_COPYNAME, ID_CTX_COPYPATH, ID_CTX_RENAME,
    ID_ENC_BASE = 1100,
    ID_DETAIL_BASE = 1200,
};

enum Col { ColName, ColSize, ColDetail, ColCount };

struct DetailItem { const wchar_t* label; int fmt; };
const DetailItem kDetails[] = {
    {L"Packed", LVCFMT_RIGHT}, {L"Ratio", LVCFMT_RIGHT}, {L"Modified", LVCFMT_LEFT},
    {L"CRC32", LVCFMT_LEFT},   {L"Type", LVCFMT_LEFT},   {L"Method", LVCFMT_LEFT},
};

constexpr UINT_PTR kTimerSearch = 1, kTimerPreview = 2, kTimerUpdate = 3;
// timers of the preview pane window
constexpr UINT_PTR kTimerMedia = 10, kTimerDocZoom = 11;
constexpr double kZoomStep = 1.25;

// Code pages offered in the "Names" menu (F12).
struct CodePageItem { uint32_t cp; const wchar_t* label; };
const CodePageItem kCodePages[] = {
    {0,     L"Automatic (recommended)"},
    {65001, L"UTF-8"},
    {720,   L"Arabic - DOS (CP720)"},
    {864,   L"Arabic - DOS (CP864)"},
    {1256,  L"Arabic - Windows (1256)"},
    {437,   L"Western - DOS (CP437)"},
    {850,   L"Western European - DOS (CP850)"},
    {1252,  L"Western - Windows (1252)"},
    {866,   L"Cyrillic - DOS (CP866)"},
    {932,   L"Japanese (Shift-JIS)"},
    {936,   L"Chinese Simplified (GBK)"},
};

// Extensions that run code when opened: ask before "Extract & Open" (F8 basic).
// The list and the trailing-dot / hidden-character handling live in the security layer (tested there).
bool IsRiskyExtension(const std::wstring& name) { return security::IsRiskyFileName(name); }

std::wstring Ext(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    std::wstring e = name.substr(dot);
    CharLowerBuffW(e.data(), static_cast<DWORD>(e.size()));
    return e;
}

std::wstring FileNameOf(const std::wstring& p) { return p.substr(p.find_last_of(L"\\/") + 1); }
std::wstring FolderOf(const std::wstring& p) {
    const auto s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

} // namespace

// ============================================================ job plumbing

struct MainWindow::UiProgress final : archive::ProgressSink {
    HWND hwnd;
    std::atomic<uint64_t> total{0}, done{0};
    std::mutex m;
    std::wstring item;
    std::chrono::steady_clock::time_point last{};
    explicit UiProgress(HWND h) : hwnd(h) {}
    void OnTotal(uint64_t t) override { total = t; }
    void OnCompleted(uint64_t d) override {
        done = d;
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::milliseconds(100)) { last = now; PostMessageW(hwnd, WM_APP_PROGRESS, 0, 0); }
    }
    void OnCurrentItem(const std::wstring& n) override { std::lock_guard l(m); item = n; }
    std::wstring Item() { std::lock_guard l(m); return item; }
};

struct OpenResult {
    Result<std::unique_ptr<archive::Archive>> result{ErrorInfo{}};
    uint32_t codePage = 0;
    double ms = 0;
};

struct MainWindow::JobResult {
    std::wstring title;            // "Extract", "Test" ...
    std::wstring doneText;         // success sentence
    archive::OperationReport report;
    std::optional<ErrorInfo> error;
    bool indexChanged = false;     // archive was modified -> refresh
    std::wstring openAfter;        // open this archive when done (New archive)
    std::wstring launchFile;       // Extract & Open
    bool riskConfirmed = false;    // the user already agreed to open a file that can run code
    std::wstring dragDir;          // drag out: files extracted here, then handed to Explorer
    bool quiet = false;            // only status text if everything went fine
};

struct MainWindow::PreviewResult {
    uint64_t gen = 0;
    uint32_t node = 0;
    preview::Kind kind = preview::Kind::None;
    preview::Image image;
    std::wstring text;
    bool truncated = false;
    std::shared_ptr<const std::vector<uint8_t>> docBytes;
    uint32_t docPage = 0, docPages = 0;
    std::wstring mediaFile, mediaDir;
    std::shared_ptr<const preview::AudioTags> audio;   // audio: song details + cover
    std::string rtf;                                   // Word
    std::shared_ptr<const preview::Sheet> sheet;       // Excel
    std::wstring dwgVersion;                           // AutoCAD drawing: "AutoCAD 2018 - 2025 drawing"
    std::shared_ptr<const cad::Drawing> cad;           // F7: the real drawing (DWG converted / DXF)
    bool cadPending = false;                           // DWG thumbnail now, the real drawing follows
    std::wstring cadNote;                              // the real drawing could not be shown: why
    std::optional<ErrorInfo> error;
};

namespace {
bool IsDoc(preview::Kind k) {
    return k == preview::Kind::Pdf || k == preview::Kind::Docx || k == preview::Kind::Xlsx || k == preview::Kind::Pptx;
}

// Worker side: one page / slide / sheet of a document that is already in RAM.
void RenderDoc(preview::Kind kind, const preview::Bytes& bytes, uint32_t page, uint32_t width, preview::Image& image,
               uint32_t& docPage, uint32_t& docPages, std::string& rtf, std::shared_ptr<const preview::Sheet>& sheet,
               std::optional<ErrorInfo>& error) {
    if (kind == preview::Kind::Docx) {
        // Text width of the view in twips: pane minus RichEdit margins and the scroll bar.
        const UINT dpi = std::max<UINT>(96, GetDpiForSystem());
        const int usable = std::max(200, static_cast<int>(width) - MulDiv(56, static_cast<int>(dpi), 96));
        auto r = preview::DocxToRtf(bytes, static_cast<uint32_t>(MulDiv(usable, 1440, static_cast<int>(dpi))));
        if (r) rtf = std::move(r.value()); else error = r.error();
        return;
    }
    if (kind == preview::Kind::Xlsx) {
        auto s = preview::ReadXlsxSheet(bytes, page);
        if (!s) { error = s.error(); return; }
        docPage = s.value().index;
        docPages = static_cast<uint32_t>(s.value().sheetNames.size());
        sheet = std::make_shared<const preview::Sheet>(std::move(s.value()));
        return;
    }
    auto pg = kind == preview::Kind::Pptx ? preview::RenderPptxSlide(bytes, page, width)
                                          : preview::RenderPdfPage(*bytes, page, width);
    if (!pg) { error = pg.error(); return; }
    docPage = pg.value().pageIndex;
    docPages = pg.value().pageCount;
    image = std::move(pg.value().image);
}

// RichEdit needs storage for every picture it reads from RTF (pictures become
// static OLE objects). Storage lives in RAM; nothing can be pasted, dragged or
// opened (the control is read-only and our RTF never contains \object).
class RichOleCallback final : public IRichEditOleCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IRichEditOleCallback) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }     // static lifetime
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetNewStorage(LPSTORAGE* stg) override {
        if (!stg) return E_POINTER;
        *stg = nullptr;
        ILockBytes* lb = nullptr;
        HRESULT hr = CreateILockBytesOnHGlobal(nullptr, TRUE, &lb);
        if (FAILED(hr)) return hr;
        hr = StgCreateDocfileOnILockBytes(lb, STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE, 0, stg);
        lb->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetInPlaceContext(LPOLEINPLACEFRAME*, LPOLEINPLACEUIWINDOW*, LPOLEINPLACEFRAMEINFO) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShowContainerUI(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInsertObject(LPCLSID, LPSTORAGE, LONG) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteObject(LPOLEOBJECT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryAcceptData(LPDATAOBJECT, CLIPFORMAT*, DWORD, BOOL, HGLOBAL) override { return E_FAIL; }   // no paste / drop
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClipboardData(CHARRANGE*, DWORD, LPDATAOBJECT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDragDropEffect(BOOL, DWORD, LPDWORD effect) override { if (effect) *effect = DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContextMenu(WORD, LPOLEOBJECT, CHARRANGE*, HMENU*) override { return E_NOTIMPL; }
};
RichOleCallback g_richOle;

LRESULT CALLBACK CtrlWheelToParent(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL))
        return SendMessageW(GetParent(h), m, w, l);
    return DefSubclassProc(h, m, w, l);
}

// Audio seek bar: a click on the bar jumps straight to that point (a plain
// trackbar only moves one page towards the click). The thumb is moved under
// the cursor first, so the normal handling then turns the press into a drag.
LRESULT CALLBACK ClickToPosition(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_LBUTTONDOWN && IsWindowEnabled(h)) {
        RECT ch{}, th{};
        SendMessageW(h, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&ch));
        SendMessageW(h, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&th));
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!PtInRect(&th, p)) {
            const int lo = static_cast<int>(SendMessageW(h, TBM_GETRANGEMIN, 0, 0));
            const int hi = static_cast<int>(SendMessageW(h, TBM_GETRANGEMAX, 0, 0));
            const int half = (th.right - th.left) / 2;
            const int x0 = ch.left + half, x1 = ch.right - half;
            const double f = x1 > x0 ? std::clamp(static_cast<double>(p.x - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
            const int pos = lo + static_cast<int>(f * (hi - lo) + 0.5);
            SendMessageW(h, TBM_SETPOS, TRUE, pos);
            SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, pos), reinterpret_cast<LPARAM>(h));
        }
    }
    return DefSubclassProc(h, m, w, l);
}

std::wstring FormatTime(double s) {
    const long long t = static_cast<long long>(s + 0.5);
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(b, L"%lld:%02lld", t / 60, t % 60);
    return b;
}

// RichEdit reads our RTF through this callback.
DWORD CALLBACK RtfStreamIn(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* got) {
    auto* s = reinterpret_cast<std::pair<const std::string*, size_t>*>(cookie);
    const size_t n = std::min<size_t>(static_cast<size_t>(cb), s->first->size() - s->second);
    memcpy(buf, s->first->data() + s->second, n);
    s->second += n;
    *got = static_cast<LONG>(n);
    return 0;
}
} // namespace

namespace {
// Delete a preview temp folder (a media file copy). Best effort: anything left
// over is removed at the next start (CleanTempFolder in main.cpp).
void DeleteTempDir(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring from = dir;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// Worker side (F7): the real drawing. The entry goes through the normal safe extraction into a
// private temp folder; a DWG is converted by the dwg2dxf helper (its own limited process) with a
// plain ASCII name; the DXF is read by the CAD core. The temp folder is always deleted.
// Worker side: one entry -> a private temp folder (normal safe extraction, no Mark of the Web, never
// opened by the shell). Returns the file path ("" + err on failure); the caller deletes `dir`.
std::wstring ExtractPreviewCopy(archive::Archive& arc, uint32_t arcIndex, const std::wstring& strip, const wchar_t* prefix,
                                archive::CancelToken& cancel, const std::function<bool()>& stale, ErrorInfo& err, std::wstring& dir) {
    dir = fs::NewTempSubfolder(prefix);
    if (dir.empty()) { err = ErrorInfo{ErrorCode::Io, L"The temporary folder for the preview could not be created.", L""}; return {}; }
    archive::ExtractOptions eo;
    eo.destination = dir;
    eo.stripPrefix = strip;
    eo.propagateMotW = false;
    const auto rep = arc.Extract({arcIndex}, eo, nullptr, cancel);
    if (rep.cancelled || cancel.IsCancelled() || stale()) { err = ErrorInfo{ErrorCode::Cancelled, L"Cancelled.", L""}; return {}; }
    std::wstring src;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { src = dir + L"\\" + fd.cFileName; break; } } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (rep.filesDone != 1 || src.empty()) {
        err = ErrorInfo{ErrorCode::Corrupted, rep.wrongPassword ? L"This file is password protected. Extract it to open it."
                                                                : L"The file could not be prepared for the preview.",
                        rep.issues.empty() ? L"" : rep.issues[0].friendly};
        return {};
    }
    return src;
}

std::shared_ptr<const cad::Drawing> LoadCad(archive::Archive& arc, uint32_t arcIndex, const std::wstring& strip, bool dwg,
                                            archive::CancelToken& cancel, const std::function<bool()>& stale, ErrorInfo& err) {
    std::wstring dir;
    const std::wstring src = ExtractPreviewCopy(arc, arcIndex, strip, L"cad-", cancel, stale, err, dir);
    struct Cleanup { std::wstring d; ~Cleanup() { DeleteTempDir(d); } } cleanup{dir};
    if (src.empty()) return nullptr;
    std::wstring dxf = src;
    if (dwg) {
        const std::wstring in = dir + L"\\in.dwg";               // dwg2dxf cannot open Unicode (Arabic) names
        if (!MoveFileW(src.c_str(), in.c_str())) { err = ErrorInfo{ErrorCode::Io, L"The drawing could not be prepared.", L"MoveFile"}; return nullptr; }
        dxf = dir + L"\\out.dxf";
        auto c = cad::ConvertDwgToDxf(cad::FindDwgConverter(), in, dxf, [&] { return cancel.IsCancelled() || stale(); });
        if (!c) { err = c.error(); return nullptr; }
        DeleteFileW(in.c_str());
    }
    auto d = cad::ReadDxf(dxf);
    if (!d) { err = d.error(); return nullptr; }
    return std::make_shared<const cad::Drawing>(std::move(d.value()));
}
} // namespace

MainWindow::MainWindow() = default;
MainWindow::~MainWindow() {
    if (media_) media_->Close();
    DeleteTempDir(mediaDir_);
    if (previewCancel_) previewCancel_->Cancel();
    if (pvScaled_) DeleteObject(pvScaled_);
    if (wicSrc_) wicSrc_->Release();
    if (zoomFont_) DeleteObject(zoomFont_);
    if (monoFont_) DeleteObject(monoFont_);
    if (coverBmp_) DeleteObject(coverBmp_);
    for (HFONT f : {audioTitleFont_, audioTimeFont_, audioGlyphFont_}) if (f) DeleteObject(f);
    if (font_) DeleteObject(font_);
    if (boldFont_) DeleteObject(boldFont_);
    if (accel_) DestroyAcceleratorTable(accel_);
}

bool MainWindow::BeginJob(const std::wstring& status) {
    if (busy_) {
        MessageBeep(MB_ICONWARNING);
        statusMessage_ = L"Please wait - another operation is running (Esc cancels it).";
        UpdateStatus();
        return false;
    }
    // A drawing still converting for the preview (can take seconds) would hold the worker:
    // the user's own task goes first, the preview conversion stops.
    if ((pvKind_ == preview::Kind::Dwg || pvKind_ == preview::Kind::Dxf) && (cadBusy_ || pvState_ == PvState::Loading) && previewCancel_) {
        previewCancel_->Cancel();
        cadBusy_ = false;
        cadNote_ = L"full view stopped for another task (select the file again to show it)";
        if (pvState_ == PvState::Loading) { pvState_ = PvState::Failed; pvMessage_ = L"The drawing view was stopped for another task.\nSelect the file again to show it."; }
        if (preview_) InvalidateRect(preview_, nullptr, TRUE);
    }
    busy_ = true;
    cancel_ = std::make_shared<archive::CancelToken>();
    progress_ = std::make_shared<UiProgress>(hwnd_);
    statusMessage_ = status + L"  (Esc to cancel)";
    if (batch_.active)
        statusMessage_ = L"[" + fmt::Number(batch_.next) + L" of " + fmt::Number(batch_.items.size()) + L"]  " + status +
                         L"  (Esc stops the rest)";
    UpdateStatus();
    SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
    return true;
}

void MainWindow::EndJob() {
    busy_ = false;
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (closing_) DestroyWindow(hwnd_);
}

archive::PasswordProvider MainWindow::MakePasswordProvider() {
    HWND h = hwnd_;
    return [this, h](const std::wstring& name) -> std::optional<std::wstring> {
        if (closing_) return std::nullopt;
        std::optional<std::wstring> res;
        // Runs on the worker: the dialog must be shown by the UI thread.
        SendMessageW(h, WM_APP_ASKPW, reinterpret_cast<WPARAM>(&name), reinterpret_cast<LPARAM>(&res));
        return res;
    };
}

// ============================================================ window creation

bool MainWindow::Create(HINSTANCE inst, int show) {
    inst_ = inst;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1 /*IDI_APP*/), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.hbrBackground = theme::Brush(theme::kBack);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW pc{sizeof(pc)};
    pc.lpfnWndProc = PreviewProcStatic;
    pc.style = CS_DBLCLKS;                       // double-click = zoom to actual size / back to fit
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = theme::Brush(theme::kBack);
    pc.lpszClassName = kPreviewClass;
    RegisterClassExW(&pc);

    WNDCLASSEXW vc{sizeof(vc)};
    vc.lpfnWndProc = VideoProcStatic;
    vc.style = CS_DBLCLKS;
    vc.hInstance = inst;
    vc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    vc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vc.lpszClassName = kVideoClass;
    RegisterClassExW(&vc);

    WNDCLASSEXW qc{sizeof(qc)};
    qc.lpfnWndProc = QuickLookProcStatic;
    qc.hInstance = inst;
    qc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    qc.hIcon = wc.hIcon;
    qc.hbrBackground = theme::Brush(theme::kBack);
    qc.lpszClassName = kQuickLookClass;
    RegisterClassExW(&qc);

    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, L"Mechano Reader", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 720, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'O', ID_OPEN},           {FCONTROL | FVIRTKEY, 'N', ID_NEW},
        {FCONTROL | FVIRTKEY, 'E', ID_EXTRACT_TO},     {FALT | FVIRTKEY, 'E', ID_EXTRACT_HERE},
        {FALT | FVIRTKEY, 'A', ID_ADD},                {FALT | FVIRTKEY, 'T', ID_TEST},
        {FCONTROL | FVIRTKEY, 'A', ID_ACC_SELECTALL},  {FVIRTKEY, VK_ESCAPE, ID_ACC_CANCEL},
        {FCONTROL | FVIRTKEY, 'F', ID_ACC_FIND},       {FVIRTKEY, VK_F3, ID_ACC_FIND},
        {FVIRTKEY, VK_F5, ID_ACC_REFRESH},             {FALT | FVIRTKEY, VK_UP, ID_UP},
        // preview zoom: Ctrl + / Ctrl - / Ctrl 0 (main keyboard and number pad)
        {FCONTROL | FVIRTKEY, VK_OEM_PLUS, ID_ACC_ZOOMIN},   {FCONTROL | FVIRTKEY, VK_ADD, ID_ACC_ZOOMIN},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, ID_ACC_ZOOMIN},
        {FCONTROL | FVIRTKEY, VK_OEM_MINUS, ID_ACC_ZOOMOUT}, {FCONTROL | FVIRTKEY, VK_SUBTRACT, ID_ACC_ZOOMOUT},
        {FCONTROL | FVIRTKEY, '0', ID_ACC_ZOOMFIT},          {FCONTROL | FVIRTKEY, VK_NUMPAD0, ID_ACC_ZOOMFIT},
        {FVIRTKEY, VK_F1, ID_ACC_HELP},
    };
    accel_ = CreateAcceleratorTableW(acc, static_cast<int>(std::size(acc)));

    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    PostMessageW(hwnd_, WM_APP_LATEINIT, 0, 0);
    return true;
}

void MainWindow::CreateChildren() {
    // Flat toolbar: Segoe MDL2 icon + label (theme: cream band, pink hover).
    struct Btn { int id; const wchar_t* text; wchar_t glyph; };
    const Btn btns[] = {{ID_OPEN, L"Open", 0xE8E5},           {ID_NEW, L"New", 0xE8A5},       {ID_ADD, L"Add", 0xE710},
                        {ID_EXTRACT_TO, L"Extract To...", 0xE8DE}, {ID_EXTRACT_HERE, L"Extract Here", 0xE896},
                        {ID_TEST, L"Test", 0xE73E},           {ID_DELETE, L"Delete", 0xE74D}, {ID_ENCODING, L"Names", 0xE8C1},
                        {ID_UP, L"Up", 0xE74A}};
    for (const auto& b : btns) {
        HWND btn = CreateWindowExW(0, WC_BUTTONW, b.text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(b.id)), inst_, nullptr);
        theme::MakeFlatButton(btn, theme::ButtonStyle::Toolbar, b.glyph);
        toolButtons_.push_back(btn);
    }
    // App menu: Restart, updates. Its label becomes "Update" when a new version is found.
    menuBtn_ = CreateWindowExW(0, WC_BUTTONW, L"Menu", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MENU)), inst_, nullptr);
    theme::MakeFlatButton(menuBtn_, theme::ButtonStyle::Toolbar, 0xE700);
    toolButtons_.push_back(menuBtn_);

    // The search box: no 3D edge; a rounded frame is painted around it (WM_PAINT).
    search_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(ID_SEARCH), inst_, nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search in archive (Ctrl+F)"));

    pathLabel_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE,
                                 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(ID_PATH), inst_, nullptr);

    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(ID_LIST), inst_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
    SetWindowTheme(list_, L"Explorer", nullptr);
    theme::ThemeListView(list_);                // warm paper rows, pink selection, our own header
    theme::SetHeaderInfo(sortCol_, sortAsc_, ColDetail);

    // (File icons: the system image list is attached in CreateLateChildren, after the window shows.)

    // Remembered choice for the detail column (HKCU per user, or settings.ini in portable mode).
    const uint32_t saved = settings::GetNumber(L"DetailColumn", static_cast<uint32_t>(Detail::Modified));
    if (saved < static_cast<uint32_t>(Detail::Count)) detail_ = static_cast<Detail>(saved);

    const struct { const wchar_t* t; int w; int fmt; } cols[ColCount] = {
        {L"Name", 320, LVCFMT_LEFT}, {L"Size", 90, LVCFMT_RIGHT},
        // The third column has a small arrow in its header: click it to choose what it shows.
        {kDetails[static_cast<int>(detail_)].label, 150, kDetails[static_cast<int>(detail_)].fmt | LVCFMT_SPLITBUTTON}};
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = const_cast<wchar_t*>(cols[i].t);
        c.cx = Dpi(cols[i].w);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(list_, i, &c);
    }

    preview_ = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                               0, 0, 0, 0, hwnd_, nullptr, inst_, this);
    pvText_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
                              0, 0, 0, 0, preview_, nullptr, inst_, nullptr);
    SendMessageW(pvText_, EM_SETLIMITTEXT, 0, 0);
    // Ctrl + mouse wheel over the text / Word / Excel views zooms through the pane (one zoom for all).
    SetWindowSubclass(pvText_, CtrlWheelToParent, 1, 0);
    // (Word and Excel views: created in CreateLateChildren, after the window shows.)
    pvVideo_ = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, preview_, nullptr, inst_, this);
    pvBtnA_ = CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | BS_PUSHBUTTON,
                              0, 0, 0, 0, preview_, reinterpret_cast<HMENU>(ID_PV_A), inst_, nullptr);
    pvBtnB_ = CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | BS_PUSHBUTTON,
                              0, 0, 0, 0, preview_, reinterpret_cast<HMENU>(ID_PV_B), inst_, nullptr);
    auto smallBtn = [&](int id, const wchar_t* text) {
        return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, preview_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    };
    pvZoomOut_ = smallBtn(ID_PV_ZOOMOUT, L"−");
    pvZoomPct_ = smallBtn(ID_PV_ZOOMFIT, L"Fit");
    pvZoomIn_ = smallBtn(ID_PV_ZOOMIN, L"+");
    pvBack_ = smallBtn(ID_PV_BACK, L"« 10 s");
    pvFwd_ = smallBtn(ID_PV_FWD, L"10 s »");
    pvSeek_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
                              0, 0, 0, 0, preview_, nullptr, inst_, nullptr);
    SendMessageW(pvSeek_, TBM_SETRANGE, FALSE, MAKELPARAM(0, 1000));
    SendMessageW(pvSeek_, TBM_SETLINESIZE, 0, 10);
    SendMessageW(pvSeek_, TBM_SETPAGESIZE, 0, 50);
    SetWindowSubclass(pvSeek_, ClickToPosition, 1, 0);
    media_ = std::make_unique<MediaPlayer>(hwnd_, WM_APP_MEDIA);
    mediaBar_.Create(preview_, inst_, media_.get(), hwnd_, WM_APP_MEDIA);
    pvOpenBtn_ = CreateWindowExW(0, WC_BUTTONW, L"Open in program", WS_CHILD | BS_PUSHBUTTON,
                                 0, 0, 0, 0, preview_, reinterpret_cast<HMENU>(ID_PV_OPEN), inst_, nullptr);
    pvExtractBtn_ = CreateWindowExW(0, WC_BUTTONW, L"Extract...", WS_CHILD | BS_PUSHBUTTON,
                                    0, 0, 0, 0, preview_, reinterpret_cast<HMENU>(ID_PV_EXTRACT), inst_, nullptr);
    // theme: coral main action, cream secondary buttons
    theme::MakeFlatButton(pvOpenBtn_, theme::ButtonStyle::Primary, 0xE8A7);
    theme::MakeFlatButton(pvExtractBtn_, theme::ButtonStyle::Secondary, 0xE896);
    for (HWND b : {pvBtnA_, pvBtnB_, pvZoomOut_, pvZoomPct_, pvZoomIn_, pvBack_, pvFwd_})
        theme::MakeFlatButton(b, theme::ButtonStyle::Secondary);

    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                              0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
    theme::ThemeStatusBar(status_);
    theme::ApplyTitleBar(hwnd_);
    ApplyFonts();
}

// Phase 6 (startup): things not needed for the first picture are made right after the
// window is on screen (posted message, handled before any archive result can arrive).
// Saves ~150 ms of start time: shell icon list ~100 ms, RichEdit ~17 ms, grid ~35 ms.
void MainWindow::CreateLateChildren() {
    if (pvGrid_) return;
    // System image list (file icons come from Windows per extension, no icon files shipped)
    // + the RichEdit DLL: loaded on a short background thread, the UI keeps responding.
    // The image list is a process-wide handle; WM_APP_ICONS attaches it to the list.
    HWND h = hwnd_;
    std::thread([h] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHFILEINFOW sfi{};
        auto sysList = SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                                      SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        PostMessageW(h, WM_APP_ICONS, 0, static_cast<LPARAM>(sysList));
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
    // Word: RichEdit 4.1 (msftedit.dll ships with Windows). Read-only, no OLE callback
    // (embedded objects are never created), no auto URL detection.
    if (LoadLibraryExW(L"msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        pvRich_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                  WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                                  0, 0, 0, 0, preview_, nullptr, inst_, nullptr);
        if (pvRich_) {
            SendMessageW(pvRich_, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);
            SendMessageW(pvRich_, EM_AUTOURLDETECT, 0, 0);
            SendMessageW(pvRich_, EM_SETOLECALLBACK, 0, reinterpret_cast<LPARAM>(static_cast<IRichEditOleCallback*>(&g_richOle)));
            SendMessageW(pvRich_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(theme::kWhite));
            const int m = Dpi(12);
            SendMessageW(pvRich_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(m, m));
            SendMessageW(pvRich_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            SetWindowSubclass(pvRich_, CtrlWheelToParent, 1, 0);
        }
    }
    // Excel: our own sheet view (paints only the visible cells, Excel look).
    sheetView_.Create(preview_, inst_);
    pvGrid_ = sheetView_.hwnd();
    LayoutPreviewChildren();
    InvalidateRect(list_, nullptr, FALSE);
}

void MainWindow::ApplyFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, static_cast<UINT>(dpi_));
    if (font_) DeleteObject(font_);
    if (boldFont_) DeleteObject(boldFont_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    bold.lfHeight = MulDiv(bold.lfHeight, 5, 4);
    boldFont_ = CreateFontIndirectW(&bold);
    EnumChildWindows(hwnd_, [](HWND child, LPARAM f) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font_));

    // Text preview: fixed-width font (Arabic falls back through Windows font linking).
    if (monoFont_) DeleteObject(monoFont_);
    LOGFONTW mono = ncm.lfMessageFont;
    lstrcpynW(mono.lfFaceName, L"Consolas", LF_FACESIZE);
    monoFont_ = CreateFontIndirectW(&mono);
    if (pvText_) SendMessageW(pvText_, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont_), TRUE);
}

void MainWindow::Layout() {
    if (!list_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status_, &sr);
    const int statusH = sr.bottom - sr.top;
    {
        const int w = rc.right;
        int parts[3] = {w * 45 / 100, w * 75 / 100, -1};
        SendMessageW(status_, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    }

    const int pad = Dpi(6), btnH = Dpi(30), barH = btnH + 2 * pad;
    int x = pad;
    for (HWND b : toolButtons_) {
        const int w = theme::ButtonWidth(b, dpi_);
        MoveWindow(b, x, pad, w, btnH, TRUE);
        x += w + Dpi(2);
    }
    // search: a rounded frame (painted in WM_PAINT) with the borderless edit inside; shrinks on narrow windows
    barH_ = barH;
    const int searchW = std::clamp(static_cast<int>(rc.right) - x - pad - Dpi(10), Dpi(150), Dpi(280));
    searchFrame_ = {rc.right - pad - searchW, pad + Dpi(1), rc.right - pad, pad + btnH - Dpi(1)};
    {
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        const int eh = tm.tmHeight + Dpi(2);
        const int ey = (searchFrame_.top + searchFrame_.bottom - eh) / 2;
        MoveWindow(search_, searchFrame_.left + Dpi(30), ey, searchW - Dpi(38), eh, TRUE);
    }

    const int pathH = Dpi(24);
    MoveWindow(pathLabel_, pad, barH, rc.right - 2 * pad, pathH, TRUE);

    const int top = barH + pathH, bottom = rc.bottom - statusH;
    if (split_ <= 0) split_ = rc.right * 58 / 100;
    split_ = std::clamp(split_, Dpi(200), std::max(Dpi(200), static_cast<int>(rc.right) - Dpi(200)));
    MoveWindow(list_, 0, top, split_, bottom - top, TRUE);
    // While Quick Look is open the preview pane lives in that window instead.
    if (!qlOpen_) MoveWindow(preview_, split_ + Dpi(5), top, rc.right - split_ - Dpi(5), bottom - top, TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ============================================================ view model

void MainWindow::RebuildView() {
    view_.clear();
    if (!index_) {
        ListView_SetItemCountEx(list_, 0, 0);
        UpdatePreview();
        UpdateStatus();
        return;
    }
    wchar_t q[256]{};
    GetWindowTextW(search_, q, 256);
    searchMode_ = q[0] != 0;
    if (searchMode_) {
        // Search-as-you-type over the whole archive (names only, in-memory index).
        const int qlen = lstrlenW(q);
        const auto& nodes = index_->nodes();
        for (uint32_t i = 1; i < nodes.size() && view_.size() < 200000; ++i) {
            const auto& n = nodes[i];
            if (FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, n.name.c_str(),
                                static_cast<int>(n.name.size()), q, qlen, nullptr, nullptr, nullptr, 0) >= 0)
                view_.push_back(i);
        }
    } else {
        view_ = index_->node(folder_).children;
    }
    SortView();
    ListView_SetItemCountEx(list_, static_cast<int>(view_.size()), 0);
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (!view_.empty()) ListView_SetItemState(list_, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    InvalidateRect(list_, nullptr, TRUE);
    UpdateTitleAndPath();
    UpdatePreview();
    UpdateStatus();
}

void MainWindow::SortView() {
    if (!index_) return;
    const auto& nodes = index_->nodes();
    const auto& entries = index_->entries();
    auto mtime = [&](uint32_t n) -> uint64_t {
        const auto e = nodes[n].entry;
        if (e == ArchiveIndex::kNoEntry) return 0;
        return (uint64_t(entries[e].mtime.dwHighDateTime) << 32) | entries[e].mtime.dwLowDateTime;
    };
    auto crc = [&](uint32_t n) -> uint64_t {
        const auto e = nodes[n].entry;
        return (e == ArchiveIndex::kNoEntry || !entries[e].hasCrc) ? 0 : (1ull << 32) | entries[e].crc;
    };
    auto method = [&](uint32_t n) -> std::wstring {
        const auto e = nodes[n].entry;
        return e == ArchiveIndex::kNoEntry ? std::wstring() : index_->info().methods[entries[e].method];
    };
    auto cmpStr = [](const std::wstring& x, const std::wstring& y) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, x.c_str(), static_cast<int>(x.size()),
                               y.c_str(), static_cast<int>(y.size()), nullptr, nullptr, 0) - CSTR_EQUAL;
    };
    std::stable_sort(view_.begin(), view_.end(), [&](uint32_t a, uint32_t b) {
        const auto& A = nodes[a];
        const auto& B = nodes[b];
        if (A.isDir != B.isDir) return A.isDir;           // folders always first
        int c = 0;
        if (sortCol_ == ColSize) {
            c = A.size < B.size ? -1 : A.size > B.size ? 1 : 0;
        } else if (sortCol_ == ColDetail) {
            switch (detail_) {
            case Detail::Packed: c = A.packSize < B.packSize ? -1 : A.packSize > B.packSize ? 1 : 0; break;
            case Detail::Ratio: {
                const double ra = A.size ? double(A.packSize) / A.size : 0, rb = B.size ? double(B.packSize) / B.size : 0;
                c = ra < rb ? -1 : ra > rb ? 1 : 0;
                break;
            }
            case Detail::Modified: { const auto ma = mtime(a), mb = mtime(b); c = ma < mb ? -1 : ma > mb ? 1 : 0; break; }
            case Detail::Crc: { const auto ca = crc(a), cb = crc(b); c = ca < cb ? -1 : ca > cb ? 1 : 0; break; }
            case Detail::Type: if (!A.isDir) c = cmpStr(TypeNameOf(A.name), TypeNameOf(B.name)); break;
            case Detail::Method: c = cmpStr(method(a), method(b)); break;
            default: break;
            }
        }
        if (c == 0)
            c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                A.name.c_str(), static_cast<int>(A.name.size()), B.name.c_str(),
                                static_cast<int>(B.name.size()), nullptr, nullptr, 0) - CSTR_EQUAL;
        return sortAsc_ ? c < 0 : c > 0;
    });
}

void MainWindow::NavigateTo(uint32_t folder) {
    if (!index_) return;
    folder_ = folder;
    if (searchMode_) SetWindowTextW(search_, L"");   // triggers EN_CHANGE -> timer -> RebuildView
    RebuildView();
}

void MainWindow::GoUp() {
    if (!index_) return;
    if (searchMode_) { SetWindowTextW(search_, L""); RebuildView(); return; }
    if (folder_ == ArchiveIndex::kRoot) return;
    const uint32_t from = folder_;
    NavigateTo(index_->node(folder_).parent);
    // Keep the folder we came from selected.
    for (size_t i = 0; i < view_.size(); ++i)
        if (view_[i] == from) {
            ListView_SetItemState(list_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, static_cast<int>(i), FALSE);
            break;
        }
}

std::vector<uint32_t> MainWindow::SelectedNodes() const {
    std::vector<uint32_t> out;
    int i = -1;
    while ((i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) >= 0)
        if (static_cast<size_t>(i) < view_.size()) out.push_back(view_[static_cast<size_t>(i)]);
    return out;
}

void MainWindow::ActivateSelection() {
    const int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (i < 0 || static_cast<size_t>(i) >= view_.size()) return;
    const uint32_t n = view_[static_cast<size_t>(i)];
    if (index_->node(n).isDir) NavigateTo(n);
    else CmdExtractAndOpen();
}

int MainWindow::IconFor(uint32_t node) {
    const auto& n = index_->node(node);
    const std::wstring key = n.isDir ? L"<dir>" : Ext(n.name);
    if (auto it = iconCache_.find(key); it != iconCache_.end()) return it->second;
    SHFILEINFOW sfi{};
    const std::wstring probe = n.isDir ? L"folder" : (L"x" + key);
    SHGetFileInfoW(probe.c_str(), n.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    iconCache_[key] = sfi.iIcon;
    return sfi.iIcon;
}

void MainWindow::OnGetDispInfo(NMLVDISPINFOW* di) {
    auto& it = di->item;
    if (!index_ || it.iItem < 0 || static_cast<size_t>(it.iItem) >= view_.size()) return;
    const uint32_t nid = view_[static_cast<size_t>(it.iItem)];
    const auto& n = index_->node(nid);
    const archive::EntryRecord* e = n.entry != ArchiveIndex::kNoEntry ? &index_->entries()[n.entry] : nullptr;
    if (it.mask & LVIF_IMAGE) it.iImage = IconFor(nid);
    if (!(it.mask & LVIF_TEXT)) return;
    std::wstring s;
    switch (it.iSubItem) {
    case ColName:
        s = searchMode_ ? index_->PathOf(nid) : n.name;
        if (e && e->encrypted) s += L" *";          // like WinRAR: * marks encrypted files
        break;
    case ColSize: s = n.isDir ? (n.fileCount ? fmt::Size(n.size) : L"") : fmt::Size(n.size); break;
    case ColDetail:
        switch (detail_) {
        case Detail::Packed: s = (n.isDir && !n.fileCount) ? L"" : fmt::Size(n.packSize); break;
        case Detail::Ratio: s = fmt::Ratio(n.packSize, n.size); break;
        case Detail::Modified: s = e ? fmt::DateTime(e->mtime) : L""; break;
        case Detail::Crc: s = (e && e->hasCrc && !n.isDir) ? fmt::Hex32(e->crc) : L""; break;
        case Detail::Type: s = n.isDir ? L"File folder" : TypeNameOf(n.name); break;
        case Detail::Method: s = e ? index_->info().methods[e->method] : L""; break;
        default: break;
        }
        break;
    }
    lstrcpynW(it.pszText, s.c_str(), it.cchTextMax);
}

// Windows' own type name for an extension ("PNG File", "Microsoft Word Document").
std::wstring MainWindow::TypeNameOf(const std::wstring& name) {
    const std::wstring ext = Ext(name);
    if (auto it = typeNameCache_.find(ext); it != typeNameCache_.end()) return it->second;
    SHFILEINFOW sfi{};
    SHGetFileInfoW((L"x" + ext).c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME);
    return typeNameCache_[ext] = sfi.szTypeName;
}

void MainWindow::ShowDetailMenu() {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < static_cast<int>(Detail::Count); ++i)
        AppendMenuW(m, MF_STRING | (i == static_cast<int>(detail_) ? MF_CHECKED : 0), ID_DETAIL_BASE + i, kDetails[i].label);
    RECT r{};
    HWND hdr = ListView_GetHeader(list_);
    if (!Header_GetItemDropDownRect(hdr, ColDetail, &r)) Header_GetItemRect(hdr, ColDetail, &r);
    MapWindowPoints(hdr, nullptr, reinterpret_cast<POINT*>(&r), 2);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_DETAIL_BASE && cmd < ID_DETAIL_BASE + static_cast<UINT>(Detail::Count))
        SetDetail(static_cast<Detail>(cmd - ID_DETAIL_BASE));
}

void MainWindow::SetDetail(Detail d) {
    detail_ = d;
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_FMT;
    c.pszText = const_cast<wchar_t*>(kDetails[static_cast<int>(d)].label);
    c.fmt = kDetails[static_cast<int>(d)].fmt | LVCFMT_SPLITBUTTON;
    ListView_SetColumn(list_, ColDetail, &c);
    settings::SetNumber(L"DetailColumn", static_cast<uint32_t>(d));
    if (sortCol_ == ColDetail) SortView();
    InvalidateRect(list_, nullptr, FALSE);
}

void MainWindow::UpdateTitleAndPath() {
    if (!index_) {
        SetWindowTextW(hwnd_, L"Mechano Reader");
        SetWindowTextW(pathLabel_, L"  Open an archive (Ctrl+O), or drag one onto this window.  To create a new archive, drag files here or press New (Ctrl+N).");
        return;
    }
    const std::wstring name = FileNameOf(index_->info().path);
    SetWindowTextW(hwnd_, (name + L" - Mechano Reader").c_str());
    std::wstring crumb = L"  \U0001F4E6 " + name;
    if (searchMode_) crumb += L"   ›   search results (" + fmt::Number(view_.size()) + L")";
    else {
        const std::wstring p = index_->PathOf(folder_);
        size_t s = 0;
        while (s < p.size()) {
            size_t t = p.find(L'\\', s);
            if (t == std::wstring::npos) t = p.size();
            crumb += L"   ›   " + p.substr(s, t - s);
            s = t + 1;
        }
    }
    SetWindowTextW(pathLabel_, crumb.c_str());
}

void MainWindow::UpdateStatus() {
    std::wstring left, mid;
    if (index_) {
        const auto sel = SelectedNodes();
        if (!sel.empty()) {
            uint64_t sz = 0;
            for (auto n : sel) sz += index_->node(n).size;
            left = L"Selected " + fmt::Number(sel.size()) + L" item(s), " + fmt::Size(sz);
        } else {
            left = fmt::Number(view_.size()) + L" item(s)";
        }
        const auto& info = index_->info();
        mid = info.formatName + L"  ·  " + fmt::Number(index_->totalFiles()) + L" files  ·  " +
              fmt::Size(index_->totalSize()) + L" → " + fmt::Size(info.physicalSize ? info.physicalSize : index_->totalPacked());
        if (info.hasEncrypted) mid += L"  ·  \U0001F512";
    }
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(left.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(mid.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(statusMessage_.c_str()));
}

// ============================================================ preview / info pane

// Like the Windows Explorer preview pane: select ONE file and the pane shows
// the file itself (image / text), with a short info strip underneath. Other
// file types (and folders, several items, nothing selected) show the info card.
//
// Loading: the selected entry is decoded into RAM on the worker thread (never
// written to disk). Only the newest selection counts: older requests are
// cancelled and their results dropped (generation counter).

void MainWindow::UpdatePreview() {
    uint32_t file = 0;
    if (index_) {
        const auto sel = SelectedNodes();
        if (sel.size() == 1 && !index_->node(sel[0]).isDir && index_->node(sel[0]).entry != ArchiveIndex::kNoEntry)
            file = sel[0];
    }
    ShowWindow(pvOpenBtn_, file ? SW_SHOW : SW_HIDE);
    ShowWindow(pvExtractBtn_, file ? SW_SHOW : SW_HIDE);
    if (file != pvNode_) RequestPreview(file);
    InvalidateRect(preview_, nullptr, TRUE);
    UpdateQuickLookTitle();
}

// ============================================================ F1 Quick Look

void MainWindow::ToggleQuickLook() {
    if (qlOpen_) CloseQuickLook();
    else OpenQuickLook();
}

void MainWindow::OpenQuickLook() {
    if (qlOpen_ || !index_ || SelectedNodes().empty()) return;
    if (!ql_) {
        // Owned by the main window: stays above it, minimises with it, no taskbar button.
        // First size: 75% of the monitor work area, centred on the main window.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& wa = mi.rcWork;
        const int w = (wa.right - wa.left) * 3 / 4, h = (wa.bottom - wa.top) * 3 / 4;
        RECT mr;
        GetWindowRect(hwnd_, &mr);
        int x = (mr.left + mr.right) / 2 - w / 2, y = (mr.top + mr.bottom) / 2 - h / 2;
        x = std::clamp<int>(x, wa.left, std::max<int>(wa.left, wa.right - w));
        y = std::clamp<int>(y, wa.top, std::max<int>(wa.top, wa.bottom - h));
        ql_ = CreateWindowExW(0, kQuickLookClass, L"Quick Look", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              x, y, w, h, hwnd_, nullptr, inst_, this);
        if (!ql_) return;
        theme::ApplyTitleBar(ql_);
    }
    qlOpen_ = true;
    UpdatePreview();                          // make sure the selected file is the one shown
    SetParent(preview_, ql_);
    RECT c;
    GetClientRect(ql_, &c);
    MoveWindow(preview_, 0, 0, c.right, c.bottom, TRUE);
    UpdateQuickLookTitle();
    ShowWindow(ql_, SW_SHOW);                 // keeps its last size / maximised state
    SetForegroundWindow(ql_);
    SetFocus(ql_);
    InvalidateRect(hwnd_, nullptr, TRUE);     // the empty place left in the main window
}

void MainWindow::CloseQuickLook() {
    if (!qlOpen_) return;
    qlOpen_ = false;
    const bool active = GetForegroundWindow() == ql_;
    SetParent(preview_, hwnd_);
    ShowWindow(ql_, SW_HIDE);                 // hidden, not destroyed: size and position are kept
    Layout();
    if (active) { SetForegroundWindow(hwnd_); SetFocus(list_); }
}

void MainWindow::UpdateQuickLookTitle() {
    if (!qlOpen_ || !ql_ || !index_) return;
    std::wstring t = L"Quick Look";
    const auto sel = SelectedNodes();
    if (sel.size() == 1) t = index_->node(sel[0]).name + L"  -  Quick Look";
    else if (sel.size() > 1) t = fmt::Number(sel.size()) + L" items  -  Quick Look";
    SetWindowTextW(ql_, t.c_str());
}

bool MainWindow::PreTranslate(MSG* m) {
    // Only keys typed while the Quick Look window itself has the focus. Inside the
    // text / Word / Excel views the arrows keep scrolling the document.
    if (!qlOpen_ || m->message != WM_KEYDOWN || m->hwnd != ql_) return false;
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;   // Ctrl +/-/0 zoom etc.
    switch (m->wParam) {
    case VK_SPACE:
        CloseQuickLook();
        return true;
    case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        // Previous / next file: the list moves its selection, the preview follows.
        SendMessageW(list_, WM_KEYDOWN, m->wParam, m->lParam);
        return true;
    case VK_LEFT: case VK_RIGHT:
        // PDF / PowerPoint: previous / next page.
        if (pvState_ == PvState::Image && docPages_ > 1) {
            OnCommand(m->wParam == VK_LEFT ? ID_PV_A : ID_PV_B);
            return true;
        }
        if (pvState_ == PvState::Media && media_->IsOpen()) {
            media_->Seek(media_->Position() + (m->wParam == VK_RIGHT ? 5.0 : -5.0));
            UpdateSeekBar();
            return true;
        }
        return false;
    case VK_RETURN:
        CloseQuickLook();
        ActivateSelection();
        return true;
    }
    return false;
}

LRESULT CALLBACK MainWindow::QuickLookProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_SIZE:
        if (self && self->qlOpen_) MoveWindow(self->preview_, 0, 0, LOWORD(l), HIWORD(l), TRUE);
        return 0;
    case WM_ERASEBKGND: return 1;             // the preview pane covers the whole client area
    case WM_GETMINMAXINFO:
        if (self) reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {self->Dpi(360), self->Dpi(260)};
        return 0;
    case WM_CLOSE:
        if (self) self->CloseQuickLook();     // X button / Alt+F4: hide, the pane goes back
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void MainWindow::ResetPreview() {
    if (previewCancel_) previewCancel_->Cancel();
    previewCancel_.reset();
    ++*previewGen_;
    pvNode_ = 0;
    pvState_ = PvState::Empty;
    SetPreviewImage({});
    pvMessage_.clear();
    pvDwgVersion_.clear();
    cad_.reset();
    cadBusy_ = false;
    cadNote_.clear();
    pvTruncated_ = false;
    // zoom back to "fit" for the next file
    zoom_ = 1.0;
    viewCx_ = viewCy_ = 0.5;
    if (panning_) { panning_ = false; ReleaseCapture(); }
    if (preview_) KillTimer(preview_, kTimerDocZoom);
    if (pvRich_) SendMessageW(pvRich_, EM_SETZOOM, 0, 0);
    if (zoomFont_) { DeleteObject(zoomFont_); zoomFont_ = nullptr; }
    if (pvText_) { ShowWindow(pvText_, SW_HIDE); SetWindowTextW(pvText_, L""); }
    if (pvRich_) { ShowWindow(pvRich_, SW_HIDE); SetWindowTextW(pvRich_, L""); }
    if (pvGrid_) { ShowWindow(pvGrid_, SW_HIDE); sheetView_.SetSheet(nullptr); sheetView_.SetZoom(1.0); }
    sheet_.reset();
    pvKind_ = preview::Kind::None;
    docBytes_.reset();
    docPage_ = docPages_ = 0;
    if (media_) media_->Close();               // releases the temp file
    audio_.reset();
    if (coverBmp_) { DeleteObject(coverBmp_); coverBmp_ = nullptr; }
    SetRectEmpty(&audioTimeRc_);
    if (pvVideo_) ShowWindow(pvVideo_, SW_HIDE);
    DeleteTempDir(mediaDir_);
    mediaDir_.clear();
    if (pvBtnA_) { ShowWindow(pvBtnA_, SW_HIDE); ShowWindow(pvBtnB_, SW_HIDE); }
    if (preview_) UpdatePreviewButtons();       // hides zoom + seek controls
}

void MainWindow::SetPreviewImage(preview::Image&& img) {
    pvImage_ = std::move(img);
    if (wicSrc_) { wicSrc_->Release(); wicSrc_ = nullptr; }
    if (pvScaled_) { DeleteObject(pvScaled_); pvScaled_ = nullptr; }
    pvScaledW_ = pvScaledH_ = 0;
    pvScaledS_ = 0;
}

bool MainWindow::PagedImage() const {
    if (pvState_ != PvState::Image) return false;
    return pvKind_ == preview::Kind::Pdf || pvKind_ == preview::Kind::Pptx ||
           ((pvKind_ == preview::Kind::Dwg || pvKind_ == preview::Kind::Dxf) && cad_);
}

void MainWindow::RequestDocPage(uint32_t page, uint32_t renderWidth) {
    if ((!docBytes_ && !cad_) || page >= docPages_ || (page == docPage_ && !renderWidth)) return;
    auto cadDoc = cad_;
    if (previewCancel_) previewCancel_->Cancel();
    previewCancel_ = std::make_shared<archive::CancelToken>();
    const uint64_t gen = ++*previewGen_;
    auto genPtr = previewGen_;
    auto bytes = docBytes_;
    const uint32_t node = pvNode_;
    const preview::Kind kind = pvKind_;
    const RECT c = PreviewContentRect();
    uint32_t width = static_cast<uint32_t>(std::max<LONG>(200, c.right - c.left));
    if (renderWidth) width = renderWidth;
    else if ((kind == preview::Kind::Pdf || kind == preview::Kind::Pptx || cadDoc) && zoom_ > 1.0)
        width = std::max(width, pvImage_.width);               // next page keeps the zoomed sharpness
    width = std::min<uint32_t>(width, preview::kDecodeSide);
    docPage_ = page;
    UpdatePreviewButtons();
    HWND h = hwnd_;
    worker_.Post([genPtr, gen, bytes, cadDoc, node, kind, page, width, h] {
        if (genPtr->load() != gen) return;
        auto r = std::make_unique<PreviewResult>();
        r->gen = gen;
        r->node = node;
        r->kind = kind;
        r->docBytes = bytes;
        if (cadDoc) {                                          // CAD: Model / Layout view from the parsed drawing
            r->cad = cadDoc;
            r->docPage = page;
            r->docPages = static_cast<uint32_t>(cadDoc->views.size());
            auto im = cad::RenderView(*cadDoc, page, width);
            if (im) r->image = std::move(im.value()); else r->error = im.error();
        } else {
            RenderDoc(kind, bytes, page, width, r->image, r->docPage, r->docPages, r->rtf, r->sheet, r->error);
        }
        if (genPtr->load() != gen) return;
        if (PostMessageW(h, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
    });
}

void MainWindow::RequestPreview(uint32_t node) {
    ResetPreview();
    pvNode_ = node;
    if (!node || !index_ || !archive_) return;
    const auto& n = index_->node(node);
    const auto& e = index_->entries()[n.entry];
    const preview::Kind kind = preview::KindForName(n.name);
    if (kind == preview::Kind::None) return;                         // info card
    if (e.isLink) { pvState_ = PvState::Failed; pvMessage_ = L"Links are not previewed."; return; }
    const uint64_t limit = kind == preview::Kind::Image ? preview::kMaxImageBytes
                         : kind == preview::Kind::Pdf   ? preview::kMaxPdfBytes
                         : kind == preview::Kind::Dxf   ? cad::Limits{}.maxFileBytes
                         : kind == preview::Kind::Revit ? preview::kMaxRevitBytes
                         : kind == preview::Kind::Ifc   ? preview::kMaxIfcBytes
                         : kind == preview::Kind::Navisworks ? preview::kMaxNavisBytes
                         : kind == preview::Kind::Text || kind == preview::Kind::Dwg ? UINT64_MAX   // first part only
                         : IsDoc(kind)                  ? preview::kMaxOfficeBytes
                                                        : preview::kMaxMediaBytes;
    if (e.size > limit) {
        pvState_ = PvState::Failed;
        pvMessage_ = L"This file is too large to preview (" + fmt::Size(e.size) + L"). Use \"Open in main program\".";
        return;
    }

    pvState_ = PvState::Loading;
    pvKind_ = kind;
    const uint64_t gen = *previewGen_;
    auto genPtr = previewGen_;
    previewCancel_ = std::make_shared<archive::CancelToken>();
    auto cancel = previewCancel_;
    auto arc = archive_;
    const uint32_t arcIndex = e.arcIndex;
    const RECT c = PreviewContentRect();
    const uint32_t width = static_cast<uint32_t>(std::max<LONG>(200, c.right - c.left));
    // Media: a private temp folder, same safe extraction path as "Extract & Open".
    std::wstring mediaDir, strip;
    if (kind == preview::Kind::Video || kind == preview::Kind::Audio) {
        mediaDir = fs::NewTempSubfolder(L"pv-");
        if (mediaDir.empty()) {
            pvState_ = PvState::Failed;
            pvMessage_ = L"The temporary folder for the preview could not be created.";
            return;
        }
        const std::wstring path = index_->PathOf(node);
        const auto slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos) strip = path.substr(0, slash + 1);
    }
    // F7: DWG -> the real drawing too (after the thumbnail) when the helper is installed.
    bool convertDwg = false;
    if (kind == preview::Kind::Dwg || kind == preview::Kind::Dxf || kind == preview::Kind::Revit || kind == preview::Kind::Ifc || kind == preview::Kind::Navisworks) {
        const std::wstring path = index_->PathOf(node);
        const auto slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos) strip = path.substr(0, slash + 1);
        convertDwg = kind == preview::Kind::Dwg && !cad::FindDwgConverter().empty() && e.size <= cad::ConvertLimits{}.maxDwgBytes;
        if (kind == preview::Kind::Dwg && !convertDwg && !cad::FindDwgConverter().empty())
            cadNote_ = L"Drawing too large for the full view (" + fmt::Size(e.size) + L")";
    }
    const std::wstring entryName = n.name;
    HWND h = hwnd_;
    worker_.Post([genPtr, gen, cancel, arc, arcIndex, node, kind, width, mediaDir, strip, convertDwg, entryName, h] {
        if (genPtr->load() != gen || cancel->IsCancelled()) return;   // the user already moved on
        auto r = std::make_unique<PreviewResult>();
        r->gen = gen;
        r->node = node;
        r->kind = kind;
        auto stale = [genPtr, gen] { return genPtr->load() != gen; };
        // The real drawing: a second result after the DWG thumbnail, or the only one for DXF.
        auto loadCad = [&](bool dwg) -> std::unique_ptr<PreviewResult> {
            auto c = std::make_unique<PreviewResult>();
            c->gen = gen; c->node = node; c->kind = kind;
            ErrorInfo err;
            auto drawing = LoadCad(*arc, arcIndex, strip, dwg, *cancel, stale, err);
            if (!drawing) {
                if (err.code == ErrorCode::Cancelled) return nullptr;
                if (dwg) c->cadNote = err.friendly; else c->error = err;
                log::Info(L"CAD preview: " + err.friendly + L" | " + err.details);
                return c;
            }
            auto im = cad::RenderView(*drawing, 0, width);
            if (!im) { if (dwg) c->cadNote = im.error().friendly; else c->error = im.error(); return c; }
            c->image = std::move(im.value());
            c->docPages = static_cast<uint32_t>(drawing->views.size());
            c->cad = std::move(drawing);
            return c;
        };
        if (kind == preview::Kind::Dxf) {
            r = loadCad(false);
            if (!r || stale()) return;
            if (PostMessageW(h, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
            return;
        }
        if (kind == preview::Kind::Revit || kind == preview::Kind::Ifc || kind == preview::Kind::Navisworks) {
            // Revit / IFC / Navisworks: a private temp copy, read by our bounded readers, shown as an info card
            // (a light look at what is inside; double-click opens the real program for the rest).
            std::wstring dir;
            ErrorInfo err;
            const wchar_t* prefix = kind == preview::Kind::Ifc ? L"ifc-" : kind == preview::Kind::Navisworks ? L"nw-" : L"rvt-";
            const std::wstring src = ExtractPreviewCopy(*arc, arcIndex, strip, prefix, *cancel, stale, err, dir);
            if (!src.empty() && kind == preview::Kind::Ifc) {
                auto info = preview::ReadIfcFile(src);
                if (info) r->rtf = preview::IfcToRtf(info.value(), entryName);
                else r->error = info.error();
            } else if (!src.empty() && kind == preview::Kind::Navisworks) {
                auto info = preview::ReadNavisworksFile(src, entryName);
                if (info) r->rtf = preview::NavisworksToRtf(info.value(), entryName);
                else r->error = info.error();
            } else if (!src.empty()) {
                auto info = preview::ReadRevitFile(src, entryName);
                if (info) r->rtf = preview::RevitToRtf(info.value(), entryName);
                else r->error = info.error();
            } else if (err.code == ErrorCode::Cancelled) {
                DeleteTempDir(dir);
                return;
            } else {
                r->error = err;
            }
            DeleteTempDir(dir);
            if (stale()) return;
            if (PostMessageW(h, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
            return;
        }
        if (kind == preview::Kind::Video || kind == preview::Kind::Audio) {
            archive::ExtractOptions eo;
            eo.destination = mediaDir;
            eo.stripPrefix = strip;
            eo.propagateMotW = false;               // private copy, never opened by the shell
            r->mediaDir = mediaDir;
            auto rep = arc->Extract({arcIndex}, eo, nullptr, *cancel);
            WIN32_FIND_DATAW fd{};
            HANDLE f = FindFirstFileW((mediaDir + L"\\*").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { r->mediaFile = mediaDir + L"\\" + fd.cFileName; break; } }
                while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            if (kind == preview::Kind::Audio && !rep.cancelled && rep.filesDone == 1 && !r->mediaFile.empty() && !stale())
                r->audio = std::make_shared<preview::AudioTags>(preview::ReadAudioTags(r->mediaFile));   // title, artist, cover
            if (rep.cancelled) r->error = ErrorInfo{ErrorCode::Cancelled, L"Cancelled.", L""};
            else if (rep.filesDone != 1 || r->mediaFile.empty())
                r->error = ErrorInfo{ErrorCode::Corrupted,
                                     rep.wrongPassword ? L"This file is password protected. Extract it to play it."
                                                       : L"This file could not be prepared for playing.",
                                     rep.issues.empty() ? L"" : rep.issues[0].friendly};
        } else {
            const bool text = kind == preview::Kind::Text, dwg = kind == preview::Kind::Dwg;
            const uint64_t max = text ? preview::kMaxTextBytes
                               : dwg ? preview::kMaxDwgReadBytes
                               : kind == preview::Kind::Pdf ? preview::kMaxPdfBytes
                               : IsDoc(kind) ? preview::kMaxOfficeBytes : preview::kMaxImageBytes;
            bool dwgCut = false;                    // DWG: only the start of the file is needed
            auto data = arc->ReadEntry(arcIndex, max, *cancel, text ? &r->truncated : dwg ? &dwgCut : nullptr);
            if (!data) {
                r->error = data.error();
            } else if (dwg) {
                auto d = preview::ReadDwg(data.value());
                if (!d) {
                    r->error = d.error();
                } else {
                    const auto& code = d.value().code;
                    r->dwgVersion = d.value().version.empty()
                                        ? L"AutoCAD drawing (" + std::wstring(code.begin(), code.end()) + L")"
                                        : d.value().version + L" drawing";
                    if (d.value().hasPicture) r->image = std::move(d.value().picture);
                    else if (!convertDwg) r->error = ErrorInfo{ErrorCode::Corrupted,
                                              r->dwgVersion + L".\nNo preview picture is saved in this file. "
                                              L"Use \"Open in main program\" to view it in AutoCAD.",
                                              L"DWG without a BMP/PNG thumbnail"};
                    r->cadPending = convertDwg;
                }
            } else if (text) {
                auto t = preview::DecodeText(data.value());
                if (t) r->text = std::move(t.value()); else r->error = t.error();
            } else if (IsDoc(kind)) {
                r->docBytes = std::make_shared<const std::vector<uint8_t>>(std::move(data.value()));
                RenderDoc(kind, r->docBytes, 0, width, r->image, r->docPage, r->docPages, r->rtf, r->sheet, r->error);
            } else {
                auto im = preview::DecodeImage(data.value());
                if (im) r->image = std::move(im.value()); else r->error = im.error();
            }
        }
        if (genPtr->load() != gen) { DeleteTempDir(r->mediaDir); return; }
        const bool second = r->cadPending;
        if (PostMessageW(h, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
        else { DeleteTempDir(r->mediaDir); return; }
        // DWG: the thumbnail is on screen; now the real drawing (can take seconds for big files).
        if (second) {
            auto c = loadCad(true);
            if (!c || stale()) return;
            if (PostMessageW(h, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(c.get()))) c.release();
        }
    });
}

void MainWindow::OnPreviewResult(PreviewResult* raw) {
    std::unique_ptr<PreviewResult> r(raw);
    if (r->gen != *previewGen_ || r->node != pvNode_) {             // stale
        DeleteTempDir(r->mediaDir);
        return;
    }
    if (!r->mediaDir.empty()) mediaDir_ = r->mediaDir;              // deleted by ResetPreview
    if (r->error || (r->kind != preview::Kind::Audio && r->kind != preview::Kind::Video)) carrySound_ = false;
    if (!r->dwgVersion.empty()) pvDwgVersion_ = r->dwgVersion;
    // F7: CAD results (the real drawing, a view change, or why it cannot be shown).
    if (r->kind == preview::Kind::Dwg || r->kind == preview::Kind::Dxf) {
        if (r->cad && !r->error) {
            cad_ = r->cad;
            cadBusy_ = false;
            cadNote_.clear();
            docPage_ = r->docPage;
            docPages_ = r->docPages;
            pvState_ = PvState::Image;
            SetPreviewImage(std::move(r->image));
            UpdatePreviewButtons();
            InvalidateRect(preview_, nullptr, TRUE);
            return;
        }
        if (!r->cadNote.empty()) {                                  // DWG: keep the thumbnail, say why
            cadBusy_ = false;
            cadNote_ = r->cadNote;
            if (pvState_ == PvState::Loading) {
                pvState_ = PvState::Failed;
                pvMessage_ = pvDwgVersion_ + L".\nThe drawing could not be shown: " + r->cadNote;
            }
            InvalidateRect(preview_, nullptr, TRUE);
            return;
        }
        if (r->cadPending) {
            cadBusy_ = true;
            if (r->image.pixels.empty() && !r->error) {                 // no thumbnail: wait for the drawing
                pvState_ = PvState::Loading;
                InvalidateRect(preview_, nullptr, TRUE);
                return;
            }
        }
    }
    if (!r->error && (r->kind == preview::Kind::Video || r->kind == preview::Kind::Audio)) {
        const bool video = r->kind == preview::Kind::Video;
        pvState_ = PvState::Media;
        mediaBar_.SetStyle(video ? MediaBar::Style::Vlc : MediaBar::Style::Aimp);
        audio_ = r->audio;
        if (audio_ && !audio_->cover.pixels.empty()) {
            // the cover as a 32 bpp DIB, drawn scaled in the song panel
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = static_cast<LONG>(audio_->cover.width);
            bi.bmiHeader.biHeight = -static_cast<LONG>(audio_->cover.height);
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            coverBmp_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (coverBmp_ && bits) {
                // premultiplied BGRA laid over the panel colour (transparent corners must not turn black)
                const COLORREF bg = MediaBar::AimpBack();
                const uint8_t bb = GetBValue(bg), bgG = GetGValue(bg), br = GetRValue(bg);
                const uint8_t* s = audio_->cover.pixels.data();
                uint8_t* o = static_cast<uint8_t*>(bits);
                for (size_t i = 0; i + 3 < audio_->cover.pixels.size(); i += 4) {
                    const int inv = 255 - s[i + 3];
                    o[i] = static_cast<uint8_t>(s[i] + bb * inv / 255);
                    o[i + 1] = static_cast<uint8_t>(s[i + 1] + bgG * inv / 255);
                    o[i + 2] = static_cast<uint8_t>(s[i + 2] + br * inv / 255);
                    o[i + 3] = 255;
                }
            }
        }
        LayoutPreviewChildren();
        if (video) ShowWindow(pvVideo_, SW_SHOW);
        media_->StartWithSound(carrySound_ ? carryVolume_ : -1.0f);   // ⏮ / ⏭: the next song keeps the sound on
        carrySound_ = false;
        if (!media_->Open(r->mediaFile, video ? pvVideo_ : nullptr)) {
            pvState_ = PvState::Failed;
            pvMessage_ = media_->Error();
            ShowWindow(pvVideo_, SW_HIDE);
        }
        UpdatePreviewButtons();
        InvalidateRect(preview_, nullptr, TRUE);
        return;
    }
    if (!r->error && (IsDoc(r->kind) || r->kind == preview::Kind::Revit || r->kind == preview::Kind::Ifc || r->kind == preview::Kind::Navisworks)) {
        docBytes_ = r->docBytes;
        docPage_ = r->docPage;
        docPages_ = r->docPages;
        if (r->kind == preview::Kind::Docx || r->kind == preview::Kind::Revit || r->kind == preview::Kind::Ifc || r->kind == preview::Kind::Navisworks) {   // RTF we generate (Word / Revit / IFC card)
            pvState_ = PvState::Rich;
            // Our own RTF (pictures only, no embedded objects, no links followed): stream it into the read-only RichEdit.
            std::pair<const std::string*, size_t> src{&r->rtf, 0};
            EDITSTREAM es{reinterpret_cast<DWORD_PTR>(&src), 0, RtfStreamIn};
            // A read-only RichEdit drops pictures while loading: read-only is switched on after.
            SendMessageW(pvRich_, EM_SETREADONLY, FALSE, 0);
            SendMessageW(pvRich_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
            SendMessageW(pvRich_, EM_SETREADONLY, TRUE, 0);
            SendMessageW(pvRich_, EM_SETSEL, 0, 0);
            SendMessageW(pvRich_, WM_VSCROLL, SB_TOP, 0);
            LayoutPreviewChildren();
            ShowWindow(pvRich_, SW_SHOW);
        } else if (r->kind == preview::Kind::Xlsx) {
            pvState_ = PvState::Grid;
            sheet_ = r->sheet;
            sheetView_.SetSheet(sheet_);
            sheetView_.SetZoom(zoom_);
            LayoutPreviewChildren();
            ShowWindow(pvGrid_, (sheet_ && sheet_->rows) ? SW_SHOW : SW_HIDE);
        } else {
            pvState_ = PvState::Image;            // PDF page / PowerPoint slide
            SetPreviewImage(std::move(r->image));
        }
        ApplyChildZoom();
        UpdatePreviewButtons();
        InvalidateRect(preview_, nullptr, TRUE);
        return;
    }
    if (r->error) {
        pvState_ = PvState::Failed;
        pvMessage_ = r->error->friendly;
        if (r->error->code != ErrorCode::Cancelled) log::Info(L"Preview: " + r->error->friendly + L" | " + r->error->details);
    } else if (r->kind == preview::Kind::Text) {
        pvState_ = PvState::Text;
        pvTruncated_ = r->truncated;
        if (pvTruncated_) r->text += L"\r\n\r\n... (preview shows the first " + fmt::Size(preview::kMaxTextBytes) + L")";
        // Arabic / Hebrew text: Consolas has no Arabic letters (Windows would
        // substitute a tiny font), so use the normal UI font, and right-to-left
        // reading order when the text starts in Arabic.
        bool rtlScript = false, rtlFirst = false, sawLetter = false;
        for (size_t k = 0; k < r->text.size() && k < 65536; ++k) {
            const wchar_t ch = r->text[k];
            const bool rtl = (ch >= 0x0590 && ch <= 0x08FF) || (ch >= 0xFB1D && ch <= 0xFEFC);
            if (rtl) rtlScript = true;
            if (!sawLetter && (rtl || IsCharAlphaW(ch))) { sawLetter = true; rtlFirst = rtl; }
            if (rtlScript && sawLetter) break;
        }
        pvTextUiFont_ = rtlScript;
        SendMessageW(pvText_, WM_SETFONT, reinterpret_cast<WPARAM>(rtlScript ? font_ : monoFont_), FALSE);
        ApplyChildZoom();
        LONG_PTR ex = GetWindowLongPtrW(pvText_, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_RTLREADING | WS_EX_RIGHT | WS_EX_LEFTSCROLLBAR);
        if (rtlFirst) ex |= WS_EX_RTLREADING | WS_EX_RIGHT | WS_EX_LEFTSCROLLBAR;
        SetWindowLongPtrW(pvText_, GWL_EXSTYLE, ex);
        SetWindowTextW(pvText_, r->text.c_str());
        const RECT c = PreviewContentRect();
        MoveWindow(pvText_, c.left, c.top, c.right - c.left, c.bottom - c.top, FALSE);
        ShowWindow(pvText_, SW_SHOW);
    } else {
        pvState_ = PvState::Image;
        SetPreviewImage(std::move(r->image));
    }
    UpdatePreviewButtons();
    InvalidateRect(preview_, nullptr, TRUE);
}

namespace {
constexpr int kFooterH = 104;     // info strip under the preview (96-dpi px)
}

void MainWindow::LayoutPreviewChildren() {
    const RECT c = PreviewContentRect();
    if (pvState_ == PvState::Text)
        MoveWindow(pvText_, c.left, c.top, c.right - c.left, c.bottom - c.top, TRUE);
    if (pvState_ == PvState::Rich)
        MoveWindow(pvRich_, c.left, c.top, c.right - c.left, c.bottom - c.top, TRUE);
    if (pvState_ == PvState::Grid)
        MoveWindow(pvGrid_, c.left, c.top, c.right - c.left, c.bottom - c.top, TRUE);
    if (pvState_ == PvState::Media && pvKind_ == preview::Kind::Video) {
        MoveWindow(pvVideo_, c.left, c.top, c.right - c.left, c.bottom - c.top, TRUE);
        media_->RepaintVideo();
    }
    if (IsVideo()) {
        // VLC-style bar right under the picture, full width
        MoveWindow(mediaBar_.hwnd(), c.left, c.bottom + Dpi(4), c.right - c.left, mediaBar_.PreferredHeight(), TRUE);
    } else if (pvState_ == PvState::Media) {
        // audio: the dark AIMP-style bar continues the song panel right under it
        MoveWindow(mediaBar_.hwnd(), c.left, c.bottom, c.right - c.left, mediaBar_.PreferredHeight(), TRUE);
    }
}

void MainWindow::UpdatePreviewButtons() {
    if (!pvBtnA_ || !media_) return;
    bool show = false, nav = false;
    if ((PagedImage() && docPages_ > 1) ||
        (pvKind_ == preview::Kind::Xlsx && pvState_ == PvState::Grid && docPages_ > 1)) {
        // compact ◀ ▶ (the page / slide / sheet number is in the info line)
        SetWindowTextW(pvBtnA_, L"◀");
        SetWindowTextW(pvBtnB_, L"▶");
        EnableWindow(pvBtnA_, docPage_ > 0);
        EnableWindow(pvBtnB_, docPage_ + 1 < docPages_);
        show = nav = true;
    }                                           // audio / video: play, mute ... are in the media bar
    ShowWindow(pvBtnA_, show ? SW_SHOW : SW_HIDE);
    ShowWindow(pvBtnB_, show ? SW_SHOW : SW_HIDE);

    // zoom: [−] [125%] [+]
    const bool zoom = CanZoom();
    if (zoom) {
        double lo, hi;
        ZoomRange(lo, hi);
        SetWindowTextW(pvZoomPct_, ZoomLabel().c_str());
        EnableWindow(pvZoomOut_, zoom_ > lo + 1e-6);
        EnableWindow(pvZoomIn_, zoom_ < hi - 1e-6);
    }
    for (HWND b : {pvZoomOut_, pvZoomPct_, pvZoomIn_}) ShowWindow(b, zoom ? SW_SHOW : SW_HIDE);

    // seek bar (video / audio)
    const bool seek = pvState_ == PvState::Media && media_->IsOpen();
    for (HWND b : {pvBack_, pvSeek_, pvFwd_}) ShowWindow(b, SW_HIDE);       // old audio row, replaced by the bar
    ShowWindow(mediaBar_.hwnd(), seek ? SW_SHOW : SW_HIDE);
    if (seek) {
        SetTimer(preview_, kTimerMedia, 250, nullptr);
        UpdateSeekBar();
    } else {
        KillTimer(preview_, kTimerMedia);
    }
    (void)nav;
    // re-place the footer buttons
    RECT rc;
    GetClientRect(preview_, &rc);
    RECT f{0, PreviewContentRect().bottom, rc.right, rc.bottom};
    InvalidateRect(preview_, &f, FALSE);
}

int MainWindow::SeekRowH() const {
    if (IsVideo()) return mediaBar_.PreferredHeight() + Dpi(4);
    if (pvState_ == PvState::Media) return mediaBar_.PreferredHeight();
    return pvState_ == PvState::Media ? Dpi(38) : 0;
}

RECT MainWindow::PreviewContentRect() const {
    RECT rc;
    GetClientRect(preview_, &rc);
    const int pad = Dpi(12);
    return RECT{pad, pad, std::max<LONG>(pad + 1, rc.right - pad),
                std::max<LONG>(pad + 1, rc.bottom - Dpi(kFooterH) - SeekRowH())};
}

// ------------------------------------------------------------ zoom

bool MainWindow::CanZoom() const {
    switch (pvState_) {
    case PvState::Image: return !pvImage_.pixels.empty();
    case PvState::Text:
    case PvState::Rich: return true;
    case PvState::Grid: return sheet_ && sheet_->rows;
    default: return false;
    }
}

void MainWindow::ZoomRange(double& lo, double& hi) const {
    lo = 1.0;
    hi = 4.0;
    if (pvState_ == PvState::Image) {
        if (pvKind_ == preview::Kind::Image) {
            // pictures: up to 8 screen pixels per picture pixel
            const RECT c = PreviewContentRect();
            const double aw = std::max<LONG>(1, c.right - c.left), ah = std::max<LONG>(1, c.bottom - c.top);
            const double fit = std::min({1.0, aw / pvImage_.width, ah / pvImage_.height});
            const double origK = pvImage_.originalWidth ? static_cast<double>(pvImage_.width) / pvImage_.originalWidth : 1.0;
            hi = std::max(1.0, 8.0 * origK / fit);
        } else {
            hi = 6.0;                                   // PDF page / slide (re-rendered sharper up to 2560 px)
        }
    } else {
        lo = 0.5;                                       // text / Word / Excel: 50% .. 300%
        hi = 3.0;
    }
}

std::wstring MainWindow::ZoomLabel() {
    double pct = zoom_ * 100;
    if (pvState_ == PvState::Image && pvKind_ == preview::Kind::Image && pvImage_.originalWidth) {
        // pictures: percent of the real size (100% = one screen pixel per picture pixel)
        const RECT c = PreviewContentRect();
        double x0, y0, dw, dh;
        ImagePlacement(c, x0, y0, dw, dh);
        pct = dw / pvImage_.originalWidth * 100;
    }
    if (pvState_ != PvState::Image || pvKind_ != preview::Kind::Image) {
        if (std::abs(zoom_ - 1.0) < 1e-6) return L"100%";
    } else if (std::abs(zoom_ - 1.0) < 1e-6 && pct < 99.5) {
        return L"Fit";
    }
    return std::to_wstring(static_cast<int>(pct + 0.5)) + L"%";
}

// Where the picture goes in `area` at the current zoom; keeps the view centre inside the picture.
void MainWindow::ImagePlacement(const RECT& area, double& x0, double& y0, double& dw, double& dh) {
    const double aw = std::max<LONG>(1, area.right - area.left), ah = std::max<LONG>(1, area.bottom - area.top);
    const double W = std::max<uint32_t>(1, pvImage_.width), H = std::max<uint32_t>(1, pvImage_.height);
    const double fit = std::min({1.0, aw / W, ah / H});            // never enlarge a small picture at "fit"
    const double s = fit * zoom_;
    dw = W * s;
    dh = H * s;
    if (dw <= aw) { x0 = area.left + (aw - dw) / 2; viewCx_ = 0.5; }
    else {
        x0 = std::clamp(area.left + aw / 2 - viewCx_ * dw, area.right - dw, static_cast<double>(area.left));
        viewCx_ = (area.left + aw / 2 - x0) / dw;
    }
    if (dh <= ah) { y0 = area.top + (ah - dh) / 2; viewCy_ = 0.5; }
    else {
        y0 = std::clamp(area.top + ah / 2 - viewCy_ * dh, area.bottom - dh, static_cast<double>(area.top));
        viewCy_ = (area.top + ah / 2 - y0) / dh;
    }
}

bool MainWindow::ImagePannable() {
    if (pvState_ != PvState::Image || pvImage_.pixels.empty()) return false;
    const RECT c = PreviewContentRect();
    double x0, y0, dw, dh;
    ImagePlacement(c, x0, y0, dw, dh);
    return dw > c.right - c.left + 0.5 || dh > c.bottom - c.top + 0.5;
}

void MainWindow::SetZoom(double z, const POINT* anchor) {
    if (!CanZoom()) return;
    double lo, hi;
    ZoomRange(lo, hi);
    z = std::clamp(z, lo, hi);
    if (std::abs(z - 1.0) < 0.03) z = 1.0;
    if (std::abs(z - zoom_) < 1e-9) return;
    if (pvState_ == PvState::Image) {
        const RECT c = PreviewContentRect();
        double x0, y0, dw, dh;
        ImagePlacement(c, x0, y0, dw, dh);
        // keep the picture point under the anchor (mouse) or the view centre still
        const double ax = anchor ? anchor->x : (c.left + c.right) / 2.0;
        const double ay = anchor ? anchor->y : (c.top + c.bottom) / 2.0;
        const double u = (ax - x0) / dw, v = (ay - y0) / dh;
        const double k = z / zoom_;
        const double ndw = dw * k, ndh = dh * k;
        viewCx_ = ((c.left + c.right) / 2.0 - (ax - u * ndw)) / ndw;
        viewCy_ = ((c.top + c.bottom) / 2.0 - (ay - v * ndh)) / ndh;
        zoom_ = z;
        InvalidateRect(preview_, &c, FALSE);
        // PDF / slide: re-render sharper once the zoom settles
        if (PagedImage()) SetTimer(preview_, kTimerDocZoom, 250, nullptr);
    } else {
        zoom_ = z;
        ApplyChildZoom();
    }
    UpdatePreviewButtons();
}

// Text / Word / Excel follow zoom_ (RichEdit zoom, or a scaled font for the text and grid views).
void MainWindow::ApplyChildZoom() {
    if (pvState_ == PvState::Rich) {
        const int z = static_cast<int>(zoom_ * 100 + 0.5);
        SendMessageW(pvRich_, EM_SETZOOM, z == 100 ? 0 : z, z == 100 ? 0 : 100);
        return;
    }
    if (pvState_ == PvState::Grid) { sheetView_.SetZoom(zoom_); return; }
    if (pvState_ != PvState::Text) return;
    HWND target = pvText_;
    HFONT base = (pvState_ == PvState::Text && !pvTextUiFont_) ? monoFont_ : font_;
    HFONT use = base;
    HFONT old = zoomFont_;
    zoomFont_ = nullptr;
    if (std::abs(zoom_ - 1.0) > 1e-6) {
        LOGFONTW lf{};
        GetObjectW(base, sizeof(lf), &lf);
        lf.lfHeight = static_cast<LONG>(lf.lfHeight * zoom_);
        lf.lfWidth = 0;
        zoomFont_ = CreateFontIndirectW(&lf);
        if (zoomFont_) use = zoomFont_;
    }
    SendMessageW(target, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
    if (old) DeleteObject(old);
}

// ------------------------------------------------------------ seek bar

void MainWindow::UpdateSeekBar() {
    mediaBar_.Refresh();
    if (!media_ || !media_->IsOpen()) return;
    const double d = media_->Duration();
    EnableWindow(pvSeek_, d > 0);
    EnableWindow(pvBack_, d > 0);
    EnableWindow(pvFwd_, d > 0);
    if (!seeking_ && d > 0)
        SendMessageW(pvSeek_, TBM_SETPOS, TRUE, static_cast<LPARAM>(media_->Position() / d * 1000 + 0.5));
}

void MainWindow::OnSeekScroll(int code) {
    if (!media_ || !media_->IsOpen()) return;
    const double d = media_->Duration();
    if (d <= 0) return;
    const int pos = static_cast<int>(SendMessageW(pvSeek_, TBM_GETPOS, 0, 0));
    if (code == TB_ENDTRACK) { seeking_ = false; return; }
    if (code == TB_THUMBTRACK) seeking_ = true;
    media_->Seek(d * pos / 1000.0);
    RECT rc;
    GetClientRect(preview_, &rc);
    RECT f{0, PreviewContentRect().bottom, rc.right, rc.bottom};
    InvalidateRect(preview_, &f, FALSE);
}

// ------------------------------------------------------------ audio: AIMP-style song panel
//  ┌────────┐  Title (or file name)
//  │ cover  │  Artist
//  │        │  Album · 2021 · Genre
//  └────────┘  MP3 :: 44.1 kHz, 320 kbps, stereo
//              0:42  / 3:15          (click the time: elapsed <-> remaining)
//              Playing · sound off
void MainWindow::PaintAudioPanel(HDC dc, const RECT& area) {
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    SetDCBrushColor(dc, MediaBar::AimpBack());
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (aw < Dpi(60) || ah < Dpi(60) || !index_ || !pvNode_) return;
    auto font = [&](HFONT& f, int px, int weight, const wchar_t* face) {
        if (f) return f;
        LOGFONTW lf{};
        lf.lfHeight = -Dpi(px);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpyW(lf.lfFaceName, face);
        f = CreateFontIndirectW(&lf);
        return f;
    };
    HFONT titleF = font(audioTitleFont_, 21, FW_SEMIBOLD, L"Segoe UI");
    HFONT timeF = font(audioTimeFont_, 44, FW_LIGHT, L"Segoe UI");
    HFONT glyphF = font(audioGlyphFont_, 60, FW_NORMAL, L"Segoe MDL2 Assets");

    const std::wstring name = index_->node(pvNode_).name;
    const preview::AudioTags none;
    const preview::AudioTags& t = audio_ ? *audio_ : none;
    std::wstring title = t.title;
    if (title.empty()) {
        title = name;
        const auto dot = title.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) title.resize(dot);
    }
    std::wstring albumLine = t.album;
    for (const auto* s : {&t.year, &t.genre})
        if (!s->empty()) albumLine += (albumLine.empty() ? L"" : L"  ·  ") + *s;
    const std::wstring format = preview::AudioFormatLine(t, name);

    // cover on the left (wide pane) or on top (narrow pane)
    const int pad = Dpi(20);
    const bool wide = aw >= Dpi(430);
    int cs = wide ? std::min({ah - 2 * pad, aw * 2 / 5, Dpi(260)}) : std::min({aw - 2 * pad, ah * 2 / 5, Dpi(200)});
    cs = std::max(cs, Dpi(48));
    const int textH = Dpi(196);
    RECT cover;
    if (wide) {
        const int top = area.top + std::max(pad, (ah - cs) / 2);
        cover = {area.left + pad, top, area.left + pad + cs, top + cs};
    } else {
        const int top = area.top + std::max(pad, (ah - cs - textH - Dpi(12)) / 2);
        cover = {area.left + (aw - cs) / 2, top, area.left + (aw - cs) / 2 + cs, top + cs};
    }
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    if (coverBmp_ && audio_ && audio_->cover.width && audio_->cover.height) {
        // fit inside the square, aspect kept
        const double s = std::min(static_cast<double>(cs) / audio_->cover.width, static_cast<double>(cs) / audio_->cover.height);
        const int w = std::max(1, static_cast<int>(audio_->cover.width * s)), h = std::max(1, static_cast<int>(audio_->cover.height * s));
        const int x = cover.left + (cs - w) / 2, y = cover.top + (cs - h) / 2;
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, coverBmp_);
        StretchBlt(dc, x, y, w, h, mem, 0, 0, static_cast<int>(audio_->cover.width), static_cast<int>(audio_->cover.height), SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        // no cover in the file: AIMP's dark tile with a note
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, RGB(0x42, 0x39, 0x35));
        SetDCPenColor(dc, theme::kPlayerLine);
        RoundRect(dc, cover.left, cover.top, cover.right, cover.bottom, Dpi(10), Dpi(10));
        SelectObject(dc, glyphF);
        SetTextColor(dc, MediaBar::AimpDim());
        RECT g = cover;
        DrawTextW(dc, L"\xE8D6", -1, &g, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }

    // text column
    RECT col;
    UINT align;
    if (wide) {
        const int top = area.top + std::max(pad, (ah - textH) / 2);
        col = {cover.right + Dpi(24), top, area.right - pad, area.bottom - pad};
        align = DT_LEFT;
    } else {
        col = {area.left + pad, cover.bottom + Dpi(12), area.right - pad, area.bottom - Dpi(4)};
        align = DT_CENTER;
    }
    int y = col.top;
    auto line = [&](const std::wstring& s, HFONT f, COLORREF c, int h) {
        if (y + h > col.bottom + Dpi(4)) return RECT{};
        RECT r{col.left, y, col.right, y + h};
        if (!s.empty()) {
            SelectObject(dc, f);
            SetTextColor(dc, c);
            DrawTextW(dc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | align);
        }
        y += h;
        return r;
    };
    SetBkMode(dc, TRANSPARENT);
    line(title, titleF, MediaBar::AimpText(), Dpi(32));
    line(t.artist.empty() ? std::wstring(L"Unknown artist") : t.artist, font_, t.artist.empty() ? MediaBar::AimpDim() : MediaBar::AimpText(), Dpi(22));
    if (!albumLine.empty()) line(albumLine, font_, MediaBar::AimpDim(), Dpi(22));
    line(format, font_, MediaBar::AimpAccent(), Dpi(22));
    y += Dpi(8);

    // big time: elapsed (or remaining) + the total
    const bool open = media_ && media_->IsOpen();
    const double d = open ? media_->Duration() : 0, pos = open ? media_->Position() : 0;
    auto fmtTime = [](double s) {
        const long v = static_cast<long>(std::max(0.0, s));
        wchar_t b[32];
        if (v >= 3600) swprintf_s(b, L"%ld:%02ld:%02ld", v / 3600, (v / 60) % 60, v % 60);
        else swprintf_s(b, L"%ld:%02ld", v / 60, v % 60);
        return std::wstring(b);
    };
    const std::wstring big = audioRemaining_ && d > 0 ? L"-" + fmtTime(d - pos) : fmtTime(pos);
    const std::wstring total = d > 0 ? L"  /  " + fmtTime(d) : L"";
    SelectObject(dc, timeF);
    SIZE bs{};
    GetTextExtentPoint32W(dc, big.c_str(), static_cast<int>(big.size()), &bs);
    SelectObject(dc, font_);
    SIZE ts{};
    GetTextExtentPoint32W(dc, total.c_str(), static_cast<int>(total.size()), &ts);
    const int th = Dpi(56);
    if (y + th <= col.bottom + Dpi(4)) {
        const int x0 = align == DT_CENTER ? col.left + (col.right - col.left - bs.cx - ts.cx) / 2 : col.left;
        audioTimeRc_ = {x0, y, x0 + bs.cx, y + th};
        SelectObject(dc, timeF);
        SetTextColor(dc, MediaBar::AimpText());
        RECT br = audioTimeRc_;
        DrawTextW(dc, big.c_str(), -1, &br, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        SelectObject(dc, font_);
        SetTextColor(dc, MediaBar::AimpDim());
        RECT tr{audioTimeRc_.right, y + Dpi(8), audioTimeRc_.right + ts.cx + Dpi(4), y + th};
        DrawTextW(dc, total.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        y += th;
    } else {
        SetRectEmpty(&audioTimeRc_);
    }
    std::wstring state = !open ? L"" : media_->IsPlaying() ? L"Playing" : L"Paused";
    if (open && media_->IsMuted()) state += L"  ·  sound off";
    line(state, font_, MediaBar::AimpDim(), Dpi(22));
}

// ⏮ / ⏭ in the song panel: select the previous / next audio file of the list (wraps around), like a playlist.
void MainWindow::StepMediaFile(int dir) {
    if (!index_ || view_.empty()) return;
    const int n = static_cast<int>(view_.size());
    int cur = ListView_GetNextItem(list_, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (cur < 0) cur = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (cur < 0) cur = dir > 0 ? -1 : n;
    for (int k = 1; k <= n; ++k) {
        const int i = ((cur + dir * k) % n + n) % n;
        const auto& node = index_->node(view_[static_cast<size_t>(i)]);
        if (preview::KindForName(node.name) != preview::Kind::Audio) continue;
        if (i == cur) return;                                // the only song
        // the next song keeps playing with sound if the user had turned it on
        carrySound_ = media_ && media_->IsOpen() && !media_->IsMuted();
        if (carrySound_) carryVolume_ = media_->Volume();
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, i, FALSE);
        return;
    }
}

void MainWindow::PaintFileContent(HDC dc, const RECT& area) {
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    if (pvState_ == PvState::Loading) {
        SetTextColor(dc, theme::kTextDim);
        RECT r = area;
        const wchar_t* msg = (pvKind_ == preview::Kind::Video || pvKind_ == preview::Kind::Audio) ? L"Preparing to play..."
                             : (pvKind_ == preview::Kind::Dwg || pvKind_ == preview::Kind::Dxf) ? L"Opening the drawing..."
                                                                                                   : L"Loading preview...";
        DrawTextW(dc, msg, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    if (pvState_ == PvState::Media && pvKind_ == preview::Kind::Audio) {
        PaintAudioPanel(dc, area);                 // AIMP-style song panel (the controls are in the bar below)
        return;
    }
    if (pvState_ == PvState::Grid && (!sheet_ || !sheet_->rows)) {
        SetTextColor(dc, theme::kTextDim);
        RECT r = area;
        const std::wstring msg = (sheet_ && !sheet_->note.empty()) ? sheet_->note : L"This sheet is empty.";
        DrawTextW(dc, msg.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    if (pvState_ != PvState::Image || pvImage_.pixels.empty() || aw <= 0 || ah <= 0) return;

    // Placement at the current zoom (zoom 1 = fit: whole picture, aspect kept, small pictures not enlarged).
    double x0, y0, dw, dh;
    ImagePlacement(area, x0, y0, dw, dh);
    const double s = dw / pvImage_.width;                       // screen px per picture px
    // Only the visible part of the picture is resampled (zoomed-in pictures stay cheap).
    const double vx0 = std::max<double>(area.left, x0), vx1 = std::min<double>(area.right, x0 + dw);
    const double vy0 = std::max<double>(area.top, y0), vy1 = std::min<double>(area.bottom, y0 + dh);
    if (vx1 <= vx0 || vy1 <= vy0) return;
    RECT srcR{static_cast<LONG>(std::floor((vx0 - x0) / s)), static_cast<LONG>(std::floor((vy0 - y0) / s)),
              static_cast<LONG>(std::ceil((vx1 - x0) / s)), static_cast<LONG>(std::ceil((vy1 - y0) / s))};
    srcR.left = std::clamp<LONG>(srcR.left, 0, static_cast<LONG>(pvImage_.width) - 1);
    srcR.top = std::clamp<LONG>(srcR.top, 0, static_cast<LONG>(pvImage_.height) - 1);
    srcR.right = std::clamp<LONG>(srcR.right, srcR.left + 1, static_cast<LONG>(pvImage_.width));
    srcR.bottom = std::clamp<LONG>(srcR.bottom, srcR.top + 1, static_cast<LONG>(pvImage_.height));
    const int tw = std::max(1, static_cast<int>((srcR.right - srcR.left) * s + 0.5));
    const int th = std::max(1, static_cast<int>((srcR.bottom - srcR.top) * s + 0.5));
    const int x = static_cast<int>(std::lround(x0 + srcR.left * s)), y = static_cast<int>(std::lround(y0 + srcR.top * s));

    if (!pvScaled_ || pvScaledW_ != tw || pvScaledH_ != th || pvScaledS_ != s || !EqualRect(&pvScaledSrc_, &srcR)) {
        // High-quality resample with WIC, once per view (pane size / zoom / pan position).
        if (pvScaled_) { DeleteObject(pvScaled_); pvScaled_ = nullptr; }
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> f;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
        if (!wicSrc_ &&
            FAILED(f->CreateBitmapFromMemory(pvImage_.width, pvImage_.height, GUID_WICPixelFormat32bppPBGRA,
                                             pvImage_.width * 4, static_cast<UINT>(pvImage_.pixels.size()),
                                             pvImage_.pixels.data(), &wicSrc_)))
            return;
        ComPtr<IWICBitmapSource> src = wicSrc_;
        if (srcR.left || srcR.top || srcR.right != static_cast<LONG>(pvImage_.width) || srcR.bottom != static_cast<LONG>(pvImage_.height)) {
            ComPtr<IWICBitmapClipper> clip;
            const WICRect wr{srcR.left, srcR.top, srcR.right - srcR.left, srcR.bottom - srcR.top};
            if (FAILED(f->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(wicSrc_, &wr))) return;
            src = clip;
        }
        if (tw != srcR.right - srcR.left || th != srcR.bottom - srcR.top) {
            ComPtr<IWICBitmapScaler> sc;
            if (FAILED(f->CreateBitmapScaler(&sc)) ||
                FAILED(sc->Initialize(src.Get(), static_cast<UINT>(tw), static_cast<UINT>(th), WICBitmapInterpolationModeHighQualityCubic)))
                return;
            src = sc;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = tw;
        bi.bmiHeader.biHeight = -th;              // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) return;
        if (FAILED(src->CopyPixels(nullptr, static_cast<UINT>(tw) * 4, static_cast<UINT>(tw) * th * 4, static_cast<BYTE*>(bits)))) {
            DeleteObject(dib);
            return;
        }
        pvScaled_ = dib;
        pvScaledW_ = tw;
        pvScaledH_ = th;
        pvScaledS_ = s;
        pvScaledSrc_ = srcR;
    }

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);     // zoomed: never draw over the footer
    if (PagedImage()) {
        // A thin frame + soft shadow so the white page stands out from the pane.
        const int px = static_cast<int>(std::lround(x0)), py = static_cast<int>(std::lround(y0));
        const int pw = static_cast<int>(std::lround(dw)), ph = static_cast<int>(std::lround(dh));
        RECT sh{px + 2, py + 2, px + pw + 3, py + ph + 3};
        FillRect(dc, &sh, theme::Brush(theme::kLine));
        RECT fr{px - 1, py - 1, px + pw + 1, py + ph + 1};
        FrameRect(dc, &fr, theme::Brush(theme::kSage));
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, pvScaled_);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};   // transparent PNGs show the pane colour
    AlphaBlend(dc, x, y, tw, th, mem, 0, 0, tw, th, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
    RestoreDC(dc, saved);
}

void MainWindow::PaintFooter(HDC dc, const RECT& area, uint32_t node) {
    const auto& n = index_->node(node);
    const auto& e = index_->entries()[n.entry];
    RECT line{area.left, area.top, area.right, area.top + 1};
    FillRect(dc, &line, theme::Brush(theme::kLine));

    int y = area.top + Dpi(8);
    SelectObject(dc, boldFont_);
    SetTextColor(dc, theme::kText);
    RECT t{area.left, y, area.right, y + Dpi(24)};
    DrawTextW(dc, n.name.c_str(), -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += Dpi(26);

    SelectObject(dc, font_);
    SetTextColor(dc, theme::kTextDim);
    std::wstring info = TypeNameOf(n.name) + L"   ·   " + fmt::Size(n.size);
    if (pvKind_ == preview::Kind::Pdf && docPages_)
        info += L"   ·   Page " + std::to_wstring(docPage_ + 1) + L" of " + std::to_wstring(docPages_);
    else if (pvKind_ == preview::Kind::Pptx && docPages_)
        info += L"   ·   Slide " + std::to_wstring(docPage_ + 1) + L" of " + std::to_wstring(docPages_);
    else if (pvKind_ == preview::Kind::Xlsx && sheet_ && docPages_) {
        const std::wstring nm = docPage_ < sheet_->sheetNames.size() ? sheet_->sheetNames[docPage_] : L"";
        // LRM after the name: an Arabic sheet name must not pull the date that follows into right-to-left order.
        // (GDI shows the isolate marks FSI/PDI as boxes, so only LRM is used.)
        constexpr wchar_t kLrm = 0x200E;
        info += L"   ·   Sheet " + std::to_wstring(docPage_ + 1) + L" of " + std::to_wstring(docPages_) + L": " + kLrm + nm + kLrm;
        if (sheet_->truncated) info += L" (first " + std::to_wstring(sheet_->rows) + L" rows)";
    }
    else if ((pvKind_ == preview::Kind::Dwg || pvKind_ == preview::Kind::Dxf) && cad_ && docPage_ < cad_->views.size()) {
        // "Model" / "Layout 2 of 9: name" (LRM: an Arabic layout name must not reorder the line).
        constexpr wchar_t kLrm = 0x200E;
        const auto& vw = cad_->views[docPage_];
        info += L"   ·   " + (docPage_ == 0 ? std::wstring(L"Model")
                                             : L"Layout " + std::to_wstring(docPage_) + L" of " + std::to_wstring(docPages_ - 1) + L": " + kLrm + vw.name + kLrm);
        if (!pvDwgVersion_.empty()) info += L"   ·   " + pvDwgVersion_;
        if (cad_->truncated) info += L"   ·   partly shown (very large drawing)";
    }
    else if (pvKind_ == preview::Kind::Dwg && pvState_ == PvState::Image)
        info += L"   ·   " + pvDwgVersion_ + L" (saved thumbnail)" +
                (cadBusy_ ? L"   ·   opening the drawing..." : cadNote_.empty() ? L"" : L"   ·   " + cadNote_);
    else if (pvState_ == PvState::Image && pvImage_.originalWidth)
        info += L"   ·   " + std::to_wstring(pvImage_.originalWidth) + L" × " + std::to_wstring(pvImage_.originalHeight);
    else if (pvState_ == PvState::Media && media_->IsOpen() && media_->Duration() > 0)
        info += L"   ·   " + FormatTime(media_->Position()) + L" / " + FormatTime(media_->Duration());
    if (e.mtime.dwLowDateTime || e.mtime.dwHighDateTime) info += L"   ·   " + fmt::DateTime(e.mtime);
    RECT i{area.left, y, area.right, y + Dpi(22)};
    DrawTextW(dc, info.c_str(), -1, &i, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += Dpi(28);

    // [Extract & Open] [Extract...]          [−] [125%] [+]  [◀] [▶]
    // [Extract & Open] [Extract...]                     [Pause] [Sound on]
    const int bw = std::max(Dpi(120), theme::ButtonWidth(pvOpenBtn_, dpi_) + Dpi(6)), bh = Dpi(30), gap = Dpi(6), sg = Dpi(3);
    int right = area.right;
    if (pvState_ == PvState::Media && !IsVideo()) {
        const int cw = Dpi(108);
        MoveWindow(pvBtnB_, right - cw, y, cw, bh, TRUE);
        MoveWindow(pvBtnA_, right - 2 * cw - gap, y, cw, bh, TRUE);
        right -= 2 * cw + 2 * gap;
    } else {
        if (IsWindowVisible(pvBtnA_)) {
            const int nw = Dpi(34);
            MoveWindow(pvBtnB_, right - nw, y, nw, bh, TRUE);
            MoveWindow(pvBtnA_, right - 2 * nw - sg, y, nw, bh, TRUE);
            right -= 2 * nw + sg + 2 * gap;
        }
        if (IsWindowVisible(pvZoomIn_)) {
            const int zw = Dpi(30), pw = Dpi(54);
            MoveWindow(pvZoomIn_, right - zw, y, zw, bh, TRUE);
            MoveWindow(pvZoomPct_, right - zw - sg - pw, y, pw, bh, TRUE);
            MoveWindow(pvZoomOut_, right - 2 * zw - 2 * sg - pw, y, zw, bh, TRUE);
            right -= 2 * zw + pw + 2 * sg + gap;
        }
    }
    MoveWindow(pvOpenBtn_, area.left, y, bw, bh, TRUE);
    // a narrow pane drops "Extract..." (still in the toolbar) rather than overlapping
    const bool fits = area.left + bw + gap + Dpi(96) <= right;
    MoveWindow(pvExtractBtn_, area.left + bw + gap, y, Dpi(96), bh, TRUE);
    if (IsWindowVisible(pvOpenBtn_)) ShowWindow(pvExtractBtn_, fits ? SW_SHOW : SW_HIDE);
}

void MainWindow::PaintPreview(HWND wnd, HDC dc) {
    RECT rc;
    GetClientRect(wnd, &rc);
    FillRect(dc, &rc, theme::Brush(theme::kBack));
    RECT edge{rc.left, rc.top, rc.left + 1, rc.bottom};
    FillRect(dc, &edge, theme::Brush(theme::kLine));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);

    // ---- a single previewable file: show the file itself
    if (index_ && pvNode_ && (pvState_ == PvState::Loading || pvState_ == PvState::Image || pvState_ == PvState::Text ||
                              pvState_ == PvState::Media || pvState_ == PvState::Rich || pvState_ == PvState::Grid)) {
        const RECT content = PreviewContentRect();
        PaintFileContent(dc, content);
        const int pad = Dpi(12);
        PaintFooter(dc, RECT{pad, content.bottom + SeekRowH() + Dpi(6), rc.right - pad, rc.bottom - pad}, pvNode_);
        return;
    }

    const int pad = Dpi(18);
    int y = pad;
    const int labelW = Dpi(120);
    auto title = [&](const std::wstring& t) {
        SelectObject(dc, boldFont_);
        SetTextColor(dc, theme::kText);
        RECT r{pad, y, rc.right - pad, y + Dpi(60)};
        DrawTextW(dc, t.c_str(), -1, &r, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        RECT calc = r;
        DrawTextW(dc, t.c_str(), -1, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EDITCONTROL);
        y += std::min<int>(calc.bottom - calc.top, Dpi(60)) + Dpi(12);
        SelectObject(dc, font_);
    };
    auto row = [&](const wchar_t* label, const std::wstring& value) {
        if (value.empty()) return;
        SetTextColor(dc, theme::kTextDim);
        RECT l{pad, y, pad + labelW, y + Dpi(22)};
        DrawTextW(dc, label, -1, &l, DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, theme::kText);
        RECT v{pad + labelW, y, rc.right - pad, y + Dpi(22)};
        DrawTextW(dc, value.c_str(), -1, &v, DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        y += Dpi(22);
    };
    SelectObject(dc, font_);

    if (!index_) {
        title(L"Mechano Reader");
        SetTextColor(dc, theme::kTextDim);
        RECT r{pad, y, rc.right - pad, rc.bottom - pad};
        DrawTextW(dc,
                  L"Open an archive to browse it without extracting.\n\n"
                  L"• Ctrl+O  open an archive\n• Ctrl+N  create a new archive\n"
                  L"• Drag files here to add them\n• Ctrl+E  extract to a folder\n"
                  L"• Enter   open folder / open file\n• Backspace  go up\n• Esc  cancel\n\n"
                  L"Select a file (picture, PDF, Word, Excel, PowerPoint, video, audio or text) to see it here.",
                  -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        return;
    }

    const auto sel = SelectedNodes();
    const auto& info = index_->info();
    if (sel.size() > 1) {
        uint64_t size = 0, packed = 0, files = 0;
        for (auto n : sel) { size += index_->node(n).size; packed += index_->node(n).packSize; files += index_->node(n).fileCount; }
        title(fmt::Number(sel.size()) + L" items selected");
        row(L"Files", fmt::Number(files));
        row(L"Size", fmt::Size(size));
        row(L"Compressed", fmt::Size(packed) + L"   (" + fmt::Ratio(packed, size) + L")");
        return;
    }
    if (sel.empty()) {
        // Archive properties (brief section 25).
        uint64_t folders = 0;
        for (const auto& n : index_->nodes()) if (n.isDir) ++folders;
        title(FileNameOf(info.path));
        row(L"Location", FolderOf(info.path));
        row(L"Format", info.formatName + (info.isSolid ? L" (solid)" : L""));
        row(L"Files", fmt::Number(index_->totalFiles()));
        row(L"Folders", fmt::Number(folders ? folders - 1 : 0));
        row(L"Original size", fmt::Size(index_->totalSize()));
        const uint64_t phys = info.physicalSize ? info.physicalSize : index_->totalPacked();
        row(L"Archive size", fmt::Size(phys));
        row(L"Ratio", fmt::Ratio(phys, index_->totalSize()));
        row(L"Encrypted", info.hasEncrypted ? L"Yes (some or all files)" : L"No");
        row(L"Can be edited", info.canUpdate ? L"Yes" : L"No (read-only format)");
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(fs::ToLongPath(info.path).c_str(), GetFileExInfoStandard, &fa)) {
            row(L"Created", fmt::DateTime(fa.ftCreationTime));
            row(L"Modified", fmt::DateTime(fa.ftLastWriteTime));
        }
        if (!info.comment.empty()) row(L"Comment", info.comment);
        for (const auto& w : info.warnings) row(L"Warning", w);
        return;
    }

    const uint32_t nid = sel[0];
    const auto& n = index_->node(nid);
    const archive::EntryRecord* e = n.entry != ArchiveIndex::kNoEntry ? &index_->entries()[n.entry] : nullptr;
    title(n.name);
    if (n.isDir) {
        row(L"Type", L"Folder");
        row(L"Files inside", fmt::Number(n.fileCount));
        row(L"Size", fmt::Size(n.size));
        row(L"Compressed", fmt::Size(n.packSize));
    } else {
        SHFILEINFOW sfi{};
        SHGetFileInfoW((L"x" + Ext(n.name)).c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME);
        row(L"Type", sfi.szTypeName);
        row(L"Size", fmt::Size(n.size) + L"  (" + fmt::Number(n.size) + L" bytes)");
        row(L"Compressed", fmt::Size(n.packSize) + L"  (" + fmt::Ratio(n.packSize, n.size) + L")");
    }
    if (e) {
        row(L"Method", index_->info().methods[e->method]);
        row(L"Modified", fmt::DateTime(e->mtime));
        if (e->hasCrc && !n.isDir) row(L"CRC32", fmt::Hex32(e->crc));
        if (e->encrypted) row(L"Encrypted", L"Yes");
        if (e->isLink) row(L"Note", L"Link entry (will not be created as a link)");
    }
    row(L"In archive", index_->PathOf(nid));
    if (!n.isDir && IsRiskyExtension(n.name)) row(L"Caution", L"This file type can run programs.");

    // Position the action buttons under the info rows (brief section 14).
    y += Dpi(14);
    const int bw = std::max(Dpi(130), theme::ButtonWidth(pvOpenBtn_, dpi_) + Dpi(6)), bh = Dpi(30);
    MoveWindow(pvOpenBtn_, pad, y, bw, bh, TRUE);
    MoveWindow(pvExtractBtn_, pad + bw + Dpi(8), y, bw, bh, TRUE);
    y += bh + Dpi(16);
    SetTextColor(dc, theme::kTextDim);
    RECT r{pad, y, rc.right - pad, rc.bottom - pad};
    const std::wstring note = !pvMessage_.empty() ? L"No preview: " + pvMessage_
                                                  : L"No preview for this file type.";
    if (!n.isDir) DrawTextW(dc, note.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX);
}

// ============================================================ commands

void MainWindow::OpenArchive(const std::wstring& path, uint32_t codePage) {
    if (!BeginJob(L"Opening " + FileNameOf(path) + L"...")) return;
    auto pw = MakePasswordProvider();
    auto cancel = cancel_;
    HWND h = hwnd_;
    worker_.Post([path, codePage, pw, cancel, h] {
        auto* r = new OpenResult;
        const auto t0 = std::chrono::steady_clock::now();
        archive::OpenOptions oo;
        oo.codePage = codePage;
        r->result = archive::Archive::Open(path, oo, pw, cancel.get());
        r->codePage = codePage;
        r->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!PostMessageW(h, WM_APP_OPENED, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::CmdOpen() {
    auto files = PickArchivesToOpen(hwnd_);
    if (files.size() == 1) OpenArchive(files[0]);
    else if (files.size() > 1) AskBatch(std::move(files));
}

void MainWindow::CmdNew(std::vector<std::wstring> files) {
    if (busy_) { BeginJob(L""); return; }
    if (files.empty()) {
        TASKDIALOG_BUTTON buttons[] = {{1, L"Choose files...\nPick one or more files"},
                                       {2, L"Choose a folder...\nCompress a whole folder with everything inside"}};
        TASKDIALOGCONFIG tc{sizeof(tc)};
        tc.hwndParent = hwnd_;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = L"Mechano Reader";
        tc.pszMainInstruction = L"What do you want to put in the new archive?";
        tc.pszContent = L"Tip: you can also drag files and folders onto the window.";
        tc.cButtons = 2;
        tc.pButtons = buttons;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        int choice = 0;
        TaskDialogIndirect(&tc, &choice, nullptr, nullptr);
        if (choice == 1) files = PickFilesToAdd(hwnd_);
        else if (choice == 2) { if (auto f = PickFolder(hwnd_, L"Folder to compress")) files.push_back(*f); }
        if (files.empty()) return;
    }
    std::wstring suggested = FileNameOf(files[0]);
    if (files.size() == 1 && !fs::IsDirectory(files[0])) {
        const auto dot = suggested.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) suggested.resize(dot);
    } else if (files.size() > 1) {
        suggested = FileNameOf(FolderOf(files[0]));
        if (suggested.empty()) suggested = L"Archive";
    }
    auto choice = PickNewArchive(hwnd_, suggested + L".zip", FolderOf(files[0]));
    if (!choice) return;
    StartCreate(std::move(files), choice->path, choice->settings);
    SecureZeroMemory(choice->settings.password.data(), choice->settings.password.size() * sizeof(wchar_t));
}

void MainWindow::StartCreate(std::vector<std::wstring> files, std::wstring path, archive::CompressionSettings settings) {
    if (!BeginJob(L"Creating " + FileNameOf(path) + L"...")) return;

    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    NewArchiveChoice choice{std::move(path), std::move(settings)};
    worker_.Post([files, c = std::move(choice), cancel, prog, h]() mutable {
        auto* r = new JobResult;
        r->title = L"Create archive";
        std::vector<std::wstring> skipped;
        auto sources = fs::CollectSources(files, L"", &skipped);
        for (auto& s : skipped) r->report.issues.push_back({s, L"Skipped (link, or could not be read).", L""});
        auto res = archive::CreateArchive(c.path, sources, c.settings, prog.get(), *cancel, &r->report);
        SecureZeroMemory(c.settings.password.data(), c.settings.password.size() * sizeof(wchar_t));
        if (!res) r->error = res.error();
        else {
            r->doneText = L"Created " + FileNameOf(c.path) + L" with " + fmt::Number(r->report.filesDone) + L" files.";
            r->openAfter = c.path;
            r->quiet = true;
        }
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::CmdAdd(std::vector<std::wstring> files) {
    if (!index_) { CmdNew(std::move(files)); return; }
    if (!index_->info().canUpdate) {
        ShowError(hwnd_, L"Cannot add files", ErrorInfo{ErrorCode::Internal,
                  L"This archive format is read-only. Extract it and create a new ZIP or 7z archive instead.",
                  L"Format: " + index_->info().formatName});
        return;
    }
    if (files.empty()) files = PickFilesToAdd(hwnd_);
    if (files.empty()) return;
    const std::wstring prefix = searchMode_ ? L"" : index_->PathOf(folder_);
    if (!BeginJob(L"Adding " + fmt::Number(files.size()) + L" item(s)...")) return;

    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    archive::CompressionSettings cs;
    cs.format = index_->info().formatName;
    worker_.Post([this, files, prefix, cs, cancel, prog, h] {
        auto* r = new JobResult;
        r->title = L"Add files";
        std::vector<std::wstring> skipped;
        archive::UpdatePlan plan;
        plan.add = fs::CollectSources(files, prefix, &skipped);
        for (auto& s : skipped) r->report.issues.push_back({s, L"Skipped (link, or could not be read).", L""});
        auto res = archive_->Update(plan, cs, prog.get(), *cancel, &r->report);
        if (!res) r->error = res.error();
        r->indexChanged = true;
        r->quiet = true;
        r->doneText = L"Added " + fmt::Number(r->report.filesDone) + L" file(s).";
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::CmdExtract(bool here) {
    if (!index_) { CmdOpen(); return; }
    const auto sel = SelectedNodes();
    const std::wstring archivePath = index_->info().path;
    std::wstring dest;
    if (here) {
        dest = FolderOf(archivePath);
    } else {
        auto picked = PickFolder(hwnd_, L"Extract to folder", FolderOf(archivePath));
        if (!picked) return;
        dest = *picked;
    }

    archive::ExtractOptions eo;
    std::vector<uint32_t> indices;
    if (sel.empty() && folder_ == ArchiveIndex::kRoot && !searchMode_) {
        // Whole archive -> Smart Extract (F5).
        eo.destination = archive::ComputeSmartTarget(*index_, archivePath, dest).destination;
    } else {
        const auto nodes = sel.empty() ? std::vector<uint32_t>{folder_} : sel;
        indices = index_->CollectArcIndices(nodes);
        eo.destination = dest;
        // Files land relative to the folder being viewed, like Explorer.
        if (!searchMode_ && folder_ != ArchiveIndex::kRoot) eo.stripPrefix = index_->PathOf(folder_) + L"\\";
        if (indices.empty()) return;
    }
    StartExtract(std::move(indices), std::move(eo));
}

void MainWindow::StartExtract(std::vector<uint32_t> indices, archive::ExtractOptions eo) {
    if (!BeginJob(L"Extracting to " + eo.destination + L"...")) return;

    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    worker_.Post([this, indices, eo, cancel, prog, h] {
        auto* r = new JobResult;
        r->title = L"Extract";
        r->report = archive_->Extract(indices, eo, prog.get(), *cancel);
        r->doneText = L"Extracted " + fmt::Number(r->report.filesDone) + L" file(s), " + fmt::Size(r->report.bytesDone) +
                      L"\nto " + eo.destination;
        r->quiet = true;
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

// ---- Explorer right-click menu (installed by the setup; see app/main.cpp for the command line)

namespace {
// "Compress to ZIP": <name>.zip next to the items. Never overwrites: "name (2).zip", "name (3).zip" ...
std::wstring ZipTargetFor(const std::vector<std::wstring>& paths, const std::wstring& destFolder = {}) {
    std::wstring base = FileNameOf(paths[0]);
    if (paths.size() == 1 && !fs::IsDirectory(paths[0])) {
        const auto dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) base.resize(dot);
    } else if (paths.size() > 1) {
        base = FileNameOf(FolderOf(paths[0]));
    }
    while (!base.empty() && (base.back() == L':' || base.back() == L'\\')) base.pop_back();
    if (base.size() < 2) base = L"Archive";
    const std::wstring folder = destFolder.empty() ? FolderOf(paths[0]) : destFolder;
    std::wstring target = folder + L"\\" + base + L".zip";
    for (int n = 2; fs::Exists(target) && n < 10000; ++n)
        target = folder + L"\\" + base + L" (" + std::to_wstring(n) + L").zip";
    return target;
}
} // namespace

void MainWindow::RunShellAction(ShellAction a, std::vector<std::wstring> paths) {
    if (paths.empty() || a == ShellAction::None) return;
    shellClose_ = true;
    switch (a) {
    case ShellAction::ExtractHere:
    case ShellAction::ExtractToFolder:
        if (paths.size() > 1) { StartBatch(a, std::move(paths)); return; }    // several archives selected
        shellAction_ = a;                         // runs when the archive has opened (WM_APP_OPENED)
        OpenArchive(paths[0]);
        return;
    case ShellAction::ZipEach:
        StartBatch(a, std::move(paths));
        return;
    case ShellAction::AddDialog:
        CmdNew(std::move(paths));                 // the usual "new archive" dialog (name, level, password)
        if (!busy_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);   // the user cancelled the dialog
        return;
    case ShellAction::AddZip: {
        const std::wstring target = ZipTargetFor(paths);
        StartCreate(std::move(paths), target, archive::CompressionSettings{});
        if (!busy_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }
    default: return;
    }
}

// The whole archive. "Extract here" = Smart Extract (one top item: straight into the folder;
// several: a "<name>\" folder). "Extract to <name>\" always ends in a folder of its own.
void MainWindow::ShellExtract(bool toFolder, std::wstring base) {
    if (!index_) return;
    const std::wstring ap = index_->info().path;
    if (base.empty()) base = FolderOf(ap);
    archive::ExtractOptions eo;
    const auto t = archive::ComputeSmartTarget(*index_, ap, base);
    eo.destination = t.destination;
    if (toFolder && !t.createdSubfolder) {
        const auto& root = index_->node(ArchiveIndex::kRoot);
        const bool singleFolder = root.children.size() == 1 && index_->node(root.children[0]).isDir;
        if (!singleFolder) eo.destination = base + L"\\" + archive::ArchiveBaseName(ap);
    }
    StartExtract({}, std::move(eo));
    if (!busy_ && !batch_.active) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

// ---- F14 batch

void MainWindow::StartBatch(ShellAction a, std::vector<std::wstring> items, std::wstring destBase) {
    if (busy_) { BeginJob(L""); return; }          // "please wait" message
    if (batch_.active || items.empty()) return;
    batch_ = Batch{};
    batch_.active = true;
    batch_.action = a;
    batch_.destBase = std::move(destBase);
    batch_.items = std::move(items);
    NextBatchItem();
}

void MainWindow::NextBatchItem() {
    if (batch_.stopped || batch_.next >= batch_.items.size()) { FinishBatch(); return; }
    const std::wstring item = batch_.items[batch_.next++];
    if (batch_.action == ShellAction::ZipEach) {
        StartCreate({item}, ZipTargetFor({item}, batch_.destBase), archive::CompressionSettings{});
    } else {
        shellAction_ = batch_.action;             // extract when it has opened (WM_APP_OPENED)
        OpenArchive(item);
    }
    if (!busy_) {                                 // could not start (should not happen): note it, go on
        batch_.problems.push_back({FileNameOf(item), L"Could not start."});
        shellAction_ = ShellAction::None;
        PostMessageW(hwnd_, WM_APP_BATCHNEXT, 0, 0);
    }
}

void MainWindow::FinishBatch() {
    const Batch b = std::move(batch_);
    batch_ = Batch{};
    const size_t total = b.items.size();
    const bool zip = b.action == ShellAction::ZipEach;
    std::wstring head = zip ? L"Compressed " : L"Extracted ";
    head += fmt::Number(b.ok) + L" of " + fmt::Number(total) + (zip ? L" item(s) to ZIP." : L" archive(s).");
    std::wstring sub = fmt::Number(b.files) + L" file(s), " + fmt::Size(b.bytes);
    if (b.stopped) sub += L"\nStopped: " + fmt::Number(total - b.next) + L" not done.";
    statusMessage_ = (b.problems.empty() && !b.stopped ? L"✔ " : L"⚠ ") + head + L" " + sub;
    for (auto& c : statusMessage_) if (c == L'\n') c = L' ';
    UpdateStatus();
    if (!b.problems.empty() || (b.stopped && !shellClose_)) {
        std::wstring detail;
        for (const auto& [name, what] : b.problems) detail += name + L": " + what + L"\n";
        const std::wstring prob = fmt::Number(b.problems.size()) + L" with problems";
        TASKDIALOGCONFIG tc{sizeof(tc)};
        tc.hwndParent = hwnd_;
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_EXPANDED_BY_DEFAULT;
        tc.pszWindowTitle = L"Mechano Reader";
        tc.pszMainIcon = b.problems.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON;
        tc.pszMainInstruction = head.c_str();
        tc.pszContent = sub.c_str();
        if (!detail.empty()) {
            tc.pszExpandedInformation = detail.c_str();
            tc.pszExpandedControlText = prob.c_str();
            tc.pszCollapsedControlText = prob.c_str();
        }
        tc.dwCommonButtons = TDCBF_OK_BUTTON;
        TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
    }
    if (shellClose_) { shellClose_ = false; PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
}

// Several archives dropped on the window or chosen in Open.
void MainWindow::AskBatch(std::vector<std::wstring> archives) {
    TASKDIALOG_BUTTON buttons[] = {
        {1, L"Extract each one here\nNext to each archive (a folder of its own when it has many items)"},
        {2, L"Extract all to a folder...\nEach archive in its own folder inside the folder you choose"},
        {3, L"Put them all in one new archive\nThe archives themselves, as files"},
        {4, L"Add them to the open archive\nThe archives themselves, as files"}};
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = hwnd_;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Mechano Reader";
    const std::wstring head = fmt::Number(archives.size()) + L" archives: what do you want to do?";
    tc.pszMainInstruction = head.c_str();
    tc.pszContent = L"Archives with a password ask for it one by one. Esc stops the rest.";
    tc.cButtons = index_ && index_->info().canUpdate ? 4 : 3;     // "add to the open archive" only when it can be changed
    tc.pButtons = buttons;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int choice = 0;
    TaskDialogIndirect(&tc, &choice, nullptr, nullptr);
    if (choice == 4) { CmdAdd(std::move(archives)); return; }
    if (choice == 1) StartBatch(ShellAction::ExtractHere, std::move(archives));
    else if (choice == 2) {
        if (auto f = PickFolder(hwnd_, L"Extract all archives to", FolderOf(archives[0])))
            StartBatch(ShellAction::ExtractToFolder, std::move(archives), *f);
    } else if (choice == 3) CmdNew(std::move(archives));
}

void MainWindow::CmdExtractAndOpen() {
    if (!index_) return;
    const auto sel = SelectedNodes();
    int focused = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    uint32_t nid = sel.size() == 1 ? sel[0] : (focused >= 0 && static_cast<size_t>(focused) < view_.size() ? view_[static_cast<size_t>(focused)] : 0);
    if (!nid) return;
    const auto& n = index_->node(nid);
    if (n.isDir || n.entry == ArchiveIndex::kNoEntry) return;
    const bool risky = IsRiskyExtension(n.name);
    if (risky &&
        !Confirm(hwnd_, L"Open a program from the archive?",
                 L"\"" + n.name + L"\" can run code on your computer. Only open it if you trust where this archive came from.")) return;

    archive::ExtractOptions eo;
    eo.destination = fs::NewTempSubfolder(L"op-");
    if (eo.destination.empty()) { MessageBeep(MB_ICONWARNING); return; }
    const std::wstring path = index_->PathOf(nid);
    const auto slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) eo.stripPrefix = path.substr(0, slash + 1);
    const uint32_t arcIndex = index_->entries()[n.entry].arcIndex;
    if (!BeginJob(L"Opening " + n.name + L"...")) return;

    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    const std::wstring launch = eo.destination + L"\\" + n.name;
    worker_.Post([this, arcIndex, eo, cancel, prog, h, launch, risky] {
        auto* r = new JobResult;
        r->title = L"Open in main program";
        r->riskConfirmed = risky;
        r->report = archive_->Extract({arcIndex}, eo, prog.get(), *cancel);
        r->quiet = true;
        if (r->report.filesDone == 1) {
            // The sanitizer may have renamed it; use what was actually written.
            r->launchFile = launch;
            if (!fs::Exists(launch)) {
                WIN32_FIND_DATAW fd{};
                HANDLE f = FindFirstFileW((eo.destination + L"\\*").c_str(), &fd);
                if (f != INVALID_HANDLE_VALUE) {
                    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { r->launchFile = eo.destination + L"\\" + fd.cFileName; break; } }
                    while (FindNextFileW(f, &fd));
                    FindClose(f);
                }
            }
        }
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

// ============================================================ Phase 5: context menu, drag out, shortcuts

void MainWindow::ShowContextMenu(LPARAM screenPt) {
    if (!index_) return;
    POINT pt{GET_X_LPARAM(screenPt), GET_Y_LPARAM(screenPt)};
    if (screenPt == -1) {
        // Shift+F10 / menu key: open under the focused row (or the list's corner).
        RECT r{};
        const int f = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
        if (f < 0 || !ListView_GetItemRect(list_, f, &r, LVIR_LABEL)) GetClientRect(list_, &r), r.bottom = r.top;
        pt = {r.left + Dpi(16), r.bottom};
        ClientToScreen(list_, &pt);
    }
    const auto sel = SelectedNodes();
    const bool one = sel.size() == 1, any = !sel.empty();
    const bool oneFile = one && !index_->node(sel[0]).isDir;
    const bool oneDir = one && index_->node(sel[0]).isDir;
    const bool idle = !busy_;
    auto flag = [](bool on) { return MF_STRING | (on ? 0u : MF_GRAYED); };

    HMENU m = CreatePopupMenu();
    if (oneDir) AppendMenuW(m, flag(true), ID_CTX_OPEN, L"Open folder\tEnter");
    else AppendMenuW(m, flag(oneFile && idle), ID_PV_OPEN, L"Open in main program\tEnter");
    AppendMenuW(m, flag(any), ID_CTX_QUICKLOOK, L"Quick Look\tSpace");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, flag(idle), ID_EXTRACT_TO, any ? L"Extract selected to...\tCtrl+E" : L"Extract all to...\tCtrl+E");
    AppendMenuW(m, flag(idle), ID_EXTRACT_HERE, L"Extract here\tAlt+E");
    AppendMenuW(m, flag(idle), ID_TEST, any ? L"Test selected\tAlt+T" : L"Test archive\tAlt+T");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, flag(idle), ID_ADD, L"Add files...\tAlt+A");
    AppendMenuW(m, flag(one && idle && index_->info().canUpdate), ID_CTX_RENAME, L"Rename\tF2");
    AppendMenuW(m, flag(any && idle), ID_DELETE, L"Delete from archive\tDel");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, flag(any), ID_CTX_COPYNAME, L"Copy name");
    AppendMenuW(m, flag(any), ID_CTX_COPYPATH, L"Copy path inside archive");
    AppendMenuW(m, flag(true), ID_ACC_SELECTALL, L"Select all\tCtrl+A");
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(static_cast<int>(cmd));
}

void MainWindow::CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(g)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

// Drag from the list to Explorer / the desktop: the selection is extracted (same safe
// path as Extract) to a private temp folder, then that folder's items are dragged.
// The temp folder is removed at the next start, because Explorer may still be copying.
void MainWindow::CmdDragOut() {
    if (!index_ || busy_) return;
    const auto sel = SelectedNodes();
    if (sel.empty()) return;
    const auto indices = index_->CollectArcIndices(sel);
    if (indices.empty()) return;
    archive::ExtractOptions eo;
    eo.destination = fs::NewTempSubfolder(L"dd-");
    if (eo.destination.empty()) { MessageBeep(MB_ICONWARNING); return; }
    if (!searchMode_ && folder_ != ArchiveIndex::kRoot) eo.stripPrefix = index_->PathOf(folder_) + L"\\";
    if (!BeginJob(L"Preparing " + fmt::Number(sel.size()) + L" item(s) to drag...")) return;
    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    worker_.Post([this, indices, eo, cancel, prog, h] {
        auto* r = new JobResult;
        r->title = L"Drag out";
        r->report = archive_->Extract(indices, eo, prog.get(), *cancel);
        r->quiet = true;
        r->doneText = L"Ready";
        r->dragDir = eo.destination;
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::DoDragOut(const std::wstring& dir) {
    // Shell data object for every item in the folder: Explorer gets the same thing as
    // when you drag a file from another Explorer window (files + shell item list).
    std::vector<PIDLIST_ABSOLUTE> owned;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName((dir + L"\\" + fd.cFileName).c_str(), nullptr, &pidl, 0, nullptr))) owned.push_back(pidl);
        } while (FindNextFileW(f, &fd) && owned.size() < 10000);
        FindClose(f);
    }
    PIDLIST_ABSOLUTE folderPidl = nullptr;
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    Microsoft::WRL::ComPtr<IDataObject> data;
    if (!owned.empty() && SUCCEEDED(SHParseDisplayName(dir.c_str(), nullptr, &folderPidl, 0, nullptr)) &&
        SUCCEEDED(SHBindToObject(nullptr, folderPidl, nullptr, IID_PPV_ARGS(&folder)))) {
        std::vector<PCUITEMID_CHILD> children;
        for (auto p : owned) children.push_back(ILFindLastID(p));
        folder->GetUIObjectOf(hwnd_, static_cast<UINT>(children.size()), children.data(), IID_IDataObject, nullptr,
                              reinterpret_cast<void**>(data.GetAddressOf()));
    }
    if (data) {
        draggingOut_ = true;                  // a drop back on our own window is not "add to archive"
        DWORD effect = DROPEFFECT_NONE;
        SHDoDragDrop(hwnd_, data.Get(), nullptr, DROPEFFECT_COPY, &effect);
        draggingOut_ = false;
        statusMessage_ = effect == DROPEFFECT_NONE ? L"Drag cancelled." : L"✔ Copied by drag and drop.";
    } else {
        statusMessage_ = L"Nothing to drag.";
    }
    for (auto p : owned) CoTaskMemFree(p);
    if (folderPidl) CoTaskMemFree(folderPidl);
    UpdateStatus();
}

// ============================================================ app menu: Restart + app updates

struct MainWindow::UpdateMsg {
    enum Kind { Checked, Progress, Downloaded } kind = Checked;
    bool manual = false;
    std::optional<update::Manifest> manifest;
    std::optional<ErrorInfo> error;
    int percent = 0;
    std::wstring file;
};

// Only a copy made by the setup can update itself (unins000.exe next to the exe). Portable copies and the
// development build only get "a new version is out" + the download page.
bool MainWindow::IsInstalledCopy() const {
    return !fs::IsPortable() && fs::Exists(fs::ExeFolder() + L"\\unins000.exe");
}

std::wstring MainWindow::UpdateFolder() const {
    return fs::AppDataFolder() + L"\\update";
}

void MainWindow::SetMenuButtonText() {
    if (!menuBtn_) return;
    std::wstring t = L"Menu";
    if (updDownloading_) t = L"Update " + std::to_wstring(std::max(0, updPercent_)) + L"%";
    else if (!updReadyFile_.empty()) t = L"Restart to update";
    else if (updManifest_) t = L"Update";
    wchar_t cur[64]{};
    GetWindowTextW(menuBtn_, cur, 64);
    if (t == cur) return;
    SetWindowTextW(menuBtn_, t.c_str());
    Layout();
}

void MainWindow::ShowAppMenu() {
    const uint32_t mode = settings::GetNumber(L"AppUpdate", 0);      // 0 Tell me, 1 Install automatically, 2 Off
    HMENU m = CreatePopupMenu(), sub = CreatePopupMenu();
    AppendMenuW(sub, MF_STRING | (mode == 0 ? MF_CHECKED : 0), ID_APP_UPD_NOTIFY, L"Tell me when there is a new version");
    AppendMenuW(sub, MF_STRING | (mode == 1 ? MF_CHECKED : 0), ID_APP_UPD_AUTO, L"Install automatically (when Mechano Reader closes)");
    AppendMenuW(sub, MF_STRING | (mode == 2 ? MF_CHECKED : 0), ID_APP_UPD_OFF, L"Off (never go online)");
    CheckMenuRadioItem(sub, ID_APP_UPD_NOTIFY, ID_APP_UPD_OFF, ID_APP_UPD_NOTIFY + std::min<uint32_t>(mode, 2), MF_BYCOMMAND);
    std::wstring restart = updReadyFile_.empty() ? L"Restart" : L"Restart and install " + updManifest_->version;
    AppendMenuW(m, MF_STRING, ID_APP_RESTART, restart.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring check = L"Check for updates";
    if (updChecking_) check = L"Checking for updates...";
    else if (updDownloading_) check = L"Downloading update (" + std::to_wstring(std::max(0, updPercent_)) + L"%)...";
    else if (updManifest_ && updReadyFile_.empty()) check = L"Update to " + updManifest_->version + L"...";
    AppendMenuW(m, MF_STRING | (updChecking_ || updDownloading_ ? MF_GRAYED : 0), ID_APP_CHECK, check.c_str());
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"App updates");
    AppendMenuW(m, MF_STRING, ID_APP_PAGE, L"Open the download page");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const std::wstring about = L"Keyboard shortcuts and version (" MZR_VERSION_FULL_WSTR L")";
    AppendMenuW(m, MF_STRING, ID_APP_SHORTCUTS, about.c_str());
    RECT r;
    GetWindowRect(menuBtn_, &r);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, hwnd_, nullptr);
    DestroyMenu(m);                                          // also destroys the submenu
    switch (cmd) {
    case ID_APP_RESTART: RestartApp(); break;
    case ID_APP_CHECK:
        if (updManifest_ && updReadyFile_.empty()) AskToUpdate(true);
        else CheckForUpdates(true);
        break;
    case ID_APP_UPD_NOTIFY: case ID_APP_UPD_AUTO: case ID_APP_UPD_OFF: {
        const uint32_t v = cmd - ID_APP_UPD_NOTIFY;
        settings::SetNumber(L"AppUpdate", v);
        KillTimer(hwnd_, kTimerUpdate);
        if (v != 2) SetTimer(hwnd_, kTimerUpdate, 5000, nullptr);       // check soon with the new choice
        statusMessage_ = v == 2 ? L"App updates: off. Mechano Reader will not go online."
                       : v == 1 ? L"App updates: installed automatically when you close Mechano Reader."
                                : L"App updates: you will be told when a new version is out.";
        UpdateStatus();
        break;
    }
    case ID_APP_PAGE: ShellExecuteW(hwnd_, L"open", update::kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_APP_SHORTCUTS: ShowShortcuts(); break;
    default: break;
    }
}

// Restart: the same archive opens again. With a verified update waiting: the setup installs it quietly
// and starts Mechano Reader again (installer\MechanoZip.iss: /RELAUNCH=1 /OPEN="<archive>").
void MainWindow::RestartApp() {
    if (busy_) {
        MessageBeep(MB_ICONWARNING);
        statusMessage_ = L"A task is running: wait for it to finish (or press Esc), then Restart.";
        UpdateStatus();
        return;
    }
    const std::wstring archive = index_ ? index_->info().path : L"";
    if (!updReadyFile_.empty() && IsInstalledCopy()) {
        std::wstring args = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1";
        if (!archive.empty()) args += L" \"/OPEN=" + archive + L"\"";
        const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", updReadyFile_.c_str(), args.c_str(), nullptr, SW_HIDE));
        if (rc <= 32) {
            ShowError(hwnd_, L"Update failed", ErrorInfo{ErrorCode::Io, L"The update installer could not be started.", updReadyFile_});
            return;
        }
        updLaunched_ = true;
        log::Info(L"Update: installing " + updReadyFile_ + L" and restarting");
    } else {
        wchar_t exe[MAX_PATH * 4]{};
        GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        const std::wstring args = archive.empty() ? L"" : L"\"" + archive + L"\"";
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe, args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32) return;
    }
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWindow::CheckForUpdates(bool manual) {
    if (updChecking_ || updDownloading_) return;
    updChecking_ = true;
    if (manual) { statusMessage_ = L"Checking for updates..."; UpdateStatus(); }
    HWND h = hwnd_;
    std::thread([h, manual] {
        auto* m = new UpdateMsg;
        m->kind = UpdateMsg::Checked;
        m->manual = manual;
        auto text = update::HttpGet(update::FeedUrl(), 64 * 1024);
        if (!text) m->error = text.error();
        else {
            auto man = update::ParseManifest(text.value());
            if (man) m->manifest = std::move(man.value());
            else m->error = man.error();
        }
        if (!PostMessageW(h, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(m))) delete m;
    }).detach();
}

void MainWindow::AskToUpdate(bool manual) {
    if (!updManifest_) return;
    const auto& mf = *updManifest_;
    const bool installed = IsInstalledCopy();
    std::wstring head = L"Mechano Reader " + mf.version + L" is available";
    std::wstring body = L"You have " MZR_VERSION_WSTR L".";
    if (!mf.notes.empty()) body += L"\n\nWhat is new:\n" + mf.notes;
    wchar_t mb[32];
    swprintf_s(mb, L"%.1f MB", mf.setupSize / 1048576.0);
    const std::wstring b1 = installed ? L"Update now\nDownloads " + std::wstring(mb) + L", installs it and restarts Mechano Reader"
                                      : L"Open the download page\nThis copy is portable (or not installed): download the new version there";
    TASKDIALOG_BUTTON buttons[] = {{1, b1.c_str()}, {2, L"Later"}};
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = hwnd_;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Mechano Reader";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = head.c_str();
    tc.pszContent = body.c_str();
    tc.cButtons = 2;
    tc.pButtons = buttons;
    tc.pszFooter = L"The update is signed: Mechano Reader checks it before installing.";
    tc.pszFooterIcon = TD_SHIELD_ICON;
    int choice = 0;
    TaskDialogIndirect(&tc, &choice, nullptr, nullptr);
    (void)manual;
    if (choice != 1) return;
    if (!installed) { ShellExecuteW(hwnd_, L"open", update::kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL); return; }
    StartUpdateDownload(true);
}

void MainWindow::StartUpdateDownload(bool thenRestart) {
    if (!updManifest_ || updDownloading_) { updRestartWhenReady_ |= thenRestart; return; }
    if (!updReadyFile_.empty()) { if (thenRestart) RestartApp(); return; }
    updDownloading_ = true;
    updRestartWhenReady_ = thenRestart;
    updPercent_ = 0;
    SetMenuButtonText();
    const std::wstring dir = UpdateFolder();
    CreateDirectoryW(dir.c_str(), nullptr);
    const update::Manifest mf = *updManifest_;
    const std::wstring file = dir + L"\\Mechano Reader Setup " + mf.version + L".exe";
    HWND h = hwnd_;
    std::thread([h, mf, file] {
        int last = -1;
        auto res = update::DownloadVerified(mf.setupUrl, file, mf.setupSize, mf.setupSha256, [h, &last](double p) {
            const int pc = static_cast<int>(p * 100);
            if (pc == last) return;
            last = pc;
            auto* m = new UpdateMsg;
            m->kind = UpdateMsg::Progress;
            m->percent = pc;
            if (!PostMessageW(h, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(m))) delete m;
        }, nullptr);
        auto* m = new UpdateMsg;
        m->kind = UpdateMsg::Downloaded;
        if (res) m->file = file;
        else m->error = res.error();
        if (!PostMessageW(h, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(m))) delete m;
    }).detach();
}

void MainWindow::OnUpdateMessage(LPARAM lp) {
    std::unique_ptr<UpdateMsg> m(reinterpret_cast<UpdateMsg*>(lp));
    if (!m || closing_) return;
    switch (m->kind) {
    case UpdateMsg::Checked: {
        updChecking_ = false;
        if (m->error) {
            log::Warn(L"Update check: " + m->error->friendly + L" | " + m->error->details);
            if (m->manual) { statusMessage_.clear(); UpdateStatus(); ShowError(hwnd_, L"Check for updates", *m->error); }
            return;
        }
        if (!update::IsNewer(m->manifest->version, MZR_VERSION_WSTR)) {
            updManifest_.reset();
            SetMenuButtonText();
            if (m->manual) {
                statusMessage_ = L"✔ Mechano Reader is up to date (" MZR_VERSION_WSTR L").";
                UpdateStatus();
                TaskDialog(hwnd_, nullptr, L"Mechano Reader", L"Mechano Reader is up to date.",
                           L"You have the newest version (" MZR_VERSION_FULL_WSTR L").", TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
            }
            return;
        }
        updManifest_ = std::make_shared<update::Manifest>(std::move(*m->manifest));
        SetMenuButtonText();
        log::Info(L"Update: " + updManifest_->version + L" is available");
        const uint32_t mode = settings::GetNumber(L"AppUpdate", 0);
        if (!m->manual && mode == 1 && IsInstalledCopy()) { StartUpdateDownload(false); return; }   // quietly
        // Tell me: ask once per version on the automatic check (the Update button stays); always when asked.
        uint32_t code = 0;
        {
            unsigned a = 0, b = 0, c = 0;
            swscanf_s(updManifest_->version.c_str(), L"%u.%u.%u", &a, &b, &c);
            code = a * 1000000 + b * 1000 + c;
        }
        if (m->manual || settings::GetNumber(L"UpdateAsked", 0) != code) {
            settings::SetNumber(L"UpdateAsked", code);
            if (!busy_ || m->manual) AskToUpdate(m->manual);
        } else {
            statusMessage_ = L"Mechano Reader " + updManifest_->version + L" is available: Menu > Update.";
            UpdateStatus();
        }
        return;
    }
    case UpdateMsg::Progress:
        updPercent_ = m->percent;
        SetMenuButtonText();
        return;
    case UpdateMsg::Downloaded:
        updDownloading_ = false;
        updPercent_ = -1;
        if (m->error) {
            SetMenuButtonText();
            log::Warn(L"Update download: " + m->error->friendly + L" | " + m->error->details);
            if (updRestartWhenReady_) ShowError(hwnd_, L"Update failed", *m->error);
            updRestartWhenReady_ = false;
            return;
        }
        updReadyFile_ = m->file;
        SetMenuButtonText();
        log::Info(L"Update: verified " + updReadyFile_);
        if (updRestartWhenReady_) { updRestartWhenReady_ = false; RestartApp(); return; }
        statusMessage_ = L"✔ Update " + updManifest_->version +
                         L" is ready: it installs when you close Mechano Reader (or Menu > Restart now).";
        UpdateStatus();
        return;
    }
}

void MainWindow::ShowShortcuts() {
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = hwnd_;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = L"Keyboard shortcuts";
    tc.pszMainInstruction = L"Keyboard shortcuts";
    tc.pszContent =
        L"Ctrl+O   —   Open archive\n"
        L"Ctrl+N   —   New archive\n"
        L"Alt+A   —   Add files\n"
        L"Ctrl+E   —   Extract to...\n"
        L"Alt+E   —   Extract here\n"
        L"Alt+T   —   Test archive\n"
        L"Del   —   Delete from archive\n"
        L"F2   —   Rename (file or folder)\n"
        L"Enter, double-click   —   Open folder / open the file in its main program\n"
        L"Backspace, Alt+Up   —   Up one folder\n"
        L"Space   —   Quick Look (Space / Esc closes it)\n"
        L"Shift+F10, Menu key   —   Right-click menu\n"
        L"Ctrl+F, F3   —   Search in archive\n"
        L"Ctrl+A   —   Select all\n"
        L"F5   —   Reload archive\n"
        L"Ctrl + / Ctrl - / Ctrl 0   —   Zoom in / out / fit\n"
        L"Esc   —   Cancel the running job / clear search\n"
        L"\nIn Quick Look:\n"
        L"Up / Down   —   Previous / next file\n"
        L"Left / Right   —   Page (PDF, slides) or -5 s / +5 s (video, audio)\n"
        L"\nMouse: drag files from the list to Explorer to extract them.";
    // About line: version, author, engine, mode.
    const std::wstring footer = std::wstring(L"Mechano Reader ") + MZR_VERSION_FULL_WSTR +
                                L"  ·  Eng. Eslam Abdelqader  ·  MIT License  ·  " + archive::EngineVersion() +
                                (fs::IsPortable() ? L"  ·  Portable mode" : L"");
    tc.pszFooter = footer.c_str();
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
}

void MainWindow::BeginRename() {
    if (!index_ || busy_) return;
    if (!index_->info().canUpdate) {
        ShowError(hwnd_, L"Cannot rename", ErrorInfo{ErrorCode::Internal, L"This archive format is read-only. Only ZIP, 7z, TAR... can be changed.", L""});
        return;
    }
    const auto sel = SelectedNodes();
    if (sel.size() != 1) { MessageBeep(MB_ICONWARNING); return; }
    if (qlOpen_) CloseQuickLook();
    SetFocus(list_);
    ListView_EditLabel(list_, ListView_GetNextItem(list_, -1, LVNI_FOCUSED | LVNI_SELECTED));
}

void MainWindow::ApplyRename(uint32_t node, std::wstring newName) {
    if (!index_ || busy_ || !node) return;
    while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'\t')) newName.pop_back();   // pasted spaces
    const auto& n = index_->node(node);
    if (newName == n.name) return;
    if (const std::wstring why = security::CheckNewEntryName(newName); !why.empty()) {
        ShowError(hwnd_, L"Cannot rename", ErrorInfo{ErrorCode::Internal, why, L"New name: \"" + newName + L"\""});
        return;
    }
    // Another item with that name in the same folder? (case-insensitive, like Windows)
    const auto& nodes = index_->nodes();
    for (uint32_t i = 1; i < nodes.size(); ++i)
        if (i != node && nodes[i].parent == n.parent &&
            CompareStringOrdinal(nodes[i].name.c_str(), -1, newName.c_str(), -1, TRUE) == CSTR_EQUAL &&
            CompareStringOrdinal(n.name.c_str(), -1, newName.c_str(), -1, TRUE) != CSTR_EQUAL) {   // case-only change is fine
            ShowError(hwnd_, L"Cannot rename", ErrorInfo{ErrorCode::Internal,
                      L"\"" + newName + L"\" already exists in this folder.", L""});
            return;
        }

    // Every entry at or below the node gets the new name at the node's depth.
    // (Works on the raw stored paths, '/' or '\', so nothing else in them changes.)
    size_t depth = 0;
    for (uint32_t p = node; p != ArchiveIndex::kRoot && p != 0; p = nodes[p].parent) ++depth;
    const auto indices = index_->CollectArcIndices({node});
    std::unordered_map<uint32_t, bool> wanted;
    for (uint32_t a : indices) wanted[a] = true;
    archive::UpdatePlan plan;
    for (const auto& e : index_->entries()) {
        if (!wanted.count(e.arcIndex)) continue;
        std::vector<std::wstring> parts;
        std::wstring cur;
        for (wchar_t c : e.path) {
            if (c == L'/' || c == L'\\') { if (!cur.empty() && cur != L".") parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty() && cur != L".") parts.push_back(cur);
        if (parts.size() < depth) continue;                 // should not happen: leave it alone
        parts[depth - 1] = newName;
        std::wstring np;
        for (const auto& s : parts) { if (!np.empty()) np += L'\\'; np += s; }
        plan.rename.emplace_back(e.arcIndex, np);
    }
    if (plan.rename.empty()) return;

    if (!BeginJob(L"Renaming \"" + n.name + L"\"...")) return;
    selectAfterReload_ = newName;
    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    archive::CompressionSettings cs;
    cs.format = index_->info().formatName;
    const std::wstring oldName = n.name;
    worker_.Post([this, plan, cs, cancel, prog, h, oldName, newName] {
        auto* r = new JobResult;
        r->title = L"Rename";
        auto res = archive_->Update(plan, cs, prog.get(), *cancel, &r->report);
        if (!res) r->error = res.error();
        r->indexChanged = true;
        r->quiet = true;
        r->doneText = L"Renamed \"" + oldName + L"\" to \"" + newName + L"\"";
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::CmdTest() {
    if (!index_) return;
    if (!BeginJob(L"Testing " + FileNameOf(index_->info().path) + L"...")) return;
    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    worker_.Post([this, cancel, prog, h] {
        auto* r = new JobResult;
        r->title = L"Test archive";
        r->report = archive_->Test(prog.get(), *cancel);
        r->doneText = r->report.issues.empty() && !r->report.cancelled
                          ? L"No errors found. All " + fmt::Number(r->report.filesDone) + L" files are OK."
                          : L"Tested " + fmt::Number(r->report.filesDone) + L" file(s).";
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::CmdDelete() {
    if (!index_) return;
    const auto sel = SelectedNodes();
    if (sel.empty()) return;
    if (!index_->info().canUpdate) {
        ShowError(hwnd_, L"Cannot delete", ErrorInfo{ErrorCode::Internal, L"This archive format is read-only.", L""});
        return;
    }
    const auto indices = index_->CollectArcIndices(sel);
    if (!Confirm(hwnd_, L"Delete from archive?",
                 L"Delete " + fmt::Number(sel.size()) + L" selected item(s) (" + fmt::Number(indices.size()) +
                 L" entries) from the archive? This cannot be undone.")) return;
    if (!BeginJob(L"Deleting...")) return;
    auto cancel = cancel_;
    auto prog = progress_;
    HWND h = hwnd_;
    archive::CompressionSettings cs;
    cs.format = index_->info().formatName;
    worker_.Post([this, indices, cs, cancel, prog, h] {
        auto* r = new JobResult;
        r->title = L"Delete";
        archive::UpdatePlan plan;
        plan.remove = indices;
        auto res = archive_->Update(plan, cs, prog.get(), *cancel, &r->report);
        if (!res) r->error = res.error();
        r->indexChanged = true;
        r->quiet = true;
        r->doneText = L"Deleted " + fmt::Number(indices.size()) + L" entries.";
        if (!PostMessageW(h, WM_APP_JOBDONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::CmdEncodingMenu() {
    if (!index_) return;
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kCodePages); ++i)
        AppendMenuW(m, MF_STRING | (kCodePages[i].cp == codePage_ ? MF_CHECKED : 0), ID_ENC_BASE + i, kCodePages[i].label);
    RECT r;
    GetWindowRect(toolButtons_[7], &r);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_ENC_BASE && cmd < ID_ENC_BASE + std::size(kCodePages)) {
        const uint32_t cp = kCodePages[cmd - ID_ENC_BASE].cp;
        if (cp != codePage_) OpenArchive(index_->info().path, cp);
    }
}

void MainWindow::OnDropFiles(HDROP drop) {
    std::vector<std::wstring> files;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        files.push_back(std::move(p));
    }
    DragFinish(drop);
    if (files.empty()) return;
    // Our own drag-out dropped back on our window: those are temp copies, not new files to add.
    const std::wstring tmp = fs::TempFolder() + L"\\";
    if (draggingOut_ || CompareStringOrdinal(files[0].c_str(), static_cast<int>(std::min(files[0].size(), tmp.size())),
                                             tmp.c_str(), static_cast<int>(tmp.size()), TRUE) == CSTR_EQUAL) return;

    auto isArchive = [](const std::wstring& f) {
        if (fs::IsDirectory(f)) return false;
        const std::wstring e = Ext(f);
        static const wchar_t* arcExt[] = {L".zip", L".7z", L".rar", L".tar", L".gz", L".tgz", L".bz2", L".xz",
                                          L".zst", L".iso", L".cab", L".wim", L".001", L".jar", L".apk", L".arj", L".lzh"};
        for (auto* a : arcExt) if (e == a) return true;
        return false;
    };
    // Several archives dropped (and nothing else) -> batch: extract them all, or one new archive.
    if (files.size() > 1 && std::all_of(files.begin(), files.end(), isArchive)) { AskBatch(std::move(files)); return; }
    if (!index_) {
        // One archive dropped -> open it. Anything else -> create a new archive from it.
        if (files.size() == 1 && isArchive(files[0])) { OpenArchive(files[0]); return; }
        CmdNew(files);
        return;
    }
    const std::wstring where = searchMode_ || folder_ == ArchiveIndex::kRoot ? FileNameOf(index_->info().path)
                                                                          : index_->PathOf(folder_);
    if (Confirm(hwnd_, L"Add to archive?", L"Add " + fmt::Number(files.size()) + L" dropped item(s) to \"" + where + L"\"?"))
        CmdAdd(files);
}

void MainWindow::OnCommand(int id) {
    // After a preview button: keyboard back to the list (or to Quick Look while it is open).
    const HWND back = qlOpen_ ? ql_ : list_;
    switch (id) {
    case ID_OPEN: CmdOpen(); break;
    case ID_NEW: CmdNew(); break;
    case ID_ADD: CmdAdd(); break;
    case ID_EXTRACT_TO: CmdExtract(false); break;
    case ID_EXTRACT_HERE: CmdExtract(true); break;
    case ID_TEST: CmdTest(); break;
    case ID_DELETE: CmdDelete(); break;
    case ID_ENCODING: CmdEncodingMenu(); break;
    case ID_UP: GoUp(); break;
    case ID_MENU: ShowAppMenu(); break;
    case ID_PV_OPEN: CmdExtractAndOpen(); break;
    case ID_PV_EXTRACT: CmdExtract(false); break;
    case ID_PV_A:
        if (pvState_ == PvState::Media) media_->TogglePlay();
        else if (docPage_ > 0) { viewCy_ = 0; RequestDocPage(docPage_ - 1); }   // zoomed: new page starts at its top
        SetFocus(back);
        break;
    case ID_PV_B:
        if (pvState_ == PvState::Media) media_->ToggleMute();
        else if (docPage_ + 1 < docPages_) { viewCy_ = 0; RequestDocPage(docPage_ + 1); }
        SetFocus(back);
        break;
    case ID_PV_ZOOMIN:
    case ID_ACC_ZOOMIN: SetZoom(zoom_ * kZoomStep); if (id == ID_PV_ZOOMIN) SetFocus(back); break;
    case ID_PV_ZOOMOUT:
    case ID_ACC_ZOOMOUT: SetZoom(zoom_ / kZoomStep); if (id == ID_PV_ZOOMOUT) SetFocus(back); break;
    case ID_PV_ZOOMFIT:
    case ID_ACC_ZOOMFIT: SetZoom(1.0); if (id == ID_PV_ZOOMFIT) SetFocus(back); break;
    case ID_PV_BACK:
    case ID_PV_FWD:
        if (pvState_ == PvState::Media && media_->IsOpen()) {
            media_->Seek(media_->Position() + (id == ID_PV_FWD ? 10.0 : -10.0));
            UpdateSeekBar();
        }
        SetFocus(back);
        break;
    case ID_ACC_SELECTALL:
        if (GetFocus() == search_) SendMessageW(search_, EM_SETSEL, 0, -1);
        else ListView_SetItemState(list_, -1, LVIS_SELECTED, LVIS_SELECTED);
        break;
    case ID_ACC_CANCEL:
        if (qlOpen_) CloseQuickLook();
        else if (busy_ && cancel_) { cancel_->Cancel(); statusMessage_ = L"Cancelling..."; UpdateStatus(); }
        else if (GetFocus() == search_ && GetWindowTextLengthW(search_)) { SetWindowTextW(search_, L""); RebuildView(); }
        break;
    case ID_ACC_FIND: SetFocus(search_); SendMessageW(search_, EM_SETSEL, 0, -1); break;
    case ID_ACC_REFRESH: if (index_) OpenArchive(index_->info().path, codePage_); break;
    case ID_ACC_HELP: ShowShortcuts(); break;
    case ID_CTX_QUICKLOOK: OpenQuickLook(); break;
    case ID_CTX_OPEN: ActivateSelection(); break;
    case ID_CTX_RENAME: BeginRename(); break;
    case ID_CTX_COPYNAME:
    case ID_CTX_COPYPATH: {
        std::wstring t;
        for (uint32_t n : SelectedNodes()) {
            if (!t.empty()) t += L"\r\n";
            t += id == ID_CTX_COPYNAME ? index_->node(n).name : index_->PathOf(n);
        }
        if (!t.empty()) CopyToClipboard(t);
        break;
    }
    }
}

// ============================================================ window procedures

LRESULT CALLBACK MainWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->WndProc(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK MainWindow::PreviewProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->PreviewProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// The video picture window: MFPlay draws into it; a click toggles play / pause.
LRESULT CALLBACK MainWindow::VideoProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (self && self->media_ && self->media_->IsOpen()) self->media_->RepaintVideo();
        else FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        if (self && self->media_) self->media_->RepaintVideo();
        return 0;
    case WM_LBUTTONUP:
        if (self && self->media_) self->media_->TogglePlay();
        return 0;
    case WM_LBUTTONDBLCLK:
        // VLC: double-click the picture = full screen (Quick Look); the two clicks' pause / play cancel out
        if (self) self->ToggleQuickLook();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT MainWindow::PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        // Double-buffered to avoid flicker while resizing.
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintPreview(h, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        LayoutPreviewChildren();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CTLCOLORSTATIC:
        // Read-only text preview: normal window colours, not the grey "disabled" look.
        if (reinterpret_cast<HWND>(l) == pvText_) {
            SetTextColor(reinterpret_cast<HDC>(w), theme::kText);
            SetBkColor(reinterpret_cast<HDC>(w), theme::kWhite);
            return reinterpret_cast<LRESULT>(theme::Brush(theme::kWhite));
        }
        break;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(l);
        (void)hdr;
        break;
    }
    case WM_COMMAND: OnCommand(LOWORD(w)); return 0;
    case WM_DRAWITEM:
        if (theme::DrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(l))) return TRUE;
        break;

    // ---- seek bar
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(l) == pvSeek_) { OnSeekScroll(LOWORD(w)); return 0; }
        break;
    case WM_TIMER:
        if (w == kTimerMedia) {
            if (pvState_ == PvState::Media && media_->IsOpen()) {
                UpdateSeekBar();
                RECT rc;
                GetClientRect(h, &rc);
                RECT f{0, PreviewContentRect().bottom + SeekRowH(), rc.right, rc.bottom};
                InvalidateRect(h, &f, FALSE);                  // time "0:03 / 0:08"
                if (!IsVideo()) {                              // audio: the big time in the song panel
                    const RECT c = PreviewContentRect();
                    InvalidateRect(h, &c, FALSE);
                }
            } else {
                KillTimer(h, kTimerMedia);
            }
            return 0;
        }
        if (w == kTimerDocZoom) {
            KillTimer(h, kTimerDocZoom);
            // Zoomed PDF page / slide: render it again at the size it is shown (max 2560 px) so it stays sharp.
            if (PagedImage()) {
                double x0, y0, dw, dh;
                ImagePlacement(PreviewContentRect(), x0, y0, dw, dh);
                const uint32_t need = std::min<uint32_t>(preview::kDecodeSide, static_cast<uint32_t>(dw + 0.5));
                if (need > pvImage_.width * 105 / 100 && pvImage_.width < preview::kDecodeSide) RequestDocPage(docPage_, need);
            }
            return 0;
        }
        break;

    // ---- zoom + pan with the mouse
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(w);
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
            SetZoom(zoom_ * std::pow(kZoomStep, delta / static_cast<double>(WHEEL_DELTA)), &p);
            return 0;
        }
        if (ImagePannable()) {
            const RECT c = PreviewContentRect();
            double x0, y0, dw, dh;
            ImagePlacement(c, x0, y0, dw, dh);
            const double step = -delta / static_cast<double>(WHEEL_DELTA) * Dpi(80);
            if ((GET_KEYSTATE_WPARAM(w) & MK_SHIFT) || dh <= c.bottom - c.top) viewCx_ += step / dw;
            else viewCy_ += step / dh;
            InvalidateRect(h, &c, FALSE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        if (pvState_ == PvState::Media && !IsVideo()) {
            // song panel: a click on the big time switches elapsed <-> remaining (AIMP)
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&audioTimeRc_, p)) {
                audioRemaining_ = !audioRemaining_;
                const RECT c = PreviewContentRect();
                InvalidateRect(h, &c, FALSE);
            }
            return 0;
        }
        if (ImagePannable()) {
            const RECT c = PreviewContentRect();
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&c, p)) { panning_ = true; panLast_ = p; SetCapture(h); }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (panning_) {
            const RECT c = PreviewContentRect();
            double x0, y0, dw, dh;
            ImagePlacement(c, x0, y0, dw, dh);
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            viewCx_ -= (p.x - panLast_.x) / dw;
            viewCy_ -= (p.y - panLast_.y) / dh;
            panLast_ = p;
            InvalidateRect(h, &c, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (panning_) { panning_ = false; if (m == WM_LBUTTONUP) ReleaseCapture(); }
        return 0;
    case WM_LBUTTONDBLCLK:
        // fit <-> actual size (pictures) / 200% (pages, slides)
        if (pvState_ == PvState::Image && CanZoom()) {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            double target = 2.0;
            if (pvKind_ == preview::Kind::Image && pvImage_.originalWidth) {
                double x0, y0, dw, dh;
                ImagePlacement(PreviewContentRect(), x0, y0, dw, dh);
                const double actual = zoom_ * pvImage_.originalWidth / dw;      // zoom where 1 picture px = 1 screen px
                target = actual > 1.05 ? actual : 2.0;
            }
            SetZoom(std::abs(zoom_ - 1.0) > 1e-6 ? 1.0 : target, &p);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && reinterpret_cast<HWND>(w) == h && ImagePannable()) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            const RECT c = PreviewContentRect();
            if (PtInRect(&c, p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        dpi_ = static_cast<int>(GetDpiForWindow(hwnd_));
        CreateChildren();
        UpdateTitleAndPath();
        UpdateStatus();
        return 0;

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        const RECT* r = reinterpret_cast<RECT*>(lp);
        ApplyFonts();
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_SIZE: Layout(); return 0;

    // ---- theme: the toolbar band, the search frame, the splitter
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        theme::Fill(dc, rc, theme::kBack);
        RECT band{0, 0, rc.right, barH_};
        theme::Fill(dc, band, theme::kBar);
        RECT line{0, barH_ - 1, rc.right, barH_};
        theme::Fill(dc, line, theme::kLine);
        if (!IsRectEmpty(&searchFrame_)) {
            const bool focus = GetFocus() == search_;
            theme::RoundRectFill(dc, searchFrame_, Dpi(10), theme::kPaper, focus ? theme::kCoral : theme::kLine);
            RECT icon{searchFrame_.left + Dpi(8), searchFrame_.top, searchFrame_.left + Dpi(26), searchFrame_.bottom};
            SelectObject(dc, theme::IconFont(Dpi(13)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, theme::kIcon);
            DrawTextW(dc, L"\xE721", 1, &icon, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        // splitter: a thin sage line in the middle of the gap
        RECT lr;
        GetWindowRect(list_, &lr);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&lr), 2);
        RECT sp{split_ + Dpi(2), lr.top, split_ + Dpi(3), lr.bottom};
        theme::Fill(dc, sp, theme::kLine);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:                    // the path line under the toolbar
        if (reinterpret_cast<HWND>(lp) == pathLabel_) {
            SetTextColor(reinterpret_cast<HDC>(wp), theme::kTextDim);
            SetBkColor(reinterpret_cast<HDC>(wp), theme::kBack);
            return reinterpret_cast<LRESULT>(theme::Brush(theme::kBack));
        }
        break;
    case WM_CTLCOLOREDIT:                      // the search box
        if (reinterpret_cast<HWND>(lp) == search_) {
            SetTextColor(reinterpret_cast<HDC>(wp), theme::kText);
            SetBkColor(reinterpret_cast<HDC>(wp), theme::kPaper);
            return reinterpret_cast<LRESULT>(theme::Brush(theme::kPaper));
        }
        break;
    case WM_DRAWITEM:
        if (theme::DrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp))) return TRUE;
        break;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {Dpi(720), Dpi(420)};
        return 0;
    }

    // ---- splitter between list and preview
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd_, &p);
        if (LOWORD(lp) == HTCLIENT && p.x >= split_ && p.x < split_ + Dpi(5)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
        if (busy_ && LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING)); return TRUE; }
        break;
    }
    case WM_LBUTTONDOWN:
        if (GET_X_LPARAM(lp) >= split_ && GET_X_LPARAM(lp) < split_ + Dpi(5)) { dragging_ = true; SetCapture(hwnd_); }
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_) { split_ = GET_X_LPARAM(lp); Layout(); }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) { dragging_ = false; ReleaseCapture(); }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEARCH && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {   // frame colour
            RECT f = searchFrame_;
            InvalidateRect(hwnd_, &f, FALSE);
            return 0;
        }
        if (LOWORD(wp) == ID_SEARCH && HIWORD(wp) == EN_CHANGE) { SetTimer(hwnd_, kTimerSearch, 150, nullptr); return 0; }
        OnCommand(LOWORD(wp));
        return 0;

    case WM_TIMER:
        KillTimer(hwnd_, wp);
        if (wp == kTimerUpdate) {
            if (settings::GetNumber(L"AppUpdate", 0) != 2) CheckForUpdates(false);
            SetTimer(hwnd_, kTimerUpdate, 12u * 60 * 60 * 1000, nullptr);
        } else if (wp == kTimerSearch) {
            const bool wasSearch = searchMode_;
            RebuildView();
            if (wasSearch && !searchMode_) NavigateTo(folder_);
        } else if (wp == kTimerPreview) {
            UpdatePreview();
            UpdateStatus();
        }
        return 0;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->hwndFrom != list_) break;
        switch (nm->code) {
        case NM_CUSTOMDRAW: return theme::ListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lp));
        case LVN_GETDISPINFOW: OnGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(lp)); return 0;
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED: SetTimer(hwnd_, kTimerPreview, 40, nullptr); return 0;
        case LVN_BEGINDRAG: CmdDragOut(); return 0;
        case LVN_BEGINLABELEDITW: {
            // In-place rename. TRUE = not allowed now.
            const int i = reinterpret_cast<NMLVDISPINFOW*>(lp)->item.iItem;
            if (!index_ || busy_ || qlOpen_ || !index_->info().canUpdate || i < 0 || static_cast<size_t>(i) >= view_.size())
                return TRUE;
            const auto& n = index_->node(view_[static_cast<size_t>(i)]);
            if (HWND ed = ListView_GetEditControl(list_)) {
                SetWindowTextW(ed, n.name.c_str());        // search results show the full path: edit the name only
                SendMessageW(ed, EM_SETLIMITTEXT, 255, 0);
                // Like Explorer: the name is selected without its extension.
                const auto dot = n.name.find_last_of(L'.');
                SendMessageW(ed, EM_SETSEL, 0, (!n.isDir && dot != std::wstring::npos && dot > 0) ? dot : -1);
            }
            return FALSE;
        }
        case LVN_ENDLABELEDITW: {
            const auto* di = reinterpret_cast<NMLVDISPINFOW*>(lp);
            if (di->item.pszText && index_ && di->item.iItem >= 0 && static_cast<size_t>(di->item.iItem) < view_.size())
                ApplyRename(view_[static_cast<size_t>(di->item.iItem)], di->item.pszText);
            return FALSE;                                    // the list is rebuilt when the archive is saved
        }
        case NM_DBLCLK:
        case NM_RETURN: ActivateSelection(); return 0;
        case LVN_KEYDOWN: {
            const auto* kd = reinterpret_cast<NMLVKEYDOWN*>(lp);
            if (kd->wVKey == VK_BACK) GoUp();
            else if (kd->wVKey == VK_DELETE) CmdDelete();
            else if (kd->wVKey == VK_SPACE) ToggleQuickLook();
            else if (kd->wVKey == VK_F2) BeginRename();
            return 0;
        }
        case LVN_COLUMNDROPDOWN:
            if (reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem == ColDetail) ShowDetailMenu();
            return 0;
        case LVN_COLUMNCLICK: {
            const int col = reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;
            sortAsc_ = (col == sortCol_) ? !sortAsc_ : true;
            sortCol_ = col;
            theme::SetHeaderInfo(sortCol_, sortAsc_, ColDetail);
            InvalidateRect(ListView_GetHeader(list_), nullptr, FALSE);
            SortView();
            InvalidateRect(list_, nullptr, FALSE);
            return 0;
        }
        case LVN_ODFINDITEMW: {
            // Type-to-find in the virtual list.
            auto* fi = reinterpret_cast<NMLVFINDITEMW*>(lp);
            if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !index_) return -1;
            const int len = lstrlenW(fi->lvfi.psz);
            const int count = static_cast<int>(view_.size());
            for (int k = 0; k < count; ++k) {
                const int i = (fi->iStart + k) % std::max(1, count);
                const auto& nm2 = index_->node(view_[static_cast<size_t>(i)]).name;
                if (static_cast<int>(nm2.size()) >= len &&
                    CompareStringOrdinal(nm2.c_str(), len, fi->lvfi.psz, len, TRUE) == CSTR_EQUAL) return i;
            }
            return -1;
        }
        }
        break;
    }

    case WM_DROPFILES: OnDropFiles(reinterpret_cast<HDROP>(wp)); return 0;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp) == list_) { ShowContextMenu(lp); return 0; }
        break;

    // ---- worker results
    case WM_APP_PROGRESS: {
        if (!busy_ || !progress_) return 0;
        const uint64_t t = progress_->total, d = progress_->done;
        std::wstring s = t ? std::to_wstring(std::min<uint64_t>(100, d * 100 / t)) + L"%  " : L"";
        std::wstring item = progress_->Item();
        if (item.size() > 60) item = L"..." + item.substr(item.size() - 57);
        statusMessage_ = s + item + L"   (Esc to cancel)";
        SendMessageW(status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(statusMessage_.c_str()));
        return 0;
    }
    case WM_APP_ASKPW: {
        const auto* name = reinterpret_cast<const std::wstring*>(wp);
        auto* out = reinterpret_cast<std::optional<std::wstring>*>(lp);
        *out = closing_ ? std::nullopt : AskPassword(hwnd_, *name);
        return 0;
    }
    case WM_APP_LATEINIT:
        CreateLateChildren();
        {
            // Old update installers (already installed, or never used) are removed; the first automatic
            // check comes 2 minutes after start (not while the user starts working), then every 12 hours.
            const std::wstring dir = UpdateFolder();
            WIN32_FIND_DATAW fd{};
            HANDLE fh = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); }
                while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            if (settings::GetNumber(L"AppUpdate", 0) != 2) SetTimer(hwnd_, kTimerUpdate, 2 * 60 * 1000, nullptr);
        }
        return 0;
    case WM_APP_ICONS:
        if (lp) { ListView_SetImageList(list_, reinterpret_cast<HIMAGELIST>(lp), LVSIL_SMALL); InvalidateRect(list_, nullptr, FALSE); }
        return 0;
    case WM_APP_PREVIEW:
        OnPreviewResult(reinterpret_cast<PreviewResult*>(lp));
        return 0;
    case WM_APP_MEDIA:
        if (wp == MediaBar::PreviousFile || wp == MediaBar::NextFile) { StepMediaFile(wp == MediaBar::NextFile ? 1 : -1); return 0; }
        if (wp == MediaBar::FullScreen) { ToggleQuickLook(); return 0; }
        if (pvState_ == PvState::Media && media_->HasError()) {
            pvState_ = PvState::Failed;
            pvMessage_ = media_->Error();
            media_->Close();
            ShowWindow(pvVideo_, SW_HIDE);
        }
        UpdatePreviewButtons();
        InvalidateRect(preview_, nullptr, FALSE);
        return 0;
    case WM_APP_OPENED: {
        std::unique_ptr<OpenResult> r(reinterpret_cast<OpenResult*>(lp));
        EndJob();
        if (closing_) return 0;
        if (r->result) { CloseQuickLook(); ResetPreview(); }     // node numbers belong to the old archive
        if (!r->result && batch_.active) {
            // Batch: note it, go on with the next archive (Cancel / Esc stops the rest).
            shellAction_ = ShellAction::None;
            log::Warn(L"Batch open failed: " + r->result.error().details);
            if (r->result.error().code == ErrorCode::Cancelled) batch_.stopped = true;
            else batch_.problems.push_back({FileNameOf(batch_.items[batch_.next - 1]), r->result.error().friendly});
            NextBatchItem();
            return 0;
        }
        if (!r->result) {
            if (r->result.error().code != ErrorCode::Cancelled)
                ShowError(hwnd_, L"Cannot open the archive", r->result.error());
            statusMessage_.clear();
            log::Warn(L"Open failed: " + r->result.error().details);
            UpdateStatus();
            if (shellClose_) { shellAction_ = ShellAction::None; PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
            return 0;
        }
        archive_ = std::move(r->result.value());
        index_ = archive_->Index();
        codePage_ = r->codePage;
        folder_ = ArchiveIndex::kRoot;
        SetWindowTextW(search_, L"");
        KillTimer(hwnd_, kTimerSearch);
        searchMode_ = false;
        wchar_t ms[32];
        swprintf_s(ms, L"%.0f", r->ms);
        statusMessage_ = L"Opened in " + std::wstring(ms) + L" ms";
        if (!index_->info().warnings.empty()) statusMessage_ = L"⚠ " + index_->info().warnings[0];
        RebuildView();
        SetFocus(list_);
        if (shellAction_ != ShellAction::None) {
            const bool toFolder = shellAction_ == ShellAction::ExtractToFolder;
            shellAction_ = ShellAction::None;
            ShellExtract(toFolder, batch_.active ? batch_.destBase : std::wstring());
        }
        return 0;
    }
    case WM_APP_BATCHNEXT:
        if (batch_.active && !busy_) NextBatchItem();
        return 0;
    case WM_APP_JOBDONE: {
        std::unique_ptr<JobResult> r(reinterpret_cast<JobResult*>(lp));
        EndJob();
        if (closing_) return 0;
        if (batch_.active) {
            // Batch: no dialog per item; everything goes into the summary at the end.
            const std::wstring name = FileNameOf(batch_.items[batch_.next - 1]);
            if (r->report.cancelled || (r->error && r->error->code == ErrorCode::Cancelled)) batch_.stopped = true;
            else if (r->error) batch_.problems.push_back({name, r->error->friendly});
            else if (r->report.wrongPassword) batch_.problems.push_back({name, L"Wrong password."});
            else if (!r->report.issues.empty())
                batch_.problems.push_back({name, fmt::Number(r->report.issues.size()) + L" file(s) not extracted, e.g. " +
                                                     r->report.issues[0].path + L": " + r->report.issues[0].friendly});
            else ++batch_.ok;
            batch_.files += r->report.filesDone;
            batch_.bytes += r->report.bytesDone;
            if (r->error) log::Error(L"Batch " + name + L": " + r->error->friendly + L" | " + r->error->details);
            for (const auto& i : r->report.issues) log::Warn(L"Batch " + name + L": " + i.path + L" - " + i.friendly);
            NextBatchItem();
            return 0;
        }
        if (r->indexChanged && archive_) {
            // Keep the user in the same folder after add / delete.
            const std::wstring here = index_ ? index_->PathOf(folder_) : L"";
            ResetPreview();
            index_ = archive_->Index();
            const uint32_t f = index_->FindFolder(here);
            folder_ = f == ArchiveIndex::kNoEntry ? ArchiveIndex::kRoot : f;
            RebuildView();
            // After a rename: select the renamed item again.
            for (size_t k = 0; !selectAfterReload_.empty() && k < view_.size(); ++k)
                if (index_->node(view_[k]).name == selectAfterReload_) {
                    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
                    ListView_SetItemState(list_, static_cast<int>(k), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(list_, static_cast<int>(k), FALSE);
                    break;
                }
        }
        selectAfterReload_.clear();
        statusMessage_ = r->report.cancelled ? L"Cancelled." : L"";
        if (r->error) {
            if (r->error->code != ErrorCode::Cancelled) ShowError(hwnd_, r->title + L" failed", *r->error);
            log::Error(r->title + L": " + r->error->friendly + L" | " + r->error->details);
        } else if (!r->report.issues.empty() || r->report.wrongPassword || (!r->quiet && !r->report.cancelled)) {
            ShowReport(hwnd_, r->title, r->doneText, r->report);
            for (const auto& i : r->report.issues) log::Warn(r->title + L": " + i.path + L" - " + i.friendly);
        } else if (!r->report.cancelled) {
            statusMessage_ = L"✔ " + r->doneText;
            for (auto& c : statusMessage_) if (c == L'\n') c = L' ';
        }
        if (shellClose_) {
            // Explorer menu task: the window was only its progress view, and any problem was shown above.
            shellClose_ = false;
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return 0;
        }
        if (!r->dragDir.empty() && !r->error && !r->report.cancelled && r->report.filesDone + r->report.foldersDone > 0) {
            // Start the drag only if the mouse button is still down (the user is still dragging).
            if (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) < 0) DoDragOut(r->dragDir);
            else { statusMessage_ = L"Drag cancelled (the mouse was released before the files were ready)."; UpdateStatus(); }
            return 0;
        }
        if (!r->launchFile.empty() && !r->error && r->report.issues.empty()) {
            // S3: check the file that is really on disk (the sanitizer may have changed the name,
            // e.g. "invoice.exe." became "invoice.exe"). Ask if the first check did not.
            const std::wstring shown = r->launchFile.substr(r->launchFile.find_last_of(L'\\') + 1);
            if (r->riskConfirmed || !IsRiskyExtension(r->launchFile) ||
                Confirm(hwnd_, L"Open a program from the archive?",
                        L"\"" + shown + L"\" can run code on your computer. Only open it if you trust where this archive came from."))
                ShellExecuteW(hwnd_, nullptr, r->launchFile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (!r->openAfter.empty() && !r->error) { UpdateStatus(); OpenArchive(r->openAfter); return 0; }
        UpdateStatus();
        return 0;
    }

    case WM_CLOSE:
        CloseQuickLook();               // the preview pane goes back before anything is destroyed
        if (busy_) {
            // Let the running job stop cleanly; the window closes when it reports back.
            closing_ = true;
            if (cancel_) cancel_->Cancel();
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        // "Install automatically": a verified update waits -> install it now, quietly (no restart: the user closed us).
        if (!updReadyFile_.empty() && !updLaunched_ && IsInstalledCopy()) {
            updLaunched_ = true;
            ShellExecuteW(nullptr, L"open", updReadyFile_.c_str(), L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS",
                          nullptr, SW_HIDE);
            log::Info(L"Update: installing " + updReadyFile_ + L" on close");
        }
        PostQuitMessage(0);
        return 0;
    case WM_APP_UPDATE:
        OnUpdateMessage(lp);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace mzr::ui
