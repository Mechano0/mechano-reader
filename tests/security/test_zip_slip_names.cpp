// Security tests: hostile entry names seen in real Zip Slip / traversal attacks.
// Every one of these MUST be rejected or neutralised so it lands inside the destination.
#include "../mzr_test.h"
#include "core/security/PathSanitizer.h"

using namespace mzr;
using namespace mzr::security;

static bool RejectedWith(std::wstring_view name, ErrorCode code) {
    auto r = SanitizeEntryName(name);
    return !r.ok() && r.error().code == code;
}

TEST_CASE("zip slip: parent traversal is rejected") {
    CHECK(RejectedWith(L"../evil.exe", ErrorCode::ParentTraversal));
    CHECK(RejectedWith(L"..\\evil.exe", ErrorCode::ParentTraversal));
    CHECK(RejectedWith(L"a/../../evil.exe", ErrorCode::ParentTraversal));
    CHECK(RejectedWith(L"a/b/../c.txt", ErrorCode::ParentTraversal));   // even "harmless" ones: no resolving
    CHECK(RejectedWith(L"..", ErrorCode::ParentTraversal));
}

TEST_CASE("zip slip: absolute, drive and UNC paths are rejected") {
    CHECK(RejectedWith(L"C:\\Windows\\System32\\evil.dll", ErrorCode::AbsolutePath));
    CHECK(RejectedWith(L"c:/Users/Public/x.exe", ErrorCode::AbsolutePath));
    CHECK(RejectedWith(L"C:relative.txt", ErrorCode::AbsolutePath));
    CHECK(RejectedWith(L"\\\\server\\share\\x.exe", ErrorCode::AbsolutePath));
    CHECK(RejectedWith(L"//server/share/x.exe", ErrorCode::AbsolutePath));
    CHECK(RejectedWith(L"\\\\?\\C:\\x.exe", ErrorCode::AbsolutePath));
    CHECK(RejectedWith(L"\\\\.\\PhysicalDrive0", ErrorCode::AbsolutePath));
}

TEST_CASE("null byte truncation is rejected") {
    const wchar_t raw[] = L"safe.txt\0.exe";
    CHECK(RejectedWith(std::wstring_view(raw, 13), ErrorCode::NullByteInName));
}

TEST_CASE("startup folder attack lands inside destination") {
    // Leading separator is made relative, so it cannot reach the real Startup folder.
    auto s = SanitizeEntryName(L"/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup/x.bat");
    CHECK(s.ok());
    auto r = ResolveInsideDestination(L"C:\\Temp\\out", s.value());
    CHECK(r.ok() && r.value().rfind(L"C:\\Temp\\out\\", 0) == 0);
}

TEST_CASE("ADS and device names cannot escape") {
    auto s = SanitizeEntryName(L"x:$DATA");      // drive-like => rejected
    CHECK(!s.ok());
    auto s2 = SanitizeEntryName(L"dir/file.txt::$DATA");
    CHECK(s2.ok() && s2.value().relative == L"dir\\file.txt__$DATA");
    auto s3 = SanitizeEntryName(L"aux");
    CHECK(s3.ok() && s3.value().relative == L"_aux");
}
