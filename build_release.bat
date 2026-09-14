@echo off
REM ============================================================================
REM build_release.bat - Shippable Release build for ASampler VST3.
REM
REM Builds the Release config (full optimisation, no debug symbols) and runs
REM the test suite against it as a final sanity check.
REM
REM Outputs:
REM   build\ASampler_artefacts\Release\VST3\ASampler.vst3\
REM
REM Inside the VST3 bundle, the two files that actually ship are:
REM   Contents\x86_64-win\ASampler.vst3   (the VST3 DLL)
REM   Contents\Resources\moduleinfo.json
REM
REM Zip the whole ASampler.vst3 folder (not just the inner DLL) and ship that.
REM
REM For day-to-day development, use build.bat instead.
REM ============================================================================

setlocal enabledelayedexpansion
pushd "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=Release"

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
echo [3/3] Running tests against %CONFIG% binaries...
ctest --test-dir "%BUILD_DIR%" --output-on-failure -C %CONFIG%
if errorlevel 1 goto :fail

echo.
echo ============================================================================
echo RELEASE BUILD OK. Shippable VST3 bundle at:
echo   %CD%\%BUILD_DIR%\ASampler_artefacts\%CONFIG%\VST3\ASampler.vst3
echo.
echo Zip the entire ASampler.vst3 folder ^(not just the inner DLL^) to ship.
echo ============================================================================
popd
endlocal
pause
exit /b 0

:fail
echo.
echo ============================================================================
echo RELEASE BUILD FAILED. See errors above.
echo ============================================================================
popd
endlocal
pause
exit /b 1
