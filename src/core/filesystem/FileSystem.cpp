#include "core/filesystem/FileSystem.h"

#include <shlobj.h>
#include <bcrypt.h>
#include <deque>
#pragma comment(lib, "bcrypt.lib")

namespace mzr::fs {

std::wstring ToLongPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

static DWORD Attr(const std::wstring& p) { return GetFileAttributesW(ToLongPath(p).c_str()); }

bool Exists(const std::wstring& p) { return Attr(p) != INVALID_FILE_ATTRIBUTES; }
bool IsDirectory(const std::wstring& p) {
    const DWORD a = Attr(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool IsReparsePoint(const std::wstring& p) {
    const DWORD a = Attr(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool CreateDirectories(const std::wstring& fullDir, std::wstring* error) {
    if (IsDirectory(fullDir)) return true;
    // Length of the root that must already exist: "C:\" or "\\server\share\".
    size_t rootLen = 0;
    if (fullDir.rfind(L"\\\\", 0) == 0) {
        const size_t a = fullDir.find(L'\\', 2);
        const size_t b = a == std::wstring::npos ? std::wstring::npos : fullDir.find(L'\\', a + 1);
        rootLen = b == std::wstring::npos ? fullDir.size() : b + 1;
    } else if (fullDir.size() >= 3 && fullDir[1] == L':' && fullDir[2] == L'\\') {
        rootLen = 3;
    }
    // Walk down from the root, creating each missing component.
    size_t pos = rootLen;
    while (pos <= fullDir.size()) {
        size_t end = fullDir.find(L'\\', pos);
        if (end == std::wstring::npos) end = fullDir.size();
        const bool emptyComp = end == pos;
        const std::wstring cur = fullDir.substr(0, end);
        pos = end + 1;
        if (emptyComp) continue;
        const DWORD a = Attr(cur);
        if (a == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(ToLongPath(cur).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                if (error) *error = L"CreateDirectory failed (" + std::to_wstring(GetLastError()) + L"): " + cur;
                return false;
            }
        } else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
            if (error) *error = L"A file with the same name already exists: " + cur;
            return false;
        }
    }
    return true;
}

std::wstring UniqueName(const std::wstring& full) {
    if (!Exists(full)) return full;
    const size_t slash = full.find_last_of(L'\\');
    size_t dot = full.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) dot = full.size();
    const std::wstring stem = full.substr(0, dot), ext = full.substr(dot);
    for (int i = 2; i < 100000; ++i) {
        std::wstring c = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!Exists(c)) return c;
    }
    return full;
}

uint64_t FreeSpace(const std::wstring& folder) {
    ULARGE_INTEGER freeBytes{};
    std::wstring f = folder;
    // Walk up until an existing folder is found (destination may not exist yet).
    while (!f.empty() && !IsDirectory(f)) {
        const size_t s = f.find_last_of(L'\\');
        if (s == std::wstring::npos || s < 2) break;
        f.resize(s);
    }
    if (f.size() == 2 && f[1] == L':') f += L'\\';
    if (!GetDiskFreeSpaceExW(f.c_str(), &freeBytes, nullptr, nullptr)) return 0;
    return freeBytes.QuadPart;
}

std::string ReadZoneIdentifier(const std::wstring& file) {
    HANDLE h = CreateFileW((ToLongPath(file) + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string data(4096, '\0');          // Zone.Identifier is a tiny INI text
    DWORD got = 0;
    ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(h);
    data.resize(got);
    return data;
}

bool WriteZoneIdentifier(const std::wstring& file, const std::string& data) {
    if (data.empty()) return true;
    HANDLE h = CreateFileW((ToLongPath(file) + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;          // e.g. FAT32/exFAT: no streams, not an error
    DWORD w = 0;
    const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
    CloseHandle(h);
    return ok && w == data.size();
}

std::vector<archive::SourceItem> CollectSources(const std::vector<std::wstring>& selected,
                                                const std::wstring& archivePrefix,
                                                std::vector<std::wstring>* skipped) {
    std::vector<archive::SourceItem> out;
    auto join = [](const std::wstring& a, const std::wstring& b) {
        if (a.empty()) return b;
        return a + L"\\" + b;
    };
    for (const auto& sel : selected) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(ToLongPath(sel).c_str(), GetFileExInfoStandard, &fa)) {
            if (skipped) skipped->push_back(sel);
            continue;
        }
        std::wstring name = sel;
        while (name.size() > 3 && name.back() == L'\\') name.pop_back();
        name = name.substr(name.find_last_of(L'\\') + 1);

        archive::SourceItem top;
        top.diskPath = sel;
        top.archivePath = join(archivePrefix, name);
        top.isDir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        top.attrib = fa.dwFileAttributes;
        top.mtime = fa.ftLastWriteTime;
        top.size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        out.push_back(top);
        if (!top.isDir || (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;

        // Breadth-first walk with FindFirstFileEx (large fetch, no short names: faster on HDDs).
        std::deque<std::pair<std::wstring, std::wstring>> q{{sel, top.archivePath}};
        while (!q.empty()) {
            auto [dir, arc] = q.front();
            q.pop_front();
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileExW(ToLongPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) { if (skipped) skipped->push_back(dir); continue; }
            do {
                const std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {   // never follow links
                    if (skipped) skipped->push_back(dir + L"\\" + n);
                    continue;
                }
                archive::SourceItem it;
                it.diskPath = dir + L"\\" + n;
                it.archivePath = arc + L"\\" + n;
                it.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                it.attrib = fd.dwFileAttributes;
                it.mtime = fd.ftLastWriteTime;
                it.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                out.push_back(it);
                if (it.isDir) q.emplace_back(it.diskPath, it.archivePath);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return out;
}

std::wstring ExeFolder() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return buf.substr(0, buf.find_last_of(L'\\'));
}

bool IsPortable() {
    static const bool portable = [] {
        const std::wstring dir = ExeFolder();
        return !dir.empty() && Exists(dir + L"\\portable.flag") && !IsDirectory(dir + L"\\portable.flag");
    }();
    return portable;
}

static std::wstring SystemTempMechanoZip() {
    wchar_t tmp[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    std::wstring t = (n > 0 && n <= MAX_PATH) ? std::wstring(tmp, n) : std::wstring(L"C:\\Windows\\Temp\\");
    if (t.back() != L'\\') t += L'\\';
    return t + L"MechanoZip";
}

std::wstring AppDataFolder() {
    std::wstring out;
    if (IsPortable()) {
        out = ExeFolder() + L"\\Data";
    } else {
        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) {
            out = std::wstring(base) + L"\\MechanoZip";
            CoTaskMemFree(base);
        } else {
            out = SystemTempMechanoZip();
        }
    }
    CreateDirectories(out);
    return out;
}

std::wstring TempFolder() {
    std::wstring t = IsPortable() ? SystemTempMechanoZip() : AppDataFolder() + L"\\tmp";
    CreateDirectories(t);
    return t;
}

std::wstring RandomHex(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        // Practically never happens; still unique enough together with CREATE_NEW / CreateDirectory checks.
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>((c.QuadPart >> (8 * (i % 8))) ^ (GetCurrentThreadId() * (i + 1)));
    }
    static const wchar_t* hx = L"0123456789abcdef";
    std::wstring s;
    s.reserve(bytes * 2);
    for (uint8_t v : b) { s += hx[v >> 4]; s += hx[v & 15]; }
    return s;
}

std::wstring NewTempSubfolder(const std::wstring& prefix) {
    const std::wstring base = TempFolder() + L"\\" + prefix;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::wstring dir = base + RandomHex();
        if (CreateDirectoryW(ToLongPath(dir).c_str(), nullptr)) return dir;     // fails if it already exists
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

} // namespace mzr::fs
