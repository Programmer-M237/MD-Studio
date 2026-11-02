@echo off
rem ===========================================================================
rem  Local Windows build -> dist\MDStudio\MDStudio.exe (portable folder)
rem  1. Install Qt 6 with the "MinGW 64-bit" kit (+ Qt's MinGW and Ninja tools)
rem     from https://www.qt.io/download-qt-installer
rem  2. Edit the two paths below to match your install, then double-click.
rem ===========================================================================
setlocal
set QT_KIT=C:\Qt\6.7.3\mingw_64
set QT_TOOLS=C:\Qt\Tools

set PATH=%QT_KIT%\bin;%QT_TOOLS%\mingw1310_64\bin;%QT_TOOLS%\Ninja;%PATH%

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT_KIT% || goto :fail
cmake --build build || goto :fail

if exist dist rmdir /s /q dist
mkdir dist\MDStudio
copy build\MDStudio.exe dist\MDStudio\ >nul
windeployqt --release --no-translations dist\MDStudio\MDStudio.exe || goto :fail

echo.
echo Done:  dist\MDStudio\MDStudio.exe
exit /b 0
:fail
echo.
echo Build failed - check the QT_KIT / QT_TOOLS paths at the top of this file.
exit /b 1
