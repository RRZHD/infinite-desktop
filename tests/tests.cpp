// Тесты InfiniteDesktop. Сборка и запуск: build.bat test
//
// Собираются с теми же модулями src\*.cpp, что и программа (без main.cpp). Проверяют
// логику, которой не нужен запущенный холст: геометрию парковки, выбор окна по
// направлению, файлы раскладки/хоткеев/закладок и сохранность места окна на холсте при
// сворачивании и пересборке списка. Чужие окна тесты только читают и никуда не двигают;
// своё тестовое окно создают сами. Файлы пишутся во временный каталог (LOCALAPPDATA
// подменяется), настоящие настройки не затрагиваются.
//
// Режим снимков — tests.exe --shot <масштаб> <каталог>: рисует окно настроек и миникарту
// в заданном масштабе интерфейса и сохраняет PNG (проверка вёрстки на 150/200 % без
// смены масштаба в системе).

#include "../src/id_common.h"
#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; \
    wprintf(L"  FAIL %hs:%d  %hs\n", __FILE__, __LINE__, #cond); } } while (0)
static void Section(const wchar_t* name) { wprintf(L"%s\n", name); }

// ---------- Геометрия ----------
static void TestParkPosition() {
    Section(L"ParkPosition");
    const RECT scr = { 0, 0, 1920, 1080 };
    int x, y;
    x = 5000; y = 300;  ParkPosition(scr, 800, 600, x, y);      // далеко справа
    CHECK(x == 1919 && y == 300);                               // левый край на 1 px в кадре
    x = -9000; y = 300; ParkPosition(scr, 800, 600, x, y);      // далеко слева
    CHECK(x == -799 && y == 300);                               // правый край на 1 px в кадре
    x = 100; y = 4000;  ParkPosition(scr, 800, 600, x, y);      // снизу
    CHECK(x == 100 && y == 1079);
    x = 100; y = -4000; ParkPosition(scr, 800, 600, x, y);      // сверху
    CHECK(x == 100 && y == -599);
    x = 5000; y = 5000; ParkPosition(scr, 800, 600, x, y);      // по диагонали — в угол
    CHECK(x == 1919 && y == 1079);
    x = 200; y = 150;   ParkPosition(scr, 800, 600, x, y);      // в кадре — не трогаем
    CHECK(x == 200 && y == 150);
    const RECT multi = { -1920, 0, 1920, 1080 };                // второй монитор слева
    x = -9000; y = 0;   ParkPosition(multi, 800, 600, x, y);
    CHECK(x == -1920 - 800 + 1);
}

static void TestPickFocusTarget() {
    Section(L"PickFocusTarget");
    // 0 — активное окно в центре; 1 — прямо справа; 2 — справа ближе, но по диагонали; 3 — слева
    std::vector<RECT> w = {
        { 0, 0, 100, 100 }, { 600, 0, 700, 100 }, { 400, 400, 500, 500 }, { -500, 0, -400, 100 } };
    CHECK(PickFocusTarget(w, 0, 50, 50, 1, 0) == 1);     // прямо стоящее выигрывает у диагонального
    CHECK(PickFocusTarget(w, 0, 50, 50, -1, 0) == 3);
    CHECK(PickFocusTarget(w, 0, 50, 50, 0, 1) == 2);     // вниз — только диагональное
    CHECK(PickFocusTarget(w, 0, 50, 50, 0, -1) == -1);   // сверху никого
    std::vector<RECT> one = { { 0, 0, 100, 100 } };
    CHECK(PickFocusTarget(one, 0, 50, 50, 1, 0) == -1);  // само себя не выбирает
    CHECK(PickFocusTarget(one, -1, -500, 50, 1, 0) == 0);   // не от окна, а от центра экрана
}

// ---------- Сопоставление раскладки ----------
static SavedWin Saved(const wchar_t* exe, const wchar_t* cls, const wchar_t* title, LONG x) {
    SavedWin s; s.exe = exe; s.cls = cls; s.title = title; s.world = { x, 0, x + 10, 10 }; s.used = false;
    return s;
}
static void TestMatchSavedLayout() {
    Section(L"MatchSavedLayout");
    // Один класс окна у разных программ (Chromium/Electron) — раньше путались.
    std::vector<SavedWin> saved = {
        Saved(L"code.exe",   L"Chrome_WidgetWin_1", L"main.cpp - Code", 100),
        Saved(L"chrome.exe", L"Chrome_WidgetWin_1", L"Inbox - Chrome",  200),
        Saved(L"chrome.exe", L"Chrome_WidgetWin_1", L"Docs - Chrome",   300) };
    std::vector<WinKeyId> keys = {
        { L"chrome.exe", L"Chrome_WidgetWin_1", L"New Tab - Chrome" },   // заголовок сменился
        { L"code.exe",   L"Chrome_WidgetWin_1", L"other.cpp - Code" },   // заголовок сменился
        { L"chrome.exe", L"Chrome_WidgetWin_1", L"Docs - Chrome" },      // точное совпадение
        { L"slack.exe",  L"Chrome_WidgetWin_1", L"Slack" } };            // программы нет в файле
    std::vector<int> m = MatchSavedLayout(saved, keys);
    CHECK(m[2] == 2);        // точное совпадение берётся первым проходом
    CHECK(m[0] == 1);        // оставшаяся запись chrome — по программе и классу
    CHECK(m[1] == 0);        // code получает своё место, а не место chrome
    CHECK(m[3] == -1);       // чужая программа с тем же классом места не получает
    CHECK(saved[0].used && saved[1].used && saved[2].used);

    // Запись старого формата (без программы) подходит по классу — как раньше.
    std::vector<SavedWin> legacy = { Saved(L"", L"Notepad", L"a.txt - Notepad", 500) };
    std::vector<WinKeyId> k2 = { { L"notepad.exe", L"Notepad", L"b.txt - Notepad" },
                                 { L"notepad.exe", L"Notepad", L"c.txt - Notepad" } };
    std::vector<int> m2 = MatchSavedLayout(legacy, k2);
    CHECK(m2[0] == 0 && m2[1] == -1);   // одна запись — одному окну
}

// ---------- Файлы ----------
static std::wstring g_tmp;   // временный каталог вместо %LOCALAPPDATA%
static void UseTempConfigDir() {
    wchar_t t[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, t);
    g_tmp = std::wstring(t) + L"InfiniteDesktopTests_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_tmp.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", g_tmp.c_str());
}
static void RemoveTempConfigDir() {
    for (const wchar_t* f : { L"layout.txt", L"layout.txt.tmp", L"hotkeys.txt", L"marks.txt", L"ui.txt",
                              L"session.txt", L"wall.log" })
        DeleteFileW(ConfigPath(f).c_str());
    RemoveDirectoryW((g_tmp + L"\\InfiniteDesktop").c_str());
    RemoveDirectoryW(g_tmp.c_str());
}
static void WriteFileText(const wchar_t* name, const char* text) {
    std::ofstream f(ConfigPath(name).c_str(), std::ios::binary | std::ios::trunc);
    f.write(text, (std::streamsize)strlen(text));
}

static LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static HWND MakeTestWindow(const wchar_t* title, bool visible) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW c = {};
        c.lpfnWndProc = TestWndProc; c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = L"InfiniteDesktopTestWindow";
        c.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&c);
        reg = true;
    }
    HWND h = CreateWindowExW(0, L"InfiniteDesktopTestWindow", title, WS_OVERLAPPEDWINDOW,
                             200, 200, 320, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h && visible) ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

static TrackedWin FakeWin(HWND h, RECT world) {
    TrackedWin w;
    w.hwnd = h; w.world = world; w.vm = { 0, 0, 0, 0 }; w.pinned = false;
    return w;
}

static void TestLayoutFile() {
    Section(L"layout.txt");
    AppState app;
    HWND a = MakeTestWindow(L"First\twith tab", false), b = MakeTestWindow(L"Second", false);
    app.wins.push_back(FakeWin(a, { -5000, 120, -4200, 720 }));
    app.wins.push_back(FakeWin(b, { 30000, -900, 30640, -420 }));
    HWND c = MakeTestWindow(L"Minimized one", false);
    app.away.push_back({ c, { 7, 8, 9, 10 }, false, false, { 0, 0 } });   // свёрнутые тоже сохраняются
    SaveLayout(app);
    CHECK(GetFileAttributesW(ConfigPath(L"layout.txt.tmp").c_str()) == INVALID_FILE_ATTRIBUTES);   // временный убран

    AppState in;
    LoadLayout(in);
    CHECK(in.saved.size() == 3);
    if (in.saved.size() == 3) {
        CHECK(in.saved[0].world.left == -5000 && in.saved[0].world.bottom == 720);
        CHECK(in.saved[0].cls == L"InfiniteDesktopTestWindow");
        CHECK(in.saved[0].title == L"First\twith tab");            // заголовок — остаток строки
        CHECK(in.saved[0].exe == L"tests.exe");
        CHECK(in.saved[1].world.left == 30000 && in.saved[1].world.top == -900);
        CHECK(in.saved[1].title == L"Second");
        CHECK(in.saved[2].title == L"Minimized one" && in.saved[2].world.left == 7);
    }
    DestroyWindow(a); DestroyWindow(b); DestroyWindow(c);

    // Старый формат: без заголовка версии и без поля программы.
    WriteFileText(L"layout.txt", "10\t20\t30\t40\tNotepad\tUntitled - Notepad\r\nbroken line\n\n");
    AppState old;
    LoadLayout(old);
    CHECK(old.saved.size() == 1);
    if (old.saved.size() == 1) {
        CHECK(old.saved[0].exe.empty() && old.saved[0].cls == L"Notepad");
        CHECK(old.saved[0].title == L"Untitled - Notepad" && old.saved[0].world.right == 30);
    }
}

static void TestHotkeysFile() {
    Section(L"hotkeys.txt");
    // Формат v1 (4 места): ids 15…18 — «запомнить 1…4», 19…22 — «перейти 1…4». В v2 мест 9,
    // и те же id значат другое — старый файл должен мигрировать, а не лечь мимо.
    const int oldMark1 = HK_FOCUS_DOWN + 1, oldGoto1 = oldMark1 + 4;
    std::string v1;
    v1 += std::to_string(HK_HOME) + " " + std::to_string(MOD_CONTROL | MOD_SHIFT) + " " + std::to_string('J') + "\n";
    for (int i = 0; i < 4; ++i) {
        v1 += std::to_string(oldMark1 + i) + " " + std::to_string(MOD_WIN | MOD_SHIFT) + " " + std::to_string('1' + i) + "\n";
        v1 += std::to_string(oldGoto1 + i) + " " + std::to_string(MOD_WIN) + " " + std::to_string('1' + i) + "\n";
    }
    WriteFileText(L"hotkeys.txt", v1.c_str());
    AppState app;
    InitDefaultKeys(app);
    LoadHotkeys(app);
    CHECK(app.keys[HK_HOME].mods == (MOD_CONTROL | MOD_SHIFT) && app.keys[HK_HOME].vk == 'J');
    CHECK(app.keys[HK_MARK1].mods == (MOD_WIN | MOD_SHIFT) && app.keys[HK_MARK1].vk == '1');
    CHECK(app.keys[HK_GOTO1].mods == MOD_WIN && app.keys[HK_GOTO1].vk == '1');
    CHECK(app.keys[HK_MARK9].mods == (MOD_WIN | MOD_SHIFT) && app.keys[HK_MARK9].vk == '9');   // группа согласована
    CHECK(app.keys[HK_GOTO9].mods == MOD_WIN && app.keys[HK_GOTO9].vk == '9');
    CHECK(app.keys[HK_LEFT].vk == VK_LEFT);   // не упомянутое в файле — по умолчанию

    // Повреждённый файл не должен портить привязки.
    WriteFileText(L"hotkeys.txt", "v2\nabc\n9999 1 2\n-5 1 2\n");
    AppState bad;
    InitDefaultKeys(bad);
    LoadHotkeys(bad);
    CHECK(bad.keys[HK_HOME].vk == 'H' && bad.keys[HK_GOTO5].vk == '5');
}

static void TestMarksFile() {
    Section(L"marks.txt");
    AppState app;
    app.targetX = -12345.4; app.targetY = 678.6;
    SetMark(app, 0);
    app.targetX = 5; app.targetY = 6;
    SetMark(app, 8);
    SetMark(app, 9);      // вне диапазона — игнорируется
    AppState in;
    LoadMarks(in);
    CHECK(in.marks[0].set && in.marks[0].x == -12345.0 && in.marks[0].y == 679.0);
    CHECK(in.marks[8].set && in.marks[8].x == 5.0);
    CHECK(!in.marks[1].set);
    in.targetX = 0; in.targetY = 0;
    GotoMark(in, 0);
    CHECK(in.targetX == -12345.0 && in.targetY == 679.0);
    GotoMark(in, 3);      // не задана — камера на месте
    CHECK(in.targetX == -12345.0);
}

static void TestSessionFile() {
    Section(L"session.txt (recovery after an unclean exit)");
    DeleteFileW(ConfigPath(L"session.txt").c_str());
    // Обычный запуск: Snap был включён, иконки были видны — это и записано в файл сеанса.
    AppState first;
    first.prevWinArranging = TRUE; first.iconsWereVisible = true;
    BeginSession(first);
    CHECK(GetFileAttributesW(ConfigPath(L"session.txt").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(first.prevWinArranging == TRUE && first.iconsWereVisible);

    // Процесс сняли. Следующий запуск застаёт систему в «нашем» состоянии: Snap выключен,
    // иконки спрятаны. Без файла он принял бы это за исходное и ничего не вернул бы.
    AppState second;
    second.prevWinArranging = FALSE; second.iconsWereVisible = false;
    second.iconsWin = (HWND)(INT_PTR)1;     // окно иконок «найдено»; само окно тесту не нужно
    BeginSession(second);
    CHECK(second.prevWinArranging == TRUE);   // при выходе Snap будет включён
    CHECK(second.iconsWereVisible);           // и иконки показаны
    CHECK(!second.iconsLayerOurs);            // слоя иконок нет — возвращать нечего

    // Чистый выход убирает файл; следующий запуск верит тому, что застал.
    EndSession();
    CHECK(GetFileAttributesW(ConfigPath(L"session.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    AppState third;
    third.prevWinArranging = FALSE; third.iconsWereVisible = false;
    BeginSession(third);
    CHECK(third.prevWinArranging == FALSE && !third.iconsWereVisible);   // пользователь сам выключил Snap
    EndSession();
}

// ---------- Сохранность места окна на холсте ----------
static TrackedWin* Find(AppState& app, HWND h) {
    for (auto& w : app.wins) if (w.hwnd == h) return &w;
    return nullptr;
}
static void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    MSG m;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}

static void TestWindowKeepsPlace() {
    Section(L"window keeps its place (minimize, rebuild)");
    AppState app;
    g_app = &app;                     // EnumWindows-колбэки берут состояние отсюда
    UpdateVirtualScreen(app);
    app.hudVisible = false;           // без превью — у тестов нет окна миникарты
    HWND h = MakeTestWindow(L"InfiniteDesktop test window", true);
    Pump(150);
    RebuildWindowList(app);           // только читает положение окон, ничего не двигает
    TrackedWin* w = Find(app, h);
    CHECK(w != nullptr);
    if (!w) { DestroyWindow(h); g_app = nullptr; return; }

    // Изображаем парковку: настоящее место окна — далеко на холсте, а на экране оно стоит
    // там, куда его «припарковали» (берём фактическую позицию, окно не двигаем).
    RECT r; GetWindowRect(h, &r);
    const RECT farWorld = { 40000, 30000, 40000 + (r.right - r.left), 30000 + (r.bottom - r.top) };
    auto park = [&](TrackedWin& t) { t.world = farWorld; t.parkedOff = true; t.parkedPos = { r.left, r.top }; t.pinned = true; };
    park(*w);

    // 1. Пересборка списка (Ctrl+Alt+R) — место и закрепление сохраняются.
    RebuildWindowList(app);
    w = Find(app, h);
    CHECK(w && w->world.left == 40000 && w->world.top == 30000 && w->parkedOff && w->pinned);

    // 2. Свернули (как Win+D) и развернули — то же.
    ShowWindow(h, SW_SHOWMINNOACTIVE);
    Pump(400);
    SyncWorldFromScreen(app);
    CHECK(Find(app, h) == nullptr);                       // свёрнутое из списка уходит
    CHECK(!app.away.empty());                             // …но помнится
    ShowWindow(h, SW_SHOWNOACTIVATE);
    Pump(400);
    AddNewWindows(app);
    w = Find(app, h);
    CHECK(w && w->world.left == 40000 && w->world.top == 30000 && w->parkedOff && w->pinned);
    bool stillAway = false;
    for (auto& a : app.away) if (a.hwnd == h) stillAway = true;
    CHECK(!stillAway);

    // 3. Окно было в кадре (не на парковке): после сворачивания его место берётся с экрана.
    if (w) { w->parkedOff = false; w->world = { r.left, r.top, r.right, r.bottom }; w->pinned = false; }
    ShowWindow(h, SW_SHOWMINNOACTIVE);
    Pump(400);
    SyncWorldFromScreen(app);
    app.camX = 1000; app.camY = 0;                        // камера уехала, пока окно свёрнуто
    ShowWindow(h, SW_SHOWNOACTIVATE);
    Pump(400);
    AddNewWindows(app);
    w = Find(app, h);
    CHECK(w && w->world.left == r.left + 1000 && !w->parkedOff);   // мир = экран + камера

    // 4. Закрытое окно забывается совсем.
    DestroyWindow(h);
    CHECK(PruneDeadWindows(app));
    CHECK(Find(app, h) == nullptr);
    SyncWorldFromScreen(app);
    for (auto& a : app.away) CHECK(a.hwnd != h);
    g_app = nullptr;
}

static void TestWheelInput() {
    Section(L"wheel input is applied on the main thread");
    AppState app;
    app.vsX = 0; app.vsY = 0; app.vsW = 1920; app.vsH = 1080;
    CHECK(!WheelInputPending(app));
    // То, что делает hook-поток: копит дельту и точку.
    app.wheelZoomPt.store(((long long)-300 << 32) | (unsigned int)-200);
    app.wheelZoom.fetch_add(-WHEEL_DELTA);
    app.wheelZoom.fetch_add(-WHEEL_DELTA);
    CHECK(WheelInputPending(app));
    ApplyWheelInput(app);
    CHECK(!WheelInputPending(app));
    CHECK(fabs(app.zoomTarget - 1.0 / (ZOOM_STEP * ZOOM_STEP)) < 1e-9);   // два шага сложились
    CHECK(app.axCX == -300.0 && app.axCY == -200.0);                      // отрицательные координаты целы
    app.wheelPanX.fetch_add(100); app.wheelPanY.fetch_add(-50);
    ApplyWheelInput(app);
    CHECK(app.targetX == 100 * TP_WHEEL && app.targetY == 50 * TP_WHEEL);
}

// ---------- Тень окна ----------
static void TestShadowBitmap() {
    Section(L"window shadow matches the measured Windows shadow");
    std::vector<DWORD> px; int sw = 0, sh = 0; RECT pad = {};
    // Небольшое окно: картинка 1:1, поля — по длине снятых профилей (неактивное окно).
    BuildShadowBitmap(500, 300, false, false, 1.0, px, sw, sh, pad);
    CHECK(pad.left == 40 && pad.top == 13);                       // длины таблиц сбоку и сверху
    CHECK(sw == 500 + pad.left + pad.right && sh == 300 + pad.top + pad.bottom);   // без уменьшения
    auto a = [&](int x, int y) { return (int)(px[(size_t)(y + pad.top) * sw + (x + pad.left)] >> 24); };   // x,y — от угла рамки
    CHECK(abs(a(-1, 150) - 28) <= 1);          // вплотную слева от рамки, посередине высоты: как у Windows
    CHECK(abs(a(-10, 150) - 11) <= 1);         // в 10 px слева
    CHECK(abs(a(250, 300) - 56) <= 2);         // вплотную под окном
    CHECK(abs(a(250, 330) - 22) <= 2);         // в 31 px под окном
    CHECK(abs(a(250, -1) - 16) <= 1);          // вплотную над окном — тень смещена вниз, сверху её мало
    CHECK(a(250, -13) <= 1 && a(-40, 150) <= 1);                  // у края полей тень сошла на нет
    CHECK(a(250, 150) == 0);                   // под серединой окна тени нет (её закрывает само окно)
    CHECK(a(2, 298) > 20);                     // …а у кромки есть: её видно в скруглённом углу
    size_t coloured = 0;
    for (DWORD p : px) if (p & 0x00FFFFFF) ++coloured;
    CHECK(coloured == 0);                      // чистый чёрный с альфой
    // У активного окна тень сильнее.
    std::vector<DWORD> pa; int aw = 0, ah = 0; RECT apad = {};
    BuildShadowBitmap(500, 300, true, false, 1.0, pa, aw, ah, apad);
    CHECK((int)(pa[(size_t)(300 + apad.top) * aw + (250 + apad.left)] >> 24) > a(250, 300) + 15);
    // Маленькое окно: источник превью не меньше допустимого системой, лишнее — прозрачные поля.
    BuildShadowBitmap(60, 20, false, false, 1.0, px, sw, sh, pad);
    CHECK(sw >= 220 && sh >= 110);
    CHECK(pad.right == sw - 60 - pad.left && pad.bottom == sh - 20 - pad.top);
    // Большое окно: уменьшенная картинка, поля согласованы с её размером.
    BuildShadowBitmap(3440, 1400, false, false, 1.0, px, sw, sh, pad);
    CHECK(sw < 3440 && (3440 + pad.left + pad.right) % sw == 0);
    CHECK((size_t)sw * sh <= 450000);
    // У окна с тёмным заголовком тень примерно вдвое сильнее — как рисует Windows.
    {
        std::vector<DWORD> pd; int dw = 0, dh = 0; RECT dpad = {};
        BuildShadowBitmap(500, 300, false, true, 1.0, pd, dw, dh, dpad);
        auto da = [&](int x, int y) { return (int)(pd[(size_t)(y + dpad.top) * dw + (x + dpad.left)] >> 24); };
        CHECK(abs(da(-1, 150) - 63) <= 2 && abs(da(250, 300) - 111) <= 3 && abs(da(250, -1) - 39) <= 2);
        BuildShadowBitmap(500, 300, true, true, 1.0, pd, dw, dh, dpad);
        CHECK(abs(da(-1, 150) - 91) <= 2 && abs(da(250, 300) - 151) <= 3);
        const std::vector<ShadowLayer>& DL = ShadowLayers(false, true);
        double keep = 1.0;
        for (auto& l : DL) if (l.db >= 1) keep *= (1.0 - l.alpha);
        CHECK(fabs((1.0 - keep) * 255.0 - 111) <= 2.5);
    }
    // Та же тень контурами (для Direct2D): суммарное затемнение у кромок — как в профиле.
    for (int act = 0; act < 2; ++act) {
        const std::vector<ShadowLayer>& L = ShadowLayers(act != 0, false);
        auto total = [&](int side, float d) {      // 0 сбоку, 1 сверху, 2 снизу; d — px от рамки
            double keep = 1.0;
            for (auto& l : L) { float reach = side == 0 ? l.dl : side == 1 ? l.dt : l.db; if (reach >= d) keep *= (1.0 - l.alpha); }
            return (1.0 - keep) * 255.0;
        };
        const double side1 = act ? 41 : 28, top1 = act ? 27 : 16, bot1 = act ? 78 : 56, bot31 = act ? 34 : 22;
        CHECK(fabs(total(0, 1) - side1) <= 2.5);
        CHECK(fabs(total(1, 1) - top1) <= 2.5);
        CHECK(fabs(total(2, 1) - bot1) <= 2.5);
        CHECK(fabs(total(2, 31) - bot31) <= 2.5);
        CHECK(total(0, 60) < 1.0 && total(2, 90) < 1.0);          // вдали тени нет
    }
    // Масштаб 200 %: тень вдвое шире.
    BuildShadowBitmap(500, 300, false, false, 2.0, px, sw, sh, pad);
    CHECK(pad.left == 80 && pad.top == 26);
}

// ---------- Перетаскивание окна в обзоре ----------
static void TestOverviewDrag() {
    Section(L"dragging a window in the overview follows the cursor exactly");
    AppState app;
    g_app = &app;
    WNDCLASSW c = {};
    c.lpfnWndProc = OvProc; c.hInstance = GetModuleHandleW(nullptr); c.lpszClassName = L"InfiniteDesktopTestOverview";
    RegisterClassW(&c);
    app.ov = CreateWindowExW(WS_EX_TOOLWINDOW, L"InfiniteDesktopTestOverview", L"", WS_POPUP, 0, 0, 2000, 1200,
                             nullptr, nullptr, c.hInstance, nullptr);   // скрытое: нужны только сообщения
    HWND target = MakeTestWindow(L"drag me", false);
    app.wins.push_back(FakeWin(target, { 1000, 1000, 2000, 1800 }));
    app.overview = true;
    app.zoom = app.zoomTarget = 0.37;       // «неудобный» масштаб: 1 px курсора = 2,7 px мира
    app.axCX = app.axCY = 0; app.axWX = app.axWY = 0;   // клиент = мир * масштаб
    auto at = [](int x, int y) { return MAKELPARAM(x, y); };

    // Медленное перетаскивание шагами по 1 px: окно под курсором в (500,500).
    SendMessageW(app.ov, WM_LBUTTONDOWN, MK_LBUTTON, at(500, 500));
    CHECK(app.ovDragHwnd == target);
    for (int i = 1; i <= 120; ++i) SendMessageW(app.ov, WM_MOUSEMOVE, MK_LBUTTON, at(500 + i, 500 + i / 2));
    CHECK(app.ovDragMoved);                 // раньше медленное перетаскивание не начиналось
    // Точка захвата осталась под курсором: сдвиг мира = сдвиг курсора / масштаб, без накопленной ошибки.
    CHECK(app.wins[0].world.left == 1000 + llround(120 / 0.37));
    CHECK(app.wins[0].world.top  == 1000 + llround(60 / 0.37));
    CHECK(app.wins[0].world.right - app.wins[0].world.left == 1000);   // размер не меняется
    // Туда и обратно — ровно в исходную точку (раньше остатки округления копились).
    for (int i = 120; i >= 0; --i) SendMessageW(app.ov, WM_MOUSEMOVE, MK_LBUTTON, at(500 + i, 500 + i / 2));
    CHECK(app.wins[0].world.left == 1000 && app.wins[0].world.top == 1000);
    app.ovLastZoom = 0.37;
    SendMessageW(app.ov, WM_MOUSEMOVE, MK_LBUTTON, at(540, 500));
    SendMessageW(app.ov, WM_LBUTTONUP, 0, at(540, 500));
    CHECK(app.ovDragHwnd == nullptr);
    CHECK(app.ovLastZoom == -1);            // после отпускания кадр перерисуется с последним движением
    CHECK(app.wins[0].world.left == 1000 + llround(40 / 0.37));

    // Дрожание в пределах порога — это клик, а не перетаскивание: окно не двигается.
    RECT before = app.wins[0].world;
    DestroyWindow(target);                  // клик по закрытому окну ничего не делает — проверяем только сдвиг
    SendMessageW(app.ov, WM_LBUTTONDOWN, MK_LBUTTON, at(600, 500));
    SendMessageW(app.ov, WM_MOUSEMOVE, MK_LBUTTON, at(602, 501));
    CHECK(!app.ovDragMoved);
    SendMessageW(app.ov, WM_LBUTTONUP, 0, at(602, 501));
    CHECK(EqualRect(&before, &app.wins[0].world));

    app.wins.clear();
    DestroyWindow(app.ov);
    g_app = nullptr;
}

// ---------- Снимки интерфейса в заданном масштабе ----------
static bool SavePng(AppState& app, HWND hwnd, const std::wstring& path) {
    RECT r; GetWindowRect(hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    ScopedDC mdc(CreateCompatibleDC(sdc));
    ReleaseDC(nullptr, sdc);
    void* bits = nullptr;
    ScopedBitmap dib(CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!mdc || !dib || !bits) return false;
    HGDIOBJ old = SelectObject(mdc, dib);
    BOOL ok = PrintWindow(hwnd, mdc, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mdc, old);
    if (!ok || !app.wicFactory) return false;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> enc;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(app.wicFactory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(app.wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    // WriteSource сам приводит формат: кодировщик PNG может выбрать не 32-битный, и запись
    // сырых 32-битных строк дала бы сдвинутую картинку.
    Microsoft::WRL::ComPtr<IWICBitmap> src;
    if (FAILED(app.wicFactory->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR,
                                                      (UINT)w * 4, (UINT)w * h * 4, (BYTE*)bits, &src))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetSize((UINT)w, (UINT)h);
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WriteSource(src.Get(), nullptr))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

static int Shots(float scale, const std::wstring& dir) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        AppState app;
        g_app = &app;
        InitLanguage();
        EnableDpiAwareness();
        UpdateVirtualScreen(app);
        InitGraphics(app);
        InitUi(app);
        app.forceScale = scale;
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW c = {};
        c.hInstance = hi; c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.lpfnWndProc = HudProc;      c.lpszClassName = L"InfiniteDesktopTestHUD";      RegisterClassW(&c);
        c.lpfnWndProc = SettingsProc; c.lpszClassName = L"InfiniteDesktopTestSettings"; RegisterClassW(&c);
        app.hud = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
            L"InfiniteDesktopTestHUD", L"", WS_POPUP | WS_VISIBLE, 0, 0, MAP_W, MAP_H, nullptr, nullptr, hi, nullptr);
        SetLayeredWindowAttributes(app.hud, 0, 255, LWA_ALPHA);
        PlaceHud(app);
        app.settings = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"InfiniteDesktopTestSettings", Tr(S_SETTINGS_TITLE), WS_POPUP | WS_CAPTION | WS_SYSMENU,
            120, 120, SET_W, 480, nullptr, nullptr, hi, nullptr);
        InitDefaultKeys(app);
        app.keyFailed[HK_HOME] = true;                    // показать и состояние «занято»
        app.camX = app.targetX = 0; app.camY = app.targetY = 0;
        for (int i = 0; i < 4; ++i) {                     // несколько «окон» на карте
            app.wins.push_back(FakeWin(nullptr, { i * 1500 - 1800, (i % 2) * 900 - 300,
                                                  i * 1500 - 600,  (i % 2) * 900 + 500 }));
            app.wins.back().pinned = (i == 2);
        }
        app.previewsOn = false;
        RefreshHud(app);
        OpenSettings(app);
        Pump(600);
        bool a = SavePng(app, app.settings, dir + L"\\settings.png");
        bool b = SavePng(app, app.hud, dir + L"\\minimap.png");
        RECT sr, hr; GetWindowRect(app.settings, &sr); GetWindowRect(app.hud, &hr);
        wprintf(L"scale %.2f: settings %dx%d (%s), minimap %dx%d (%s)\n", scale,
                (int)(sr.right - sr.left), (int)(sr.bottom - sr.top), a ? L"saved" : L"FAILED",
                (int)(hr.right - hr.left), (int)(hr.bottom - hr.top), b ? L"saved" : L"FAILED");
        app.wins.clear();
        DestroyWindow(app.settings);
        DestroyWindow(app.hud);
        g_app = nullptr;
    }
    CoUninitialize();
    return 0;
}

int DummyMain(int delayUs, int x, int y, int w, int h, int flags);   // bench.cpp
int ShadowCalib(const std::wstring& outDir);
int Bench(int count, double seconds, int slowCount, int slowUs); // bench.cpp
int BenchModes(int count, double seconds, int slowCount, int slowUs);
int BenchGlide(int count, double seconds);
int BenchReal(const std::wstring& outDir);

int wmain(int argc, wchar_t** argv) {
    if (argc >= 7 && !lstrcmpW(argv[1], L"--dummy"))
        return DummyMain(_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]), _wtoi(argv[6]), argc > 7 ? _wtoi(argv[7]) : 0);
    if (argc == 3 && !lstrcmpW(argv[1], L"--shadow-calib")) return ShadowCalib(argv[2]);
    if (argc >= 2 && !lstrcmpW(argv[1], L"--bench-real")) return BenchReal(argc > 2 ? argv[2] : L"");
    if (argc >= 2 && !lstrcmpW(argv[1], L"--bench-glide"))   // плавная панорама при разной доле рабочего потока
        return BenchGlide(argc > 2 ? _wtoi(argv[2]) : 24, argc > 3 ? _wtof(argv[3]) : 2.5);
    if (argc >= 2 && !lstrcmpW(argv[1], L"--bench-modes"))   // сравнение способов перемещения окон
        return BenchModes(argc > 2 ? _wtoi(argv[2]) : 12, argc > 3 ? _wtof(argv[3]) : 3.0,
                          argc > 4 ? _wtoi(argv[4]) : 0, argc > 5 ? _wtoi(argv[5]) : 0);
    if (argc >= 2 && !lstrcmpW(argv[1], L"--bench"))   // --bench [windows] [seconds] [slow windows] [slow microseconds]
        return Bench(argc > 2 ? _wtoi(argv[2]) : 12, argc > 3 ? _wtof(argv[3]) : 4.0,
                     argc > 4 ? _wtoi(argv[4]) : 0, argc > 5 ? _wtoi(argv[5]) : 0);
    if (argc == 4 && !lstrcmpW(argv[1], L"--shot")) return Shots((float)_wtof(argv[2]), argv[3]);
    UseTempConfigDir();
    TestParkPosition();
    TestPickFocusTarget();
    TestMatchSavedLayout();
    TestLayoutFile();
    TestHotkeysFile();
    TestMarksFile();
    TestSessionFile();
    TestWheelInput();
    TestOverviewDrag();
    TestShadowBitmap();
    TestWindowKeepsPlace();
    RemoveTempConfigDir();
    wprintf(L"\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
