#pragma once
// DWG -> DXF through LibreDWG's dwg2dxf.exe (GPL-3, shipped next to the app in the "cad" folder).
// It always runs as a SEPARATE process inside a Windows Job Object:
//   - memory limit, time limit, no child processes, killed if our app closes or the user moves on;
//   - no window, below-normal priority, its output goes nowhere.
// A damaged or hostile DWG can at worst crash that helper process, never Mechano Reader.
#include <cstdint>
#include <functional>
#include <string>
#include "utils/Result.h"

namespace mzr::cad {

struct ConvertLimits {
    uint32_t timeoutMs       = 120'000;          // 2 minutes
    uint64_t maxProcessBytes = 2ull << 30;       // 2 GB of RAM for the helper
    uint64_t maxDwgBytes     = 512ull << 20;     // bigger drawings are not converted
};

// "<exe folder>\cad\dwg2dxf.exe", or empty if the helper is not installed.
std::wstring FindDwgConverter();

// Converts `dwgPath` to `dxfPath`. Both should be in a private temp folder with plain ASCII
// names (dwg2dxf cannot open Unicode file names). `cancelled` is polled every 100 ms.
Result<bool> ConvertDwgToDxf(const std::wstring& converter, const std::wstring& dwgPath, const std::wstring& dxfPath,
                             const std::function<bool()>& cancelled, const ConvertLimits& limits = {});

} // namespace mzr::cad
