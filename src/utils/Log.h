#pragma once
// Lightweight bounded log (brief section 27).
// File: %LOCALAPPDATA%\MechanoZip\mechanozip.log, rotated at 1 MB (one .old kept).
// NEVER pass passwords or file contents here.
#include <string>

namespace mzr::log {

enum class Level { Info, Warning, Error };

void Init(bool enabled = true);
void Write(Level level, const std::wstring& message);
inline void Info(const std::wstring& m) { Write(Level::Info, m); }
inline void Warn(const std::wstring& m) { Write(Level::Warning, m); }
inline void Error(const std::wstring& m) { Write(Level::Error, m); }
std::wstring FilePath();

} // namespace mzr::log
