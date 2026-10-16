@echo off
setlocal enabledelayedexpansion
rem ============================================================================
rem  Regenerates the committed SRT binding headers in zstd\zstdgpu\srt_headers
rem  from zstdgpu_srt_decl.h using the host tool zstdgpu_srt_tool.c.
rem
rem  The generated headers are checked into source control, so this only needs
rem  to be run when zstdgpu_srt_decl.h (the SRT schema) changes. After running,
rem  review and commit the updated files under zstd\zstdgpu\srt_headers.
rem ============================================================================

set "SCRIPT_DIR=%~dp0"
set "ZSTDGPU_DIR=%SCRIPT_DIR%..\..\zstdgpu"
set "OUT_DIR=srt_headers"
set "TMP_DIR=%SCRIPT_DIR%.build"

rem --- Locate Visual Studio and set up the x64 host toolchain -----------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [srtgen] vswhere.exe not found; is Visual Studio installed?
  exit /b 1
)
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo [srtgen] Could not locate a VC toolchain via vswhere.
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [srtgen] vcvars64.bat failed.
  exit /b 1
)

rem --- Build the tool ---------------------------------------------------------
rem stb_ds.h and zstdgpu_srt_decl.h sit next to zstdgpu_srt_tool.c, so no extra
rem include paths are required. /Zc:preprocessor is mandatory (the tool relies
rem on the conformant preprocessor for macro-parameter stringification).
if not exist "%TMP_DIR%" mkdir "%TMP_DIR%"
cl.exe /nologo /Zc:preprocessor /W4 /WX "%SCRIPT_DIR%zstdgpu_srt_tool.c" /Fe:"%TMP_DIR%\zstdgpu_srt_tool.exe" /Fo:"%TMP_DIR%\zstdgpu_srt_tool.obj"
if errorlevel 1 (
  echo [srtgen] Failed to compile zstdgpu_srt_tool.c
  exit /b 1
)

rem --- Run the tool (writes into zstdgpu\srt_headers) --------------------------
pushd "%ZSTDGPU_DIR%"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
"%TMP_DIR%\zstdgpu_srt_tool.exe" "%OUT_DIR%"
set "RC=%errorlevel%"
popd
if not "%RC%"=="0" (
  echo [srtgen] Generation failed with code %RC%
  exit /b %RC%
)

echo [srtgen] Regenerated SRT headers in zstd\zstdgpu\%OUT_DIR%
endlocal
