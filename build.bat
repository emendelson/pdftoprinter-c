@echo off
REM ===========================================================================
REM  PDFtoPrinter (C) - build script (Visual Studio 2022, x64)
REM  On first run it downloads the PDFium prebuilt SDK (needs Windows 10+ for
REM  the built-in curl and tar). Output: PDFtoPrinterNative.exe + pdfium.dll.
REM ===========================================================================
cd /d "%~dp0"

REM ---- locate Visual Studio 2022 (any edition) via vswhere -------------------
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Could not find Visual Studio. Install VS 2022 with the C++ workload. & exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

REM ---- fetch PDFium on first build ------------------------------------------
if not exist "pdfium\include\fpdfview.h" (
  echo Downloading PDFium prebuilt binaries ...
  curl -L -o pdfium-win-x64.tgz https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-win-x64.tgz
  if errorlevel 1 (echo PDFium download failed. & exit /b 1)
  if not exist pdfium mkdir pdfium
  tar -xzf pdfium-win-x64.tgz -C pdfium
  del pdfium-win-x64.tgz
)

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
REM rename-triggered variants: *Select* = console menu, *SelectGUI* = listbox dialog
copy /Y PDFtoPrinterNative.exe PDFtoPrinterSelect.exe >nul
copy /Y PDFtoPrinterNative.exe PDFtoPrinterSelectGUI.exe >nul
echo BUILD OK
