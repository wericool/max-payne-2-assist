#pragma once
#include <windows.h>
#include <cstdint>

// The caller sets the fixed-function pipeline and restores the game's Direct3D state.
namespace AssistPanel {
constexpr int width = 512, height = 385;
constexpr int textureHeight = 512;
constexpr int tabTop = 51, tabBottom = 84, tabDivider = width / 2;
constexpr int firstRowTop = 94, rowStep = 34, rowHeight = 30;
constexpr int painkillerRow = 6, ammoRow = 7, weaponRow = 8, explosiveRow = 9;
constexpr int enemyRangeRow = 10, itemRangeRow = 11;
constexpr int rangeHeight = 55;
constexpr int rangeLeft = 40, rangeRight = 472;

enum class Tab { enemies, items };
inline int firstRow(Tab tab) { return tab == Tab::enemies ? 0 : painkillerRow; }
inline int endRow(Tab tab) { return tab == Tab::enemies ? painkillerRow : explosiveRow + 1; }
inline int rangeRow(Tab tab) { return tab == Tab::enemies ? enemyRangeRow : itemRangeRow; }
inline bool isRangeRow(int row) { return row == enemyRangeRow || row == itemRangeRow; }
inline int rowTop(int row) { return firstRowTop + rowStep * (row < painkillerRow ? row : row - painkillerRow); }
inline int rangeTop(int row) { return rowTop((row == enemyRangeRow ? painkillerRow : explosiveRow + 1) - 1) + rowStep; }
inline int rangeTrack(int row) { return rangeTop(row) + 41; }

inline int rangePosition(int step) {
    return rangeLeft + (rangeRight - rangeLeft) * step / lastEspDistance;
}

inline int rangeStepAt(float x) {
    const float step = (x - rangeLeft) * lastEspDistance / (rangeRight - rangeLeft);
    return std::clamp(static_cast<int>(step + 0.5f), 0, lastEspDistance);
}

inline bool overTabs(float x, float y) {
    return x >= 16 && x < width - 16 && y >= tabTop && y < tabBottom;
}

inline int hitRow(Tab tab, float x, float y) {
    const float relativeY = y - firstRowTop;
    if (x < 16 || x >= width - 16 || relativeY < 0) return -1;
    const int pixel = static_cast<int>(relativeY);
    const int row = firstRow(tab) + pixel / rowStep;
    if (row < endRow(tab)) return pixel % rowStep < rowHeight ? row : -1;
    const int distance = rangeRow(tab);
    if (y < rangeTop(distance) + rangeHeight) return distance;
    return -1;
}
struct TextureVertex { float x, y, z, rhw, u, v; };
struct LockedRect { int pitch; void* bits; };

template<class Function> Function method(void* object, unsigned slot) {
    return reinterpret_cast<Function>((*reinterpret_cast<void***>(object))[slot]);
}

class Panel {
    void* texture_ = nullptr;
    void* device_ = nullptr;
    bool dirty_ = true;

    bool paint(Tab tab, int selected, const AssistSettings& settings) {
        using Lock = HRESULT (WINAPI*)(void*, UINT, LockedRect*, const RECT*, DWORD);
        using Unlock = HRESULT (WINAPI*)(void*, UINT);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = width;
        bitmap.bmiHeader.biHeight = -textureHeight;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dib) {
            DeleteDC(dc);
            return false;
        }
        const auto oldBitmap = SelectObject(dc, dib);
        const auto fill = [dc](RECT rect, COLORREF color) {
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
        };
        const auto font = [](int size, int weight) {
            return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        };
        HFONT titleFont = font(26, FW_SEMIBOLD);
        HFONT regularFont = font(17, FW_NORMAL);
        const auto text = [dc](int x, int y, int right, const wchar_t* caption, COLORREF color, bool alignRight = false) {
            RECT rect{x, y, right, y + 38};
            SetTextColor(dc, color);
            DrawTextW(dc, caption, -1, &rect, DT_TOP | DT_SINGLELINE | (alignRight ? DT_RIGHT : DT_LEFT));
        };
        constexpr auto white = RGB(233, 239, 244);
        constexpr auto accent = RGB(129, 214, 206);
        constexpr auto muted = RGB(175, 188, 201);
        fill({0, 0, width, textureHeight}, RGB(23, 26, 31));
        fill({0, 0, width, 3}, accent);
        SetBkMode(dc, TRANSPARENT);
        const auto oldFont = SelectObject(dc, titleFont);
        text(24, 16, 485, L"MP2Assist", white);
        SelectObject(dc, regularFont);
        const bool enemy = tab == Tab::enemies;
        fill({16, tabTop, tabDivider - 2, tabBottom}, enemy ? RGB(48, 67, 74) : RGB(34, 39, 46));
        fill({tabDivider + 2, tabTop, width - 16, tabBottom}, enemy ? RGB(34, 39, 46) : RGB(48, 67, 74));
        const int underlineLeft = enemy ? 16 : tabDivider + 2;
        const int underlineRight = enemy ? tabDivider - 2 : width - 16;
        fill({underlineLeft, tabBottom - 2, underlineRight, tabBottom}, accent);
        text(29, tabTop + 6, tabDivider - 8, L"Враги", enemy ? white : muted);
        text(tabDivider + 15, tabTop + 6, width - 24, L"Предметы", enemy ? muted : white);

        const wchar_t* labels[] = {L"Рамки", L"Кости", L"Хитбокс головы", L"Без разброса", L"Цвет подсветки",
            L"Полоска здоровья", L"Обезболивающие", L"Патроны", L"Оружие", L"Взрывные предметы"};
        const wchar_t* colors[] = {L"Красный", L"Бирюзовый", L"Жёлтый"};
        const auto state = [](bool on) { return on ? L"Вкл." : L"Выкл."; };
        const wchar_t* values[] = {state(settings.boxes), state(settings.skeleton), state(settings.head),
            state(settings.noSpread), colors[settings.color], state(settings.health), state(settings.painkillers),
            state(settings.ammo), state(settings.weapons), state(settings.explosives)};
        const auto rowBackground = [&](int top, int backgroundHeight, int row) {
            fill({16, top, width - 16, top + backgroundHeight}, row == selected ? RGB(48, 67, 74) : RGB(34, 39, 46));
            if (row == selected) fill({16, top, 19, top + backgroundHeight}, accent);
        };
        for (int row = firstRow(tab); row < endRow(tab); ++row) {
            const int top = rowTop(row);
            rowBackground(top, rowHeight, row);
            text(29, top + 3, 484, labels[row], white);
            text(240, top + 3, 484, values[row], accent, true);
        }
        {
            const int row = rangeRow(tab);
            const int top = rangeTop(row), track = rangeTrack(row);
            const int selectedStep = enemy ? settings.espDistance : settings.itemDistance;
            rowBackground(top, rangeHeight, row);
            text(29, top + 5, 484, enemy ? L"Дальность врагов" : L"Дальность предметов", white);
            text(240, top + 5, 484, espDistances[selectedStep].label, accent, true);
            const int marker = rangePosition(selectedStep);
            fill({rangeLeft, track - 2, rangeRight, track + 2}, RGB(99, 112, 123));
            if (marker > rangeLeft) fill({rangeLeft, track - 2, marker, track + 2}, accent);
            for (int index = 0; index <= lastEspDistance; ++index) {
                const int x = rangePosition(index);
                fill({x - 1, track - 4, x + 2, track + 4}, index <= selectedStep ? accent : muted);
            }
            fill({marker - 6, track - 8, marker + 7, track + 8}, accent);
        }
        text(24, 358, 490, L"Мышь — изменить     ·     Insert — закрыть", muted);
        GdiFlush();

        LockedRect lock{};
        const bool locked = SUCCEEDED(method<Lock>(texture_, 16)(texture_, 0, &lock, nullptr, 0));
        if (locked) {
            for (int y = 0; y < textureHeight; ++y) {
                std::memcpy(static_cast<BYTE*>(lock.bits) + y * lock.pitch,
                            static_cast<BYTE*>(pixels) + y * width * 4, width * 4);
            }
            method<Unlock>(texture_, 17)(texture_, 0);
        }

        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(titleFont);
        DeleteObject(regularFont);
        DeleteObject(dib);
        DeleteDC(dc);
        return locked;
    }

public:
    void invalidate() { dirty_ = true; }

    void clear() {
        if (texture_) method<ULONG(WINAPI*)(void*)>(texture_, 2)(texture_);
        texture_ = nullptr;
        device_ = nullptr;
        dirty_ = true;
    }

    HRESULT draw(void* device, float left, float top, float scale, Tab tab, int row, const AssistSettings& settings) {
        using Create = HRESULT (WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
        using State = HRESULT (WINAPI*)(void*, DWORD);
        using SetTexture = HRESULT (WINAPI*)(void*, DWORD, void*);
        using Stage = HRESULT (WINAPI*)(void*, DWORD, DWORD, DWORD);
        using Draw = HRESULT (WINAPI*)(void*, DWORD, UINT, const void*, UINT);
        if (device_ != device) clear();
        if (!texture_) {
            const auto result = method<Create>(device, 20)(device, width, textureHeight, 1, 0, 22, 1, &texture_);
            if (FAILED(result)) return result; // D3DFMT_X8R8G8B8, D3DPOOL_MANAGED
            device_ = device;
        }
        if (dirty_ && !paint(tab, row, settings)) return E_FAIL;
        dirty_ = false;

        method<State>(device, 76)(device, 0x104); // D3DFVF_XYZRHW | D3DFVF_TEX1
        method<SetTexture>(device, 61)(device, 0, texture_);
        auto stage = method<Stage>(device, 63);
        stage(device, 0, 2, 2); // COLORARG1 = TEXTURE
        stage(device, 0, 5, 2); // ALPHAARG1 = TEXTURE
        stage(device, 0, 16, 2); // MAGFILTER = LINEAR
        stage(device, 0, 17, 2); // MINFILTER = LINEAR
        const float right = left + width * scale, bottom = top + height * scale;
        const TextureVertex vertices[] = {
            {left, top, 0, 1, 0, 0}, {right, top, 0, 1, 1, 0},
            {left, bottom, 0, 1, 0, float(height) / textureHeight},
            {right, bottom, 0, 1, 1, float(height) / textureHeight},
        };
        return method<Draw>(device, 72)(device, 5, 2, vertices, sizeof(TextureVertex)); // D3DPT_TRIANGLESTRIP
    }
};
}
