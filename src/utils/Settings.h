#pragma once
// Small user settings (numbers only for now).
// Installed: HKCU\Software\MechanoZip.  Portable mode (F11): <exe>\Data\settings.ini, no registry.
#include <cstdint>

namespace mzr::settings {

uint32_t GetNumber(const wchar_t* name, uint32_t fallback);
void     SetNumber(const wchar_t* name, uint32_t value);

} // namespace mzr::settings
