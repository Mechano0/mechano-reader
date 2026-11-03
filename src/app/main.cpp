// MECHANO ZIP READER - entry point.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <mfapi.h>

#include "core/archive/ArchiveEngine.h"
#include "core/filesystem/FileSystem.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"
#include "utils/Log.h"

namespace {

// Milliseconds since the process was created (includes the Windows loader time).
double SinceStart() {
    FILETIME c{}, e{}, k{}, u{}, now{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimePreciseAsFileTime(&now);
    auto v = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(v(now) - v(c)) / 1e4;
}

// Remove leftovers from "Extract & Open" of previous sessions (files may be in
// use by other programs; those are simply skipped).
void CleanTempFolder() {
    const std::wstring tmp = mzr::fs::TempFolder();
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((tmp + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring from = tmp + L"\\" + n;
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- Explorer right-click menu, several files selected.
// Explorer starts one copy of the program per selected file ("MechanoZip.exe /add <file>").
// The first copy waits a moment and collects the files of the others (WM_COPYDATA to a hidden
// message-only window); the others hand their file over and quit. One archive for the whole selection.
constexpr wchar_t kCollectorClass[] = L"MechanoReader.Collector";
constexpr ULONG_PTR kCopyMagic = 0x4D5A5231;            // "MZR1"

struct Collected {
    std::vector<std::wstring> paths;
    ULONGLONG last = 0;
};

LRESULT CALLBACK CollectorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        auto* c = reinterpret_cast<Collected*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (!c || !cd || cd->dwData != kCopyMagic || !cd->lpData || cd->cbData == 0 || cd->cbData % sizeof(wchar_t) ||
            cd->cbData > 32768 * sizeof(wchar_t) || c->paths.size() >= 5000)
            return FALSE;
        c->paths.emplace_back(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
        c->last = GetTickCount64();
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

// Returns false when this copy handed its files to the first copy (and should just quit).
bool CollectSelection(const std::wstring& verb, std::vector<std::wstring>& paths) {
    const std::wstring mutexName = L"Local\\MechanoReader.Collect" + verb;
    HANDLE mtx = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    const bool first = mtx && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!first) {
        // Another copy is collecting: give it our files. No answer in 3 s -> work alone.
        for (int i = 0; i < 60; ++i) {
            if (HWND w = FindWindowExW(HWND_MESSAGE, nullptr, kCollectorClass, verb.c_str())) {
                bool ok = true;
                for (const auto& p : paths) {
                    COPYDATASTRUCT cd{kCopyMagic, static_cast<DWORD>(p.size() * sizeof(wchar_t)), const_cast<wchar_t*>(p.data())};
                    DWORD_PTR res = 0;
                    if (!SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 2000, &res) || !res) ok = false;
                }
                if (ok) { if (mtx) CloseHandle(mtx); return false; }
                break;
            }
            Sleep(50);
        }
        if (mtx) CloseHandle(mtx);
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = CollectorProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCollectorClass;
    RegisterClassW(&wc);
    Collected c{paths, GetTickCount64()};
    if (HWND w = CreateWindowExW(0, kCollectorClass, verb.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr)) {
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&c));
        const ULONGLONG start = GetTickCount64();
        // Until no new file came for 0.5 s (10 s at most).
        while (GetTickCount64() - c.last < 500 && GetTickCount64() - start < 10000) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        }
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        DestroyWindow(w);
    }
    CloseHandle(mtx);
    // Keep real, absolute paths only, each once.
    std::vector<std::wstring> out;
    for (auto& p : c.paths) {
        if (p.size() < 3 || (p[1] != L':' && !(p[0] == L'\\' && p[1] == L'\\'))) continue;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        bool dup = false;
        for (auto& o : out) if (CompareStringOrdinal(o.c_str(), -1, p.c_str(), -1, TRUE) == CSTR_EQUAL) { dup = true; break; }
        if (!dup) out.push_back(std::move(p));
    }
    paths = std::move(out);
    return true;
}

std::wstring FullPath(const wchar_t* p) {
    // Resolve relative paths ("..\x.zip") against the current folder.
    static wchar_t full[32768];
    const DWORD n = GetFullPathNameW(p, 32768, full, nullptr);
    return (n > 0 && n < 32768) ? std::wstring(full, n) : std::wstring(p);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Security hardening before anything else loads:
    // only load DLLs from System32 and the app folder (blocks DLL planting).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    // Command line:
    //   MechanoZip.exe <archive>                   open it (file association / Open With / drop on the exe)
    //   MechanoZip.exe /extract-here <archive>     Explorer menu: Extract here (Smart Extract)
    //   MechanoZip.exe /extract-to <archive>       Explorer menu: Extract to "<name>\"
    //   MechanoZip.exe /add <file or folder>...    Explorer menu: Add to archive... (dialog)
    //   MechanoZip.exe /zip <file or folder>...    Explorer menu: Compress to "<name>.zip"
    //   MechanoZip.exe /zip-each <items>...        Explorer menu: Compress each to its own ZIP (batch)
    //   (/extract-here and /extract-to with several archives selected = a batch in one window)
    using SA = mzr::ui::MainWindow::ShellAction;
    SA action = SA::None;
    std::vector<std::wstring> paths;
    std::wstring verb;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int i = 1;
        if (argv && argc > 1 && argv[1][0] == L'/') {
            verb = argv[1];
            if (_wcsicmp(argv[1], L"/extract-here") == 0) action = SA::ExtractHere;
            else if (_wcsicmp(argv[1], L"/extract-to") == 0) action = SA::ExtractToFolder;
            else if (_wcsicmp(argv[1], L"/add") == 0) action = SA::AddDialog;
            else if (_wcsicmp(argv[1], L"/zip") == 0) action = SA::AddZip;
            else if (_wcsicmp(argv[1], L"/zip-each") == 0) action = SA::ZipEach;
            i = 2;
        }
        for (; argv && i < argc; ++i) if (argv[i][0]) paths.push_back(FullPath(argv[i]));
        if (argv) LocalFree(argv);
        if (!verb.empty() && (action == SA::None || paths.empty())) return 0;     // unknown switch: do nothing
    }
    // Every Explorer menu command: several selected items become one task (one archive, or one batch).
    if (action != SA::None && !CollectSelection(verb.substr(1), paths)) return 0;
    if (action != SA::None && paths.empty()) return 0;

    // OleInitialize (not just COM): the Word preview's RichEdit shows pictures as OLE static objects.
    if (FAILED(OleInitialize(nullptr))) return 1;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    mzr::log::Init(true);
    const double tOle = SinceStart();
    std::wstring err;
    if (!mzr::archive::LoadEngine(&err)) {
        mzr::log::Error(err);
        mzr::ui::ShowError(nullptr, L"Mechano Reader cannot start",
                           mzr::ErrorInfo{mzr::ErrorCode::Internal,
                                          L"A required program file (7z.dll) is missing or damaged. Please reinstall Mechano Reader.", err});
        OleUninitialize();
        return 2;
    }
    const double t7z = SinceStart();
    CleanTempFolder();
    const double tTemp = SinceStart();
    // Media Foundation starts on the first video / audio preview (MediaPlayer::Open), not here.

    const double tMf = SinceStart();

    int code = 0;
    {
        mzr::ui::MainWindow win;
        if (!win.Create(inst, show)) { OleUninitialize(); return 3; }
        {
            wchar_t b[200];
            swprintf_s(b, L"Startup: window shown at %.0f ms (loader+OLE %.0f, 7z.dll %.0f, temp clean %.0f, window %.0f)",
                       SinceStart(), tOle, t7z - tOle, tTemp - t7z, SinceStart() - tMf);
            mzr::log::Info(b);
        }

        if (action != SA::None) win.RunShellAction(action, std::move(paths));
        else if (!paths.empty()) win.OpenArchive(paths[0]);

        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (win.PreTranslate(&m)) continue;
            if (TranslateAcceleratorW(win.hwnd(), win.accelerators(), &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        code = static_cast<int>(m.wParam);
    }
    mzr::ui::MediaPlayer::ShutdownEngine();
    OleUninitialize();
    return code;
}
