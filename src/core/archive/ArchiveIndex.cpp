#include "core/archive/ArchiveIndex.h"

#include <algorithm>
#include <unordered_map>

namespace mzr::archive {
namespace {

// Case-insensitive key (archives from Linux may contain "A" and "a"; we merge
// folders the way Windows would show them, files stay separate nodes).
std::wstring FoldKey(uint32_t parent, std::wstring_view name) {
    std::wstring k = std::to_wstring(parent);
    k += L'|';
    k.append(name);
    CharUpperBuffW(k.data(), static_cast<DWORD>(k.size()));
    return k;
}

} // namespace

std::shared_ptr<const ArchiveIndex> ArchiveIndex::Build(ArchiveInfo info, std::vector<EntryRecord> entries) {
    auto idx = std::make_shared<ArchiveIndex>();
    idx->info_ = std::move(info);
    idx->entries_ = std::move(entries);
    auto& nodes = idx->nodes_;
    nodes.reserve(idx->entries_.size() + 16);
    nodes.push_back(Node{NodeName{}, kRoot, kNoEntry, true});

    std::unordered_map<std::wstring, uint32_t> folders;   // FoldKey -> node
    folders.reserve(idx->entries_.size() / 4 + 16);

    // The name points into the entry path when it is its last part (NUL right after it);
    // otherwise one copy goes into the pool. entries_ is never resized after this point.
    auto& pool = idx->namePool_;
    auto nameOf = [&](std::wstring_view comp, const std::wstring& owner) -> NodeName {
        if (comp.data() + comp.size() == owner.data() + owner.size()) return NodeName(comp.data(), comp.size());
        pool.emplace_back(comp);
        return NodeName(pool.back().c_str(), pool.back().size());
    };
    const std::wstring* curPath = nullptr;

    auto getFolder = [&](uint32_t parent, std::wstring_view name) -> uint32_t {
        const auto key = FoldKey(parent, name);
        if (auto it = folders.find(key); it != folders.end()) return it->second;
        const auto id = static_cast<uint32_t>(nodes.size());
        nodes.push_back(Node{nameOf(name, *curPath), parent, kNoEntry, true});
        nodes[parent].children.push_back(id);
        folders.emplace(key, id);
        return id;
    };

    for (uint32_t e = 0; e < idx->entries_.size(); ++e) {
        const auto& rec = idx->entries_[e];
        if (rec.isAltStream) continue;                         // NTFS streams are not shown
        // Split on both separators; drop empty, "." components. ".." is kept as a
        // visible name here (display only); extraction re-validates everything.
        std::vector<std::wstring_view> parts;
        curPath = &rec.path;
        std::wstring_view p = rec.path;
        size_t s = 0;
        while (s <= p.size()) {
            size_t t = p.find_first_of(L"\\/", s);
            if (t == std::wstring_view::npos) t = p.size();
            auto c = p.substr(s, t - s);
            if (!c.empty() && c != L".") parts.push_back(c);
            s = t + 1;
        }
        if (parts.empty()) continue;

        uint32_t parent = kRoot;
        for (size_t i = 0; i + 1 < parts.size(); ++i) parent = getFolder(parent, parts[i]);

        if (rec.isDir) {
            const uint32_t id = getFolder(parent, parts.back());
            nodes[id].entry = e;                                // real folder entry (has date/attrib)
        } else {
            const auto id = static_cast<uint32_t>(nodes.size());
            Node n{nameOf(parts.back(), rec.path), parent, e, false};
            n.size = rec.size;
            n.packSize = rec.packSize;
            n.fileCount = 1;
            nodes.push_back(std::move(n));
            nodes[parent].children.push_back(id);
        }
    }

    // Folder totals: children always have a larger id than their parent, so a
    // single reverse pass accumulates bottom-up.
    for (size_t i = nodes.size(); i-- > 1;) {
        auto& n = nodes[i];
        auto& par = nodes[n.parent];
        par.size += n.size;
        par.packSize += n.packSize;
        par.fileCount += n.fileCount;
    }

    // Sort children: folders first, then natural-ish name order (case-insensitive, locale aware).
    for (auto& n : nodes) {
        if (n.children.size() < 2) continue;
        std::sort(n.children.begin(), n.children.end(), [&](uint32_t a, uint32_t b) {
            const auto& A = nodes[a];
            const auto& B = nodes[b];
            if (A.isDir != B.isDir) return A.isDir;
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                   A.name.c_str(), static_cast<int>(A.name.size()),
                                   B.name.c_str(), static_cast<int>(B.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
    }
    return idx;
}

std::wstring ArchiveIndex::PathOf(uint32_t node) const {
    std::vector<const NodeName*> parts;
    for (uint32_t n = node; n != kRoot; n = nodes_[n].parent) parts.push_back(&nodes_[n].name);
    std::wstring out;
    for (size_t i = parts.size(); i-- > 0;) {
        out.append(parts[i]->data(), parts[i]->size());
        if (i) out += L'\\';
    }
    return out;
}

uint32_t ArchiveIndex::FindFolder(const std::wstring& path) const {
    uint32_t cur = kRoot;
    size_t s = 0;
    while (s < path.size()) {
        size_t t = path.find(L'\\', s);
        if (t == std::wstring::npos) t = path.size();
        const std::wstring_view comp(path.data() + s, t - s);
        s = t + 1;
        if (comp.empty()) continue;
        uint32_t found = kNoEntry;
        for (uint32_t c : nodes_[cur].children) {
            const auto& n = nodes_[c];
            if (n.isDir && CompareStringOrdinal(n.name.c_str(), static_cast<int>(n.name.size()),
                                                comp.data(), static_cast<int>(comp.size()), TRUE) == CSTR_EQUAL) {
                found = c;
                break;
            }
        }
        if (found == kNoEntry) return kNoEntry;
        cur = found;
    }
    return cur;
}

std::vector<uint32_t> ArchiveIndex::CollectArcIndices(const std::vector<uint32_t>& start) const {
    std::vector<uint32_t> out;
    std::vector<uint32_t> stack(start.begin(), start.end());
    while (!stack.empty()) {
        const uint32_t n = stack.back();
        stack.pop_back();
        const auto& node = nodes_[n];
        if (node.entry != kNoEntry) out.push_back(entries_[node.entry].arcIndex);
        for (uint32_t c : node.children) stack.push_back(c);
    }
    std::sort(out.begin(), out.end());               // 7-Zip requires sorted indices
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace mzr::archive
