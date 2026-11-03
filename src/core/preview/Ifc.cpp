#include "core/preview/Ifc.h"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace mzr::preview {
namespace {

constexpr size_t kMaxEntityChars = 32u << 20;    // one #id=TYPE(...) record
constexpr uint64_t kMaxEntities = 50'000'000;

ErrorInfo Bad(const std::wstring& why) {
    return ErrorInfo{ErrorCode::Corrupted, L"This IFC file could not be read (it may be damaged or not an IFC text file).", why};
}

// Element types we count, with the name shown in the card. Everything else is "other data".
const std::unordered_map<std::string, const wchar_t*>& Categories() {
    static const std::unordered_map<std::string, const wchar_t*> m = {
        {"IFCWALL", L"Walls"}, {"IFCWALLSTANDARDCASE", L"Walls"}, {"IFCWALLELEMENTEDCASE", L"Walls"},
        {"IFCCURTAINWALL", L"Curtain walls"}, {"IFCSLAB", L"Slabs / floors"}, {"IFCSLABSTANDARDCASE", L"Slabs / floors"},
        {"IFCROOF", L"Roofs"}, {"IFCDOOR", L"Doors"}, {"IFCDOORSTANDARDCASE", L"Doors"}, {"IFCWINDOW", L"Windows"},
        {"IFCWINDOWSTANDARDCASE", L"Windows"}, {"IFCCOLUMN", L"Columns"}, {"IFCCOLUMNSTANDARDCASE", L"Columns"},
        {"IFCBEAM", L"Beams"}, {"IFCBEAMSTANDARDCASE", L"Beams"}, {"IFCMEMBER", L"Members"}, {"IFCMEMBERSTANDARDCASE", L"Members"},
        {"IFCPLATE", L"Plates"}, {"IFCPLATESTANDARDCASE", L"Plates"}, {"IFCSTAIR", L"Stairs"}, {"IFCSTAIRFLIGHT", L"Stair flights"},
        {"IFCRAMP", L"Ramps"}, {"IFCRAMPFLIGHT", L"Ramp flights"}, {"IFCRAILING", L"Railings"}, {"IFCCOVERING", L"Coverings / ceilings"},
        {"IFCFOOTING", L"Footings"}, {"IFCPILE", L"Piles"}, {"IFCCHIMNEY", L"Chimneys"}, {"IFCSHADINGDEVICE", L"Shading devices"},
        {"IFCSPACE", L"Spaces / rooms"}, {"IFCZONE", L"Zones"}, {"IFCOPENINGELEMENT", L"Openings"},
        {"IFCFURNISHINGELEMENT", L"Furniture"}, {"IFCFURNITURE", L"Furniture"}, {"IFCSYSTEMFURNITUREELEMENT", L"Furniture"},
        {"IFCBUILDINGELEMENTPROXY", L"Generic elements"}, {"IFCBUILDINGELEMENTPART", L"Element parts"},
        {"IFCDISCRETEACCESSORY", L"Accessories"}, {"IFCMECHANICALFASTENER", L"Fasteners"}, {"IFCREINFORCINGBAR", L"Rebars"},
        {"IFCREINFORCINGMESH", L"Rebar meshes"}, {"IFCTENDON", L"Tendons"},
        {"IFCDUCTSEGMENT", L"Ducts"}, {"IFCDUCTFITTING", L"Duct fittings"}, {"IFCAIRTERMINAL", L"Air terminals"},
        {"IFCPIPESEGMENT", L"Pipes"}, {"IFCPIPEFITTING", L"Pipe fittings"}, {"IFCVALVE", L"Valves"}, {"IFCPUMP", L"Pumps"},
        {"IFCSANITARYTERMINAL", L"Sanitary fixtures"}, {"IFCFIRESUPPRESSIONTERMINAL", L"Sprinklers / fire terminals"},
        {"IFCCABLESEGMENT", L"Cables"}, {"IFCCABLECARRIERSEGMENT", L"Cable trays"}, {"IFCCABLECARRIERFITTING", L"Cable tray fittings"},
        {"IFCLIGHTFIXTURE", L"Light fixtures"}, {"IFCLAMP", L"Lamps"}, {"IFCOUTLET", L"Outlets"}, {"IFCSWITCHINGDEVICE", L"Switches"},
        {"IFCELECTRICAPPLIANCE", L"Electric appliances"}, {"IFCELECTRICDISTRIBUTIONBOARD", L"Distribution boards"},
        {"IFCUNITARYEQUIPMENT", L"HVAC units"}, {"IFCFAN", L"Fans"}, {"IFCCHILLER", L"Chillers"}, {"IFCBOILER", L"Boilers"},
        {"IFCTANK", L"Tanks"}, {"IFCSENSOR", L"Sensors"}, {"IFCALARM", L"Alarms"}, {"IFCFLOWSEGMENT", L"Flow segments"},
        {"IFCFLOWFITTING", L"Flow fittings"}, {"IFCFLOWTERMINAL", L"Flow terminals"}, {"IFCFLOWCONTROLLER", L"Flow controllers"},
        {"IFCFLOWMOVINGDEVICE", L"Flow moving devices"}, {"IFCENERGYCONVERSIONDEVICE", L"Energy conversion devices"},
        {"IFCDISTRIBUTIONELEMENT", L"Distribution elements"}, {"IFCDISTRIBUTIONCONTROLELEMENT", L"Control elements"},
        {"IFCTRANSPORTELEMENT", L"Lifts / escalators"}, {"IFCGEOGRAPHICELEMENT", L"Landscape / terrain"},
        {"IFCCIVILELEMENT", L"Civil elements"}, {"IFCELEMENTASSEMBLY", L"Assemblies"}, {"IFCANNOTATION", L"Annotations"},
        {"IFCGRID", L"Grids"}, {"IFCVIRTUALELEMENT", L"Virtual elements"}, {"IFCROAD", L"Roads"}, {"IFCBRIDGE", L"Bridges"},
        {"IFCRAILWAY", L"Railways"}, {"IFCALIGNMENT", L"Alignments"}, {"IFCPAVEMENT", L"Pavements"}, {"IFCKERB", L"Kerbs"},
        {"IFCEARTHWORKSFILL", L"Earthworks"}, {"IFCCOURSE", L"Courses"}, {"IFCBEARING", L"Bearings"}};
    return m;
}

// Splits the top-level arguments of "TYPE(a,'b,c',(1,2),$)" -> {"a", "'b,c'", "(1,2)", "$"}.
std::vector<std::string_view> Args(std::string_view body) {
    std::vector<std::string_view> out;
    int depth = 0;
    bool str = false;
    size_t start = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (str) { if (c == '\'') { if (i + 1 < body.size() && body[i + 1] == '\'') ++i; else str = false; } continue; }
        if (c == '\'') str = true;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) { out.push_back(body.substr(start, i - start)); start = i + 1; if (out.size() > 64) return out; }
    }
    out.push_back(body.substr(start));
    return out;
}

std::wstring Str(std::string_view a) {
    if (a.size() < 2 || a.front() != '\'' || a.back() != '\'') return {};
    return DecodeStepString(std::string(a.substr(1, a.size() - 2)));
}
uint64_t Ref(std::string_view a) {
    if (a.size() < 2 || a[0] != '#') return 0;
    uint64_t v = 0;
    std::from_chars(a.data() + 1, a.data() + a.size(), v);
    return v;
}
std::vector<uint64_t> RefList(std::string_view a, size_t cap) {
    std::vector<uint64_t> out;
    for (size_t i = 0; i < a.size() && out.size() < cap; ++i) {
        if (a[i] != '#') continue;
        uint64_t v = 0;
        auto r = std::from_chars(a.data() + i + 1, a.data() + a.size(), v);
        if (r.ec == std::errc()) out.push_back(v);
        i = static_cast<size_t>(r.ptr - a.data());
    }
    return out;
}
bool Num(std::string_view a, double& v) {
    while (!a.empty() && a.front() == ' ') a.remove_prefix(1);
    auto r = std::from_chars(a.data(), a.data() + a.size(), v);
    return r.ec == std::errc() && std::isfinite(v);
}

class Reader {
public:
    Result<IfcInfo> Run(const std::function<bool(std::string&)>& nextLine) {
        std::string line, rec;
        bool inData = false, sawIso = false, inString = false;
        while (nextLine(line)) {
            if (!sawIso) {
                if (line.find("ISO-10303-21") == std::string::npos) {
                    if (line.find_first_not_of(" \t\r") != std::string::npos) return Bad(L"no ISO-10303-21 header");
                    continue;
                }
                sawIso = true;
                continue;
            }
            // Records end with ';' outside strings; one record can span lines.
            for (char c : line) {
                if (c == '\'') inString = !inString;
                rec += c;
                if (!inString && c == ';') { Record(rec, inData); rec.clear(); }
                if (rec.size() > kMaxEntityChars) return Bad(L"record too long");
            }
            if (!rec.empty()) rec += ' ';
            if (info_.entities >= kMaxEntities) { info_.truncated = true; break; }
            if (stop_) break;
            (void)inData;
        }
        if (!sawIso) return Bad(L"not a STEP file");
        Finish();
        return std::move(info_);
    }

private:
    void Record(std::string& r, bool& inData) {
        size_t b = r.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return;
        std::string_view v(r);
        v.remove_prefix(b);
        if (v.rfind("DATA;", 0) == 0) { inData = true; return; }
        if (v.rfind("ENDSEC;", 0) == 0) { inData = false; return; }
        if (v.rfind("END-ISO-10303-21", 0) == 0) { stop_ = true; return; }
        if (!inData) { Header(v); return; }
        if (v.empty() || v[0] != '#') return;
        const size_t eq = v.find('=');
        const size_t open = v.find('(', eq);
        if (eq == std::string_view::npos || open == std::string_view::npos) return;
        uint64_t id = 0;
        std::from_chars(v.data() + 1, v.data() + eq, id);
        std::string type(v.substr(eq + 1, open - eq - 1));
        while (!type.empty() && type.back() == ' ') type.pop_back();
        while (!type.empty() && type.front() == ' ') type.erase(0, 1);
        const size_t close = v.rfind(')');
        if (close == std::string_view::npos || close <= open) return;
        const std::string_view body = v.substr(open + 1, close - open - 1);
        ++info_.entities;
        Entity(id, type, body);
    }

    void Header(std::string_view v) {
        auto field = [&](const char* name) -> std::string_view {
            if (v.rfind(name, 0) != 0) return {};
            const size_t o = v.find('('), c = v.rfind(')');
            return (o == std::string_view::npos || c == std::string_view::npos || c <= o) ? std::string_view() : v.substr(o + 1, c - o - 1);
        };
        if (auto f = field("FILE_SCHEMA"); !f.empty()) {
            const size_t q = f.find('\''), e = f.find('\'', q + 1);
            if (q != std::string_view::npos && e != std::string_view::npos) info_.schema = Str(f.substr(q, e - q + 1));
        } else if (auto n = field("FILE_NAME"); !n.empty()) {
            const auto a = Args(n);
            if (a.size() > 0) info_.fileName = Str(a[0]);
            if (a.size() > 1) info_.timeStamp = Str(a[1]);
            if (a.size() > 2) for (auto x : Args(a[2].size() > 2 ? a[2].substr(1, a[2].size() - 2) : a[2])) { auto s = Str(x); if (!s.empty()) { info_.author = s; break; } }
            if (a.size() > 3) for (auto x : Args(a[3].size() > 2 ? a[3].substr(1, a[3].size() - 2) : a[3])) { auto s = Str(x); if (!s.empty()) { info_.organization = s; break; } }
            if (a.size() > 4) info_.preprocessor = Str(a[4]);
            if (a.size() > 5) info_.application = Str(a[5]);
        } else if (auto d = field("FILE_DESCRIPTION"); !d.empty()) {
            const size_t p = d.find("ViewDefinition [");
            if (p != std::string_view::npos) {
                const size_t e = d.find(']', p);
                if (e != std::string_view::npos) info_.viewDefinition = std::wstring(d.begin() + static_cast<ptrdiff_t>(p + 16), d.begin() + static_cast<ptrdiff_t>(e));
            }
        }
    }

    void Entity(uint64_t id, const std::string& type, std::string_view body) {
        const auto& cats = Categories();
        if (auto it = cats.find(type); it != cats.end()) {
            ++info_.elements;
            ++info_.counts[it->second];
            if (elementCat_.size() < 20'000'000) elementCat_[id] = it->second;
            if (type != "IFCSPACE" && type != "IFCOPENINGELEMENT") return;     // spaces can also contain elements
        }
        if (type == "IFCPROPERTYSET" || type == "IFCELEMENTQUANTITY") { ++info_.propertySets; return; }
        if (type.size() > 4 && type.compare(type.size() - 4, 4, "TYPE") == 0 && type != "IFCSURFACESTYLE") { ++info_.types; }
        if (type == "IFCPROJECT") {
            const auto a = Args(body);
            if (a.size() > 2) info_.project = Str(a[2]);
            if (a.size() > 3) info_.projectDescription = Str(a[3]);
            if (info_.project.empty() && a.size() > 5) info_.project = Str(a[5]);   // LongName
        } else if (type == "IFCSITE" && info_.site.empty()) {
            const auto a = Args(body);
            if (a.size() > 2) info_.site = Str(a[2]);
        } else if (type == "IFCBUILDING" && info_.building.empty()) {
            const auto a = Args(body);
            if (a.size() > 2) info_.building = Str(a[2]);
            if (info_.building.empty() && a.size() > 7) info_.building = Str(a[7]);
        } else if (type == "IFCBUILDINGSTOREY") {
            const auto a = Args(body);
            if (storeyIndex_.size() >= 10000) return;
            IfcStorey s;
            if (a.size() > 2) s.name = Str(a[2]);
            if (s.name.empty() && a.size() > 7) s.name = Str(a[7]);
            if (a.size() > 9) s.hasElevation = Num(a[9], s.elevation);
            storeyIndex_[id] = info_.storeys.size();
            info_.storeys.push_back(std::move(s));
        } else if (type == "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
            const auto a = Args(body);
            if (a.size() > 5 && contained_.size() < 2'000'000) contained_.push_back({Ref(a[5]), RefList(a[4], 5'000'000)});
        } else if (type == "IFCRELAGGREGATES") {
            const auto a = Args(body);
            if (a.size() > 5) for (uint64_t child : RefList(a[5], 1'000'000)) if (parent_.size() < 20'000'000) parent_[child] = Ref(a[4]);
        } else if (type == "IFCSIUNIT") {
            const auto a = Args(body);
            if (a.size() > 3 && a[1].find("LENGTHUNIT") != std::string_view::npos && !lengthSet_) {
                lengthSet_ = true;
                const bool milli = a[2].find("MILLI") != std::string_view::npos, centi = a[2].find("CENTI") != std::string_view::npos;
                info_.lengthUnit = milli ? L"mm" : centi ? L"cm" : L"m";
            }
        } else if (type == "IFCCONVERSIONBASEDUNIT") {
            const auto a = Args(body);
            if (a.size() > 2 && a[1].find("LENGTHUNIT") != std::string_view::npos && !lengthSet_) {
                const std::wstring n = Str(a[2]);
                if (n.find(L"FOOT") != std::wstring::npos || n.find(L"foot") != std::wstring::npos) { info_.lengthUnit = L"ft"; lengthSet_ = true; }
                else if (n.find(L"INCH") != std::wstring::npos || n.find(L"inch") != std::wstring::npos) { info_.lengthUnit = L"in"; lengthSet_ = true; }
            }
        }
    }

    void Finish() {
        // Elements -> storey: directly, or through a space / other container that belongs to a storey.
        auto storeyOf = [&](uint64_t container) -> int64_t {
            for (int hop = 0; hop < 16 && container; ++hop) {
                if (auto s = storeyIndex_.find(container); s != storeyIndex_.end()) return static_cast<int64_t>(s->second);
                auto p = parent_.find(container);
                if (p == parent_.end()) break;
                container = p->second;
            }
            return -1;
        };
        for (const auto& [container, elems] : contained_) {
            const int64_t si = storeyOf(container);
            if (si < 0) continue;
            IfcStorey& st = info_.storeys[static_cast<size_t>(si)];
            for (uint64_t e : elems) {
                auto c = elementCat_.find(e);
                if (c == elementCat_.end()) continue;
                ++st.counts[c->second];
                ++st.total;
            }
        }
        std::stable_sort(info_.storeys.begin(), info_.storeys.end(), [](const IfcStorey& a, const IfcStorey& b) {
            return (a.hasElevation ? a.elevation : 0) < (b.hasElevation ? b.elevation : 0);
        });
    }

    IfcInfo info_;
    bool stop_ = false, lengthSet_ = false;
    std::unordered_map<uint64_t, const wchar_t*> elementCat_;
    std::unordered_map<uint64_t, size_t> storeyIndex_;
    std::unordered_map<uint64_t, uint64_t> parent_;
    std::vector<std::pair<uint64_t, std::vector<uint64_t>>> contained_;
};

} // namespace

std::wstring DecodeStepString(const std::string& raw) {
    std::wstring out;
    for (size_t i = 0; i < raw.size() && out.size() < 65536; ++i) {
        const char c = raw[i];
        if (c == '\'' && i + 1 < raw.size() && raw[i + 1] == '\'') { out += L'\''; ++i; continue; }
        if (c == '\\' && i + 3 < raw.size() && raw[i + 1] == 'X' && raw[i + 2] == '2' && raw[i + 3] == '\\') {
            size_t j = i + 4;                                                          // \X2\ 0645 0631 \X0\  (UTF-16 hex)
            while (j + 4 <= raw.size() && raw[j] != '\\') {
                unsigned v = 0;
                auto r = std::from_chars(raw.data() + j, raw.data() + j + 4, v, 16);
                if (r.ec != std::errc() || r.ptr != raw.data() + j + 4) break;
                out += static_cast<wchar_t>(v);
                j += 4;
            }
            if (raw.compare(j, 4, "\\X0\\") == 0) j += 4;
            i = j - 1;
            continue;
        }
        if (c == '\\' && i + 4 < raw.size() && raw[i + 1] == 'X' && raw[i + 2] == '\\') {   // \X\E9  (latin-1)
            unsigned v = 0;
            auto r = std::from_chars(raw.data() + i + 3, raw.data() + i + 5, v, 16);
            if (r.ec == std::errc()) { out += static_cast<wchar_t>(v); i += 4; continue; }
        }
        if (c == '\\' && i + 3 < raw.size() && raw[i + 1] == 'S' && raw[i + 2] == '\\') {    // \S\c = c + 128
            out += static_cast<wchar_t>(static_cast<unsigned char>(raw[i + 3]) + 128);
            i += 3;
            continue;
        }
        if (c == '\\' && i + 3 < raw.size() && raw[i + 1] == 'P' && raw[i + 3] == '\\') { i += 3; continue; }   // code page switch
        if (c == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') { out += L'\\'; ++i; continue; }
        out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return out;
}

Result<IfcInfo> ReadIfcText(const std::string& text) {
    size_t pos = 0;
    Reader r;
    return r.Run([&](std::string& line) {
        if (pos >= text.size()) return false;
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        line.assign(text, pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = e + 1;
        return true;
    });
}

Result<IfcInfo> ReadIfcFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ErrorInfo{ErrorCode::Io, L"The IFC file could not be opened.", L"CreateFile " + std::to_wstring(GetLastError())};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (static_cast<uint64_t>(size.QuadPart) > kMaxIfcBytes) { CloseHandle(h); return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This IFC file is too large to preview.", L""}; }
    std::vector<char> buf(1 << 20);
    size_t have = 0, at = 0;
    bool eof = false;
    Reader r;
    Result<IfcInfo> res = ErrorInfo{};
    try {
        res = r.Run([&](std::string& line) {
            line.clear();
            for (;;) {
                if (at >= have) {
                    if (eof) return !line.empty();
                    DWORD got = 0;
                    if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) { eof = true; continue; }
                    have = got; at = 0;
                }
                const char* s = buf.data() + at;
                const char* nl = static_cast<const char*>(memchr(s, '\n', have - at));
                const size_t n = nl ? static_cast<size_t>(nl - s) : have - at;
                line.append(s, n);
                at += n + (nl ? 1 : 0);
                if (line.size() > kMaxEntityChars) return true;          // the reader refuses it
                if (nl) break;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        });
    } catch (const std::bad_alloc&) {
        res = ErrorInfo{ErrorCode::SizeLimitExceeded, L"Not enough memory to read this IFC file.", L"bad_alloc"};
    }
    CloseHandle(h);
    return res;
}

// ============================================================ card

namespace {
void Esc(std::string& out, const std::wstring& s) {
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
        else if (c == L'\n') out += "\\line ";
        else if (c < 0x20) continue;
        else if (c < 0x80) out += static_cast<char>(c);
        else out += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?";
    }
}
void Row(std::string& rtf, const std::vector<int>& widths, const std::vector<std::wstring>& cells, bool header = false) {
    rtf += "\\trowd\\trgaph90\\trleft0";
    int x = 0;
    for (int w : widths) { x += w; rtf += "\\clbrdrb\\brdrs\\brdrw5\\brdrcf2"; if (header) rtf += "\\clcbpat3"; rtf += "\\cellx" + std::to_string(x); }
    rtf += "\\pard\\intbl\\sb40\\sa40 ";        // the space ends the control word ("00 floor" must not become \sa4000)
    for (size_t i = 0; i < widths.size(); ++i) {
        if (header) rtf += "\\b ";
        Esc(rtf, i < cells.size() ? cells[i].substr(0, 300) : L"");
        if (header) rtf += "\\b0 ";
        rtf += "\\cell ";
    }
    rtf += "\\row\n";
}
void Heading(std::string& rtf, const std::wstring& t) { rtf += "\\pard\\sb240\\sa80\\b\\fs24 "; Esc(rtf, t); rtf += "\\b0\\fs20\\par\n"; }
std::wstring Count(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}
std::wstring Elev(const IfcStorey& s, const std::wstring& unit) {
    if (!s.hasElevation) return L"";
    wchar_t b[64];
    swprintf_s(b, L"%.3f", s.elevation);
    std::wstring v = b;
    while (v.find(L'.') != std::wstring::npos && (v.back() == L'0' || v.back() == L'.')) v.pop_back();
    return v + L" " + unit;
}
} // namespace

std::string IfcToRtf(const IfcInfo& info, const std::wstring& fileName) {
    std::string rtf = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}}"
                      "{\\colortbl ;\\red96\\green96\\blue96;\\red210\\green210\\blue214;\\red240\\green241\\blue243;}"
                      "\\viewkind4\\f0\\fs20\n\\pard\\sa40\\b\\fs30 ";
    Esc(rtf, fileName);
    rtf += "\\b0\\fs20\\par\n\\pard\\sa160\\cf1 ";
    std::wstring sub = L"IFC model  \u00B7  " + (info.schema.empty() ? std::wstring(L"schema unknown") : info.schema);
    if (!info.application.empty()) sub += L"  \u00B7  exported by " + info.application;
    else if (!info.preprocessor.empty()) sub += L"  \u00B7  exported by " + info.preprocessor;
    sub += L"  \u00B7  " + Count(info.elements) + L" elements";
    Esc(rtf, sub);
    rtf += "\\cf0\\par\n";

    Heading(rtf, L"File");
    const std::vector<int> two{2600, 6400};
    auto row = [&](const wchar_t* k, const std::wstring& v) { if (!v.empty()) Row(rtf, two, {k, v}); };
    row(L"Project", info.project);
    row(L"Description", info.projectDescription);
    row(L"Site", info.site);
    row(L"Building", info.building);
    row(L"Author", info.author);
    row(L"Organization", info.organization);
    row(L"Created", info.timeStamp.size() >= 16 ? info.timeStamp.substr(0, 10) + L" " + info.timeStamp.substr(11, 5) : info.timeStamp);
    row(L"Authoring tool", info.preprocessor);
    row(L"Model view", info.viewDefinition);
    row(L"Length unit", info.lengthUnit);

    if (!info.storeys.empty()) {
        Heading(rtf, L"Storeys (" + std::to_wstring(info.storeys.size()) + L")");
        const std::vector<int> w{3600, 1800, 1400, 2200};
        Row(rtf, w, {L"Storey", L"Elevation", L"Elements", L"Most of it"}, true);
        for (const auto& s : info.storeys) {
            std::wstring top;
            uint64_t best = 0;
            for (const auto& [k, v] : s.counts) if (v > best) { best = v; top = k; }
            Row(rtf, w, {s.name.empty() ? L"(no name)" : s.name, Elev(s, info.lengthUnit), Count(s.total), top});
        }
    }
    if (!info.counts.empty()) {
        Heading(rtf, L"Elements by type");
        std::vector<std::pair<std::wstring, uint64_t>> v(info.counts.begin(), info.counts.end());
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        const std::vector<int> w{4600, 1800};
        Row(rtf, w, {L"Type", L"Count"}, true);
        for (const auto& [k, n] : v) Row(rtf, w, {k, Count(n)});
    }
    Heading(rtf, L"Data");
    row(L"Property sets", Count(info.propertySets));
    row(L"Element types", Count(info.types));
    row(L"Records in the file", Count(info.entities));
    rtf += "\\pard\\sb240\\cf1\\fs16 ";
    Esc(rtf, info.truncated ? L"Very large file: only the first part was counted. " : L"");
    Esc(rtf, L"Double-click opens the model in its main program (a BIM viewer or Revit).");
    rtf += "\\cf0\\fs20\\par\n}";
    return rtf;
}

} // namespace mzr::preview
