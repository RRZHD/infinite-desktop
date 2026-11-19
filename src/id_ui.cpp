// Тема интерфейса (цвета Windows) и примитивы Direct2D для окон приложения.
// Часть InfiniteDesktop.

#include "id_common.h"

using Microsoft::WRL::ComPtr;

// a → b на долю t (t = 0 → a, t = 1 → b)
COLORREF UiMix(COLORREF a, COLORREF b, float t) {
    auto ch = [&](int sa, int sb) { return (int)(sa + (sb - sa) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}
static float Lum(COLORREF c) {
    return (0.2126f * GetRValue(c) + 0.7152f * GetGValue(c) + 0.0722f * GetBValue(c)) / 255.0f;
}

// Палитра. Нейтрали — Fluent (Windows 11) для светлой/тёмной темы приложений
// (AppsUseLightTheme), акцент — системный (DwmGetColorizationColor). Геометрия и
// состояния элементов — по стайлгайду: поле 28–32 px, радиус 8, рамка 1 px, фокус —
// рамка акцентом + мягкое кольцо 3 px (акцент 18 %), переключатель 38×22 с кнопкой 16.
static void LoadTheme(AppState& app) {
    Theme t;
    DWORD light = 0, sz = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz) != ERROR_SUCCESS)
        light = 0;
    t.dark = (light == 0);
    DWORD argb = 0; BOOL opaque = FALSE;
    t.accent = RGB(0, 120, 212);                       // системный по умолчанию
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
        t.accent = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    if (t.dark) {
        t.bg = RGB(0x20, 0x20, 0x20);  t.surface = RGB(0x2b, 0x2b, 0x2b); t.control = RGB(0x2f, 0x2f, 0x2f);
        t.controlHover = RGB(0x38, 0x38, 0x38); t.border = RGB(0x3a, 0x3a, 0x3a); t.borderStrong = RGB(0x55, 0x55, 0x55);
        t.text = RGB(0xff, 0xff, 0xff); t.muted = RGB(0x9e, 0x9e, 0x9e);
        t.danger = RGB(0xff, 0x99, 0xa4); t.warning = RGB(0xfc, 0xe1, 0x00); t.success = RGB(0x6c, 0xcb, 0x5f);
        t.accentInk = UiMix(t.accent, RGB(255, 255, 255), 0.25f);   // как AccentLight2 в тёмной теме
    } else {
        t.bg = RGB(0xf3, 0xf3, 0xf3);  t.surface = RGB(0xfb, 0xfb, 0xfb); t.control = RGB(0xff, 0xff, 0xff);
        t.controlHover = RGB(0xf3, 0xf3, 0xf3); t.border = RGB(0xe5, 0xe5, 0xe5); t.borderStrong = RGB(0xc9, 0xc9, 0xc9);
        t.text = RGB(0x1b, 0x1b, 0x1b); t.muted = RGB(0x61, 0x61, 0x61);
        t.danger = RGB(0xc4, 0x2b, 0x1c); t.warning = RGB(0x9d, 0x5d, 0x00); t.success = RGB(0x0f, 0x7b, 0x0f);
        t.accentInk = UiMix(t.accent, RGB(0, 0, 0), 0.15f);
    }
    t.accentSoft = UiMix(t.accent, t.control, 0.82f);  // 18 % акцента над поверхностью
    t.onAccent   = Lum(t.accent) > 0.6f ? RGB(0, 0, 0) : RGB(255, 255, 255);
    app.theme = t;
}

static ComPtr<IDWriteTextFormat> MakeFmt(AppState& app, float px, DWRITE_FONT_WEIGHT w) {
    ComPtr<IDWriteTextFormat> f;
    if (!app.dwFactory) return f;
    app.dwFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, px, Tr(S_LOCALE), &f);
    if (f) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);   // по вертикали — по центру строки
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

// Шрифты (после InitGraphics — нужна фабрика DirectWrite) и тема.
void InitUi(AppState& app) {
    app.fmtTitle = MakeFmt(app, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    app.fmtBody  = MakeFmt(app, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    app.fmtCtrl  = MakeFmt(app, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    app.fmtSmall = MakeFmt(app, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    app.fmtSec   = MakeFmt(app, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    LoadTheme(app);
}

// Пользователь сменил тему/акцент Windows (WM_SETTINGCHANGE «ImmersiveColorSet»,
// WM_DWMCOLORIZATIONCOLORCHANGED): перечитать палитру и перерисовать всё, что её держит.
void OnThemeChanged(AppState& app) {
    LoadTheme(app);
    ResetMapGdi(app);
    if (app.hud)      { ApplyWin11Style(app, app.hud);      InvalidateRect(app.hud, nullptr, FALSE); }
    if (app.settings) { ApplyWin11Style(app, app.settings); InvalidateRect(app.settings, nullptr, FALSE); }
    RefreshTrayIcon(app);
}

// ---------- Примитивы Direct2D ----------
D2D1_COLOR_F ToD2D(COLORREF c, float a) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
}

// Render target окна + одна кисть (цвет задаётся перед каждой заливкой). Создаётся
// лениво, подгоняется под размер клиента; после потери устройства владелец сбрасывает
// обе ComPtr и мы создаём заново.
// scale — масштаб интерфейса по DPI монитора: рисуем в логических пикселях (96 DPI), а
// Direct2D сам растягивает геометрию и текст до физических.
bool EnsureUiTarget(AppState& app, HWND hwnd, ComPtr<ID2D1HwndRenderTarget>& rt, ComPtr<ID2D1SolidColorBrush>& br, float scale) {
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U sz = D2D1::SizeU((UINT32)max(1L, rc.right), (UINT32)max(1L, rc.bottom));
    if (rt) {
        D2D1_SIZE_U have = rt->GetPixelSize();
        if (have.width != sz.width || have.height != sz.height) rt->Resize(sz);
        rt->SetDpi(96.0f * scale, 96.0f * scale);
        return br != nullptr;
    }
    if (!app.d2dFactory) return false;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(hwnd, sz, D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(app.d2dFactory->CreateHwndRenderTarget(rp, hp, rt.GetAddressOf()))) return false;
    rt->SetDpi(96.0f * scale, 96.0f * scale);
    rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), br.GetAddressOf());
    return br != nullptr;
}

void UiFillRR(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, float rad, COLORREF c, float a) {
    br->SetColor(ToD2D(c, a));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
}

// Контур рисуется по центру линии: сдвигаем на полширины внутрь, чтобы рамка 1 px
// ложилась ровно в пиксели и не размывалась.
void UiStrokeRR(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, float rad, COLORREF c, float a, float w) {
    D2D1_RECT_F q = { r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2 };
    br->SetColor(ToD2D(c, a));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(q, rad, rad), br, w);
}

void UiText(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, IDWriteTextFormat* f, const std::wstring& s,
            const D2D1_RECT_F& r, COLORREF c, DWRITE_TEXT_ALIGNMENT al) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(al);
    br->SetColor(ToD2D(c, 1.0f));
    rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
