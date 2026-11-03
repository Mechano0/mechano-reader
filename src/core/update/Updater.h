#pragma once
// App updates (optional; Eslam 2026-11-03: "like Mechano DM"). The ONLY thing in the app that uses the internet,
// and only when the user allows it (menu > App updates: Tell me / Install automatically / Off).
//
// The release on GitHub carries update.txt (made by sign-release.ps1):
//   MechanoReaderUpdate 1
//   version=0.9.1
//   setup_url=https://github.com/Mechano0/mechano-reader/releases/latest/download/Mechano.Reader.Setup.exe
//   setup_size=8291666
//   setup_sha256=<64 hex>
//   notes=<one line, "\n" = new line>
//   signature=<base64 ECDSA P-256 / SHA-256 signature, r||s, over every byte above this line>
// Only the public key is in the app: a changed update.txt is refused, and the downloaded Setup must have the
// signed size and SHA-256 before it is run.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "utils/Result.h"

namespace mzr::update {

struct Manifest {
    std::wstring version;          // "0.9.1"
    std::wstring setupUrl;         // https only
    uint64_t     setupSize = 0;
    std::string  setupSha256;      // 64 lowercase hex
    std::wstring notes;            // may contain new lines
};

// Where update.txt is published (latest release).
extern const wchar_t kFeedUrl[];
extern const wchar_t kReleasesPage[];
// kFeedUrl, or the MECHANO_READER_UPDATE_FEED test address (https, or http on 127.0.0.1).
std::wstring FeedUrl();

// Checks the format and the signature. `publicKeyBlob` = BCRYPT_ECCPUBLIC_BLOB (P-256); empty = the release key.
Result<Manifest> ParseManifest(const std::string& text, const std::vector<uint8_t>& publicKeyBlob = {});

// "0.10.0" > "0.9.3"
bool IsNewer(const std::wstring& candidate, const std::wstring& current);

// Small HTTPS GET (update.txt). Follows redirects. maxBytes protects against a huge answer.
Result<std::string> HttpGet(const std::wstring& url, size_t maxBytes);

// Downloads to `file` (via file.part) and checks size + SHA-256. Keeps an already verified file.
// progress(0..1) is called from this thread. Returns false in `cancel` -> stops.
Result<bool> DownloadVerified(const std::wstring& url, const std::wstring& file, uint64_t size, const std::string& sha256,
                              const std::function<void(double)>& progress, const std::atomic<bool>* cancel);

// SHA-256 of a file as lowercase hex ("" on error).
std::string Sha256File(const std::wstring& file);

std::vector<uint8_t> Base64Decode(const std::string& s);

} // namespace mzr::update
