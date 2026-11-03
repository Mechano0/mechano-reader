// Renderer of the Mechano CAD core: one view -> BGRA picture with GDI+ (part of Windows,
// anti-aliased lines, Arabic text shaping). See Cad.h.
#include "core/cad/Cad.h"

#include <windows.h>
#include <objidl.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace mzr::cad {
namespace {

using namespace Gdiplus;

bool StartGdiPlus() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        GdiplusStartupInput in;
        ULONG_PTR token = 0;
        ok = GdiplusStartup(&token, &in, nullptr) == Ok;     // kept for the life of the process
    });
    return ok;
}

struct Look {
    Color background, foreground;
    bool paper;
};

// Device transform: world -> pixels (y down).
struct Map {
    double s = 1, ox = 0, oy = 0;          // X = (x - ox) * s, Y = (oy - y) * s
    PointF operator()(Pt p) const { return PointF(static_cast<REAL>((p.x - ox) * s), static_cast<REAL>((oy - p.y) * s)); }
};

class Painter {
public:
    Painter(Graphics& g, const Look& look, int w, int h) : g_(g), look_(look), w_(w), h_(h),
        family_(L"Arial"), font_(&family_, 100.0f, FontStyleRegular, UnitPixel) {
        const UINT16 em = family_.GetEmHeight(FontStyleRegular);
        ascent_ = 100.0f * family_.GetCellAscent(FontStyleRegular) / em;
        descent_ = 100.0f * family_.GetCellDescent(FontStyleRegular) / em;
        line_ = 100.0f * family_.GetLineSpacing(FontStyleRegular) / em;
    }

    // Draws the primitives of `v`. `toPaper` maps the view's own coordinates first (viewports).
    void Draw(const View& v, const Map& m, const std::function<Pt(Pt)>* toPaper) {
        std::vector<PointF> pts;
        for (const Prim& p : v.prims) {
            if (p.first + p.count > v.pts.size()) continue;
            const Color c = ColorOf(p.rgb, p.kind == PrimKind::HatchFill ? 70 : 255);
            auto dev = [&](Pt q) { return m(toPaper ? (*toPaper)(q) : q); };
            if (p.kind == PrimKind::Text) {
                if (texts_ >= 30000 || p.text >= v.texts.size()) continue;
                Text(v.texts[p.text], p, dev(v.pts[p.first]), m.s * (toPaper ? scale_ : 1), c);
                continue;
            }
            pts.clear();
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (uint32_t k = 0; k < p.count; ++k) {
                const Pt q = v.pts[p.first + k];
                if (std::isnan(q.x)) { pts.push_back(PointF(NAN, NAN)); continue; }
                const PointF d = dev(q);
                minX = std::min(minX, d.X); maxX = std::max(maxX, d.X); minY = std::min(minY, d.Y); maxY = std::max(maxY, d.Y);
                pts.push_back(d);
            }
            if (maxX < -2 || minX > w_ + 2 || maxY < -2 || minY > h_ + 2) continue;    // outside the picture
            if (p.kind == PrimKind::Dot) {
                SolidBrush b(c);
                g_.FillRectangle(&b, pts[0].X - 1, pts[0].Y - 1, 2.0f, 2.0f);
            } else if (p.kind == PrimKind::Lines) {
                g_.DrawLines(PenOf(c), pts.data(), static_cast<INT>(pts.size()));
            } else {
                GraphicsPath path(FillModeAlternate);
                size_t start = 0;
                for (size_t k = 0; k <= pts.size(); ++k) {
                    if (k == pts.size() || std::isnan(pts[k].X)) {
                        if (k - start >= 3) { path.StartFigure(); path.AddPolygon(&pts[start], static_cast<INT>(k - start)); }
                        start = k + 1;
                    }
                }
                SolidBrush b(c);
                g_.FillPath(&b, &path);
            }
        }
    }

    void SetViewportScale(double s) { scale_ = s; }

private:
    Color ColorOf(uint32_t rgb, BYTE alpha) const {
        if (rgb == kForeground) { Color f = look_.foreground; return Color(alpha, f.GetR(), f.GetG(), f.GetB()); }
        BYTE r = static_cast<BYTE>(rgb >> 16), g = static_cast<BYTE>(rgb >> 8), b = static_cast<BYTE>(rgb);
        if (look_.paper && r > 235 && g > 235 && b > 235) r = g = b = 0;       // white on paper prints black
        if (!look_.paper && r < 20 && g < 20 && b < 20) r = g = b = 235;       // black on the dark model shows white
        return Color(alpha, r, g, b);
    }

    Pen* PenOf(const Color& c) {
        const ARGB key = c.GetValue();
        auto it = pens_.find(key);
        if (it != pens_.end()) return it->second.get();
        auto pen = std::make_unique<Pen>(c, 1.0f);
        Pen* raw = pen.get();
        pens_.emplace(key, std::move(pen));
        return raw;
    }

    void Text(const std::wstring& s, const Prim& p, PointF at, double pxPerUnit, const Color& c) {
        const double capPx = p.height * pxPerUnit;
        if (!(capPx >= 1.5) || capPx > h_ * 4.0) return;                     // too small to read / absurd
        ++texts_;
        const float k = static_cast<float>(capPx / 72.0);                    // the 100 px font has a ~72 px cap
        int lines = 1;
        for (wchar_t ch : s) if (ch == L'\n') ++lines;
        const float cap = 72.0f;
        float y0 = 0;
        switch (p.vAlign) {
        case 3: y0 = -(ascent_ - cap); break;                                // top
        case 2: y0 = -(ascent_ - cap) - ((lines - 1) * line_ + cap) / 2; break;   // middle
        case 1: y0 = -(lines - 1) * line_ - ascent_ - descent_ * 0.5f; break;      // bottom
        default: y0 = -(lines - 1) * line_ - ascent_; break;                 // baseline
        }
        StringFormat fmt(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        fmt.SetAlignment(p.hAlign == 1 ? StringAlignmentCenter : p.hAlign == 2 ? StringAlignmentFar : StringAlignmentNear);
        GraphicsState st = g_.Save();
        g_.TranslateTransform(at.X, at.Y);
        g_.RotateTransform(static_cast<REAL>(-p.rotation * 180.0 / 3.14159265358979323846));
        g_.ScaleTransform(k * p.widthFactor, k);
        SolidBrush b(c);
        g_.DrawString(s.c_str(), static_cast<INT>(s.size()), &font_, PointF(0, y0), &fmt, &b);
        g_.Restore(st);
    }

    Graphics& g_;
    Look look_;
    int w_, h_;
    FontFamily family_;
    Font font_;
    float ascent_ = 90, descent_ = 20, line_ = 115;
    std::unordered_map<ARGB, std::unique_ptr<Pen>> pens_;
    size_t texts_ = 0;
    double scale_ = 1;
};

} // namespace

Result<preview::Image> RenderView(const Drawing& d, uint32_t index, uint32_t targetWidth) {
    if (index >= d.views.size()) return ErrorInfo{ErrorCode::Internal, L"This view does not exist.", L""};
    if (!StartGdiPlus()) return ErrorInfo{ErrorCode::Internal, L"The drawing could not be drawn (GDI+ is not available).", L""};
    const View& v = d.views[index];

    // World box with a small margin; picture size from its shape.
    double minX = v.minX, minY = v.minY, maxX = v.maxX, maxY = v.maxY;
    const double mx = (maxX - minX) * 0.02, my = (maxY - minY) * 0.02;
    minX -= mx; maxX += mx; minY -= my; maxY += my;
    const double ww = maxX - minX, wh = maxY - minY;
    if (!(ww > 0) || !(wh > 0) || !std::isfinite(ww) || !std::isfinite(wh))
        return ErrorInfo{ErrorCode::Corrupted, L"This drawing has no visible size.", L""};
    double W = std::clamp<double>(targetWidth, 64, preview::kDecodeSide);
    double H = W * wh / ww;
    if (H > preview::kDecodeSide) { W = W * preview::kDecodeSide / H; H = preview::kDecodeSide; }
    if (H < 32) H = 32;
    if (W < 16) W = 16;
    const int w = static_cast<int>(W), h = static_cast<int>(H);
    Map m;
    m.s = std::min(w / ww, h / wh);
    m.ox = minX - (w / m.s - ww) / 2;
    m.oy = maxY + (h / m.s - wh) / 2;

    preview::Image img;
    img.width = img.originalWidth = static_cast<uint32_t>(w);
    img.height = img.originalHeight = static_cast<uint32_t>(h);
    try { img.pixels.assign(static_cast<size_t>(w) * h * 4, 0); }
    catch (const std::bad_alloc&) { return ErrorInfo{ErrorCode::SizeLimitExceeded, L"Not enough memory to draw this view.", L""}; }

    Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, img.pixels.data());
    if (bmp.GetLastStatus() != Ok) return ErrorInfo{ErrorCode::Internal, L"The drawing could not be drawn.", L"Bitmap"};
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    Look look;
    look.paper = v.paper;
    if (v.paper) {
        g.Clear(Color(255, 150, 152, 156));
        SolidBrush white(Color(255, 255, 255, 255));
        const PointF a = m({v.minX, v.maxY}), b = m({v.maxX, v.minY});
        g.FillRectangle(&white, a.X, a.Y, b.X - a.X, b.Y - a.Y);
        look.background = Color(255, 255, 255, 255);
        look.foreground = Color(255, 0, 0, 0);
    } else {
        g.Clear(Color(255, 33, 40, 48));                                     // AutoCAD-like dark model space
        look.background = Color(255, 33, 40, 48);
        look.foreground = Color(255, 236, 236, 236);
    }

    Painter painter(g, look, w, h);
    // Layout: the model through each viewport first, then the paper-space objects on top.
    if (v.paper && !d.views.empty()) {
        const View& model = d.views[0];
        for (const Viewport& vp : v.viewports) {
            const PointF a = m({vp.paperCenter.x - vp.paperWidth / 2, vp.paperCenter.y + vp.paperHeight / 2});
            const PointF b = m({vp.paperCenter.x + vp.paperWidth / 2, vp.paperCenter.y - vp.paperHeight / 2});
            g.SetClip(RectF(a.X, a.Y, b.X - a.X, b.Y - a.Y));
            std::function<Pt(Pt)> toPaper = [&vp](Pt q) {
                return Pt{vp.paperCenter.x + (q.x - vp.modelCenter.x) * vp.scale, vp.paperCenter.y + (q.y - vp.modelCenter.y) * vp.scale};
            };
            painter.SetViewportScale(vp.scale);
            painter.Draw(model, m, &toPaper);
            g.ResetClip();
        }
        painter.SetViewportScale(1);
    }
    painter.Draw(v, m, nullptr);
    g.Flush(FlushIntentionSync);
    return img;
}

} // namespace mzr::cad
