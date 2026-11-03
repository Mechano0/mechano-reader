#include "../mzr_test.h"
#include "core/security/PathSanitizer.h"

using namespace mzr;
using namespace mzr::security;

static std::wstring Rel(std::wstring_view s) {
    auto r = SanitizeEntryName(s);
    return r ? r.value().relative : L"<rejected>";
}
static ErrorCode Code(std::wstring_view s) {
    auto r = SanitizeEntryName(s);
    return r ? ErrorCode::None : r.error().code;
}

TEST_CASE("normal names pass unchanged") {
    CHECK(Rel(L"report.pdf") == L"report.pdf");
    CHECK(Rel(L"docs/2024/report.pdf") == L"docs\\2024\\report.pdf");
    CHECK(Rel(L"docs\\a.txt") == L"docs\\a.txt");
    CHECK(Rel(L"folder/") == L"folder");
    auto r = SanitizeEntryName(L"docs/a.txt");
    CHECK(r.ok() && !r.value().wasModified);
}

TEST_CASE("arabic and unicode names are preserved") {
    CHECK(Rel(L"مستندات/تقرير المشروع.pdf") == L"مستندات\\تقرير المشروع.pdf");
    CHECK(Rel(L"日本語/файл.txt") == L"日本語\\файл.txt");
}

TEST_CASE("redundant separators and dot components are collapsed") {
    CHECK(Rel(L"a//b///c.txt") == L"a\\b\\c.txt");
    CHECK(Rel(L"./a/./b.txt") == L"a\\b.txt");
    CHECK(Rel(L"/leading/slash.txt") == L"leading\\slash.txt");
}

TEST_CASE("forbidden characters and ADS colons are replaced") {
    CHECK(Rel(L"a<b>c.txt") == L"a_b_c.txt");
    CHECK(Rel(L"file.txt:hidden.exe") == L"file.txt_hidden.exe");
    CHECK(Rel(L"q?*|\".txt") == L"q____.txt");
    CHECK(Rel(L"tab\there.txt") == L"tab_here.txt");
}

TEST_CASE("trailing dots and spaces are removed") {
    CHECK(Rel(L"name.txt. . ") == L"name.txt");
    CHECK(Rel(L"folder./x") == L"folder\\x");
    CHECK(Rel(L".../x") == L"_\\x");
}

TEST_CASE("reserved DOS device names are renamed") {
    CHECK(IsReservedDosName(L"CON"));
    CHECK(IsReservedDosName(L"nul.txt"));
    CHECK(IsReservedDosName(L"COM1.tar.gz"));
    CHECK(IsReservedDosName(L"lpt9"));
    CHECK(IsReservedDosName(L"COM\u00B9"));
    CHECK(!IsReservedDosName(L"CONSOLE.txt"));
    CHECK(!IsReservedDosName(L"COM10"));
    CHECK(Rel(L"dir/CON.txt") == L"dir\\_CON.txt");
}

TEST_CASE("bidi override characters are flagged") {
    auto r = SanitizeEntryName(L"invoice\u202Efdp.exe");
    CHECK(r.ok());
    CHECK(r.ok() && r.value().hasBidiOverride);
    CHECK(r.ok() && r.value().relative == L"invoice_fdp.exe");
}

TEST_CASE("limits: length and depth") {
    PathLimits lim; lim.maxDepth = 3;
    CHECK(!SanitizeEntryName(L"a/b/c/d.txt", lim).ok());
    CHECK(SanitizeEntryName(L"a/b/c.txt", lim).ok());
    CHECK(Code(std::wstring(300, L'x')) == ErrorCode::NameTooLong);
    CHECK(Code(std::wstring(2000, L'x')) == ErrorCode::NameTooLong);
}

TEST_CASE("empty names are rejected") {
    CHECK(Code(L"") == ErrorCode::EmptyName);
    CHECK(Code(L"/") == ErrorCode::EmptyName);
    CHECK(Code(L"./.") == ErrorCode::EmptyName);
}

TEST_CASE("IsPathInside respects component boundaries") {
    CHECK(IsPathInside(L"C:\\out", L"C:\\out\\a.txt"));
    CHECK(IsPathInside(L"C:\\out\\", L"C:\\out\\a.txt"));
    CHECK(IsPathInside(L"c:\\OUT", L"C:\\out\\a.txt"));
    CHECK(!IsPathInside(L"C:\\out", L"C:\\outside\\a.txt"));
    CHECK(!IsPathInside(L"C:\\out", L"C:\\out"));
    CHECK(!IsPathInside(L"C:\\out", L"D:\\out\\a.txt"));
    CHECK(IsPathInside(L"C:\\", L"C:\\a.txt"));
}

TEST_CASE("ResolveInsideDestination produces a full path under the root") {
    auto s = SanitizeEntryName(L"docs/a.txt");
    CHECK(s.ok());
    auto r = ResolveInsideDestination(L"C:\\Temp\\mzr_out", s.value());
    CHECK(r.ok());
    CHECK(r.ok() && r.value() == L"C:\\Temp\\mzr_out\\docs\\a.txt");
}

TEST_CASE("rename: new entry names are checked") {
    CHECK(CheckNewEntryName(L"report v2.pdf").empty());
    CHECK(CheckNewEntryName(L"مخطط الدور الأرضي.dwg").empty());
    CHECK(CheckNewEntryName(L".gitignore").empty());
    CHECK(!CheckNewEntryName(L"").empty());
    CHECK(!CheckNewEntryName(L"..").empty());
    CHECK(!CheckNewEntryName(L"a\\b.txt").empty());
    CHECK(!CheckNewEntryName(L"..\\..\\evil.txt").empty());
    CHECK(!CheckNewEntryName(L"a/b.txt").empty());
    CHECK(!CheckNewEntryName(L"c:x").empty());
    CHECK(!CheckNewEntryName(L"what?.txt").empty());
    CHECK(!CheckNewEntryName(L"name.").empty());
    CHECK(!CheckNewEntryName(L"name ").empty());
    CHECK(!CheckNewEntryName(L"CON").empty());
    CHECK(!CheckNewEntryName(L"nul.txt").empty());
    CHECK(!CheckNewEntryName(L"invoice\x202E" L"fdp.exe").empty());
    CHECK(!CheckNewEntryName(L"tab\there").empty());
    CHECK(!CheckNewEntryName(std::wstring(256, L'a')).empty());
}
