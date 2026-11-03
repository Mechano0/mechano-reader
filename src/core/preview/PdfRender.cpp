#include "core/preview/PdfRender.h"

#include <windows.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>

namespace mzr::preview {

using namespace winrt;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage::Streams;

Result<PdfPage> RenderPdfPage(const std::vector<uint8_t>& pdf, uint32_t pageIndex, uint32_t targetWidth) {
    if (pdf.empty() || pdf.size() > kMaxPdfBytes)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This PDF is too large to preview.", L""};
    try {
        // PDF bytes -> in-memory stream (never written to disk).
        InMemoryRandomAccessStream in;
        {
            DataWriter w(in);
            w.WriteBytes(array_view<const uint8_t>(pdf.data(), pdf.data() + pdf.size()));
            w.StoreAsync().get();
            w.FlushAsync().get();
            w.DetachStream();
        }
        in.Seek(0);

        PdfDocument doc{nullptr};
        try {
            doc = PdfDocument::LoadFromStreamAsync(in).get();
        } catch (const hresult_error& e) {
            // 0x8007052B = wrong/missing password.
            if (static_cast<uint32_t>(e.code()) == 0x8007052Bu)
                return ErrorInfo{ErrorCode::WrongPassword, L"This PDF is password protected. Extract it to open it.", L""};
            return ErrorInfo{ErrorCode::Corrupted, L"This PDF could not be read (it may be damaged).",
                             std::wstring(e.message().c_str())};
        }
        if (doc.IsPasswordProtected())
            return ErrorInfo{ErrorCode::WrongPassword, L"This PDF is password protected. Extract it to open it.", L""};

        PdfPage out;
        out.pageCount = doc.PageCount();
        if (!out.pageCount) return ErrorInfo{ErrorCode::Corrupted, L"This PDF has no pages.", L""};
        out.pageIndex = std::min(pageIndex, out.pageCount - 1);

        auto page = doc.GetPage(out.pageIndex);
        const auto sz = page.Size();     // in DIPs
        PdfPageRenderOptions opt;
        uint32_t w = std::clamp<uint32_t>(targetWidth, 200, kDecodeSide);
        // Very tall or very wide pages: keep the longer side within kDecodeSide.
        if (sz.Width > 0 && sz.Height > 0) {
            const double h = w * static_cast<double>(sz.Height) / sz.Width;
            if (h > kDecodeSide) w = std::max<uint32_t>(1, static_cast<uint32_t>(w * kDecodeSide / h));
        }
        opt.DestinationWidth(w);

        InMemoryRandomAccessStream png;
        page.RenderToStreamAsync(png, opt).get();      // PNG, white background
        const auto size = static_cast<uint32_t>(png.Size());
        std::vector<uint8_t> bytes(size);
        {
            DataReader r(png.GetInputStreamAt(0));
            r.LoadAsync(size).get();
            r.ReadBytes(array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        }
        auto img = DecodeImage(bytes);
        if (!img) return img.error();
        out.image = std::move(img.value());
        return out;
    } catch (const hresult_error& e) {
        return ErrorInfo{ErrorCode::Corrupted, L"This PDF could not be shown.", std::wstring(e.message().c_str())};
    } catch (...) {
        return ErrorInfo{ErrorCode::Corrupted, L"This PDF could not be shown.", L"unknown exception"};
    }
}

} // namespace mzr::preview
