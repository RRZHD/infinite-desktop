// Иконка в области уведомлений: настройки, переключатели, выход.
// Часть InfiniteDesktop.

#include "id_common.h"

static const UINT TRAY_ID = 1;
enum { TM_SETTINGS = 1, TM_MAP, TM_PREVIEW, TM_REFRESH, TM_EXIT };

// Иконка рисуется кодом — в сборке нет ресурсов (plain cl). Образ тот же, что на
// миникарте: акцентный квадрат, в нём рамка вьюпорта и окошко. Альфа берётся из
// второго, «масочного» рисунка: GDI не пишет альфа-канал в 32-битный DIB.
static HICON MakeTrayIcon(AppState& app, int s) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s; bi.bmiHeader.biHeight = -s;   // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    ScopedDC dc(CreateCompatibleDC(sdc)), mdc(CreateCompatibleDC(sdc));
    ReleaseDC(nullptr, sdc);
    void* px = nullptr; void* mpx = nullptr;
    ScopedBitmap bmp(CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &px, nullptr, 0));
    ScopedBitmap msk(CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &mpx, nullptr, 0));
    if (!dc || !mdc || !bmp || !msk || !px || !mpx) return nullptr;
    HGDIOBJ o1 = SelectObject(dc, bmp), o2 = SelectObject(mdc, msk);

    auto draw = [&](HDC h, COLORREF fill, COLORREF ink) {
        RECT all = { 0, 0, s, s };
        FillRect(h, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
        ScopedBrush fb(CreateSolidBrush(fill)); ScopedPen fp(CreatePen(PS_SOLID, 1, fill));
        SelectObject(h, fb); SelectObject(h, fp);
        int in = max(1, s / 16), d = s * 6 / 16;               // скругление (диаметр)
        RoundRect(h, in, in, s - in, s - in, d, d);
        int pw = max(1, s / 10), m = s * 5 / 16;              // рамка вьюпорта
        ScopedPen ip(CreatePen(PS_SOLID, pw, ink));
        SelectObject(h, ip); SelectObject(h, GetStockObject(HOLLOW_BRUSH));
        Rectangle(h, m, m, s - m, s - m);
        ScopedBrush ib(CreateSolidBrush(ink));                 // окошко у правого-нижнего угла
        SelectObject(h, ib); SelectObject(h, GetStockObject(NULL_PEN));
        int q = max(2, s * 4 / 16);
        Rectangle(h, s - m - q + 1, s - m - q + 1, s - m + 2, s - m + 2);
        SelectObject(h, GetStockObject(BLACK_BRUSH)); SelectObject(h, GetStockObject(BLACK_PEN));
    };
    draw(dc,  app.theme.accent, app.theme.onAccent);
    draw(mdc, RGB(255, 255, 255), RGB(255, 255, 255));        // покрытие → альфа
    GdiFlush();
    for (int i = 0; i < s * s; ++i) {                          // альфа из маски + предумножение
        BYTE a = ((const BYTE*)mpx)[i * 4];
        BYTE* p = (BYTE*)px + i * 4;
        p[0] = (BYTE)(p[0] * a / 255); p[1] = (BYTE)(p[1] * a / 255); p[2] = (BYTE)(p[2] * a / 255); p[3] = a;
    }
    SelectObject(dc, o1); SelectObject(mdc, o2);
    ScopedBitmap mono(CreateBitmap(s, s, 1, 1, nullptr));      // обязательна для CreateIconIndirect
    ICONINFO ii = {};
    ii.fIcon = TRUE; ii.hbmMask = mono; ii.hbmColor = bmp;
    return CreateIconIndirect(&ii);
}

void AddTrayIcon(AppState& app) {
    if (!app.hud) return;
    if (!app.trayIcon) app.trayIcon = MakeTrayIcon(app, GetSystemMetrics(SM_CXSMICON));
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hud; nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = app.trayIcon;
    lstrcpynW(nid.szTip, Tr(S_TRAY_TIP), ARRAYSIZE(nid.szTip));
    if (app.trayAdded) { Shell_NotifyIconW(NIM_MODIFY, &nid); return; }
    app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;   // координаты и событие — в одном сообщении
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

// Перерисовать иконку (сменился акцент/тема).
void RefreshTrayIcon(AppState& app) {
    if (app.trayIcon) { DestroyIcon(app.trayIcon); app.trayIcon = nullptr; }
    AddTrayIcon(app);
}

void RemoveTrayIcon(AppState& app) {
    if (app.trayAdded) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid); nid.hWnd = app.hud; nid.uID = TRAY_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        app.trayAdded = false;
    }
    if (app.trayIcon) { DestroyIcon(app.trayIcon); app.trayIcon = nullptr; }
}

// Общие переключатели: ими пользуются и хоткеи, и меню трея.
void ToggleMinimap(AppState& app) {
    app.hudVisible = !app.hudVisible;
    if (app.hudVisible) { RegisterAllThumbs(app); ShowWindow(app.hud, SW_SHOWNA); }
    else {
        for (auto& w : app.wins) w.thumb.reset();   // RAII: превью снимаются
        ShowWindow(app.hud, SW_HIDE);
    }
}
void TogglePreviews(AppState& app) {
    app.previewsOn = !app.previewsOn;
    if (app.previewsOn) { if (app.hudVisible) RegisterAllThumbs(app); }
    else for (auto& w : app.wins) w.thumb.reset();
    InvalidateRect(app.hud, nullptr, FALSE);
}

static void ShowTrayMenu(AppState& app, int x, int y) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, TM_SETTINGS, Tr(S_MENU_SETTINGS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (app.hudVisible ? MF_CHECKED : 0), TM_MAP,     Tr(S_MENU_MAP));
    AppendMenuW(m, MF_STRING | (app.previewsOn ? MF_CHECKED : 0), TM_PREVIEW, Tr(S_MENU_PREVIEW));
    AppendMenuW(m, MF_STRING, TM_REFRESH, Tr(S_MENU_REFRESH));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, TM_EXIT, Tr(S_MENU_EXIT));
    SetMenuDefaultItem(m, TM_SETTINGS, FALSE);
    // Требование TrackPopupMenu: владелец должен быть активен, иначе меню не закроется
    // кликом мимо; WM_NULL после — чтобы оно закрылось корректно.
    SetForegroundWindow(app.hud);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, 0, app.hud, nullptr);
    PostMessageW(app.hud, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case TM_SETTINGS: OpenSettings(app); break;
    case TM_MAP:      ToggleMinimap(app); break;
    case TM_PREVIEW:  TogglePreviews(app); break;
    case TM_REFRESH:  RebuildWindowList(app); RepositionAll(app); break;
    case TM_EXIT:     PostQuitMessage(0); break;
    }
}

// NOTIFYICON_VERSION_4: LOWORD(lp) — событие, wp — экранные координаты.
void OnTrayMessage(AppState& app, WPARAM wp, LPARAM lp) {
    switch (LOWORD(lp)) {
    case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK:
        OpenSettings(app);
        break;
    case WM_CONTEXTMENU:
        ShowTrayMenu(app, (short)LOWORD(wp), (short)HIWORD(wp));
        break;
    }
}
