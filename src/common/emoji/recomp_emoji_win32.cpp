/* recomp_emoji_win32.cpp — color emoji through DirectWrite + Direct2D on
 * Segoe UI Emoji: the OS's own look, with the OS's own shaping (flags, ZWJ
 * sequences, skin tones). Renders into a WIC bitmap so nothing here touches
 * the launcher's GL context. Windows only; the dispatcher stubs it out
 * everywhere else. */
#include "recomp_emoji.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

struct Factories {
    ID2D1Factory*       d2d = nullptr;
    IDWriteFactory*     dw = nullptr;
    IWICImagingFactory* wic = nullptr;
    bool                tried = false;
    bool                ok = false;
};
Factories g;

template <class T> void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

bool init() {
    if (g.tried) return g.ok;
    g.tried = true;
    /* Whatever apartment the host already chose is fine; only a failure that
     * is not "already initialized differently" is fatal. */
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                 nullptr, reinterpret_cast<void**>(&g.d2d))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g.dw))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g.wic))))
        return false;
    g.ok = true;
    return true;
}

} // namespace

extern "C" int recomp_emoji_render_win32(const char* utf8, size_t len, int px,
                                         RecompEmojiBitmap* out) {
    if (!out || !utf8 || !len || px <= 0) return 0;
    std::memset(out, 0, sizeof(*out));
    if (!init()) return 0;

    /* UTF-8 -> UTF-16. */
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, nullptr, 0);
    if (wlen <= 0) return 0;
    wchar_t* wtext = (wchar_t*)std::calloc((size_t)wlen + 1, sizeof(wchar_t));
    if (!wtext) return 0;
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, wtext, wlen);

    int result = 0;
    IDWriteTextFormat* fmt = nullptr;
    IDWriteTextLayout* layout = nullptr;
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;

    /* Segoe UI Emoji's line box is ~1.33 em; size the em so the box is about
     * the pixel height asked for, and take whatever box results. */
    const float em = (float)px / 1.33f;
    if (FAILED(g.dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em,
                                      L"en-us", &fmt)))
        goto done;
    if (FAILED(g.dw->CreateTextLayout(wtext, (UINT32)wlen, fmt, 4096.0f, 4096.0f, &layout)))
        goto done;
    {
        DWRITE_TEXT_METRICS tm;
        if (FAILED(layout->GetMetrics(&tm))) goto done;
        const int w = (int)std::ceil(tm.widthIncludingTrailingWhitespace);
        const int h = (int)std::ceil(tm.height);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) goto done;
        if (FAILED(g.wic->CreateBitmap((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapCacheOnDemand, &bmp)))
            goto done;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(g.d2d->CreateWicBitmapRenderTarget(bmp, &props, &rt))) goto done;
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush)))
            goto done;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), layout, brush,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        if (FAILED(rt->EndDraw())) goto done;

        unsigned char* pix = (unsigned char*)std::calloc((size_t)w * (size_t)h, 4);
        if (!pix) goto done;
        if (FAILED(bmp->CopyPixels(nullptr, (UINT)w * 4, (UINT)(w * h * 4), pix))) {
            std::free(pix);
            goto done;
        }
        /* Premultiplied BGRA -> straight RGBA. */
        for (int i = 0; i < w * h; ++i) {
            unsigned char* p = pix + (size_t)i * 4;
            const unsigned a = p[3];
            unsigned b = p[0], gch = p[1], r = p[2];
            if (a && a < 255) {
                r = r * 255 / a; gch = gch * 255 / a; b = b * 255 / a;
                if (r > 255) r = 255;
                if (gch > 255) gch = 255;
                if (b > 255) b = 255;
            }
            p[0] = (unsigned char)r;
            p[1] = (unsigned char)gch;
            p[2] = (unsigned char)b;
        }
        out->rgba = pix;
        out->w = w;
        out->h = h;
        result = 1;
    }
done:
    release(brush);
    release(rt);
    release(bmp);
    release(layout);
    release(fmt);
    std::free(wtext);
    return result;
}

#else
/* Non-Windows: the dispatcher provides the stub. */
#endif
