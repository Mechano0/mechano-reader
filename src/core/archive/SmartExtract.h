#pragma once
// Smart Extract (F5): never produce "folder\folder\..." and never spill 200
// loose files into the user's Downloads folder.
//   - archive has ONE top-level item  -> extract directly into the chosen folder
//   - archive has several top items   -> create "<archive name>\" and extract into it
#include <string>
#include "core/archive/ArchiveIndex.h"

namespace mzr::archive {

struct SmartTarget {
    std::wstring destination;     // final folder to pass to Extract
    bool         createdSubfolder = false;
};

// archivePath: full path of the archive (name used for the subfolder).
// baseFolder:  where the user asked to extract ("Extract Here" = archive's folder).
SmartTarget ComputeSmartTarget(const ArchiveIndex& index, const std::wstring& archivePath,
                               const std::wstring& baseFolder);

// "report.final.tar.gz" -> "report.final", "photos.zip" -> "photos", "a.7z.001" -> "a"
std::wstring ArchiveBaseName(const std::wstring& archivePath);

} // namespace mzr::archive
