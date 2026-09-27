@echo off
rem Host test for i18n::chatbuf (in-place chat entry rewrite) vs a fake CChat buffer.
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0chatbuf_test.cpp" "%~dp0..\src\i18n\chatbuf.cpp" /Fe:"%~dp0chatbuf_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0chatbuf_test.exe"
exit /b %errorlevel%
