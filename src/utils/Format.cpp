#include "utils/Format.h"

#include <cwchar>

namespace mzr::fmt {

std::wstring Size(uint64_t b) {
    wchar_t buf[64];
    if (b < 1024) swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) swprintf_s(buf, L"%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf_s(buf, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return buf;
}

std::wstring Ratio(uint64_t packed, uint64_t size) {
    if (!size) return L"";
    wchar_t buf[16];
    swprintf_s(buf, L"%u%%", static_cast<unsigned>((packed * 100 + size / 2) / size));
    return buf;
}

std::wstring DateTime(const FILETIME& ft) {
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return L"";
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t d[64]{}, t[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    return std::wstring(d) + L" " + t;
}

std::wstring Hex32(uint32_t v) {
    wchar_t buf[16];
    swprintf_s(buf, L"%08X", v);
    return buf;
}

std::wstring Number(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), 1, L',');
    return s;
}

} // namespace mzr::fmt
