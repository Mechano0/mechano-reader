#include "core/security/ExtractGuard.h"

#include <string>

namespace mzr::security {

ExtractGuard::ExtractGuard(BombLimits limits, uint64_t freeDiskBytes, uint64_t archiveBytes)
    : limits_(limits), freeDisk_(freeDiskBytes), archiveBytes_(archiveBytes) {}

Result<bool> ExtractGuard::BeginEntry(uint64_t packedSize) {
    if (++entries_ > limits_.maxEntries)
        return ErrorInfo{ErrorCode::TooManyEntries,
                         L"This archive contains too many items to extract safely.",
                         L"Entry limit: " + std::to_wstring(limits_.maxEntries)};
    entryWritten_ = 0;
    entryPacked_  = packedSize;
    return true;
}

Result<bool> ExtractGuard::OnBytesWritten(uint64_t chunk) {
    // Overflow-safe accumulation (a hostile size field must not wrap around).
    if (chunk > UINT64_MAX - total_)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"The archive reports an impossible size.", L"uint64 overflow"};
    total_        += chunk;
    entryWritten_ += chunk;

    if (freeDisk_ && total_ > freeDisk_)
        return ErrorInfo{ErrorCode::DiskSpace, L"There is not enough free disk space to extract these files.",
                         L"Written: " + std::to_wstring(total_) + L"  Free: " + std::to_wstring(freeDisk_)};

    if (limits_.maxTotalBytes && total_ > limits_.maxTotalBytes)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"The extracted data is larger than the allowed limit.",
                         L"Limit: " + std::to_wstring(limits_.maxTotalBytes)};

    // Ratio test only after a meaningful amount of output, so normal highly
    // compressible files (logs, text) are never flagged.
    const auto bomb = [](uint64_t out, uint64_t in, const wchar_t* what) {
        return ErrorInfo{ErrorCode::SuspiciousRatio,
                         L"This file expands to a suspiciously large size (possible zip bomb). Extraction was stopped.",
                         L"Unpacked: " + std::to_wstring(out) + L"  Packed (" + what + L"): " + std::to_wstring(in)};
    };
    if (entryPacked_) {
        // The entry's own packed size is known: per-entry ratio.
        if (entryWritten_ >= limits_.ratioCheckMinBytes && entryWritten_ / entryPacked_ > limits_.maxRatio)
            return bomb(entryWritten_, entryPacked_, L"entry");
    } else if (archiveBytes_) {
        // Unknown (0) = a non-first file of a solid block, NOT "1 byte" (S2: that flagged every
        // big file in a solid 7z). Fall back to everything written vs the whole archive file.
        if (total_ >= limits_.ratioCheckMinBytes && total_ / archiveBytes_ > limits_.maxRatio)
            return bomb(total_, archiveBytes_, L"archive");
    }
    return true;
}

} // namespace mzr::security
