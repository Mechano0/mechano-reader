// App updates: the signed update.txt is accepted only with a good signature (test key, made like
// sign-release.ps1 does), any changed byte is refused, version order, the download check (size + SHA-256).
#include <cstring>
#include <fstream>
#include "../mzr_test.h"
#include "core/update/Updater.h"
#include "core/filesystem/FileSystem.h"

using namespace mzr;
using namespace mzr::update;

namespace {
// A throw-away test key pair (NOT the release key). The body below was signed with its private part.
const char kTestPub[] = "RUNTMSAAAABP9EomYT4S4PrPMOskMAmo44ocRsml2GhTX9JVrPb77dtNWinybOErM1uAqoBktLRFUEdt0UTAG0fgksfFN+bz";
const char kTestSig[] = "NgN5keC9tpWJKeKHg/+WTWtZJSyhqkHRoxnxkFnsKGdFCb1YUrmcwC4x+n+V1t/An2ci5Lbs+nidFxczUWfJMA==";
std::string Ab32() { std::string s; for (int i = 0; i < 32; ++i) s += "ab"; return s; }   // 64 hex
std::string Body() {
    return "MechanoReaderUpdate 1\nversion=1.2.3\nsetup_url=https://example.com/Mechano.Reader.Setup.exe\nsetup_size=1234\n"
           "setup_sha256=" + Ab32() + "\nnotes=First line\\nSecond line\n";
}
} // namespace

TEST_CASE("update: signed update.txt is accepted, any change is refused") {
    const auto key = Base64Decode(kTestPub);
    CHECK(key.size() == 72);
    const std::string good = Body() + "signature=" + kTestSig + "\n";
    auto r = ParseManifest(good, key);
    CHECK(r.ok());
    if (!r.ok()) return;
    CHECK(r.value().version == L"1.2.3");
    CHECK(r.value().setupUrl == L"https://example.com/Mechano.Reader.Setup.exe");
    CHECK(r.value().setupSize == 1234);
    CHECK(r.value().setupSha256 == Ab32());
    CHECK(r.value().notes == L"First line\nSecond line");

    // The real release key does not accept the test signature.
    CHECK(!ParseManifest(good).ok());
    // One changed character anywhere above the signature -> refused.
    std::string t = good; t[t.find("1.2.3")] = '9';
    CHECK(!ParseManifest(t, key).ok());
    t = good; t[t.find("1234")] = '9';
    CHECK(!ParseManifest(t, key).ok());
    t = good; t.replace(t.find("example.com"), 7, "evil.co");
    CHECK(!ParseManifest(t, key).ok());
    // Broken signature / missing parts / junk.
    t = good; t[t.find("signature=") + 12] ^= 1;
    CHECK(!ParseManifest(t, key).ok());
    CHECK(!ParseManifest(Body(), key).ok());
    CHECK(!ParseManifest("", key).ok());
    CHECK(!ParseManifest(std::string(100000, 'x'), key).ok());
    CHECK(!ParseManifest("<html>Not Found</html>", key).ok());
    // Windows line ends after the signature are fine.
    CHECK(ParseManifest(Body() + "signature=" + kTestSig + "\r\n", key).ok());
}

TEST_CASE("update: version order") {
    CHECK(IsNewer(L"0.9.1", L"0.9.0"));
    CHECK(IsNewer(L"0.10.0", L"0.9.9"));
    CHECK(IsNewer(L"1.0", L"0.99.99"));
    CHECK(!IsNewer(L"0.9.0", L"0.9.0"));
    CHECK(!IsNewer(L"0.8.9", L"0.9.0"));
    CHECK(!IsNewer(L"", L"0.9.0"));
}

TEST_CASE("update: file SHA-256") {
    const std::wstring dir = fs::NewTempSubfolder(L"upd-");
    CHECK(!dir.empty());
    const std::wstring f = dir + L"\\abc.bin";
    { std::ofstream o(f, std::ios::binary); o << "abc"; }
    CHECK(Sha256File(f) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256File(dir + L"\\missing.bin").empty());
    DeleteFileW(f.c_str());
    RemoveDirectoryW(dir.c_str());
}
