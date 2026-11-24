#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <shellapi.h>
#include <cstdio>
#include "launcher_ui.hpp"
#include "widescreen.hpp"
#include "sha256.hpp"

namespace {
namespace fs = std::filesystem;


struct FileVersion { const wchar_t* file; const char* sha256; };
constexpr FileVersion supported[] = {
    {L"MaxPayne2.exe", "B1677BD72F47E1ED7ABDBF5441149867C53FF0E0C2D1C3EECB1815FB718263AB"},
    {L"X_GameObjectsMFC.dll", "98823044E9BA0E9DFD61C0008C1831A2372CFCCD7A287314CC93E50B30AE2628"},
    {L"X_LevelMFC.dll", "0C37C97E81D93619DB6F0A4B03947C4A05A4A45AEF50B5DF970EE4A58463CCD7"},
    {L"X_MessageMFC.dll", "0CB2B59EEC07C6FEC01E5390F2A06977C8BFD733005ECA87218477BAEC2649DE"},
    {L"X_LevelRuntimeMFC.dll", "42565F3579EAF1F2D592CD4B60DB9257D1BB1CC5D9F6DE17D4A57545C14252B3"},
    {L"X_PhysicalSimulationMFC.dll", "CD97CB986712F7F540E7AF83353F4980B6C7AFFC997BD657946B78EC3F77C99F"},
    {L"X_SharedDBMFC.dll", "02684E6465EA6097A50D01A48D785D98B3197DF1966F4CCB1D18E3B576375C54"},
    {L"KF2MFC.dll", "AE16AB1360D20C1478334E9D55B989B782BAF6DB8A3F5A67D95537F882B469EE"},
    {L"X_Inputmfc.dll", "196DD65447D6614C7EDE82FB6F4BF902712DF1DBFE15DF2589D99247DD69C27C"},
    {L"e2mfc.dll", "9107267FD91D101782F00689431D2FAAF7EC8E1984B1A37A189F9C26C09D4D72"},
    {L"e2driver/e2_d3d8_driver_mfc.dll", "9D72637C51F7A17DD9D95B079E5508ECBE8F1799C34395E0876945D741FF11AE"},
};

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

BYTE* remoteModule(DWORD process, const wchar_t* name) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process));
    if (snapshot.value == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.value, &entry)) return nullptr;
    do {
        if (_wcsicmp(entry.szModule, name) == 0) return entry.modBaseAddr;
    } while (Module32NextW(snapshot.value, &entry));
    return nullptr;
}

bool gameReady(DWORD pid) {
    if (!pid || !remoteModule(pid, L"e2_d3d8_driver_mfc.dll")) return false;
    const auto graphicsBase = reinterpret_cast<uintptr_t>(remoteModule(pid, L"e2mfc.dll"));
    if (!graphicsBase) return false;
    Handle game(OpenProcess(PROCESS_VM_READ, FALSE, pid));
    if (!game.value) return false;
    const auto readPointer = [&](uintptr_t address, uintptr_t& value) {
        return ReadProcessMemory(game.value, reinterpret_cast<const void*>(address), &value, sizeof(value), nullptr) != FALSE;
    };
    // P_Driver::m_initializedDriver and the Direct3D device for the verified x86 build.
    uintptr_t driver = 0, device = 0;
    return readPointer(graphicsBase + 0x62400, driver) && driver &&
           readPointer(driver + 0x100, device) && device;
}

bool remoteCall(HANDLE process, BYTE* entry, void* argument, DWORD& result) {
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry),
                                     argument, 0, nullptr));
    return thread.value && WaitForSingleObject(thread.value, 10000) == WAIT_OBJECT_0 &&
           GetExitCodeThread(thread.value, &result);
}

bool loadMod(HANDLE game, DWORD pid, const fs::path& dll, bool diagnostic) {
    const auto localLoadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!localLoadLibrary || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                 reinterpret_cast<LPCWSTR>(localLoadLibrary), &owner)) return false;
    wchar_t ownerPath[MAX_PATH]{};
    if (!GetModuleFileNameW(owner, ownerPath, MAX_PATH)) return false;
    const fs::path ownerName = fs::path(ownerPath).filename();
    BYTE* loadHost = remoteModule(pid, ownerName.c_str());
    if (!loadHost) return false;

    HMODULE localMod = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!localMod) return false;
    const auto localInitialize = GetProcAddress(localMod, "Initialize");
    if (!localInitialize) {
        FreeLibrary(localMod);
        return false;
    }
    const auto initOffset = reinterpret_cast<BYTE*>(localInitialize) - reinterpret_cast<BYTE*>(localMod);
    FreeLibrary(localMod);

    const auto dllName = dll.wstring();
    const auto size = (dllName.size() + 1) * sizeof(wchar_t);
    void* data = VirtualAllocEx(game, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!data) return false;
    const auto loadOffset = reinterpret_cast<BYTE*>(localLoadLibrary) - reinterpret_cast<BYTE*>(owner);
    DWORD remoteDll = 0;
    if (!WriteProcessMemory(game, data, dllName.c_str(), size, nullptr) ||
        !remoteCall(game, loadHost + loadOffset, data, remoteDll)) return false;
    VirtualFreeEx(game, data, 0, MEM_RELEASE);
    if (!remoteDll) return false;
    DWORD initialized = 0;
    return remoteCall(game, reinterpret_cast<BYTE*>(remoteDll) + initOffset, reinterpret_cast<void*>(static_cast<ULONG_PTR>(diagnostic)), initialized) &&
           initialized == 1;
}

DWORD findGame(const fs::path& expected) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return 0;
    do {
        if (_wcsicmp(entry.szExeFile, L"MaxPayne2.exe") != 0) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        wchar_t path[32768]{};
        DWORD capacity = static_cast<DWORD>(std::size(path));
        if (process.value && QueryFullProcessImageNameW(process.value, 0, path, &capacity) &&
            _wcsicmp(path, expected.c_str()) == 0) return entry.th32ProcessID;
    } while (Process32NextW(snapshot.value, &entry));
    return 0;
}

fs::path steamExecutable() {
    wchar_t steam[32768]{};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Valve\Steam)", L"SteamExe",
                     RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS) return {};
    return fs::path(steam).make_preferred();
}

fs::path gameInLibrary(const fs::path& library) {
    const auto apps = library / L"steamapps";
    if (!fs::is_regular_file(apps / L"appmanifest_12260.acf")) return {};
    auto game = (apps / L"common/Max Payne 2 Ru").make_preferred();
    return fs::is_regular_file(game / L"MaxPayne2.exe") ? game : fs::path{};
}

fs::path findSteamGame() {
    const auto steam = steamExecutable();
    if (steam.empty()) return {};
    const auto root = steam.parent_path();
    if (auto game = gameInLibrary(root); !game.empty()) return game;

    std::ifstream libraries(root / L"steamapps/libraryfolders.vdf");
    std::string line;
    while (std::getline(libraries, line)) {
        const auto key = line.find_first_not_of(" \t");
        constexpr char pathKey[] = "\"path\"";
        if (key == std::string::npos || line.compare(key, sizeof(pathKey) - 1, pathKey) != 0) continue;
        const auto start = line.find('"', key + sizeof(pathKey) - 1);
        const auto end = line.rfind('"');
        if (start == std::string::npos || end <= start) continue;
        std::string path;
        for (size_t index = start + 1; index < end; ++index) {
            if (line[index] == '\\' && index + 1 < end && line[index + 1] == '\\') ++index;
            path += line[index];
        }
        if (auto game = gameInLibrary(fs::u8path(path)); !game.empty()) return game;
    }
    return {};
}

bool startThroughSteam(bool diagnostic, bool skipDialog, const AssistSettings& settings) {
    const auto steam = steamExecutable();
    if (steam.empty()) return false;
    std::wstring command = L"\"" + steam.wstring() + L"\" -applaunch 12260 -skipstartup";
    if (settings.displayMode != 2) command += L" -window";
    if (skipDialog) command += L" -nodialog";
    if (diagnostic) command += L" -developer";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(steam.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool configureDisplay(const fs::path& gamePath, const AssistSettings& settings) {
    const Resolution resolution = resolutions[settings.resolution];
    const auto fixIni = gamePath / L"scripts/MaxPayne2.WidescreenFix.ini";
    if (!fs::is_regular_file(fixIni)) return false;
    if (!WritePrivateProfileStringW(L"MAIN", L"BorderlessWindowedMode",
                                    settings.displayMode == 0 ? L"1" : L"0", fixIni.c_str()) ||
        !WritePrivateProfileStringW(L"MAIN", L"HudAspectRatioConstraint",
                                    settings.resolution == 2 ? L"16:9" : L"21:9", fixIni.c_str())) return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LR"(Software\Remedy Entertainment\Max Payne 2\Video Settings)",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto set = [key](const wchar_t* name, DWORD value) {
        return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                              sizeof(value)) == ERROR_SUCCESS;
    };
    const bool ok = set(L"Display Width", resolution.width) && set(L"Display Height", resolution.height) &&
        set(L"Display Bits Per Pixel", 32);
    RegCloseKey(key);
    return ok;
}

constexpr DWORD attachRights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;

bool verifyGame(const fs::path& gamePath, std::wstring& error) {
    if (gamePath.empty()) {
        error = L"Max Payne 2 (RU) не найдена в библиотеках Steam.";
        return false;
    }
    for (const auto& gameFile : supported) {
        if (fileSha256(gamePath / gameFile.file) != gameFile.sha256) {
            error = L"Не найдена или не поддерживается версия файла " + std::wstring(gameFile.file);
            return false;
        }
    }
    return true;
}

int waitForGame(const fs::path& exe, const fs::path& mod, bool diagnostic, std::wstring& error,
                HANDLE cancel, TrainerStatus status, void* context) {
    while (WaitForSingleObject(cancel, 0) == WAIT_TIMEOUT) {
        if (status) status(context, false);
        DWORD pid = 0;
        do {
            pid = findGame(exe);
            if (!gameReady(pid)) pid = 0;
            if (!pid && WaitForSingleObject(cancel, 300) != WAIT_TIMEOUT) return 0;
        } while (!pid);
        if (WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT) return 0;

        Handle game(OpenProcess(attachRights, FALSE, pid));
        if (!game.value) {
            if (findGame(exe) != pid) continue;
            error = L"Не удалось открыть процесс игры.";
            return 1;
        }
        if (!remoteModule(pid, L"MP2Assist.dll")) {
            if (!verifyGame(exe.parent_path(), error)) return 1;
            if (!loadMod(game.value, pid, mod, diagnostic)) {
                if (WaitForSingleObject(game.value, 0) == WAIT_OBJECT_0) continue;
                error = L"Трейнер обнаружил игру, но модуль не загрузился.";
                return 1;
            }
        }
        if (status) status(context, true);
        const HANDLE wait[] = {cancel, game.value};
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
    }
    return 0;
}

int run(const fs::path& gamePath, bool verifyOnly, bool diag, bool skipDialog,
        const AssistSettings& settings, std::wstring& error, HANDLE trainerCancel = nullptr,
        TrainerStatus status = nullptr, void* context = nullptr) {
    if (!verifyGame(gamePath, error)) return 1;
    if (verifyOnly) return 0;

    wchar_t modPath[MAX_PATH]{};
    if (!siblingPath(nullptr, L"MP2Assist.dll", modPath) || !fs::is_regular_file(modPath)) {
        error = L"Рядом с запускатором отсутствует MP2Assist.dll.";
        return 1;
    }

    const auto exe = gamePath / L"MaxPayne2.exe";
    if (trainerCancel) return waitForGame(exe, modPath, diag, error, trainerCancel, status, context);
    DWORD pid = findGame(exe);
    if (!pid) {
        if (!hasWidescreen(gamePath)) {
            error = L"Сначала нажми «Установить Widescreen Fix».";
            return 1;
        }
        if (!configureDisplay(gamePath, settings)) {
            error = L"Не удалось записать настройки экрана. Проверь наличие Widescreen Fix в папке игры.";
            return 1;
        }
        if (!startThroughSteam(diag, skipDialog, settings)) {
            error = L"Не удалось запустить Steam.";
            return 1;
        }
    } else if (remoteModule(pid, L"MP2Assist.dll")) {
        error = L"Игра уже запущена. Для смены разрешения или режима экрана перезапусти её.";
        return 1;
    }
    for (int attempt = 0; attempt < 250 && !gameReady(pid); ++attempt) {
        Sleep(100);
        pid = findGame(exe);
    }
    if (!gameReady(pid)) {
        error = L"Основное окно Max Payne 2 (RU) не открылось. Проверь окно Steam.";
        return 1;
    }
    Handle game(OpenProcess(attachRights, FALSE, pid));
    if (!game.value || !loadMod(game.value, pid, modPath, diag)) {
        error = L"Игра запущена, но MP2Assist не удалось подключить.";
        return 1;
    }
    return 0;
}

bool installFixFor(const fs::path& gamePath, std::wstring& status) {
    try {
        if (!verifyGame(gamePath, status)) return false;
        if (findGame(gamePath / L"MaxPayne2.exe")) {
            status = L"Закрой Max Payne 2 перед установкой Widescreen Fix.";
            return false;
        }
        switch (installWidescreen(gamePath, status)) {
        case WidescreenInstall::installed:
            status = L"Widescreen Fix установлен. Выбери разрешение и режим экрана.";
            return true;
        case WidescreenInstall::alreadyInstalled:
            status = L"Widescreen Fix уже установлен. Выбери разрешение и режим экрана.";
            return true;
        case WidescreenInstall::failed:
            return false;
        }
    } catch (const std::exception&) {
        status = L"Не удалось прочитать или записать файлы Widescreen Fix в папке игры.";
    }
    return false;
}

bool installFixFromWindow(std::wstring& status) {
    try {
        return installFixFor(findSteamGame(), status);
    } catch (const std::exception&) {
        status = L"Не удалось прочитать библиотеки Steam.";
        return false;
    }
}

int launchFromWindow(const AssistSettings& settings, std::wstring& error,
                     HANDLE trainerCancel, TrainerStatus status, void* context) {
    try {
        return run(findSteamGame(), false, false, true, settings, error, trainerCancel, status, context);
    } catch (const std::exception&) {
        error = L"Не удалось открыть файлы игры или запускатора.";
        return 1;
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    struct Args { wchar_t** value; ~Args() { LocalFree(value); } } args{argv};
    if (argc == 1) return showLauncher(instance, launchFromWindow, installFixFromWindow);
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
    fs::path path;
    bool verifyOnly = false, diag = false, skipDialog = true, trainer = false, installFix = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--verify") == 0) verifyOnly = true;
        else if (wcscmp(argv[i], L"--diagnostics") == 0) diag = true;
        else if (wcscmp(argv[i], L"--wait") == 0) trainer = true;
        else if (wcscmp(argv[i], L"--install-fix") == 0) installFix = true;
        else if (wcscmp(argv[i], L"--nodialog") == 0) skipDialog = true;
        else if (wcscmp(argv[i], L"--setup-dialog") == 0) skipDialog = false;
        else if (wcscmp(argv[i], L"--game") == 0 && i + 1 < argc) path = argv[++i];
        else {
            std::wcerr << L"Usage: MP2Assist.exe [--game PATH] [--verify] [--diagnostics] [--setup-dialog] [--wait] [--install-fix]\n";
            return 1;
        }
    }
    try {
        path = path.empty() ? findSteamGame() : fs::absolute(path).make_preferred();
        if (installFix) {
            std::wstring status;
            const bool installed = installFixFor(path, status);
            (installed ? std::wcout : std::wcerr) << status << L"\n";
            return installed ? 0 : 1;
        }
        wchar_t profile[MAX_PATH]{};
        if (!siblingPath(nullptr, L"MP2Assist.ini", profile)) return 1;
        std::wstring error;
        Handle trainerCancel(trainer ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr);
        if (trainer && !trainerCancel.value) return 1;
        const auto status = +[](void*, bool attached) {
            std::wcout << (attached ? L"MP2Assist attached.\n" : L"Waiting for Max Payne 2 (RU).\n") << std::flush;
        };
        const int result = run(path, verifyOnly, diag, skipDialog, readSettings(profile), error,
                               trainerCancel.value, status);
        if (result) std::wcerr << error << L"\n";
        else std::wcout << (verifyOnly ? L"Max Payne 2 game files verified.\n" : L"MP2Assist loaded. Press Insert.\n");
        return result;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
