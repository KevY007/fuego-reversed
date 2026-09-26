@echo off
rem Host test for i18n::surfaces packet rebuild (dialogs / 3D labels) vs real captures.
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0surfaces_test.cpp" "%~dp0..\src\i18n\surfaces_rebuild.cpp" "%~dp0..\src\i18n\huffman.cpp" /Fe:"%~dp0surfaces_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0surfaces_test.exe"
exit /b %errorlevel%
