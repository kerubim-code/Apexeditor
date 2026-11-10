@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :no_vc

set "COMMON=/nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
set "LIBS=user32.lib gdi32.lib ole32.lib shell32.lib advapi32.lib urlmon.lib windowsapp.lib winhttp.lib shlwapi.lib windowscodecs.lib gdiplus.lib comdlg32.lib"

set "LIQUID=liquidDX11-main"
set "GLASS_SRC=%LIQUID%\examples\example_win32_directx11\src\glass\glass.cpp %LIQUID%\examples\example_win32_directx11\src\glass\backdrop.cpp"
set "LIQUID_IMGUI_SRC=%LIQUID%\imgui.cpp %LIQUID%\imgui_draw.cpp %LIQUID%\imgui_tables.cpp %LIQUID%\imgui_widgets.cpp %LIQUID%\misc\freetype\imgui_freetype.cpp %LIQUID%\backends\imgui_impl_dx11.cpp %LIQUID%\backends\imgui_impl_win32.cpp"
set "LIQUID_INC=/I%LIQUID% /I%LIQUID%\backends /I%LIQUID%\data /I%LIQUID%\examples\example_win32_directx11\src /I%LIQUID%\thirdparty\freetype\include"
set "GLASS_LIBS=d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib winmm.lib %LIQUID%\thirdparty\freetype\win64\freetype.lib"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

call "%VCVARS%" >nul 2>&1
if errorlevel 1 goto :no_vc

if not exist build mkdir build
if not exist dist mkdir dist
if not exist build\client mkdir build\client

if /i "%TARGET%"=="client" goto :client
goto :all

:all
call :client
if errorlevel 1 goto :failed
echo.
echo ============================================================
echo   BUILD OK  -  dist\APEXClient.exe
echo ============================================================
exit /b 0

:client
echo [client] dist\APEXClient.exe
rc /nologo /fo build\client\app.res app.rc
if errorlevel 1 exit /b 1
cl %COMMON% %LIQUID_INC% ^
    client\main.cpp client\app.cpp client\sections.cpp client\logo.cpp ^
    client\videocfg.cpp client\spotify.cpp client\crosshair.cpp client\overlay.cpp ^
    client\color\adl_backend.cpp client\color\gamma.cpp client\color\vibrance.cpp ^
    %GLASS_SRC% %LIQUID_IMGUI_SRC% ^
    build\client\app.res ^
    /Fo:build\client\ /Fe:dist\APEXClient.exe %LIBS% %GLASS_LIBS%
if errorlevel 1 exit /b 1
exit /b 0

:no_vc
echo [build] Visual Studio Build Tools not found:
echo         %VCVARS%
exit /b 1

:failed
echo.
echo [build] FAILED
exit /b 1
