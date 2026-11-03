#pragma once
// Revit files (.rvt project, .rfa family, .rte / .rft templates) WITHOUT Revit.
// The model itself is a closed Autodesk format (the big "Partitions" streams) and is never read.
// What is read, from a few small streams of the OLE compound file:
//   BasicFileInfo     Revit version, build, last saved path, central model path (workshared), locale
//   RevitPreview4.0   the 128 x 128 PNG thumbnail Revit saves
//   ProjectInformation (projects) a small ZIP with an Atom XML: project name, number, client, address ...
//   PartAtom          (families) Atom XML: family name, category, types with their parameter values
//   TransmissionData  XML: linked / external files (Revit links, CAD links, keynotes ...)
// Our own bounded compound-file reader is used (not the Windows structured-storage parser): every
// sector chain, count and size is limited, so a hostile file can neither loop nor eat memory.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "utils/Result.h"

namespace mzr::preview {

constexpr uint64_t kMaxRevitBytes = 1ull << 30;     // bigger files are not previewed (extracted to a temp file first)

// ---- OLE compound file (CFB) reader: reusable for other old Office-style containers.
class CompoundFile {
public:
    using ReadAt = std::function<bool(uint64_t offset, void* buffer, size_t bytes)>;
    static Result<CompoundFile> Open(ReadAt readAt, uint64_t fileSize);
    bool Has(const std::wstring& path) const;                                   // "Global/Latest"
    Result<std::vector<uint8_t>> Read(const std::wstring& path, uint64_t maxBytes) const;
    std::vector<std::wstring> StreamNames() const;                              // top level streams + storages ("Formats/")

private:
    struct Entry { std::wstring name; uint8_t type = 0; uint32_t left = 0xFFFFFFFF, right = 0xFFFFFFFF, child = 0xFFFFFFFF, start = 0; uint64_t size = 0; };
    int Find(const std::wstring& path) const;                                    // -1 if missing
    Result<std::vector<uint8_t>> Chain(uint32_t start, uint64_t size, bool mini) const;
    ReadAt read_;
    uint64_t fileSize_ = 0;
    uint32_t sectorSize_ = 512, miniCutoff_ = 4096;
    std::vector<uint32_t> fat_, miniFat_;
    std::vector<Entry> dir_;
    std::vector<uint8_t> miniStream_;
};

struct RevitParam { std::wstring group, name, value; };
struct RevitType { std::wstring name; std::vector<RevitParam> params; };
struct RevitLink { std::wstring type, path, state; };

struct RevitInfo {
    enum class Kind { Project, ProjectTemplate, Family, FamilyTemplate } kind = Kind::Project;
    std::wstring version;            // "2023"
    std::wstring build;              // "20220726_1500(x64)"
    std::wstring lastSavePath, centralPath, locale;
    bool workshared = false;
    std::wstring title, category, updated;          // from the Atom XML ("2024-01-31T00:14:32Z")
    std::vector<uint8_t> thumbnailPng;              // empty if none
    std::vector<RevitParam> params;                 // project information / family parameters
    std::vector<RevitType> types;                   // families
    std::vector<RevitLink> links;
    std::vector<std::wstring> notes;                // what could not be read
};

// Reads a Revit file through `readAt` (random access). `fileName` decides the kind by extension.
Result<RevitInfo> ReadRevit(CompoundFile::ReadAt readAt, uint64_t fileSize, const std::wstring& fileName);
Result<RevitInfo> ReadRevitFile(const std::wstring& path, const std::wstring& fileName);

// The info card as RTF for the read-only rich text view (picture, tables, Arabic text).
std::string RevitToRtf(const RevitInfo& info, const std::wstring& fileName);

} // namespace mzr::preview
