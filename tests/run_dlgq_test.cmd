@echo off
rem Host test for i18n::dlgq (ordered, timing-faithful release of held dialogs).
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0dlgq_test.cpp" "%~dp0..\src\i18n\dlgq.cpp" /Fe:"%~dp0dlgq_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0dlgq_test.exe"
exit /b %errorlevel%
