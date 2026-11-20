@echo off
setlocal
set "BIN=D:\dev\mingw64\bin"
if not exist "%BIN%\g++.exe" (
  for /d %%D in ("D:\*\RedPanda-CPP\mingw64\bin") do set "BIN=%%~fD"
)
if not exist "%BIN%\g++.exe" (
  echo Cannot find g++. Put MinGW in PATH or edit this script.
  exit /b 1
)
set "PATH=%BIN%;%PATH%"
set "WD=D:\dev\WinDivert-2.2.2-A"
set "OUT=%~dp0release"
if not exist "%OUT%" mkdir "%OUT%"

"%BIN%\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -I"%WD%\include" -o "%OUT%\FastGithubCpp.exe" "%~dp0main.cpp" -L"%WD%\x64" -lWinDivert -lws2_32 -ldnsapi
if errorlevel 1 exit /b 1

pushd "%~dp0"
"%BIN%\windres.exe" -i app.rc -o app.res -O coff
if errorlevel 1 (popd & exit /b 1)
popd

"%BIN%\g++.exe" -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static -o "%OUT%\FastGithubCpp.UI.exe" "%~dp0ui.cpp" "%~dp0app.res" -lws2_32 -lshell32
if errorlevel 1 exit /b 1

copy /Y "%~dp0config.ini" "%OUT%\" >nul
copy /Y "%~dp0FastGithubCpp.UI.exe.manifest" "%OUT%\" >nul
copy /Y "%WD%\x64\WinDivert.dll" "%OUT%\" >nul
copy /Y "%WD%\x64\WinDivert64.sys" "%OUT%\" >nul
echo Built to: %OUT%
