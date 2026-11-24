#pragma once
#include <filesystem>
#include <string>

enum class WidescreenInstall { failed, installed, alreadyInstalled };
WidescreenInstall installWidescreen(const std::filesystem::path& game, std::wstring& error);
bool hasWidescreen(const std::filesystem::path& game);
