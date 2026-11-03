#include "core/update/Updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <memory>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace mzr::update {

const wchar_t kFeedUrl[] = L"https://github.com/Mechano0/mechano-reader/releases/latest/download/update.txt";
const wchar_t kReleasesPage[] = L"https://github.com/Mechano0/mechano-reader/releases/latest";

// Testing only: MECHANO_READER_UPDATE_FEED=<url> (https, or http on 127.0.0.1). The signature and the Setup hash are
// checked exactly the same way, so this cannot install anything that is not signed with the release key.
std::wstring FeedUrl() {
    wchar_t v[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"MECHANO_READER_UPDATE_FEED", v, 1024);
    if (n > 0 && n < 1024) {
        const std::wstring s(v, n);
        if (s.rfind(L"https://", 0) == 0 || s.rfind(L"http://127.0.0.1:", 0) == 0) return s;
    }
    return kFeedUrl;
}

namespace {

// Release key (made 2026-11-03 by sign-release.ps1 -NewKey; private part: 01-wip\.private\update-signing-key.blob).
constexpr char kReleaseKey[] =
    "RUNTMSAAAAC+enPvyWpdQaDHuoAycobXqOw/T6P91yNKfP825Rw/NpU7nzKmytOHXWiVgQvmW1M3t5VegBG2lZgnM80uOJbR";

ErrorInfo Err(ErrorCode c, const std::wstring& friendly, const std::wstring& details = {}) {
    return ErrorInfo{c, friendly, details};
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Sha256(const void* data, size_t n, uint8_t out[32]) {
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)),
                                     static_cast<ULONG>(n), out, 32));
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += hx[p[i] >> 4]; s += hx[p[i] & 15]; }
    return s;
}

bool VerifyP256(const std::vector<uint8_t>& blob, const std::string& message, const std::vector<uint8_t>& sig) {
    if (blob.size() != 72 || sig.size() != 64) return false;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                            const_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0)))
        return false;
    uint8_t hash[32];
    bool ok = Sha256(message.data(), message.size(), hash) &&
              BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, hash, 32, const_cast<PUCHAR>(sig.data()),
                                                   static_cast<ULONG>(sig.size()), 0));
    BCryptDestroyKey(key);
    return ok;
}

struct Http {
    HINTERNET s = nullptr, c = nullptr, r = nullptr;
    ~Http() { if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); if (s) WinHttpCloseHandle(s); }
};

// Opens a GET request and waits for the headers. Returns the status code (0 on a network error, `why` set).
DWORD OpenGet(Http& h, const std::wstring& url, std::wstring& why) {
    URL_COMPONENTS u{sizeof(u)};
    wchar_t host[256]{}, path[2048]{};
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) { why = L"bad address"; return 0; }
    const bool loopback = u.nScheme == INTERNET_SCHEME_HTTP && std::wstring(host) == L"127.0.0.1";   // local tests only
    if (u.nScheme != INTERNET_SCHEME_HTTPS && !loopback) { why = L"not an https address"; return 0; }
    h.s = WinHttpOpen(L"MechanoReader-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.s) h.s = WinHttpOpen(L"MechanoReader-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.s) { why = L"WinHttpOpen " + std::to_wstring(GetLastError()); return 0; }
    WinHttpSetTimeouts(h.s, 15000, 15000, 20000, 30000);
    h.c = WinHttpConnect(h.s, host, u.nPort, 0);
    if (!h.c) { why = L"connect " + std::to_wstring(GetLastError()); return 0; }
    h.r = WinHttpOpenRequest(h.c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, loopback ? 0 : WINHTTP_FLAG_SECURE);
    if (!h.r) { why = L"request " + std::to_wstring(GetLastError()); return 0; }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;      // GitHub -> its download server
    WinHttpSetOption(h.r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpSendRequest(h.r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h.r, nullptr)) {
        why = L"no answer (" + std::to_wstring(GetLastError()) + L")";
        return 0;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(h.r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    return status;
}

ErrorInfo NetError(DWORD status, const std::wstring& why) {
    if (status == 404) return Err(ErrorCode::Io, L"No update information has been published yet.", L"HTTP 404");
    if (status == 0) return Err(ErrorCode::Io, L"Could not reach GitHub. Check the internet connection and try again.", why);
    return Err(ErrorCode::Io, L"GitHub did not answer as expected. Try again later.", L"HTTP " + std::to_wstring(status));
}

} // namespace

std::vector<uint8_t> Base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return out;
    out.resize(n);
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()), CRYPT_STRING_BASE64, out.data(), &n, nullptr, nullptr)) out.clear();
    else out.resize(n);
    return out;
}

Result<Manifest> ParseManifest(const std::string& text, const std::vector<uint8_t>& publicKeyBlob) {
    const ErrorInfo bad = Err(ErrorCode::Corrupted, L"The update information is not signed correctly, so it was ignored.");
    if (text.size() > 64 * 1024 || text.rfind("MechanoReaderUpdate 1\n", 0) != 0) return bad;
    const size_t sigPos = text.find("\nsignature=");
    if (sigPos == std::string::npos) return bad;
    const std::string signedPart = text.substr(0, sigPos + 1);          // every byte above the signature line
    std::string sigText = text.substr(sigPos + 11);
    while (!sigText.empty() && (sigText.back() == '\n' || sigText.back() == '\r' || sigText.back() == ' ')) sigText.pop_back();
    const std::vector<uint8_t> key = publicKeyBlob.empty() ? Base64Decode(kReleaseKey) : publicKeyBlob;
    if (!VerifyP256(key, signedPart, Base64Decode(sigText))) return bad;

    Manifest m;
    size_t pos = 0;
    while (pos < signedPart.size()) {
        const size_t nl = signedPart.find('\n', pos);
        const std::string line = signedPart.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version") m.version = Utf8ToWide(v);
        else if (k == "setup_url") m.setupUrl = Utf8ToWide(v);
        else if (k == "setup_size") m.setupSize = _strtoui64(v.c_str(), nullptr, 10);
        else if (k == "setup_sha256") m.setupSha256 = v;
        else if (k == "notes") {
            std::string n;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == 'n') { n += '\n'; ++i; }
                else n += v[i];
            }
            m.notes = Utf8ToWide(n.substr(0, 4000));
        }
    }
    // Signed, but still check the facts are usable.
    bool okVer = !m.version.empty() && m.version.size() < 20;
    for (wchar_t c : m.version) if (!(iswdigit(c) || c == L'.')) okVer = false;
    bool okSha = m.setupSha256.size() == 64;
    for (char c : m.setupSha256) if (!isxdigit(static_cast<unsigned char>(c))) okSha = false;
    if (!okVer || !okSha || m.setupSize == 0 || m.setupSize > (512ull << 20) || (m.setupUrl.rfind(L"https://", 0) != 0 && m.setupUrl.rfind(L"http://127.0.0.1:", 0) != 0)) return bad;
    for (auto& c : m.setupSha256) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return m;
}

bool IsNewer(const std::wstring& a, const std::wstring& b) {
    auto parts = [](const std::wstring& s) {
        std::vector<unsigned long> v;
        size_t i = 0;
        while (i < s.size() && v.size() < 4) {
            v.push_back(wcstoul(s.c_str() + i, nullptr, 10));
            const size_t d = s.find(L'.', i);
            if (d == std::wstring::npos) break;
            i = d + 1;
        }
        v.resize(4, 0);
        return v;
    };
    const auto pa = parts(a), pb = parts(b);
    for (size_t i = 0; i < 4; ++i) if (pa[i] != pb[i]) return pa[i] > pb[i];
    return false;
}

Result<std::string> HttpGet(const std::wstring& url, size_t maxBytes) {
    Http h;
    std::wstring why;
    const DWORD status = OpenGet(h, url, why);
    if (status != 200) return NetError(status, why);
    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.r, &avail)) return Err(ErrorCode::Io, L"The connection was interrupted.", std::to_wstring(GetLastError()));
        if (avail == 0) break;
        if (out.size() + avail > maxBytes) return Err(ErrorCode::Io, L"The update information is too big, so it was ignored.");
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(h.r, out.data() + at, avail, &got)) return Err(ErrorCode::Io, L"The connection was interrupted.");
        out.resize(at + got);
    }
    return out;
}

std::string Sha256File(const std::wstring& file) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_HASH_HANDLE hh = nullptr;
    std::string res;
    if (BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hh, nullptr, 0, nullptr, 0, 0))) {
        std::vector<uint8_t> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
            ok = BCRYPT_SUCCESS(BCryptHashData(hh, buf.data(), got, 0));
        uint8_t d[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hh, d, 32, 0))) res = Hex(d, 32);
        BCryptDestroyHash(hh);
    }
    CloseHandle(f);
    return res;
}

Result<bool> DownloadVerified(const std::wstring& url, const std::wstring& file, uint64_t size, const std::string& sha256,
                              const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a) &&
            ((static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == size && Sha256File(file) == sha256)
            return true;                                                     // already downloaded and checked
    }
    Http h;
    std::wstring why;
    const DWORD status = OpenGet(h, url, why);
    if (status != 200) return NetError(status, why);
    const std::wstring part = file + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return Err(ErrorCode::Io, L"Could not save the update.", part);
    BCRYPT_HASH_HANDLE hh = nullptr;
    BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hh, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(256 * 1024);
    uint64_t total = 0;
    std::wstring fail;
    for (;;) {
        if (cancel && cancel->load()) { fail = L"cancelled"; break; }
        DWORD got = 0;
        if (!WinHttpReadData(h.r, buf.data(), static_cast<DWORD>(buf.size()), &got)) { fail = L"read " + std::to_wstring(GetLastError()); break; }
        if (got == 0) break;
        total += got;
        if (total > size) { fail = L"bigger than the signed size"; break; }
        DWORD w = 0;
        if (!WriteFile(f, buf.data(), got, &w, nullptr) || w != got) { fail = L"write"; break; }
        if (hh) BCryptHashData(hh, buf.data(), got, 0);
        if (progress) progress(static_cast<double>(total) / static_cast<double>(size));
    }
    CloseHandle(f);
    uint8_t d[32]{};
    const bool hashed = hh && BCRYPT_SUCCESS(BCryptFinishHash(hh, d, 32, 0));
    if (hh) BCryptDestroyHash(hh);
    if (fail.empty() && (total != size || !hashed || Hex(d, 32) != sha256)) fail = L"size or SHA-256 differs from the signed release";
    if (!fail.empty()) {
        DeleteFileW(part.c_str());
        if (fail == L"cancelled") return Err(ErrorCode::Cancelled, L"Cancelled.");
        return Err(ErrorCode::Corrupted, L"The downloaded update does not match the signed release, so it was deleted.", fail);
    }
    DeleteFileW(file.c_str());
    if (!MoveFileW(part.c_str(), file.c_str())) return Err(ErrorCode::Io, L"Could not save the update.", file);
    return true;
}

} // namespace mzr::update
