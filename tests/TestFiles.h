#pragma once
// Helpers for tests that touch the disk: a private temp folder per test that
// is deleted afterwards.
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "core/filesystem/FileSystem.h"

namespace mzr_test {

class TempDir {
public:
    explicit TempDir(const wchar_t* tag) {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path_ = std::wstring(base) + L"mzr_test_" + tag + L"_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + std::to_wstring(GetTickCount64());
        mzr::fs::CreateDirectories(path_);
    }
    ~TempDir() { Remove(path_); }
    const std::wstring& path() const { return path_; }
    std::wstring operator/(const std::wstring& rel) const { return path_ + L"\\" + rel; }

    static void Remove(const std::wstring& dir) {
        std::wstring from = dir;
        from.push_back(L'\0');           // SHFileOperation needs a double-NUL list
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
private:
    std::wstring path_;
};

inline bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    mzr::fs::CreateDirectories(path.substr(0, path.find_last_of(L'\\')));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
    CloseHandle(h);
    return w == data.size();
}

inline std::string ReadFileBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string data(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD r = 0;
    if (!data.empty()) ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &r, nullptr);
    CloseHandle(h);
    return data;
}

inline bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

} // namespace mzr_test
