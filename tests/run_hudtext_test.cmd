@echo off
rem Host test for i18n::hudtext (line-by-line HUD / 3D-label translation, number stand-ins).
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /utf-8 /I"%~dp0..\src" "%~dp0hudtext_test.cpp" "%~dp0..\src\i18n\hudtext.cpp" "%~dp0..\src\i18n\surfaces_rebuild.cpp" "%~dp0..\src\i18n\huffman.cpp" /Fe:"%~dp0hudtext_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0hudtext_test.exe"
exit /b %errorlevel%
