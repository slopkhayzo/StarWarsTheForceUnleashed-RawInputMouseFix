@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Star Wars: The Force Unleashed is a 32-bit game: always build with the x86 toolchain.
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" (
    echo Microsoft C++ Build Tools were not found.
    echo Install Visual Studio Build Tools with Desktop development with C++.
    exit /b 1
  )
  set "VSINSTALL="
  for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
  if not defined VSINSTALL (
    echo The Visual Studio C++ toolchain was not found.
    exit /b 1
  )
  for %%D in ("!VSWHERE!") do set "PATH=%%~dpD;!PATH!"
  call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
  if errorlevel 1 exit /b 1
)

if not exist "build" mkdir "build"
set "CFLAGS=/nologo /std:c++17 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE"

rem "build-msvc.cmd dev" adds the development tools (mouse reader trace).
set "PLUGINFLAGS="
if /i "%~1"=="dev" set "PLUGINFLAGS=/DSWTFU_DEV_TOOLS=1"

cl.exe %CFLAGS% %PLUGINFLAGS% /LD /Fo:"build\swtfu-raw-mouse-fix.obj" "swtfu_raw_mouse_fix.cpp" ^
  /link /OUT:"build\SWTFURawMouseFix.asi" /IMPLIB:"build\SWTFURawMouseFix.lib" bcrypt.lib user32.lib
if errorlevel 1 (
  echo.
  echo Build failed.
  exit /b 1
)

cl.exe %CFLAGS% /Fo:"build\asi-load-test.obj" "asi-load-test.cpp" /link /OUT:"build\asi-load-test.exe"
if errorlevel 1 (
  echo.
  echo ASI load-test build failed.
  exit /b 1
)

cl.exe %CFLAGS% /Fo:"build\layout-profile-test.obj" "layout-profile-test.cpp" /link /OUT:"build\layout-profile-test.exe"
if errorlevel 1 (
  echo.
  echo Layout-profile test build failed.
  exit /b 1
)

cl.exe %CFLAGS% /Fo:"build\counts-test.obj" "counts-test.cpp" /link /OUT:"build\counts-test.exe"
if errorlevel 1 (
  echo.
  echo Count test build failed.
  exit /b 1
)

"build\counts-test.exe"
if errorlevel 1 (
  echo.
  echo Count tests failed.
  exit /b 1
)

rem Optional: validate real executables when their paths are set. SWTFU_EXE_UNPACKED is the
rem Steamless copy (research\bin\steam), whose code must match; SWTFU_EXE_STEAM is the Steam exe,
rem whose code is encrypted on disk and must be rejected (the plugin waits for the decryption).
if defined SWTFU_EXE_UNPACKED (
  "build\layout-profile-test.exe" "%SWTFU_EXE_UNPACKED%" "Steam, unpacked"
  if errorlevel 1 (
    echo.
    echo Layout-profile test failed on the unpacked executable.
    exit /b 1
  )
)
if defined SWTFU_EXE_STEAM (
  "build\layout-profile-test.exe" "%SWTFU_EXE_STEAM%" "Steam, encrypted" --expect-reject
  if errorlevel 1 (
    echo.
    echo Layout-profile test failed on the encrypted Steam executable.
    exit /b 1
  )
)

if not exist "build\layout-host" mkdir "build\layout-host"
copy /Y "build\asi-load-test.exe" "build\layout-host\SWTFU.exe" >nul
if errorlevel 1 (
  echo.
  echo Layout-host test staging failed.
  exit /b 1
)

"build\asi-load-test.exe" "build\SWTFURawMouseFix.asi"
if errorlevel 1 (
  echo.
  echo ASI load test failed.
  exit /b 1
)

rem A loader may unload the ASI right after loading it (another mod's loader does): no crash.
"build\asi-load-test.exe" "build\SWTFURawMouseFix.asi" --load-unload
if errorlevel 1 (
  echo.
  echo ASI load-unload test failed.
  exit /b 1
)

rem The renamed host has the right name but not the image base and size: it must fail closed.
"build\layout-host\SWTFU.exe" "build\SWTFURawMouseFix.asi" --layout-host
if errorlevel 1 (
  echo.
  echo Executable-layout fail-closed test failed.
  exit /b 1
)

echo.
echo Built and load-tested: %CD%\build\SWTFURawMouseFix.asi
endlocal
