@echo off
rem Compila o PainelLeve com o Zig (zig cc). Resultado: PainelLeve.exe nesta pasta.
setlocal
cd /d "%~dp0"
rem Uso: build.cmd [nome-da-dll]  (padrao PainelLeveTap.dll; um nome diferente permite testar sem reiniciar o Explorer)
set DLL=%~1
if "%DLL%"=="" set DLL=PainelLeveTap.dll
set ZIG=zig
where zig >nul 2>nul || for /f "delims=" %%z in ('dir /s /b "%LOCALAPPDATA%\Microsoft\WinGet\Packages\zig.exe" 2^>nul') do set "ZIG=%%z"
"%ZIG%" cc -std=c11 -O2 -s -Wl,--subsystem,windows -Wall -Wextra -Wno-missing-field-initializers -o PainelLeve.exe main.c mixer.c ^
  -luser32 -lgdi32 -lshell32 -ladvapi32 -liphlpapi -lws2_32 -ldwmapi -lole32 -luuid -lversion -lwinmm -lshlwapi
if errorlevel 1 (echo FALHOU: exe & exit /b 1)
echo OK: PainelLeve.exe
"%ZIG%" c++ -std=c++17 -O2 -s -shared -fno-exceptions -Wall -Wextra -o "%DLL%" tap.cpp tap.def ^
  -luser32 -lole32 -loleaut32 -luuid
if errorlevel 1 (echo FALHOU: dll & exit /b 1)
echo OK: %DLL%
