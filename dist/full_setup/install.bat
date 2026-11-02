@echo off
chcp 65001 >nul
setlocal
rem Puts the mod's files into the game's binaries\x64: iphlpapi.dll (the mod), xvidcore.dll (REA replay fix),
rem goh_snapshots.ini (instant replay jumps on). The game's own xvidcore.dll is kept as xvidcore_orig.dll.
rem Then the replay_unlock mod (Replays tab in Extras) into mods\ and on in every profile (replay_mod.ps1).
tasklist /fi "imagename eq call_to_arms.exe" | find /i "call_to_arms.exe" >nul && (echo Сначала закрой игру. & pause & exit /b 1)
for /f "usebackq delims=" %%P in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0find_game.ps1"`) do set "BIN=%%P"
if not defined BIN (
    echo Не нашёл папку игры. Положи все файлы архива в ...\Call to Arms - Gates of Hell\binaries\x64 и запусти install.bat оттуда.
    pause & exit /b 1
)
echo Папка игры: %BIN%

rem the game's own xvidcore.dll is about 1 MB, the replay fix 64 KB
if not exist "%BIN%\xvidcore_orig.dll" (
    move "%BIN%\xvidcore.dll" "%BIN%\xvidcore_orig.dll" >nul || goto :fail
) else (
    if exist "%BIN%\xvidcore.dll" for %%F in ("%BIN%\xvidcore.dll") do if %%~zF GTR 300000 move /y "%BIN%\xvidcore.dll" "%BIN%\xvidcore_orig.dll" >nul || goto :fail
)
copy /y "%~dp0files\xvidcore.dll" "%BIN%\xvidcore.dll" >nul || goto :fail
copy /y "%~dp0files\iphlpapi.dll" "%BIN%\iphlpapi.dll" >nul || goto :fail
copy /y "%~dp0files\goh_snapshots.ini" "%BIN%\goh_snapshots.ini" >nul || goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0replay_mod.ps1" "%BIN%" install || goto :fail
echo.
echo Готово. Реплеи: Дополнительно -^> Реплеи. F7 — компетитив, F8 — панель реплея, F9 — графики, F10 — музыка.
echo Удалить всё: uninstall.bat
pause
exit /b 0

:fail
rem never leave the game without an xvidcore.dll
if not exist "%BIN%\xvidcore.dll" if exist "%BIN%\xvidcore_orig.dll" move "%BIN%\xvidcore_orig.dll" "%BIN%\xvidcore.dll" >nul
echo Не получилось скопировать файлы. Закрой игру и запусти install.bat ещё раз (можно от имени администратора).
pause
exit /b 1
