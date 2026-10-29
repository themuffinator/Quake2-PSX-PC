@echo off
setlocal
pushd "%~dp0"
if exist "disc\game.cue" (
    start "" /d "%~dp0" "%~dp0q2psx.exe" --disc "%~dp0disc\game.cue" %*
) else (
    start "" /d "%~dp0" "%~dp0q2psx.exe" %*
)
set "q2psx_exit=%errorlevel%"
popd
exit /b %q2psx_exit%
