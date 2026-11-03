#pragma once
// File System Layer: every disk operation the app performs goes through here.
// All paths are converted to the \\?\ long form internally so deep archives
// (> 260 chars) work on every Windows 10/11 machine.
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "core/archive/ArchiveTypes.h"

namespace mzr::fs {

// "C:\a\b" -> "\\?\C:\a\b",  "\\srv\s\x" -> "\\?\UNC\srv\s\x". Idempotent.
std::wstring ToLongPath(const std::wstring& fullPath);

bool Exists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool IsReparsePoint(const std::wstring& path);   // symlink / junction / mount point

// Creates every missing folder of `fullDir` (absolute path).
// Note: the app itself never creates links, so an archive can't plant a
// junction that later redirects writes; links already present in the user's
// own destination are the user's choice and are respected.
bool CreateDirectories(const std::wstring& fullDir, std::wstring* error = nullptr);

// "file.txt" -> "file (2).txt" until the name is free.
std::wstring UniqueName(const std::wstring& fullPath);

uint64_t FreeSpace(const std::wstring& folder);

// Mark of the Web: read the archive's Zone.Identifier stream (empty if none)
// and write it onto an extracted file.
std::string ReadZoneIdentifier(const std::wstring& file);
bool WriteZoneIdentifier(const std::wstring& file, const std::string& data);

// Expand user-selected files/folders into a flat list for an archive.
// Folders are walked recursively; reparse points are NOT followed (no loops,
// no surprise content). `archivePrefix` is prepended inside the archive.
std::vector<archive::SourceItem> CollectSources(const std::vector<std::wstring>& selected,
                                                const std::wstring& archivePrefix,
                                                std::vector<std::wstring>* skipped = nullptr);

// Folder of MechanoZip.exe (no trailing '\').
std::wstring ExeFolder();

// Portable mode (F11): a file named "portable.flag" next to the exe. Then settings and the
// log live in <exe folder>\Data (no registry writes), temp files in %TEMP%\MechanoZip.
bool IsPortable();

// Settings + log folder: %LOCALAPPDATA%\MechanoZip, or <exe folder>\Data in portable mode.
std::wstring AppDataFolder();
// Temp files: <AppDataFolder>\tmp, or %TEMP%\MechanoZip in portable mode (fast local disk,
// nothing big written to the USB stick). Removed at the next start.
std::wstring TempFolder();

// Cryptographically random hex text (BCryptGenRandom), 2 characters per byte.
std::wstring RandomHex(size_t bytes = 8);

// A NEW, empty folder <TempFolder>\<prefix><random>. Never reuses an existing folder
// (CreateDirectory must succeed). Empty string on failure.
std::wstring NewTempSubfolder(const std::wstring& prefix);

} // namespace mzr::fs
