#include "core/preview/Revit.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <pugixml.hpp>
#include "core/archive/ArchiveEngine.h"

namespace mzr::preview {
namespace {

constexpr uint32_t kEnd = 0xFFFFFFFE, kFree = 0xFFFFFFFF, kMaxRegular = 0xFFFFFFFA;
constexpr uint64_t kMaxInfoStream = 8ull << 20;      // every stream we read is small; bigger = refused

ErrorInfo Bad(const std::wstring& why) {
    return ErrorInfo{ErrorCode::Corrupted, L"This Revit file could not be read (it may be damaged or not a Revit file).", why};
}
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32; }

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

// ============================================================ compound file

Result<CompoundFile> CompoundFile::Open(ReadAt readAt, uint64_t fileSize) {
    CompoundFile cf;
    cf.read_ = std::move(readAt);
    cf.fileSize_ = fileSize;
    uint8_t h[512];
    if (fileSize < 1024 || !cf.read_(0, h, sizeof(h))) return Bad(L"too small");
    static const uint8_t kSig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (memcmp(h, kSig, 8) != 0) return Bad(L"not an OLE compound file");
    const uint16_t shift = U16(h + 0x1E);
    if (shift != 9 && shift != 12) return Bad(L"bad sector size");
    cf.sectorSize_ = 1u << shift;
    if (U16(h + 0x20) != 6) return Bad(L"bad mini sector size");
    const uint32_t numFat = U32(h + 0x2C), firstDir = U32(h + 0x30), firstMiniFat = U32(h + 0x3C), numMiniFat = U32(h + 0x40);
    const uint32_t firstDifat = U32(h + 0x44), numDifat = U32(h + 0x48);
    cf.miniCutoff_ = U32(h + 0x38);
    if (cf.miniCutoff_ != 4096) cf.miniCutoff_ = 4096;
    const uint64_t sectors = fileSize / cf.sectorSize_;                   // upper bound for any index
    if (numFat == 0 || numFat > sectors) return Bad(L"bad FAT count");

    // FAT sector list: 109 in the header, then the DIFAT chain.
    std::vector<uint32_t> fatSectors;
    for (int i = 0; i < 109 && fatSectors.size() < numFat; ++i) fatSectors.push_back(U32(h + 0x4C + i * 4));
    uint32_t d = firstDifat;
    std::vector<uint8_t> sec(cf.sectorSize_);
    for (uint32_t k = 0; k < numDifat && fatSectors.size() < numFat; ++k) {
        if (d > kMaxRegular || d >= sectors) return Bad(L"bad DIFAT chain");
        if (!cf.read_((uint64_t(d) + 1) * cf.sectorSize_, sec.data(), sec.size())) return Bad(L"read DIFAT");
        const uint32_t per = cf.sectorSize_ / 4 - 1;
        for (uint32_t i = 0; i < per && fatSectors.size() < numFat; ++i) fatSectors.push_back(U32(sec.data() + i * 4));
        d = U32(sec.data() + per * 4);
    }
    if (fatSectors.size() < numFat) return Bad(L"FAT list too short");
    try { cf.fat_.reserve(static_cast<size_t>(numFat) * (cf.sectorSize_ / 4)); } catch (...) { return Bad(L"FAT too big"); }
    for (uint32_t s : fatSectors) {
        if (s > kMaxRegular || s >= sectors) return Bad(L"bad FAT sector");
        if (!cf.read_((uint64_t(s) + 1) * cf.sectorSize_, sec.data(), sec.size())) return Bad(L"read FAT");
        for (uint32_t i = 0; i < cf.sectorSize_ / 4; ++i) cf.fat_.push_back(U32(sec.data() + i * 4));
    }

    // Directory.
    auto dirBytes = cf.Chain(firstDir, 0, false);
    if (!dirBytes) return dirBytes.error();
    const auto& db = dirBytes.value();
    for (size_t off = 0; off + 128 <= db.size() && cf.dir_.size() < 200000; off += 128) {
        const uint8_t* e = db.data() + off;
        Entry en;
        const uint16_t nameBytes = std::min<uint16_t>(U16(e + 0x40), 64);
        for (int i = 0; i + 1 < nameBytes - 1 && i < 62; i += 2) en.name += static_cast<wchar_t>(U16(e + i));
        en.type = e[0x42];
        en.left = U32(e + 0x44); en.right = U32(e + 0x48); en.child = U32(e + 0x4C);
        en.start = U32(e + 0x74);
        en.size = cf.sectorSize_ == 512 ? U32(e + 0x78) : U64(e + 0x78);
        cf.dir_.push_back(std::move(en));
    }
    if (cf.dir_.empty() || cf.dir_[0].type != 5) return Bad(L"no root entry");

    // Mini FAT (small streams live in the root entry's "mini stream").
    if (numMiniFat && firstMiniFat <= kMaxRegular) {
        auto mf = cf.Chain(firstMiniFat, uint64_t(numMiniFat) * cf.sectorSize_, false);
        if (mf) for (size_t i = 0; i + 4 <= mf.value().size(); i += 4) cf.miniFat_.push_back(U32(mf.value().data() + i));
    }
    const Entry& root = cf.dir_[0];
    if (root.size && root.size <= (256ull << 20)) {                      // only the small streams use it
        auto ms = cf.Chain(root.start, root.size, false);
        if (ms) cf.miniStream_ = std::move(ms.value());
    }
    return cf;
}

// Follows a sector chain. size 0 = whole chain (directory). Every step is checked: index in range,
// at most as many steps as there are FAT entries (no loops), never more than the asked size.
Result<std::vector<uint8_t>> CompoundFile::Chain(uint32_t start, uint64_t size, bool mini) const {
    std::vector<uint8_t> out;
    const auto& fat = mini ? miniFat_ : fat_;
    const uint32_t unit = mini ? 64 : sectorSize_;
    const uint64_t limit = size ? size : (64ull << 20);
    try { out.reserve(static_cast<size_t>(std::min<uint64_t>(limit, 16ull << 20))); } catch (...) {}
    uint32_t s = start;
    size_t steps = 0;
    std::vector<uint8_t> buf(unit);
    while (s != kEnd && out.size() < limit) {
        if (s > kMaxRegular || s >= fat.size() || ++steps > fat.size()) return Bad(L"bad sector chain");
        if (mini) {
            const uint64_t pos = uint64_t(s) * 64;
            if (pos + 64 > miniStream_.size()) return Bad(L"mini sector outside the mini stream");
            out.insert(out.end(), miniStream_.begin() + static_cast<ptrdiff_t>(pos), miniStream_.begin() + static_cast<ptrdiff_t>(pos + 64));
        } else {
            const uint64_t off = (uint64_t(s) + 1) * sectorSize_;
            if (off + sectorSize_ > fileSize_ + sectorSize_) return Bad(L"sector outside the file");
            std::fill(buf.begin(), buf.end(), uint8_t(0));
            if (!read_(off, buf.data(), static_cast<size_t>(std::min<uint64_t>(sectorSize_, fileSize_ - std::min(off, fileSize_))))) return Bad(L"read error");
            out.insert(out.end(), buf.begin(), buf.end());
        }
        s = fat[s];
    }
    if (size && out.size() < size) return Bad(L"stream shorter than its size");
    if (size) out.resize(static_cast<size_t>(size));
    return out;
}

int CompoundFile::Find(const std::wstring& path) const {
    if (dir_.empty()) return -1;
    uint32_t parent = 0;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find(L'/', pos);
        const std::wstring part = path.substr(pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos);
        pos = slash == std::wstring::npos ? path.size() + 1 : slash + 1;
        // Children of `parent` form a red-black tree: walk it without recursion, never twice.
        std::vector<uint32_t> stack{dir_[parent].child};
        std::vector<char> seen(dir_.size(), 0);
        int found = -1;
        while (!stack.empty() && found < 0) {
            const uint32_t i = stack.back(); stack.pop_back();
            if (i >= dir_.size() || seen[i]) continue;
            seen[i] = 1;
            if (SameName(dir_[i].name, part)) { found = static_cast<int>(i); break; }
            stack.push_back(dir_[i].left);
            stack.push_back(dir_[i].right);
        }
        if (found < 0) return -1;
        parent = static_cast<uint32_t>(found);
    }
    return static_cast<int>(parent);
}

bool CompoundFile::Has(const std::wstring& path) const { const int i = Find(path); return i > 0 && dir_[static_cast<size_t>(i)].type == 2; }

Result<std::vector<uint8_t>> CompoundFile::Read(const std::wstring& path, uint64_t maxBytes) const {
    const int i = Find(path);
    if (i <= 0 || dir_[static_cast<size_t>(i)].type != 2) return ErrorInfo{ErrorCode::Corrupted, L"Missing part.", path};
    const Entry& e = dir_[static_cast<size_t>(i)];
    if (e.size > maxBytes) return ErrorInfo{ErrorCode::SizeLimitExceeded, L"Part too large.", path};
    if (e.size == 0) return std::vector<uint8_t>();
    return Chain(e.start, e.size, e.size < miniCutoff_);
}

std::vector<std::wstring> CompoundFile::StreamNames() const {
    std::vector<std::wstring> out;
    if (dir_.empty()) return out;
    std::vector<uint32_t> stack{dir_[0].child};
    std::vector<char> seen(dir_.size(), 0);
    while (!stack.empty() && out.size() < 1000) {
        const uint32_t i = stack.back(); stack.pop_back();
        if (i >= dir_.size() || seen[i]) continue;
        seen[i] = 1;
        out.push_back(dir_[i].name + (dir_[i].type == 1 ? L"/" : L""));
        stack.push_back(dir_[i].left);
        stack.push_back(dir_[i].right);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ============================================================ Revit streams

namespace {

std::wstring W8(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}
const char* LocalName(const char* n) { const char* c = strchr(n, ':'); return c ? c + 1 : n; }
bool IsEl(const pugi::xml_node& n, const char* local) { return n.type() == pugi::node_element && strcmp(LocalName(n.name()), local) == 0; }
pugi::xml_node Kid(const pugi::xml_node& n, const char* local) { for (auto c : n.children()) if (IsEl(c, local)) return c; return {}; }

bool LoadXml(pugi::xml_document& d, const std::vector<uint8_t>& b) {
    if (b.empty()) return false;
    // TransmissionData is a 4-byte character count + UTF-16 XML; PartAtom / project info are plain UTF-8.
    const bool utf16 = b.size() > 8 && b[4] == '<' && b[5] == 0;
    const uint8_t* p = utf16 ? b.data() + 4 : b.data();
    const size_t len = utf16 ? (b.size() - 4) & ~size_t(1) : b.size();
    if (d.load_buffer(p, len, pugi::parse_default, utf16 ? pugi::encoding_utf16_le : pugi::encoding_auto).status != pugi::status_ok) return false;
    // Depth check without recursion (same rule as the Office preview: deeper than 128 = refused).
    size_t depth = 1;
    for (pugi::xml_node n = d.first_child(); n;) {
        if (depth > 128) { d.reset(); return false; }
        if (auto c = n.first_child()) { n = c; ++depth; continue; }
        while (n != d && !n.next_sibling()) { n = n.parent(); --depth; }
        if (n == d) break;
        n = n.next_sibling();
    }
    return true;
}

std::wstring Trimmed(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (iswspace(s[i]) || s[i] == 0x200F || s[i] == 0x200E)) ++i;   // incl. direction marks
    return s.substr(i);
}

std::wstring Pretty(const char* xmlName, const char* displayName) {
    std::wstring s = *displayName ? W8(displayName) : W8(LocalName(xmlName));
    for (auto& c : s) if (c == L'_') c = L' ';
    return s;
}

// Parameters of one Atom node: <Name displayName=".." type="..">value</Name>
void ReadParams(const pugi::xml_node& n, const std::wstring& group, std::vector<RevitParam>& out) {
    for (auto p : n.children()) {
        if (p.type() != pugi::node_element || IsEl(p, "title") || out.size() >= 2000) continue;
        out.push_back({group, Pretty(p.name(), p.attribute("displayName").value()), Trimmed(W8(p.text().get()))});
    }
}

void ReadAtom(const std::vector<uint8_t>& b, RevitInfo& info) {
    pugi::xml_document d;
    if (!LoadXml(d, b)) { info.notes.push_back(L"The project / family description could not be read."); return; }
    auto entry = Kid(d, "entry");
    if (auto t = Kid(entry, "title")) info.title = Trimmed(W8(t.text().get()));
    if (auto u = Kid(entry, "updated")) info.updated = W8(u.text().get());
    if (auto c = Kid(entry, "category")) if (auto t = Kid(c, "term")) info.category = W8(t.text().get());
    if (auto link = Kid(entry, "link")) if (auto df = Kid(link, "design-file")) if (auto v = Kid(df, "product-version")) {
        if (info.version.empty()) info.version = W8(v.text().get());
    }
    for (auto f : Kid(entry, "features").children()) {
        if (!IsEl(f, "feature")) continue;
        for (auto g : f.children()) {
            if (!IsEl(g, "group")) continue;
            ReadParams(g, Trimmed(W8(Kid(g, "title").text().get())), info.params);
        }
    }
    if (auto fam = Kid(entry, "family")) {
        for (auto part : fam.children()) {
            if (!IsEl(part, "part") || info.types.size() >= 1000) continue;
            RevitType t;
            t.name = Trimmed(W8(Kid(part, "title").text().get()));
            ReadParams(part, L"", t.params);
            info.types.push_back(std::move(t));
        }
    }
}

// BasicFileInfo: length-prefixed UTF-16 strings (2016+), or "Key: value" lines (older files).
void ReadBasicInfo(const std::vector<uint8_t>& b, RevitInfo& info) {
    struct S { size_t at; std::wstring s; };
    std::vector<S> strs;
    for (size_t i = 0; i + 4 <= b.size() && strs.size() < 300;) {
        const uint32_t n = U32(b.data() + i);
        if (n > 0 && n < 2000 && i + 4 + size_t(n) * 2 <= b.size()) {
            std::wstring s(n, L'\0');
            bool ok = true;
            for (uint32_t k = 0; k < n; ++k) {
                const wchar_t c = static_cast<wchar_t>(U16(b.data() + i + 4 + k * 2));
                if (c < 0x20 && c != L'\r' && c != L'\n' && c != L'\t') { ok = false; break; }
                s[k] = c;
            }
            if (ok) { strs.push_back({i, s}); i += 4 + size_t(n) * 2; continue; }
        }
        ++i;
    }
    auto isPath = [](const std::wstring& s) {
        if (s.size() < 5) return false;
        std::wstring low = s; for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        const bool ext = low.ends_with(L".rvt") || low.ends_with(L".rfa") || low.ends_with(L".rte") || low.ends_with(L".rft");
        return ext && (low.find(L":\\") != std::wstring::npos || low.rfind(L"\\\\", 0) == 0 || low.rfind(L"rsn://", 0) == 0 || low.rfind(L"autodesk docs", 0) == 0);
    };
    size_t versionAt = SIZE_MAX;
    for (const auto& s : strs) {
        if (info.version.empty() && s.s.size() == 4 && iswdigit(s.s[0]) && s.s[0] == L'2') { info.version = s.s; versionAt = s.at; }
        else if (info.build.empty() && s.s.size() >= 13 && iswdigit(s.s[0]) && s.s[8] == L'_') info.build = s.s;
        else if (info.locale.empty() && s.s.size() == 3 && iswupper(s.s[0]) && iswupper(s.s[1]) && iswupper(s.s[2])) info.locale = s.s;
    }
    info.workshared = b.size() > 5 && U32(b.data()) >= 10 && b[4] != 0;
    for (const auto& s : strs) {
        if (!isPath(s.s)) continue;
        if (info.workshared && s.at < versionAt && info.centralPath.empty()) info.centralPath = s.s;
        else if (info.lastSavePath.empty() && s.at > versionAt) info.lastSavePath = s.s;
    }
    if (info.lastSavePath.empty()) for (const auto& s : strs) if (isPath(s.s)) { info.lastSavePath = s.s; break; }
    // Older files: one UTF-16 text block with "Format: Autodesk Revit 2015", "Build: ...", ...
    if (info.version.empty() && b.size() >= 2) {
        std::wstring all(b.size() / 2, L'\0');
        for (size_t k = 0; k < all.size(); ++k) all[k] = static_cast<wchar_t>(U16(b.data() + k * 2));
        auto field = [&](const wchar_t* key) {
            const size_t p = all.find(key);
            if (p == std::wstring::npos) return std::wstring();
            size_t e = all.find_first_of(L"\r\n", p);
            return Trimmed(all.substr(p + wcslen(key), (e == std::wstring::npos ? all.size() : e) - p - wcslen(key)));
        };
        std::wstring f = field(L"Format:");
        const size_t y = f.find(L"20");
        if (y != std::wstring::npos && y + 4 <= f.size()) info.version = f.substr(y, 4);
        if (info.build.empty()) info.build = field(L"Build:");
        if (info.lastSavePath.empty()) info.lastSavePath = field(L"Last Save Path:");
        if (info.centralPath.empty()) info.centralPath = field(L"Central Model Path:");
        const std::wstring ws = field(L"Worksharing:");
        if (!ws.empty()) info.workshared = ws.find(L"Not enabled") == std::wstring::npos;
    }
}

void ReadLinks(const std::vector<uint8_t>& b, RevitInfo& info) {
    pugi::xml_document d;
    if (!LoadXml(d, b)) return;
    for (auto r : Kid(d, "TransmissionData").children()) {
        if (!IsEl(r, "ExternalFileReference") || info.links.size() >= 2000) continue;
        RevitLink l;
        l.type = W8(Kid(r, "ExternalFileReferenceType").text().get());
        l.path = W8(Kid(r, "LastSavedPath").text().get());
        if (l.path.empty()) l.path = W8(Kid(r, "DesiredPath").text().get());
        l.state = W8(Kid(r, "LastSavedLoadState").text().get());
        if (l.path.empty() && l.state == L"Not Found") continue;           // empty placeholders (families)
        info.links.push_back(std::move(l));
    }
}

} // namespace

Result<RevitInfo> ReadRevit(CompoundFile::ReadAt readAt, uint64_t fileSize, const std::wstring& fileName) {
    auto cfr = CompoundFile::Open(std::move(readAt), fileSize);
    if (!cfr) return cfr.error();
    const CompoundFile& cf = cfr.value();
    if (!cf.Has(L"BasicFileInfo")) return Bad(L"no BasicFileInfo (not a Revit file)");
    RevitInfo info;
    std::wstring low = fileName;
    for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
    info.kind = low.ends_with(L".rfa") ? RevitInfo::Kind::Family : low.ends_with(L".rft") ? RevitInfo::Kind::FamilyTemplate
              : low.ends_with(L".rte") ? RevitInfo::Kind::ProjectTemplate : RevitInfo::Kind::Project;
    try {
        if (auto b = cf.Read(L"BasicFileInfo", kMaxInfoStream)) ReadBasicInfo(b.value(), info);
        if (auto p = cf.Read(L"RevitPreview4.0", kMaxInfoStream)) {
            const auto& v = p.value();
            static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            const auto it = std::search(v.begin(), v.begin() + static_cast<ptrdiff_t>(std::min<size_t>(v.size(), 65536)), kPng, kPng + 8);
            if (it != v.begin() + static_cast<ptrdiff_t>(std::min<size_t>(v.size(), 65536))) info.thumbnailPng.assign(it, v.end());
        }
        if (cf.Has(L"PartAtom")) {
            if (auto a = cf.Read(L"PartAtom", kMaxInfoStream)) ReadAtom(a.value(), info);
            if (info.kind == RevitInfo::Kind::Project) info.kind = RevitInfo::Kind::Family;
        } else if (cf.Has(L"ProjectInformation")) {
            // A small ZIP with one Atom XML file: read it with our own archive engine, in RAM.
            if (auto z = cf.Read(L"ProjectInformation", kMaxInfoStream)) {
                auto data = std::make_shared<const std::vector<uint8_t>>(std::move(z.value()));
                archive::OpenOptions oo;
                oo.onlyFormat = L"zip";
                oo.maxEntries = 50;
                auto arc = archive::Archive::OpenMemory(data, L"projectinfo.zip", oo);
                if (arc) {
                    archive::CancelToken none;
                    for (const auto& e : arc.value()->Index()->entries()) {
                        if (e.isDir || e.size > kMaxInfoStream) continue;
                        auto x = arc.value()->ReadEntry(e.arcIndex, kMaxInfoStream, none);
                        if (x) { ReadAtom(x.value(), info); break; }
                    }
                } else {
                    info.notes.push_back(L"Project information could not be read.");
                }
            }
        }
        if (auto t = cf.Read(L"TransmissionData", kMaxInfoStream)) ReadLinks(t.value(), info);
    } catch (const std::bad_alloc&) {
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"Not enough memory to read this Revit file.", L"bad_alloc"};
    }
    if (info.version.empty() && info.thumbnailPng.empty() && info.params.empty()) return Bad(L"nothing readable");
    return info;
}

Result<RevitInfo> ReadRevitFile(const std::wstring& path, const std::wstring& fileName) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ErrorInfo{ErrorCode::Io, L"The Revit file could not be opened.", L"CreateFile " + std::to_wstring(GetLastError())};
    std::shared_ptr<void> guard(h, CloseHandle);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    auto readAt = [h](uint64_t off, void* buf, size_t n) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, static_cast<DWORD>(n), &got, &ov) && got == n;
    };
    return ReadRevit(readAt, static_cast<uint64_t>(size.QuadPart), fileName);
}

// ============================================================ the card (RTF)

namespace {

void Esc(std::string& out, const std::wstring& s) {
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
        else if (c == L'\r') continue;
        else if (c == L'\n') out += "\\line ";
        else if (c == L'\t') out += "\\tab ";
        else if (c < 0x20) continue;
        else if (c < 0x80) out += static_cast<char>(c);
        else { out += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?"; }
    }
}

std::wstring KindText(RevitInfo::Kind k) {
    switch (k) {
    case RevitInfo::Kind::Family: return L"family";
    case RevitInfo::Kind::FamilyTemplate: return L"family template";
    case RevitInfo::Kind::ProjectTemplate: return L"project template";
    default: return L"project";
    }
}

std::wstring Date(const std::wstring& atom) {             // "2024-01-31T00:14:32Z" -> "2024-01-31 00:14"
    if (atom.size() < 16) return atom;
    return atom.substr(0, 10) + L" " + atom.substr(11, 5);
}

struct Table {
    std::string& rtf;
    std::vector<int> widths;                                  // twips
    void Row(const std::vector<std::wstring>& cells, bool header = false, int highlight = -1) {
        rtf += "\\trowd\\trgaph90\\trleft0";
        int x = 0;
        for (int w : widths) {
            x += w;
            rtf += "\\clbrdrb\\brdrs\\brdrw5\\brdrcf2";
            if (header) rtf += "\\clcbpat3";
            rtf += "\\cellx" + std::to_string(x);
        }
        rtf += "\\pard\\intbl\\sb40\\sa40 ";    // the space ends the control word ("2024 ..." must not become \sa402024)
        for (size_t i = 0; i < widths.size(); ++i) {
            const std::wstring v = i < cells.size() ? cells[i] : L"";
            if (header) rtf += "\\b ";
            if (static_cast<int>(i) == highlight) rtf += "\\cf4 ";
            Esc(rtf, v.size() > 300 ? v.substr(0, 300) + L"\u2026" : v);
            if (static_cast<int>(i) == highlight) rtf += "\\cf0 ";
            if (header) rtf += "\\b0 ";
            rtf += "\\cell ";
        }
        rtf += "\\row\n";
    }
};

void Heading(std::string& rtf, const std::wstring& text) {
    rtf += "\\pard\\sb240\\sa80\\b\\fs24 ";
    Esc(rtf, text);
    rtf += "\\b0\\fs20\\par\n";
}

// Revit keeps only a 128 x 128 PNG ("RevitPreview4.0"); there is no bigger picture in the file. For the card it
// is enlarged once here, with WIC's best filter (high-quality cubic), to `size` px, given rounded corners on the
// white card and stored again as PNG, so the rich text view shows it 1:1 (its own stretching is blurry / blocky).
std::vector<uint8_t> ThumbnailTile(const std::vector<uint8_t>& png, UINT size) {
    using Microsoft::WRL::ComPtr;
    std::vector<uint8_t> out;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    ComPtr<IWICStream> in;
    if (FAILED(f->CreateStream(&in)) ||
        FAILED(in->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())))) return out;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(f->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame))) return out;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return out;
    const double k = static_cast<double>(size) / std::max(w, h);
    const UINT dw = std::max<UINT>(1, static_cast<UINT>(w * k + 0.5)), dh = std::max<UINT>(1, static_cast<UINT>(h * k + 0.5));
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(f->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(conv.Get(), dw, dh, WICBitmapInterpolationModeHighQualityCubic))) return out;
    std::vector<uint8_t> px(static_cast<size_t>(dw) * dh * 4);
    if (FAILED(sc->CopyPixels(nullptr, dw * 4, static_cast<UINT>(px.size()), px.data()))) return out;
    // Over white (the card), corners rounded with soft (anti-aliased) edges.
    const double r = std::max(6.0, size / 22.0);
    for (UINT y = 0; y < dh; ++y)
        for (UINT x = 0; x < dw; ++x) {
            const double cx = std::clamp(x + 0.5, r, dw - r), cy = std::clamp(y + 0.5, r, dh - r);
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double cover = std::clamp(r - d + 0.5, 0.0, 1.0);
            uint8_t* p = &px[(static_cast<size_t>(y) * dw + x) * 4];
            const double a = p[3] / 255.0 * cover;
            for (int c = 0; c < 3; ++c)                                   // premultiplied colour + white behind
                p[c] = static_cast<uint8_t>(std::clamp(p[c] * cover + 255.0 * (1.0 - a), 0.0, 255.0));
            p[3] = 255;
        }
    ComPtr<IWICBitmap> bmp;
    ComPtr<IStream> mem;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fe;
    if (FAILED(f->CreateBitmapFromMemory(dw, dh, GUID_WICPixelFormat32bppBGRA, dw * 4, static_cast<UINT>(px.size()), px.data(), &bmp)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&fe, nullptr)) || FAILED(fe->Initialize(nullptr)) ||
        FAILED(fe->SetSize(dw, dh))) return out;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;                // no alpha needed: smaller PNG
    if (FAILED(fe->SetPixelFormat(&fmt))) return out;
    ComPtr<IWICFormatConverter> to24;
    if (FAILED(f->CreateFormatConverter(&to24)) ||
        FAILED(to24->Initialize(bmp.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(fe->WriteSource(to24.Get(), nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit())) return out;
    STATSTG st{};
    if (FAILED(mem->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart > (16u << 20)) return out;
    out.resize(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG got = 0;
    if (FAILED(mem->Seek(zero, STREAM_SEEK_SET, nullptr)) || FAILED(mem->Read(out.data(), static_cast<ULONG>(out.size()), &got)) ||
        got != out.size()) out.clear();
    return out;
}

void Picture(std::string& rtf, const std::vector<uint8_t>& png, int w, int h, int goalW, int goalH) {
    rtf += "{\\pict\\pngblip\\picw" + std::to_string(w) + "\\pich" + std::to_string(h) + "\\picwgoal" +
           std::to_string(goalW) + "\\pichgoal" + std::to_string(goalH) + "\n";
    static const char* hx = "0123456789abcdef";
    rtf.reserve(rtf.size() + png.size() * 2 + 8192);
    for (size_t i = 0; i < png.size(); ++i) {
        rtf += hx[png[i] >> 4];
        rtf += hx[png[i] & 15];
        if ((i & 63) == 63) rtf += '\n';
    }
    rtf += "}";
}

std::wstring NiceDate(const std::wstring& atom) {        // "2024-01-31T00:14:32Z" -> "31 Jan 2024, 00:14"
    if (atom.size() < 10 || atom[4] != L'-' || atom[7] != L'-') return atom;
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    const int m = _wtoi(atom.substr(5, 2).c_str());
    if (m < 1 || m > 12) return Date(atom);
    std::wstring s = std::to_wstring(_wtoi(atom.substr(8, 2).c_str())) + L" " + mon[m - 1] + L" " + atom.substr(0, 4);
    if (atom.size() >= 16) s += L", " + atom.substr(11, 5);
    return s;
}

std::wstring FolderPart(const std::wstring& p) {
    const auto s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s + 1);
}

std::wstring Param(const RevitInfo& info, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
        for (const auto& p : info.params)
            if (!p.value.empty() && SameName(p.name, n)) return p.value;
    return {};
}

} // namespace

std::string RevitToRtf(const RevitInfo& info, const std::wstring& fileName) {
    // Colours: 1 dim text, 2 table lines, 3 table header, 4 warning text, 5 "Not found" badge, 6 "Found" badge,
    // 7 main text, 8 card border, 9 labels.
    std::string rtf = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fnil Segoe MDL2 Assets;}}"
                      "{\\colortbl ;\\red96\\green96\\blue96;\\red210\\green210\\blue214;\\red240\\green241\\blue243;"
                      "\\red196\\green110\\blue0;\\red252\\green227\\blue166;\\red212\\green235\\blue217;"
                      "\\red40\\green40\\blue44;\\red226\\green226\\blue230;\\red110\\green110\\blue116;}"
                      "\\viewkind4\\f0\\fs20\\cf7\n";
    const bool family = info.kind == RevitInfo::Kind::Family || info.kind == RevitInfo::Kind::FamilyTemplate;

    // ---- The card (like the approved design): picture on the left, the main facts on the right.
    constexpr int kThumbPx = 256;                                  // 2x Revit's 128 px picture
    constexpr int kThumbTw = kThumbPx * 15;                        // twips at 96 dpi
    constexpr int kPad = 240;
    constexpr int kLeftW = kThumbTw + 2 * kPad, kRightW = 5600;
    std::vector<uint8_t> tile;
    UINT tw = 0;
    if (!info.thumbnailPng.empty() && info.thumbnailPng.size() < (4u << 20)) {
        tile = ThumbnailTile(info.thumbnailPng, kThumbPx);
        tw = kThumbPx;
    }
    const std::string border = "\\brdrs\\brdrw15\\brdrcf8";
    // Inner spacing: \trgaph (left / right of every cell; RichEdit ignores \trpadd) + \sb / \sa on the paragraphs.
    const std::string top = "\\sb" + std::to_string(kPad);
    rtf += "\\trowd\\trgaph" + std::to_string(kPad) + "\\trleft0";
    rtf += "\\clvertalt\\clbrdrt" + border + "\\clbrdrl" + border + "\\clbrdrb" + border + "\\cellx" + std::to_string(kLeftW);
    rtf += "\\clvertalt\\clbrdrt" + border + "\\clbrdrr" + border + "\\clbrdrb" + border + "\\cellx" + std::to_string(kLeftW + kRightW);
    // Left: picture + caption.
    const std::string endCell = "\\cf7\\fs20\\par\n\\pard\\intbl\\fs8\\cell\n";   // an empty small last line = bottom spacing
    if (!tile.empty()) {
        rtf += "\\pard\\intbl\\qc" + top + "\\sa80 ";
        Picture(rtf, tile, static_cast<int>(tw), static_cast<int>(tw), kThumbTw, kThumbTw);
        rtf += "\\par\n\\pard\\intbl\\qc\\cf9\\fs16 ";
        Esc(rtf, L"Saved thumbnail \u00B7 128 \u00D7 128 (shown 2\u00D7)");
        rtf += endCell;
    } else if (!info.thumbnailPng.empty()) {                       // could not enlarge: the original, drawn 2x
        rtf += "\\pard\\intbl\\qc" + top + "\\sa80 ";
        Picture(rtf, info.thumbnailPng, 128, 128, kThumbTw, kThumbTw);
        rtf += "\\par\n\\pard\\intbl\\qc\\cf9\\fs16 ";
        Esc(rtf, L"Saved thumbnail \u00B7 128 \u00D7 128");
        rtf += endCell;
    } else {
        rtf += "\\pard\\intbl\\qc" + top + "\\cf9\\fs16 ";
        Esc(rtf, L"No thumbnail saved in this file");
        rtf += endCell;
    }
    // Right: title, facts, linked files.
    rtf += "\\pard\\intbl" + top + "\\sa200{\\f1\\fs30 ";
    Esc(rtf, family ? L"\uE8B9" : L"\uE80F");                  // Segoe MDL2: family (puzzle-like) / project (building)
    rtf += "}\\~\\~\\b\\fs30 ";
    Esc(rtf, fileName);
    rtf += "\\b0\\fs20\\par\n";
    auto fact = [&](const std::wstring& label, const std::wstring& value) {
        if (value.empty()) return;
        rtf += "\\pard\\intbl\\li1900\\fi-1900\\tx1900\\sa80\\cf9 ";
        Esc(rtf, label);
        rtf += "\\tab\\cf7 ";
        Esc(rtf, value.size() > 260 ? value.substr(0, 260) + L"\u2026" : value);
        rtf += "\\par\n";
    };
    std::wstring version = info.version.empty() ? L"(unknown)" : info.version;
    if (!info.build.empty()) {
        std::wstring b = info.build;
        if (const auto p = b.find(L'('); p != std::wstring::npos) b.resize(p);   // "20220726_1500(x64)"
        version += L" \u00B7 build " + b;
    }
    fact(L"Revit version", version);
    std::wstring kind = KindText(info.kind);
    if (!kind.empty()) kind[0] = static_cast<wchar_t>(std::towupper(kind[0]));
    if (info.workshared) kind += L" (workshared)";
    if (!info.category.empty()) kind += L" \u00B7 " + info.category;
    fact(L"File kind", kind);
    if (!family) {
        fact(L"Project name", Param(info, {L"Project Name"}));
        fact(L"Project number", Param(info, {L"Project Number"}));
        fact(L"Client", Param(info, {L"Client Name"}));
        fact(L"Building", Param(info, {L"Building Name"}));
    }
    fact(L"Last saved", NiceDate(info.updated));
    fact(L"Saved at", FolderPart(info.lastSavePath));
    if (info.workshared) fact(L"Central model", info.centralPath.empty() ? L"(not recorded)" : info.centralPath);
    fact(L"Language", info.locale);
    if (!info.links.empty()) {
        rtf += "\\pard\\intbl\\sb160\\sa80\\b ";
        Esc(rtf, L"Linked files (" + std::to_wstring(info.links.size()) + L")");
        rtf += "\\b0\\par\n";
        size_t shown = 0;
        for (const auto& l : info.links) {
            if (++shown > 30) {
                rtf += "\\pard\\intbl\\cf9 ";
                Esc(rtf, L"+ " + std::to_wstring(info.links.size() - 30) + L" more (see the list below)");
                rtf += "\\cf7\\par\n";
                break;
            }
            const bool missing = l.state == L"Not Found";
            const std::wstring name = l.path.substr(l.path.find_last_of(L"\\/") + 1);
            rtf += "\\pard\\intbl\\sa60 {\\highlight";
            rtf += missing ? "5" : "6";
            rtf += "\\fs17 \\~";
            Esc(rtf, missing ? L"Not found" : (l.state.empty() ? L"Linked" : l.state));
            rtf += "\\~}\\~ ";
            Esc(rtf, l.type.empty() ? name : l.type + L" \u00B7 " + name);
            rtf += "\\par\n";
        }
    }
    rtf += "\\pard\\intbl\\fs8\\cell\\row\n\\pard\\fs20\\par\n";   // an empty small last line = bottom spacing

    // ---- Details below the card.
    // Project information / family parameters (only filled values, grouped like Revit).
    std::vector<const RevitParam*> filled;
    for (const auto& p : info.params) if (!p.value.empty()) filled.push_back(&p);
    if (!filled.empty()) {
        Heading(rtf, family ? L"Family parameters" : L"Project information");
        Table t{rtf, {2600, 6400}};
        std::wstring group;
        for (const auto* p : filled) {
            if (p->group != group && !p->group.empty()) { group = p->group; t.Row({group, L""}, true); }
            t.Row({p->name, p->value});
        }
    }
    // Family types: one row per type, one column per parameter (first 9 parameters).
    if (!info.types.empty()) {
        Heading(rtf, L"Types (" + std::to_wstring(info.types.size()) + L")");
        std::vector<std::wstring> cols;
        for (const auto& t : info.types)
            for (const auto& p : t.params)
                if (cols.size() < 9 && std::find(cols.begin(), cols.end(), p.name) == cols.end()) cols.push_back(p.name);
        std::vector<int> widths{2200};
        for (size_t i = 0; i < cols.size(); ++i) widths.push_back(std::max(900, static_cast<int>(6800 / std::max<size_t>(1, cols.size()))));
        Table t{rtf, widths};
        std::vector<std::wstring> head{L"Type"};
        head.insert(head.end(), cols.begin(), cols.end());
        t.Row(head, true);
        size_t shown = 0;
        for (const auto& ty : info.types) {
            if (++shown > 500) break;
            std::vector<std::wstring> row{ty.name};
            for (const auto& c : cols) {
                std::wstring v;
                for (const auto& p : ty.params) if (p.name == c) { v = p.value; break; }
                row.push_back(v);
            }
            t.Row(row);
        }
    }
    // Linked files with their full paths.
    if (!info.links.empty()) {
        Heading(rtf, L"Linked files: full paths");
        Table t{rtf, {2400, 4800, 1800}};
        t.Row({L"Type", L"Path", L"State when saved"}, true);
        for (const auto& l : info.links) t.Row({l.type, l.path, l.state}, false, l.state == L"Not Found" ? 2 : -1);
    }
    rtf += "\\pard\\sb240\\cf1\\fs16 ";
    Esc(rtf, L"The 3D model, views and sheets are stored in Autodesk's closed format and need Revit (double-click opens the file in its main program).");
    rtf += "\\cf0\\fs20\\par\n";
    for (const auto& n : info.notes) { rtf += "\\pard\\cf4 "; Esc(rtf, n); rtf += "\\cf0\\par\n"; }
    rtf += "}";
    return rtf;
}

} // namespace mzr::preview
