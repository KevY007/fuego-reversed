@echo off
rem build.cmd -- incremental headless build of bin\FuegoReversed.asi (no Visual Studio IDE needed).
rem
rem Use this after editing source. It does NOT re-check/rebuild dependencies -- run
rem scripts\bootstrap.cmd once first (and again only when the gta-reversed submodule moves).
rem If the generated deps are missing, MSBuild stops with a clear "run bootstrap" message
rem (see deps.props). Extra args are passed through to MSBuild, e.g.:
rem   scripts\build.cmd -t:Rebuild
rem   scripts\build.cmd -p:Configuration=Release
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio 2026 not found ^(vswhere.exe missing^). Install the "Desktop development with C++" workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo ERROR: no Visual Studio install with the C++ x86 toolset. Install "Desktop development with C++".
  exit /b 1
)
set "MSBUILD=%VSPATH%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
  echo ERROR: MSBuild not found at "%MSBUILD%".
  exit /b 1
)
"%MSBUILD%" "%~dp0..\FuegoReversed.sln" -p:Configuration=Release -p:Platform=Win32 -m -nologo -v:minimal %*
exit /b %errorlevel%
