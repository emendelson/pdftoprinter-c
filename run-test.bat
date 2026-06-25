@echo off
REM ---- PDFtoPrinter (C): non-interactive smoke tests -------------------------
REM Run from a normal Command Prompt (NOT Git Bash, which mangles /options).
cd /d "%~dp0"
set EXE=PDFtoPrinterNative.exe

echo ============================================================
echo 1) Usage screen
echo ============================================================
%EXE%

echo.
echo ============================================================
echo 2) Mock run (no printing) over the mixed-size sample, with CSV
echo ============================================================
%EXE% "examples\mixed-sizes.pdf" /mock /csv

echo.
echo ============================================================
echo 3) Render self-test to BMP (no printer, no dialog)
echo ============================================================
%EXE% --selftest "examples\mixed-sizes.pdf" preview.bmp 150

echo.
echo ============================================================
echo 4) Preview size-to-tray decisions with /autotray (no printing)
echo ============================================================
%EXE% "examples\mixed-sizes.pdf" "%DEFAULT_PRINTER%" /autotray /mock

echo.
echo Done. CSV is in %%TEMP%%\PDFPrinterTmp\summary_utf8.csv
echo.
echo Real print examples:
echo    %EXE% file.pdf /shrink-to-fit /auto-center
echo    %EXE% file.pdf "Your Printer Name" /duplex
echo    %EXE% file.pdf /settings=examples\tray-map.cfg.example /mock
echo ============================================================
