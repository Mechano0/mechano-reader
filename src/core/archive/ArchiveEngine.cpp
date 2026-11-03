// ArchiveEngine.cpp - the 7z.dll bridge.
// This is the ONLY translation unit that includes 7-Zip SDK headers, and the
// only one that defines the 7-Zip interface GUIDs (MyInitGuid.h).
#include "core/archive/ArchiveEngine.h"

#include <windows.h>
#include <oleauto.h>
#include <propidl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// ---- 7-Zip SDK (interfaces only; no 7-Zip .cpp files are compiled in) ----
#include "Common/MyInitGuid.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"

#include "core/filesystem/FileSystem.h"
#include "core/security/PathSanitizer.h"

namespace mzr::archive {
namespace {

// ============================================================ DLL + formats

using Func_CreateObjectT = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
using Func_GetNumberOfFormatsT = HRESULT(WINAPI*)(UInt32*);
using Func_GetHandlerProperty2T = HRESULT(WINAPI*)(UInt32, PROPID, PROPVARIANT*);

struct FormatInfo {
    std::wstring name;
    GUID clsid{};
    std::vector<std::wstring> extensions;   // lower case, no dot
    bool canUpdate = false;
};

struct Engine {
    std::once_flag once;
    HMODULE dll = nullptr;
    Func_CreateObjectT createObject = nullptr;
    std::vector<FormatInfo> formats;
    std::wstring error;
};
Engine g_engine;

// RAII PROPVARIANT
struct Prop {
    PROPVARIANT v;
    Prop() { PropVariantInit(&v); }
    ~Prop() { PropVariantClear(&v); }
    Prop(const Prop&) = delete;
    Prop& operator=(const Prop&) = delete;
    PROPVARIANT* operator&() { PropVariantClear(&v); PropVariantInit(&v); return &v; }
};

std::wstring PropStr(const PROPVARIANT& v) {
    return (v.vt == VT_BSTR && v.bstrVal) ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : std::wstring();
}
bool PropBool(const PROPVARIANT& v) { return v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE; }
uint64_t PropU64(const PROPVARIANT& v) {
    switch (v.vt) {
    case VT_UI1: return v.bVal;
    case VT_UI2: return v.uiVal;
    case VT_UI4: return v.ulVal;
    case VT_UI8: return v.uhVal.QuadPart;
    case VT_I4:  return v.lVal < 0 ? 0 : static_cast<uint64_t>(v.lVal);
    case VT_I8:  return v.hVal.QuadPart < 0 ? 0 : static_cast<uint64_t>(v.hVal.QuadPart);
    default:     return 0;
    }
}

std::wstring ExeFolder() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return buf.substr(0, buf.find_last_of(L'\\'));
}

void InitEngine() {
    auto& e = g_engine;
    const std::wstring dllPath = ExeFolder() + L"\\7z.dll";
    // Full path + restricted search: a 7z.dll planted in the current folder is never loaded.
    e.dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!e.dll) { e.error = L"7z.dll was not found next to the program: " + dllPath; return; }

#pragma warning(push)
#pragma warning(disable : 4191)   // FARPROC cast
    e.createObject = reinterpret_cast<Func_CreateObjectT>(GetProcAddress(e.dll, "CreateObject"));
    auto numFormats = reinterpret_cast<Func_GetNumberOfFormatsT>(GetProcAddress(e.dll, "GetNumberOfFormats"));
    auto handlerProp = reinterpret_cast<Func_GetHandlerProperty2T>(GetProcAddress(e.dll, "GetHandlerProperty2"));
#pragma warning(pop)
    if (!e.createObject || !numFormats || !handlerProp) { e.error = L"7z.dll is damaged or the wrong version."; return; }

    UInt32 n = 0;
    numFormats(&n);
    for (UInt32 i = 0; i < n; ++i) {
        FormatInfo f;
        Prop p;
        handlerProp(i, NArchive::NHandlerPropID::kName, &p);
        f.name = PropStr(p.v);
        handlerProp(i, NArchive::NHandlerPropID::kClassID, &p);
        if (p.v.vt != VT_BSTR || SysStringByteLen(p.v.bstrVal) != sizeof(GUID)) continue;
        memcpy(&f.clsid, p.v.bstrVal, sizeof(GUID));
        handlerProp(i, NArchive::NHandlerPropID::kExtension, &p);
        std::wstring exts = PropStr(p.v);
        CharLowerBuffW(exts.data(), static_cast<DWORD>(exts.size()));
        for (size_t s = 0; s < exts.size();) {
            size_t t = exts.find(L' ', s);
            if (t == std::wstring::npos) t = exts.size();
            if (t > s) f.extensions.push_back(exts.substr(s, t - s));
            s = t + 1;
        }
        handlerProp(i, NArchive::NHandlerPropID::kUpdate, &p);
        f.canUpdate = PropBool(p.v);
        e.formats.push_back(std::move(f));
    }
    if (e.formats.empty()) e.error = L"7z.dll reports no archive formats.";
}

const FormatInfo* FindFormat(std::wstring_view name) {
    for (const auto& f : g_engine.formats)
        if (CompareStringOrdinal(f.name.c_str(), -1, name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL) return &f;
    return nullptr;
}

// Formats tried when the extension did not identify the archive. Executable /
// disk-image handlers (PE, ELF, MBR, VHD...) are deliberately NOT in this list:
// opening a random .exe as an "archive" would surprise users.
const wchar_t* const kFallbackFormats[] = {
    L"zip", L"7z", L"Rar5", L"Rar", L"tar", L"gzip", L"bzip2", L"xz", L"zstd",
    L"Iso", L"Udf", L"Cab", L"wim", L"lzma", L"arj", L"lzh", L"Cpio", L"Rpm", L"Z"};

template <typename T>
T* CreateObj(const GUID& clsid, const GUID& iid) {
    void* p = nullptr;
    if (!g_engine.createObject || g_engine.createObject(&clsid, &iid, &p) != S_OK) return nullptr;
    return static_cast<T*>(p);
}

// Minimal intrusive smart pointer for COM objects.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : p_(p) {}                 // adopts reference
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr& operator=(const ComPtr& o) { if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    ~ComPtr() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    template <typename U> ComPtr<U> As(const GUID& iid) const {
        void* q = nullptr;
        if (p_ && p_->QueryInterface(iid, &q) == S_OK) return ComPtr<U>(static_cast<U*>(q));
        return {};
    }
private:
    T* p_ = nullptr;
};

#define MZR_IUNKNOWN_REFCOUNT                                                          \
    std::atomic<ULONG> ref_{1};                                                        \
public:                                                                                \
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++ref_; }              \
    ULONG STDMETHODCALLTYPE Release() noexcept override {                              \
        const ULONG r = --ref_;                                                        \
        if (!r) delete this;                                                           \
        return r;                                                                      \
    }

// ============================================================ streams

class InFileStream final : public IInStream, public IStreamGetSize {
    MZR_IUNKNOWN_REFCOUNT
public:
    explicit InFileStream(HANDLE h) : h_(h) {}
    ~InFileStream() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

    static ComPtr<InFileStream> OpenRead(const std::wstring& path, DWORD* err = nullptr) {
        HANDLE h = CreateFileW(fs::ToLongPath(path).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return {}; }
        return ComPtr<InFileStream>(new InFileStream(h));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream || iid == IID_IInStream) *out = static_cast<IInStream*>(this);
        else if (iid == IID_IStreamGetSize) *out = static_cast<IStreamGetSize*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processed) noexcept override {
        DWORD got = 0;
        const BOOL ok = ReadFile(h_, data, size, &got, nullptr);
        if (processed) *processed = got;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* newPos) noexcept override {
        if (origin > 2) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER d; d.QuadPart = offset;
        LARGE_INTEGER np{};
        if (!SetFilePointerEx(h_, d, &np, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (newPos) *newPos = static_cast<UInt64>(np.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSize(UInt64* size) noexcept override {
        LARGE_INTEGER s{};
        if (!GetFileSizeEx(h_, &s)) return HRESULT_FROM_WIN32(GetLastError());
        *size = static_cast<UInt64>(s.QuadPart);
        return S_OK;
    }
private:
    HANDLE h_;
};

// Read-only stream over bytes in RAM (Office files inside an archive).
class InMemStream final : public IInStream, public IStreamGetSize {
    MZR_IUNKNOWN_REFCOUNT
public:
    explicit InMemStream(std::shared_ptr<const std::vector<uint8_t>> d) : d_(std::move(d)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream || iid == IID_IInStream) *out = static_cast<IInStream*>(this);
        else if (iid == IID_IStreamGetSize) *out = static_cast<IStreamGetSize*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processed) noexcept override {
        const uint64_t n = d_->size();
        const uint64_t avail = pos_ < n ? n - pos_ : 0;
        const UInt32 take = static_cast<UInt32>(std::min<uint64_t>(avail, size));
        if (take) memcpy(data, d_->data() + pos_, take);
        pos_ += take;
        if (processed) *processed = take;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* newPos) noexcept override {
        Int64 base = origin == 0 ? 0 : origin == 1 ? static_cast<Int64>(pos_) : origin == 2 ? static_cast<Int64>(d_->size()) : -1;
        if (base < 0) return STG_E_INVALIDFUNCTION;
        // Overflow-safe (S8): base >= 0, so only a huge positive offset can wrap.
        if (offset > 0 && offset > INT64_MAX - base) return STG_E_INVALIDFUNCTION;
        const Int64 np = base + offset;
        if (np < 0) return HRESULT_FROM_WIN32(ERROR_NEGATIVE_SEEK);
        pos_ = static_cast<uint64_t>(np);
        if (newPos) *newPos = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSize(UInt64* size) noexcept override { *size = d_->size(); return S_OK; }
private:
    std::shared_ptr<const std::vector<uint8_t>> d_;
    uint64_t pos_ = 0;
};

// Output stream used for both archive writing (needs Seek/SetSize) and file
// extraction (sequential; feeds the bomb guard + cancel check).
class OutFileStream final : public IOutStream {
    MZR_IUNKNOWN_REFCOUNT
public:
    OutFileStream(HANDLE h, security::ExtractGuard* guard, CancelToken* cancel, ErrorInfo* abortReason)
        : h_(h), guard_(guard), cancel_(cancel), abort_(abortReason) {}
    ~OutFileStream() { if (h_ != INVALID_HANDLE_VALUE && owns_) CloseHandle(h_); }
    void SetOwnsHandle(bool v) { owns_ = v; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream || iid == IID_IOutStream) *out = static_cast<IOutStream*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* processed) noexcept override {
        if (processed) *processed = 0;
        if (cancel_ && cancel_->IsCancelled()) return E_ABORT;
        if (guard_) {
            auto r = guard_->OnBytesWritten(size);
            if (!r) { if (abort_) *abort_ = r.error(); return E_ABORT; }
        }
        DWORD w = 0;
        if (!WriteFile(h_, data, size, &w, nullptr)) {
            if (abort_ && abort_->code == ErrorCode::None)
                *abort_ = ErrorInfo{ErrorCode::Io, L"Writing to the disk failed (the disk may be full or removed).",
                                    L"WriteFile error " + std::to_wstring(GetLastError())};
            return E_FAIL;
        }
        if (processed) *processed = w;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* newPos) noexcept override {
        if (origin > 2) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER d; d.QuadPart = offset;
        LARGE_INTEGER np{};
        if (!SetFilePointerEx(h_, d, &np, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (newPos) *newPos = static_cast<UInt64>(np.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(UInt64 newSize) noexcept override {
        LARGE_INTEGER cur{}, zero{}, ns{};
        SetFilePointerEx(h_, zero, &cur, FILE_CURRENT);
        ns.QuadPart = static_cast<LONGLONG>(newSize);
        if (!SetFilePointerEx(h_, ns, nullptr, FILE_BEGIN) || !SetEndOfFile(h_)) return HRESULT_FROM_WIN32(GetLastError());
        SetFilePointerEx(h_, cur, nullptr, FILE_BEGIN);
        return S_OK;
    }
private:
    HANDLE h_;
    bool owns_ = false;
    security::ExtractGuard* guard_;
    CancelToken* cancel_;
    ErrorInfo* abort_;
};

void WipeString(std::wstring& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}

// ============================================================ open callback

class OpenCallback final : public IArchiveOpenCallback, public ICryptoGetTextPassword, public IArchiveOpenVolumeCallback {
    MZR_IUNKNOWN_REFCOUNT
public:
    OpenCallback(std::wstring archivePath, PasswordProvider pw, CancelToken* cancel)
        : path_(std::move(archivePath)), provider_(std::move(pw)), cancel_(cancel) {
        const auto slash = path_.find_last_of(L'\\');
        // No folder (archive in RAM) -> no volume lookup at all (never the current directory).
        dir_ = slash == std::wstring::npos ? L"" : path_.substr(0, slash);
        name_ = slash == std::wstring::npos ? path_ : path_.substr(slash + 1);
    }
    ~OpenCallback() { WipeString(password_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback) *out = static_cast<IArchiveOpenCallback*>(this);
        else if (iid == IID_ICryptoGetTextPassword) *out = static_cast<ICryptoGetTextPassword*>(this);
        else if (iid == IID_IArchiveOpenVolumeCallback) *out = static_cast<IArchiveOpenVolumeCallback*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) noexcept override {
        return (cancel_ && cancel_->IsCancelled()) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) noexcept override {
        passwordAsked_ = true;
        if (password_.empty()) {
            if (!provider_) return E_ABORT;
            auto pw = provider_(name_);
            if (!pw) { userCancelled_ = true; return E_ABORT; }
            password_ = *pw;
            WipeString(*pw);
        }
        *password = SysAllocString(password_.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }
    // Multi-volume support (archive.7z.001/.002, .zip/.z01, .part1.rar ...).
    HRESULT STDMETHODCALLTYPE GetProperty(PROPID propID, PROPVARIANT* value) noexcept override {
        PropVariantInit(value);
        if (propID == kpidName) {
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(currentVolumeName_.empty() ? name_.c_str() : currentVolumeName_.c_str());
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, IInStream** stream) noexcept override {
        *stream = nullptr;
        // The handler derives volume names from the first name; still, never allow
        // it to reach outside the archive's own folder.
        const std::wstring n = name ? name : L"";
        if (dir_.empty()) return S_FALSE;
        if (n.empty() || n.find_first_of(L"\\/:") != std::wstring::npos || n == L"." || n == L"..") return S_FALSE;
        auto s = InFileStream::OpenRead(dir_ + L"\\" + n);
        if (!s) return S_FALSE;
        currentVolumeName_ = n;
        *stream = s.get();
        s->AddRef();
        return S_OK;
    }

    std::wstring TakePassword() { std::wstring p = password_; return p; }
    bool PasswordAsked() const { return passwordAsked_; }
    bool UserCancelled() const { return userCancelled_; }

private:
    std::wstring path_, dir_, name_, currentVolumeName_;
    PasswordProvider provider_;
    CancelToken* cancel_;
    std::wstring password_;
    bool passwordAsked_ = false;
    bool userCancelled_ = false;
};

// ============================================================ extract callback

std::wstring FriendlyOpResult(Int32 r, bool encrypted) {
    using namespace NArchive::NExtract::NOperationResult;
    switch (r) {
    case kUnsupportedMethod: return L"This file uses a compression method that is not supported.";
    case kCRCError:
    case kDataError:
        return encrypted ? L"The password is wrong, or the file is damaged."
                         : L"This file is damaged (the archive appears to be corrupted).";
    case kWrongPassword: return L"The password is wrong.";
    case kUnexpectedEnd: return L"The archive ends unexpectedly (it may be incomplete or a volume is missing).";
    case kDataAfterEnd:  return L"There is extra data after the end of this file.";
    case kIsNotArc:      return L"This is not a valid archive.";
    case kHeadersError:  return L"The archive headers are damaged.";
    case kUnavailable:   return L"The data for this file is not available (a volume may be missing).";
    default:             return L"This file could not be extracted.";
    }
}

class ExtractCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
    MZR_IUNKNOWN_REFCOUNT
public:
    ExtractCallback(const std::vector<EntryRecord>& entries, const ExtractOptions* opt, bool testMode,
                    ProgressSink* progress, CancelToken& cancel, security::ExtractGuard* guard,
                    std::wstring password, PasswordProvider provider, std::wstring archiveName,
                    std::string zoneId, OperationReport& report)
        : entries_(entries), opt_(opt), test_(testMode), progress_(progress), cancel_(cancel), guard_(guard),
          password_(std::move(password)), provider_(std::move(provider)), archiveName_(std::move(archiveName)),
          zoneId_(std::move(zoneId)), report_(report) {}
    ~ExtractCallback() { CloseCurrent(false); WipeString(password_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback || iid == IID_IProgress)
            *out = static_cast<IArchiveExtractCallback*>(this);
        else if (iid == IID_ICryptoGetTextPassword) *out = static_cast<ICryptoGetTextPassword*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }

    // ---- IProgress
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) noexcept override {
        if (progress_) progress_->OnTotal(total);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* v) noexcept override {
        if (v && progress_) progress_->OnCompleted(*v);
        return cancel_.IsCancelled() ? E_ABORT : S_OK;
    }

    // ---- password
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) noexcept override {
        if (password_.empty()) {
            if (!provider_) { report_.wrongPassword = true; return E_ABORT; }
            auto pw = provider_(archiveName_);
            if (!pw) { report_.cancelled = true; return E_ABORT; }
            password_ = *pw;
            WipeString(*pw);
        }
        *password = SysAllocString(password_.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }

    // ---- extraction
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream** out, Int32 askMode) noexcept override {
        *out = nullptr;
        cur_ = index;
        curFile_.clear();
        curFinal_.clear();
        if (cancel_.IsCancelled()) return E_ABORT;
        if (index >= entries_.size()) return S_OK;
        const EntryRecord& e = entries_[index];
        if (progress_) progress_->OnCurrentItem(e.path);
        if (askMode != NArchive::NExtract::NAskMode::kExtract || test_) return S_OK;

        // 1. Things we never materialise.
        if (e.isAltStream) { Skip(e, L"An NTFS alternate data stream was skipped."); return S_OK; }
        if (e.isLink && opt_->links == LinkMode::Skip) {
            Skip(e, L"A symbolic link or hard link was skipped for safety.");
            return S_OK;
        }

        // 2. Untrusted name -> safe relative path.
        std::wstring raw = e.path;
        if (!opt_->stripPrefix.empty() && raw.size() > opt_->stripPrefix.size()) {
            std::wstring norm = raw;
            for (auto& c : norm) if (c == L'/') c = L'\\';
            if (CompareStringOrdinal(norm.c_str(), static_cast<int>(opt_->stripPrefix.size()),
                                     opt_->stripPrefix.c_str(), static_cast<int>(opt_->stripPrefix.size()), TRUE) == CSTR_EQUAL)
                raw = norm.substr(opt_->stripPrefix.size());
        }
        auto san = security::SanitizeEntryName(raw);
        if (!san) { Skip(e, san.error().friendly, san.error().details); return S_OK; }
        if (san.value().hasBidiOverride)
            report_.issues.push_back({e.path, L"The name contained hidden text-direction characters that can disguise the real file type. They were replaced with \"_\".", L""});

        // 3. Prove it stays inside the destination.
        auto full = security::ResolveInsideDestination(opt_->destination, san.value());
        if (!full) { Skip(e, full.error().friendly, full.error().details); return S_OK; }
        std::wstring target = full.value();

        if (e.isDir) {
            std::wstring err;
            if (!fs::CreateDirectories(target, &err)) report_.issues.push_back({e.path, L"Could not create a folder.", err});
            else ++report_.foldersDone;
            return S_OK;
        }

        // 4. Parent folder.
        const std::wstring parent = target.substr(0, target.find_last_of(L'\\'));
        std::wstring err;
        if (!fs::CreateDirectories(parent, &err)) { Skip(e, L"Could not create the folder for this file.", err); return S_OK; }

        // 5. Existing file policy. An existing link is never written through.
        DWORD disposition = CREATE_NEW;
        std::wstring overwriteTarget;          // set = write to a temp file, then replace this one
        if (fs::Exists(target)) {
            if (fs::IsReparsePoint(target) || fs::IsDirectory(target)) {
                if (opt_->overwrite == OverwriteMode::Skip) { ++report_.skipped; return S_OK; }
                target = fs::UniqueName(target);
            } else {
                switch (opt_->overwrite) {
                case OverwriteMode::Skip: ++report_.skipped; return S_OK;
                case OverwriteMode::Rename: target = fs::UniqueName(target); break;
                case OverwriteMode::Overwrite:
                    // S4: never truncate the user's file up front. The entry is written to a new
                    // temp file next to it and only replaces the original once it decoded OK
                    // (a damaged entry / wrong password / cancel leaves the original untouched,
                    // and a hard-linked original is replaced, not written through).
                    overwriteTarget = target;
                    break;
                }
            }
        }

        // 6. Bomb limits per entry.
        if (guard_) {
            auto g = guard_->BeginEntry(e.packSize);
            if (!g) { abortReason_ = g.error(); return E_ABORT; }
        }

        HANDLE h = INVALID_HANDLE_VALUE;
        if (overwriteTarget.empty()) {
            h = CreateFileW(fs::ToLongPath(target).c_str(), GENERIC_WRITE, 0, nullptr, disposition,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        } else {
            for (int attempt = 0; attempt < 16 && h == INVALID_HANDLE_VALUE; ++attempt) {
                target = parent + L"\\~mzr" + fs::RandomHex() + L".tmp";
                h = CreateFileW(fs::ToLongPath(target).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
            }
        }
        if (h == INVALID_HANDLE_VALUE) {
            Skip(e, L"Could not create the file (it may be open in another program, or access is denied).",
                 L"CreateFile error " + std::to_wstring(GetLastError()) + L": " + target);
            return S_OK;
        }
        curHandle_ = h;
        curFile_ = target;
        curFinal_ = overwriteTarget;
        auto* stream = new OutFileStream(h, guard_, &cancel_, &abortReason_);
        *out = stream;   // ref = 1, handed to 7-Zip; we keep the handle ownership
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 opRes) noexcept override {
        const bool ok = opRes == NArchive::NExtract::NOperationResult::kOK;
        const EntryRecord* e = cur_ < entries_.size() ? &entries_[cur_] : nullptr;
        if (!ok) {
            if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword ||
                (e && e->encrypted && (opRes == NArchive::NExtract::NOperationResult::kCRCError ||
                                       opRes == NArchive::NExtract::NOperationResult::kDataError)))
                report_.wrongPassword = true;
            report_.issues.push_back({e ? e->path : L"", FriendlyOpResult(opRes, e && e->encrypted),
                                      L"7-Zip operation result " + std::to_wstring(opRes)});
        }
        if (test_) {
            if (ok && e && !e->isDir) { ++report_.filesDone; report_.bytesDone += e->size; }
        } else if (curHandle_ != INVALID_HANDLE_VALUE) {
            const bool written = CloseCurrent(ok);
            if (written && e) { ++report_.filesDone; report_.bytesDone += e->size; }
        }
        return S_OK;
    }

    ErrorInfo abortReason_;

private:
    void Skip(const EntryRecord& e, std::wstring friendly, std::wstring details = {}) {
        ++report_.skipped;
        report_.issues.push_back({e.path, std::move(friendly), std::move(details)});
    }

    // Finish the current output file: times, safe attributes, Mark of the Web.
    // A failed/partial file is deleted so a damaged file is never left looking valid.
    // Returns true when the file is in place.
    bool CloseCurrent(bool success) {
        if (curHandle_ == INVALID_HANDLE_VALUE) return false;
        const EntryRecord* e = cur_ < entries_.size() ? &entries_[cur_] : nullptr;
        if (success && e && opt_ && opt_->restoreTimes && (e->mtime.dwLowDateTime || e->mtime.dwHighDateTime))
            SetFileTime(curHandle_, nullptr, nullptr, &e->mtime);
        CloseHandle(curHandle_);
        curHandle_ = INVALID_HANDLE_VALUE;
        if (!success) {
            DeleteFileW(fs::ToLongPath(curFile_).c_str());     // the temp copy when overwriting: original kept
            return false;
        }
        if (opt_ && opt_->propagateMotW && !zoneId_.empty()) fs::WriteZoneIdentifier(curFile_, zoneId_);
        if (!curFinal_.empty()) {
            // Overwrite: the new file decoded OK, now it replaces the old one.
            const std::wstring finalLong = fs::ToLongPath(curFinal_);
            const DWORD oldAttr = GetFileAttributesW(finalLong.c_str());
            if (oldAttr != INVALID_FILE_ATTRIBUTES &&
                (oldAttr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                SetFileAttributesW(finalLong.c_str(), FILE_ATTRIBUTE_NORMAL);   // else the replace is refused
            if (!MoveFileExW(fs::ToLongPath(curFile_).c_str(), finalLong.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                const DWORD err = GetLastError();
                if (oldAttr != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(finalLong.c_str(), oldAttr);
                DeleteFileW(fs::ToLongPath(curFile_).c_str());
                ++report_.skipped;
                report_.issues.push_back({e ? e->path : L"",
                                          L"The existing file could not be replaced (it may be open in another program). It was kept as it was.",
                                          L"MoveFileEx error " + std::to_wstring(err) + L": " + curFinal_});
                return false;
            }
            curFile_ = curFinal_;
        }
        if (e) {
            // Only harmless attributes are restored (never SYSTEM, never reparse, never Unix bits).
            const DWORD safe = e->attrib & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
            if (safe) SetFileAttributesW(fs::ToLongPath(curFile_).c_str(), safe);
        }
        return true;
    }

    const std::vector<EntryRecord>& entries_;
    const ExtractOptions* opt_;
    bool test_;
    ProgressSink* progress_;
    CancelToken& cancel_;
    security::ExtractGuard* guard_;
    std::wstring password_;
    PasswordProvider provider_;
    std::wstring archiveName_;
    std::string zoneId_;
    OperationReport& report_;
    UInt32 cur_ = 0xFFFFFFFF;
    HANDLE curHandle_ = INVALID_HANDLE_VALUE;
    std::wstring curFile_;
    std::wstring curFinal_;     // overwrite mode: the existing file curFile_ replaces on success
};

// ============================================================ update callback

struct UpdateItem {
    bool         newData = false;
    bool         newProps = false;
    UInt32       arcIndex = 0xFFFFFFFF;     // for kept / renamed
    std::wstring path;                      // archive path
    bool         isDir = false;
    uint64_t     size = 0;
    uint32_t     attrib = 0;
    FILETIME     mtime{};
    std::wstring diskPath;                  // newData only
};

class UpdateCallback final : public IArchiveUpdateCallback, public ICryptoGetTextPassword2 {
    MZR_IUNKNOWN_REFCOUNT
public:
    UpdateCallback(const std::vector<UpdateItem>& items, std::wstring password, ProgressSink* progress,
                   CancelToken& cancel, OperationReport& report)
        : items_(items), password_(std::move(password)), progress_(progress), cancel_(cancel), report_(report) {}
    ~UpdateCallback() { WipeString(password_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_IArchiveUpdateCallback || iid == IID_IProgress)
            *out = static_cast<IArchiveUpdateCallback*>(this);
        else if (iid == IID_ICryptoGetTextPassword2) *out = static_cast<ICryptoGetTextPassword2*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) noexcept override {
        if (progress_) progress_->OnTotal(total);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* v) noexcept override {
        if (v && progress_) progress_->OnCompleted(*v);
        return cancel_.IsCancelled() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32 index, Int32* newData, Int32* newProps, UInt32* indexInArchive) noexcept override {
        if (index >= items_.size()) return E_INVALIDARG;
        const auto& it = items_[index];
        if (newData) *newData = it.newData ? 1 : 0;
        if (newProps) *newProps = it.newProps ? 1 : 0;
        if (indexInArchive) *indexInArchive = it.newData ? static_cast<UInt32>(-1) : it.arcIndex;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID propID, PROPVARIANT* value) noexcept override {
        PropVariantInit(value);
        if (index >= items_.size()) return E_INVALIDARG;
        const auto& it = items_[index];
        switch (propID) {
        case kpidIsAnti: value->vt = VT_BOOL; value->boolVal = VARIANT_FALSE; break;
        case kpidPath:   value->vt = VT_BSTR; value->bstrVal = SysAllocString(it.path.c_str()); break;
        case kpidIsDir:  value->vt = VT_BOOL; value->boolVal = it.isDir ? VARIANT_TRUE : VARIANT_FALSE; break;
        case kpidSize:   value->vt = VT_UI8;  value->uhVal.QuadPart = it.size; break;
        case kpidAttrib: value->vt = VT_UI4;  value->ulVal = it.attrib; break;
        case kpidMTime:
            if (it.mtime.dwLowDateTime || it.mtime.dwHighDateTime) { value->vt = VT_FILETIME; value->filetime = it.mtime; }
            break;
        default: break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialInStream** in) noexcept override {
        *in = nullptr;
        if (index >= items_.size()) return E_INVALIDARG;
        const auto& it = items_[index];
        if (progress_) progress_->OnCurrentItem(it.path);
        if (it.isDir) return S_OK;
        DWORD err = 0;
        auto s = InFileStream::OpenRead(it.diskPath, &err);
        if (!s) {
            ++report_.skipped;
            report_.issues.push_back({it.diskPath, L"This file could not be read (it may be locked by another program).",
                                      L"CreateFile error " + std::to_wstring(err)});
            return S_FALSE;   // skip this file, continue with the rest
        }
        *in = s.get();
        s->AddRef();
        ++report_.filesDone;
        report_.bytesDone += it.size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(Int32* defined, BSTR* password) noexcept override {
        *defined = password_.empty() ? 0 : 1;
        *password = SysAllocString(password_.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }

private:
    const std::vector<UpdateItem>& items_;
    std::wstring password_;
    ProgressSink* progress_;
    CancelToken& cancel_;
    OperationReport& report_;
};

// ============================================================ helpers

ErrorInfo EngineError() {
    return ErrorInfo{ErrorCode::Internal, L"The archive engine (7z.dll) could not be loaded. Please reinstall the program.",
                     g_engine.error};
}

std::wstring LowerExt(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring e = path.substr(dot + 1);
    CharLowerBuffW(e.data(), static_cast<DWORD>(e.size()));
    return e;
}

std::wstring HrText(HRESULT hr) {
    wchar_t buf[32];
    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// Applies compression settings through ISetProperties.
HRESULT ApplySettings(IUnknown* handler, const std::wstring& format, const CompressionSettings& s, uint32_t codePage) {
    void* p = nullptr;
    if (handler->QueryInterface(IID_ISetProperties, &p) != S_OK || !p) return S_OK;
    ComPtr<ISetProperties> setProps(static_cast<ISetProperties*>(p));

    std::vector<const wchar_t*> names;
    std::vector<PROPVARIANT> values;
    auto addU4 = [&](const wchar_t* n, UInt32 v) { PROPVARIANT pv; PropVariantInit(&pv); pv.vt = VT_UI4; pv.ulVal = v; names.push_back(n); values.push_back(pv); };
    auto addBool = [&](const wchar_t* n, bool v) { PROPVARIANT pv; PropVariantInit(&pv); pv.vt = VT_BOOL; pv.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; names.push_back(n); values.push_back(pv); };
    auto addStr = [&](const wchar_t* n, const wchar_t* v) { PROPVARIANT pv; PropVariantInit(&pv); pv.vt = VT_BSTR; pv.bstrVal = SysAllocString(v); names.push_back(n); values.push_back(pv); };

    const bool isZip = CompareStringOrdinal(format.c_str(), -1, L"zip", -1, TRUE) == CSTR_EQUAL;
    addU4(L"x", static_cast<UInt32>(s.level));
    if (s.threads) addU4(L"mt", s.threads);
    if (isZip) {
        addBool(L"cu", true);                      // always store names as UTF-8 (Arabic-safe everywhere)
        if (!s.password.empty()) addStr(L"em", L"AES256");
    } else if (!s.password.empty() && s.encryptHeaders) {
        addBool(L"he", true);
    }
    (void)codePage;

    const HRESULT hr = setProps->SetProperties(names.data(), values.data(), static_cast<UInt32>(names.size()));
    for (auto& v : values) PropVariantClear(&v);
    return hr;
}

// F12: force the code page used to decode non-UTF-8 names (must be set before Open).
void ApplyCodePage(IUnknown* handler, uint32_t codePage) {
    void* p = nullptr;
    if (!codePage || handler->QueryInterface(IID_ISetProperties, &p) != S_OK || !p) return;
    ComPtr<ISetProperties> setProps(static_cast<ISetProperties*>(p));
    const wchar_t* names[] = {L"cp"};
    PROPVARIANT v; PropVariantInit(&v); v.vt = VT_UI4; v.ulVal = codePage;
    setProps->SetProperties(names, &v, 1);
}

// Writes the update to a temp file next to `finalPath`, then moves it in place.
Result<bool> RunUpdate(IOutArchive* out, const std::wstring& finalPath, const std::vector<UpdateItem>& items,
                       const CompressionSettings& settings, ProgressSink* progress, CancelToken& cancel,
                       OperationReport& report, const std::function<void()>& beforeReplace) {
    const std::wstring dir = finalPath.substr(0, finalPath.find_last_of(L'\\'));
    wchar_t tmpName[MAX_PATH + 1]{};
    std::wstring tmp;
    if (GetTempFileNameW(dir.c_str(), L"mzr", 0, tmpName)) tmp = tmpName;
    else tmp = finalPath + L".mzr.tmp";

    HANDLE h = CreateFileW(fs::ToLongPath(tmp).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return ErrorInfo{ErrorCode::Io, L"Could not create the new archive file (check that the folder is writable).",
                         L"CreateFile error " + std::to_wstring(GetLastError()) + L": " + tmp};

    ErrorInfo abortReason;
    auto* outStream = new OutFileStream(h, nullptr, &cancel, &abortReason);
    outStream->SetOwnsHandle(true);
    ComPtr<OutFileStream> outHolder(outStream);
    auto* cb = new UpdateCallback(items, settings.password, progress, cancel, report);
    ComPtr<UpdateCallback> cbHolder(cb);

    const HRESULT hr = out->UpdateItems(outStream, static_cast<UInt32>(items.size()), cb);
    outHolder.reset();                         // closes the temp file handle
    cbHolder.reset();

    if (hr != S_OK) {
        DeleteFileW(fs::ToLongPath(tmp).c_str());
        if (hr == E_ABORT && cancel.IsCancelled()) {
            report.cancelled = true;
            return ErrorInfo{ErrorCode::Cancelled, L"The operation was cancelled. The original archive was not changed.", L""};
        }
        if (abortReason.code != ErrorCode::None) return abortReason;
        return ErrorInfo{ErrorCode::Io, L"The archive could not be written. The original archive was not changed.",
                         L"UpdateItems returned " + HrText(hr)};
    }

    if (beforeReplace) beforeReplace();        // release the old archive's file handle
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    BOOL moved = FALSE;
    if (fs::Exists(finalPath))
        moved = ReplaceFileW(fs::ToLongPath(finalPath).c_str(), fs::ToLongPath(tmp).c_str(), nullptr,
                             REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr);
    if (!moved)
        moved = MoveFileExW(fs::ToLongPath(tmp).c_str(), fs::ToLongPath(finalPath).c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED);
    if (!moved) {
        const DWORD e = GetLastError();
        DeleteFileW(fs::ToLongPath(tmp).c_str());
        return ErrorInfo{ErrorCode::Io, L"The new archive could not replace the old one (is it open in another program?).",
                         L"ReplaceFile/MoveFileEx error " + std::to_wstring(e)};
    }
    return true;
}

// ZIP has no defined password encoding, so 7-Zip (like WinZip) only accepts
// printable ASCII there. 7z stores passwords as UTF-16 and accepts anything.
std::optional<ErrorInfo> CheckPassword(const std::wstring& format, const std::wstring& password) {
    if (password.empty() || CompareStringOrdinal(format.c_str(), -1, L"zip", -1, TRUE) != CSTR_EQUAL) return std::nullopt;
    for (wchar_t c : password)
        if (c < 0x20 || c > 0x7E)
            return ErrorInfo{ErrorCode::Internal,
                             L"ZIP passwords can only contain English letters, numbers and symbols. "
                             L"To use an Arabic (or other non-English) password, choose the 7z format instead.",
                             L"Non-ASCII character in ZIP password"};
    return std::nullopt;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring NormalizeArcPath(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    CharUpperBuffW(p.data(), static_cast<DWORD>(p.size()));
    return p;
}

} // namespace

// ============================================================ public API

std::wstring ArchiveBaseNameForStream(const std::wstring& fileName);   // defined at the end

bool LoadEngine(std::wstring* error) {
    std::call_once(g_engine.once, InitEngine);
    if (!g_engine.error.empty() && error) *error = g_engine.error;
    return g_engine.error.empty();
}

std::wstring EngineVersion() { return L"7-Zip 26.05"; }

std::vector<std::wstring> WritableFormats() { return {L"zip", L"7z"}; }

struct Archive::Impl {
    ComPtr<IInArchive> handler;
    ComPtr<IInStream> stream;
    std::wstring formatName;
    OpenOptions options;
    PasswordProvider passwords;
    std::wstring password;            // remembered for this session only (RAM), wiped on close
    ~Impl() {
        if (handler) handler->Close();
        WipeString(password);
    }
};

Archive::~Archive() = default;

Result<std::unique_ptr<Archive>> Archive::Open(const std::wstring& path, const OpenOptions& options,
                                               PasswordProvider passwords, CancelToken* cancel) {
    if (!LoadEngine()) return EngineError();

    DWORD err = 0;
    auto fileStream = InFileStream::OpenRead(path, &err);
    if (!fileStream)
        return ErrorInfo{ErrorCode::Io, L"The archive could not be opened. It may have been moved, deleted, or be in use.",
                         L"CreateFile error " + std::to_wstring(err) + L": " + path};
    return OpenFrom(static_cast<IInStream*>(fileStream.get()), path, options, std::move(passwords), cancel);
}

Result<std::unique_ptr<Archive>> Archive::OpenMemory(std::shared_ptr<const std::vector<uint8_t>> data,
                                                     const std::wstring& name, const OpenOptions& options) {
    if (!LoadEngine()) return EngineError();
    if (!data) return ErrorInfo{ErrorCode::Internal, L"Nothing to open.", L""};
    ComPtr<InMemStream> s(new InMemStream(std::move(data)));
    // The name must not contain a folder: OpenCallback then refuses any volume lookup.
    std::wstring n = name.substr(name.find_last_of(L"\\/") == std::wstring::npos ? 0 : name.find_last_of(L"\\/") + 1);
    return OpenFrom(static_cast<IInStream*>(s.get()), n, options, nullptr, nullptr);
}

Result<std::unique_ptr<Archive>> Archive::OpenFrom(void* rawStream, const std::wstring& path, const OpenOptions& options,
                                                   PasswordProvider passwords, CancelToken* cancel) {
    auto* streamPtr = static_cast<IInStream*>(rawStream);
    streamPtr->AddRef();
    ComPtr<IInStream> stream(streamPtr);

    // Candidate formats: extension match first, then the common-archive fallback list.
    std::vector<const FormatInfo*> candidates;
    const std::wstring ext = LowerExt(path);
    if (!options.onlyFormat.empty()) {
        if (auto* f = FindFormat(options.onlyFormat)) candidates.push_back(f);
    } else {
        for (const auto& f : g_engine.formats)
            if (!ext.empty() && std::find(f.extensions.begin(), f.extensions.end(), ext) != f.extensions.end())
                candidates.push_back(&f);
        for (auto* name : kFallbackFormats)
            if (auto* f = FindFormat(name); f && std::find(candidates.begin(), candidates.end(), f) == candidates.end())
                candidates.push_back(f);
    }

    const std::wstring fileName = path.substr(path.find_last_of(L'\\') + 1);
    bool passwordCancelled = false;
    bool sawEncryptedHeaders = false;

    for (const FormatInfo* fmt : candidates) {
        ComPtr<IInArchive> handler(CreateObj<IInArchive>(fmt->clsid, IID_IInArchive));
        if (!handler) continue;
        ApplyCodePage(handler.get(), options.codePage);
        UInt64 seekPos = 0;
        stream->Seek(0, FILE_BEGIN, &seekPos);
        auto* ocb = new OpenCallback(path, passwords, cancel);
        ComPtr<OpenCallback> ocbHolder(ocb);
        const UInt64 maxCheck = 1 << 22;      // allow SFX stubs up to 4 MB
        const HRESULT hr = handler->Open(stream.get(), &maxCheck, ocb);
        if (hr == E_ABORT && ocb->UserCancelled()) { passwordCancelled = true; break; }
        if (cancel && cancel->IsCancelled())
            return ErrorInfo{ErrorCode::Cancelled, L"Opening the archive was cancelled.", L""};
        if (hr != S_OK) {
            if (ocb->PasswordAsked()) sawEncryptedHeaders = true;
            continue;
        }

        // ---- success: read the entry list (metadata only, no data is decompressed)
        auto arc = std::unique_ptr<Archive>(new Archive());
        arc->impl_ = std::make_unique<Impl>();
        arc->path_ = path;
        auto& impl = *arc->impl_;
        impl.handler = handler;
        impl.stream = stream;
        impl.formatName = fmt->name;
        impl.options = options;
        impl.passwords = passwords;
        impl.password = ocb->TakePassword();

        UInt32 count = 0;
        handler->GetNumberOfItems(&count);
        if (count > options.maxEntries)
            return ErrorInfo{ErrorCode::TooManyEntries, L"This archive contains too many items to open safely.",
                             L"Items: " + std::to_wstring(count) + L"  Limit: " + std::to_wstring(options.maxEntries)};

        ArchiveInfo info;
        info.path = path;
        info.formatName = fmt->name;
        info.canUpdate = fmt->canUpdate;
        {
            Prop p;
            handler->GetArchiveProperty(kpidPhySize, &p);  info.physicalSize = PropU64(p.v);
            handler->GetArchiveProperty(kpidSolid, &p);    info.isSolid = PropBool(p.v);
            handler->GetArchiveProperty(kpidComment, &p);  info.comment = PropStr(p.v);
            handler->GetArchiveProperty(kpidErrorFlags, &p);
            const uint64_t ef = PropU64(p.v);
            if (ef & kpv_ErrorFlags_HeadersError)  info.warnings.push_back(L"Some archive headers are damaged.");
            if (ef & kpv_ErrorFlags_UnexpectedEnd) info.warnings.push_back(L"The archive ends unexpectedly (it may be incomplete).");
            if (ef & kpv_ErrorFlags_DataAfterEnd)  info.warnings.push_back(L"There is extra data after the end of the archive.");
            handler->GetArchiveProperty(kpidWarningFlags, &p);
            if (PropU64(p.v)) info.warnings.push_back(L"The archive has minor format problems.");
        }

        std::vector<EntryRecord> entries;
        entries.reserve(count);
        std::unordered_map<std::wstring, uint16_t> methodIds;
        for (UInt32 i = 0; i < count; ++i) {
            if (cancel && cancel->IsCancelled())
                return ErrorInfo{ErrorCode::Cancelled, L"Opening the archive was cancelled.", L""};
            EntryRecord e;
            e.arcIndex = i;
            Prop p;
            handler->GetProperty(i, kpidPath, &p);       e.path = PropStr(p.v);
            if (e.path.empty()) {
                // Single-stream formats (gz, xz, bz2) may have no stored name.
                e.path = count == 1 ? ArchiveBaseNameForStream(fileName) : L"item" + std::to_wstring(i);
            }
            handler->GetProperty(i, kpidIsDir, &p);      e.isDir = PropBool(p.v);
            handler->GetProperty(i, kpidSize, &p);       e.size = PropU64(p.v);
            handler->GetProperty(i, kpidPackSize, &p);   e.packSize = PropU64(p.v);
            handler->GetProperty(i, kpidAttrib, &p);     e.attrib = static_cast<uint32_t>(PropU64(p.v));
            handler->GetProperty(i, kpidEncrypted, &p);  e.encrypted = PropBool(p.v);
            handler->GetProperty(i, kpidMethod, &p);
            if (std::wstring m = PropStr(p.v); !m.empty()) {
                auto it = methodIds.find(m);
                if (it == methodIds.end() && info.methods.size() < 0xFFFF) {
                    it = methodIds.emplace(m, static_cast<uint16_t>(info.methods.size())).first;
                    info.methods.push_back(std::move(m));
                }
                if (it != methodIds.end()) e.method = it->second;
            }
            handler->GetProperty(i, kpidIsAltStream, &p); e.isAltStream = PropBool(p.v);
            handler->GetProperty(i, kpidCRC, &p);
            if (p.v.vt == VT_UI4) { e.hasCrc = true; e.crc = p.v.ulVal; }
            handler->GetProperty(i, kpidMTime, &p);
            if (p.v.vt == VT_FILETIME) e.mtime = p.v.filetime;
            handler->GetProperty(i, kpidSymLink, &p);    const bool sym = !PropStr(p.v).empty();
            handler->GetProperty(i, kpidHardLink, &p);   const bool hard = !PropStr(p.v).empty();
            // Unix symlink stored in ZIP/TAR: high 16 bits = st_mode when bit 0x8000 is set.
            const bool unixLink = (e.attrib & 0x8000) && (((e.attrib >> 16) & 0xF000) == 0xA000);
            e.isLink = sym || hard || unixLink || (e.attrib & FILE_ATTRIBUTE_REPARSE_POINT);
            if (e.encrypted) info.hasEncrypted = true;
            entries.push_back(std::move(e));
        }
        arc->index_ = ArchiveIndex::Build(std::move(info), std::move(entries));
        return std::move(arc);
    }

    if (passwordCancelled)
        return ErrorInfo{ErrorCode::Cancelled, L"A password is required to open this archive.", L"Password prompt cancelled"};
    if (sawEncryptedHeaders)
        return ErrorInfo{ErrorCode::WrongPassword, L"The password is wrong, or the archive is damaged.", L"Encrypted headers could not be opened"};
    return ErrorInfo{ErrorCode::Corrupted,
                     L"This file is not a supported archive, or the archive is damaged.",
                     L"No handler accepted the file. Extension: ." + ext};
}

OperationReport Archive::Extract(const std::vector<uint32_t>& arcIndices, const ExtractOptions& options,
                                 ProgressSink* progress, CancelToken& cancel, const security::BombLimits& limits) {
    OperationReport report;
    if (!impl_ || !impl_->handler) { report.issues.push_back({L"", L"The archive is not open.", L""}); return report; }

    std::wstring err;
    if (!fs::CreateDirectories(options.destination, &err)) {
        report.issues.push_back({options.destination, L"The destination folder could not be created.", err});
        return report;
    }

    // Archive size: ratio test for entries without their own packed size (solid blocks).
    uint64_t arcBytes = index_->info().physicalSize;
    if (!arcBytes) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(fs::ToLongPath(path_).c_str(), GetFileExInfoStandard, &fa))
            arcBytes = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }
    security::ExtractGuard guard(limits, fs::FreeSpace(options.destination), arcBytes);
    const std::string zone = options.propagateMotW ? fs::ReadZoneIdentifier(path_) : std::string();
    const std::wstring name = path_.substr(path_.find_last_of(L'\\') + 1);

    auto* cb = new ExtractCallback(index_->entries(), &options, false, progress, cancel, &guard,
                                   impl_->password, impl_->passwords, name, zone, report);
    ComPtr<ExtractCallback> holder(cb);

    std::vector<UInt32> idx(arcIndices.begin(), arcIndices.end());
    std::sort(idx.begin(), idx.end());
    const HRESULT hr = arcIndices.empty() ? impl_->handler->Extract(nullptr, static_cast<UInt32>(-1), 0, cb)
                                          : impl_->handler->Extract(idx.data(), static_cast<UInt32>(idx.size()), 0, cb);
    if (hr == E_ABORT) {
        if (cb->abortReason_.code != ErrorCode::None)
            report.issues.push_back({L"", cb->abortReason_.friendly, cb->abortReason_.details});
        else if (cancel.IsCancelled())
            report.cancelled = true;
    } else if (hr != S_OK) {
        report.issues.push_back({L"", L"Extraction stopped because the archive appears to be corrupted.",
                                 L"Extract returned " + HrText(hr)});
    }
    return report;
}

OperationReport Archive::Test(ProgressSink* progress, CancelToken& cancel) {
    OperationReport report;
    if (!impl_ || !impl_->handler) return report;
    const std::wstring name = path_.substr(path_.find_last_of(L'\\') + 1);
    auto* cb = new ExtractCallback(index_->entries(), nullptr, true, progress, cancel, nullptr,
                                   impl_->password, impl_->passwords, name, {}, report);
    ComPtr<ExtractCallback> holder(cb);
    const HRESULT hr = impl_->handler->Extract(nullptr, static_cast<UInt32>(-1), 1, cb);
    if (hr == E_ABORT && cancel.IsCancelled()) report.cancelled = true;
    else if (hr != S_OK && hr != E_ABORT)
        report.issues.push_back({L"", L"Testing stopped because the archive appears to be corrupted.", L"Extract(test) returned " + HrText(hr)});
    return report;
}

// ---- ReadEntry: one file -> RAM (Preview)

namespace {

class MemOutStream final : public ISequentialOutStream {
    MZR_IUNKNOWN_REFCOUNT
public:
    MemOutStream(std::vector<uint8_t>& buf, uint64_t max, CancelToken& cancel, bool& tooLarge)
        : buf_(buf), max_(max), cancel_(cancel), tooLarge_(tooLarge) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) *out = static_cast<ISequentialOutStream*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* processed) noexcept override {
        if (processed) *processed = 0;
        if (cancel_.IsCancelled()) return E_ABORT;
        // Never trust the declared size: stop at max_. The part of the chunk that
        // still fits is kept (a truncated text preview needs it).
        const uint64_t room = max_ - std::min<uint64_t>(max_, buf_.size());
        const size_t take = static_cast<size_t>(std::min<uint64_t>(room, size));
        try {
            const auto* p = static_cast<const uint8_t*>(data);
            buf_.insert(buf_.end(), p, p + take);
        } catch (...) { return E_OUTOFMEMORY; }
        if (take < size) { tooLarge_ = true; return E_ABORT; }
        if (processed) *processed = size;
        return S_OK;
    }
private:
    std::vector<uint8_t>& buf_;
    uint64_t max_;
    CancelToken& cancel_;
    bool& tooLarge_;
};

class MemExtractCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
    MZR_IUNKNOWN_REFCOUNT
public:
    MemExtractCallback(UInt32 index, std::vector<uint8_t>& buf, uint64_t max, CancelToken& cancel, std::wstring password)
        : index_(index), buf_(buf), max_(max), cancel_(cancel), password_(std::move(password)) {}
    ~MemExtractCallback() { WipeString(password_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback || iid == IID_IProgress)
            *out = static_cast<IArchiveExtractCallback*>(this);
        else if (iid == IID_ICryptoGetTextPassword) *out = static_cast<ICryptoGetTextPassword*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) noexcept override { return cancel_.IsCancelled() ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) noexcept override {
        needPassword_ = true;
        if (password_.empty()) return E_ABORT;           // Preview never prompts
        *password = SysAllocString(password_.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream** out, Int32 askMode) noexcept override {
        *out = nullptr;
        if (index != index_ || askMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
        *out = new MemOutStream(buf_, max_, cancel_, tooLarge_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 r) noexcept override { opResult_ = r; return S_OK; }

    bool tooLarge_ = false;
    bool needPassword_ = false;
    Int32 opResult_ = NArchive::NExtract::NOperationResult::kOK;
private:
    UInt32 index_;
    std::vector<uint8_t>& buf_;
    uint64_t max_;
    CancelToken& cancel_;
    std::wstring password_;
};

} // namespace

Result<std::vector<uint8_t>> Archive::ReadEntry(uint32_t arcIndex, uint64_t maxBytes, CancelToken& cancel,
                                                bool* truncated) {
    if (truncated) *truncated = false;
    if (!index_) return ErrorInfo{ErrorCode::Internal, L"The archive is not open.", L"ReadEntry"};
    const auto& entries = index_->entries();
    const EntryRecord* found = nullptr;
    if (arcIndex < entries.size() && entries[arcIndex].arcIndex == arcIndex) found = &entries[arcIndex];
    else for (const auto& x : entries) if (x.arcIndex == arcIndex) { found = &x; break; }
    if (!impl_ || !impl_->handler || !found)
        return ErrorInfo{ErrorCode::Internal, L"The archive is not open.", L"ReadEntry"};
    const EntryRecord& e = *found;
    if (e.isDir) return ErrorInfo{ErrorCode::Internal, L"This is a folder.", L""};
    if (e.size > maxBytes && !truncated)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This file is too large to preview.", L"Size " + std::to_wstring(e.size)};

    std::vector<uint8_t> buf;
    try { buf.reserve(static_cast<size_t>(std::min<uint64_t>(e.size, maxBytes))); } catch (...) {}
    auto* cb = new MemExtractCallback(arcIndex, buf, maxBytes, cancel, impl_->password);
    ComPtr<MemExtractCallback> holder(cb);
    const UInt32 idx = arcIndex;
    const HRESULT hr = impl_->handler->Extract(&idx, 1, 0, cb);
    if (cancel.IsCancelled()) return ErrorInfo{ErrorCode::Cancelled, L"Cancelled.", L""};
    if (cb->tooLarge_ && truncated) {
        // MemOutStream refused the chunk that crossed the limit; keep what fits.
        *truncated = true;
        return buf;
    }
    if (cb->tooLarge_)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This file is too large to preview.", L"More than " + std::to_wstring(maxBytes) + L" bytes"};
    if (cb->needPassword_ && impl_->password.empty())
        return ErrorInfo{ErrorCode::WrongPassword, L"This file is password protected. Extract it to see it.", L""};
    if (hr != S_OK || cb->opResult_ != NArchive::NExtract::NOperationResult::kOK)
        return ErrorInfo{ErrorCode::Corrupted, FriendlyOpResult(cb->opResult_, e.encrypted),
                         L"ReadEntry hr " + HrText(hr) + L" op " + std::to_wstring(cb->opResult_)};
    return buf;
}

Result<bool> Archive::Update(const UpdatePlan& plan, const CompressionSettings& settings,
                             ProgressSink* progress, CancelToken& cancel, OperationReport* reportOut) {
    OperationReport local;
    OperationReport& report = reportOut ? *reportOut : local;
    if (!impl_ || !impl_->handler) return ErrorInfo{ErrorCode::Internal, L"The archive is not open.", L""};

    void* p = nullptr;
    if (impl_->handler->QueryInterface(IID_IOutArchive, &p) != S_OK || !p)
        return ErrorInfo{ErrorCode::Internal, L"This archive format cannot be modified (it is read-only). Extract it and create a new ZIP or 7z instead.",
                         L"Format: " + impl_->formatName};
    ComPtr<IOutArchive> out(static_cast<IOutArchive*>(p));
    if (!index_->info().canUpdate)
        return ErrorInfo{ErrorCode::Internal, L"This archive format cannot be modified.", L"Format: " + impl_->formatName};

    // Which existing entries survive?
    std::unordered_set<uint32_t> removed(plan.remove.begin(), plan.remove.end());
    std::unordered_map<uint32_t, std::wstring> renamed(plan.rename.begin(), plan.rename.end());
    std::unordered_set<std::wstring> replacedPaths;
    for (const auto& a : plan.add) replacedPaths.insert(NormalizeArcPath(a.archivePath));

    std::vector<UpdateItem> items;
    const auto& entries = index_->entries();
    items.reserve(entries.size() + plan.add.size());
    for (const auto& e : entries) {
        if (removed.count(e.arcIndex)) continue;
        if (!e.isDir && replacedPaths.count(NormalizeArcPath(e.path))) continue;   // replaced by a newer file
        UpdateItem it;
        it.arcIndex = e.arcIndex;
        it.path = e.path;
        it.isDir = e.isDir;
        it.size = e.size;
        it.attrib = e.attrib;
        it.mtime = e.mtime;
        if (auto r = renamed.find(e.arcIndex); r != renamed.end()) { it.newProps = true; it.path = r->second; }
        items.push_back(std::move(it));
    }
    for (const auto& s : plan.add) {
        if (SamePath(s.diskPath, path_)) { ++report.skipped; continue; }   // never add the archive into itself
        if (s.isDir && replacedPaths.count(NormalizeArcPath(s.archivePath))) {
            // Skip a new folder entry if the archive already has that folder.
            bool exists = false;
            for (const auto& e : entries) if (e.isDir && NormalizeArcPath(e.path) == NormalizeArcPath(s.archivePath)) { exists = true; break; }
            if (exists) continue;
        }
        UpdateItem it;
        it.newData = it.newProps = true;
        it.path = s.archivePath;
        it.isDir = s.isDir;
        it.size = s.size;
        it.attrib = s.attrib;
        it.mtime = s.mtime;
        it.diskPath = s.diskPath;
        items.push_back(std::move(it));
    }

    CompressionSettings eff = settings;
    if (eff.password.empty()) eff.password = impl_->password;   // keep existing encryption for new files
    if (auto bad = CheckPassword(impl_->formatName, eff.password)) { WipeString(eff.password); return *bad; }
    if (FAILED(ApplySettings(out.get(), impl_->formatName, eff, 0)))
        return ErrorInfo{ErrorCode::Internal, L"These compression settings are not supported by this format.", L"SetProperties failed"};

    auto res = RunUpdate(out.get(), path_, items, eff, progress, cancel, report, [&] {
        out.reset();
        impl_->handler->Close();
        impl_->handler.reset();
        impl_->stream.reset();
    });
    WipeString(eff.password);

    // Re-open (the new file on success, the untouched original on failure).
    CancelToken none;
    auto reopened = Archive::Open(path_, impl_->options, impl_->passwords, &none);
    if (reopened) {
        std::wstring keep = impl_->password;
        impl_ = std::move(reopened.value()->impl_);
        if (impl_->password.empty()) impl_->password = keep;
        WipeString(keep);
        index_ = reopened.value()->index_;
    }
    if (!res) return res;
    if (!reopened) return reopened.error();
    return true;
}

Result<bool> CreateArchive(const std::wstring& outPath, const std::vector<SourceItem>& sources,
                           const CompressionSettings& settings, ProgressSink* progress,
                           CancelToken& cancel, OperationReport* reportOut) {
    OperationReport local;
    OperationReport& report = reportOut ? *reportOut : local;
    if (!LoadEngine()) return EngineError();
    const FormatInfo* fmt = FindFormat(settings.format);
    if (!fmt || !fmt->canUpdate)
        return ErrorInfo{ErrorCode::Internal, L"This archive type cannot be created.", L"Format: " + settings.format};
    if (auto bad = CheckPassword(fmt->name, settings.password)) return *bad;

    ComPtr<IOutArchive> out(CreateObj<IOutArchive>(fmt->clsid, IID_IOutArchive));
    if (!out) return ErrorInfo{ErrorCode::Internal, L"The archive engine failed to start.", L"CreateObject(IOutArchive) failed"};
    if (FAILED(ApplySettings(out.get(), fmt->name, settings, 0)))
        return ErrorInfo{ErrorCode::Internal, L"These compression settings are not supported by this format.", L"SetProperties failed"};

    std::vector<UpdateItem> items;
    items.reserve(sources.size());
    for (const auto& s : sources) {
        if (SamePath(s.diskPath, outPath)) { ++report.skipped; continue; }
        UpdateItem it;
        it.newData = it.newProps = true;
        it.path = s.archivePath;
        it.isDir = s.isDir;
        it.size = s.size;
        it.attrib = s.attrib;
        it.mtime = s.mtime;
        it.diskPath = s.diskPath;
        items.push_back(std::move(it));
    }
    return RunUpdate(out.get(), outPath, items, settings, progress, cancel, report, nullptr);
}

// "data.csv.gz" -> "data.csv",  "backup.tgz" -> "backup.tar"
std::wstring ArchiveBaseNameForStream(const std::wstring& fileName) {
    const size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return fileName + L"~";
    std::wstring ext = fileName.substr(dot + 1);
    CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
    const std::wstring stem = fileName.substr(0, dot);
    if (ext == L"tgz" || ext == L"tbz" || ext == L"tbz2" || ext == L"txz" || ext == L"tzst") return stem + L".tar";
    return stem;
}

} // namespace mzr::archive
