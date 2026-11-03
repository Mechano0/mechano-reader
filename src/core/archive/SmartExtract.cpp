#include "core/archive/SmartExtract.h"

#include <cwctype>

namespace mzr::archive {
namespace {

bool EndsWithNoCase(const std::wstring& s, std::wstring_view suffix) {
    if (s.size() < suffix.size()) return false;
    return CompareStringOrdinal(s.c_str() + s.size() - suffix.size(), static_cast<int>(suffix.size()),
                                suffix.data(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

bool IsAllDigits(std::wstring_view s) {
    if (s.empty()) return false;
    for (wchar_t c : s) if (!std::iswdigit(c)) return false;
    return true;
}

} // namespace

std::wstring ArchiveBaseName(const std::wstring& archivePath) {
    std::wstring name = archivePath.substr(archivePath.find_last_of(L"\\/") + 1);
    // Volume suffix ".001"
    if (auto dot = name.find_last_of(L'.'); dot != std::wstring::npos && IsAllDigits(std::wstring_view(name).substr(dot + 1)))
        name.resize(dot);
    // Double extensions first
    for (auto ext : {L".tar.gz", L".tar.bz2", L".tar.xz", L".tar.zst", L".tar.lz"}) {
        std::wstring_view e(ext);
        if (EndsWithNoCase(name, e)) { name.resize(name.size() - e.size()); return name.empty() ? L"archive" : name; }
    }
    if (auto dot = name.find_last_of(L'.'); dot != std::wstring::npos && dot > 0) name.resize(dot);
    // ".part1" of RAR volumes
    if (auto dot = name.find_last_of(L'.'); dot != std::wstring::npos) {
        std::wstring_view tail = std::wstring_view(name).substr(dot + 1);
        if (tail.size() > 4 && CompareStringOrdinal(tail.data(), 4, L"part", 4, TRUE) == CSTR_EQUAL && IsAllDigits(tail.substr(4)))
            name.resize(dot);
    }
    return name.empty() ? L"archive" : name;
}

SmartTarget ComputeSmartTarget(const ArchiveIndex& index, const std::wstring& archivePath,
                               const std::wstring& baseFolder) {
    SmartTarget t;
    std::wstring base = baseFolder;
    while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();

    const auto& root = index.node(ArchiveIndex::kRoot);
    if (root.children.size() == 1) {
        t.destination = base;          // the single item already is the "folder"
        return t;
    }
    t.destination = base + L"\\" + ArchiveBaseName(archivePath);
    t.createdSubfolder = true;
    return t;
}

} // namespace mzr::archive
