@echo off
rem Builds the add-on for each game with the MSVC x64 toolchain (VS 2022 or its Build Tools):
rem   build\CRStreamingFix.addon64        Control Resonant
rem   build\AW2StreamingFix.addon64       Alan Wake 2
rem   build\ControlStreamingFix.addon64   Control
rem
rem   build.bat                  build all three
rem   build.bat test             also run the offline tests (no game needed)
rem   build.bat test "path\to\CONTROLResonant.exe" "path\to\AlanWake2.exe" "path\to\Control_DX12.exe"
rem                              ... and check the add-on against those games' own files
rem                              (any of them, in any order; Control_DX11.exe works too)
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 call :find_msvc
where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC not found. Run this from an "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

if not exist build mkdir build
set FLAGS=/nologo /MT /EHsc /std:c++17 /W4 /DUNICODE /D_UNICODE /Ithird_party\imgui /Ithird_party\reshade

call :addon CRStreamingFix || exit /b 1
call :addon AW2StreamingFix /DCRSF_GAME_AW2 || exit /b 1
call :addon ControlStreamingFix /DCRSF_GAME_CONTROL || exit /b 1
if /i not "%~1"=="test" exit /b 0

rem The add-on only wakes up inside the game's executable, so each test harness is built under that name.
call :lifecycle CRStreamingFix CONTROLResonant.exe test_lifecycle.cpp || exit /b 1
call :lifecycle AW2StreamingFix AlanWake2.exe test_lifecycle.cpp /DCRSF_GAME_AW2 || exit /b 1
call :lifecycle ControlStreamingFix Control_DX12.exe test_lifecycle_control.cpp /DCRSF_GAME_CONTROL || exit /b 1

:next_exe
shift
if "%~1"=="" exit /b 0
if /i "%~nx1"=="CONTROLResonant.exe" (
    call :locate CRStreamingFix "%~1" || exit /b 1
) else if /i "%~nx1"=="AlanWake2.exe" (
    call :locate AW2StreamingFix "%~1" /DCRSF_GAME_AW2 || exit /b 1
) else if /i "%~nx1"=="Control_DX12.exe" (
    call :locate ControlStreamingFix "%~1" /DCRSF_GAME_CONTROL || exit /b 1
    call :live ControlStreamingFix "%~1" /DCRSF_GAME_CONTROL || exit /b 1
) else if /i "%~nx1"=="Control_DX11.exe" (
    call :locate ControlStreamingFix "%~1" /DCRSF_GAME_CONTROL || exit /b 1
    call :live ControlStreamingFix "%~1" /DCRSF_GAME_CONTROL || exit /b 1
) else (
    echo "%~1" is not the executable of a game this builds for.
    exit /b 1
)
goto next_exe

rem :addon <name> [define]
:addon
rc /nologo %~2 /fo build\%1.res src\CRStreamingFix.rc || exit /b 1
cl %FLAGS% %~2 /LD /O2 /wd4100 src\CRStreamingFix.cpp build\%1.res /Fobuild\%1.obj /Febuild\%1.addon64 /link /DLL psapi.lib user32.lib || exit /b 1
del build\%1.lib build\%1.exp 2>nul
echo Built build\%1.addon64
exit /b 0

rem :lifecycle <name> <game exe> <test source> [define]
:lifecycle
if not exist build\test-%1 mkdir build\test-%1
cl %FLAGS% %~4 /Od src\%3 /Fobuild\test-%1\ /Febuild\test-%1\%2 || exit /b 1
copy /y build\%1.addon64 build\test-%1\ >nul
build\test-%1\%2 || exit /b 1
exit /b 0

rem :locate <name> <path to the game exe> [define]
rem Maps the game's files and runs the add-on's lookup on them. Nothing of the game is executed.
:locate
cl %FLAGS% %~3 /O2 /wd4100 src\test_locate.cpp /Fobuild\test_locate_%1.obj /Febuild\test_locate_%1.exe /link psapi.lib user32.lib || exit /b 1
build\test_locate_%1.exe "%~2" || exit /b 1
exit /b 0

rem :live <name> <path to the game exe> <define>
rem Control only: loads the game's rl and renderer DLLs for real and lets the add-on work on their tweakables.
:live
if not exist build\test-%1-live mkdir build\test-%1-live
cl %FLAGS% %~3 /Od src\test_control_live.cpp /Fobuild\test-%1-live\ /Febuild\test-%1-live\%~nx2 || exit /b 1
copy /y build\%1.addon64 build\test-%1-live\ >nul
build\test-%1-live\%~nx2 "%~dp2." || exit /b 1
exit /b 0

:find_msvc
for %%R in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for %%E in (BuildTools Community Professional Enterprise) do (
        if exist "%%~R\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            call "%%~R\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
            exit /b 0
        )
    )
)
exit /b 1
