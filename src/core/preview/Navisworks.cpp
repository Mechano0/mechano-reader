#include "core/preview/Navisworks.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <unordered_set>
#include "utils/Format.h"

namespace mzr::preview {
namespace {

constexpr uint64_t kWholeScan = 24ull << 20;      // files up to this size are scanned completely
constexpr uint64_t kHeadScan = 16ull << 20;       // bigger files: the first 16 MB ...
constexpr uint64_t kTailScan = 8ull << 20;        // ... and the last 8 MB
constexpr size_t kMaxString = 1024;               // one text run (longer runs are cut)
constexpr size_t kMaxFiles = 300;
constexpr size_t kMaxVersions = 4;
constexpr size_t kMaxPicture = 4u << 20;          // thumbnail bytes
constexpr int kMaxPictureTries = 64;

NavisInfo::Kind KindOf(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    if (ext == L"nwc") return NavisInfo::Kind::Nwc;
    if (ext == L"nwf") return NavisInfo::Kind::Nwf;
    return NavisInfo::Kind::Nwd;
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

// A PNG that starts at b[p]: walks the chunks (IHDR first, IEND last). Returns its length, 0 if not valid.
size_t PngAt(const uint8_t* b, size_t n, size_t p, uint32_t& w, uint32_t& h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n - p < 8 + 25 || memcmp(b + p, sig, 8) != 0) return 0;
    size_t q = p + 8;
    bool first = true;
    for (int chunks = 0; chunks < 100000; ++chunks) {
        if (n - q < 12) return 0;
        const uint32_t len = Be32(b + q);
        if (len > kMaxPicture || len > n - q - 12) return 0;
        const uint8_t* type = b + q + 4;
        if (first) {
            if (memcmp(type, "IHDR", 4) != 0 || len != 13) return 0;
            w = Be32(b + q + 8);
            h = Be32(b + q + 12);
            first = false;
        }
        q += 12 + static_cast<size_t>(len);
        if (q - p > kMaxPicture) return 0;
        if (memcmp(type, "IEND", 4) == 0) return q - p;
    }
    return 0;
}

// A JPEG that starts at b[p]: walks the markers up to EOI. Returns its length, 0 if not valid.
size_t JpegAt(const uint8_t* b, size_t n, size_t p, uint32_t& w, uint32_t& h) {
    if (n - p < 4 || b[p] != 0xFF || b[p + 1] != 0xD8 || b[p + 2] != 0xFF) return 0;
    size_t q = p + 2;
    w = h = 0;
    for (int markers = 0; markers < 10000; ++markers) {
        if (q - p > kMaxPicture || n - q < 2 || b[q] != 0xFF) return 0;
        uint8_t m = b[q + 1];
        while (m == 0xFF) { ++q; if (n - q < 2) return 0; m = b[q + 1]; }       // fill bytes
        q += 2;
        if (m == 0xD9) return (w && h) ? q - p : 0;                            // EOI
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (n - q < 2) return 0;
        const uint32_t len = Be16(b + q);
        if (len < 2 || len > n - q) return 0;
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof && len >= 7) { h = Be16(b + q + 3); w = Be16(b + q + 5); }
        q += len;
        if (m == 0xDA) {                                                       // entropy-coded data: up to the next real marker
            while (true) {
                if (q - p > kMaxPicture || n - q < 2) return 0;
                if (b[q] == 0xFF && b[q + 1] != 0x00 && !(b[q + 1] >= 0xD0 && b[q + 1] <= 0xD7)) break;
                ++q;
            }
        }
    }
    return 0;
}

// Keeps the biggest sensible picture found in the buffer.
void FindPicture(const uint8_t* b, size_t n, NavisInfo& info) {
    int tries = 0;
    for (size_t p = 0; p + 4 <= n && tries < kMaxPictureTries; ++p) {
        const bool png = b[p] == 0x89 && b[p + 1] == 'P' && b[p + 2] == 'N' && b[p + 3] == 'G';
        const bool jpg = b[p] == 0xFF && b[p + 1] == 0xD8 && b[p + 2] == 0xFF;
        if (!png && !jpg) continue;
        ++tries;
        uint32_t w = 0, h = 0;
        const size_t len = png ? PngAt(b, n, p, w, h) : JpegAt(b, n, p, w, h);
        if (!len || w < 32 || h < 32 || w > 4096 || h > 4096) continue;
        if (uint64_t(w) * h > uint64_t(info.thumbnailWidth) * info.thumbnailHeight) {
            info.thumbnail.assign(b + p, b + p + len);
            info.thumbnailJpeg = jpg;
            info.thumbnailWidth = w;
            info.thumbnailHeight = h;
        }
        p += len - 1;
    }
}

// File types a Navisworks model is usually built from.
const std::vector<std::wstring>& SourceExts() {
    static const std::vector<std::wstring> e = {
        L"nwc", L"nwd", L"nwf", L"rvt", L"rfa", L"dwg", L"dxf", L"dwf", L"dwfx", L"dgn", L"ifc", L"ifczip", L"skp",
        L"3ds", L"fbx", L"stp", L"step", L"sat", L"ipt", L"iam", L"prt", L"asm", L"sldprt", L"sldasm", L"igs", L"iges",
        L"rvm", L"3dm", L"rcp", L"rcs", L"jt", L"x_t", L"catpart", L"catproduct", L"nwcache"};
    return e;
}

bool PathChar(wchar_t c) {
    return c >= 0x20 && c != L'"' && c != L'<' && c != L'>' && c != L'|' && c != L'*' && c != L'?';
}

struct Collector {
    NavisInfo& info;
    std::unordered_set<std::wstring> seenFiles, seenVersions;

    static std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }

    void Add(const std::wstring& s) {
        const std::wstring low = Lower(s);
        // The program that wrote the file.
        const auto nav = low.find(L"navisworks");
        if (nav != std::wstring::npos && s.size() <= 160 && info.versions.size() < kMaxVersions) {
            std::wstring v = s;
            v.erase(0, v.find_first_not_of(L" \t"));
            while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
            if (!v.empty() && seenVersions.insert(Lower(v)).second) info.versions.push_back(v);
        }
        // File references: "...name.ext" followed by the end or a non-name character.
        size_t prevEnd = 0;                                                    // a file never starts inside the previous one
        for (size_t dot = low.find(L'.'); dot != std::wstring::npos; dot = low.find(L'.', dot + 1)) {
            for (const auto& ext : SourceExts()) {
                if (low.compare(dot + 1, ext.size(), ext) != 0) continue;
                const size_t end = dot + 1 + ext.size();
                if (end < low.size() && (std::iswalnum(low[end]) || low[end] == L'_')) continue;
                if (dot == 0 || std::iswspace(s[dot - 1]) || s[dot - 1] == L'\\' || s[dot - 1] == L'/') break;   // no name before the dot
                // Start: a drive "X:\", a UNC "\\", else the first path character run before the dot.
                size_t start = dot;
                while (start > prevEnd && PathChar(s[start - 1])) --start;
                for (size_t i = dot; i-- > start;) {                          // the closest one ("C:\a.rvt C:\b.dwg" = 2 files)
                    if (i + 2 < dot && std::iswalpha(s[i]) && s[i + 1] == L':' && (s[i + 2] == L'\\' || s[i + 2] == L'/')) { start = i; break; }
                    if (i + 1 < dot && s[i] == L'\\' && s[i + 1] == L'\\' && (i == start || s[i - 1] != L'\\')) { start = i; break; }
                }
                std::wstring f = s.substr(start, end - start);
                f.erase(0, f.find_first_not_of(L" \t"));
                const auto slash = f.find_last_of(L"\\/");
                const std::wstring base = slash == std::wstring::npos ? f : f.substr(slash + 1);
                if (base.size() < ext.size() + 2) break;                       // ".rvt" alone is not a file
                if (info.files.size() >= kMaxFiles) { info.filesTruncated = true; return; }
                if (seenFiles.insert(Lower(f)).second) info.files.push_back(f);
                prevEnd = end;
                break;
            }
        }
    }
};

bool TextByte(uint8_t c) { return c >= 0x20 && c < 0x7F; }
bool TextWide(wchar_t c) {
    return (c >= 0x20 && c < 0x7F) || (c >= 0xC0 && c <= 0x24F) || (c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFEFC);
}

// Readable runs: 8-bit ASCII and UTF-16LE (both byte alignments), at least 6 characters.
void ScanText(const uint8_t* b, size_t n, Collector& col) {
    std::wstring run;
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && TextByte(b[i]) && run.size() < kMaxString) { run += static_cast<wchar_t>(b[i]); continue; }
        if (run.size() >= 6) col.Add(run);
        run.clear();
        if (i < n && TextByte(b[i])) run += static_cast<wchar_t>(b[i]);
    }
    for (size_t align = 0; align < 2; ++align) {
        run.clear();
        for (size_t i = align; i + 1 <= n; i += 2) {
            const bool more = i + 1 < n;
            const wchar_t c = more ? static_cast<wchar_t>(b[i] | (b[i + 1] << 8)) : 0;
            if (more && TextWide(c) && run.size() < kMaxString) { run += c; continue; }
            if (run.size() >= 6) col.Add(run);
            run.clear();
            if (more && TextWide(c)) run += c;
            if (!more) break;
        }
        if (run.size() >= 6) col.Add(run);
    }
}

void ScanBuffer(const std::vector<uint8_t>& buf, NavisInfo& info, Collector& col) {
    if (buf.empty()) return;
    if (info.thumbnail.empty() || info.thumbnailWidth < 64) FindPicture(buf.data(), buf.size(), info);
    ScanText(buf.data(), buf.size(), col);
}

} // namespace

Result<NavisInfo> ReadNavisworks(NavisReadAt readAt, uint64_t fileSize, const std::wstring& fileName) {
    NavisInfo info;
    info.kind = KindOf(fileName);
    info.size = fileSize;
    if (fileSize < 16) return ErrorInfo{ErrorCode::Corrupted, L"This Navisworks file is empty or damaged.", L"size " + std::to_wstring(fileSize)};
    uint8_t head[8] = {};
    if (!readAt(0, head, sizeof head)) return ErrorInfo{ErrorCode::Io, L"The Navisworks file could not be read.", L"header"};
    static const uint8_t cfb[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    info.container = memcmp(head, cfb, 8) == 0 ? L"OLE compound file"
                   : (head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) ? L"ZIP"
                   : head[0] == '<' || (head[0] == 0xEF && head[1] == 0xBB) ? L"XML text"
                   : L"Navisworks binary";
    Collector col{info, {}, {}};
    auto scan = [&](uint64_t off, uint64_t len) -> bool {
        std::vector<uint8_t> buf;
        try { buf.resize(static_cast<size_t>(len)); } catch (...) { return false; }
        if (!readAt(off, buf.data(), buf.size())) return false;
        ScanBuffer(buf, info, col);
        return true;
    };
    bool ok;
    if (fileSize <= kWholeScan) {
        ok = scan(0, fileSize);
    } else {
        info.partial = true;
        ok = scan(0, kHeadScan) && scan(fileSize - kTailScan, kTailScan);
    }
    if (!ok) return ErrorInfo{ErrorCode::Io, L"The Navisworks file could not be read.", L"scan"};
    return info;
}

Result<NavisInfo> ReadNavisworksBytes(const std::vector<uint8_t>& data, const std::wstring& fileName) {
    auto readAt = [&data](uint64_t off, void* buf, size_t n) {
        if (off > data.size() || n > data.size() - off) return false;
        if (n) memcpy(buf, data.data() + off, n);
        return true;
    };
    return ReadNavisworks(readAt, data.size(), fileName);
}

Result<NavisInfo> ReadNavisworksFile(const std::wstring& path, const std::wstring& fileName) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ErrorInfo{ErrorCode::Io, L"The Navisworks file could not be opened.", L"CreateFile " + std::to_wstring(GetLastError())};
    std::shared_ptr<void> guard(h, CloseHandle);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    auto readAt = [h](uint64_t off, void* buf, size_t n) {
        uint8_t* p = static_cast<uint8_t*>(buf);
        while (n) {                                                            // ReadFile takes at most 4 GB at once
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 64u << 20));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            if (!ReadFile(h, p, chunk, &got, &ov) || got != chunk) return false;
            p += chunk; off += chunk; n -= chunk;
        }
        return true;
    };
    return ReadNavisworks(readAt, static_cast<uint64_t>(size.QuadPart), fileName);
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

void Row(std::string& rtf, const std::vector<int>& widths, const std::vector<std::wstring>& cells, bool header = false) {
    rtf += "\\trowd\\trgaph90\\trleft0";
    int x = 0;
    for (int w : widths) {
        x += w;
        rtf += "\\clbrdrb\\brdrs\\brdrw5\\brdrcf2";
        if (header) rtf += "\\clcbpat3";
        rtf += "\\cellx" + std::to_string(x);
    }
    rtf += "\\pard\\intbl\\sb40\\sa40 ";       // the space ends the control word (a cell starting with digits)
    for (size_t i = 0; i < widths.size(); ++i) {
        std::wstring v = i < cells.size() ? cells[i] : L"";
        if (v.size() > 300) v = v.substr(0, 300) + L"...";
        if (header) rtf += "\\b ";
        Esc(rtf, v);
        if (header) rtf += "\\b0 ";
        rtf += "\\cell ";
    }
    rtf += "\\row\n";
}

void Heading(std::string& rtf, const std::wstring& t) { rtf += "\\pard\\sb240\\sa80\\b\\fs24 "; Esc(rtf, t); rtf += "\\b0\\fs20\\par\n"; }

std::wstring KindText(NavisInfo::Kind k) {
    switch (k) {
    case NavisInfo::Kind::Nwc: return L"Navisworks cache (NWC): one source file converted for Navisworks";
    case NavisInfo::Kind::Nwf: return L"Navisworks working file (NWF): links to the appended files, no geometry inside";
    default: return L"Navisworks published model (NWD): all geometry and data in one file";
    }
}

} // namespace

std::string NavisworksToRtf(const NavisInfo& info, const std::wstring& fileName) {
    std::string rtf = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}}"
                      "{\\colortbl ;\\red96\\green96\\blue96;\\red210\\green210\\blue214;\\red240\\green241\\blue243;\\red196\\green110\\blue0;}"
                      "\\viewkind4\\f0\\fs20\n";
    rtf += "\\pard\\sa40\\b\\fs30 ";
    Esc(rtf, fileName);
    rtf += "\\b0\\fs20\\par\n\\pard\\sa160\\cf1 ";
    Esc(rtf, KindText(info.kind));
    rtf += "\\cf0\\par\n";

    if (!info.thumbnail.empty() && info.thumbnailWidth && info.thumbnailHeight) {
        // Shown at most 3.5 inch wide / 2.5 inch high, aspect kept.
        const double s = std::min({1.0, 5040.0 / (info.thumbnailWidth * 15.0), 3600.0 / (info.thumbnailHeight * 15.0)});
        const int gw = static_cast<int>(info.thumbnailWidth * 15.0 * s), gh = static_cast<int>(info.thumbnailHeight * 15.0 * s);
        rtf += std::string("\\pard\\sa120{\\pict") + (info.thumbnailJpeg ? "\\jpegblip" : "\\pngblip") +
               "\\picw" + std::to_string(info.thumbnailWidth) + "\\pich" + std::to_string(info.thumbnailHeight) +
               "\\picwgoal" + std::to_string(gw) + "\\pichgoal" + std::to_string(gh) + "\n";
        static const char* hx = "0123456789abcdef";
        rtf.reserve(rtf.size() + info.thumbnail.size() * 2 + 8192);
        for (size_t i = 0; i < info.thumbnail.size(); ++i) {
            rtf += hx[info.thumbnail[i] >> 4];
            rtf += hx[info.thumbnail[i] & 15];
            if ((i & 63) == 63) rtf += '\n';
        }
        rtf += "}\\par\n\\pard\\sa160\\cf1\\fs16 Thumbnail saved in the file (" + std::to_string(info.thumbnailWidth) + " x " +
               std::to_string(info.thumbnailHeight) + ")\\cf0\\fs20\\par\n";
    }

    Heading(rtf, L"File");
    {
        const std::vector<int> w{2600, 6400};
        Row(rtf, w, {L"Size", fmt::Size(info.size)});
        if (!info.versions.empty()) {
            std::wstring v;
            for (const auto& s : info.versions) { if (!v.empty()) v += L"\n"; v += s; }
            Row(rtf, w, {L"Written by", v});
        }
        Row(rtf, w, {L"Format", info.container});
    }

    if (!info.files.empty()) {
        Heading(rtf, (info.kind == NavisInfo::Kind::Nwf ? L"Appended files (" : L"Source files referenced (") +
                     std::to_wstring(info.files.size()) + (info.filesTruncated ? L"+" : L"") + L")");
        const std::vector<int> w{3400, 5600};
        Row(rtf, w, {L"File", L"Folder"}, true);
        for (const auto& f : info.files) {
            const auto slash = f.find_last_of(L"\\/");
            if (slash == std::wstring::npos) Row(rtf, w, {f, L""});
            else Row(rtf, w, {f.substr(slash + 1), f.substr(0, slash)});
        }
    }

    rtf += "\\pard\\sb240\\cf1\\fs16 ";
    std::wstring note = L"Navisworks stores the model in Autodesk's closed format, so this card shows only what can be read from outside "
                        L"(thumbnail, version, referenced files). Double-click opens the file in Navisworks (Navisworks Freedom is a free viewer).";
    if (info.partial) note += L" Large file: only its beginning and end were checked.";
    Esc(rtf, note);
    rtf += "\\cf0\\fs20\\par\n";
    if (info.thumbnail.empty() && info.versions.empty() && info.files.empty()) {
        rtf += "\\pard\\cf4 ";
        Esc(rtf, L"No readable details were found inside this file (its data is compressed).");
        rtf += "\\cf0\\par\n";
    }
    rtf += "}";
    return rtf;
}

} // namespace mzr::preview
