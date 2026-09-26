@echo off
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do (
    set "VS=%%i"
)
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0i18n_tests.cpp" "%~dp0..\src\i18n\json_min.cpp" "%~dp0..\src\i18n\charset.cpp" "%~dp0..\src\i18n\protect.cpp" "%~dp0..\src\i18n\detect.cpp" "%~dp0..\src\i18n\cache.cpp" "%~dp0..\src\i18n\validate.cpp" /Fe:"%~dp0i18n_tests.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0i18n_tests.exe"
exit /b %errorlevel%
