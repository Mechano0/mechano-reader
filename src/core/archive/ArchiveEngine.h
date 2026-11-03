#pragma once
// Archive Engine: the ONLY module that talks to 7z.dll.
//
//   auto arc = Archive::Open(path, {}, pwProvider, cancel);
//   arc.value()->Index()           -> immutable tree for the UI
//   arc.value()->Extract(...)      -> every write passes the Security Layer
//   arc.value()->Update(...)       -> add / delete / rename, atomic replace
//   CreateArchive(...)             -> new zip / 7z
//
// Thread rule: one Archive object is used by ONE worker thread at a time
// (7-Zip handlers are not thread-safe). The ArchiveIndex it hands out is
// immutable and can be read from any thread.
#include <memory>
#include <string>
#include <vector>
#include "core/archive/ArchiveIndex.h"
#include "core/archive/ArchiveTypes.h"
#include "core/security/ExtractGuard.h"
#include "utils/Result.h"

namespace mzr::archive {

// Loads 7z.dll from the application folder (never from PATH / current dir).
// Safe to call many times. Returns false + message if the DLL is missing/broken.
bool LoadEngine(std::wstring* error = nullptr);
std::wstring EngineVersion();

struct OpenOptions {
    uint32_t codePage = 0;          // 0 = automatic (UTF-8 flag or OEM). F12: 720 / 864 / 1256 / 437 ...
    uint64_t maxEntries = 2'000'000; // "huge metadata" protection
    std::wstring onlyFormat;         // e.g. L"zip": try only this handler (Office files are ZIP)
};

struct UpdatePlan {
    std::vector<SourceItem> add;                              // new / replacing files
    std::vector<uint32_t>   remove;                           // archive indices to delete
    std::vector<std::pair<uint32_t, std::wstring>> rename;    // archive index -> new full path
};

class Archive {
public:
    static Result<std::unique_ptr<Archive>> Open(const std::wstring& path, const OpenOptions& options,
                                                 PasswordProvider passwords, CancelToken* cancel = nullptr);
    // Open an archive that is already in RAM (a DOCX/XLSX/PPTX read from another
    // archive for Preview). Multi-volume and passwords are not supported here.
    static Result<std::unique_ptr<Archive>> OpenMemory(std::shared_ptr<const std::vector<uint8_t>> data,
                                                       const std::wstring& name, const OpenOptions& options);
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    std::shared_ptr<const ArchiveIndex> Index() const { return index_; }
    const std::wstring& Path() const { return path_; }

    // arcIndices empty = everything.
    OperationReport Extract(const std::vector<uint32_t>& arcIndices, const ExtractOptions& options,
                            ProgressSink* progress, CancelToken& cancel,
                            const security::BombLimits& limits = {});

    // Decode every item and verify CRC without writing anything.
    OperationReport Test(ProgressSink* progress, CancelToken& cancel);

    // Decode ONE file into RAM for Preview (never touches the disk). Stops with
    // TooLarge as soon as more than maxBytes come out (bomb-safe). Encrypted
    // files only work if the password is already known (no prompt here).
    // With `truncated` set, a bigger file returns its first maxBytes instead (text preview).
    Result<std::vector<uint8_t>> ReadEntry(uint32_t arcIndex, uint64_t maxBytes, CancelToken& cancel,
                                           bool* truncated = nullptr);

    // Writes a new archive next to the original, then atomically replaces it
    // and re-opens. On any failure the original file is untouched.
    Result<bool> Update(const UpdatePlan& plan, const CompressionSettings& settings,
                        ProgressSink* progress, CancelToken& cancel, OperationReport* report);

    struct Impl;
private:
    Archive() = default;
    // stream = IInStream* (7-Zip type, kept out of this header).
    static Result<std::unique_ptr<Archive>> OpenFrom(void* stream, const std::wstring& path, const OpenOptions& options,
                                                     PasswordProvider passwords, CancelToken* cancel);
    std::unique_ptr<Impl> impl_;
    std::wstring path_;
    std::shared_ptr<const ArchiveIndex> index_;
};

// Create a brand-new archive (format from settings.format: "zip" or "7z").
Result<bool> CreateArchive(const std::wstring& outPath, const std::vector<SourceItem>& sources,
                           const CompressionSettings& settings, ProgressSink* progress,
                           CancelToken& cancel, OperationReport* report);

// Formats we can write (name as 7-Zip calls it).
std::vector<std::wstring> WritableFormats();

} // namespace mzr::archive
