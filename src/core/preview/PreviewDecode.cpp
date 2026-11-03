#include "core/preview/PreviewDecode.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

namespace mzr::preview {
namespace {

using Microsoft::WRL::ComPtr;

bool HasExt(const std::wstring& name, std::initializer_list<const wchar_t*> exts) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const wchar_t* e = name.c_str() + dot + 1;
    for (auto* x : exts)
        if (CompareStringOrdinal(e, -1, x, -1, TRUE) == CSTR_EQUAL) return true;
    return false;
}

ErrorInfo NoImage(const std::wstring& details) {
    return ErrorInfo{ErrorCode::Corrupted, L"This image could not be shown (unsupported or damaged).", details};
}

std::wstring HrHex(HRESULT hr) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
    return b;
}

// EXIF orientation (tag 274) -> WIC transform. 1 / missing = no change.
WICBitmapTransformOptions OrientationOf(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return WICBitmapTransformRotate0;
    PROPVARIANT v;
    PropVariantInit(&v);
    WICBitmapTransformOptions t = WICBitmapTransformRotate0;
    for (const wchar_t* path : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        if (SUCCEEDED(q->GetMetadataByName(path, &v))) {
            const unsigned o = v.vt == VT_UI2 ? v.uiVal : 1;
            switch (o) {
            case 2: t = WICBitmapTransformFlipHorizontal; break;
            case 3: t = WICBitmapTransformRotate180; break;
            case 4: t = WICBitmapTransformFlipVertical; break;
            case 5: t = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
            case 6: t = WICBitmapTransformRotate90; break;
            case 7: t = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
            case 8: t = WICBitmapTransformRotate270; break;
            default: break;
            }
            PropVariantClear(&v);
            break;
        }
    }
    return t;
}

} // namespace

Kind KindForName(const std::wstring& name) {
    if (HasExt(name, {L"png", L"jpg", L"jpeg", L"jpe", L"jfif", L"bmp", L"dib", L"gif", L"tif", L"tiff", L"ico",
                      L"webp", L"jxr", L"wdp", L"heic", L"heif", L"avif", L"dds"}))
        return Kind::Image;
    if (HasExt(name, {L"pdf"})) return Kind::Pdf;
    if (HasExt(name, {L"dwg"})) return Kind::Dwg;
    if (HasExt(name, {L"dxf"})) return Kind::Dxf;
    if (HasExt(name, {L"rvt", L"rfa", L"rte", L"rft"})) return Kind::Revit;
    if (HasExt(name, {L"ifc"})) return Kind::Ifc;
    if (HasExt(name, {L"nwd", L"nwc", L"nwf"})) return Kind::Navisworks;
    // Office Open XML (macro-enabled variants too: macros are never read or run).
    if (HasExt(name, {L"docx", L"docm", L"dotx", L"dotm"})) return Kind::Docx;
    if (HasExt(name, {L"xlsx", L"xlsm", L"xltx", L"xltm"})) return Kind::Xlsx;
    if (HasExt(name, {L"pptx", L"pptm", L"potx", L"potm", L"ppsx", L"ppsm"})) return Kind::Pptx;
    // Formats Windows' Media Foundation can play without extra codecs.
    if (HasExt(name, {L"mp4", L"m4v", L"mov", L"wmv", L"avi", L"mkv", L"webm", L"3gp", L"3g2", L"mts", L"m2ts", L"asf"}))
        return Kind::Video;
    if (HasExt(name, {L"mp3", L"wav", L"wma", L"m4a", L"aac", L"flac", L"ogg", L"opus", L"amr"}))
        return Kind::Audio;
    if (HasExt(name, {L"txt", L"md", L"markdown", L"csv", L"tsv", L"log", L"json", L"xml", L"ini", L"cfg", L"conf",
                      L"yaml", L"yml", L"toml", L"html", L"htm", L"svg", L"css", L"js", L"ts", L"c", L"cpp", L"cc",
                      L"h", L"hpp", L"cs", L"java", L"py", L"rb", L"go", L"rs", L"php", L"sql", L"sh", L"ps1",
                      L"bat", L"cmd", L"reg", L"nfo", L"diz", L"srt", L"vtt", L"lsp", L"pat", L"lin",
                      L"rtf", L"tex", L"gitignore", L"editorconfig", L"properties", L"gradle", L"cmake", L"mk"}))
        return Kind::Text;
    // "README", "LICENSE", "Makefile" ... (no extension) are almost always text.
    const auto slash = name.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? name : name.substr(slash + 1);
    if (base.find(L'.') == std::wstring::npos) {
        for (auto* n : {L"readme", L"license", L"licence", L"makefile", L"changelog", L"authors", L"copying", L"notice"})
            if (CompareStringOrdinal(base.c_str(), -1, n, -1, TRUE) == CSTR_EQUAL) return Kind::Text;
    }
    return Kind::None;
}

Result<Image> DecodeImage(const std::vector<uint8_t>& data, uint32_t maxSide) {
    if (data.empty() || data.size() > 0xFFFFFFFFull) return NoImage(L"empty or too large buffer");
    ComPtr<IWICImagingFactory> f;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) return NoImage(L"WIC factory " + HrHex(hr));

    ComPtr<IWICStream> s;
    if (FAILED(hr = f->CreateStream(&s)) ||
        FAILED(hr = s->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()))))
        return NoImage(L"stream " + HrHex(hr));

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(hr = f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
        return NoImage(L"no decoder " + HrHex(hr));
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = dec->GetFrame(0, &frame))) return NoImage(L"frame " + HrHex(hr));

    UINT w = 0, h = 0;
    if (FAILED(hr = frame->GetSize(&w, &h)) || !w || !h) return NoImage(L"size " + HrHex(hr));
    if (w > kMaxImageSide || h > kMaxImageSide)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This image is too large to preview.",
                         std::to_wstring(w) + L" x " + std::to_wstring(h)};

    // Reduce big photos while decoding so RAM stays small.
    ComPtr<IWICBitmapSource> src = frame;
    UINT nw = w, nh = h;
    if (std::max(w, h) > maxSide) {
        const double k = static_cast<double>(maxSide) / std::max(w, h);
        nw = std::max<UINT>(1, static_cast<UINT>(w * k + 0.5));
        nh = std::max<UINT>(1, static_cast<UINT>(h * k + 0.5));
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(hr = f->CreateBitmapScaler(&sc)) ||
            FAILED(hr = sc->Initialize(src.Get(), nw, nh, WICBitmapInterpolationModeFant)))
            return NoImage(L"scaler " + HrHex(hr));
        src = sc;
    }

    const WICBitmapTransformOptions t = OrientationOf(frame.Get());
    if (t != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> fr;
        if (SUCCEEDED(f->CreateBitmapFlipRotator(&fr)) && SUCCEEDED(fr->Initialize(src.Get(), t))) {
            src = fr;
            if (t & (WICBitmapTransformRotate90 | WICBitmapTransformRotate270)) std::swap(nw, nh);
        }
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(hr = f->CreateFormatConverter(&conv)) ||
        FAILED(hr = conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return NoImage(L"convert " + HrHex(hr));

    Image img;
    img.width = nw;
    img.height = nh;
    img.originalWidth = (t & (WICBitmapTransformRotate90 | WICBitmapTransformRotate270)) ? h : w;
    img.originalHeight = (t & (WICBitmapTransformRotate90 | WICBitmapTransformRotate270)) ? w : h;
    try {
        img.pixels.resize(static_cast<size_t>(nw) * nh * 4);
    } catch (...) {
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"Not enough memory to preview this image.", L""};
    }
    if (FAILED(hr = conv->CopyPixels(nullptr, nw * 4, static_cast<UINT>(img.pixels.size()), img.pixels.data())))
        return NoImage(L"pixels " + HrHex(hr));
    return img;
}

Result<std::wstring> DecodeText(const std::vector<uint8_t>& data) {
    std::wstring out;
    const uint8_t* p = data.data();
    size_t n = data.size();
    auto notText = [] {
        return ErrorInfo{ErrorCode::Corrupted, L"This file does not contain readable text.", L"binary data"};
    };

    if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
        // UTF-16 with BOM.
        const bool be = p[0] == 0xFE;
        p += 2; n -= 2;
        out.resize(n / 2);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = be ? static_cast<wchar_t>((p[2 * i] << 8) | p[2 * i + 1])
                        : static_cast<wchar_t>(p[2 * i] | (p[2 * i + 1] << 8));
    } else {
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
        // NUL bytes in the first 8 KB = binary file.
        const size_t probe = std::min<size_t>(n, 8192);
        for (size_t i = 0; i < probe; ++i) if (p[i] == 0) return notText();
        if (n > 0x7FFFFFFF) n = 0x7FFFFFFF;
        const int len = static_cast<int>(n);
        // A cut-off UTF-8 sequence at the end (truncated preview) must not
        // make the whole file fall back to ANSI: drop up to 3 trailing bytes.
        int strictLen = len;
        int wlen = 0;
        for (int drop = 0; drop <= 3 && strictLen > 0; ++drop, --strictLen) {
            wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(p), strictLen, nullptr, 0);
            if (wlen > 0) break;
        }
        if (wlen > 0) {
            out.resize(static_cast<size_t>(wlen));
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), strictLen, out.data(), wlen);
        } else if (len > 0) {
            wlen = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(p), len, nullptr, 0);
            out.resize(static_cast<size_t>(wlen));
            MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(p), len, out.data(), wlen);
        }
    }

    // CRLF for the edit control; drop other control characters except tab.
    std::wstring res;
    res.reserve(out.size() + out.size() / 32);
    for (size_t i = 0; i < out.size(); ++i) {
        const wchar_t c = out[i];
        if (c == L'\r') { res += L"\r\n"; if (i + 1 < out.size() && out[i + 1] == L'\n') ++i; }
        else if (c == L'\n') res += L"\r\n";
        else if (c == 0) return notText();
        else if (c < 0x20 && c != L'\t') res += L' ';
        else res += c;
    }
    return res;
}

} // namespace mzr::preview
