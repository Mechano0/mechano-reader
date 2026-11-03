#pragma once
// ExtractGuard: mandatory decompression-bomb protection (brief section 18).
// It is fed by the extraction stream as bytes are written. It does not trust
// the sizes declared in the archive, only the bytes actually produced.
#include <cstdint>
#include "utils/Result.h"

namespace mzr::security {

struct BombLimits {
    uint64_t maxEntries          = 1'000'000;
    uint64_t maxTotalBytes       = 0;          // 0 = use free disk space only
    uint64_t ratioCheckMinBytes  = 1ull << 30; // ratio test starts after 1 GB produced
    uint64_t maxRatio            = 1000;       // unpacked / packed
};

class ExtractGuard {
public:
    // archiveBytes: size of the archive file. Used for the ratio test of entries whose own
    // packed size is unknown (solid 7z/RAR report it only for the first file of a block).
    ExtractGuard(BombLimits limits, uint64_t freeDiskBytes, uint64_t archiveBytes = 0);

    // Call once per entry before writing it. packedSize 0 = unknown.
    Result<bool> BeginEntry(uint64_t packedSize);
    // Call for every chunk written. Returns an error the moment a limit is crossed.
    Result<bool> OnBytesWritten(uint64_t chunkBytes);

    uint64_t TotalWritten() const { return total_; }

private:
    BombLimits limits_;
    uint64_t   freeDisk_;
    uint64_t   archiveBytes_;
    uint64_t   entries_      = 0;
    uint64_t   total_        = 0;
    uint64_t   entryWritten_ = 0;
    uint64_t   entryPacked_  = 0;
};

} // namespace mzr::security
