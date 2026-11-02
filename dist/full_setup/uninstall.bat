@echo off
chcp 65001 >nul
setlocal
rem Removes what install.bat put in (and what the mod made next to it) and gives the game its own xvidcore.dll back.
tasklist /fi "imagename eq call_to_arms.exe" | find /i "call_to_arms.exe" >nul && (echo Сначала закрой игру. / Close the game first. & pause & exit /b 1)
for /f "usebackq delims=" %%P in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0find_game.ps1"`) do set "BIN=%%P"
if not defined BIN (echo Не нашёл папку игры. Запусти uninstall.bat из ...\binaries\x64. / Game folder not found: run uninstall.bat from ...\binaries\x64. & pause & exit /b 1)
echo Папка игры / Game folder: %BIN%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0old_mod.ps1" "%BIN%"
for %%F in (iphlpapi.dll iphlpapi.old.dll iphlpapi.old2.dll goh_snapshots.ini goh_online.ini goh_music.ini) do if exist "%BIN%\%%F" del /f "%BIN%\%%F" || goto :fail
for %%F in ("%BIN%\goh_balance_profile_*.bin") do del /f "%%F" 2>nul
if exist "%BIN%\xvidcore_orig.dll" (
    rem after a game update its own (newer, about 1 MB) DLL is already in place: only the old copy goes
    set "NEWER="
    if exist "%BIN%\xvidcore.dll" for %%F in ("%BIN%\xvidcore.dll") do if %%~zF GTR 300000 set "NEWER=1"
    if defined NEWER (del /f "%BIN%\xvidcore_orig.dll") else (move /y "%BIN%\xvidcore_orig.dll" "%BIN%\xvidcore.dll" >nul || goto :fail)
)
echo.
echo Мод удалён, игра снова ванильная. Логи goh_*.log можно удалить вручную.
echo Ключ Документы\My Games\gates of hell\goh_mod_key.txt оставлен: он закрепляет твой SteamID на сервере мода.
echo.
echo The mod is removed, the game is vanilla again. The goh_*.log logs can be deleted by hand.
echo The key Documents\My Games\gates of hell\goh_mod_key.txt stays: it ties your SteamID to you on the mod's server.
pause
exit /b 0

:fail
echo Не получилось удалить файлы. Закрой игру и запусти uninstall.bat ещё раз.
echo Could not remove the files. Close the game and run uninstall.bat again.
pause
exit /b 1
