# PDFtoPrinter (C)

A small, native Windows command-line utility that sends PDF files straight to a
printer. It renders pages with [PDFium](https://pdfium.googlesource.com/pdfium/)
and prints them through a GDI printer device context, so it needs **no external
PDF viewer**.

This is a native C rewrite (by Claude Code) of the long-standing AutoIt-based
`PDFtoPrinter.exe`. Besides being self-contained, it adds page-layout control
and the ability to **specify the printer's paper tray according to a PDF page's 
size**, something otherwise found mainly in Adobe Acrobat and PDF-XChange (and
implemented in the AutoIt version through the settings of the included
PDF-XChange Viewer, which is not in this new C version).

## Features

- Print one file, several files, a wildcard (`AA*.pdf`), or a whole folder
  (`/r`, `/R[#]` for recursive).
- Page ranges, including reverse order and odd/even (see below).
- Multiple copies, encrypted PDFs (`/p:password`).
- **Layout:** `/scale=#` (percent) or fit options, `/auto-rotate`,
  `/auto-center`, `/portrait`, `/landscape`.
- **Two-sided:** `/duplex`, `/duplex=short`, `/simplex`.
- **Paper source by page size:** an explicit per-printer `tray <size>=<bin>`
  map, or `/autotray` to let the driver match (off by default — see below).
- Printer selection: default printer, a console menu, or a GUI list box
  (by EXE name — see below).
- Settings files (`settings.cfg` / `/settings=`), CSV logging (`/csv`),
  dry-run preview (`/mock`).

## Building

Requirements: **Visual Studio 2022** with the C++ workload, on **Windows 10 or
later** (the build uses the built-in `curl` and `tar`).

```
build.bat
```

On the first run, `build.bat` downloads the prebuilt PDFium SDK from
[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) into a
`pdfium\` folder, then compiles. The output is `PDFtoPrinterNative.exe` plus
`pdfium.dll` (which must sit next to the EXE), and two renamed copies
(`PDFtoPrinterSelect.exe`, `PDFtoPrinterSelectGUI.exe`).

`run-test.bat` runs a few non-interactive smoke tests (run it from a normal
Command Prompt, not Git Bash).

## Usage

```
PDFtoPrinter [path\]file.pdf [more.pdf ...] ["printer name"] [pages=...]
             [copies=#] [focus="title"] [/r] [/R[#]] [/p:password]
             [/csv] [/mock] [/s]
             [/scale=#|fit] [/shrink-to-fit] [/expand-to-fit]
             [/auto-rotate] [/auto-center] [/portrait] [/landscape]
             [/duplex|/duplex=short] [/simplex] [/tray=#] [/autotray]
             [/outfile=path] [/settings=profile.cfg] [/listtrays]
             [/render=bitmap|ps|ps42]
```

- Quote any path/filename containing spaces. Relative paths and `*`/`?`
  wildcards are OK. Multiple named files override a wildcard.
- The default printer is used unless a printer name is given.
- `/scale=#` is an explicit percentage and overrides the fit options.
  `/shrink-to-fit` shrinks oversized pages, `/expand-to-fit` enlarges small
  pages, and using both fits either way.
- `/mock` lists what would print (and the per-page tray decisions) without
  printing. `/s` runs silently. `/outfile=path` prints to a file.
- `/render=` picks how pages reach the printer. `bitmap` (default) sends each
  page as a full-page image. `ps` sends PostScript level 3 inside the
  driver's own job (PostScript drivers only; others fall back to `bitmap`), and
  `ps42` also embeds TrueType fonts as Type 42. Type 42 only applies to
  embedded CID TrueType fonts; `examples/ps42-test.pdf` is a test page made of
  them.

### Page ranges

```
3            single page
2-4,6,8-9    ranges and singles
8-           page 8 to the end
z-1          all pages, reversed
z-1:odd      reversed odd pages   (also :even)
r5-r2        5th-from-last to 2nd-from-last
```

### Printer selection by EXE name

Rename (or copy) the executable to change how a printer is chosen when none is
named on the command line:

- `PDFtoPrinter*.exe` &rarr; default printer.
- `*Select*.exe` &rarr; a numbered console menu.
- `*SelectGUI*.exe` &rarr; a GUI list-box dialog.

(`build.bat` produces the `Select` and `SelectGUI` copies for you.)

### Choosing the paper tray by page size

First, list a printer's paper bins:

```
PDFtoPrinterNative.exe /listtrays "Your Printer Name"
```

This prints each bin as `/tray=<number>  <name>`. **Bin numbers are
printer-specific** (an HP "Tray 2" might be 260 on one model and 258 on
another), so size&rarr;tray maps are per-printer.

Then either select a bin directly:

```
PDFtoPrinterNative.exe file.pdf /tray=260
```

…or map sizes to bins in a settings file and let each page pick its tray
(see [`examples/tray-map.cfg.example`](examples/tray-map.cfg.example)):

```
Your Printer Name
tray letter=259
tray legal=258
```

```
PDFtoPrinterNative.exe doc.pdf /settings=mymap.cfg
```

### Choosing the paper source by page size (on by default)

**This happens automatically — no options needed.** Each page's paper size is
sent to the printer, which pulls from whichever tray holds that paper, so a
COM 10 envelope comes from the envelope tray and a letter page from the letter
tray. It is the same behaviour as Acrobat's "Choose paper source by PDF page
size" and as the original AutoIt PDFtoPrinter, which shipped with that option
enabled.

Recognized size names: `letter legal a4 a3 a5 tabloid executive statement folio
b5 a6`, and the envelopes `com10 env9 env11 env12 env14 monarch personal dl c5
c6 c65 envb5`. A page matching none of those is sent as a custom size, so a
tray configured for it can still be matched.

Two ways to change it:

- **`/no-autotray`** turns it off entirely: pages print on whatever paper the
  printer's own default source provides. Useful for unattended printing where a
  size that isn't loaded would otherwise make the printer pause and prompt for
  a manual feed.
- **`/autotray=form`** additionally forces the printer's "Automatically Select"
  bin (`DMBIN_FORMSOURCE`). Some drivers want this; others ignore it — an HP
  LaserJet Pro MFP 4101 ignores that bin for envelopes (which is why it is not
  the default), while an older HP LaserJet P3015 honours it.

Both work in a settings file as `no-autotray` or `autotray=form`. Preview any
job's tray decisions without printing by adding `/autotray /mock`.

> Tip: always preview with `/mock` first — it shows each page's chosen tray
> without printing.

### Settings files

A `settings.cfg` next to the EXE is loaded automatically; `/settings=file.cfg`
loads another profile. Each line is one option **without** the leading slash
(e.g. `duplex`, `shrink-to-fit`, `copies=1`, a bare printer name, or a
`tray <size>=<bin>` line). Lines starting with `#` or `;` are comments.
Precedence: built-in defaults &rarr; `settings.cfg` &rarr; `/settings=` file
&rarr; the command line (last wins).

## Notes

- The program is a GUI-subsystem app (like the original AutoIt build): no
  console window flashes when launched from Explorer, and double-clicking it
  with no arguments shows the help in a message box. When run from a command
  prompt it attaches to that console for text output — which also means an
  interactive `cmd` returns immediately; use `start /wait` in scripts that need
  to block.
- `--selftest in.pdf out.bmp [dpi]` renders page 1 to a BMP with no printer
  involved (a rendering diagnostic).

## Credits & license

- Created by **Edward Mendelson**, author of the original PDFtoPrinter. The
  design, the printer-tray research, and all on-hardware testing are his.
- The native C implementation was written by **Claude** (Anthropic's Claude
  Code), working from Edward's design and direction in a paired session.
- PDF rendering by [PDFium](https://pdfium.googlesource.com/pdfium/) (BSD), via
  the prebuilt binaries from
  [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries).
  PDFium is downloaded at build time and is **not** redistributed in this repo.
- See [LICENSE](LICENSE) for this project's license.
