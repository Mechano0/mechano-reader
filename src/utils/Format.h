#pragma once
// Human-friendly formatting for the UI (sizes in KB/MB/GB, dates, ratios).
#include <windows.h>
#include <cstdint>
#include <string>

namespace mzr::fmt {

std::wstring Size(uint64_t bytes);                    // "1.4 MB"
std::wstring Ratio(uint64_t packed, uint64_t size);   // "37%"  (packed / size)
std::wstring DateTime(const FILETIME& ft);            // user locale, short date + time
std::wstring Hex32(uint32_t v);                       // "1A2B3C4D"
std::wstring Number(uint64_t v);                      // "12,345"

} // namespace mzr::fmt
