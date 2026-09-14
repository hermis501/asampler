@echo off
REM ============================================================================
REM build.bat - Development build for ASampler VST3.
REM
REM Builds the RelWithDebInfo config (optimised, with debug symbols) and runs
REM the test suite. Use this while iterating on the source.
REM
REM Outputs:
REM   build\ASampler_artefacts\RelWithDebInfo\VST3\ASampler.vst3\
REM
REM For a shippable Release build, use build_release.bat instead.
REM ============================================================================

setlocal enabledelayedexpansion
pushd "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=RelWithDebInfo"

REM SDK locations (ASAMPLER_JUCE_DIR / ASAMPLER_CATCH2_DIR) are CACHE variables
REM defaulted in CMakeLists.txt, so an existing build dir keeps whatever path it
REM was first configured with. If an SDK has since moved, reusing that cache
REM fails in confusing ways - detect the stale path and reconfigure from scratch.
set "STALE_SDK="
if exist "%BUILD_DIR%\CMakeCache.txt" (
    for %%V in (ASAMPLER_JUCE_DIR ASAMPLER_CATCH2_DIR) do (
        for /f "tokens=2 delims==" %%P in ('findstr /b /c:"%%V:PATH=" "%BUILD_DIR%\CMakeCache.txt"') do (
            set "SDK_PATH=%%P"
            set "SDK_PATH=!SDK_PATH:/=\!"
            if not exist "!SDK_PATH!\CMakeLists.txt" set "STALE_SDK=!STALE_SDK! %%V=!SDK_PATH!"
        )
    )
)

if defined STALE_SDK (
    echo [1/3] Cached SDK path no longer exists:!STALE_SDK!
    echo       Deleting "%BUILD_DIR%" and reconfiguring from scratch...
    rmdir /s /q "%BUILD_DIR%"
)

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [1/3] Configuring CMake...
    cmake -S . -B "%BUILD_DIR%"
    if errorlevel 1 goto :fail
) else (
    echo [1/3] CMake already configured, skipping.
)

echo.
echo [2/3] Building %CONFIG%...
cmake --build "%BUILD_DIR%" --config %CONFIG%
if errorlevel 1 goto :fail

echo.
echo [3/3] Running tests...
ctest --test-dir "%BUILD_DIR%" --output-on-failure -C %CONFIG%
if errorlevel 1 goto :fail

echo.
echo ============================================================================
echo BUILD OK.
echo   VST3:        %CD%\%BUILD_DIR%\ASampler_artefacts\%CONFIG%\VST3\ASampler.vst3
echo ============================================================================
popd
endlocal
pause
exit /b 0

:fail
echo.
echo ============================================================================
echo BUILD FAILED. See errors above.
echo ============================================================================
popd
endlocal
pause
exit /b 1
