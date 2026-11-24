#pragma once
#include <windows.h>
#include <cstdint>

// The caller sets the fixed-function pipeline and restores the game's Direct3D state.
namespace AssistPanel {
constexpr int width = 512, height = 446;
constexpr int textureHeight = 512;
constexpr int firstRowTop = 77, rowStep = 38, rowHeight = 32;
constexpr int rangeRow = 6, rowCount = rangeRow + 1;
constexpr int rangeTop = firstRowTop + rowStep * rangeRow, rangeHeight = 66;
constexpr int rangeLeft = 40, rangeRight = 472;
constexpr int rangeTrack = rangeTop + 47;

inline int rangePosition(int step) {
    return rangeLeft + (rangeRight - rangeLeft) * step / lastEspDistance;
}

inline int rangeStepAt(float x) {
    const float step = (x - rangeLeft) * lastEspDistance / (rangeRight - rangeLeft);
    return std::clamp(static_cast<int>(step + 0.5f), 0, lastEspDistance);
}

inline int hitRow(float x, float y) {
    const float relativeY = y - firstRowTop;
    if (x < 16 || x >= width - 16 || relativeY < 0 || y >= rangeTop + rangeHeight) return -1;
    if (y >= rangeTop) return rangeRow;
    const int pixel = static_cast<int>(relativeY);
    return pixel % rowStep < rowHeight ? pixel / rowStep : -1;
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

    bool paint(int selected, const AssistSettings& settings, bool espVisible) {
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
        text(24, 51, 485, espVisible ? L"НАСТРОЙКИ ЧИТА" : L"ESP ПРИОСТАНОВЛЕН (F6)", muted);

        const wchar_t* labels[] = {L"Рамки", L"Кости", L"Хитбокс головы", L"Без разброса", L"Цвет подсветки", L"Полоска здоровья"};
        const wchar_t* colors[] = {L"Красный", L"Бирюзовый", L"Жёлтый"};
        const auto state = [](bool on) { return on ? L"Вкл." : L"Выкл."; };
        const wchar_t* values[] = {state(settings.boxes), state(settings.skeleton), state(settings.head),
                                  state(settings.noSpread), colors[settings.color], state(settings.health)};
        for (int row = 0; row < rangeRow; ++row) {
            const int top = firstRowTop + rowStep * row;
            fill({16, top, width - 16, top + rowHeight}, row == selected ? RGB(48, 67, 74) : RGB(34, 39, 46));
            if (row == selected) fill({16, top, 19, top + rowHeight}, accent);
            text(29, top + 5, 484, labels[row], white);
            text(240, top + 5, 484, values[row], accent, true);
        }
        fill({16, rangeTop, width - 16, rangeTop + rangeHeight},
             selected == rangeRow ? RGB(48, 67, 74) : RGB(34, 39, 46));
        if (selected == rangeRow) fill({16, rangeTop, 19, rangeTop + rangeHeight}, accent);
        text(29, rangeTop + 7, 484, L"Дальность ESP", white);
        text(240, rangeTop + 7, 484, espDistances[settings.espDistance].label, accent, true);
        const int marker = rangePosition(settings.espDistance);
        fill({rangeLeft, rangeTrack - 2, rangeRight, rangeTrack + 2}, RGB(99, 112, 123));
        if (marker > rangeLeft) fill({rangeLeft, rangeTrack - 2, marker, rangeTrack + 2}, accent);
        for (int index = 0; index <= lastEspDistance; ++index) {
            const int x = rangePosition(index);
            fill({x - 1, rangeTrack - 4, x + 2, rangeTrack + 4}, index <= settings.espDistance ? accent : muted);
        }
        fill({marker - 6, rangeTrack - 8, marker + 7, rangeTrack + 8}, accent);

        text(24, 386, 490, L"Мышь — изменить     ·     Insert — закрыть", muted);
        text(24, 412, 490, L"PgUp / PgDn — выбор     ·     Home / End — изменить", muted);
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

    HRESULT draw(void* device, float left, float top, float scale, int row, const AssistSettings& settings, bool espVisible) {
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
        if (dirty_ && !paint(row, settings, espVisible)) return E_FAIL;
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
