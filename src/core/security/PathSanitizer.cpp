#include "core/security/PathSanitizer.h"

#include <windows.h>
#include <cwctype>

namespace mzr::security {
namespace {

ErrorInfo Err(ErrorCode code, std::wstring friendly, std::wstring_view name) {
    return ErrorInfo{code, std::move(friendly), L"Entry name: \"" + std::wstring(name) + L"\""};
}

// Characters Windows forbids in a file name, plus ':' (would create an NTFS
// alternate data stream such as "file.txt:hidden.exe").
bool IsForbiddenChar(wchar_t c) {
    return c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
           c == L'|' || c == L'?' || c == L'*';
}

// Unicode bidirectional control characters used to disguise extensions
// ("invoice\u202Efdp.exe" displays as "invoiceexe.pdf").
bool IsBidiControl(wchar_t c) {
    return (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F;
}

wchar_t Upper(wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); }

} // namespace

bool IsReservedDosName(std::wstring_view component) {
    // Reserved base name = part before the first '.', trailing spaces ignored.
    // "CON", "con.txt", "COM1.tar.gz", "LPT¹" are all reserved on Windows.
    std::wstring base(component.substr(0, component.find(L'.')));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    for (auto& c : base) c = Upper(c);

    static const wchar_t* kFixed[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (auto* r : kFixed) if (base == r) return true;

    if (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0)) {
        const wchar_t d = base[3];
        // 1-9 plus superscript ¹ ² ³ (Windows treats these as device names too).
        // COM0/LPT0 are reserved on recent Windows as well, so we include '0'.
        if ((d >= L'0' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3) return true;
    }
    return false;
}

Result<SanitizedPath> SanitizeEntryName(std::wstring_view raw, const PathLimits& limits) {
    if (raw.empty())
        return Err(ErrorCode::EmptyName, L"This item has no name and was skipped.", raw);

    // A NUL in the middle is a truncation trick ("evil.exe\0.txt"). Never repair it, reject.
    if (raw.find(L'\0') != std::wstring_view::npos)
        return Err(ErrorCode::NullByteInName, L"This item has a hidden invalid character in its name and was skipped for safety.", raw);

    if (raw.size() > limits.maxTotalChars)
        return Err(ErrorCode::NameTooLong, L"This item's path is too long and was skipped.", raw);

    // Normalise separators: ZIP uses '/', old Windows tools wrote '\'.
    std::wstring path(raw);
    for (auto& c : path) if (c == L'/') c = L'\\';

    // Absolute / drive / UNC / device forms. "C:x" (drive-relative) is rejected too.
    const bool driveForm = path.size() >= 2 && path[1] == L':' && std::iswalpha(path[0]);
    if (driveForm || path.rfind(L"\\\\", 0) == 0)
        return Err(ErrorCode::AbsolutePath, L"This item tries to write to an absolute location and was blocked.", raw);

    SanitizedPath out;
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L'\\', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring comp = path.substr(start, end - start);
        start = end + 1;

        if (comp.empty() || comp == L".") continue;          // "a//b", "./a", leading '\' (made relative)
        if (comp == L"..")
            return Err(ErrorCode::ParentTraversal, L"This item tries to write outside the destination folder and was blocked.", raw);

        for (auto& c : comp) {
            if (IsBidiControl(c)) { out.hasBidiOverride = true; c = L'_'; out.wasModified = true; }
            else if (IsForbiddenChar(c)) { c = L'_'; out.wasModified = true; }
        }
        // Windows silently strips trailing dots/spaces: "a.txt. " == "a.txt". Make it explicit.
        while (!comp.empty() && (comp.back() == L'.' || comp.back() == L' ')) { comp.pop_back(); out.wasModified = true; }
        // A component made only of dots (e.g. "...") would become empty, or worse, resolve upward.
        if (comp.empty()) { comp = L"_"; out.wasModified = true; }
        if (IsReservedDosName(comp)) { comp.insert(0, L"_"); out.wasModified = true; }
        if (comp.size() > limits.maxComponentChars)
            return Err(ErrorCode::NameTooLong, L"A file or folder name in this item is too long and was skipped.", raw);

        parts.push_back(std::move(comp));
        if (parts.size() > limits.maxDepth)
            return Err(ErrorCode::PathTooDeep, L"This item is nested too deeply and was skipped.", raw);
    }

    if (parts.empty())
        return Err(ErrorCode::EmptyName, L"This item has no usable name and was skipped.", raw);

    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.relative += L'\\';
        out.relative += parts[i];
    }
    if (out.relative != std::wstring(raw)) {
        // Separator changes alone ('/' -> '\') are not a "modification" the user cares about.
        std::wstring rawBack(raw);
        for (auto& c : rawBack) if (c == L'/') c = L'\\';
        while (!rawBack.empty() && rawBack.back() == L'\\') rawBack.pop_back();
        if (rawBack != out.relative) out.wasModified = true;
    }
    return out;
}

static std::wstring FullPath(const std::wstring& p) {
    // Extended-length aware: GetFullPathNameW handles >MAX_PATH when given enough buffer.
    DWORD need = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (need == 0) return {};
    std::wstring buf(need, L'\0');
    DWORD got = GetFullPathNameW(p.c_str(), need, buf.data(), nullptr);
    if (got == 0 || got >= need) return {};
    buf.resize(got);
    return buf;
}

bool IsPathInside(const std::wstring& fullRoot, const std::wstring& fullCandidate) {
    std::wstring root = fullRoot;
    while (root.size() > 3 && root.back() == L'\\') root.pop_back();   // keep "C:\"
    if (fullCandidate.size() <= root.size()) return false;            // must be strictly below root
    // Case-insensitive compare the way NTFS does it (ordinal, upper-case table).
    if (CompareStringOrdinal(fullCandidate.c_str(), static_cast<int>(root.size()),
                             root.c_str(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL)
        return false;
    // Must split on a component boundary: "C:\out" must not accept "C:\outside\x".
    return root.back() == L'\\' || fullCandidate[root.size()] == L'\\';
}

Result<std::wstring> ResolveInsideDestination(const std::wstring& destinationRoot,
                                              const SanitizedPath& sanitized) {
    const std::wstring root = FullPath(destinationRoot);
    if (root.empty())
        return ErrorInfo{ErrorCode::Io, L"The destination folder is not valid.", L"GetFullPathNameW failed for destination"};

    std::wstring joined = root;
    if (joined.back() != L'\\') joined += L'\\';
    joined += sanitized.relative;

    const std::wstring full = FullPath(joined);
    if (full.empty() || !IsPathInside(root, full))
        return ErrorInfo{ErrorCode::EscapesDestination,
                         L"This item tries to write outside the destination folder and was blocked.",
                         L"Resolved path: " + full + L"  Destination: " + root};
    return full;
}

} // namespace mzr::security

namespace mzr::security {

bool IsRiskyFileName(std::wstring_view name) {
    // Last path component only.
    const size_t slash = name.find_last_of(L"\\/");
    std::wstring n(slash == std::wstring_view::npos ? name : name.substr(slash + 1));
    std::wstring clean;
    for (wchar_t c : n) if (!IsBidiControl(c)) clean += c;
    while (!clean.empty() && (clean.back() == L'.' || clean.back() == L' ')) clean.pop_back();   // Windows drops them
    const size_t dot = clean.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = clean.substr(dot + 1);
    static const wchar_t* kRisky[] = {
        // programs, libraries, installers
        L"exe", L"com", L"scr", L"pif", L"cpl", L"dll", L"msi", L"msp", L"mst", L"msc", L"appx", L"msix",
        L"appxbundle", L"msixbundle", L"application", L"appref-ms", L"xbap", L"gadget", L"jar", L"xll", L"diagcab",
        // scripts
        L"bat", L"cmd", L"ps1", L"psm1", L"psd1", L"ps1xml", L"vb", L"vbs", L"vbe", L"js", L"jse", L"wsf", L"wsh",
        L"wsc", L"sct", L"hta", L"msh", L"reg", L"inf", L"chm", L"hlp",
        // shortcuts / shell files that point somewhere else
        L"lnk", L"url", L"website", L"scf", L"settingcontent-ms", L"library-ms", L"search-ms",
        // disk images (mounting shows content that bypasses Mark of the Web on old systems)
        L"iso", L"img", L"vhd", L"vhdx"};
    for (auto* r : kRisky)
        if (CompareStringOrdinal(ext.c_str(), -1, r, -1, TRUE) == CSTR_EQUAL) return true;
    return false;
}

std::wstring CheckNewEntryName(std::wstring_view name) {
    if (name.empty()) return L"The name cannot be empty.";
    if (name.size() > 255) return L"The name is too long (255 characters at most).";
    if (name == L"." || name == L"..") return L"\".\" and \"..\" cannot be used as a name.";
    for (wchar_t c : name) {
        if (c < 0x20) return L"The name cannot contain control characters.";
        if (wcschr(L"\\/:*?\"<>|", c)) return L"A name cannot contain any of these characters:  \\ / : * ? \" < > |";
        // Right-to-left override and friends: used to disguise ".exe" files as documents.
        if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069))
            return L"The name cannot contain hidden text-direction characters.";
    }
    if (name.back() == L'.' || name.back() == L' ') return L"A name cannot end with a dot or a space.";
    if (name.front() == L' ') return L"A name cannot start with a space.";
    if (IsReservedDosName(name)) return L"This name is reserved by Windows (CON, PRN, AUX, NUL, COM1, LPT1 ...).";
    return {};
}

} // namespace mzr::security
