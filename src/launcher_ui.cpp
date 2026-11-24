#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <string>
#include <memory>
#include "launcher_ui.hpp"

namespace {
enum ControlId { ResolutionId = 100, DisplayId, BoxesId, SpreadId, ColorId, PlayId, BonesId, HeadId, TrainerId, InstallFixId, HealthId };
constexpr UINT launchFinished = WM_APP + 1, trainerStatusChanged = WM_APP + 2, installFinished = WM_APP + 3;
constexpr COLORREF background = RGB(23, 26, 31);
constexpr COLORREF surface = RGB(34, 39, 46);
constexpr COLORREF foreground = RGB(234, 239, 244);
constexpr COLORREF muted = RGB(175, 188, 201);
constexpr COLORREF accent = RGB(129, 214, 206);

struct Launcher {
    HINSTANCE instance;
    LaunchGame launch;
    InstallFix install;
    wchar_t profile[MAX_PATH]{};
    HWND window{}, resolution{}, display{}, boxes{}, bones{}, head{}, health{}, spread{}, color{}, play{}, trainer{}, installFix{}, status{};
    HANDLE cancel{};
    HFONT body{}, title{}, section{};
    HBRUSH backdrop{}, inputBackground{};
    int dpi = 96;
    bool launching = false, installing = false, closePending = false;
};

int scale(const Launcher& ui, int value) { return MulDiv(value, ui.dpi, 96); }

HWND control(Launcher& ui, const wchar_t* type, const wchar_t* caption, DWORD style,
             int x, int y, int width, int height, int id = 0, HFONT font = nullptr) {
    HWND result = CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style,
        scale(ui, x), scale(ui, y), scale(ui, width), scale(ui, height), ui.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ui.instance, nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : ui.body), TRUE);
    return result;
}

void fillCombo(HWND combo, const wchar_t* const* items, size_t count, int selected) {
    for (size_t index = 0; index < count; ++index) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[index]));
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void createControls(Launcher& ui) {
    ui.dpi = static_cast<int>(GetDpiForWindow(ui.window));
    const auto makeFont = [&](int pixels, int weight) {
        return CreateFontW(-scale(ui, pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
    };
    ui.body = makeFont(15, FW_NORMAL);
    ui.title = makeFont(30, FW_SEMIBOLD);
    ui.section = makeFont(16, FW_SEMIBOLD);
    ui.backdrop = CreateSolidBrush(background);
    ui.inputBackground = CreateSolidBrush(surface);
    BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(ui.window, 20, &darkTitle, sizeof(darkTitle));
    const auto saved = readSettings(ui.profile);
    constexpr DWORD comboStyle = WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS;
    control(ui, L"STATIC", L"Max Payne 2", 0, 30, 22, 420, 40, 0, ui.title);
    control(ui, L"STATIC", L"Steam (RU) · MP2Assist + Widescreen Fix", 0, 32, 67, 445, 26);
    control(ui, L"STATIC", L"Экран", 0, 32, 111, 200, 26, 0, ui.section);
    control(ui, L"STATIC", L"Разрешение", 0, 32, 145, 210, 21);
    control(ui, L"STATIC", L"Режим", 0, 255, 145, 210, 21);
    ui.resolution = control(ui, L"COMBOBOX", L"", comboStyle, 32, 169, 201, 150, ResolutionId);
    ui.display = control(ui, L"COMBOBOX", L"", comboStyle, 255, 169, 201, 150, DisplayId);

    ui.installFix = control(ui, L"BUTTON", L"Установить Widescreen Fix", BS_OWNERDRAW | WS_TABSTOP,
                            32, 210, 424, 38, InstallFixId, ui.section);
    control(ui, L"STATIC", L"Настройки чита", 0, 32, 279, 200, 26, 0, ui.section);
    auto toggle = [&](const wchar_t* text, int x, int y, int width, int id, bool checked) {
        HWND handle = control(ui, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width, 27, id);
        SendMessageW(handle, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        return handle;
    };
    ui.boxes = toggle(L"Рамки", 32, 315, 140, BoxesId, saved.boxes);
    ui.bones = toggle(L"Кости", 173, 315, 105, BonesId, saved.skeleton);
    ui.head = toggle(L"Хитбокс головы", 292, 315, 164, HeadId, saved.head);
    ui.health = toggle(L"Полоска здоровья", 32, 350, 201, HealthId, saved.health);
    ui.spread = toggle(L"Отключить разброс", 32, 387, 201, SpreadId, saved.noSpread);
    control(ui, L"STATIC", L"Цвет подсветки", 0, 255, 353, 200, 21);
    ui.color = control(ui, L"COMBOBOX", L"", comboStyle, 255, 377, 201, 150, ColorId);

    const wchar_t* resolutionItems[] = {L"3440 × 1440 (21:9)", L"2560 × 1080 (21:9)", L"1920 × 1080 (16:9)"};
    const wchar_t* displayItems[] = {L"Окно без рамки", L"Окно с рамкой", L"Полный экран"};
    const wchar_t* colorItems[] = {L"Красный", L"Бирюзовый", L"Жёлтый"};
    fillCombo(ui.resolution, resolutionItems, std::size(resolutionItems), saved.resolution);
    fillCombo(ui.display, displayItems, std::size(displayItems), saved.displayMode);
    fillCombo(ui.color, colorItems, std::size(colorItems), saved.color);
    control(ui, L"STATIC", L"Insert — открыть или закрыть меню в игре", 0, 32, 426, 430, 25);
    ui.play = control(ui, L"BUTTON", L"Играть через Steam", BS_OWNERDRAW | WS_TABSTOP,
                      32, 469, 209, 46, PlayId, ui.section);
    ui.trainer = control(ui, L"BUTTON", L"Ждать запуска", BS_OWNERDRAW | WS_TABSTOP,
                         254, 469, 202, 46, TrainerId, ui.section);
    ui.status = control(ui, L"STATIC", L"", 0, 32, 532, 424, 46);
    SetFocus(ui.play);
}

int selected(HWND combo) { return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)); }

struct Work { HWND window; LaunchGame launch; AssistSettings settings; HANDLE cancel; };
DWORD WINAPI launchWorker(void* data) {
    std::unique_ptr<Work> task(static_cast<Work*>(data));
    auto error = std::make_unique<std::wstring>();
    const auto status = +[](void* context, bool attached) {
        PostMessageW(static_cast<HWND>(context), trainerStatusChanged, attached, 0);
    };
    task->launch(task->settings, *error, task->cancel, status, task->window);
    if (PostMessageW(task->window, launchFinished, 0, reinterpret_cast<LPARAM>(error.get()))) {
        error.release();
    }
    return 0;
}

struct InstallWork { HWND window; InstallFix install; };
DWORD WINAPI installWorker(void* data) {
    std::unique_ptr<InstallWork> task(static_cast<InstallWork*>(data));
    auto status = std::make_unique<std::wstring>();
    const bool installed = task->install(*status);
    if (PostMessageW(task->window, installFinished, installed, reinterpret_cast<LPARAM>(status.get()))) {
        status.release();
    }
    return 0;
}

void startInstall(Launcher& ui) {
    ui.installing = true;
    EnableWindow(ui.installFix, FALSE);
    EnableWindow(ui.play, FALSE);
    EnableWindow(ui.trainer, FALSE);
    SetWindowTextW(ui.status, L"Проверяю установку Widescreen Fix…");
    auto* work = new InstallWork{ui.window, ui.install};
    HANDLE worker = CreateThread(nullptr, 0, &installWorker, work, 0, nullptr);
    if (worker) {
        CloseHandle(worker);
    } else {
        delete work;
        ui.installing = false;
        EnableWindow(ui.installFix, TRUE);
        EnableWindow(ui.play, TRUE);
        EnableWindow(ui.trainer, TRUE);
        SetWindowTextW(ui.status, L"Не удалось запустить установщик.");
    }
}

void submit(Launcher& ui, bool trainer) {
    if (ui.installing) return;
    if (ui.launching) {
        if (trainer && ui.cancel) SetEvent(ui.cancel);
        return;
    }
    const auto checked = [](HWND field) { return SendMessageW(field, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    const AssistSettings settings{selected(ui.resolution), selected(ui.display), checked(ui.boxes),
        checked(ui.bones), checked(ui.head), checked(ui.health), checked(ui.spread), selected(ui.color)};
    const bool saved = writeSetting(ui.profile, L"Resolution", settings.resolution) &&
        writeSetting(ui.profile, L"DisplayMode", settings.displayMode) &&
        writeSetting(ui.profile, L"ESPBoxes", settings.boxes) &&
        writeSetting(ui.profile, L"ESPSkeleton", settings.skeleton) &&
        writeSetting(ui.profile, L"ESPHead", settings.head) &&
        writeSetting(ui.profile, L"ESPHealth", settings.health) &&
        writeSetting(ui.profile, L"NoSpread", settings.noSpread) &&
        writeSetting(ui.profile, L"Color", settings.color);
    if (!saved) {
        SetWindowTextW(ui.status, L"Не удалось сохранить настройки рядом с запускатором.");
        return;
    }
    if (trainer) {
        ui.cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ui.cancel) {
            SetWindowTextW(ui.status, L"Не удалось включить режим ожидания.");
            return;
        }
    }
    ui.launching = true;
    EnableWindow(ui.play, FALSE);
    EnableWindow(ui.installFix, FALSE);
    EnableWindow(ui.trainer, trainer);
    if (trainer) {
        SetWindowTextW(ui.trainer, L"Остановить ожидание");
        SetWindowTextW(ui.status, L"Жду Max Payne 2 (RU). Запусти игру в Steam.");
    } else {
        SetWindowTextW(ui.status, L"Запускаю игру через Steam…");
    }

    auto* work = new Work{ui.window, ui.launch, settings, ui.cancel};
    HANDLE thread = CreateThread(nullptr, 0, &launchWorker, work, 0, nullptr);
    if (thread) CloseHandle(thread);
    else {
        delete work;
        if (ui.cancel) CloseHandle(ui.cancel);
        ui.cancel = nullptr;
        ui.launching = false;
        EnableWindow(ui.play, TRUE);
        EnableWindow(ui.installFix, TRUE);
        EnableWindow(ui.trainer, TRUE);
        SetWindowTextW(ui.trainer, L"Ждать запуска");
        SetWindowTextW(ui.status, L"Не удалось запустить обработчик Steam.");
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* ui = static_cast<Launcher*>(create->lpCreateParams);
        ui->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    auto* ui = reinterpret_cast<Launcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!ui) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE:
        createControls(*ui);
        return 0;
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, ui->backdrop);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        const auto child = reinterpret_cast<HWND>(lParam);
        const bool list = message == WM_CTLCOLORLISTBOX;
        SetTextColor(dc, child == ui->status ? accent : foreground);
        SetBkColor(dc, list ? surface : background);
        return reinterpret_cast<LRESULT>(list ? ui->inputBackground : ui->backdrop);
    }
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID != PlayId && item->CtlID != TrainerId && item->CtlID != InstallFixId) break;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool primary = item->CtlID == PlayId;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const auto brush = CreateSolidBrush(disabled ? surface : primary
            ? (pressed ? RGB(93, 181, 173) : accent)
            : (pressed ? RGB(50, 65, 74) : surface));
        FillRect(item->hDC, &item->rcItem, brush);
        DeleteObject(brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, disabled ? muted : primary ? background : foreground);
        SelectObject(item->hDC, ui->section);
        wchar_t caption[80];
        GetWindowTextW(item->hwndItem, caption, static_cast<int>(std::size(caption)));
        DrawTextW(item->hDC, caption, -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (item->itemState & ODS_FOCUS) {
            InflateRect(&item->rcItem, -4, -4);
            DrawFocusRect(item->hDC, &item->rcItem);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == InstallFixId && !ui->launching && !ui->installing) {
            startInstall(*ui);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && (LOWORD(wParam) == PlayId || LOWORD(wParam) == TrainerId)) {
            submit(*ui, LOWORD(wParam) == TrainerId);
        }
        return 0;
    case installFinished: {
        std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lParam));
        ui->installing = false;
        EnableWindow(ui->installFix, TRUE);
        EnableWindow(ui->play, TRUE);
        EnableWindow(ui->trainer, TRUE);
        if (wParam) SetWindowTextW(ui->installFix, L"Widescreen Fix установлен");
        SetWindowTextW(ui->status, status->c_str());
        if (ui->closePending) DestroyWindow(window);
        return 0;
    }
    case trainerStatusChanged:
        if (ui->cancel) {
            SetWindowTextW(ui->status, wParam ? L"Трейнер подключён. Insert открывает меню в игре."
                                              : L"Жду Max Payne 2 (RU). Запусти игру в Steam.");
        }
        return 0;
    case launchFinished: {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
        const bool wasTrainer = ui->cancel != nullptr;
        if (wasTrainer) CloseHandle(ui->cancel);
        ui->cancel = nullptr;
        ui->launching = false;
        EnableWindow(ui->play, TRUE);
        EnableWindow(ui->installFix, TRUE);
        EnableWindow(ui->trainer, TRUE);
        SetWindowTextW(ui->trainer, L"Ждать запуска");
        SetWindowTextW(ui->status, error->empty() ?
            (wasTrainer ? L"Ожидание остановлено." : L"Игра запущена. Нажми Insert для меню чита.") : error->c_str());
        if (ui->closePending) DestroyWindow(window);
        else if (!wasTrainer && error->empty()) ShowWindow(window, SW_MINIMIZE);
        return 0;
    }
    case WM_CLOSE:
        if (ui->installing) {
            ui->closePending = true;
            return 0;
        }
        if (ui->cancel) {
            ui->closePending = true;
            SetEvent(ui->cancel);
            return 0;
        }
        if (ui->launching) return 0;
        break;
    case WM_DESTROY:
        DeleteObject(ui->body);
        DeleteObject(ui->title);
        DeleteObject(ui->section);
        DeleteObject(ui->backdrop);
        DeleteObject(ui->inputBackground);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int showLauncher(HINSTANCE instance, LaunchGame launch, InstallFix install) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    Launcher ui{instance, launch, install};
    if (!siblingPath(nullptr, L"MP2Assist.ini", ui.profile)) return 1;
    WNDCLASSW klass{};
    klass.lpfnWndProc = windowProc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.lpszClassName = L"MP2AssistLauncher";
    if (!RegisterClassW(&klass)) return 1;

    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const UINT dpi = GetDpiForSystem();
    RECT frame{0, 0, MulDiv(488, dpi, 96), MulDiv(592, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND window = CreateWindowExW(0, klass.lpszClassName, L"Max Payne 2 · Запуск", style,
        (work.left + work.right - width) / 2, (work.top + work.bottom - height) / 2,
        width, height, nullptr, nullptr, instance, &ui);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
