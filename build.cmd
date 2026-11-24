@echo off
setlocal
cd /d "%~dp0"
if defined VSCMD_ARG_TGT_ARCH goto compile
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MP2_VS=%%I"
if not defined MP2_VS exit /b 1
call "%MP2_VS%\VC\Auxiliary\Build\vcvars32.bat" || exit /b 1
:compile
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 1
if not exist build mkdir build
if not exist release mkdir release
cl /nologo /std:c++17 /utf-8 /DUNICODE /D_UNICODE /O2 /MT /W4 /EHsc /LD /Fobuild\mod.obj src\mod.cpp user32.lib gdi32.lib /link /DEF:src\mod.def /OUT:release\MP2Assist.dll /IMPLIB:build\MP2Assist.lib || exit /b 1
cl /nologo /std:c++17 /utf-8 /DUNICODE /D_UNICODE /O2 /MT /W4 /EHsc /Fobuild\ /Ferelease\MP2Assist.exe src\launcher.cpp src\launcher_ui.cpp src\widescreen.cpp bcrypt.lib advapi32.lib shell32.lib user32.lib gdi32.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
