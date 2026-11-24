#pragma once
#include <windows.h>
#include <algorithm>
#include <cwchar>

struct EspDistance { int meters; const wchar_t* label; };
inline constexpr EspDistance espDistances[] = {
    {5, L"5 м"}, {10, L"10 м"}, {20, L"20 м"}, {30, L"30 м"},
    {50, L"50 м"}, {70, L"70 м"}, {100, L"100 м"}, {0, L"Вся карта"},
};
inline constexpr int lastEspDistance = static_cast<int>(sizeof(espDistances) / sizeof(espDistances[0])) - 1;

struct AssistSettings {
    int resolution = 0;
    int displayMode = 0;
    bool boxes = true;
    bool skeleton = true;
    bool head = true;
    bool health = true;
    bool noSpread = true;
    int color = 0;
    int espDistance = lastEspDistance;
    int itemDistance = lastEspDistance;
    bool painkillers = true;
    bool ammo = true;
    bool weapons = true;
    bool explosives = true;
};

struct Resolution { DWORD width, height; };
inline constexpr Resolution resolutions[] = {{3440, 1440}, {2560, 1080}, {1920, 1080}};
inline constexpr wchar_t profileSection[] = L"MP2Assist";

inline bool siblingPath(HMODULE module, const wchar_t* filename, wchar_t (&path)[MAX_PATH]) {
    const DWORD size = GetModuleFileNameW(module, path, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    return slash && wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), filename) == 0;
}

inline AssistSettings readSettings(const wchar_t* path) {
    const auto read = [path](const wchar_t* field, int fallback, int max) {
        const UINT value = GetPrivateProfileIntW(profileSection, field, fallback, path);
        return value <= static_cast<UINT>(max) ? static_cast<int>(value) : fallback;
    };
    const int oldEsp = read(L"ESP", 2, 2);
    return {read(L"Resolution", 0, 2), read(L"DisplayMode", 0, 2),
            read(L"ESPBoxes", oldEsp != 0, 1) != 0, read(L"ESPSkeleton", oldEsp == 2, 1) != 0,
            read(L"ESPHead", oldEsp != 0, 1) != 0, read(L"ESPHealth", 1, 1) != 0,
            read(L"NoSpread", 1, 1) != 0,
            read(L"Color", 0, 2), read(L"ESPDistance", lastEspDistance, lastEspDistance),
            read(L"ESPItemDistance", lastEspDistance, lastEspDistance),
            read(L"ESPPainkillers", 1, 1) != 0, read(L"ESPAmmo", 1, 1) != 0,
            read(L"ESPWeapons", 1, 1) != 0, read(L"ESPExplosives", 1, 1) != 0};
}

inline bool writeSetting(const wchar_t* path, const wchar_t* key, int value) {
    wchar_t text[16];
    _itow_s(value, text, 10);
    return WritePrivateProfileStringW(profileSection, key, text, path) != FALSE;
}
