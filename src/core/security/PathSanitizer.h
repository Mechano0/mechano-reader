#pragma once
// PathSanitizer: turns an untrusted archive entry name into a safe relative
// Windows path, then proves the final full path stays inside the destination.
// Protects against: Zip Slip / path traversal, absolute + UNC + device paths,
// NUL bytes, reserved DOS names, ADS (':' streams), trailing dots/spaces,
// control characters, excessive depth and length.  (Brief sections 18 and 22.)
#include <string>
#include <string_view>
#include <vector>
#include "utils/Result.h"

namespace mzr::security {

struct PathLimits {
    size_t maxComponentChars = 255;   // NTFS per-name limit
    size_t maxTotalChars     = 1024;  // relative path length we accept from an archive
    size_t maxDepth          = 64;    // folder nesting
};

struct SanitizedPath {
    std::wstring relative;        // e.g. L"docs\\report.pdf"  (always '\', never leading '\')
    bool         wasModified = false;  // characters were replaced / names renamed
    bool         hasBidiOverride = false; // U+202E etc. present (F8 warning, name kept visible-safe)
};

// Step 1 (pure, no disk access, fully unit-testable).
Result<SanitizedPath> SanitizeEntryName(std::wstring_view rawName, const PathLimits& limits = {});

// Step 2: join with the destination and verify with GetFullPathNameW that the
// result is still inside `destinationRoot` (case-insensitive prefix check on a
// component boundary). `destinationRoot` must be an absolute path.
Result<std::wstring> ResolveInsideDestination(const std::wstring& destinationRoot,
                                              const SanitizedPath& sanitized);

// Helpers exposed for tests.
bool IsReservedDosName(std::wstring_view component);
bool IsPathInside(const std::wstring& fullRoot, const std::wstring& fullCandidate);

// True when opening a file with this name can run code (exe, scripts, shortcuts, installers ...).
// Looks at the name the way Windows will see it on disk: trailing dots / spaces and hidden
// text-direction characters are removed first, so "invoice.exe." and "invoice.exe " count (S3).
bool IsRiskyFileName(std::wstring_view name);

// Rename inside an archive: checks ONE new name typed by the user.
// Returns an empty string when it is fine, otherwise the reason to show.
std::wstring CheckNewEntryName(std::wstring_view name);

} // namespace mzr::security
