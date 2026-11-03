#pragma once
// ErrorInfo = what the user sees (friendly) + what the "Details" button shows (technical).
// Brief section 26: never show only "Error 0x...".
#include <string>

namespace mzr {

enum class ErrorCode {
    None = 0,
    // Path / security
    EmptyName,
    NullByteInName,
    AbsolutePath,        // C:\..., \\server\..., \..., \\?\...
    ParentTraversal,     // contains ".."
    NameTooLong,
    PathTooDeep,
    EscapesDestination,  // final full path is outside the destination folder
    // Bomb / limits
    TooManyEntries,
    SizeLimitExceeded,
    SuspiciousRatio,
    DiskSpace,
    // Generic
    Corrupted,
    WrongPassword,
    Io,
    Cancelled,
    Internal,
};

struct ErrorInfo {
    ErrorCode    code = ErrorCode::None;
    std::wstring friendly;   // shown to the user (localized later)
    std::wstring details;    // technical text for the "Details" button / log (never a password)
};

} // namespace mzr
