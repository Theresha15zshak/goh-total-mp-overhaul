@echo off
chcp 65001 >nul
setlocal
rem Removes what install.bat put in and gives the game its own xvidcore.dll back.
tasklist /fi "imagename eq call_to_arms.exe" | find /i "call_to_arms.exe" >nul && (echo Сначала закрой игру. & pause & exit /b 1)
for /f "usebackq delims=" %%P in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0find_game.ps1"`) do set "BIN=%%P"
if not defined BIN (echo Не нашёл папку игры. Запусти uninstall.bat из ...\binaries\x64. & pause & exit /b 1)
echo Папка игры: %BIN%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0replay_mod.ps1" "%BIN%" uninstall
for %%F in (iphlpapi.dll iphlpapi.old.dll iphlpapi.old2.dll goh_snapshots.ini) do if exist "%BIN%\%%F" del /f "%BIN%\%%F" || goto :fail
if exist "%BIN%\xvidcore_orig.dll" (
    rem after a game update its own (newer, about 1 MB) DLL is already in place: only the old copy goes
    set "NEWER="
    if exist "%BIN%\xvidcore.dll" for %%F in ("%BIN%\xvidcore.dll") do if %%~zF GTR 300000 set "NEWER=1"
    if defined NEWER (del /f "%BIN%\xvidcore_orig.dll") else (move /y "%BIN%\xvidcore_orig.dll" "%BIN%\xvidcore.dll" >nul || goto :fail)
)
echo.
echo Мод удалён, игра снова ванильная. Логи goh_replay*.log можно удалить вручную.
pause
exit /b 0

:fail
echo Не получилось удалить файлы. Закрой игру и запусти uninstall.bat ещё раз.
pause
exit /b 1
