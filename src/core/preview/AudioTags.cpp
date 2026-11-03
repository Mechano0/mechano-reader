#include "core/preview/AudioTags.h"

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cwctype>
#include <vector>

#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace mzr::preview {
namespace {

constexpr size_t kMaxCoverBytes = 16u << 20;

std::wstring Text(IPropertyStore* ps, REFPROPERTYKEY key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) {
            out = v.pwszVal;
        } else if (v.vt == (VT_VECTOR | VT_LPWSTR)) {               // several artists / genres
            for (ULONG i = 0; i < v.calpwstr.cElems && i < 8; ++i) {
                if (!v.calpwstr.pElems[i]) continue;
                if (!out.empty()) out += L"; ";
                out += v.calpwstr.pElems[i];
            }
        }
    }
    PropVariantClear(&v);
    if (out.size() > 300) out.resize(300);
    for (auto& c : out) if (c < 0x20) c = L' ';
    return out;
}

uint32_t Number(IPropertyStore* ps, REFPROPERTYKEY key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    uint32_t n = 0;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        if (v.vt == VT_UI4) n = v.ulVal;
        else if (v.vt == VT_UI2) n = v.uiVal;
        else if (v.vt == VT_I4 && v.lVal > 0) n = static_cast<uint32_t>(v.lVal);
    }
    PropVariantClear(&v);
    return n;
}

// The embedded front cover: Windows exposes it as a stream.
void Cover(IPropertyStore* ps, AudioTags& t) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_ThumbnailStream, &v)) && v.vt == VT_STREAM && v.pStream) {
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        for (;;) {
            ULONG got = 0;
            const HRESULT hr = v.pStream->Read(buf, sizeof buf, &got);
            if (got) data.insert(data.end(), buf, buf + got);
            if (FAILED(hr) || got == 0 || data.size() > kMaxCoverBytes) break;
        }
        if (!data.empty() && data.size() <= kMaxCoverBytes) {
            auto img = DecodeImage(data, 512);
            if (img) t.cover = std::move(img.value());
        }
    }
    PropVariantClear(&v);
}

} // namespace

AudioTags ReadAudioTags(const std::wstring& path) {
    AudioTags t;
    ComPtr<IPropertyStore> ps;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) || !ps) return t;
    t.title = Text(ps.Get(), PKEY_Title);
    t.artist = Text(ps.Get(), PKEY_Music_Artist);
    if (t.artist.empty()) t.artist = Text(ps.Get(), PKEY_Music_AlbumArtist);
    t.album = Text(ps.Get(), PKEY_Music_AlbumTitle);
    t.genre = Text(ps.Get(), PKEY_Music_Genre);
    const uint32_t year = Number(ps.Get(), PKEY_Media_Year);
    if (year > 0 && year < 10000) t.year = std::to_wstring(year);
    t.sampleRate = Number(ps.Get(), PKEY_Audio_SampleRate);
    t.bitRate = Number(ps.Get(), PKEY_Audio_EncodingBitrate);
    t.channels = Number(ps.Get(), PKEY_Audio_ChannelCount);
    Cover(ps.Get(), t);
    return t;
}

std::wstring AudioFormatLine(const AudioTags& t, const std::wstring& fileName) {
    const auto dot = fileName.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : fileName.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    std::wstring parts;
    auto add = [&](const std::wstring& s) { if (!parts.empty()) parts += L", "; parts += s; };
    if (t.sampleRate) {
        wchar_t b[32];
        if (t.sampleRate % 1000 == 0) swprintf_s(b, L"%u kHz", t.sampleRate / 1000);
        else swprintf_s(b, L"%.1f kHz", t.sampleRate / 1000.0);
        add(b);
    }
    if (t.bitRate) add(std::to_wstring((t.bitRate + 500) / 1000) + L" kbps");
    if (t.channels == 1) add(L"mono");
    else if (t.channels == 2) add(L"stereo");
    else if (t.channels > 2) add(std::to_wstring(t.channels) + L" channels");
    if (parts.empty()) return ext;
    return ext.empty() ? parts : ext + L"  ::  " + parts;
}

} // namespace mzr::preview
