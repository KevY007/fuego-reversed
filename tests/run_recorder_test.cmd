@echo off
rem Host test for rec::Timeline (the packet recorder's decoding + noise control).
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0recorder_test.cpp" "%~dp0..\src\recorder\timeline.cpp" "%~dp0..\src\i18n\huffman.cpp" /Fe:"%~dp0recorder_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0recorder_test.exe"
exit /b %errorlevel%
