// DXF reader of the Mechano CAD core. See Cad.h.
// Streams the file line by line (a 185 MB DXF never sits in RAM as text), keeps compact
// entities, then expands blocks into flat primitives per view with hard limits.
#include "core/cad/Cad.h"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace mzr::cad {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

ErrorInfo Bad(std::wstring friendly, std::wstring details = {}) {
    return ErrorInfo{ErrorCode::Corrupted, std::move(friendly), std::move(details)};
}

// ------------------------------------------------------------------ line source

class LineSource {
public:
    explicit LineSource(HANDLE h) : h_(h) { buf_.resize(1 << 20); }
    explicit LineSource(const std::string& text) : mem_(&text) {}
    ~LineSource() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    LineSource(const LineSource&) = delete;
    LineSource& operator=(const LineSource&) = delete;

    // One line without CR/LF. False at the end.
    bool Read(std::string& line) {
        line.clear();
        for (;;) {
            if (pos_ >= len_ && !Fill()) return !line.empty();
            const char* b = data_ + pos_;
            const char* nl = static_cast<const char*>(memchr(b, '\n', len_ - pos_));
            const size_t n = nl ? static_cast<size_t>(nl - b) : len_ - pos_;
            line.append(b, n);
            pos_ += n + (nl ? 1 : 0);
            if (nl) break;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

private:
    bool Fill() {
        if (mem_) {
            if (memDone_) return false;
            memDone_ = true;
            data_ = mem_->data(); len_ = mem_->size(); pos_ = 0;
            return len_ > 0;
        }
        DWORD got = 0;
        if (!ReadFile(h_, buf_.data(), static_cast<DWORD>(buf_.size()), &got, nullptr) || got == 0) return false;
        data_ = buf_.data(); len_ = got; pos_ = 0;
        return true;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    const std::string* mem_ = nullptr;
    bool memDone_ = false;
    std::vector<char> buf_;
    const char* data_ = nullptr;
    size_t len_ = 0, pos_ = 0;
};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
double Num(std::string_view s, double def = 0) {
    s = Trim(s);
    double v = def;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || !std::isfinite(v)) return def;
    return v;
}
long long Int(std::string_view s, long long def = 0) {
    s = Trim(s);
    long long v = def;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() ? v : def;
}
std::string Upper(std::string_view s) {
    std::string u(Trim(s));
    for (auto& c : u) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return u;
}

// ------------------------------------------------------------------ geometry helpers

struct Xf {                                // p' = (a x + c y + e, b x + d y + f)
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Pt operator()(Pt p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Pt Dir(Pt v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    Xf operator*(const Xf& r) const {     // this after r
        return {a * r.a + c * r.b, b * r.a + d * r.b, a * r.c + c * r.d, b * r.c + d * r.d,
                a * r.e + c * r.f + e, b * r.e + d * r.f + f};
    }
};
Xf Translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
Xf Rotate(double rad) { const double cs = std::cos(rad), sn = std::sin(rad); return {cs, sn, -sn, cs, 0, 0}; }
Xf Scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }

// Object Coordinate System of a 2D entity (DXF "arbitrary axis algorithm"), projected on XY.
Xf Ocs(double nx, double ny, double nz) {
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12) return {};
    nx /= len; ny /= len; nz /= len;
    if (std::abs(nz - 1) < 1e-12) return {};
    double ax, ay, az;
    if (std::abs(nx) < 1.0 / 64 && std::abs(ny) < 1.0 / 64) { ax = nz; ay = 0; az = -nx; }   // Wy x N
    else { ax = -ny; ay = nx; az = 0; }                                                    // Wz x N
    const double al = std::sqrt(ax * ax + ay * ay + az * az);
    ax /= al; ay /= al; az /= al;
    const double bx = ny * az - nz * ay, by = nz * ax - nx * az;                          // N x Ax
    return {ax, ay, bx, by, 0, 0};
}

int ArcSegments(double sweep) {
    return std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / (2 * kPi) * 72)), 4, 360);
}

// ------------------------------------------------------------------ compact entities

enum class T : uint8_t { Line, Poly, Circle, Arc, Ellipse, Spline, Text, MText, Point, Solid, Face3d,
                         Insert, Dimension, Segments, Hatch, Viewport };

struct Vtx { double x = 0, y = 0, bulge = 0; };

struct Ent {
    T t = T::Line;
    bool paper = false;
    bool closed = false;
    bool solidFill = false;
    int16_t aci = 256;                       // 256 BYLAYER, 0 BYBLOCK
    uint32_t rgb = 0;                        // true colour | 0x01000000, 0 = none
    int32_t layer = 0;
    double ex = 0, ey = 0, ez = 1;           // extrusion
    double d[16] = {};
    uint32_t v0 = 0, vn = 0;                 // vertices
    uint32_t k0 = 0, kn = 0;                 // knots (splines)
    uint32_t s = UINT32_MAX;                 // text (texts pool) or block name (names pool)
    uint8_t hAlign = 0, vAlign = 0;
};

struct Layer { int aci = 7; uint32_t rgb = 0; bool hidden = false; };

struct Block { Pt base; std::vector<Ent> ents; };

struct LayoutRec { std::wstring name; long long tab = 0; std::string blockHandle; };

// ------------------------------------------------------------------ the reader

class Reader {
public:
    explicit Reader(const Limits& l) : lim_(l) {
        layers_.push_back({});                               // index 0 = layer "0"
        layerIdx_["0"] = 0;
    }

    Result<Drawing> Run(LineSource& src) {
        std::string code, value;
        enum class Sec { None, Header, Tables, Blocks, Entities, Objects } sec = Sec::None;
        std::string type;                                    // current record ("LINE", "LAYER", ...)
        codes_.clear();
        bool any = false;
        auto flush = [&] {
            if (!type.empty()) Record(sec == Sec::Header ? "" : type, sec);
            codes_.clear();
        };
        size_t lineNo = 0;
        while (src.Read(code)) {
            if (!src.Read(value)) break;
            lineNo += 2;
            if (lineNo == 2 && code.rfind("AutoCAD Binary DXF", 0) == 0)
                return Bad(L"This is a binary DXF file, which the preview cannot read yet.", L"Binary DXF");
            const std::string_view cv = Trim(code);
            int gc = 0;
            if (std::from_chars(cv.data(), cv.data() + cv.size(), gc).ec != std::errc())
                return Bad(L"This DXF file is damaged.", L"Bad group code near line " + std::to_wstring(lineNo));
            any = true;
            if (gc == 0) {
                flush();
                const std::string v = Upper(value);
                if (v == "SECTION") { type.clear(); expectSecName_ = true; continue; }
                if (v == "ENDSEC") { sec = Sec::None; type.clear(); continue; }
                if (v == "EOF") break;
                type = v;
                if (sec == Sec::Header) type.clear();
                if (truncated_ && entities_ >= lim_.maxEntities) break;
                continue;
            }
            if (expectSecName_ && gc == 2) {
                expectSecName_ = false;
                const std::string n = Upper(value);
                sec = n == "HEADER" ? Sec::Header : n == "TABLES" ? Sec::Tables : n == "BLOCKS" ? Sec::Blocks
                    : n == "ENTITIES" ? Sec::Entities : n == "OBJECTS" ? Sec::Objects : Sec::None;
                continue;
            }
            if (sec == Sec::Header) { Header(gc, value); continue; }
            if (type.empty()) continue;
            if (codes_.size() < 200000) {                     // one record: bounded
                codes_.emplace_back(gc, std::string());
                codes_.back().second.assign(value);
            }
        }
        flush();
        if (!any) return Bad(L"This file is empty or is not a DXF drawing.", L"No group codes");
        return Build();
    }

private:
    using Codes = std::vector<std::pair<int, std::string>>;

    // -------------------------------------------------- header
    void Header(int gc, const std::string& v) {
        if (gc == 9) { hdrVar_ = Upper(v); return; }
        if (gc == 1 && hdrVar_ == "$ACADVER") acadVer_ = std::string(Trim(v));
        if (gc == 3 && hdrVar_ == "$DWGCODEPAGE") {
            const std::string u = Upper(v);
            if (u.rfind("ANSI_", 0) == 0) codePage_ = static_cast<UINT>(Int(u.substr(5), 1252));
            else if (u.rfind("DOS", 0) == 0) codePage_ = static_cast<UINT>(Int(u.substr(3), 437));
        }
    }

    // -------------------------------------------------- text decoding
    std::wstring Decode(std::string_view raw) {
        if (raw.size() > lim_.maxTextChars * 4) raw = raw.substr(0, lim_.maxTextChars * 4);
        const UINT cp = acadVer_ >= "AC1021" ? CP_UTF8 : codePage_;
        std::wstring w;
        if (!raw.empty()) {
            const int n = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            if (n > 0) {
                w.resize(static_cast<size_t>(n));
                MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n);
            }
        }
        // \U+0627 escapes (older DXF versions write Unicode this way).
        std::wstring out;
        out.reserve(w.size());
        for (size_t i = 0; i < w.size(); ++i) {
            if (w[i] == L'\\' && i + 6 < w.size() + 1 && (w[i + 1] == L'U' || w[i + 1] == L'u') && i + 2 < w.size() && w[i + 2] == L'+') {
                unsigned v = 0; size_t j = i + 3, k = 0;
                for (; k < 4 && j < w.size() && iswxdigit(w[j]); ++k, ++j)
                    v = v * 16 + static_cast<unsigned>(iswdigit(w[j]) ? w[j] - L'0' : (towupper(w[j]) - L'A' + 10));
                if (k == 4) { out += static_cast<wchar_t>(v); i = j - 1; continue; }
            }
            out += w[i];
        }
        if (out.size() > lim_.maxTextChars) out.resize(lim_.maxTextChars);
        return out;
    }

    static std::wstring PlainText(std::wstring s) {      // TEXT / ATTRIB: %%d %%p %%c, %%u %%o
        std::wstring o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'%' && i + 2 < s.size() && s[i + 1] == L'%') {
                const wchar_t c = static_cast<wchar_t>(towlower(s[i + 2]));
                if (c == L'd') { o += L'°'; i += 2; continue; }
                if (c == L'p') { o += L'±'; i += 2; continue; }
                if (c == L'c') { o += L'⌀'; i += 2; continue; }
                if (c == L'u' || c == L'o' || c == L'k') { i += 2; continue; }
                if (c == L'%') { o += L'%'; i += 2; continue; }
            }
            o += s[i];
        }
        return o;
    }

    static std::wstring MTextPlain(const std::wstring& s) {   // strip MTEXT formatting codes
        std::wstring o;
        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t c = s[i];
            if (c == L'{' || c == L'}') continue;
            if (c != L'\\' || i + 1 >= s.size()) { o += c; continue; }
            const wchar_t k = s[++i];
            switch (k) {
            case L'P': o += L'\n'; break;
            case L'~': o += L' '; break;
            case L'\\': case L'{': case L'}': o += k; break;
            case L'S': {                                       // stacked text a^b / a/b / a#b
                size_t e = s.find(L';', i);
                if (e == std::wstring::npos) e = s.size();
                if (!o.empty() && iswdigit(o.back())) o += L' ';   // "4\S3/4;" -> "4 3/4"
                for (size_t j = i + 1; j < e; ++j) o += (s[j] == L'^' || s[j] == L'#') ? L'/' : s[j];
                i = e;
                break;
            }
            case L'L': case L'l': case L'O': case L'o': case L'K': case L'k': case L'N': break;
            default: {                                         // \f..; \H..; \C..; \W..; \Q..; \T..; \A..; \p..;
                const size_t e = s.find(L';', i);
                if (e == std::wstring::npos) return o;
                i = e;
            }
            }
        }
        return o;
    }

    // -------------------------------------------------- records
    int32_t LayerOf(const std::string& name) {
        const std::string u = Upper(name);
        auto it = layerIdx_.find(u);
        if (it != layerIdx_.end()) return it->second;
        if (layers_.size() > 100000) return 0;
        layers_.push_back({});
        layerIdx_[u] = static_cast<int32_t>(layers_.size() - 1);
        return static_cast<int32_t>(layers_.size() - 1);
    }

    std::vector<Ent>& Target(bool paper, int sec) {
        if (sec == 3 /*Blocks*/ && curBlock_) return curBlock_->ents;
        return paper ? paperEnts_ : modelEnts_;
    }

    template <typename Sec>
    void Record(const std::string& type, Sec sec) {
        const int s = static_cast<int>(sec);                   // 2 tables, 3 blocks, 4 entities, 5 objects
        if (s == 2) { Table(type); return; }
        if (s == 5) { if (type == "LAYOUT") Layout(); return; }
        if (s == 3 && type == "BLOCK") { BeginBlock(); return; }
        if (s == 3 && type == "ENDBLK") { curBlock_ = nullptr; return; }
        if (s != 3 && s != 4) return;
        if (entities_ >= lim_.maxEntities) { truncated_ = true; return; }
        ++entities_;
        Entity(type, s);
    }

    void Table(const std::string& type) {
        if (type == "LAYER") {
            std::string name; int aci = 7; uint32_t rgb = 0; long long flags = 0;
            for (auto& [c, v] : codes_) {
                if (c == 2) name = v;
                else if (c == 62) aci = static_cast<int>(Int(v, 7));
                else if (c == 420) rgb = 0x01000000u | (static_cast<uint32_t>(Int(v)) & 0xFFFFFF);
                else if (c == 70) flags = Int(v);
            }
            Layer& l = layers_[static_cast<size_t>(LayerOf(name))];
            l.hidden = aci < 0 || (flags & 1);                 // off or frozen
            l.aci = std::abs(aci) == 0 ? 7 : std::abs(aci);
            l.rgb = rgb;
        } else if (type == "BLOCK_RECORD") {
            std::string h, n;
            for (auto& [c, v] : codes_) { if (c == 5) h = Upper(v); else if (c == 2) n = Upper(v); }
            if (!h.empty()) recordName_[h] = n;
        }
    }

    void Layout() {
        LayoutRec r;
        bool inLayout = false;
        for (auto& [c, v] : codes_) {
            if (c == 100) inLayout = Upper(v) == "ACDBLAYOUT";
            else if (c == 1 && inLayout) r.name = Decode(v);       // (code 1 before it is the page setup name)
            else if (c == 71) r.tab = Int(v);
            else if (c == 330 && inLayout) r.blockHandle = Upper(v);
        }
        layouts_.push_back(std::move(r));
    }

    void BeginBlock() {
        std::string name; Pt base;
        for (auto& [c, v] : codes_) {
            if (c == 2) name = Upper(v);
            else if (c == 10) base.x = Num(v);
            else if (c == 20) base.y = Num(v);
        }
        Block& b = blocks_[name];
        b.base = base;
        b.ents.clear();
        curBlock_ = &b;
    }

    uint32_t AddName(const std::string& n) {
        const std::string u = Upper(n);
        auto it = nameIdx_.find(u);
        if (it != nameIdx_.end()) return it->second;
        names_.push_back(u);
        nameIdx_[u] = static_cast<uint32_t>(names_.size() - 1);
        return static_cast<uint32_t>(names_.size() - 1);
    }
    uint32_t AddText(std::wstring t) { texts_.push_back(std::move(t)); return static_cast<uint32_t>(texts_.size() - 1); }

    bool RoomFor(size_t n) {
        if (vtx_.size() + n > lim_.maxPoints) { truncated_ = stop_ = true; return false; }
        return true;
    }

    void Common(Ent& e, int c, const std::string& v) {
        switch (c) {
        case 8: e.layer = LayerOf(v); break;
        case 62: e.aci = static_cast<int16_t>(std::clamp<long long>(Int(v, 256), -257, 257)); break;
        case 420: e.rgb = 0x01000000u | (static_cast<uint32_t>(Int(v)) & 0xFFFFFF); break;
        case 67: e.paper = Int(v) == 1; break;
        case 210: e.ex = Num(v); break;
        case 220: e.ey = Num(v); break;
        case 230: e.ez = Num(v, 1); break;
        default: break;
        }
    }

    void Entity(const std::string& type, int sec) {
        // VERTEX / SEQEND belong to the POLYLINE before them.
        if (type == "VERTEX") { Vertex(); return; }
        if (type == "SEQEND") { EndPolyline(); return; }
        EndPolyline();

        Ent e;
        bool invisible = false;
        for (auto& [c, v] : codes_) { Common(e, c, v); if (c == 60 && Int(v) == 1) invisible = true; }
        if (invisible) return;
        auto num = [&](int code, double def = 0) {
            for (auto& [c, v] : codes_) if (c == code) return Num(v, def);
            return def;
        };
        auto str = [&](int code) -> const std::string* {
            for (auto& [c, v] : codes_) if (c == code) return &v;
            return nullptr;
        };

        if (type == "LINE") {
            e.t = T::Line;
            e.d[0] = num(10); e.d[1] = num(20); e.d[2] = num(11); e.d[3] = num(21);
        } else if (type == "POINT") {
            e.t = T::Point; e.d[0] = num(10); e.d[1] = num(20);
        } else if (type == "CIRCLE" || type == "ARC") {
            e.t = type == "ARC" ? T::Arc : T::Circle;
            e.d[0] = num(10); e.d[1] = num(20); e.d[2] = num(40); e.d[3] = num(50); e.d[4] = num(51, 360);
        } else if (type == "ELLIPSE") {
            e.t = T::Ellipse;
            e.d[0] = num(10); e.d[1] = num(20); e.d[2] = num(11); e.d[3] = num(21); e.d[4] = num(40, 1);
            e.d[5] = num(41, 0); e.d[6] = num(42, 2 * kPi);
        } else if (type == "LWPOLYLINE") {
            e.t = T::Poly;
            e.closed = (static_cast<long long>(num(70)) & 1) != 0;
            e.v0 = static_cast<uint32_t>(vtx_.size());
            for (auto& [c, v] : codes_) {
                if (c == 10) { if (!RoomFor(1)) break; vtx_.push_back({Num(v), 0, 0}); }
                else if (c == 20 && vtx_.size() > e.v0) vtx_.back().y = Num(v);
                else if (c == 42 && vtx_.size() > e.v0) vtx_.back().bulge = Num(v);
            }
            e.vn = static_cast<uint32_t>(vtx_.size()) - e.v0;
        } else if (type == "POLYLINE") {
            e.t = T::Poly;
            const long long f = static_cast<long long>(num(70));
            e.closed = (f & 1) != 0;
            e.d[0] = static_cast<double>(f);
            e.v0 = static_cast<uint32_t>(vtx_.size());
            Target(e.paper, sec).push_back(e);
            openPoly_ = &Target(e.paper, sec).back();
            polyFace_ = (f & 64) != 0;
            faceSegs_.clear();
            return;
        } else if (type == "SPLINE") {
            e.t = T::Spline;
            e.closed = (static_cast<long long>(num(70)) & 1) != 0;
            e.d[0] = num(71, 3);
            e.v0 = static_cast<uint32_t>(vtx_.size());
            e.k0 = static_cast<uint32_t>(knots_.size());
            std::vector<Vtx> fit;
            size_t wi = 0;
            for (auto& [c, v] : codes_) {
                if (c == 40 && knots_.size() - e.k0 < 100000) knots_.push_back(Num(v));
                else if (c == 10) { if (!RoomFor(1)) break; vtx_.push_back({Num(v), 0, 1}); }
                else if (c == 20 && vtx_.size() > e.v0) vtx_.back().y = Num(v);
                else if (c == 41) { if (e.v0 + wi < vtx_.size()) vtx_[e.v0 + wi].bulge = Num(v, 1); ++wi; }
                else if (c == 11 && fit.size() < 100000) fit.push_back({Num(v), 0, 0});
                else if (c == 21 && !fit.empty()) fit.back().y = Num(v);
            }
            e.vn = static_cast<uint32_t>(vtx_.size()) - e.v0;
            e.kn = static_cast<uint32_t>(knots_.size()) - e.k0;
            if (e.vn < 2 && fit.size() >= 2 && RoomFor(fit.size())) {   // only fit points: draw through them
                e.t = T::Poly; e.closed = false;
                e.v0 = static_cast<uint32_t>(vtx_.size());
                vtx_.insert(vtx_.end(), fit.begin(), fit.end());
                e.vn = static_cast<uint32_t>(fit.size());
            }
        } else if (type == "TEXT" || type == "ATTRIB") {
            const bool attrib = type == "ATTRIB";
            if (attrib && (static_cast<long long>(num(70)) & 1)) return;     // invisible attribute
            const std::string* t = str(1);
            if (!t || t->empty()) return;
            e.t = T::Text;
            const int h = static_cast<int>(num(72)), vv = static_cast<int>(num(attrib ? 74 : 73));
            const bool aligned = h != 0 || vv != 0;
            e.d[0] = aligned && str(11) ? num(11) : num(10);
            e.d[1] = aligned && str(21) ? num(21) : num(20);
            e.d[2] = num(40, 1); e.d[3] = num(50) * kPi / 180; e.d[4] = num(41, 1);
            e.hAlign = static_cast<uint8_t>(h == 1 || h == 4 ? 1 : h == 2 ? 2 : 0);
            e.vAlign = static_cast<uint8_t>(h == 4 ? 2 : std::clamp(vv, 0, 3));
            e.s = AddText(PlainText(Decode(*t)));
        } else if (type == "MTEXT") {
            std::string raw;
            for (auto& [c, v] : codes_) if (c == 3 && raw.size() < lim_.maxTextChars * 4) raw += v;
            if (const std::string* t = str(1)) raw += *t;
            if (raw.empty()) return;
            e.t = T::Text;
            e.d[0] = num(10); e.d[1] = num(20); e.d[2] = num(40, 1);
            if (str(11)) e.d[3] = std::atan2(num(21), num(11, 1)); else e.d[3] = num(50);
            e.d[4] = 1;
            const int a = std::clamp(static_cast<int>(num(71, 1)), 1, 9);
            e.hAlign = static_cast<uint8_t>((a - 1) % 3);
            e.vAlign = static_cast<uint8_t>(a <= 3 ? 3 : a <= 6 ? 2 : 1);
            e.ex = 0; e.ey = 0; e.ez = 1;                     // MTEXT points are WCS
            e.s = AddText(PlainText(MTextPlain(Decode(raw))));
        } else if (type == "SOLID" || type == "TRACE" || type == "3DFACE") {
            e.t = type == "3DFACE" ? T::Face3d : T::Solid;
            for (int i = 0; i < 4; ++i) { e.d[i * 2] = num(10 + i); e.d[i * 2 + 1] = num(20 + i); }
            if (!str(13)) { e.d[6] = e.d[4]; e.d[7] = e.d[5]; }
        } else if (type == "INSERT" || type == "ACAD_TABLE") {
            const std::string* n = str(2);
            if (!n) return;
            e.t = T::Insert;
            e.s = AddName(*n);
            e.d[0] = num(10); e.d[1] = num(20);
            e.d[2] = num(41, 1); e.d[3] = num(42, 1); e.d[4] = num(50) * kPi / 180;
            e.d[5] = std::max(1.0, num(70, 1)); e.d[6] = std::max(1.0, num(71, 1));
            e.d[7] = num(44); e.d[8] = num(45);
            if (type == "ACAD_TABLE") { e.d[2] = e.d[3] = 1; e.d[4] = 0; e.d[5] = e.d[6] = 1; }
        } else if (type == "DIMENSION") {
            const std::string* n = str(2);
            if (!n) return;
            e.t = T::Dimension;
            e.s = AddName(*n);
        } else if (type == "LEADER") {
            e.t = T::Poly;
            e.v0 = static_cast<uint32_t>(vtx_.size());
            for (auto& [c, v] : codes_) {
                if (c == 10) { if (!RoomFor(1)) break; vtx_.push_back({Num(v), 0, 0}); }
                else if (c == 20 && vtx_.size() > e.v0) vtx_.back().y = Num(v);
            }
            e.vn = static_cast<uint32_t>(vtx_.size()) - e.v0;
            e.ex = 0; e.ey = 0; e.ez = 1;
        } else if (type == "HATCH") {
            if (!Hatch(e)) return;
        } else if (type == "VIEWPORT") {
            e.t = T::Viewport;
            e.d[0] = num(10); e.d[1] = num(20); e.d[2] = num(40); e.d[3] = num(41);
            e.d[4] = num(12); e.d[5] = num(22); e.d[6] = num(17); e.d[7] = num(27); e.d[8] = num(45);
            e.d[9] = num(68, 1); e.d[10] = num(69, 2);
            e.paper = true;
        } else {
            return;                                           // not drawn (3DSOLID, IMAGE, MLINE, ...)
        }
        Target(e.paper, sec).push_back(e);
    }

    void Vertex() {
        if (!openPoly_) return;
        double x = 0, y = 0, b = 0; long long f = 0; long long idx[4] = {0, 0, 0, 0};
        for (auto& [c, v] : codes_) {
            if (c == 10) x = Num(v); else if (c == 20) y = Num(v); else if (c == 42) b = Num(v);
            else if (c == 70) f = Int(v);
            else if (c >= 71 && c <= 74) idx[c - 71] = Int(v);
        }
        if (polyFace_ && (f & 128) && !(f & 64)) {            // polyface face record: edges by vertex index
            for (int i = 0; i < 4; ++i) {
                const long long a = idx[i], bb = idx[(i + 1) % 4] ? idx[(i + 1) % 4] : idx[0];
                if (a > 0 && bb != 0) faceSegs_.push_back({a - 1, (bb < 0 ? -bb : bb) - 1});
                if (!idx[(i + 1) % 4]) break;
            }
            return;
        }
        if (f & 16) return;                                   // spline frame control point
        if (!RoomFor(1)) return;
        vtx_.push_back({x, y, b});
        openPoly_->vn = static_cast<uint32_t>(vtx_.size()) - openPoly_->v0;
    }

    void EndPolyline() {
        if (!openPoly_) return;
        if (polyFace_) {                                      // convert to explicit segments
            const uint32_t base = openPoly_->v0, n = openPoly_->vn;
            std::vector<Vtx> seg;
            for (auto& [a, b] : faceSegs_)
                if (a >= 0 && b >= 0 && a < n && b < n) { seg.push_back(vtx_[base + a]); seg.push_back(vtx_[base + b]); }
            vtx_.resize(base);
            if (RoomFor(seg.size())) vtx_.insert(vtx_.end(), seg.begin(), seg.end());
            openPoly_->t = T::Segments;
            openPoly_->vn = static_cast<uint32_t>(vtx_.size()) - base;
        }
        openPoly_ = nullptr;
        polyFace_ = false;
        faceSegs_.clear();
    }

    // HATCH boundaries -> point lists (separated by a NaN vertex), already flattened (arcs).
    bool Hatch(Ent& e) {
        e.t = T::Hatch;
        size_t i = 0;
        const size_t n = codes_.size();
        auto at = [&](int code) { return i < n && codes_[i].first == code; };
        auto take = [&](int code, double def = 0) { if (at(code)) return Num(codes_[i++].second, def); return def; };
        while (i < n && codes_[i].first != 91) {
            if (codes_[i].first == 70) e.solidFill = Int(codes_[i].second) == 1;
            ++i;
        }
        if (i >= n) return false;
        const long long paths = std::min<long long>(Int(codes_[i++].second), 10000);
        e.v0 = static_cast<uint32_t>(vtx_.size());
        for (long long p = 0; p < paths && i < n; ++p) {
            while (i < n && codes_[i].first != 92) ++i;
            if (i >= n) break;
            const long long flags = Int(codes_[i++].second);
            if (vtx_.size() > e.v0) { if (!RoomFor(1)) break; vtx_.push_back({kNaN, kNaN, 0}); }
            if (flags & 2) {                                  // polyline path
                const bool bulge = take(72) != 0;
                take(73);
                const long long nv = std::min<long long>(static_cast<long long>(take(93)), 1000000);
                std::vector<Vtx> pv;
                for (long long k = 0; k < nv && i < n; ++k) {
                    if (!at(10)) break;                       // fewer vertices than claimed
                    Vtx v; v.x = take(10); v.y = take(20);
                    if (bulge) v.bulge = take(42);
                    pv.push_back(v);
                }
                for (size_t k = 0; k < pv.size(); ++k) {
                    if (!RoomFor(1)) return true;
                    vtx_.push_back({pv[k].x, pv[k].y, 0});
                    if (pv[k].bulge != 0) BulgePoints(pv[k], pv[(k + 1) % pv.size()]);
                }
            } else {                                          // edges
                const long long ne = std::min<long long>(static_cast<long long>(take(93)), 100000);
                for (long long k = 0; k < ne && i < n; ++k) {
                    while (i < n && codes_[i].first != 72) ++i;
                    const int et = static_cast<int>(take(72));
                    if (et == 1) {
                        const double x1 = take(10), y1 = take(20), x2 = take(11), y2 = take(21);
                        if (!RoomFor(2)) return true;
                        vtx_.push_back({x1, y1, 0}); vtx_.push_back({x2, y2, 0});
                    } else if (et == 2 || et == 3) {
                        const double cx = take(10), cy = take(20);
                        double mx = 0, my = 0;
                        if (et == 3) { mx = take(11); my = take(21); }
                        const double r = take(40, 1);
                        double a0 = take(50) * kPi / 180, a1 = take(51) * kPi / 180;
                        const bool ccw = take(73, 1) != 0;
                        if (!ccw) { a0 = -a0; a1 = -a1; }
                        if (a1 < a0) a1 += 2 * kPi;
                        const int segs = ArcSegments(a1 - a0);
                        if (!RoomFor(static_cast<size_t>(segs) + 1)) return true;
                        for (int s = 0; s <= segs; ++s) {
                            const double t = a0 + (a1 - a0) * s / segs;
                            if (et == 2) vtx_.push_back({cx + r * std::cos(t), cy + r * std::sin(t), 0});
                            else vtx_.push_back({cx + mx * std::cos(t) - my * r * std::sin(t),
                                                 cy + my * std::cos(t) + mx * r * std::sin(t), 0});
                        }
                    } else if (et == 4) {                     // spline edge: control polygon (trial)
                        take(94); take(73); take(74);
                        const long long nk = std::min<long long>(static_cast<long long>(take(95)), 100000);
                        const long long nc = std::min<long long>(static_cast<long long>(take(96)), 100000);
                        for (long long q = 0; q < nk && at(40); ++q) take(40);
                        for (long long q = 0; q < nc && i < n; ++q) {
                            if (!at(10)) break;
                            const double x = take(10), y = take(20);
                            take(42);
                            if (!RoomFor(1)) return true;
                            vtx_.push_back({x, y, 0});
                        }
                        if (at(97)) { const long long nf = static_cast<long long>(take(97)); for (long long q = 0; q < nf && at(11); ++q) { take(11); take(21); } }
                    } else {
                        break;
                    }
                }
            }
        }
        e.vn = static_cast<uint32_t>(vtx_.size()) - e.v0;
        return e.vn >= 3;
    }

    void BulgePoints(const Vtx& a, const Vtx& b) {          // points strictly between a and b
        const double dx = b.x - a.x, dy = b.y - a.y, chord = std::hypot(dx, dy);
        if (chord < 1e-12 || std::abs(a.bulge) > 1e6) return;
        const double sweep = 4 * std::atan(a.bulge);
        const double r = chord / (2 * std::sin(std::abs(sweep) / 2));
        const double mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
        // Signed distance of the centre from the chord (negative for arcs over 180 degrees).
        const double h = r * std::cos(std::abs(sweep) / 2);
        const double sgn = a.bulge > 0 ? 1 : -1;                          // centre on the left for counter-clockwise
        const double cx = mx - sgn * h * dy / chord;
        const double cy = my + sgn * h * dx / chord;
        const double a0 = std::atan2(a.y - cy, a.x - cx);
        const int segs = ArcSegments(sweep);
        if (!RoomFor(static_cast<size_t>(segs))) return;
        for (int s = 1; s < segs; ++s) {
            const double t = a0 + sweep * s / segs;
            vtx_.push_back({cx + r * std::cos(t), cy + r * std::sin(t), 0});
        }
    }

    // ================================================== expansion into views
    struct Ctx {
        Xf xf;
        uint32_t depth = 0;
        int32_t blockLayer = -1;                 // layer of the INSERT (for entities on layer "0")
        uint32_t blockRgb = kForeground;         // colour of the INSERT (for BYBLOCK)
    };

    bool LayerHidden(int32_t layer) const { return layer >= 0 && static_cast<size_t>(layer) < layers_.size() && layers_[static_cast<size_t>(layer)].hidden; }

    uint32_t LayerRgb(int32_t layer) const {
        if (layer < 0 || static_cast<size_t>(layer) >= layers_.size()) return kForeground;
        const Layer& l = layers_[static_cast<size_t>(layer)];
        return l.rgb ? (l.rgb & 0xFFFFFF) : AciToRgb(l.aci);
    }

    // Colour and effective layer of an entity inside the current block context.
    bool Resolve(const Ent& e, const Ctx& ctx, uint32_t& rgb, int32_t& layer) const {
        layer = (e.layer == 0 && ctx.blockLayer >= 0) ? ctx.blockLayer : e.layer;
        if (LayerHidden(e.layer) || LayerHidden(layer)) return false;
        if (e.rgb) rgb = e.rgb & 0xFFFFFF;
        else if (e.aci == 256 || e.aci < 0) rgb = LayerRgb(layer);
        else if (e.aci == 0) rgb = ctx.blockRgb;
        else rgb = AciToRgb(e.aci);
        return true;
    }

    bool Budget(size_t n) {
        if (outPoints_ + n > lim_.maxPoints) { truncated_ = stop_ = true; return false; }
        outPoints_ += n;
        return true;
    }

    void AddPath(View& v, PrimKind k, uint32_t rgb, const std::vector<Pt>& pts) {
        if (pts.size() < (k == PrimKind::Dot ? 1u : 2u) || !Budget(pts.size())) return;
        Prim p; p.kind = k; p.rgb = rgb;
        p.first = static_cast<uint32_t>(v.pts.size()); p.count = static_cast<uint32_t>(pts.size());
        v.pts.insert(v.pts.end(), pts.begin(), pts.end());
        v.prims.push_back(p);
    }

    void Emit(View& v, const Ent& e, const Ctx& ctx) {
        // Every visit counts, also INSERTs that draw nothing: a block that inserts other blocks
        // 10 times, 16 levels deep, must not run for ever ("billion laughs" for drawings).
        if (++visits_ > lim_.maxPoints * 4) { truncated_ = stop_ = true; return; }
        uint32_t rgb = kForeground; int32_t layer = 0;
        if (e.t != T::Viewport && !Resolve(e, ctx, rgb, layer)) return;
        const Xf xf = (e.ex == 0 && e.ey == 0 && e.ez == 1) ? ctx.xf : ctx.xf * Ocs(e.ex, e.ey, e.ez);
        std::vector<Pt> pts;
        switch (e.t) {
        case T::Line:
            AddPath(v, PrimKind::Lines, rgb, {ctx.xf({e.d[0], e.d[1]}), ctx.xf({e.d[2], e.d[3]})});
            break;
        case T::Point:
            AddPath(v, PrimKind::Dot, rgb, {ctx.xf({e.d[0], e.d[1]})});
            break;
        case T::Circle:
        case T::Arc: {
            const double r = e.d[2];
            if (!(r > 0) || !std::isfinite(r)) break;
            double a0 = 0, a1 = 2 * kPi;
            if (e.t == T::Arc) { a0 = e.d[3] * kPi / 180; a1 = e.d[4] * kPi / 180; while (a1 <= a0) a1 += 2 * kPi; }
            const int segs = ArcSegments(a1 - a0);
            for (int s = 0; s <= segs; ++s) {
                const double t = a0 + (a1 - a0) * s / segs;
                pts.push_back(xf({e.d[0] + r * std::cos(t), e.d[1] + r * std::sin(t)}));
            }
            AddPath(v, PrimKind::Lines, rgb, pts);
            break;
        }
        case T::Ellipse: {
            const double mx = e.d[2], my = e.d[3], ratio = e.d[4];
            const double sgn = e.ez < 0 ? -1 : 1;
            const double nx = -my * ratio * sgn, ny = mx * ratio * sgn;
            double t0 = e.d[5], t1 = e.d[6];
            while (t1 <= t0) t1 += 2 * kPi;
            const int segs = ArcSegments(t1 - t0);
            for (int s = 0; s <= segs; ++s) {
                const double t = t0 + (t1 - t0) * s / segs;
                pts.push_back(ctx.xf({e.d[0] + mx * std::cos(t) + nx * std::sin(t), e.d[1] + my * std::cos(t) + ny * std::sin(t)}));
            }
            AddPath(v, PrimKind::Lines, rgb, pts);
            break;
        }
        case T::Poly: {
            if (e.vn < 2) break;
            for (uint32_t k = 0; k < e.vn; ++k) {
                const Vtx& a = vtx_[e.v0 + k];
                pts.push_back(xf({a.x, a.y}));
                const bool last = k + 1 == e.vn;
                if (last && !e.closed) break;
                if (a.bulge != 0) {
                    const Vtx& b = vtx_[e.v0 + (last ? 0 : k + 1)];
                    const size_t before = vtx_.size();
                    BulgePoints(a, b);
                    for (size_t q = before; q < vtx_.size(); ++q) pts.push_back(xf({vtx_[q].x, vtx_[q].y}));
                    vtx_.resize(before);
                }
            }
            if (e.closed) pts.push_back(xf({vtx_[e.v0].x, vtx_[e.v0].y}));
            AddPath(v, PrimKind::Lines, rgb, pts);
            break;
        }
        case T::Segments:
            for (uint32_t k = 0; k + 1 < e.vn; k += 2)
                AddPath(v, PrimKind::Lines, rgb, {xf({vtx_[e.v0 + k].x, vtx_[e.v0 + k].y}), xf({vtx_[e.v0 + k + 1].x, vtx_[e.v0 + k + 1].y})});
            break;
        case T::Spline:
            SplinePoints(e, pts);
            for (auto& p : pts) p = ctx.xf(p);
            AddPath(v, PrimKind::Lines, rgb, pts);
            break;
        case T::Solid:
        case T::Face3d: {
            const Xf& x = e.t == T::Solid ? xf : ctx.xf;
            pts = {x({e.d[0], e.d[1]}), x({e.d[2], e.d[3]}), x({e.d[6], e.d[7]}), x({e.d[4], e.d[5]})};
            if (e.t == T::Solid) AddPath(v, PrimKind::Fill, rgb, pts);
            else { pts.push_back(pts[0]); AddPath(v, PrimKind::Lines, rgb, pts); }
            break;
        }
        case T::Hatch: {
            for (uint32_t k = 0; k < e.vn; ++k) {
                const Vtx& a = vtx_[e.v0 + k];
                pts.push_back(std::isnan(a.x) ? Pt{kNaN, kNaN} : xf({a.x, a.y}));
            }
            AddPath(v, e.solidFill ? PrimKind::Fill : PrimKind::HatchFill, rgb, pts);
            break;
        }
        case T::Text: {
            if (e.s >= texts_.size() || texts_[e.s].empty() || !Budget(1)) break;
            const Xf& x = e.ez == 1 && e.ex == 0 && e.ey == 0 ? ctx.xf : xf;
            const Pt pos = x({e.d[0], e.d[1]});
            const Pt dir = x.Dir({std::cos(e.d[3]), std::sin(e.d[3])});
            const Pt up = x.Dir({-std::sin(e.d[3]), std::cos(e.d[3])});
            Prim p; p.kind = PrimKind::Text; p.rgb = rgb;
            p.first = static_cast<uint32_t>(v.pts.size()); p.count = 1;
            v.pts.push_back(pos);
            p.text = static_cast<uint32_t>(v.texts.size());
            v.texts.push_back(texts_[e.s]);
            p.height = static_cast<float>(e.d[2] * std::hypot(up.x, up.y));
            p.rotation = static_cast<float>(std::atan2(dir.y, dir.x));
            const double wl = std::hypot(dir.x, dir.y), hl = std::hypot(up.x, up.y);
            p.widthFactor = static_cast<float>(std::clamp(e.d[4] * (hl > 0 ? wl / hl : 1), 0.05, 20.0));
            p.hAlign = e.hAlign; p.vAlign = e.vAlign;
            if (std::isfinite(p.height) && p.height > 0) v.prims.push_back(p);
            break;
        }
        case T::Insert:
        case T::Dimension: {
            if (ctx.depth >= lim_.maxBlockDepth) { truncated_ = true; break; }
            if (e.s >= names_.size()) break;
            auto it = blocks_.find(names_[e.s]);
            if (it == blocks_.end()) break;
            const Block& b = it->second;
            Ctx in;
            in.depth = ctx.depth + 1;
            in.blockLayer = layer;
            in.blockRgb = rgb;
            if (e.t == T::Dimension) {
                in.xf = ctx.xf;
                for (const Ent& c : b.ents) { if (stop_) break; Emit(v, c, in); }
                break;
            }
            const uint64_t cells = static_cast<uint64_t>(e.d[5]) * static_cast<uint64_t>(e.d[6]);
            if (cells > lim_.maxInsertCells) { truncated_ = true; break; }
            for (int r = 0; r < static_cast<int>(e.d[6]); ++r)
                for (int c = 0; c < static_cast<int>(e.d[5]); ++c) {
                    if (stop_) return;
                    in.xf = xf * Translate(e.d[0], e.d[1]) * Rotate(e.d[4]) * Translate(c * e.d[7], r * e.d[8]) *
                            Scale(e.d[2], e.d[3]) * Translate(-b.base.x, -b.base.y);
                    for (const Ent& ch : b.ents) { if (stop_) break; Emit(v, ch, in); }
                }
            break;
        }
        case T::Viewport: {
            if (ctx.depth != 0 || e.d[10] == 1 || e.d[9] == 0) break;      // id 1 = the sheet itself; 0 = off
            if (!(e.d[2] > 0 && e.d[3] > 0 && e.d[8] > 0)) break;
            Viewport vp;
            vp.paperCenter = {e.d[0], e.d[1]};
            vp.paperWidth = e.d[2]; vp.paperHeight = e.d[3];
            vp.modelCenter = {e.d[4] + e.d[6], e.d[5] + e.d[7]};
            vp.scale = e.d[3] / e.d[8];
            if (std::isfinite(vp.scale) && vp.scale > 0) v.viewports.push_back(vp);
            break;
        }
        }
    }

    // B-spline (NURBS) through de Boor; falls back to the control polygon if the knots are wrong.
    void SplinePoints(const Ent& e, std::vector<Pt>& out) const {
        const int p = std::clamp(static_cast<int>(e.d[0]), 1, 11);
        const int n = static_cast<int>(e.vn);
        if (n < 2) return;
        const double* U = e.kn ? &knots_[e.k0] : nullptr;
        const bool ok = U && static_cast<int>(e.kn) == n + p + 1 && n > p;
        if (!ok) {
            for (int i = 0; i < n; ++i) out.push_back({vtx_[e.v0 + static_cast<uint32_t>(i)].x, vtx_[e.v0 + static_cast<uint32_t>(i)].y});
            return;
        }
        const double u0 = U[p], u1 = U[n];
        if (!(u1 > u0)) return;
        const int samples = std::clamp(n * 8, 16, 512);
        std::vector<double> dx(static_cast<size_t>(p) + 1), dy(dx.size()), dw(dx.size());
        for (int s = 0; s <= samples; ++s) {
            const double u = s == samples ? u1 - 1e-12 * (u1 - u0) : u0 + (u1 - u0) * s / samples;
            int k = p;
            while (k < n - 1 && U[k + 1] <= u) ++k;
            for (int j = 0; j <= p; ++j) {
                const Vtx& c = vtx_[e.v0 + static_cast<uint32_t>(k - p + j)];
                const double w = c.bulge > 0 ? c.bulge : 1;
                dx[static_cast<size_t>(j)] = c.x * w; dy[static_cast<size_t>(j)] = c.y * w; dw[static_cast<size_t>(j)] = w;
            }
            for (int r = 1; r <= p; ++r)
                for (int j = p; j >= r; --j) {
                    const double den = U[j + 1 + k - r] - U[j + k - p];
                    const double a = den > 0 ? (u - U[j + k - p]) / den : 0;
                    const auto J = static_cast<size_t>(j);
                    dx[J] = (1 - a) * dx[J - 1] + a * dx[J];
                    dy[J] = (1 - a) * dy[J - 1] + a * dy[J];
                    dw[J] = (1 - a) * dw[J - 1] + a * dw[J];
                }
            const auto P = static_cast<size_t>(p);
            if (dw[P] != 0) out.push_back({dx[P] / dw[P], dy[P] / dw[P]});
        }
    }

    // Model space often has a few objects far away from the real drawing (old copies, blocks at
    // 0,0, survey points ...). Find the group of points that holds most of the drawing: points
    // are binned on a 64 x 64 grid and the biggest connected group of non-empty cells wins.
    // Repeated a few times so a far group cannot hide a small main drawing.
    // Lines are walked cell by cell (a long line joins the areas it crosses), so only groups
    // that nothing connects count as separate.
    using Seg = std::pair<uint32_t, uint32_t>;
    static void MainCluster(std::vector<Pt>& pts, const std::vector<Seg>& segs) {
        std::vector<char> alive(pts.size(), 1);
        size_t aliveCount = pts.size();
        for (int pass = 0; pass < 4 && aliveCount > 50; ++pass) {
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (size_t i = 0; i < pts.size(); ++i) {
                if (!alive[i]) continue;
                const Pt& p = pts[i];
                x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
            }
            const double w = x1 - x0, h = y1 - y0;
            if (!(w > 0) || !(h > 0)) break;
            constexpr int G = 64;
            std::vector<uint32_t> count(G * G, 0);
            std::vector<char> used(G * G, 0);
            auto cellXY = [&](double x, double y) {
                const int cx = std::clamp(static_cast<int>((x - x0) / w * G), 0, G - 1);
                const int cy = std::clamp(static_cast<int>((y - y0) / h * G), 0, G - 1);
                return cy * G + cx;
            };
            auto cell = [&](const Pt& p) { return cellXY(p.x, p.y); };
            for (size_t i = 0; i < pts.size(); ++i)
                if (alive[i]) { const int c = cell(pts[i]); ++count[static_cast<size_t>(c)]; used[static_cast<size_t>(c)] = 1; }
            for (const Seg& s : segs) {
                if (!alive[s.first] || !alive[s.second]) continue;
                const Pt a = pts[s.first], b = pts[s.second];
                const double steps = std::ceil(std::max(std::abs(b.x - a.x) / w, std::abs(b.y - a.y) / h) * G * 2);
                const int n = std::clamp(static_cast<int>(steps), 1, 256);
                for (int k = 1; k < n; ++k)
                    used[static_cast<size_t>(cellXY(a.x + (b.x - a.x) * k / n, a.y + (b.y - a.y) * k / n))] = 1;
            }
            std::vector<int> comp(G * G, -1);
            int best = -1; uint64_t bestCount = 0, compId = 0;
            std::vector<int> stack;
            for (int c0 = 0; c0 < G * G; ++c0) {
                if (!used[static_cast<size_t>(c0)] || comp[static_cast<size_t>(c0)] >= 0) continue;
                uint64_t total = 0;
                stack.assign(1, c0);
                comp[static_cast<size_t>(c0)] = static_cast<int>(compId);
                while (!stack.empty()) {
                    const int c = stack.back(); stack.pop_back();
                    total += count[static_cast<size_t>(c)];
                    const int cx = c % G, cy = c / G;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || ny < 0 || nx >= G || ny >= G) continue;
                            const size_t n = static_cast<size_t>(ny * G + nx);
                            if (used[n] && comp[n] < 0) { comp[n] = static_cast<int>(compId); stack.push_back(static_cast<int>(n)); }
                        }
                }
                if (total > bestCount) { bestCount = total; best = static_cast<int>(compId); }
                ++compId;
            }
            if (compId <= 1 || bestCount < aliveCount / 2) break;      // one group, or no clear main group
            for (size_t i = 0; i < pts.size(); ++i)
                if (alive[i] && comp[static_cast<size_t>(cell(pts[i]))] != best) { alive[i] = 0; --aliveCount; }
        }
        std::vector<Pt> keep;
        keep.reserve(aliveCount);
        for (size_t i = 0; i < pts.size(); ++i) if (alive[i]) keep.push_back(pts[i]);
        pts.swap(keep);
    }

    void Extents(View& v, bool trimOutliers) const {
        std::vector<double> xs, ys;
        xs.reserve(v.pts.size()); ys.reserve(v.pts.size());
        std::vector<Pt> all;
        std::vector<Seg> segs;
        all.reserve(v.pts.size() + v.texts.size() * 2);
        auto ok = [](const Pt& p) { return std::isfinite(p.x) && std::isfinite(p.y); };
        for (const Prim& p : v.prims) {
            if (p.first + p.count > v.pts.size()) continue;
            if (p.kind == PrimKind::Text) {
                // Text: its rough box, not only the insertion point (centred / right-aligned text grows left).
                if (p.text >= v.texts.size()) continue;
                const Pt a = v.pts[p.first];
                if (!ok(a)) continue;
                const double len = static_cast<double>(std::min<size_t>(v.texts[p.text].size(), 200)) * p.height * 0.75 * p.widthFactor;
                const double cs = std::cos(p.rotation), sn = std::sin(p.rotation);
                const double off = p.hAlign == 1 ? -len / 2 : p.hAlign == 2 ? -len : 0;
                all.push_back({a.x + cs * off - sn * p.height, a.y + sn * off + cs * p.height});
                all.push_back({a.x + cs * (off + len), a.y + sn * (off + len)});
                segs.push_back({static_cast<uint32_t>(all.size() - 2), static_cast<uint32_t>(all.size() - 1)});
                continue;
            }
            bool havePrev = false;
            for (uint32_t k = 0; k < p.count; ++k) {
                const Pt q = v.pts[p.first + k];
                if (!ok(q)) { havePrev = false; continue; }
                all.push_back(q);
                if (havePrev) segs.push_back({static_cast<uint32_t>(all.size() - 2), static_cast<uint32_t>(all.size() - 1)});
                havePrev = true;
            }
        }
        if (trimOutliers) MainCluster(all, segs);
        for (const Pt& p : all) { xs.push_back(p.x); ys.push_back(p.y); }
        for (const Viewport& vp : v.viewports) {
            xs.push_back(vp.paperCenter.x - vp.paperWidth / 2); xs.push_back(vp.paperCenter.x + vp.paperWidth / 2);
            ys.push_back(vp.paperCenter.y - vp.paperHeight / 2); ys.push_back(vp.paperCenter.y + vp.paperHeight / 2);
        }
        if (xs.empty()) { v.minX = v.minY = 0; v.maxX = v.maxY = 1; return; }
        v.minX = *std::min_element(xs.begin(), xs.end()); v.maxX = *std::max_element(xs.begin(), xs.end());
        v.minY = *std::min_element(ys.begin(), ys.end()); v.maxY = *std::max_element(ys.begin(), ys.end());
        if (!(v.maxX > v.minX)) { v.minX -= 1; v.maxX += 1; }
        if (!(v.maxY > v.minY)) { v.minY -= 1; v.maxY += 1; }
    }

    Result<Drawing> Build() {
        EndPolyline();
        Drawing d;
        d.acadVersion = std::wstring(acadVer_.begin(), acadVer_.end());
        d.entities = entities_;

        View model;
        model.name = L"Model";
        Ctx root;
        for (const Ent& e : modelEnts_) { if (stop_) break; Emit(model, e, root); }
        if (auto it = blocks_.find("*MODEL_SPACE"); it != blocks_.end())
            for (const Ent& e : it->second.ents) { if (stop_) break; Emit(model, e, root); }
        Extents(model, true);
        d.views.push_back(std::move(model));

        // Layouts: from the LAYOUT objects (tab order); R12 files: one "Layout" if paper entities exist.
        std::sort(layouts_.begin(), layouts_.end(), [](const LayoutRec& a, const LayoutRec& b) { return a.tab < b.tab; });
        auto paperView = [&](const std::wstring& name, const std::vector<Ent>* list1, const std::vector<Ent>* list2) {
            View v;
            v.name = name;
            v.paper = true;
            for (auto* list : {list1, list2}) {
                if (!list) continue;
                for (const Ent& e : *list) { if (stop_) break; Emit(v, e, root); }
            }
            if (v.empty()) return;
            Extents(v, false);
            d.views.push_back(std::move(v));
        };
        bool usedActive = false;
        for (const LayoutRec& l : layouts_) {
            const auto rn = recordName_.find(l.blockHandle);
            const std::string block = rn == recordName_.end() ? std::string() : rn->second;
            if (block == "*MODEL_SPACE" || (block.empty() && _wcsicmp(l.name.c_str(), L"Model") == 0)) continue;
            if (block == "*PAPER_SPACE" || (block.empty() && !usedActive)) {
                usedActive = true;
                auto it = blocks_.find("*PAPER_SPACE");
                paperView(l.name, &paperEnts_, it == blocks_.end() ? nullptr : &it->second.ents);
            } else {
                auto it = blocks_.find(block);
                if (it != blocks_.end()) paperView(l.name, &it->second.ents, nullptr);
            }
        }
        if (!usedActive && !paperEnts_.empty()) paperView(L"Layout", &paperEnts_, nullptr);

        d.truncated = truncated_;
        if (d.views[0].empty() && d.views.size() == 1)
            return Bad(L"This drawing has nothing that the preview can show (it may be 3D only).", L"No 2D entities");
        return d;
    }

    const Limits& lim_;
    std::string hdrVar_, acadVer_;
    UINT codePage_ = 1252;
    bool expectSecName_ = false;
    Codes codes_;
    std::vector<Layer> layers_;
    std::unordered_map<std::string, int32_t> layerIdx_;
    std::unordered_map<std::string, Block> blocks_;
    Block* curBlock_ = nullptr;
    std::vector<Ent> modelEnts_, paperEnts_;
    std::vector<Vtx> vtx_;
    std::vector<double> knots_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, uint32_t> nameIdx_;
    std::vector<std::wstring> texts_;
    std::unordered_map<std::string, std::string> recordName_;
    std::vector<LayoutRec> layouts_;
    Ent* openPoly_ = nullptr;
    bool polyFace_ = false;
    std::vector<std::pair<long long, long long>> faceSegs_;
    uint64_t entities_ = 0, outPoints_ = 0, visits_ = 0;
    bool truncated_ = false;          // something was skipped (shown partly)
    bool stop_ = false;               // a global budget is used up: draw nothing more
};

} // namespace

uint32_t AciToRgb(int aci) {
    static const uint32_t kBase[10] = {0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF,
                                       kForeground, 0x808080, 0xC0C0C0};
    if (aci <= 0 || aci > 255) return kForeground;
    if (aci < 10) return kBase[aci];
    if (aci >= 250) {
        static const uint8_t g[6] = {51, 80, 105, 130, 190, 255};
        const uint32_t v = g[aci - 250];
        return (v << 16) | (v << 8) | v;
    }
    // 10..249: 24 hues x 5 brightness levels x (full / pastel)
    const int i = aci - 10;
    const double hue = (i / 10) * 15.0;
    static const double kV[5] = {1.0, 0.8, 0.6, 0.5, 0.3};
    const double v = kV[(i % 10) / 2];
    const double s = (i % 2) ? 0.5 : 1.0;
    const double c = v * s, x = c * (1 - std::abs(std::fmod(hue / 60.0, 2) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (hue < 60) { r = c; g = x; } else if (hue < 120) { r = x; g = c; } else if (hue < 180) { g = c; b = x; }
    else if (hue < 240) { g = x; b = c; } else if (hue < 300) { r = x; b = c; } else { r = c; b = x; }
    auto to8 = [&](double t) { return static_cast<uint32_t>(std::lround((t + m) * 255)); };
    return (to8(r) << 16) | (to8(g) << 8) | to8(b);
}

Result<Drawing> ReadDxf(const std::wstring& path, const Limits& limits) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return ErrorInfo{ErrorCode::Io, L"The drawing file could not be read.", L"CreateFile " + std::to_wstring(GetLastError())};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (static_cast<uint64_t>(size.QuadPart) > limits.maxFileBytes) {
        CloseHandle(h);
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This drawing is too large to preview.", L"DXF bytes " + std::to_wstring(size.QuadPart)};
    }
    LineSource src(h);
    Reader r(limits);
    try { return r.Run(src); }
    catch (const std::bad_alloc&) { return ErrorInfo{ErrorCode::SizeLimitExceeded, L"Not enough memory to preview this drawing.", L"bad_alloc"}; }
}

Result<Drawing> ReadDxfText(const std::string& text, const Limits& limits) {
    if (text.size() > limits.maxFileBytes)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This drawing is too large to preview.", L""};
    LineSource src(text);
    Reader r(limits);
    try { return r.Run(src); }
    catch (const std::bad_alloc&) { return ErrorInfo{ErrorCode::SizeLimitExceeded, L"Not enough memory to preview this drawing.", L"bad_alloc"}; }
}

} // namespace mzr::cad
