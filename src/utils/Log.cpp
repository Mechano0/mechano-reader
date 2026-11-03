#include "utils/Log.h"

#include <windows.h>
#include <mutex>
#include "core/filesystem/FileSystem.h"

namespace mzr::log {
namespace {
std::mutex g_mutex;
bool g_enabled = false;
std::wstring g_path;
constexpr LONGLONG kMaxBytes = 1024 * 1024;
}

void Init(bool enabled) {
    std::lock_guard lock(g_mutex);
    g_enabled = enabled;
    if (enabled && g_path.empty()) g_path = fs::AppDataFolder() + L"\\mechanozip.log";
}

std::wstring FilePath() { return g_path; }

void Write(Level level, const std::wstring& message) {
    std::lock_guard lock(g_mutex);
    if (!g_enabled || g_path.empty()) return;

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa) &&
        ((LONGLONG(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > kMaxBytes)
        MoveFileExW(g_path.c_str(), (g_path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64];
    swprintf_s(head, L"%04u-%02u-%02u %02u:%02u:%02u [%s] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               level == Level::Error ? L"ERROR" : level == Level::Warning ? L"WARN " : L"INFO ");
    std::wstring line = head + message + L"\r\n";

    const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), n, nullptr, nullptr);

    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &w, nullptr);
    CloseHandle(h);
}

} // namespace mzr::log
