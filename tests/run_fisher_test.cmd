@echo off
rem Host test for fish::Fisher (the auto fisher's state machine).
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0fisher_test.cpp" "%~dp0..\src\fishing\fisher.cpp" /Fe:"%~dp0fisher_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0fisher_test.exe"
exit /b %errorlevel%
