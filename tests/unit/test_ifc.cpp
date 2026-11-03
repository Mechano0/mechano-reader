// IFC info card: header, project, storeys with elevations, element counts per storey, Arabic names,
// records over several lines, and hostile text (never a crash or a hang).
#include <filesystem>
#include "../mzr_test.h"
#include "core/preview/Ifc.h"

using namespace mzr;
using namespace mzr::preview;

namespace {
const char* kIfc =
    "ISO-10303-21;\n"
    "HEADER;\n"
    "FILE_DESCRIPTION(('ViewDefinition [CoordinationView_V2.0]'),'2;1');\n"
    "FILE_NAME('villa.ifc','2024-01-31T10:20:30',('Eslam'),('Mechano'),'The EXPRESS Data Manager Version 5.02.0100.07 : 28 Aug 2013','20220726_1500(x64) - Exporter 23.0.0.0 - Default UI','');\n"
    "FILE_SCHEMA(('IFC2X3'));\n"
    "ENDSEC;\n"
    "DATA;\n"
    "#1=IFCPROJECT('0a',$,'\\X2\\0641064A0644064A\\X0\\ A','Villa project',$,$,$,$,$);\n"
    "#2=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n"
    "#3=IFCSITE('0b',$,'Default site',$,$,$,$,$,.ELEMENT.,$,$,$,$,$);\n"
    "#4=IFCBUILDING('0c',$,'Villa',$,$,$,$,$,.ELEMENT.,$,$,$);\n"
    "#10=IFCBUILDINGSTOREY('1a',$,'First floor',$,$,$,$,$,.ELEMENT.,3500.);\n"
    "#11=IFCBUILDINGSTOREY('1b',$,'\\X2\\06270644062F0648063100200627064406230631063606CA\\X0\\',$,$,$,$,$,.ELEMENT.,0.);\n"
    "#20=IFCWALLSTANDARDCASE('2a',$,'Wall 1',$,$,$,$,$);\n"
    "#21=IFCWALL('2b',$,'Wall 2',$,$,$,$,$);\n"
    "#22=IFCDOOR('2c',$,'Door, ''main''',$,$,$,$,$,\n  2100.,900.);\n"
    "#23=IFCWINDOW('2d',$,'Window',$,$,$,$,$,1200.,1500.);\n"
    "#24=IFCSPACE('2e',$,'Kitchen',$,$,$,$,$,.ELEMENT.,.INTERNAL.,$);\n"
    "#25=IFCFURNISHINGELEMENT('2f',$,'Table',$,$,$,$,$);\n"
    "#26=IFCCOLUMN('2g',$,'C1',$,$,$,$,$);\n"
    "#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('3a',$,$,$,(#20,#21,#22),#11);\n"
    "#31=IFCRELCONTAINEDINSPATIALSTRUCTURE('3b',$,$,$,(#23,#26),#10);\n"
    "#32=IFCRELAGGREGATES('3c',$,$,$,#11,(#24));\n"
    "#33=IFCRELCONTAINEDINSPATIALSTRUCTURE('3d',$,$,$,(#25),#24);\n"
    "#40=IFCPROPERTYSET('4a',$,'Pset_WallCommon',$,());\n"
    "#41=IFCWALLTYPE('4b',$,'Basic 200',$,$,$,$,$,$,.STANDARD.);\n"
    "ENDSEC;\n"
    "END-ISO-10303-21;\n";
} // namespace

TEST_CASE("ifc: header, project, storeys and element counts") {
    auto r = ReadIfcText(kIfc);
    CHECK(r.ok());
    if (!r.ok()) return;
    const IfcInfo& i = r.value();
    CHECK(i.schema == L"IFC2X3" && i.viewDefinition == L"CoordinationView_V2.0");
    CHECK(i.author == L"Eslam" && i.organization == L"Mechano" && i.application.find(L"20220726_1500") != std::wstring::npos);
    CHECK(i.project == L"فيلي A" && i.building == L"Villa" && i.site == L"Default site");
    CHECK(i.lengthUnit == L"mm");
    CHECK(i.elements == 7 && i.counts.at(L"Walls") == 2 && i.counts.at(L"Doors") == 1 && i.counts.at(L"Spaces / rooms") == 1);
    CHECK(i.propertySets == 1 && i.types == 1);
    CHECK(i.storeys.size() == 2);
    if (i.storeys.size() == 2) {
        CHECK(i.storeys[0].elevation == 0 && i.storeys[0].name.rfind(L"الدور", 0) == 0);   // sorted by elevation
        CHECK(i.storeys[0].total == 4 && i.storeys[0].counts.at(L"Furniture") == 1);                                // through the space
        CHECK(i.storeys[1].name == L"First floor" && i.storeys[1].total == 2);
    }
    CHECK(DecodeStepString("Door, ''main''") == L"Door, 'main'");
    const std::string rtf = IfcToRtf(i, L"villa.ifc");
    CHECK(rtf.find("Walls") != std::string::npos && rtf.find("First floor") != std::string::npos);
    for (unsigned char c : rtf) if (c >= 0x80) { CHECK(false); break; }
    // A cell that starts with digits must not glue onto the RTF control word before it (\sa40 + "00" = \sa4000).
    IfcInfo d;
    IfcStorey st;
    st.name = L"00 groundfloor";
    d.storeys.push_back(st);
    const std::string r2 = IfcToRtf(d, L"x.ifc");
    CHECK(r2.find("\\sa40 00 groundfloor") != std::string::npos && r2.find("\\sa4000") == std::string::npos);
}

TEST_CASE("ifc: real buildingSMART samples (IFC4 and IFC2x3)") {
    const auto dir = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / L"00-data" / L"test-archives" / L"ifc-samples";
    for (const wchar_t* f : {L"Building-Architecture-IFC4.ifc", L"Building-Architecture-IFC2x3.ifc"}) {
        auto r = ReadIfcFile((dir / f).wstring());
        if (!r.ok() && r.error().code == ErrorCode::Io) { std::printf("      (skipped: sample IFC not found)\n"); return; }
        CHECK(r.ok());
        if (!r.ok()) continue;
        CHECK(r.value().counts.count(L"Walls") && r.value().counts.at(L"Walls") == 4);
        CHECK(r.value().storeys.size() == 1 && r.value().storeys[0].total > 0);
        CHECK(!r.value().project.empty());
    }
}

TEST_CASE("ifc: hostile text never crashes or hangs") {
    CHECK(!ReadIfcText("").ok());
    CHECK(!ReadIfcText("hello world\n").ok());
    CHECK(ReadIfcText("ISO-10303-21;\nDATA;\n#1=IFCWALL('unterminated;\n").ok());
    std::string big = "ISO-10303-21;\nDATA;\n#1=IFCWALL('" + std::string(40u << 20, 'x');
    CHECK(!ReadIfcText(big).ok());                                            // record too long
    std::string nested = "ISO-10303-21;\nDATA;\n#1=IFCRELCONTAINEDINSPATIALSTRUCTURE('a',$,$,$," + std::string(100000, '(') + "#5" + std::string(100000, ')') + ",#2);\n";
    CHECK(ReadIfcText(nested).ok());
    std::string loops = "ISO-10303-21;\nDATA;\n#1=IFCRELAGGREGATES('a',$,$,$,#2,(#3));\n#2=IFCRELAGGREGATES('b',$,$,$,#3,(#2));\n"
                        "#3=IFCWALL('w',$,$,$,$,$,$,$);\n#4=IFCRELCONTAINEDINSPATIALSTRUCTURE('c',$,$,$,(#3),#2);\nENDSEC;\n";
    CHECK(ReadIfcText(loops).ok());                                           // parent loop: bounded hops
    CHECK(DecodeStepString("\\X2\\06\\X0\\") .size() < 10);
    CHECK(DecodeStepString("\\X2\\ZZZZ\\X0\\").size() < 20);
    CHECK(DecodeStepString(std::string(200000, 'a')).size() <= 65536);
}
