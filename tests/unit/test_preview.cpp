// Preview: read one entry into RAM, decode text and images, refuse hostile input.
#include <objbase.h>
#include "../TestFiles.h"
#include "../mzr_test.h"
#include "core/archive/ArchiveEngine.h"
#include "core/filesystem/FileSystem.h"
#include "core/preview/AudioTags.h"
#include "core/preview/PdfRender.h"
#include "core/preview/PreviewDecode.h"

using namespace mzr;
using namespace mzr::archive;
using namespace mzr::preview;
using mzr_test::TempDir;
using mzr_test::WriteFileBytes;

namespace {

std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Hand-made 24-bit BMP, w x h, every pixel the same colour.
std::vector<uint8_t> MakeBmp(int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b, bool pixelData = true) {
    const uint32_t row = static_cast<uint32_t>((w * 3 + 3) & ~3);
    const uint32_t img = pixelData ? row * static_cast<uint32_t>(h) : 0;
    std::vector<uint8_t> v(54 + img, 0);
    auto put32 = [&](size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i)); };
    v[0] = 'B'; v[1] = 'M';
    put32(2, 54 + row * static_cast<uint32_t>(h));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    v[26] = 1; v[28] = 24;
    put32(34, row * static_cast<uint32_t>(h));
    for (int32_t y = 0; pixelData && y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            const size_t p = 54 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3;
            v[p] = b; v[p + 1] = g; v[p + 2] = r;
        }
    return v;
}

// Minimal valid PDF with `pages` pages, each with a red box and a line of text.
std::vector<uint8_t> MakePdf(int pages) {
    std::vector<std::string> objs;
    std::string kids;
    for (int i = 0; i < pages; ++i) kids += std::to_string(3 + 2 * i) + " 0 R ";
    objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objs.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >>");
    const int font = 3 + 2 * pages;
    for (int i = 0; i < pages; ++i) {
        const std::string content = "1 0 0 rg 72 600 200 100 re f BT /F1 24 Tf 72 500 Td (Page " + std::to_string(i + 1) + ") Tj ET";
        objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 " +
                       std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(4 + 2 * i) + " 0 R >>");
        objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");
    }
    objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> off;
    for (size_t i = 0; i < objs.size(); ++i) {
        off.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : off) {
        char b[32];
        snprintf(b, sizeof(b), "%010zu 00000 n \n", o);
        pdf += b;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return {pdf.begin(), pdf.end()};
}

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

} // namespace

TEST_CASE("preview: kind by file name") {
    CHECK(KindForName(L"logo.PNG") == Kind::Image);
    CHECK(KindForName(L"photo.jpeg") == Kind::Image);
    CHECK(KindForName(L"ملاحظات.txt") == Kind::Text);
    CHECK(KindForName(L"README") == Kind::Text);
    CHECK(KindForName(L"setup.exe") == Kind::None);
    CHECK(KindForName(L"screensaver.scr") == Kind::None);   // .scr is a program, never "text"
    CHECK(KindForName(L"noext") == Kind::None);
}

TEST_CASE("preview: text decoding (UTF-8, BOMs, ANSI, binary)") {
    auto a = DecodeText(Bytes("line1\nline2\r\nline3"));
    CHECK(a.ok() && a.value() == L"line1\r\nline2\r\nline3");
    auto ar = DecodeText(Bytes("\xEF\xBB\xBF\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"));   // BOM + "مرحبا"
    CHECK(ar.ok() && ar.value() == L"مرحبا");
    auto u16 = DecodeText({0xFF, 0xFE, 'h', 0, 'i', 0});
    CHECK(u16.ok() && u16.value() == L"hi");
    // A UTF-8 character cut in half at the end (truncated preview) keeps the rest as UTF-8.
    auto cut = DecodeText(Bytes("\xD9\x85\xD8\xB1\xD8"));
    CHECK(cut.ok() && cut.value() == L"مر");
    auto bin = DecodeText({'M', 'Z', 0x90, 0, 3, 0});
    CHECK(!bin.ok());
    CHECK(DecodeText({}).ok());
}

TEST_CASE("preview: image decoding and limits") {
    ComInit com;
    auto img = DecodeImage(MakeBmp(4, 3, 255, 0, 0));
    CHECK(img.ok());
    if (img.ok()) {
        CHECK(img.value().width == 4 && img.value().height == 3);
        CHECK(img.value().pixels.size() == 4 * 3 * 4);
        CHECK(img.value().pixels[2] == 255 && img.value().pixels[1] == 0 && img.value().pixels[3] == 255);   // BGRA red, opaque
    }
    // Large images are reduced while decoding.
    auto big = DecodeImage(MakeBmp(400, 100, 0, 0, 255), 100);
    CHECK(big.ok() && big.value().width == 100 && big.value().height == 25 && big.value().originalWidth == 400);
    // A header claiming a huge image is refused before any pixel memory is allocated.
    auto bomb = DecodeImage(MakeBmp(20000, 20000, 0, 0, 0, false));
    CHECK(!bomb.ok());
    // Garbage and empty input fail cleanly.
    CHECK(!DecodeImage(Bytes("this is not an image at all")).ok());
    CHECK(!DecodeImage({}).ok());
}

TEST_CASE("preview: PDF pages render with the Windows PDF engine") {
    ComInit com;
    const auto pdf = MakePdf(3);
    auto p1 = RenderPdfPage(pdf, 0, 600);
    CHECK(p1.ok());
    if (p1.ok()) {
        CHECK(p1.value().pageCount == 3);
        CHECK(p1.value().pageIndex == 0);
        CHECK(p1.value().image.width == 600);
        CHECK(p1.value().image.height > 800 && p1.value().image.height < 900);   // A4 ratio
        // Top-left corner is white paper.
        const auto& px = p1.value().image.pixels;
        CHECK(px.size() > 4 && px[0] > 240 && px[1] > 240 && px[2] > 240);
    }
    auto p3 = RenderPdfPage(pdf, 2, 400);
    CHECK(p3.ok() && p3.value().pageIndex == 2);
    auto beyond = RenderPdfPage(pdf, 99, 400);                   // clamped to the last page
    CHECK(beyond.ok() && beyond.value().pageIndex == 2);
    CHECK(!RenderPdfPage(Bytes("%PDF-1.4 broken"), 0, 400).ok());
    CHECK(!RenderPdfPage({}, 0, 400).ok());
    CHECK(KindForName(L"report.PDF") == Kind::Pdf);
    CHECK(KindForName(L"clip.mp4") == Kind::Video);
    CHECK(KindForName(L"song.mp3") == Kind::Audio);
}

TEST_CASE("preview: read one entry into memory (full, limit, truncated)") {
    TempDir d(L"preview");
    std::string text;
    for (int i = 0; i < 5000; ++i) text += "line " + std::to_string(i) + "\n";
    WriteFileBytes(d / L"src\\ملف.txt", text);
    WriteFileBytes(d / L"src\\small.txt", "abc");
    CompressionSettings cs;
    CancelToken cancel;
    OperationReport rep;
    const std::wstring arcPath = d / L"p.zip";
    CHECK(CreateArchive(arcPath, fs::CollectSources({d / L"src\\ملف.txt", d / L"src\\small.txt"}, L""), cs, nullptr, cancel, &rep).ok());
    auto opened = Archive::Open(arcPath, {}, nullptr);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto& arc = *opened.value();
    uint32_t bigIdx = 0xFFFFFFFF, smallIdx = 0xFFFFFFFF;
    for (const auto& e : arc.Index()->entries()) {
        if (e.path == L"ملف.txt") bigIdx = e.arcIndex;
        if (e.path == L"small.txt") smallIdx = e.arcIndex;
    }
    CHECK(bigIdx != 0xFFFFFFFF && smallIdx != 0xFFFFFFFF);

    auto full = arc.ReadEntry(bigIdx, 1 << 20, cancel);
    CHECK(full.ok() && full.value().size() == text.size());
    CHECK(full.ok() && std::string(full.value().begin(), full.value().end()) == text);

    // Over the limit: refused (no partial data), declared size checked first.
    auto refused = arc.ReadEntry(bigIdx, 1000, cancel);
    CHECK(!refused.ok() && refused.error().code == ErrorCode::SizeLimitExceeded);

    // Text preview mode: first part only.
    bool truncated = false;
    auto part = arc.ReadEntry(bigIdx, 1000, cancel, &truncated);
    CHECK(part.ok() && truncated && part.value().size() <= 1000 && part.value().size() > 0);
    CHECK(part.ok() && text.compare(0, part.value().size(), std::string(part.value().begin(), part.value().end())) == 0);

    auto tiny = arc.ReadEntry(smallIdx, 1000, cancel, &truncated);
    CHECK(tiny.ok() && !truncated && tiny.value().size() == 3);

    CancelToken stop;
    stop.Cancel();
    CHECK(!arc.ReadEntry(bigIdx, 1 << 20, stop).ok());
    CHECK(!arc.ReadEntry(999999, 100, cancel).ok());
}

TEST_CASE("preview: encrypted entry is not previewed without the password") {
    TempDir d(L"preview-pw");
    WriteFileBytes(d / L"secret.txt", "top secret");
    CompressionSettings cs;
    cs.password = L"Pass123";
    CancelToken cancel;
    OperationReport rep;
    const std::wstring arcPath = d / L"s.zip";
    CHECK(CreateArchive(arcPath, fs::CollectSources({d / L"secret.txt"}, L""), cs, nullptr, cancel, &rep).ok());
    auto opened = Archive::Open(arcPath, {}, nullptr);   // ZIP: names are visible, data needs the password
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto r = opened.value()->ReadEntry(opened.value()->Index()->entries()[0].arcIndex, 1000, cancel);
    CHECK(!r.ok() && r.error().code == ErrorCode::WrongPassword);
}

TEST_CASE("preview: song details for the AIMP-style panel (tags, format line, broken files)") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // no file / not a song: empty details, never a crash
    CHECK(ReadAudioTags(L"Z:\\does\\not\\exist.mp3").title.empty());
    TempDir d(L"audio-tags");
    std::string junk(200000, '\0');
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 2654435761u) >> 13);
    const std::string id3("ID3\x04\x00\x00\x7F\x7F\x7F\x7F", 10);                  // ID3 header claiming a huge tag
    WriteFileBytes(d / L"src\\\u0623\u063A\u0646\u064A\u0629.mp3", id3 + junk);
    const AudioTags t = ReadAudioTags(d / L"src\\\u0623\u063A\u0646\u064A\u0629.mp3");
    CHECK(t.cover.pixels.size() <= 512u * 512u * 4u);
    // format line
    AudioTags f;
    f.sampleRate = 44100; f.bitRate = 320000; f.channels = 2;
    CHECK(AudioFormatLine(f, L"song.mp3") == L"MP3  ::  44.1 kHz, 320 kbps, stereo");
    f.sampleRate = 48000; f.bitRate = 0; f.channels = 1;
    CHECK(AudioFormatLine(f, L"a.flac") == L"FLAC  ::  48 kHz, mono");
    CHECK(AudioFormatLine(AudioTags{}, L"a.wav") == L"WAV");
    CoUninitialize();
}
