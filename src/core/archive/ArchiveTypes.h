#pragma once
// Plain data types shared by the Archive Engine, workers and UI.
// No 7-Zip types leak out of core/archive/SevenZipEngine.cpp.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mzr::archive {

// One item exactly as the archive reports it (already decoded to UTF-16).
struct EntryRecord {
    uint32_t     arcIndex = 0;      // index inside the archive handler
    std::wstring path;              // raw path from the archive (untrusted!)
    bool         isDir = false;
    bool         encrypted = false;
    bool         isLink = false;    // symlink / hardlink / reparse point: never materialised
    bool         isAltStream = false;
    bool         hasCrc = false;
    uint32_t     crc = 0;
    uint32_t     attrib = 0;
    uint64_t     size = 0;
    uint64_t     packSize = 0;
    FILETIME     mtime{};
    uint16_t     method = 0;        // index into ArchiveInfo::methods ("Deflate", "LZMA2:24" ...), 0 = none
};

struct ArchiveInfo {
    std::wstring path;
    std::wstring formatName;        // "zip", "7z", "Rar5" ...
    bool         canUpdate = false; // format supports add/delete/rename
    bool         isSolid = false;
    bool         hasEncrypted = false;
    uint64_t     physicalSize = 0;
    std::wstring comment;
    std::vector<std::wstring> methods{std::wstring()};   // each method name stored once (Phase 6 memory)
    std::vector<std::wstring> warnings;   // "data after end", "headers error" ...
};

// Cooperative cancellation shared between UI (Esc) and the worker.
struct CancelToken {
    std::atomic<bool> cancelled{false};
    void Cancel() { cancelled = true; }
    bool IsCancelled() const { return cancelled.load(); }
};

// Progress sink. Called from the worker thread; implementation must be cheap
// and thread-safe (the UI version throttles and PostMessage()s).
struct ProgressSink {
    virtual ~ProgressSink() = default;
    virtual void OnTotal(uint64_t totalBytes) = 0;
    virtual void OnCompleted(uint64_t doneBytes) = 0;
    virtual void OnCurrentItem(const std::wstring& name) = 0;
};

// Asked when an encrypted item/header needs a password. Return nullopt = cancel.
using PasswordProvider = std::function<std::optional<std::wstring>(const std::wstring& archiveName)>;

enum class OverwriteMode { Overwrite, Skip, Rename };
enum class LinkMode { Skip, AsTextFile };

struct ExtractOptions {
    std::wstring  destination;            // absolute folder (created if missing)
    OverwriteMode overwrite = OverwriteMode::Rename;
    LinkMode      links = LinkMode::Skip;
    bool          propagateMotW = true;   // copy archive's Zone.Identifier to every file
    bool          restoreTimes = true;
    std::wstring  stripPrefix;            // path prefix removed from entries ("" = none)
};

// One problem with one item. The whole operation continues; the user sees a summary.
struct ItemIssue {
    std::wstring path;
    std::wstring friendly;
    std::wstring details;
};

struct OperationReport {
    uint64_t filesDone = 0;
    uint64_t foldersDone = 0;
    uint64_t bytesDone = 0;
    uint64_t skipped = 0;
    bool     cancelled = false;
    bool     wrongPassword = false;
    std::vector<ItemIssue> issues;
    bool Ok() const { return !cancelled && issues.empty(); }
};

enum class CompressionLevel : uint32_t { Store = 0, Fastest = 1, Fast = 3, Normal = 5, Maximum = 9 };

struct CompressionSettings {
    std::wstring     format = L"zip";      // "zip" or "7z" for writing
    CompressionLevel level  = CompressionLevel::Normal;
    uint32_t         threads = 0;          // 0 = automatic
    std::wstring     password;             // empty = no encryption (never logged)
    bool             encryptHeaders = false; // 7z only: hides file names
    uint64_t         volumeSize = 0;       // 0 = single file (split = Phase 5, F16)
};

// A file or folder on disk that goes into an archive.
struct SourceItem {
    std::wstring diskPath;      // absolute path on disk
    std::wstring archivePath;   // path inside the archive, '\' separated, relative
    bool         isDir = false;
    uint64_t     size = 0;
    uint32_t     attrib = 0;
    FILETIME     mtime{};
};

} // namespace mzr::archive
