#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include "widescreen.hpp"
#include "sha256.hpp"

namespace {
namespace fs = std::filesystem;

struct Asset { const wchar_t* path; bool keepUserChanges; };
constexpr Asset assets[] = {
    {L"winmm.dll", false},
    {L"scripts/MaxPayne2.WidescreenFix.asi", false},
    {L"scripts/dxwrapper.asi", false},
    {L"scripts/MaxPayne2.WidescreenFix.ini", true},
    {L"scripts/dxwrapper.ini", true},
};

constexpr wchar_t releaseUrl[] =
    L"https://github.com/ThirteenAG/WidescreenFixesPack/releases/download/mp2/MaxPayne2.WidescreenFix.zip";
constexpr char releaseHash[] = "53BCFA5CE5D5A15DB87E8282C5CE517BAFC7DEAA87C561F86C41DBBAB64FCD93";

struct DownloadDirectory {
    fs::path path;
    bool create() {
        wchar_t temp[MAX_PATH]{};
        wchar_t unique[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH || !GetTempFileNameW(temp, L"mp2", 0, unique)) return false;
        if (!DeleteFileW(unique) || !CreateDirectoryW(unique, nullptr)) return false;
        path = unique;
        return true;
    }
    ~DownloadDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};

bool runWindowsTool(const wchar_t* name, const std::wstring& args, const fs::path& directory) {
    wchar_t windows[MAX_PATH]{};
    const UINT size = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!size || size >= MAX_PATH) return false;
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    const auto tool = fs::path(windows) / (wow64 ? L"Sysnative" : L"System32") / name;
    auto command = L"\"" + tool.wstring() + L"\" " + args;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 50000);
    DWORD result = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    else if (TerminateProcess(process.hProcess, 1)) WaitForSingleObject(process.hProcess, 3000);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && result == 0;
}

bool downloadFix(const fs::path& directory, std::wstring& error) {
    const auto download = std::wstring(
        L"--fail --silent --show-error --location --proto =https --proto-redir =https "
        L"--max-time 40 --output widescreen.zip ") + releaseUrl;
    if (!runWindowsTool(L"curl.exe", download, directory)) {
        error = L"Не удалось скачать Widescreen Fix с GitHub.";
        return false;
    }
    if (fileSha256(directory / L"widescreen.zip") != releaseHash) {
        error = L"Архив Widescreen Fix изменился. Нужна обновлённая версия MP2Assist.";
        return false;
    }
    std::wstring extract = L"-xf widescreen.zip";
    for (const auto& asset : assets) extract += L" \"" + std::wstring(asset.path) + L"\"";
    if (!runWindowsTool(L"tar.exe", extract, directory)) {
        error = L"Не удалось распаковать проверенный архив Widescreen Fix.";
        return false;
    }
    return true;
}

bool installFile(const fs::path& destination, const fs::path& source) {
    wchar_t staged[MAX_PATH]{};
    if (!GetTempFileNameW(destination.parent_path().c_str(), L"mp2", 0, staged)) return false;
    if (CopyFileW(source.c_str(), staged, FALSE) &&
        MoveFileExW(staged, destination.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(staged);
    return false;
}

struct InstallLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\MP2Assist.WidescreenInstall");
    bool acquired = false;
    ~InstallLock() {
        if (acquired) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
    bool acquire() {
        if (!handle) return false;
        const auto result = WaitForSingleObject(handle, 0);
        acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        return acquired;
    }
};
}

bool hasWidescreen(const fs::path& game) {
    for (const auto& asset : assets) {
        if (!fs::is_regular_file(game / asset.path)) return false;
    }
    return true;
}

WidescreenInstall installWidescreen(const fs::path& game, std::wstring& error) {
    InstallLock lock;
    if (!lock.acquire()) {
        error = L"Установка Widescreen Fix уже запущена.";
        return WidescreenInstall::failed;
    }
    if (hasWidescreen(game)) return WidescreenInstall::alreadyInstalled;

    DownloadDirectory download;
    if (!download.create()) {
        error = L"Не удалось создать временную папку для Widescreen Fix.";
        return WidescreenInstall::failed;
    }
    if (!downloadFix(download.path, error)) return WidescreenInstall::failed;

    struct File { fs::path path, source; bool missing{}, written{}; };
    std::array<File, std::size(assets)> files;
    for (size_t index = 0; index < files.size(); ++index) {
        const auto& asset = assets[index];
        auto& file = files[index];
        file.path = game / asset.path;
        file.source = download.path / asset.path;
        if (!fs::is_regular_file(file.source)) {
            error = L"В скачанном архиве отсутствует " + file.source.filename().wstring() + L".";
            return WidescreenInstall::failed;
        }
        file.missing = !fs::exists(file.path);
        if (!file.missing && (!fs::is_regular_file(file.path) ||
                              (!asset.keepUserChanges && fileSha256(file.path) != fileSha256(file.source)))) {
            error = L"В папке игры уже есть другой " + file.path.filename().wstring() + L". Установка остановлена.";
            return WidescreenInstall::failed;
        }
    }

    std::error_code directoryError;
    fs::create_directories(game / L"scripts", directoryError);
    if (directoryError) {
        error = L"Не удалось создать папку scripts в папке игры.";
        return WidescreenInstall::failed;
    }
    for (auto& file : files) {
        if (!file.missing) continue;
        if (!installFile(file.path, file.source)) {
            for (const auto& previous : files) {
                if (previous.written) DeleteFileW(previous.path.c_str());
            }
            error = L"Не удалось установить " + file.path.filename().wstring() + L". Проверь доступ к папке игры.";
            return WidescreenInstall::failed;
        }
        file.written = true;
    }
    return WidescreenInstall::installed;
}
