@echo off
REM ===========================================================================
REM  PDFtoPrinter (C) - build script (Visual Studio 2022, x64)
REM  On first run it downloads the pinned PDFium prebuilt SDK (needs Windows
REM  10+ for the built-in curl and tar).
REM  Output: PDFtoPrinterNative.exe + pdfium.dll.
REM ===========================================================================
cd /d "%~dp0"

REM ---- locate Visual Studio 2022 (any edition) via vswhere -------------------
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Could not find Visual Studio. Install VS 2022 with the C++ workload. & exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

REM ---- fetch PDFium (pinned) -------------------------------------------------
REM To upgrade: pick a release from github.com/bblanchon/pdfium-binaries/releases,
REM set PDFIUM_BUILD to its chromium/NNNN number and PDFIUM_SHA256 to the hash of
REM its pdfium-win-x64.tgz. A changed pin re-downloads on the next build.
REM 7906 is the pdfium.dll shipped with the First_release and v1.1 releases.
set "PDFIUM_BUILD=7906"
set "PDFIUM_SHA256=a60740ba8e9bddefa5a53113e25f62364995383d2eed032bcfc60f17209afe47"

set "PDFIUM_HAVE="
if exist "pdfium\.pinned" set /p PDFIUM_HAVE=<"pdfium\.pinned"
if not "%PDFIUM_HAVE%"=="%PDFIUM_BUILD%" (
  echo Downloading PDFium chromium/%PDFIUM_BUILD% ...
  if exist pdfium rmdir /s /q pdfium
  curl -fL -o pdfium-win-x64.tgz https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%%2F%PDFIUM_BUILD%/pdfium-win-x64.tgz
  if errorlevel 1 (echo PDFium download failed. & exit /b 1)
  powershell -NoProfile -Command "if ((Get-FileHash pdfium-win-x64.tgz -Algorithm SHA256).Hash -ne '%PDFIUM_SHA256%') { exit 1 }"
  if errorlevel 1 (echo PDFium download does not match PDFIUM_SHA256. & del pdfium-win-x64.tgz & exit /b 1)
  mkdir pdfium
  tar -xzf pdfium-win-x64.tgz -C pdfium
  if errorlevel 1 (echo PDFium extract failed. & exit /b 1)
  del pdfium-win-x64.tgz
  > pdfium\.pinned echo %PDFIUM_BUILD%
)

REM ---- version stamp (CI sets these from MinVer; local builds get 0.0.0-dev) --
if not defined VER_MAJOR set VER_MAJOR=0
if not defined VER_MINOR set VER_MINOR=0
if not defined VER_PATCH set VER_PATCH=0
if not defined VER_STRING set VER_STRING=0.0.0-dev
> version.h echo #define VER_MAJOR %VER_MAJOR%
>> version.h echo #define VER_MINOR %VER_MINOR%
>> version.h echo #define VER_PATCH %VER_PATCH%
>> version.h echo #define VER_STRING "%VER_STRING%"
echo Version %VER_STRING%

REM ---- compile resources + program -----------------------------------------
rc /nologo /fo app.res app.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)

cl /nologo /W3 /O2 /MD /D_UNICODE /DUNICODE /D_CRT_SECURE_NO_WARNINGS /I pdfium\include ^
   pdftoprinter.c app.res ^
   /Fe:PDFtoPrinterNative.exe ^
   /link /subsystem:windows /entry:wmainCRTStartup ^
   /LIBPATH:pdfium\lib pdfium.dll.lib ^
   gdi32.lib winspool.lib user32.lib shell32.lib

if errorlevel 1 (echo BUILD FAILED & exit /b 1)

copy /Y pdfium\bin\pdfium.dll . >nul
REM PDFtoPrinter.exe = default-printer name, as in the original release
copy /Y PDFtoPrinterNative.exe PDFtoPrinter.exe >nul
REM rename-triggered variants: *Select* = console menu, *SelectGUI* = listbox dialog
copy /Y PDFtoPrinterNative.exe PDFtoPrinterSelect.exe >nul
copy /Y PDFtoPrinterNative.exe PDFtoPrinterSelectGUI.exe >nul
echo BUILD OK
