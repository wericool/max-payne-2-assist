#pragma once
#include <string>
#include "settings.hpp"

using TrainerStatus = void (*)(void* context, bool attached);
using LaunchGame = int (*)(const AssistSettings& settings, std::wstring& error,
                           HANDLE trainerCancel, TrainerStatus status, void* context);
using InstallFix = bool (*)(std::wstring& status);
int showLauncher(HINSTANCE instance, LaunchGame launch, InstallFix install);
