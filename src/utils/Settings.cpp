#include "utils/Settings.h"

#include <windows.h>
#include <string>
#include "core/filesystem/FileSystem.h"

namespace mzr::settings {
namespace {
constexpr wchar_t kRegKey[] = L"Software\\MechanoZip";
constexpr wchar_t kSection[] = L"Settings";
std::wstring IniPath() { return fs::AppDataFolder() + L"\\settings.ini"; }
} // namespace

uint32_t GetNumber(const wchar_t* name, uint32_t fallback) {
    if (fs::IsPortable()) {
        wchar_t buf[32]{};
        GetPrivateProfileStringW(kSection, name, L"", buf, 32, IniPath().c_str());
        if (!buf[0]) return fallback;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(buf, &end, 10);
        return (end && *end == 0) ? static_cast<uint32_t>(v) : fallback;
    }
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) return v;
    return fallback;
}

void SetNumber(const wchar_t* name, uint32_t value) {
    if (fs::IsPortable()) {
        WritePrivateProfileStringW(kSection, name, std::to_wstring(value).c_str(), IniPath().c_str());
        return;
    }
    const DWORD v = value;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof(v));
}

} // namespace mzr::settings
