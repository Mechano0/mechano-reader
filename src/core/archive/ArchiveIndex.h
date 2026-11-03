#pragma once
// ArchiveIndex: an immutable folder tree built once from the flat entry list.
// The UI only ever reads it (shared_ptr<const ArchiveIndex>), so it can be
// used from the UI thread while the worker keeps the 7-Zip handler.
//
// Memory (Phase 6): a file's name is NOT copied - it points at the end of its
// entry path, which is already in RAM. Only folder names that are not at the end
// of a path are stored once in a small pool. Method names are stored once per
// archive (ArchiveInfo::methods). The index is never copied after Build.
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "core/archive/ArchiveTypes.h"

namespace mzr::archive {

// Read-only, NUL-terminated name owned by the ArchiveIndex (12 bytes instead of a
// 32-byte std::wstring + its own heap block). Converts to std::wstring when needed.
class NodeName {
public:
    NodeName() = default;
    NodeName(const wchar_t* p, size_t n) : p_(p), n_(static_cast<uint32_t>(n)) {}
    const wchar_t* c_str() const { return p_; }
    const wchar_t* data() const { return p_; }
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    std::wstring_view view() const { return {p_, n_}; }
    operator std::wstring_view() const { return view(); }        // NOLINT: implicit on purpose
    operator std::wstring() const { return std::wstring(p_, n_); }   // NOLINT
    size_t find_last_of(wchar_t c) const { return view().find_last_of(c); }
    friend bool operator==(const NodeName& a, std::wstring_view b) { return a.view() == b; }
    friend bool operator==(std::wstring_view a, const NodeName& b) { return a == b.view(); }
    friend std::wstring operator+(const std::wstring& a, const NodeName& b) { std::wstring s(a); s.append(b.p_, b.n_); return s; }
    friend std::wstring operator+(const wchar_t* a, const NodeName& b) { std::wstring s(a); s.append(b.p_, b.n_); return s; }
    friend std::wstring operator+(const NodeName& a, const std::wstring& b) { std::wstring s(a.p_, a.n_); s += b; return s; }
    friend std::wstring operator+(const NodeName& a, const wchar_t* b) { std::wstring s(a.p_, a.n_); s += b; return s; }
private:
    const wchar_t* p_ = L"";
    uint32_t n_ = 0;
};

class ArchiveIndex {
public:
    static constexpr uint32_t kNoEntry = 0xFFFFFFFFu;
    static constexpr uint32_t kRoot = 0;

    ArchiveIndex() = default;
    ArchiveIndex(const ArchiveIndex&) = delete;              // names point into this object
    ArchiveIndex& operator=(const ArchiveIndex&) = delete;

    struct Node {
        NodeName name;                // display name (one path component)
        uint32_t parent = kRoot;
        uint32_t entry = kNoEntry;    // index into entries(), kNoEntry for implied folders
        bool     isDir = false;
        uint64_t size = 0;            // folders: sum of contents
        uint64_t packSize = 0;
        uint32_t fileCount = 0;       // folders: files below (recursive)
        std::vector<uint32_t> children;   // folders only, sorted: folders first, then name
    };

    static std::shared_ptr<const ArchiveIndex> Build(ArchiveInfo info, std::vector<EntryRecord> entries);

    const ArchiveInfo&              info() const { return info_; }
    const std::vector<EntryRecord>& entries() const { return entries_; }
    const std::vector<Node>&        nodes() const { return nodes_; }
    const Node&                     node(uint32_t i) const { return nodes_[i]; }

    // Path of a node inside the archive, '\' separated ("docs\a.txt"). Root = "".
    std::wstring PathOf(uint32_t node) const;
    // Find a folder node by its '\' path. Returns kNoEntry if missing.
    uint32_t FindFolder(const std::wstring& path) const;
    // All archive indices (sorted) of a node and everything below it.
    std::vector<uint32_t> CollectArcIndices(const std::vector<uint32_t>& nodes) const;

    uint64_t totalFiles() const { return nodes_[kRoot].fileCount; }
    uint64_t totalSize() const { return nodes_[kRoot].size; }
    uint64_t totalPacked() const { return nodes_[kRoot].packSize; }

private:
    ArchiveInfo              info_;
    std::vector<EntryRecord> entries_;
    std::vector<Node>        nodes_;
    std::deque<std::wstring> namePool_;   // folder names that are not the end of a path (deque: never moves)
};

} // namespace mzr::archive
