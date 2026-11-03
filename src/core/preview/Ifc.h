#pragma once
// IFC (open BIM, ISO 10303-21 text) info card: what is inside, without drawing it.
// The file is streamed line by line once: header (schema, application, author, date), project / site /
// building / storeys, how many elements of each kind, per storey. Limits: file size, entity length,
// entity count; every string and list is bounded. No geometry is computed.
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "utils/Result.h"

namespace mzr::preview {

constexpr uint64_t kMaxIfcBytes = 2ull << 30;

struct IfcStorey {
    std::wstring name;
    double elevation = 0;
    bool hasElevation = false;
    std::map<std::wstring, uint64_t> counts;          // category -> elements on this storey
    uint64_t total = 0;
};

struct IfcInfo {
    std::wstring schema;                              // "IFC4", "IFC2X3", "IFC4X3_ADD2"
    std::wstring viewDefinition;                      // "ReferenceView_V1.2", "CoordinationView_V2.0"
    std::wstring fileName, timeStamp, author, organization, preprocessor, application;
    std::wstring project, projectDescription, site, building;
    std::wstring lengthUnit = L"m";                   // "mm", "m", "ft" ...
    std::vector<IfcStorey> storeys;                   // in file order, then sorted by elevation
    std::map<std::wstring, uint64_t> counts;          // category -> all elements
    uint64_t elements = 0, entities = 0, propertySets = 0, types = 0;
    bool truncated = false;
};

// Reads an IFC file (text STEP). Binary / zipped IFC (.ifczip) is not handled here.
Result<IfcInfo> ReadIfcFile(const std::wstring& path);
Result<IfcInfo> ReadIfcText(const std::string& text);

// Turns STEP string escapes into text: '' -> ', \X2\0645...\X0\ -> UTF-16, \X\E9 -> latin-1, \S\ ...
std::wstring DecodeStepString(const std::string& raw);

std::string IfcToRtf(const IfcInfo& info, const std::wstring& fileName);

} // namespace mzr::preview
