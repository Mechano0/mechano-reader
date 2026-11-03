#include "core/preview/Ooxml.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <unordered_map>

#include <pugixml.hpp>

#include "core/archive/ArchiveEngine.h"

namespace mzr::preview {
namespace {

// ============================================================ helpers

std::wstring W(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

// Prefix-agnostic names: "w:p" and "p" both match "p" (producers may use other prefixes).
const char* Local(const char* name) {
    const char* c = strchr(name, ':');
    return c ? c + 1 : name;
}
bool Is(const pugi::xml_node& n, const char* local) { return n.type() == pugi::node_element && strcmp(Local(n.name()), local) == 0; }
pugi::xml_node Child(const pugi::xml_node& n, const char* local) {
    for (auto c : n.children()) if (Is(c, local)) return c;
    return {};
}
// Pre-order walk without recursion (a hostile file must never be able to exhaust
// the stack). Visits every descendant of `root`; `fn(node, depth)` returns false to stop.
template <typename Fn>
void WalkDescendants(const pugi::xml_node& root, Fn&& fn) {
    size_t depth = 1;
    for (pugi::xml_node n = root.first_child(); n;) {
        if (!fn(n, depth)) return;
        if (auto c = n.first_child()) { n = c; ++depth; continue; }
        while (n != root && !n.next_sibling()) { n = n.parent(); --depth; }
        if (n == root) return;
        n = n.next_sibling();
    }
}
pugi::xml_node Find(const pugi::xml_node& n, const char* local) {      // first descendant
    pugi::xml_node found;
    WalkDescendants(n, [&](const pugi::xml_node& c, size_t) {
        if (Is(c, local)) { found = c; return false; }
        return true;
    });
    return found;
}
// Real Office files nest a few dozen levels (nested tables included). Anything deeper
// is refused, so the recursive converters below always have a bounded depth (S1).
constexpr size_t kMaxXmlDepth = 256;
bool DepthOk(const pugi::xml_node& doc) {
    bool ok = true;
    WalkDescendants(doc, [&](const pugi::xml_node&, size_t depth) {
        if (depth > kMaxXmlDepth) { ok = false; return false; }
        return true;
    });
    return ok;
}
const char* Attr(const pugi::xml_node& n, const char* local) {
    for (auto a : n.attributes()) if (strcmp(Local(a.name()), local) == 0) return a.value();
    return "";
}
// Relationship id (r:id). <p:sldId id="256" r:id="rId2"> has TWO attributes whose
// local name is "id": the prefixed one is the relationship.
const char* RelId(const pugi::xml_node& n) {
    for (auto a : n.attributes())
        if (strchr(a.name(), ':') && strcmp(Local(a.name()), "id") == 0) return a.value();
    return "";
}
bool OnOff(const pugi::xml_node& n) {              // <w:b/>, <w:b w:val="0"/>
    if (!n) return false;
    const char* v = Attr(n, "val");
    return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "off") == 0 || strcmp(v, "none") == 0);
}
long long Num(const char* s, long long def = 0) {
    if (!s || !*s) return def;
    char* end = nullptr;
    const long long v = strtoll(s, &end, 10);
    return end == s ? def : v;
}
uint32_t Hex(const char* s, uint32_t def) {
    if (!s || strlen(s) != 6) return def;
    char* end = nullptr;
    const unsigned long v = strtoul(s, &end, 16);
    return (end && *end == 0) ? static_cast<uint32_t>(v) : def;
}

// The Office file opened as a ZIP in RAM.
class Package {
public:
    static Result<Package> Open(const Bytes& file) {
        if (!file || file->empty() || file->size() > kMaxOfficeBytes)
            return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This Office file is too large to preview.", L""};
        archive::OpenOptions oo;
        oo.onlyFormat = L"zip";
        oo.maxEntries = 20000;
        auto a = archive::Archive::OpenMemory(file, L"office.zip", oo);
        if (!a) return ErrorInfo{ErrorCode::Corrupted,
                                 L"This Office file could not be read. It may be damaged, password protected, or an old (97-2003) file.",
                                 a.error().details};
        Package p;
        p.arc_ = std::shared_ptr<archive::Archive>(std::move(a.value()));
        for (const auto& e : p.arc_->Index()->entries()) {
            if (e.isDir) continue;
            std::wstring k = e.path;
            for (auto& c : k) { if (c == L'\\') c = L'/'; c = static_cast<wchar_t>(towlower(c)); }
            p.parts_[k] = e.arcIndex;
        }
        return p;
    }
    bool Has(const std::wstring& part) const { return parts_.count(Key(part)) != 0; }
    Result<std::vector<uint8_t>> Read(const std::wstring& part, uint64_t max) {
        auto it = parts_.find(Key(part));
        if (it == parts_.end()) return ErrorInfo{ErrorCode::Corrupted, L"A part of this Office file is missing.", part};
        archive::CancelToken none;
        return arc_->ReadEntry(it->second, max, none);
    }
    // Parse an XML part (empty document if missing or too big).
    bool Xml(const std::wstring& part, pugi::xml_document& doc) {
        auto b = Read(part, kMaxXmlPart);
        if (!b) return false;
        if (doc.load_buffer(b.value().data(), b.value().size(), pugi::parse_default | pugi::parse_ws_pcdata,
                            pugi::encoding_auto).status != pugi::status_ok) return false;
        if (!DepthOk(doc)) { doc.reset(); return false; }     // nested too deeply: treated as damaged
        return true;
    }
    // Relationships of a part: Id -> resolved part path. External targets are dropped.
    std::map<std::string, std::wstring> Rels(const std::wstring& part) {
        std::map<std::string, std::wstring> out;
        const auto slash = part.find_last_of(L'/');
        const std::wstring dir = slash == std::wstring::npos ? L"" : part.substr(0, slash + 1);
        const std::wstring file = slash == std::wstring::npos ? part : part.substr(slash + 1);
        pugi::xml_document d;
        if (!Xml(dir + L"_rels/" + file + L".rels", d)) return out;
        for (auto r : Child(d, "Relationships").children()) {
            if (!Is(r, "Relationship")) continue;
            if (_stricmp(Attr(r, "TargetMode"), "External") == 0) continue;     // never fetched
            out[Attr(r, "Id")] = Resolve(dir, W(Attr(r, "Target")));
        }
        return out;
    }
    static std::wstring Resolve(const std::wstring& dir, std::wstring target) {
        std::wstring base = (!target.empty() && target[0] == L'/') ? L"" : dir;
        if (!target.empty() && target[0] == L'/') target.erase(0, 1);
        std::vector<std::wstring> parts;
        auto push = [&](const std::wstring& s) {
            size_t i = 0;
            while (i <= s.size()) {
                size_t j = s.find(L'/', i);
                if (j == std::wstring::npos) j = s.size();
                const std::wstring seg = s.substr(i, j - i);
                if (seg == L"..") { if (!parts.empty()) parts.pop_back(); }
                else if (!seg.empty() && seg != L".") parts.push_back(seg);
                i = j + 1;
            }
        };
        push(base);
        push(target);
        std::wstring r;
        for (size_t i = 0; i < parts.size(); ++i) r += (i ? L"/" : L"") + parts[i];
        return r;
    }
private:
    static std::wstring Key(std::wstring p) {
        for (auto& c : p) { if (c == L'\\') c = L'/'; c = static_cast<wchar_t>(towlower(c)); }
        if (!p.empty() && p[0] == L'/') p.erase(0, 1);
        return p;
    }
    std::shared_ptr<archive::Archive> arc_;
    std::unordered_map<std::wstring, uint32_t> parts_;
};

// ============================================================ DOCX -> RTF

class RtfWriter {
public:
    std::string body;
    int Color(uint32_t rgb) {
        for (size_t i = 0; i < colors_.size(); ++i) if (colors_[i] == rgb) return static_cast<int>(i + 1);
        colors_.push_back(rgb);
        return static_cast<int>(colors_.size());
    }
    void Text(const std::wstring& s) {
        for (wchar_t c : s) {
            if (c == L'\\' || c == L'{' || c == L'}') { body += '\\'; body += static_cast<char>(c); }
            else if (c == L'\t') body += "\\tab ";
            else if (c == L'\r' || c == L'\n') body += "\\line ";
            else if (c < 0x20) continue;
            else if (c < 0x80) body += static_cast<char>(c);
            else { body += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?"; }
        }
    }
    std::string Finish() const {
        std::string ct = "{\\colortbl ;";
        for (uint32_t c : colors_) {
            char b[64];
            snprintf(b, sizeof(b), "\\red%u\\green%u\\blue%u;", (c >> 16) & 255, (c >> 8) & 255, c & 255);
            ct += b;
        }
        ct += "}";
        return "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}" + ct +
               "\\viewkind4\\f0\\fs22\n" + body + "}";
    }
private:
    std::vector<uint32_t> colors_;
};

class DocxConverter {
public:
    DocxConverter(Package& p, long long maxWidthTwips) : pkg_(p), maxW_(maxWidthTwips) {}

    Result<std::string> Run() {
        // Main part: from _rels/.rels (officeDocument), default word/document.xml.
        std::wstring main = L"word/document.xml";
        {
            pugi::xml_document r;
            if (pkg_.Xml(L"_rels/.rels", r))
                for (auto rel : Child(r, "Relationships").children())
                    if (strstr(Attr(rel, "Type"), "/officeDocument")) main = Package::Resolve(L"", W(Attr(rel, "Target")));
        }
        pugi::xml_document doc;
        if (!pkg_.Xml(main, doc))
            return ErrorInfo{ErrorCode::Corrupted, L"This Word file could not be read (it may be damaged or an old .doc file).", main};
        rels_ = pkg_.Rels(main);
        LoadStyles();
        auto body = Find(doc, "body");
        if (!body) return ErrorInfo{ErrorCode::Corrupted, L"This Word file has no content.", L""};
        Block(body);
        if (paragraphs_ == 0) { rtf_.body += "\\pard\\i "; rtf_.Text(L"(This document has no text.)"); rtf_.body += "\\par\n"; }
        if (truncated_) {
            rtf_.body += "\\pard\\i ";
            rtf_.Text(L"... (the preview shows the beginning of this document)");
            rtf_.body += "\\par\n";
        }
        return rtf_.Finish();
    }

private:
    void LoadStyles() {
        pugi::xml_document s;
        if (!pkg_.Xml(L"word/styles.xml", s)) return;
        for (auto st : Child(s, "styles").children()) {
            if (!Is(st, "style")) continue;
            std::string name = Attr(Child(st, "name"), "val");
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); });
            int level = -1;
            if (name == "title") level = 0;
            else if (name.rfind("heading ", 0) == 0) level = static_cast<int>(Num(name.c_str() + 8, -1));
            if (level >= 0) {
                headings_[Attr(st, "styleId")] = level;
                const uint32_t c = Hex(Attr(Child(Child(st, "rPr"), "color"), "val"), 0xFFFFFFFF);
                if (c != 0xFFFFFFFF && c != 0) headingColor_[Attr(st, "styleId")] = c;
            }
        }
    }

    // body / cell content: paragraphs and tables in order
    void Block(const pugi::xml_node& n, bool inTable = false) {
        for (auto c : n.children()) {
            if (truncated_) return;
            if (Is(c, "p")) { Paragraph(c, inTable); if (!inTable) rtf_.body += "\\par\n"; }
            else if (Is(c, "tbl")) { if (inTable) Block(c, true); else Table(c); }   // nested tables: flattened
            else if (Is(c, "sdt")) { if (auto sc = Child(c, "sdtContent")) Block(sc, inTable); }
            else if (Is(c, "tr") || Is(c, "tc") || Is(c, "customXml")) Block(c, inTable);
        }
    }

    void Paragraph(const pugi::xml_node& p, bool inTable) {
        if (++paragraphs_ > 20000) { truncated_ = true; return; }
        auto ppr = Child(p, "pPr");
        int heading = -1;
        uint32_t headColor = 0xFFFFFFFF;
        if (auto ps = Child(ppr, "pStyle")) {
            auto it = headings_.find(Attr(ps, "val"));
            if (it != headings_.end()) heading = it->second;
            auto hc = headingColor_.find(Attr(ps, "val"));
            if (hc != headingColor_.end()) headColor = hc->second;
        }
        std::string h = "\\pard\\plain\\f0\\fs22\\sa100";
        if (inTable) h += "\\intbl";
        const std::string jc = Attr(Child(ppr, "jc"), "val");
        if (jc == "center") h += "\\qc";
        else if (jc == "right" || jc == "end") h += "\\qr";
        else if (jc == "both" || jc == "distribute") h += "\\qj";
        if (OnOff(Child(ppr, "bidi"))) h += "\\rtlpar";
        const bool list = static_cast<bool>(Child(ppr, "numPr"));
        if (list) h += "\\li360\\fi-240";
        rtf_.body += h + " ";
        if (heading >= 0) {
            static const int sizes[] = {40, 32, 28, 26, 24, 22, 22};
            rtf_.body += "\\b\\fs" + std::to_string(sizes[std::min(heading, 6)]) + "\\sb120 ";
            if (headColor != 0xFFFFFFFF) rtf_.body += "\\cf" + std::to_string(rtf_.Color(headColor)) + " ";
        }
        if (list) rtf_.Text(L"•\t");
        Inline(p, heading >= 0);
    }

    void Inline(const pugi::xml_node& n, bool heading) {
        for (auto c : n.children()) {
            if (Is(c, "r")) Run(c, heading);
            else if (Is(c, "hyperlink") || Is(c, "ins") || Is(c, "smartTag") || Is(c, "fldSimple") ||
                     Is(c, "customXml") || Is(c, "sdtContent") || Is(c, "bdo") || Is(c, "dir"))
                Inline(c, heading);
            else if (Is(c, "sdt")) Inline(Child(c, "sdtContent"), heading);
            // "del" (deleted text), "pPr", bookmarks, comments: not shown
        }
    }

    void Run(const pugi::xml_node& r, bool heading) {
        auto rp = Child(r, "rPr");
        std::string fmt;
        if (OnOff(Child(rp, "b")) || OnOff(Child(rp, "bCs"))) fmt += "\\b";
        if (OnOff(Child(rp, "i")) || OnOff(Child(rp, "iCs"))) fmt += "\\i";
        if (auto u = Child(rp, "u"); u && strcmp(Attr(u, "val"), "none") != 0) fmt += "\\ul";
        if (OnOff(Child(rp, "strike"))) fmt += "\\strike";
        if (!heading)
            if (auto sz = Child(rp, "sz")) {
                const long long v = Num(Attr(sz, "val"));
                if (v >= 8 && v <= 144) fmt += "\\fs" + std::to_string(v);   // both in half-points
            }
        if (auto col = Child(rp, "color")) {
            const uint32_t c = Hex(Attr(col, "val"), 0xFFFFFFFF);
            if (c != 0xFFFFFFFF && c != 0) fmt += "\\cf" + std::to_string(rtf_.Color(c));
        }
        if (OnOff(Child(rp, "vanish"))) return;      // hidden text
        rtf_.body += "{" + fmt + " ";
        for (auto c : r.children()) {
            if (Is(c, "t")) rtf_.Text(W(c.text().get()));
            else if (Is(c, "tab")) rtf_.body += "\\tab ";
            else if (Is(c, "br") || Is(c, "cr")) rtf_.body += "\\line ";
            else if (Is(c, "noBreakHyphen")) rtf_.Text(L"-");
            else if (Is(c, "sym")) rtf_.Text(L"□");
            else if (Is(c, "drawing") || Is(c, "pict") || Is(c, "object")) Picture(c);
        }
        rtf_.body += "}";
    }

    void Picture(const pugi::xml_node& d) {
        // DrawingML: a:blip r:embed + wp:extent cx/cy.  VML: v:imagedata r:id.
        std::string rid;
        if (auto b = Find(d, "blip")) rid = Attr(b, "embed");
        if (rid.empty()) if (auto im = Find(d, "imagedata")) rid = RelId(im);
        auto it = rels_.find(rid);
        if (rid.empty() || it == rels_.end()) return;
        long long cx = 0, cy = 0;
        if (auto e = Find(d, "extent")) { cx = Num(Attr(e, "cx")); cy = Num(Attr(e, "cy")); }
        // Hostile sizes must not overflow the arithmetic below (S8): 2e9 EMU is about 55 m.
        cx = std::clamp<long long>(cx, 0, 2'000'000'000);
        cy = std::clamp<long long>(cy, 0, 2'000'000'000);
        if (imageBytes_ > (48ull << 20)) { rtf_.Text(L"[picture]"); return; }
        auto img = pkg_.Read(it->second, kMaxMediaPart);
        if (!img || img.value().size() < 8) { rtf_.Text(L"[picture]"); return; }
        const auto& v = img.value();
        const char* blip = nullptr;
        if (v[0] == 0x89 && v[1] == 'P' && v[2] == 'N' && v[3] == 'G') blip = "\\pngblip";
        else if (v[0] == 0xFF && v[1] == 0xD8) blip = "\\jpegblip";
        // EMF / WMF are not passed on (S5): metafiles are replayed by the GDI metafile parser,
        // a large attack surface for a preview. They show as "[picture]" like in PowerPoint.
        if (!blip) { rtf_.Text(L"[picture]"); return; }
        imageBytes_ += v.size();
        // Size in twips (1 twip = 635 EMU); keep inside the view width.
        long long wt = cx > 0 ? cx / 635 : 4320, ht = cy > 0 ? cy / 635 : 3240;
        if (wt > maxW_) { ht = ht * maxW_ / wt; wt = maxW_; }
        std::string s = "{\\pict" + std::string(blip) + "\\picw" + std::to_string(std::max<long long>(1, wt / 15)) +
                        "\\pich" + std::to_string(std::max<long long>(1, ht / 15)) + "\\picwgoal" + std::to_string(wt) +
                        "\\pichgoal" + std::to_string(ht) + "\n";
        static const char* hx = "0123456789abcdef";
        s.reserve(s.size() + v.size() * 2 + v.size() / 64 + 4);
        for (size_t i = 0; i < v.size(); ++i) {
            s += hx[v[i] >> 4];
            s += hx[v[i] & 15];
            if ((i & 63) == 63) s += '\n';
        }
        rtf_.body += s + "}";
    }

    void Table(const pugi::xml_node& t) {
        std::vector<long long> grid;
        for (auto g : Child(t, "tblGrid").children())
            if (Is(g, "gridCol")) grid.push_back(std::max<long long>(200, Num(Attr(g, "w"), 1500)));
        // Wider than the view: shrink all columns in proportion (like "fit to window").
        long long total = 0;
        for (auto g : grid) total += g;
        if (total > maxW_)
            for (auto& g : grid) g = std::max<long long>(200, g * maxW_ / total);
        for (auto tr : t.children()) {
            if (truncated_) return;
            if (!Is(tr, "tr")) continue;
            if (++rows_ > 5000) { truncated_ = true; return; }
            std::vector<pugi::xml_node> cells;
            for (auto tc : tr.children()) if (Is(tc, "tc")) cells.push_back(tc);
            if (cells.empty()) continue;
            std::string row = "\\trowd\\trgaph80\\trleft0";
            long long x = 0;
            size_t col = 0;
            for (auto& tc : cells) {
                const long long span = std::max<long long>(1, Num(Attr(Child(Child(tc, "tcPr"), "gridSpan"), "val"), 1));
                long long w = 0;
                for (long long k = 0; k < span; ++k, ++col) w += col < grid.size() ? grid[col] : maxW_ / static_cast<long long>(cells.size());
                x += w;
                row += "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10\\clbrdrr\\brdrs\\brdrw10\\cellx" +
                       std::to_string(x);
            }
            rtf_.body += row + "\n";
            for (auto& tc : cells) {
                // All paragraphs of the cell; the last one ends with \cell.
                std::vector<pugi::xml_node> ps;
                for (auto c : tc.children()) if (Is(c, "p") || Is(c, "tbl") || Is(c, "sdt")) ps.push_back(c);
                if (ps.empty()) { rtf_.body += "\\pard\\intbl \\cell "; continue; }
                for (size_t i = 0; i < ps.size(); ++i) {
                    if (Is(ps[i], "p")) Paragraph(ps[i], true);
                    else { rtf_.body += "\\pard\\intbl "; Block(ps[i], true); }
                    rtf_.body += i + 1 < ps.size() ? "\\par " : "\\cell ";
                }
            }
            rtf_.body += "\\row\n";
        }
        rtf_.body += "\\pard\\par\n";
    }

    Package& pkg_;
    RtfWriter rtf_;
    std::map<std::string, std::wstring> rels_;
    std::map<std::string, int> headings_;
    std::map<std::string, uint32_t> headingColor_;
    long long maxW_ = 9360;                      // view width in twips (tables / pictures fit inside)
    size_t paragraphs_ = 0, rows_ = 0;
    uint64_t imageBytes_ = 0;
    bool truncated_ = false;
};

// ============================================================ XLSX

// ---- Excel number formats -------------------------------------------------------------

// Built-in format ids (ECMA-376 18.8.30). 14 / 22 follow the Windows short date (like Excel).
std::string BuiltinFormat(int id) {
    switch (id) {
    case 1: return "0";            case 2: return "0.00";        case 3: return "#,##0";      case 4: return "#,##0.00";
    case 5: return "#,##0 ;(#,##0)"; case 6: return "#,##0 ;[Red](#,##0)"; case 7: return "#,##0.00;(#,##0.00)";
    case 8: return "#,##0.00;[Red](#,##0.00)";
    case 9: return "0%";           case 10: return "0.00%";      case 11: return "0.00E+00";  case 12: return "# ?/?";
    case 13: return "# ?\?/??";    case 14: return "\x01" "date";  case 15: return "d-mmm-yy"; case 16: return "d-mmm";
    case 17: return "mmm-yy";      case 18: return "h:mm AM/PM"; case 19: return "h:mm:ss AM/PM"; case 20: return "h:mm";
    case 21: return "h:mm:ss";     case 22: return "\x01" "datetime";
    case 37: return "#,##0 ;(#,##0)"; case 38: return "#,##0 ;[Red](#,##0)"; case 39: return "#,##0.00;(#,##0.00)";
    case 40: return "#,##0.00;[Red](#,##0.00)"; case 45: return "mm:ss"; case 46: return "[h]:mm:ss"; case 47: return "mm:ss.0";
    case 48: return "##0.0E+0";    case 49: return "@";
    default: return "";
    }
}

std::wstring Widen(const std::string& s) { return W(s.c_str()); }

std::wstring General(double v) {
    if (v == 0) return L"0";
    const double a = std::abs(v);
    wchar_t b[64];
    if (a >= 1e11 || a < 1e-9) {                       // Excel switches to scientific here
        swprintf_s(b, L"%.5E", v);
        std::wstring s = b;
        const size_t e = s.find(L'E');
        std::wstring m = s.substr(0, e), x = s.substr(e);
        while (m.find(L'.') != std::wstring::npos && (m.back() == L'0' || m.back() == L'.')) m.pop_back();
        // "E+005" -> "E+05"
        std::wstring digits = x.substr(2);
        while (digits.size() > 2 && digits[0] == L'0') digits.erase(0, 1);
        return m + x.substr(0, 2) + digits;
    }
    std::wstring s;
    for (int prec = 10; prec >= 1; --prec) {           // at most 11 characters, like a default-width column
        swprintf_s(b, L"%.*G", prec, v);
        s = b;
        if (s.size() <= 11) break;
    }
    // "1.2345E-005" / "1.2345E-05" -> Excel's two-digit exponent
    const size_t e = s.find(L'E');
    if (e != std::wstring::npos && e + 2 < s.size()) {
        std::wstring digits = s.substr(e + 2);
        while (digits.size() > 2 && digits[0] == L'0') digits.erase(0, 1);
        s = s.substr(0, e + 2) + digits;
    }
    return s;
}

void CivilDate(double serial, int& y, int& m, int& d, int& hh, int& mi, int& ss, int& dow) {
    long long days = static_cast<long long>(std::floor(serial));
    const double frac = serial - static_cast<double>(days);
    dow = static_cast<int>(((days % 7) + 7 + 6) % 7);     // serial 1 = Sunday 1900-01-01 (Excel's calendar)
    if (days < 60) days += 1;
    const long long z = days - 25569 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++yy;
    y = static_cast<int>(yy);
    long long secs = std::llround(frac * 86400.0);
    if (secs >= 86400) secs = 86399;
    hh = static_cast<int>(secs / 3600); mi = static_cast<int>((secs / 60) % 60); ss = static_cast<int>(secs % 60);
}

std::wstring FormatDate(double v, const std::string& f) {
    if (v < 0 || v >= 2958466) return L"#######";
    int y, m, d, hh, mi, ss, dow;
    CivilDate(v, y, m, d, hh, mi, ss, dow);
    static const wchar_t* kMon[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August",
                                    L"September", L"October", L"November", L"December"};
    static const wchar_t* kDay[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
    const bool ampm = f.find("AM/PM") != std::string::npos || f.find("am/pm") != std::string::npos || f.find("A/P") != std::string::npos;
    std::wstring out;
    bool lastWasHour = false;
    for (size_t i = 0; i < f.size();) {
        const char c = f[i];
        size_t n = 1;
        while (i + n < f.size() && std::tolower(static_cast<unsigned char>(f[i + n])) == std::tolower(static_cast<unsigned char>(c))) ++n;
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        wchar_t b[32];
        if (c == '"') { const size_t e = f.find('"', i + 1); out += Widen(f.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1)); i = e == std::string::npos ? f.size() : e + 1; continue; }
        if (c == '\\' && i + 1 < f.size()) { out += static_cast<wchar_t>(static_cast<unsigned char>(f[i + 1])); i += 2; continue; }
        if (c == '[') { const size_t e = f.find(']', i); const std::string in = f.substr(i + 1, e - i - 1);
                        if (!in.empty() && (std::tolower(static_cast<unsigned char>(in[0])) == 'h')) { swprintf_s(b, L"%0*d", static_cast<int>(std::min<size_t>(in.size(), 10)), static_cast<int>(std::floor(v * 24))); out += b; lastWasHour = true; }
                        i = e == std::string::npos ? f.size() : e + 1; continue; }
        if (f.compare(i, 5, "AM/PM") == 0 || f.compare(i, 5, "am/pm") == 0) { out += hh < 12 ? L"AM" : L"PM"; i += 5; continue; }
        if (f.compare(i, 3, "A/P") == 0 || f.compare(i, 3, "a/p") == 0) { out += hh < 12 ? L"A" : L"P"; i += 3; continue; }
        if (lc == 'y') { swprintf_s(b, n >= 3 ? L"%04d" : L"%02d", n >= 3 ? y : y % 100); out += b; }
        else if (lc == 'd') { if (n >= 4) out += kDay[dow]; else if (n == 3) out += std::wstring(kDay[dow]).substr(0, 3); else { swprintf_s(b, n == 2 ? L"%02d" : L"%d", d); out += b; } }
        else if (lc == 'h') { int h = hh; if (ampm) { h = hh % 12; if (!h) h = 12; } swprintf_s(b, n >= 2 ? L"%02d" : L"%d", h); out += b; lastWasHour = true; i += n; continue; }
        else if (lc == 's') { swprintf_s(b, n >= 2 ? L"%02d" : L"%d", ss); out += b; }
        else if (lc == 'm') {
            // minutes right after an hour or right before seconds; otherwise month
            size_t j = i + n; while (j < f.size() && !std::isalpha(static_cast<unsigned char>(f[j]))) ++j;
            const bool minute = n <= 2 && (lastWasHour || (j < f.size() && std::tolower(static_cast<unsigned char>(f[j])) == 's'));
            if (minute) { swprintf_s(b, n == 2 ? L"%02d" : L"%d", mi); out += b; }
            else if (n >= 5) out += std::wstring(1, kMon[m - 1][0]);
            else if (n == 4) out += kMon[m - 1];
            else if (n == 3) out += std::wstring(kMon[m - 1]).substr(0, 3);
            else { swprintf_s(b, n == 2 ? L"%02d" : L"%d", m); out += b; }
        }
        else if (c == '0' && !out.empty()) out += std::wstring(std::min<size_t>(n, 10), L'0');            // fraction of a second (rare)
        else out += std::wstring(n, static_cast<wchar_t>(static_cast<unsigned char>(c)));
        if (lc != 'h' && std::isalpha(static_cast<unsigned char>(c))) lastWasHour = false;
        i += n;
    }
    return out;
}

bool HasDateTokens(const std::string& f) {
    bool q = false;
    for (size_t i = 0; i < f.size(); ++i) {
        const char c = f[i];
        if (c == '"') q = !q;
        else if (q) continue;
        else if (c == '\\') ++i;
        else if (c == '[') { const size_t e = f.find(']', i); if (e != std::string::npos && e > i + 1 && strchr("hHmMsS", f[i + 1])) return true; i = e == std::string::npos ? f.size() : e; }
        else if (strchr("yYdDhHsS", c)) return true;
        else if ((c == 'm' || c == 'M')) return true;
        else if (c == '0' || c == '#' || c == '?') return false;
    }
    return false;
}

std::wstring FormatPlainNumber(double v, const std::string& f) {
    // Parse placeholders.
    int intZeros = 0, decZeros = 0, decAll = 0, percent = 0, scale = 0, expDigits = 0;
    bool thousands = false, inDec = false, sci = false, expPlus = false, sawDigit = false;
    std::vector<std::pair<bool, std::wstring>> parts;   // (isNumber, text)
    std::wstring lit;
    bool numberPlaced = false;
    for (size_t i = 0; i < f.size(); ++i) {
        const char c = f[i];
        if (c == '"') { const size_t e = f.find('"', i + 1); lit += Widen(f.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1)); i = e == std::string::npos ? f.size() : e; continue; }
        if (c == '\\' && i + 1 < f.size()) { lit += static_cast<wchar_t>(static_cast<unsigned char>(f[++i])); continue; }
        if (c == '_' && i + 1 < f.size()) { lit += L' '; ++i; continue; }          // space as wide as the next char
        if (c == '*' && i + 1 < f.size()) { ++i; continue; }                        // fill character: ignored
        if (c == '[') { const size_t e = f.find(']', i);
                        const std::string in = f.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
                        if (in.size() > 1 && in[0] == '$') { const size_t dash = in.find('-'); lit += Widen(in.substr(1, dash == std::string::npos ? std::string::npos : dash - 1)); }
                        i = e == std::string::npos ? f.size() : e; continue; }
        if (sci && (c == '0' || c == '#')) { ++expDigits; continue; }
        if (c == '0' || c == '#' || c == '?') {
            if (!numberPlaced) { parts.push_back({false, lit}); lit.clear(); parts.push_back({true, L""}); numberPlaced = true; }
            sawDigit = true;
            if (inDec) { ++decAll; if (c != '#') ++decZeros; } else if (c != '#') ++intZeros;
            continue;
        }
        if (c == '.' && !inDec && (sawDigit || (i + 1 < f.size() && strchr("0#?", f[i + 1])))) {
            if (!numberPlaced) { parts.push_back({false, lit}); lit.clear(); parts.push_back({true, L""}); numberPlaced = true; }
            inDec = true; continue;
        }
        if (c == ',' && sawDigit) {
            const bool more = i + 1 < f.size() && strchr("0#?", f[i + 1]);
            if (more && !inDec) thousands = true; else ++scale;
            continue;
        }
        if (c == '%') { ++percent; lit += L'%'; continue; }
        if ((c == 'E' || c == 'e') && i + 1 < f.size() && (f[i + 1] == '+' || f[i + 1] == '-') && sawDigit) { sci = true; expPlus = f[i + 1] == '+'; ++i; continue; }
        if (c == '@') continue;
        lit += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    parts.push_back({false, lit});
    // Hostile format codes ("0" a million times): Excel itself caps digits; so do we (no huge output, no long loops).
    intZeros = std::min(intZeros, 30);
    decZeros = std::min(decZeros, 15);
    decAll = std::min(decAll, 15);
    expDigits = std::min(expDigits, 10);
    percent = std::min(percent, 4);
    scale = std::min(scale, 10);
    double x = std::abs(v);
    for (int k = 0; k < percent; ++k) x *= 100;
    for (int k = 0; k < scale; ++k) x /= 1000;
    std::wstring num;
    // Big enough for any double with 15 decimals (1e308 has 309 digits): a hostile cell value must
    // never overflow it (swprintf_s would stop the program).
    wchar_t b[512];
    if (sci) {
        int e = x == 0 ? 0 : static_cast<int>(std::floor(std::log10(x)));
        const int lead = std::max(1, intZeros);
        e -= lead - 1;
        double mant = x / std::pow(10.0, e);
        swprintf_s(b, L"%.*f", decAll, mant);
        if (std::wcstod(b, nullptr) >= std::pow(10.0, lead)) { ++e; mant = x / std::pow(10.0, e); swprintf_s(b, L"%.*f", decAll, mant); }
        num = b;
        wchar_t eb[32];
        swprintf_s(eb, L"E%s%0*d", e < 0 ? L"-" : (expPlus ? L"+" : L""), std::max(1, expDigits), std::abs(e));
        num += eb;
    } else {
        swprintf_s(b, L"%.*f", std::min(decAll, 15), x);
        std::wstring s = b;
        std::wstring ip = s.substr(0, s.find(L'.')), dp = s.find(L'.') == std::wstring::npos ? L"" : s.substr(s.find(L'.') + 1);
        while (static_cast<int>(dp.size()) > decZeros && !dp.empty() && dp.back() == L'0') dp.pop_back();
        if (ip == L"0" && intZeros == 0) ip.clear();
        while (static_cast<int>(ip.size()) < intZeros) ip.insert(0, L"0");
        if (thousands && ip.size() > 3) {
            std::wstring g;
            for (size_t k = 0; k < ip.size(); ++k) { if (k && (ip.size() - k) % 3 == 0) g += L','; g += ip[k]; }
            ip = g;
        }
        num = ip + (dp.empty() && decZeros == 0 ? L"" : L"." + dp);
    }
    std::wstring out;
    for (auto& [isNum, t] : parts) out += isNum ? num : t;
    return out;
}

} // namespace

std::wstring FormatExcelNumber(double v, int fmtId, const std::string& codeIn, uint32_t* color) {
    if (color) *color = kNoColor;
    if (!std::isfinite(v)) return L"#NUM!";
    std::string code = codeIn.empty() ? BuiltinFormat(fmtId) : codeIn;
    if (code == "\x01" "date" || code == "\x01" "datetime") {      // Windows short date (+ time), like Excel
        if (v < 0 || v >= 2958466) return L"#######";
        int y, m, d, hh, mi, ss, dow;
        CivilDate(v, y, m, d, hh, mi, ss, dow);
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y); st.wMonth = static_cast<WORD>(m); st.wDay = static_cast<WORD>(d);
        st.wHour = static_cast<WORD>(hh); st.wMinute = static_cast<WORD>(mi);
        wchar_t b[128]{};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, b, 128, nullptr);
        std::wstring s = b;
        if (code == "\x01" "datetime") {
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, b, 128);
            s += L" "; s += b;
        }
        return s;
    }
    if (code.empty() || _stricmp(code.c_str(), "General") == 0) return General(v);
    // Sections: positive;negative;zero;text (quotes and brackets protect ';').
    std::vector<std::string> sec;
    {
        std::string cur; bool q = false; int br = 0;
        for (char c : code) {
            if (c == '"') q = !q;
            if (!q && c == '[') ++br;
            if (!q && c == ']' && br) --br;
            if (c == ';' && !q && !br) { sec.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        sec.push_back(cur);
    }
    std::string f = sec[0];
    bool autoMinus = v < 0;
    if (sec.size() >= 2 && v < 0) { f = sec[1]; autoMinus = false; }
    else if (sec.size() >= 3 && v == 0) f = sec[2];
    // Colours / conditions in brackets at the start of the section.
    static const struct { const char* n; uint32_t c; } kColors[] = {{"red", 0xFF0000}, {"blue", 0x0000FF}, {"green", 0x00FF00},
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"yellow", 0xFFFF00}};
    std::string clean;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '[') {
            const size_t e = f.find(']', i);
            if (e == std::string::npos) break;
            std::string in = f.substr(i + 1, e - i - 1);
            std::string low = in; for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool known = false;
            for (auto& k : kColors) if (low == k.n) { if (color) *color = k.c; known = true; }
            if (!known && (low.rfind("color", 0) == 0 || low[0] == '<' || low[0] == '>' || low[0] == '=')) known = true;   // [Color10] / conditions: skip
            if (!known) clean += f.substr(i, e - i + 1);                       // [$€-x], [h] ... kept
            i = e;
            continue;
        }
        clean += f[i];
        if (f[i] == '"') { const size_t e = f.find('"', i + 1); if (e == std::string::npos) break; clean += f.substr(i + 1, e - i); i = e; }
    }
    std::wstring s = HasDateTokens(clean) ? FormatDate(v, clean) : FormatPlainNumber(v, clean);
    if (autoMinus && s.find_first_of(L"123456789") != std::wstring::npos) s.insert(0, L"-");
    return s;
}

namespace {
} // namespace

// ============================================================ public API

std::wstring ExcelDate(double serial, bool withTime) {
    // Day 1 = 1900-01-01; Excel wrongly counts 1900-02-29 (serial 60).
    long long days = static_cast<long long>(std::floor(serial));
    double frac = serial - static_cast<double>(days);
    if (days < 60) days += 1;
    // 1899-12-30 + days, civil-from-days (Howard Hinnant).
    long long z = days - 25569 + 719468;   // 25569 = serial of 1970-01-01
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    wchar_t b[64];
    if (withTime) {
        long long secs = static_cast<long long>(std::llround(frac * 86400.0));
        if (secs >= 86400) secs = 86399;
        swprintf_s(b, L"%04lld-%02u-%02u %02lld:%02lld", y, m, d, secs / 3600, (secs / 60) % 60);
    } else {
        swprintf_s(b, L"%04lld-%02u-%02u", y, m, d);
    }
    return b;
}

int ColumnIndex(const std::wstring& ref) {
    int c = 0, n = 0;
    for (wchar_t ch : ref) {
        if (ch >= L'A' && ch <= L'Z') c = c * 26 + (ch - L'A' + 1);
        else if (ch >= L'a' && ch <= L'z') c = c * 26 + (ch - L'a' + 1);
        else break;
        if (++n > 3) return -1;
    }
    return n ? c - 1 : -1;
}

Result<std::string> DocxToRtf(const Bytes& file, uint32_t maxWidthTwips) {
    auto p = Package::Open(file);
    if (!p) return p.error();
    try {
        DocxConverter c(p.value(), std::clamp<long long>(maxWidthTwips, 1440, 20000));
        return c.Run();
    } catch (const std::bad_alloc&) {
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This document is too large to preview.", L"bad_alloc"};
    }
}

namespace {

double Dbl(const char* s, double def) {
    if (!s || !*s) return def;
    char* end = nullptr;
    const double v = strtod(s, &end);
    return (end == s || !std::isfinite(v)) ? def : v;
}

// "B12" -> row 11, col 1 (0-based).
bool CellRef(const std::string& ref, uint32_t& row, uint32_t& col) {
    size_t i = 0;
    long long c = 0;
    while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i])) && i < 3) {
        c = c * 26 + (std::toupper(static_cast<unsigned char>(ref[i])) - 'A' + 1); ++i;
    }
    if (!c || i >= ref.size()) return false;
    long long r = 0;
    for (; i < ref.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(ref[i]))) return false;
        r = r * 10 + (ref[i] - '0');
        if (r > 2'000'000) return false;
    }
    if (!r) return false;
    row = static_cast<uint32_t>(r - 1);
    col = static_cast<uint32_t>(c - 1);
    return true;
}

// Excel's theme colour index order: lt1, dk1, lt2, dk2, accent1..6, hlink, folHlink (Office 2013+ default theme).
std::vector<uint32_t> DefaultTheme() {
    return {0xFFFFFF, 0x000000, 0xE7E6E6, 0x44546A, 0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72};
}

uint32_t HexRgb(const char* s) {
    const size_t n = strlen(s);
    if (n != 6 && n != 8) return kNoColor;
    char* end = nullptr;
    const unsigned long v = strtoul(s + (n == 8 ? 2 : 0), &end, 16);       // ARGB: alpha ignored
    return (end && *end == 0) ? static_cast<uint32_t>(v & 0xFFFFFF) : kNoColor;
}

void ReadTheme(const pugi::xml_document& d, std::vector<uint32_t>& theme) {
    // clrScheme order: dk1 lt1 dk2 lt2 accent1..6 hlink folHlink  ->  Excel order (lt1 dk1 lt2 dk2 ...)
    static const int kMap[12] = {1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11};
    auto scheme = Find(d, "clrScheme");
    int k = 0;
    for (auto c : scheme.children()) {
        if (c.type() != pugi::node_element || k >= 12) continue;
        uint32_t rgb = kNoColor;
        if (auto s = Child(c, "srgbClr")) rgb = HexRgb(Attr(s, "val"));
        else if (auto sc = Child(c, "sysClr")) rgb = HexRgb(Attr(sc, "lastClr"));
        if (rgb != kNoColor) theme[static_cast<size_t>(kMap[k])] = rgb;
        ++k;
    }
}

// Lighter / darker shade (ECMA-376 tint on the HLS lightness).
uint32_t ApplyTint(uint32_t rgb, double tint) {
    if (tint == 0 || rgb == kNoColor) return rgb;
    tint = std::clamp(tint, -1.0, 1.0);
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    double h = 0, s = 0, l = (mx + mn) / 2;
    if (mx != mn) {
        const double dlt = mx - mn;
        s = l > 0.5 ? dlt / (2 - mx - mn) : dlt / (mx + mn);
        if (mx == r) h = (g - b) / dlt + (g < b ? 6 : 0); else if (mx == g) h = (b - r) / dlt + 2; else h = (r - g) / dlt + 4;
        h /= 6;
    }
    l = tint < 0 ? l * (1 + tint) : l * (1 - tint) + tint;
    auto hue = [](double p, double q, double t) {
        if (t < 0) t += 1; if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    if (s == 0) { r = g = b = l; }
    else {
        const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue(p, q, h + 1.0 / 3); g = hue(p, q, h); b = hue(p, q, h - 1.0 / 3);
    }
    auto to8 = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    return (to8(r) << 16) | (to8(g) << 8) | to8(b);
}

// <color rgb="FFRRGGBB"/> | theme="4" tint="-0.25" | indexed="10" | auto="1"
uint32_t ReadColor(const pugi::xml_node& n, const std::vector<uint32_t>& theme) {
    static const uint32_t kIndexed[64] = {
        0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
        0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
        0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080,
        0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF,
        0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF,
        0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99,
        0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699, 0x969696,
        0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333};
    if (!n) return kNoColor;
    uint32_t c = kNoColor;
    if (*Attr(n, "rgb")) c = HexRgb(Attr(n, "rgb"));
    else if (*Attr(n, "theme")) { const long long t = Num(Attr(n, "theme"), -1); if (t >= 0 && static_cast<size_t>(t) < theme.size()) c = theme[static_cast<size_t>(t)]; }
    else if (*Attr(n, "indexed")) {
        const long long i = Num(Attr(n, "indexed"), -1);
        if (i >= 0 && i < 64) c = kIndexed[i]; else if (i == 64) c = 0x000000; else if (i == 65) c = 0xFFFFFF;
    }
    if (c != kNoColor && *Attr(n, "tint")) c = ApplyTint(c, Dbl(Attr(n, "tint"), 0));
    return c;
}

CellBorder ReadBorderSide(const pugi::xml_node& side, const std::vector<uint32_t>& theme) {
    CellBorder b;
    if (!side) return b;
    const std::string s = Attr(side, "style");
    if (s.empty() || s == "none") return b;
    if (s == "medium" || s == "mediumDashed" || s == "mediumDashDot" || s == "mediumDashDotDot" || s == "slantDashDot") b.style = 2;
    else if (s == "thick") b.style = 3;
    else if (s == "double") b.style = 4;
    else b.style = 1;                                    // thin, hair, dotted, dashed ...
    const uint32_t c = ReadColor(Child(side, "color"), theme);
    b.color = c == kNoColor ? 0x000000 : c;
    return b;
}

void ReadStyles(const pugi::xml_node& ss, const std::vector<uint32_t>& theme, Sheet& out,
                std::vector<int>& xfFmt, std::map<int, std::string>& customFmt) {
    for (auto f : Child(ss, "numFmts").children())
        if (Is(f, "numFmt")) customFmt[static_cast<int>(Num(Attr(f, "numFmtId")))] = Attr(f, "formatCode");

    std::vector<CellStyle> fonts;                        // font fields only
    for (auto f : Child(ss, "fonts").children()) {
        if (!Is(f, "font")) continue;
        CellStyle s;
        s.bold = OnOff(Child(f, "b")); s.italic = OnOff(Child(f, "i")); s.strike = OnOff(Child(f, "strike"));
        if (auto u = Child(f, "u")) s.underline = strcmp(Attr(u, "val"), "none") != 0;
        if (auto sz = Child(f, "sz")) s.size = static_cast<float>(std::clamp(Dbl(Attr(sz, "val"), 11), 1.0, 409.0));
        s.color = ReadColor(Child(f, "color"), theme);
        std::wstring name = W(Attr(Child(f, "name"), "val"));
        if (name.empty() || name.size() > 64) name = L"Calibri";
        auto it = std::find(out.fontNames.begin(), out.fontNames.end(), name);
        if (it == out.fontNames.end() && out.fontNames.size() < 1000) { out.fontNames.push_back(name); it = out.fontNames.end() - 1; }
        s.font = it == out.fontNames.end() ? uint16_t(0) : static_cast<uint16_t>(it - out.fontNames.begin());
        fonts.push_back(s);
        if (fonts.size() > 10000) break;
    }
    std::vector<uint32_t> fills;
    for (auto f : Child(ss, "fills").children()) {
        if (!Is(f, "fill")) continue;
        uint32_t c = kNoColor;
        if (auto p = Child(f, "patternFill")) {
            const std::string t = Attr(p, "patternType");
            if (t == "solid") c = ReadColor(Child(p, "fgColor"), theme);
            else if (!t.empty() && t != "none" && t != "gray125" && t != "gray0625") {   // patterns: a lighter shade
                c = ReadColor(Child(p, "fgColor"), theme);
                if (c != kNoColor) c = ApplyTint(c, 0.5);
            }
        } else if (auto gr = Child(f, "gradientFill")) {
            if (auto stop = Child(gr, "stop")) c = ReadColor(Child(stop, "color"), theme);
        }
        fills.push_back(c);
        if (fills.size() > 10000) break;
    }
    std::vector<std::array<CellBorder, 4>> borders;      // left right top bottom
    for (auto b : Child(ss, "borders").children()) {
        if (!Is(b, "border")) continue;
        borders.push_back({ReadBorderSide(Child(b, "left"), theme), ReadBorderSide(Child(b, "right"), theme),
                           ReadBorderSide(Child(b, "top"), theme), ReadBorderSide(Child(b, "bottom"), theme)});
        // Some writers use start / end instead of left / right.
        if (!borders.back()[0].style) borders.back()[0] = ReadBorderSide(Child(b, "start"), theme);
        if (!borders.back()[1].style) borders.back()[1] = ReadBorderSide(Child(b, "end"), theme);
        if (borders.size() > 10000) break;
    }
    out.styles.clear();
    for (auto xf : Child(ss, "cellXfs").children()) {
        if (!Is(xf, "xf")) continue;
        xfFmt.push_back(static_cast<int>(Num(Attr(xf, "numFmtId"))));
        CellStyle s;
        const long long fo = Num(Attr(xf, "fontId"), 0), fi = Num(Attr(xf, "fillId"), 0), bo = Num(Attr(xf, "borderId"), 0);
        if (fo >= 0 && static_cast<size_t>(fo) < fonts.size()) s = fonts[static_cast<size_t>(fo)];
        if (fi >= 0 && static_cast<size_t>(fi) < fills.size()) s.fill = fills[static_cast<size_t>(fi)];
        if (bo >= 0 && static_cast<size_t>(bo) < borders.size()) {
            const auto& b = borders[static_cast<size_t>(bo)];
            s.left = b[0]; s.right = b[1]; s.top = b[2]; s.bottom = b[3];
        }
        if (auto al = Child(xf, "alignment")) {
            const std::string h = Attr(al, "horizontal"), v = Attr(al, "vertical");
            s.hAlign = h == "left" ? 1 : h == "center" ? 2 : h == "right" ? 3 : h == "fill" ? 4 : h == "justify" || h == "distributed" ? 5
                     : h == "centerContinuous" ? 6 : 0;
            s.vAlign = v == "top" ? 0 : v == "center" || v == "justify" || v == "distributed" ? 1 : 2;
            s.wrap = strcmp(Attr(al, "wrapText"), "1") == 0 || strcmp(Attr(al, "wrapText"), "true") == 0;
            s.indent = static_cast<uint8_t>(std::clamp<long long>(Num(Attr(al, "indent"), 0), 0, 50));
        }
        out.styles.push_back(s);
        if (out.styles.size() >= 65000) break;
    }
}

} // namespace

Result<Sheet> ReadXlsxSheet(const Bytes& file, uint32_t sheetIndex) {
    auto pr = Package::Open(file);
    if (!pr) return pr.error();
    Package& pkg = pr.value();
    try {
        pugi::xml_document wb;
        if (!pkg.Xml(L"xl/workbook.xml", wb))
            return ErrorInfo{ErrorCode::Corrupted, L"This Excel file could not be read (it may be damaged or an old .xls file).", L""};
        const auto rels = pkg.Rels(L"xl/workbook.xml");
        Sheet out;
        std::vector<std::wstring> paths;
        for (auto s : Find(wb, "sheets").children()) {
            if (!Is(s, "sheet")) continue;
            out.sheetNames.push_back(W(Attr(s, "name")));
            auto it = rels.find(RelId(s));
            paths.push_back(it == rels.end() ? L"" : it->second);
        }
        if (paths.empty()) return ErrorInfo{ErrorCode::Corrupted, L"This workbook has no sheets.", L""};
        out.index = std::min<uint32_t>(sheetIndex, static_cast<uint32_t>(paths.size() - 1));
        const std::wstring path = paths[out.index];
        if (path.find(L"worksheets/") == std::wstring::npos) {
            out.note = L"This is a chart or dialog sheet (not previewed).";
            return out;
        }

        // Shared strings.
        std::vector<std::wstring> sst;
        {
            pugi::xml_document d;
            std::wstring sstPath = L"xl/sharedStrings.xml";
            for (const auto& [id, target] : rels) if (target.find(L"sharedStrings") != std::wstring::npos) sstPath = target;
            if (pkg.Xml(sstPath, d))
                for (auto si : Child(d, "sst").children()) {
                    if (!Is(si, "si")) continue;
                    std::wstring s;
                    // all <t> except phonetic hints (<rPh>)
                    std::function<void(const pugi::xml_node&)> walk = [&](const pugi::xml_node& n) {
                        for (auto c : n.children()) {
                            if (Is(c, "rPh")) continue;
                            if (Is(c, "t")) s += W(c.text().get());
                            else walk(c);
                        }
                    };
                    walk(si);
                    sst.push_back(std::move(s));
                    if (sst.size() > 2'000'000) break;
                }
        }
        // Theme colours (fills and fonts often use them: theme="4" tint="0.79").
        std::vector<uint32_t> theme = DefaultTheme();
        {
            std::wstring themePath = L"xl/theme/theme1.xml";
            for (const auto& [id, target] : rels) if (target.find(L"theme/") != std::wstring::npos) themePath = target;
            pugi::xml_document d;
            if (pkg.Xml(themePath, d)) ReadTheme(d, theme);
        }
        // Styles: number format + font + fill + border + alignment per cell style index.
        std::vector<int> xfFmt;
        std::map<int, std::string> customFmt;
        out.fontNames.push_back(L"Calibri");
        out.styles.push_back(CellStyle{});
        {
            pugi::xml_document d;
            if (pkg.Xml(L"xl/styles.xml", d)) ReadStyles(Child(d, "styleSheet"), theme, out, xfFmt, customFmt);
            if (out.styles.empty()) out.styles.push_back(CellStyle{});
        }

        pugi::xml_document sh;
        if (!pkg.Xml(path, sh)) return ErrorInfo{ErrorCode::Corrupted, L"This sheet could not be read.", path};
        if (auto sv = Find(sh, "sheetView")) {
            const char* r = Attr(sv, "rightToLeft");
            out.rightToLeft = strcmp(r, "1") == 0 || strcmp(r, "true") == 0;
            const char* g = Attr(sv, "showGridLines");
            out.showGridLines = !(strcmp(g, "0") == 0 || strcmp(g, "false") == 0);
        }
        if (auto fp = Find(sh, "sheetFormatPr")) {
            if (*Attr(fp, "defaultRowHeight")) out.defaultRowHeight = static_cast<float>(std::clamp(Dbl(Attr(fp, "defaultRowHeight"), 15), 1.0, 409.0));
            if (*Attr(fp, "defaultColWidth")) out.defaultColWidth = static_cast<float>(std::clamp(Dbl(Attr(fp, "defaultColWidth"), 8.43), 0.5, 255.0));
            else if (*Attr(fp, "baseColWidth")) out.defaultColWidth = static_cast<float>(std::clamp(Dbl(Attr(fp, "baseColWidth"), 8) + 0.71, 0.5, 255.0));
        }
        struct ColDef { uint32_t a, b; float w; };
        std::vector<ColDef> colDefs;
        for (auto col : Find(sh, "cols").children()) {
            if (!Is(col, "col")) continue;
            const long long a = Num(Attr(col, "min"), 0), b = Num(Attr(col, "max"), a);
            if (a < 1 || b < a) continue;
            const bool hidden = strcmp(Attr(col, "hidden"), "1") == 0 || strcmp(Attr(col, "hidden"), "true") == 0;
            const float w = hidden ? 0.0f : static_cast<float>(std::clamp(Dbl(Attr(col, "width"), out.defaultColWidth), 0.0, 255.0));
            colDefs.push_back({static_cast<uint32_t>(std::min<long long>(a, kMaxSheetCols + 1)), static_cast<uint32_t>(std::min<long long>(b, kMaxSheetCols)), w});
        }

        auto styleOf = [&](long long s) { return (s >= 0 && static_cast<size_t>(s) < out.styles.size()) ? static_cast<uint16_t>(s) : uint16_t(0); };
        auto visible = [&](uint16_t s) {                   // a style that shows even without a value
            const CellStyle& cs = out.styles[s];
            return cs.fill != kNoColor || cs.left.style || cs.right.style || cs.top.style || cs.bottom.style;
        };
        struct Cell { uint32_t r, c; std::wstring v; uint16_t style; uint8_t kind; uint32_t color; };
        std::vector<Cell> cells;
        std::vector<std::pair<uint32_t, float>> rowHts;
        uint32_t maxR = 0, maxC = 0;
        uint32_t rowNo = 0;
        bool anyValue = false;
        for (auto row : Find(sh, "sheetData").children()) {
            if (!Is(row, "row")) continue;
            rowNo = static_cast<uint32_t>(std::max<long long>(rowNo + 1, Num(Attr(row, "r"), rowNo + 1)));
            if (rowNo > kMaxSheetRows) { out.truncated = true; break; }
            const bool rowHidden = strcmp(Attr(row, "hidden"), "1") == 0 || strcmp(Attr(row, "hidden"), "true") == 0;
            if (rowHidden) rowHts.push_back({rowNo - 1, 0.0f});
            else if (*Attr(row, "ht")) rowHts.push_back({rowNo - 1, static_cast<float>(std::clamp(Dbl(Attr(row, "ht"), out.defaultRowHeight), 0.0, 409.0))});
            int col = -1;
            for (auto c : row.children()) {
                if (!Is(c, "c")) continue;
                const int ci = ColumnIndex(W(Attr(c, "r")));
                col = ci >= 0 ? ci : col + 1;
                if (col >= static_cast<int>(kMaxSheetCols)) { out.truncated = true; continue; }
                const uint16_t st = styleOf(Num(Attr(c, "s"), 0));
                const std::string t = Attr(c, "t");
                const char* v = Child(c, "v").text().get();
                std::wstring s;
                uint8_t kind = 0;
                uint32_t color = kNoColor;
                if (t == "s") { const long long k = Num(v, -1); if (k >= 0 && static_cast<size_t>(k) < sst.size()) s = sst[static_cast<size_t>(k)]; }
                else if (t == "inlineStr") { for (auto tt : Child(c, "is").children()) if (Is(tt, "t")) s += W(tt.text().get()); else if (Is(tt, "r")) s += W(Child(tt, "t").text().get()); }
                else if (t == "str") s = W(v);
                else if (t == "e") { s = W(v); kind = 3; }
                else if (t == "b") { s = strcmp(v, "1") == 0 ? L"TRUE" : L"FALSE"; kind = 2; }
                else if (*v) {
                    const int fid = st < xfFmt.size() ? xfFmt[st] : 0;
                    auto cf = customFmt.find(fid);
                    s = FormatExcelNumber(strtod(v, nullptr), fid, cf == customFmt.end() ? std::string() : cf->second, &color);
                    kind = 1;
                }
                if (s.empty() && !visible(st)) continue;
                if (s.size() > 1000) s = s.substr(0, 1000) + L"…";
                if (!s.empty()) anyValue = true;
                maxR = std::max(maxR, rowNo);
                maxC = std::max(maxC, static_cast<uint32_t>(col + 1));
                cells.push_back({rowNo - 1, static_cast<uint32_t>(col), std::move(s), st, kind, color});
            }
        }
        for (auto mc : Find(sh, "mergeCells").children()) {
            if (!Is(mc, "mergeCell")) continue;
            const std::string ref = Attr(mc, "ref");
            const size_t colon = ref.find(':');
            if (colon == std::string::npos) continue;
            CellMerge m;
            if (!CellRef(ref.substr(0, colon), m.r0, m.c0) || !CellRef(ref.substr(colon + 1), m.r1, m.c1)) continue;
            if (m.r1 < m.r0 || m.c1 < m.c0 || m.r0 >= kMaxSheetRows || m.c0 >= kMaxSheetCols) continue;
            m.r1 = std::min<uint32_t>(m.r1, kMaxSheetRows - 1);
            m.c1 = std::min<uint32_t>(m.c1, kMaxSheetCols - 1);
            if (out.merges.size() < 100000) out.merges.push_back(m);
        }
        // A merged block only counts when something is inside it.
        for (const auto& m : out.merges)
            for (const auto& c : cells)
                if (c.r == m.r0 && c.c == m.c0) { maxR = std::max(maxR, m.r1 + 1); maxC = std::max(maxC, m.c1 + 1); break; }

        out.rows = maxR;
        out.cols = maxC;
        const size_t n = static_cast<size_t>(maxR) * maxC;
        out.cells.assign(n, std::wstring());
        out.cellStyle.assign(n, 0);
        out.cellKind.assign(n, 0);
        out.cellColor.assign(n, kNoColor);
        for (auto& c : cells) {
            if (c.r >= maxR || c.c >= maxC) continue;
            const size_t k = static_cast<size_t>(c.r) * maxC + c.c;
            out.cells[k] = std::move(c.v);
            out.cellStyle[k] = c.style;
            out.cellKind[k] = c.kind;
            out.cellColor[k] = c.color;
        }
        out.colWidth.assign(maxC, out.defaultColWidth);
        for (const auto& cd : colDefs)
            for (uint32_t k = cd.a; k <= cd.b && k <= maxC; ++k) out.colWidth[k - 1] = cd.w;
        out.rowHeight.assign(maxR, out.defaultRowHeight);
        for (const auto& [r, h] : rowHts) if (r < maxR) out.rowHeight[r] = h;
        if (!anyValue && out.note.empty()) out.note = L"This sheet is empty.";
        return out;
    } catch (const std::bad_alloc&) {
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This workbook is too large to preview.", L"bad_alloc"};
    }
}

// ============================================================ PPTX

namespace {

struct Xfrm { long long x = 0, y = 0, cx = 0, cy = 0; bool ok = false; };
Xfrm ReadXfrm(const pugi::xml_node& x) {
    Xfrm f;
    if (!x) return f;
    auto off = Child(x, "off"), ext = Child(x, "ext");
    if (!off || !ext) return f;
    f.x = Num(Attr(off, "x")); f.y = Num(Attr(off, "y"));
    f.cx = Num(Attr(ext, "cx")); f.cy = Num(Attr(ext, "cy"));
    f.ok = f.cx > 0 && f.cy > 0;
    return f;
}

// Group transform: child coordinates -> slide coordinates.
struct Tx {
    double ox = 0, oy = 0, sx = 1, sy = 1;
    Xfrm Map(const Xfrm& f) const {
        Xfrm r = f;
        r.x = static_cast<long long>(ox + f.x * sx); r.y = static_cast<long long>(oy + f.y * sy);
        r.cx = static_cast<long long>(f.cx * sx);    r.cy = static_cast<long long>(f.cy * sy);
        return r;
    }
};

COLORREF SchemeColor(const std::string& name) {
    // Office default theme (themes are not read in this version).
    if (name == "bg1" || name == "lt1") return RGB(255, 255, 255);
    if (name == "tx2" || name == "dk2") return RGB(0x44, 0x54, 0x6A);
    if (name == "bg2" || name == "lt2") return RGB(0xE7, 0xE6, 0xE6);
    if (name == "accent1") return RGB(0x44, 0x72, 0xC4);
    if (name == "accent2") return RGB(0xED, 0x7D, 0x31);
    if (name == "accent3") return RGB(0xA5, 0xA5, 0xA5);
    if (name == "accent4") return RGB(0xFF, 0xC0, 0x00);
    if (name == "accent5") return RGB(0x5B, 0x9B, 0xD5);
    if (name == "accent6") return RGB(0x70, 0xAD, 0x47);
    return RGB(0, 0, 0);
}
bool FillColor(const pugi::xml_node& parent, COLORREF& out) {
    auto sf = Child(parent, "solidFill");
    if (!sf) return false;
    if (auto s = Child(sf, "srgbClr")) { const uint32_t v = Hex(Attr(s, "val"), 0); out = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); return true; }
    if (auto s = Child(sf, "schemeClr")) { out = SchemeColor(Attr(s, "val")); return true; }
    if (auto s = Child(sf, "sysClr")) { const uint32_t v = Hex(Attr(s, "lastClr"), 0); out = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); return true; }
    return false;
}

struct Placeholder { std::string type, idx; Xfrm xf; };

class SlideRenderer {
public:
    SlideRenderer(Package& p, HDC dc, double k) : pkg_(p), dc_(dc), k_(k) {}

    void Draw(const std::wstring& slidePath) {
        // Placeholder positions come from the layout, then the master, when the slide omits them.
        const auto rels = pkg_.Rels(slidePath);
        for (const auto& [id, target] : rels)
            if (target.find(L"slideLayouts/") != std::wstring::npos) {
                LoadPlaceholders(target, layout_);
                for (const auto& [id2, t2] : pkg_.Rels(target))
                    if (t2.find(L"slideMasters/") != std::wstring::npos) LoadPlaceholders(t2, master_);
            }
        pugi::xml_document d;
        if (!pkg_.Xml(slidePath, d)) return;
        rels_ = rels;
        auto sld = Child(d, "sld");
        auto cSld = Child(sld, "cSld");
        COLORREF bg;
        if (FillColor(Child(Child(cSld, "bg"), "bgPr"), bg)) {
            RECT all{0, 0, 100000, 100000};
            HBRUSH b = CreateSolidBrush(bg);
            FillRect(dc_, &all, b);
            DeleteObject(b);
        }
        Tree(Child(cSld, "spTree"), Tx{});
    }

private:
    void LoadPlaceholders(const std::wstring& part, std::vector<Placeholder>& out) {
        pugi::xml_document d;
        if (!pkg_.Xml(part, d)) return;
        std::function<void(const pugi::xml_node&)> walk = [&](const pugi::xml_node& n) {
            for (auto c : n.children()) {
                if (Is(c, "sp")) {
                    auto ph = Find(Child(c, "nvSpPr"), "ph");
                    if (!ph) continue;
                    Placeholder p{Attr(ph, "type"), Attr(ph, "idx"), ReadXfrm(Child(Child(c, "spPr"), "xfrm"))};
                    if (p.xf.ok) out.push_back(p);
                } else if (Is(c, "grpSp") || Is(c, "spTree") || Is(c, "cSld")) walk(c);
            }
        };
        walk(Child(d.first_child(), "cSld"));
    }
    static std::string Norm(const std::string& t) {
        if (t == "ctrTitle") return "title";
        if (t.empty()) return "body";
        return t;
    }
    Xfrm Inherit(const pugi::xml_node& ph) {
        const std::string type = Norm(Attr(ph, "type")), idx = Attr(ph, "idx");
        for (auto* list : {&layout_, &master_}) {
            if (!idx.empty()) for (auto& p : *list) if (p.idx == idx) return p.xf;
            for (auto& p : *list) if (Norm(p.type) == type) return p.xf;
        }
        return {};
    }

    RECT Px(const Xfrm& f) const {
        return RECT{static_cast<LONG>(f.x * k_), static_cast<LONG>(f.y * k_),
                    static_cast<LONG>((f.x + f.cx) * k_), static_cast<LONG>((f.y + f.cy) * k_)};
    }

    void Tree(const pugi::xml_node& n, const Tx& tx) {
        for (auto c : n.children()) {
            if (++shapes_ > 3000) return;
            if (Is(c, "sp")) Shape(c, tx);
            else if (Is(c, "pic")) Pic(c, tx);
            else if (Is(c, "grpSp")) {
                auto x = Child(Child(c, "grpSpPr"), "xfrm");
                Xfrm g = ReadXfrm(x);
                auto chOff = Child(x, "chOff"), chExt = Child(x, "chExt");
                Tx inner = tx;
                if (g.ok && chOff && chExt && Num(Attr(chExt, "cx")) > 0 && Num(Attr(chExt, "cy")) > 0) {
                    const Xfrm gm = tx.Map(g);
                    const double sx = static_cast<double>(gm.cx) / Num(Attr(chExt, "cx"));
                    const double sy = static_cast<double>(gm.cy) / Num(Attr(chExt, "cy"));
                    inner = Tx{gm.x - Num(Attr(chOff, "x")) * sx, gm.y - Num(Attr(chOff, "y")) * sy, sx, sy};
                }
                Tree(c, inner);
            } else if (Is(c, "graphicFrame")) TableFrame(c, tx);
            else if (Is(c, "AlternateContent")) { if (auto ch = Child(c, "Choice")) Tree(ch, tx); }
        }
    }

    void Shape(const pugi::xml_node& sp, const Tx& tx) {
        auto spPr = Child(sp, "spPr");
        Xfrm f = ReadXfrm(Child(spPr, "xfrm"));
        auto ph = Find(Child(sp, "nvSpPr"), "ph");
        if (!f.ok && ph) f = Inherit(ph);
        if (!f.ok) return;
        const RECT r = Px(tx.Map(f));
        COLORREF fill;
        if (FillColor(spPr, fill)) {
            HBRUSH b = CreateSolidBrush(fill);
            HGDIOBJ ob = SelectObject(dc_, b);
            HGDIOBJ op = SelectObject(dc_, GetStockObject(NULL_PEN));
            const std::string geom = Attr(Child(spPr, "prstGeom"), "prst");
            if (geom == "ellipse") Ellipse(dc_, r.left, r.top, r.right + 1, r.bottom + 1);
            else if (geom == "roundRect") RoundRect(dc_, r.left, r.top, r.right + 1, r.bottom + 1, (r.right - r.left) / 6, (r.bottom - r.top) / 6);
            else Rectangle(dc_, r.left, r.top, r.right + 1, r.bottom + 1);
            SelectObject(dc_, ob);
            SelectObject(dc_, op);
            DeleteObject(b);
        }
        const std::string type = ph ? Norm(Attr(ph, "type")) : "";
        const int defaultPt = type == "title" ? 40 : (type == "subTitle" || type == "body") ? 22 : 18;
        Text(Child(sp, "txBody"), r, defaultPt, type == "title");
    }

    void Text(const pugi::xml_node& body, RECT r, int defaultPt, bool title) {
        if (!body) return;
        // Insets: 0.1 in left/right, 0.05 in top/bottom.
        const int ix = static_cast<int>(91440 * k_), iy = static_cast<int>(45720 * k_);
        r.left += ix; r.right -= ix; r.top += iy; r.bottom -= iy;
        if (r.right <= r.left) return;
        struct Para { std::wstring text; HFONT font; COLORREF color; UINT align; int height; };
        std::vector<Para> paras;
        int total = 0;
        for (auto p : body.children()) {
            if (!Is(p, "p")) continue;
            std::wstring t;
            pugi::xml_node firstRpr;
            for (auto rr : p.children()) {
                if (Is(rr, "r") || Is(rr, "fld")) { t += W(Child(rr, "t").text().get()); if (!firstRpr) firstRpr = Child(rr, "rPr"); }
                else if (Is(rr, "br")) t += L"\n";
            }
            auto ppr = Child(p, "pPr");
            if (!firstRpr) firstRpr = Child(p, "endParaRPr");
            if (Child(ppr, "buChar") && !t.empty()) t = W(Attr(Child(ppr, "buChar"), "char")) + L" " + t;
            const long long sz = Num(Attr(firstRpr, "sz"), defaultPt * 100);
            const int px = std::max(4, static_cast<int>(sz / 100.0 * 12700 * k_));
            LOGFONTW lf{};
            lf.lfHeight = -px;
            lf.lfWeight = (OnOff(Child(firstRpr, "b")) || strcmp(Attr(firstRpr, "b"), "1") == 0 || title) ? FW_BOLD : FW_NORMAL;
            if (title && strcmp(Attr(firstRpr, "b"), "0") == 0) lf.lfWeight = FW_NORMAL;
            lf.lfItalic = strcmp(Attr(firstRpr, "i"), "1") == 0;
            lf.lfUnderline = *Attr(firstRpr, "u") && strcmp(Attr(firstRpr, "u"), "none") != 0;
            lf.lfQuality = ANTIALIASED_QUALITY;
            lf.lfCharSet = DEFAULT_CHARSET;
            const std::wstring face = W(Attr(Child(firstRpr, "latin"), "typeface"));
            lstrcpynW(lf.lfFaceName, (face.empty() || face[0] == L'+') ? L"Segoe UI" : face.c_str(), LF_FACESIZE);
            COLORREF col = RGB(0, 0, 0);
            FillColor(firstRpr, col);
            const std::string algn = Attr(ppr, "algn");
            UINT a = algn == "ctr" ? DT_CENTER : algn == "r" ? DT_RIGHT : DT_LEFT;
            if (strcmp(Attr(ppr, "rtl"), "1") == 0) a = (a == DT_LEFT ? DT_RIGHT : a) | DT_RTLREADING;
            Para pa{t, CreateFontIndirectW(&lf), col, a, 0};
            HGDIOBJ of = SelectObject(dc_, pa.font);
            RECT c = r;
            DrawTextW(dc_, t.empty() ? L" " : t.c_str(), -1, &c, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc_, of);
            pa.height = static_cast<int>((c.bottom - c.top) * 1.08);
            total += pa.height;
            paras.push_back(std::move(pa));
            if (paras.size() > 200) break;
        }
        const std::string anchor = Attr(Child(body, "bodyPr"), "anchor");
        int y = r.top;
        if (anchor == "ctr" || (anchor.empty() && title)) y = r.top + (r.bottom - r.top - total) / 2;
        else if (anchor == "b") y = r.bottom - total;
        SetBkMode(dc_, TRANSPARENT);
        for (auto& pa : paras) {
            HGDIOBJ of = SelectObject(dc_, pa.font);
            SetTextColor(dc_, pa.color);
            RECT c{r.left, y, r.right, y + pa.height};
            DrawTextW(dc_, pa.text.c_str(), -1, &c, pa.align | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc_, of);
            DeleteObject(pa.font);
            y += pa.height;
        }
    }

    void Pic(const pugi::xml_node& pic, const Tx& tx) {
        Xfrm f = ReadXfrm(Child(Child(pic, "spPr"), "xfrm"));
        if (!f.ok) return;
        const RECT r = Px(tx.Map(f));
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;
        auto it = rels_.find(Attr(Find(pic, "blip"), "embed"));
        std::vector<uint8_t> bytes;
        if (it != rels_.end() && imageBytes_ < (96ull << 20)) {
            auto b = pkg_.Read(it->second, kMaxMediaPart);
            if (b) bytes = std::move(b.value());
        }
        imageBytes_ += bytes.size();
        auto img = bytes.empty() ? Result<Image>(ErrorInfo{}) : DecodeImage(bytes, static_cast<uint32_t>(std::max(w, h)));
        if (!img) {
            // Unsupported picture type (e.g. EMF/WMF): a light placeholder box.
            RECT pr = r;
            FillRect(dc_, &pr, GetSysColorBrush(COLOR_3DFACE));
            return;
        }
        const Image& im = img.value();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(im.width);
        bi.bmiHeader.biHeight = -static_cast<LONG>(im.height);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP hb = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hb) return;
        memcpy(bits, im.pixels.data(), im.pixels.size());
        HDC mem = CreateCompatibleDC(dc_);
        HGDIOBJ o = SelectObject(mem, hb);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc_, r.left, r.top, w, h, mem, 0, 0, static_cast<int>(im.width), static_cast<int>(im.height), bf);
        SelectObject(mem, o);
        DeleteDC(mem);
        DeleteObject(hb);
    }

    void TableFrame(const pugi::xml_node& gf, const Tx& tx) {
        auto tbl = Find(gf, "tbl");
        Xfrm f = ReadXfrm(Child(gf, "xfrm"));
        if (!tbl || !f.ok) return;
        const RECT r = Px(tx.Map(f));
        std::vector<long long> colW;
        for (auto g : Child(tbl, "tblGrid").children()) if (Is(g, "gridCol")) colW.push_back(Num(Attr(g, "w")));
        int y = r.top;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x9A, 0x9A, 0x9A));
        HGDIOBJ op = SelectObject(dc_, pen);
        HGDIOBJ ob = SelectObject(dc_, GetStockObject(NULL_BRUSH));
        int rows = 0;
        for (auto tr : tbl.children()) {
            if (!Is(tr, "tr") || ++rows > 200) continue;
            const int h = std::max(4, static_cast<int>(Num(Attr(tr, "h"), 370840) * k_));
            int x = r.left;
            size_t ci = 0;
            for (auto tc : tr.children()) {
                if (!Is(tc, "tc")) continue;
                const int w = static_cast<int>((ci < colW.size() ? colW[ci] : 1828800) * k_);
                RECT cell{x, y, x + w, y + h};
                COLORREF fill;
                if (FillColor(Child(tc, "tcPr"), fill)) { HBRUSH b = CreateSolidBrush(fill); FillRect(dc_, &cell, b); DeleteObject(b); }
                Rectangle(dc_, cell.left, cell.top, cell.right + 1, cell.bottom + 1);
                Text(Child(tc, "txBody"), cell, 14, false);
                x += w;
                ++ci;
            }
            y += h;
        }
        SelectObject(dc_, op);
        SelectObject(dc_, ob);
        DeleteObject(pen);
    }

    Package& pkg_;
    HDC dc_;
    double k_;
    std::map<std::string, std::wstring> rels_;
    std::vector<Placeholder> layout_, master_;
    size_t shapes_ = 0;
    uint64_t imageBytes_ = 0;
};

} // namespace

Result<PdfPage> RenderPptxSlide(const Bytes& file, uint32_t slideIndex, uint32_t targetWidth) {
    auto pr = Package::Open(file);
    if (!pr) return pr.error();
    Package& pkg = pr.value();
    try {
        pugi::xml_document pres;
        if (!pkg.Xml(L"ppt/presentation.xml", pres))
            return ErrorInfo{ErrorCode::Corrupted, L"This PowerPoint file could not be read (it may be damaged or an old .ppt file).", L""};
        const auto rels = pkg.Rels(L"ppt/presentation.xml");
        std::vector<std::wstring> slides;
        for (auto s : Find(pres, "sldIdLst").children()) {
            if (!Is(s, "sldId")) continue;
            auto it = rels.find(RelId(s));
            if (it != rels.end()) slides.push_back(it->second);
        }
        if (slides.empty()) return ErrorInfo{ErrorCode::Corrupted, L"This presentation has no slides.", L""};
        auto sz = Find(pres, "sldSz");
        const long long cx = std::max<long long>(1, Num(Attr(sz, "cx"), 12192000));
        const long long cy = std::max<long long>(1, Num(Attr(sz, "cy"), 6858000));

        PdfPage out;
        out.pageCount = static_cast<uint32_t>(slides.size());
        out.pageIndex = std::min<uint32_t>(slideIndex, out.pageCount - 1);
        const int W = static_cast<int>(std::clamp<uint32_t>(targetWidth, 200, kDecodeSide));
        int H = static_cast<int>(W * static_cast<double>(cy) / cx);
        H = std::clamp(H, 1, static_cast<int>(kDecodeSide));
        const double k = static_cast<double>(W) / cx;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { DeleteDC(dc); return ErrorInfo{ErrorCode::Internal, L"Not enough memory to draw the slide.", L""}; }
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT all{0, 0, W, H};
        FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        IntersectClipRect(dc, 0, 0, W, H);
        {
            SlideRenderer sr(pkg, dc, k);
            sr.Draw(slides[out.pageIndex]);
        }
        GdiFlush();
        out.image.width = static_cast<uint32_t>(W);
        out.image.height = static_cast<uint32_t>(H);
        out.image.originalWidth = static_cast<uint32_t>(W);
        out.image.originalHeight = static_cast<uint32_t>(H);
        out.image.pixels.assign(static_cast<const uint8_t*>(bits), static_cast<const uint8_t*>(bits) + static_cast<size_t>(W) * H * 4);
        for (size_t i = 3; i < out.image.pixels.size(); i += 4) out.image.pixels[i] = 255;   // GDI leaves alpha undefined
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        return out;
    } catch (const std::bad_alloc&) {
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This presentation is too large to preview.", L"bad_alloc"};
    }
}

} // namespace mzr::preview
