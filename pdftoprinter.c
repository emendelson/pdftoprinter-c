/* ============================================================================
 *  PDFtoPrinter (native) - v1
 *  ---------------------------------------------------------------------------
 *  Native Win32 C replacement for the AutoIt + PDF-XChange Viewer version.
 *  Renders PDF pages with PDFium and prints them through a GDI printer DC,
 *  driving the DEVMODE struct directly. No external viewer, no qpdf.
 *
 *  v1 scope: drop-in match of the original command line, plus the new layout
 *  flags (/scale=, /shrink-to-fit, /expand-to-fit, /auto-rotate, /auto-center,
 *  /portrait, /landscape, /duplex, /simplex), batch/wildcard/recursive file
 *  enumeration, and the settings.cfg / /settings= config loader.
 *
 *  v2 (later): paper-source-by-PDF-size tray mapping (config "tray <size>=<bin>"
 *  lines are reserved/ignored here), tuned across Edward's three HP printers.
 *
 *  Build: see build.bat (VS 2022 x64).
 * ========================================================================== */

#include <windows.h>
#include <winspool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include "fpdfview.h"

/* ----- Exit codes (mirror the AutoIt version where meaningful) ----------- */
#define EXIT_OK             0
#define EXIT_NO_ARGS        2
#define EXIT_NO_PRINTER     3
#define EXIT_BAD_COPIES     4
#define EXIT_BAD_RX         5
#define EXIT_PRINTER_INVALID 6
#define EXIT_NO_PDF_ARG     7
#define EXIT_NO_PDF_MATCH   8
#define EXIT_PRINT_ERRORS   9

/* ----- Scaling model ----------------------------------------------------- */
enum { SCALE_FLAGS = 0, SCALE_PERCENT = 1 };

/* ----- Options populated by config files + the command line -------------- */
typedef struct {
    wchar_t  printername[512];
    int      printerSpecified;       /* user named a printer                 */
    wchar_t  pageselector[1024];     /* text after "pages="                  */
    int      copies;
    wchar_t  focus[512];
    wchar_t  mydir[MAX_PATH];        /* CSV output folder base               */
    wchar_t  outfile[MAX_PATH];      /* print-to-file / port redirect        */
    wchar_t  password[256];
    int      recur;                  /* 1=this folder, 0=unlimited, -n=depth */
    int      csv, mock, silent, debug;

    int      scaleMode;              /* SCALE_FLAGS | SCALE_PERCENT          */
    double   scalePct;
    int      shrink, expand;         /* /shrink-to-fit, /expand-to-fit       */
    int      autorotate, autocenter;
    int      orient;                 /* 0=unset, 1=portrait, 2=landscape     */
    int      duplex;                 /* 0=unset, 1/2/3 = DMDUP_*             */
    int      tray;                   /* 0=unset, else dmDefaultSource value  */
    int      listtrays;              /* /listtrays diagnostic, then exit     */
    int      autotray;               /* /autotray: pick bin by page size     */
    int      autoBin;                /* detected by-size bin (15 or 7)       */
    struct { wchar_t name[32]; int bin; } traymap[32];  /* tray <size>=<bin> */
    int      traymapCount;
} options;

/* ----- Simple growable vector of wide strings ---------------------------- */
typedef struct { wchar_t **a; int n, cap; } WideVec;
static void wv_push(WideVec *v, const wchar_t *s) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 16;
        v->a = (wchar_t**)realloc(v->a, v->cap * sizeof(wchar_t*)); }
    v->a[v->n++] = _wcsdup(s);
}

/* ----- Growable vector of ints (expanded page list) ---------------------- */
typedef struct { int *a; int n, cap; } IntVec;
static void iv_push(IntVec *v, int x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 64;
        v->a = (int*)realloc(v->a, v->cap * sizeof(int)); }
    v->a[v->n++] = x;
}

/* ----- CSV summary rows -------------------------------------------------- */
typedef struct {
    int     index;
    wchar_t folder[MAX_PATH];
    wchar_t filename[MAX_PATH];
    wchar_t datetime[32];
    int     encrypted;
    int     pagecount;
    int     pagesSelected;
    int     copies;
    wchar_t result[16];
    wchar_t errorinfo[512];
} CsvRow;
static CsvRow *g_rows = NULL;
static int     g_rowCount = 0, g_rowCap = 0;
static CsvRow *new_row(void) {
    if (g_rowCount == g_rowCap) { g_rowCap = g_rowCap ? g_rowCap * 2 : 32;
        g_rows = (CsvRow*)realloc(g_rows, g_rowCap * sizeof(CsvRow)); }
    CsvRow *r = &g_rows[g_rowCount++];
    ZeroMemory(r, sizeof(*r));
    return r;
}

static wchar_t g_errors[8192] = L"";

/* ========================================================================= */
/*  Small helpers                                                            */
/* ========================================================================= */

static void msg(const options *o, const wchar_t *s) {
    /* Plain console reporting; silent suppresses nothing on stderr but stays
       non-interactive. */
    fwprintf(stderr, L"%s\n", s);
}

static int ends_with_pdf(const wchar_t *s) {
    size_t n = wcslen(s);
    return n >= 4 && _wcsicmp(s + n - 4, L".pdf") == 0;
}

/* Compare arg to a flag name, ignoring an optional leading '/'. Lets config
   files write "duplex" while the command line writes "/duplex". */
static int optmatch(const wchar_t *arg, const wchar_t *name) {
    const wchar_t *a = arg;
    if (*a == L'/') a++;
    return _wcsicmp(a, name) == 0;
}
/* If arg (minus optional '/') begins with prefix, return pointer to the rest,
   else NULL. */
static const wchar_t *optprefix(const wchar_t *arg, const wchar_t *prefix) {
    const wchar_t *a = arg;
    if (*a == L'/') a++;
    size_t n = wcslen(prefix);
    if (_wcsnicmp(a, prefix, n) == 0) return a + n;
    return NULL;
}

static void to_utf8(const wchar_t *w, char *out, int outBytes) {
    if (!w || !*w) { if (outBytes) out[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outBytes, NULL, NULL);
}

static void now_string(wchar_t *out, int cch) {
    SYSTEMTIME st; GetLocalTime(&st);
    _snwprintf(out, cch, L"%04d-%02d-%02d %02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* ========================================================================= */
/*  Page-range expansion  (ports splitexpand/parsepage to an ordered list)   */
/*    Tokens: comma separated.  Each token: PART[:odd|even]                   */
/*    PART: single | left-right ; anchors z / r<k> ; empty-left=1 empty-right=N*/
/*    Out-of-range pages dropped; duplicates kept; ranges may be reversed.    */
/* ========================================================================= */

static int resolve_anchor(const wchar_t *s, int N, int isLeft, int *ok) {
    wchar_t buf[64]; int i = 0;
    *ok = 1;
    while (*s && i < 63) buf[i++] = (wchar_t)towlower(*s++);
    buf[i] = 0;
    if (buf[0] == 0)                       return isLeft ? 1 : N;
    if (wcscmp(buf, L"z") == 0)            return N;
    if (buf[0] == L'r' && iswdigit(buf[1])) { int k = _wtoi(buf + 1); return N - k + 1; }
    if (iswdigit(buf[0]) || buf[0] == L'+') return _wtoi(buf);
    *ok = 0;
    return 0;
}

/* Expand one selector into ivec (page numbers). Returns 1 ok, 0 parse error. */
static int expand_pages(const wchar_t *sel, int N, IntVec *out) {
    if (!sel || !*sel) {                       /* default: all pages 1..N    */
        for (int p = 1; p <= N; p++) iv_push(out, p);
        return 1;
    }
    wchar_t *work = _wcsdup(sel);
    wchar_t *ctx = NULL;
    wchar_t *tok = wcstok(work, L",", &ctx);
    int okAll = 1;
    while (tok) {
        /* trim spaces */
        while (*tok == L' ') tok++;
        size_t tl = wcslen(tok);
        while (tl && tok[tl - 1] == L' ') tok[--tl] = 0;
        if (*tok) {
            /* split off :odd / :even */
            int filter = 0;                    /* 0 all, 1 odd, 2 even       */
            wchar_t *colon = wcschr(tok, L':');
            if (colon) {
                *colon = 0;
                if (_wcsicmp(colon + 1, L"odd") == 0)  filter = 1;
                else if (_wcsicmp(colon + 1, L"even") == 0) filter = 2;
                else { okAll = 0; break; }
            }
            int a, b, ok1 = 1, ok2 = 1;
            wchar_t *dash = wcschr(tok, L'-');
            if (!dash) {
                a = b = resolve_anchor(tok, N, 1, &ok1);
                ok2 = ok1;
            } else {
                *dash = 0;
                a = resolve_anchor(tok, N, 1, &ok1);
                b = resolve_anchor(dash + 1, N, 0, &ok2);
            }
            if (!ok1 || !ok2) { okAll = 0; break; }
            int step = (b < a) ? -1 : 1;
            for (int p = a; ; p += step) {
                if (p >= 1 && p <= N) {
                    if (filter == 0 ||
                        (filter == 1 && (p & 1)) ||
                        (filter == 2 && !(p & 1)))
                        iv_push(out, p);
                }
                if (p == b) break;
            }
        }
        tok = wcstok(NULL, L",", &ctx);
    }
    free(work);
    return okAll;
}

/* ========================================================================= */
/*  File enumeration (wildcards + recursion)                                 */
/* ========================================================================= */

static void enum_dir(const wchar_t *dir, const wchar_t *pattern,
                     int curDepth, int maxDepth, WideVec *out) {
    wchar_t glob[MAX_PATH];
    _snwprintf(glob, MAX_PATH, L"%s\\%s", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t full[MAX_PATH];
                _snwprintf(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                wv_push(out, full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (curDepth < maxDepth) {
        wchar_t sub[MAX_PATH];
        _snwprintf(sub, MAX_PATH, L"%s\\*", dir);
        HANDLE hd = FindFirstFileW(sub, &fd);
        if (hd != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
                    wchar_t child[MAX_PATH];
                    _snwprintf(child, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                    enum_dir(child, pattern, curDepth + 1, maxDepth, out);
                }
            } while (FindNextFileW(hd, &fd));
            FindClose(hd);
        }
    }
}

/* Expand one .pdf argument (possibly containing wildcards) into out. */
static void expand_one_file_arg(const wchar_t *arg, int recur, WideVec *out) {
    wchar_t full[MAX_PATH];
    GetFullPathNameW(arg, MAX_PATH, full, NULL);
    int hasWild = (wcschr(full, L'*') || wcschr(full, L'?'));
    if (!hasWild) { wv_push(out, full); return; }

    wchar_t *slash = wcsrchr(full, L'\\');
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    if (slash) {
        size_t dl = (size_t)(slash - full);
        wcsncpy(dir, full, dl); dir[dl] = 0;
        wcscpy(pattern, slash + 1);
    } else { wcscpy(dir, L"."); wcscpy(pattern, full); }

    int maxDepth = (recur == 1) ? 1 : (recur == 0) ? 1000000 : (-recur + 1);
    enum_dir(dir, pattern, 1, maxDepth, out);
}

/* ========================================================================= */
/*  Option parsing (shared by command line and config files)                 */
/* ========================================================================= */

static void apply_arg(options *o, const wchar_t *arg, int fromConfig,
                      WideVec *fileArgs) {
    const wchar_t *rest;

    if (ends_with_pdf(arg)) {
        if (!fromConfig) wv_push(fileArgs, arg);   /* config holds how, not what */
        return;
    }
    if ((rest = optprefix(arg, L"pages="))) { wcsncpy(o->pageselector, rest, 1023); return; }
    if ((rest = optprefix(arg, L"copies="))) {
        int c = _wtoi(rest); if (c < 0) c = -c; if (c == 0) c = 1;
        o->copies = c; return;
    }
    if ((rest = optprefix(arg, L"focus="))) { wcsncpy(o->focus, rest, 511); return; }
    if ((rest = optprefix(arg, L"mydir="))) { wcsncpy(o->mydir, rest, MAX_PATH - 1); return; }
    if ((rest = optprefix(arg, L"outfile="))) { wcsncpy(o->outfile, rest, MAX_PATH - 1); return; }
    if ((rest = optprefix(arg, L"scale="))) {
        if (_wcsicmp(rest, L"fit") == 0) { o->shrink = o->expand = 1; o->scaleMode = SCALE_FLAGS; }
        else { o->scaleMode = SCALE_PERCENT; o->scalePct = _wtof(rest); if (o->scalePct <= 0) o->scalePct = 100; }
        return;
    }
    if ((rest = optprefix(arg, L"duplex="))) {
        o->duplex = (_wcsicmp(rest, L"short") == 0) ? DMDUP_HORIZONTAL : DMDUP_VERTICAL; return;
    }
    if ((rest = optprefix(arg, L"p:"))) { wcsncpy(o->password, rest, 255); return; }
    if (optprefix(arg, L"settings=")) return;          /* handled in main    */
    if ((rest = optprefix(arg, L"tray="))) { o->tray = _wtoi(rest); return; }  /* manual bin */
    {
        /* "tray <size>=<bin>" map line (e.g. tray legal=258) */
        const wchar_t *m = optprefix(arg, L"tray ");
        if (!m) m = optprefix(arg, L"tray\t");
        if (m) {
            const wchar_t *eq = wcschr(m, L'=');
            if (eq && o->traymapCount < 32) {
                wchar_t nm[32]; int nl = 0; const wchar_t *p = m;
                while (p < eq && (*p == L' ' || *p == L'\t')) p++;
                while (p < eq && nl < 31) nm[nl++] = (wchar_t)towlower(*p++);
                while (nl && nm[nl - 1] == L' ') nl--;
                nm[nl] = 0;
                int bin = _wtoi(eq + 1);
                if (nm[0] && bin) {
                    wcscpy(o->traymap[o->traymapCount].name, nm);
                    o->traymap[o->traymapCount].bin = bin;
                    o->traymapCount++;
                }
            }
            return;
        }
    }
    if (optmatch(arg, L"autotray"))  { o->autotray = 1; return; }
    if (optprefix(arg, L"tray"))     return;           /* any other tray* -> ignore */

    if (optmatch(arg, L"listtrays")) { o->listtrays = 1; return; }

    if (optmatch(arg, L"shrink-to-fit")) { o->shrink = 1; return; }
    if (optmatch(arg, L"expand-to-fit")) { o->expand = 1; return; }
    if (optmatch(arg, L"auto-rotate"))   { o->autorotate = 1; return; }
    if (optmatch(arg, L"auto-center"))   { o->autocenter = 1; return; }
    if (optmatch(arg, L"portrait"))      { o->orient = DMORIENT_PORTRAIT; return; }
    if (optmatch(arg, L"landscape"))     { o->orient = DMORIENT_LANDSCAPE; return; }
    if (optmatch(arg, L"duplex"))        { o->duplex = DMDUP_VERTICAL; return; }
    if (optmatch(arg, L"simplex"))       { o->duplex = DMDUP_SIMPLEX; return; }
    if (optmatch(arg, L"csv"))   { o->csv = 1; return; }
    if (optmatch(arg, L"mock"))  { o->mock = 1; return; }
    if (optmatch(arg, L"debug")) { o->debug = 1; return; }
    if (optmatch(arg, L"s"))     { o->silent = 1; return; }

    /* Recursion: case-sensitive r vs R, per the documented behavior. */
    {
        const wchar_t *a = arg; if (*a == L'/') a++;
        if (wcscmp(a, L"r") == 0) { o->recur = 1; return; }
        if (wcscmp(a, L"R") == 0) { o->recur = 0; return; }
        if ((a[0] == L'R' || a[0] == L'r') && iswdigit(a[1])) {
            o->recur = -_wtoi(a + 1); return;
        }
    }

    /* Anything else is a printer name. */
    wcsncpy(o->printername, arg, 511);
    o->printerSpecified = 1;
}

/* Load a config file: each non-blank, non-comment line is one option token. */
static int load_config(options *o, const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(h, NULL);
    char *raw = (char*)malloc(size + 2);
    DWORD got = 0; ReadFile(h, raw, size, &got, NULL);
    CloseHandle(h);
    raw[got] = 0; raw[got + 1] = 0;

    wchar_t *wtext;
    if (got >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        wtext = _wcsdup((wchar_t*)(raw + 2));        /* UTF-16LE with BOM     */
    } else {
        const char *p = raw;
        if (got >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
            && (unsigned char)p[2] == 0xBF) p += 3;  /* skip UTF-8 BOM        */
        int wn = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
        wtext = (wchar_t*)malloc(wn * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, p, -1, wtext, wn);
    }
    free(raw);

    wchar_t *ctx = NULL;
    wchar_t *line = wcstok(wtext, L"\r\n", &ctx);
    while (line) {
        while (*line == L' ' || *line == L'\t') line++;
        size_t ll = wcslen(line);
        while (ll && (line[ll - 1] == L' ' || line[ll - 1] == L'\t')) line[--ll] = 0;
        if (*line && *line != L'#' && *line != L';')
            apply_arg(o, line, 1, NULL);
        line = wcstok(NULL, L"\r\n", &ctx);
    }
    free(wtext);
    return 1;
}

/* ========================================================================= */
/*  Printer / DEVMODE setup                                                  */
/* ========================================================================= */

static HDC open_printer_dc(const options *o, const wchar_t *printerName, DEVMODEW **outDm) {
    *outDm = NULL;
    HANDLE hPrinter = NULL;
    if (!OpenPrinterW((LPWSTR)printerName, &hPrinter, NULL)) {
        /* UNC paths may still work via CreateDC even if OpenPrinter fails. */
        if (wcsncmp(printerName, L"\\\\", 2) != 0) return NULL;
    }

    DEVMODEW *dm = NULL;
    if (hPrinter) {
        LONG cb = DocumentPropertiesW(NULL, hPrinter, (LPWSTR)printerName, NULL, NULL, 0);
        if (cb > 0) {
            dm = (DEVMODEW*)malloc(cb);
            if (DocumentPropertiesW(NULL, hPrinter, (LPWSTR)printerName,
                                    dm, NULL, DM_OUT_BUFFER) == IDOK) {
                if (o->orient) { dm->dmOrientation = (short)o->orient; dm->dmFields |= DM_ORIENTATION; }
                if (o->duplex) { dm->dmDuplex      = (short)o->duplex; dm->dmFields |= DM_DUPLEX; }
                if (o->tray)   { dm->dmDefaultSource = (short)o->tray; dm->dmFields |= DM_DEFAULTSOURCE; }
                dm->dmCopies = 1; dm->dmFields |= DM_COPIES;     /* we loop copies ourselves */
                DocumentPropertiesW(NULL, hPrinter, (LPWSTR)printerName,
                                    dm, dm, DM_IN_BUFFER | DM_OUT_BUFFER);
            } else { free(dm); dm = NULL; }
        }
    }

    HDC hdc = CreateDCW(L"WINSPOOL", printerName, NULL, dm);
    if (hPrinter) ClosePrinter(hPrinter);
    if (!hdc) { if (dm) free(dm); return NULL; }
    *outDm = dm;                     /* keep for per-page ResetDC; caller frees */
    return hdc;
}

static int get_default_printer(wchar_t *out, int cch) {
    DWORD n = cch;
    return GetDefaultPrinterW(out, &n);
}

/* The printer's port name (needed by DeviceCapabilities). NULL if unavailable. */
static const wchar_t *get_printer_port(const wchar_t *printer, wchar_t *buf, int cch) {
    const wchar_t *port = NULL; HANDLE hp = NULL;
    if (OpenPrinterW((LPWSTR)printer, &hp, NULL)) {
        DWORD needed = 0; GetPrinterW(hp, 2, NULL, 0, &needed);
        if (needed) {
            BYTE *b = (BYTE*)malloc(needed);
            if (GetPrinterW(hp, 2, b, needed, &needed)) {
                PRINTER_INFO_2W *pi2 = (PRINTER_INFO_2W*)b;
                if (pi2->pPortName) { wcsncpy(buf, pi2->pPortName, cch - 1); buf[cch - 1] = 0; port = buf; }
            }
            free(b);
        }
        ClosePrinter(hp);
    }
    return port;
}

/* ----- Known paper sizes (points) for size->tray classification ---------- */
typedef struct { const wchar_t *name; int dmpaper; double w, h; } PaperSize;
static const PaperSize g_papers[] = {
    { L"letter",    DMPAPER_LETTER,     612.0,  792.0 },
    { L"legal",     DMPAPER_LEGAL,      612.0, 1008.0 },
    { L"a4",        DMPAPER_A4,         595.0,  842.0 },
    { L"a3",        DMPAPER_A3,         842.0, 1191.0 },
    { L"a5",        DMPAPER_A5,         420.0,  595.0 },
    { L"tabloid",   DMPAPER_TABLOID,    792.0, 1224.0 },
    { L"executive", DMPAPER_EXECUTIVE,  522.0,  756.0 },
    { L"statement", DMPAPER_STATEMENT,  396.0,  612.0 },
    { L"folio",     DMPAPER_FOLIO,      612.0,  936.0 },
    { L"b5",        DMPAPER_B5,         516.0,  729.0 },
    { L"a6",        DMPAPER_A6,         297.0,  420.0 },
};
static const int g_paperCount = (int)(sizeof(g_papers) / sizeof(g_papers[0]));

/* Match a page size to a known paper (orientation-independent). -1 if none. */
static int classify_paper(double wpt, double hpt) {
    double pmin = wpt < hpt ? wpt : hpt, pmax = wpt < hpt ? hpt : wpt;
    const double tol = 12.0;                 /* ~1/6 inch slack */
    for (int i = 0; i < g_paperCount; i++) {
        double smin = g_papers[i].w < g_papers[i].h ? g_papers[i].w : g_papers[i].h;
        double smax = g_papers[i].w < g_papers[i].h ? g_papers[i].h : g_papers[i].w;
        if (fabs(pmin - smin) <= tol && fabs(pmax - smax) <= tol) return i;
    }
    return -1;
}

static int tray_by_size_active(const options *o) {
    return o->traymapCount > 0 || o->autotray;
}

/* Decide the bin for a page of size (wpt,hpt). 0 = leave default. *paperIdx
   receives the g_papers index (or -1 for an unrecognized size). */
static int choose_bin_for_page(const options *o, double wpt, double hpt, int *paperIdx) {
    int idx = classify_paper(wpt, hpt);
    *paperIdx = idx;
    if (o->traymapCount && idx >= 0)
        for (int i = 0; i < o->traymapCount; i++)
            if (_wcsicmp(o->traymap[i].name, g_papers[idx].name) == 0) return o->traymap[i].bin;
    if (o->autotray) return o->autoBin;      /* driver picks tray by size */
    return 0;
}

/* The printer's "auto-select by paper size" bin: 15 (FORMSOURCE) if present,
   else 7 (AUTO), else 0. */
static int detect_auto_bin(const wchar_t *printer) {
    wchar_t portbuf[256]; const wchar_t *port = get_printer_port(printer, portbuf, 256);
    int nb = DeviceCapabilitiesW(printer, port, DC_BINS, NULL, NULL);
    if (nb <= 0) return 0;
    WORD *bins = (WORD*)malloc(nb * sizeof(WORD));
    DeviceCapabilitiesW(printer, port, DC_BINS, (LPWSTR)bins, NULL);
    int has15 = 0, has7 = 0;
    for (int i = 0; i < nb; i++) { if (bins[i] == 15) has15 = 1; if (bins[i] == 7) has7 = 1; }
    free(bins);
    return has15 ? 15 : (has7 ? 7 : 0);
}

/* /listtrays: print each of the printer's paper bins as "number = name", so the
   user can discover the right value to pass to /tray=# (HP bins are usually
   device-specific numbers >= 256, not 1/2/3). */
static int list_trays(const wchar_t *printer) {
    wchar_t portbuf[256];
    const wchar_t *port = get_printer_port(printer, portbuf, 256);

    int nb = DeviceCapabilitiesW(printer, port, DC_BINS, NULL, NULL);
    if (nb <= 0) { wprintf(L"No tray information available for \"%s\".\n", printer); return 1; }

    WORD    *bins  = (WORD*)malloc(nb * sizeof(WORD));
    wchar_t *names = (wchar_t*)malloc((size_t)nb * 24 * sizeof(wchar_t));
    DeviceCapabilitiesW(printer, port, DC_BINS,     (LPWSTR)bins, NULL);
    int nn = DeviceCapabilitiesW(printer, port, DC_BINNAMES, names, NULL);

    wprintf(L"Paper trays for \"%s\":\n", printer);
    for (int i = 0; i < nb; i++) {
        wchar_t nm[25]; wcsncpy(nm, names + (size_t)i * 24, 24); nm[24] = 0;
        wprintf(L"  /tray=%-5d  %s\n", bins[i], (i < nn) ? nm : L"");
    }
    free(bins); free(names);
    return 0;
}

/* ========================================================================= */
/*  Render one page and emit it to the printer DC                            */
/* ========================================================================= */

static int print_page(HDC hdc, DEVMODEW *dm, FPDF_DOCUMENT doc, int pageIndex, const options *o) {
    FS_SIZEF sz;
    if (!FPDF_GetPageSizeByIndexF(doc, pageIndex, &sz)) return 0;
    double wpt = sz.width, hpt = sz.height;

    /* Size-based paper source: set the bin (and paper size) for this page,
       then ResetDC so the printable area below reflects the new paper. */
    if (tray_by_size_active(o) && dm) {
        int pidx; int bin = choose_bin_for_page(o, wpt, hpt, &pidx);
        if (!o->silent)
            wprintf(L"  page %d: %s %.0fx%.0f pt -> /tray=%d\n", pageIndex + 1,
                    pidx >= 0 ? g_papers[pidx].name : L"(custom)", wpt, hpt, bin);
        if (bin) {
            dm->dmDefaultSource = (short)bin; dm->dmFields |= DM_DEFAULTSOURCE;
            if (pidx >= 0) { dm->dmPaperSize = (short)g_papers[pidx].dmpaper; dm->dmFields |= DM_PAPERSIZE; }
            ResetDCW(hdc, dm);
        }
    }

    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX), dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    int prW  = GetDeviceCaps(hdc, HORZRES),    prH  = GetDeviceCaps(hdc, VERTRES);

    int rotate = 0; double ewpt = wpt, ehpt = hpt;
    if (o->autorotate) {
        int pageLand = wpt > hpt, areaLand = prW > prH;
        if (pageLand != areaLand) { rotate = 1; ewpt = hpt; ehpt = wpt; }
    }
    double nW = ewpt / 72.0 * dpiX, nH = ehpt / 72.0 * dpiY;

    double s;
    if (o->scaleMode == SCALE_PERCENT) {
        s = o->scalePct / 100.0;
    } else {
        double fitW = prW / nW, fitH = prH / nH;
        double fit = fitW < fitH ? fitW : fitH;
        if (o->shrink && o->expand) s = fit;
        else if (o->shrink)        s = (fit < 1.0) ? fit : 1.0;
        else if (o->expand)        s = (fit > 1.0) ? fit : 1.0;
        else                       s = 1.0;
    }

    int tw = (int)(nW * s + 0.5), th = (int)(nH * s + 0.5);
    if (tw < 1) tw = 1; if (th < 1) th = 1;
    if (tw > 20000) tw = 20000; if (th > 20000) th = 20000;

    int x = o->autocenter ? (prW - tw) / 2 : 0;
    int y = o->autocenter ? (prH - th) / 2 : 0;

    size_t stride = (size_t)tw * 4;
    unsigned char *buf = (unsigned char*)malloc(stride * th);
    if (!buf) return 0;

    FPDF_BITMAP bm = FPDFBitmap_CreateEx(tw, th, FPDFBitmap_BGRx, buf, (int)stride);
    if (!bm) { free(buf); return 0; }
    FPDFBitmap_FillRect(bm, 0, 0, tw, th, 0xFFFFFFFF);

    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) { FPDFBitmap_Destroy(bm); free(buf); return 0; }
    FPDF_RenderPageBitmap(bm, page, 0, 0, tw, th, rotate, FPDF_PRINTING | FPDF_ANNOT);
    FPDF_ClosePage(page);
    FPDFBitmap_Destroy(bm);

    BITMAPINFO bmi; ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize     = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth    = tw;
    bmi.bmiHeader.biHeight   = -th;            /* top-down to match PDFium    */
    bmi.bmiHeader.biPlanes   = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    int ok = 1;
    if (StartPage(hdc) <= 0) ok = 0;
    if (ok) {
        SetStretchBltMode(hdc, COLORONCOLOR);
        if (StretchDIBits(hdc, x, y, tw, th, 0, 0, tw, th,
                           buf, &bmi, DIB_RGB_COLORS, SRCCOPY) == 0) {
            /* GDI_ERROR on some drivers if 0 scanlines; treat 0 as failure */
        }
        if (EndPage(hdc) <= 0) ok = 0;
    }
    free(buf);
    return ok;
}

/* ========================================================================= */
/*  Print one PDF file                                                       */
/* ========================================================================= */

static void split_path(const wchar_t *full, wchar_t *folder, wchar_t *name) {
    const wchar_t *slash = wcsrchr(full, L'\\');
    if (slash) {
        size_t dl = (size_t)(slash - full) + 1;
        wcsncpy(folder, full, dl); folder[dl] = 0;
        wcscpy(name, slash + 1);
    } else { wcscpy(folder, L""); wcscpy(name, full); }
}

static void print_file(HDC hdc, DEVMODEW *dm, const wchar_t *path, int index, const options *o) {
    CsvRow *row = new_row();
    row->index = index;
    row->copies = o->copies;
    split_path(path, row->folder, row->filename);
    now_string(row->datetime, 32);
    wcscpy(row->result, L"Fail");

    /* ----- read file into memory (Unicode-safe) -------------------------- */
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wcscpy(row->errorinfo, L"File not found.");
        _snwprintf(g_errors + wcslen(g_errors), 1024, L"\n%s : file not found.", path);
        return;
    }
    DWORD size = GetFileSize(h, NULL);
    void *buf = malloc(size ? size : 1);
    DWORD got = 0; ReadFile(h, buf, size, &got, NULL);
    CloseHandle(h);

    /* ----- load with PDFium --------------------------------------------- */
    char pw[256]; to_utf8(o->password, pw, sizeof(pw));
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, (int)got, o->password[0] ? pw : NULL);
    if (!doc) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD) {
            row->encrypted = 1;
            wcscpy(row->errorinfo, o->password[0] ?
                   L"Wrong password." : L"Password protected; supply /p:password.");
        } else {
            wcscpy(row->errorinfo, L"Could not open PDF (corrupt or invalid).");
        }
        _snwprintf(g_errors + wcslen(g_errors), 1024, L"\n%s : %s", path, row->errorinfo);
        free(buf);
        return;
    }

    int N = FPDF_GetPageCount(doc);
    row->pagecount = N;

    IntVec pages; ZeroMemory(&pages, sizeof(pages));
    if (!expand_pages(o->pageselector, N, &pages) || pages.n == 0) {
        wcscpy(row->errorinfo, L"Invalid or empty page selector.");
        _snwprintf(g_errors + wcslen(g_errors), 1024, L"\n%s : %s", path, row->errorinfo);
        FPDF_CloseDocument(doc); free(buf); free(pages.a);
        return;
    }
    row->pagesSelected = pages.n;

    if (o->mock) {                              /* /mock: list only, no print */
        if (tray_by_size_active(o)) {
            for (int i = 0; i < pages.n; i++) {
                FS_SIZEF sz; int pidx;
                FPDF_GetPageSizeByIndexF(doc, pages.a[i] - 1, &sz);
                int bin = choose_bin_for_page(o, sz.width, sz.height, &pidx);
                wprintf(L"  page %d: %s %.0fx%.0f pt -> /tray=%d\n", pages.a[i],
                        pidx >= 0 ? g_papers[pidx].name : L"(custom)", sz.width, sz.height, bin);
            }
        }
        wcscpy(row->result, L"Success");
        wcscpy(row->errorinfo, L"(mock - not printed)");
        FPDF_CloseDocument(doc); free(buf); free(pages.a);
        return;
    }

    /* ----- print -------------------------------------------------------- */
    DOCINFOW di; ZeroMemory(&di, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = row->filename;
    di.lpszOutput  = o->outfile[0] ? o->outfile : NULL;

    int ok = 1;
    if (StartDocW(hdc, &di) <= 0) {
        ok = 0;
    } else {
        for (int c = 0; c < o->copies && ok; c++)
            for (int i = 0; i < pages.n && ok; i++)
                if (!print_page(hdc, dm, doc, pages.a[i] - 1, o)) ok = 0;
        if (ok) EndDoc(hdc); else AbortDoc(hdc);
    }

    if (ok) { wcscpy(row->result, L"Success"); }
    else {
        wcscpy(row->errorinfo, L"Printing failed (StartDoc/render/EndDoc).");
        _snwprintf(g_errors + wcslen(g_errors), 1024, L"\n%s : %s", path, row->errorinfo);
    }

    FPDF_CloseDocument(doc); free(buf); free(pages.a);
}

/* ========================================================================= */
/*  CSV output                                                               */
/* ========================================================================= */

static void csv_field(FILE *f, const wchar_t *s) {
    int needQuote = (wcschr(s, L',') || wcschr(s, L'"') ||
                     wcschr(s, L'\n') || wcschr(s, L'\r')) != 0;
    char u8[2048];
    if (needQuote) {
        /* double internal quotes */
        wchar_t tmp[1024]; int j = 0;
        for (int i = 0; s[i] && j < 1022; i++) {
            if (s[i] == L'"') tmp[j++] = L'"';
            tmp[j++] = s[i];
        }
        tmp[j] = 0;
        to_utf8(tmp, u8, sizeof(u8));
        fprintf(f, "\"%s\"", u8);
    } else {
        to_utf8(s, u8, sizeof(u8));
        fputs(u8, f);
    }
}

static void write_csv(const options *o) {
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    if (o->mydir[0]) wcscpy(dir, o->mydir);
    else { GetTempPathW(MAX_PATH, dir); wcscat(dir, L"PDFPrinterTmp"); }
    CreateDirectoryW(dir, NULL);
    _snwprintf(file, MAX_PATH, L"%s\\summary_utf8.csv", dir);

    FILE *f = _wfopen(file, L"wb");
    if (!f) { fwprintf(stderr, L"Could not write CSV to %s\n", file); return; }
    fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);     /* UTF-8 BOM          */
    fputs("Index,Folder,Filename,Datetime,IsEncrypted,PageCount,"
          "PagesSelected,Copies,Result,ErrorInfo\n", f);
    for (int i = 0; i < g_rowCount; i++) {
        CsvRow *r = &g_rows[i];
        wchar_t num[64];
        _snwprintf(num, 64, L"%d", r->index);             csv_field(f, num); fputc(',', f);
        csv_field(f, r->folder);   fputc(',', f);
        csv_field(f, r->filename); fputc(',', f);
        csv_field(f, r->datetime); fputc(',', f);
        _snwprintf(num, 64, L"%d", r->encrypted);         csv_field(f, num); fputc(',', f);
        _snwprintf(num, 64, L"%d", r->pagecount);         csv_field(f, num); fputc(',', f);
        _snwprintf(num, 64, L"%d", r->pagesSelected);     csv_field(f, num); fputc(',', f);
        _snwprintf(num, 64, L"%d", r->copies);            csv_field(f, num); fputc(',', f);
        csv_field(f, r->result);   fputc(',', f);
        csv_field(f, r->errorinfo); fputc('\n', f);
    }
    fclose(f);
    fwprintf(stderr, L"CSV written to %s\n", file);
}

/* ========================================================================= */
/*  Usage                                                                    */
/* ========================================================================= */

static void show_usage(void) {
    wprintf(
L"PDFtoPrinter (native) - v1\n\n"
L"Usage:\n"
L"  PDFtoPrinter [path\\]file.pdf [more.pdf ...] [\"printer name\"] [pages=...]\n"
L"               [copies=#] [focus=\"title\"] [/r] [/R[#]] [/p:password]\n"
L"               [/csv] [/mock] [/s]\n"
L"               [/scale=#|fit] [/shrink-to-fit] [/expand-to-fit]\n"
L"               [/auto-rotate] [/auto-center] [/portrait] [/landscape]\n"
L"               [/duplex|/duplex=short] [/simplex] [/tray=#] [/autotray]\n"
L"               [/outfile=path] [/settings=profile.cfg] [/listtrays]\n\n"
L"Wildcards (* ?) and relative paths are OK. Multiple named files override a\n"
L"wildcard. Default printer is used unless a printer name is given.\n\n"
L"Page ranges: 3 | 2-4,6,8-9 | 8- | z-1 (reverse) | z-1:odd|even | r5-r2.\n\n"
L"Scaling: /scale=# is an explicit percent and wins over the fit flags.\n"
L"  /shrink-to-fit + /expand-to-fit together = fit either way.\n\n"
L"Trays: /tray=# selects a paper bin. /autotray, or a per-printer 'tray <size>=#'\n"
L"  config map, chooses the bin from each page's paper size. Run /listtrays\n"
L"  (optionally with a printer name) to list a printer's bin numbers and names.\n\n"
L"settings.cfg next to the EXE is auto-loaded; /settings=file selects another.\n"
L"Config lines are options minus the leading slash; # or ; comments.\n");
}

/* ========================================================================= */
/*  GUI printer picker (rename EXE to *SelectGUI*) - single-select listbox,   */
/*  matching the style of vDosSelPtr / the DOSBox-X ports.                    */
/* ========================================================================= */

#define GUI_MAX_PRINTERS 256
static wchar_t g_guiNames[GUI_MAX_PRINTERS][256];
static int     g_guiCount = 0, g_guiDefault = -1, g_guiDone = 0;
static wchar_t g_guiResult[512];
static HWND    g_guiList = NULL;

static void gui_enum_printers(void) {
    g_guiCount = 0; g_guiDefault = -1;
    DWORD needed = 0, count = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 4,
                  NULL, 0, &needed, &count);
    if (!needed) return;
    BYTE *pbuf = (BYTE*)malloc(needed);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 4,
                       pbuf, needed, &needed, &count)) { free(pbuf); return; }
    PRINTER_INFO_4W *pi = (PRINTER_INFO_4W*)pbuf;
    wchar_t def[512]; DWORD dn = 512; def[0] = 0; GetDefaultPrinterW(def, &dn);
    for (DWORD i = 0; i < count && g_guiCount < GUI_MAX_PRINTERS; i++) {
        wcsncpy(g_guiNames[g_guiCount], pi[i].pPrinterName, 255);
        g_guiNames[g_guiCount][255] = 0;
        if (def[0] && wcscmp(def, pi[i].pPrinterName) == 0) g_guiDefault = g_guiCount;
        g_guiCount++;
    }
    free(pbuf);
}

/* Bring our window to the foreground even under a foreground lock. */
static void force_foreground(HWND hwnd) {
    HWND hFore = GetForegroundWindow();
    DWORD fg = hFore ? GetWindowThreadProcessId(hFore, NULL) : 0;
    DWORD me = GetCurrentThreadId(), saved = 0;
    SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &saved, 0);
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)0, SPIF_SENDCHANGE);
    if (fg && fg != me) AttachThreadInput(fg, me, TRUE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd); ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd); SetActiveWindow(hwnd); SetFocus(hwnd);
    if (fg && fg != me) AttachThreadInput(fg, me, FALSE);
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                          (PVOID)(UINT_PTR)saved, SPIF_SENDCHANGE);
}

static LRESULT CALLBACK gui_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (g_guiList) {
                int sel = (int)SendMessageW(g_guiList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < g_guiCount) wcscpy(g_guiResult, g_guiNames[sel]);
                else g_guiResult[0] = 0;
            }
            g_guiDone = 1; return 0;
        case IDCANCEL:
            g_guiResult[0] = 0; g_guiDone = 1; return 0;
        default:
            if (HIWORD(wParam) == LBN_DBLCLK) {        /* double-click == OK */
                SendMessageW(hwnd, WM_COMMAND, IDOK, 0); return 0;
            }
            break;
        }
        break;
    case WM_CLOSE:
        g_guiResult[0] = 0; g_guiDone = 1; return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Show the picker; on OK copies the chosen name to out and returns 1, else 0. */
static int gui_select_printer(wchar_t *out, int cch) {
    gui_enum_printers();
    if (g_guiCount == 0) {
        MessageBoxW(NULL, L"No printers are available to select.", L"PDFtoPrinter", MB_OK);
        return 0;
    }
    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = gui_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));   /* app icon */
    wc.lpszClassName = L"PDFtoPrinterSelDlg";
    RegisterClassW(&wc);

    int w = 400, h = 410;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2; if (x < 0) x = 0;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2; if (y < 0) y = 0;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, L"PDFtoPrinterSelDlg",
                   L"Select a printer for this document:",
                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                   x, y, w, h, NULL, NULL, hInst, NULL);
    if (!hwnd) return 0;
    RECT rc; GetClientRect(hwnd, &rc);

    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Verdana");

    HWND hLabel = CreateWindowExW(0, L"STATIC", L"Available printers:",
                   WS_CHILD | WS_VISIBLE, 12, 10, rc.right - 24, 20, hwnd, NULL, hInst, NULL);
    g_guiList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                   12, 34, rc.right - 24, rc.bottom - 34 - 48, hwnd, (HMENU)100, hInst, NULL);
    HWND hOK = CreateWindowExW(0, L"BUTTON", L"OK",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                   rc.right - 180, rc.bottom - 38, 80, 28, hwnd, (HMENU)IDOK, hInst, NULL);
    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                   rc.right - 92, rc.bottom - 38, 80, 28, hwnd, (HMENU)IDCANCEL, hInst, NULL);

    SendMessageW(hLabel,    WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_guiList, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(hOK,       WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(hCancel,   WM_SETFONT, (WPARAM)font, TRUE);

    for (int i = 0; i < g_guiCount; i++)
        SendMessageW(g_guiList, LB_ADDSTRING, 0, (LPARAM)g_guiNames[i]);
    SendMessageW(g_guiList, LB_SETCURSEL, (g_guiDefault >= 0 ? g_guiDefault : 0), 0);

    ShowWindow(hwnd, SW_SHOW);
    force_foreground(hwnd);
    SetFocus(g_guiList);

    g_guiDone = 0; MSG msg;
    while (!g_guiDone && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DestroyWindow(hwnd);
    g_guiList = NULL;
    if (font) DeleteObject(font);

    if (g_guiResult[0] == 0) return 0;
    wcsncpy(out, g_guiResult, cch - 1); out[cch - 1] = 0;
    return 1;
}

/* ========================================================================= */
/*  Diagnostic: render page 1 of a PDF to a BMP (no printer, no dialog).      */
/*  Usage:  PDFtoPrinterNative --selftest in.pdf out.bmp [dpi]                */
/* ========================================================================= */

static int write_bmp32(const wchar_t *path, const unsigned char *buf,
                       int w, int h, int stride) {
    BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof(fh));
    BITMAPINFOHEADER ih; ZeroMemory(&ih, sizeof(ih));
    DWORD imgBytes = (DWORD)stride * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imgBytes;
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h;  /* top-down */
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(buf, imgBytes, 1, f);
    fclose(f);
    return 1;
}

static int do_selftest(const wchar_t *pdf, const wchar_t *bmp, int dpi) {
    FPDF_InitLibrary();
    HANDLE h = CreateFileW(pdf, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { wprintf(L"selftest: cannot open %s\n", pdf); return 1; }
    DWORD size = GetFileSize(h, NULL);
    void *buf = malloc(size ? size : 1);
    DWORD got = 0; ReadFile(h, buf, size, &got, NULL); CloseHandle(h);

    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, (int)got, NULL);
    if (!doc) { wprintf(L"selftest: FPDF_LoadMemDocument failed err=%lu\n", FPDF_GetLastError()); free(buf); return 1; }

    FS_SIZEF sz; FPDF_GetPageSizeByIndexF(doc, 0, &sz);
    int w = (int)(sz.width / 72.0 * dpi + 0.5);
    int hh = (int)(sz.height / 72.0 * dpi + 0.5);
    int stride = w * 4;
    unsigned char *pix = (unsigned char*)malloc((size_t)stride * hh);
    FPDF_BITMAP bm = FPDFBitmap_CreateEx(w, hh, FPDFBitmap_BGRx, pix, stride);
    FPDFBitmap_FillRect(bm, 0, 0, w, hh, 0xFFFFFFFF);
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FPDF_RenderPageBitmap(bm, page, 0, 0, w, hh, 0, FPDF_PRINTING | FPDF_ANNOT);
    FPDF_ClosePage(page);

    /* count non-white pixels so we can assert the page actually has ink */
    long ink = 0;
    for (int i = 0; i < w * hh; i++) {
        const unsigned char *p = pix + (size_t)i * 4;
        if (p[0] < 250 || p[1] < 250 || p[2] < 250) ink++;
    }
    write_bmp32(bmp, pix, w, hh, stride);
    wprintf(L"selftest: page1 size=%.0fx%.0f pt -> %dx%d px @ %d dpi, ink pixels=%ld -> %s\n",
            sz.width, sz.height, w, hh, dpi, ink, bmp);

    FPDFBitmap_Destroy(bm); free(pix);
    FPDF_CloseDocument(doc); free(buf);
    FPDF_DestroyLibrary();
    return ink > 0 ? 0 : 2;
}

/* ========================================================================= */
/*  main                                                                     */
/* ========================================================================= */

static void exe_dir(wchar_t *out) {
    GetModuleFileNameW(NULL, out, MAX_PATH);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
}
static int exe_name_has(const wchar_t *needle) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
    _wcslwr(base);
    return wcsstr(base, needle) != NULL;
}

/* GUI help text — flowing paragraphs, blank-line breaks only (no hard wraps). */
static const wchar_t *g_guiHelp =
L"PDFtoPrinter prints PDF files to a Windows printer from the command line.\r\n\r\n"
L"Usage: PDFtoPrinter [path\\]filename.pdf [other filenames] [\"printer name\"] "
L"[pages=...] [copies=#] [focus=\"window title\"] [options]\r\n\r\n"
L"Use quotation marks around a path or filename that contains spaces. Relative paths "
L"and filename wildcards (* and ?) are OK. If you give more than one filename, do not "
L"use a wildcard or /r or /R.\r\n\r\n"
L"The default printer is used unless you name one. Rename the program to "
L"PDFtoPrinterSelect.exe for a text menu of printers, or PDFtoPrinterSelectGUI.exe for "
L"a list-box menu (no menu appears if a printer name is given).\r\n\r\n"
L"Page ranges: 3, or 2-4,6,8-9, or 8-, or z-1 for reverse order, or z-1:odd or z-1:even, "
L"or r5-r2 for fifth-from-last to second-from-last.\r\n\r\n"
L"Scaling and layout: /scale=# sets an exact percentage and overrides the fit options. "
L"/shrink-to-fit shrinks oversized pages; /expand-to-fit enlarges small pages; using both "
L"fits either way. /auto-rotate turns landscape pages to match the paper; /auto-center "
L"centers a page smaller than the sheet; /portrait and /landscape force an orientation.\r\n\r\n"
L"Two-sided: /duplex prints both sides on the long edge, /duplex=short uses the short edge, "
L"/simplex forces single-sided.\r\n\r\n"
L"Paper source: /tray=# selects a paper bin. /autotray, or a per-printer settings file with "
L"\"tray <size>=#\" lines (for example tray legal=258), chooses the bin automatically from each "
L"page's paper size. Run /listtrays (optionally with a printer name) to see the bin numbers and "
L"names for a printer.\r\n\r\n"
L"Other options: /r recurses the current folder; /R# recurses # levels; /p:password opens an "
L"encrypted PDF; /csv writes a list of files printed; /mock lists files without printing; "
L"/s runs silently; /outfile=path prints to a file.\r\n\r\n"
L"Settings files: settings.cfg next to the program loads automatically; /settings=file.cfg "
L"loads another. Each line is one option without the leading slash; lines starting with # or "
L"; are comments.";

int wmain(int argc, wchar_t **argv) {
    /* GUI-subsystem build: no console window is created on double-click. When
       launched from a shell, attach to that console so text output still works;
       when stdout is redirected to a pipe/file, leave it alone. "commandline"
       is false only for a true Explorer double-click -> show GUI help instead. */
    FILE  *fdummy;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  ft   = hOut ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
    int    redirected = (ft == FILE_TYPE_DISK || ft == FILE_TYPE_PIPE);
    int    attached   = AttachConsole(ATTACH_PARENT_PROCESS) ? 1 : 0;
    if (attached && !redirected) {
        freopen_s(&fdummy, "CONOUT$", "w", stdout);
        freopen_s(&fdummy, "CONOUT$", "w", stderr);
        freopen_s(&fdummy, "CONIN$",  "r", stdin);
    }
    int commandline = attached || redirected;

    if (argc < 2) {
        if (commandline) show_usage();
        else MessageBoxW(NULL, g_guiHelp, L"PDFtoPrinter", MB_OK | MB_ICONINFORMATION);
        return EXIT_NO_ARGS;
    }

    if (wcscmp(argv[1], L"--selftest") == 0 && argc >= 4)
        return do_selftest(argv[2], argv[3], argc >= 5 ? _wtoi(argv[4]) : 150);

    options o; ZeroMemory(&o, sizeof(o));
    o.copies = 1;
    o.recur = 1;
    o.scaleMode = SCALE_FLAGS;
    o.scalePct = 100.0;

    WideVec fileArgs; ZeroMemory(&fileArgs, sizeof(fileArgs));

    /* 1) defaults already set.  2) settings.cfg next to exe. */
    wchar_t dir[MAX_PATH], cfg[MAX_PATH];
    exe_dir(dir);
    _snwprintf(cfg, MAX_PATH, L"%ssettings.cfg", dir);
    load_config(&o, cfg);

    /* 3) /settings= profile from the command line (layered on top). */
    for (int i = 1; i < argc; i++) {
        const wchar_t *rest = optprefix(argv[i], L"settings=");
        if (rest) {
            wchar_t path[MAX_PATH];
            if (wcschr(rest, L'\\') || wcschr(rest, L'/') || (rest[0] && rest[1] == L':'))
                GetFullPathNameW(rest, MAX_PATH, path, NULL);
            else
                _snwprintf(path, MAX_PATH, L"%s%s", dir, rest);   /* next to exe */
            if (!load_config(&o, path))
                fwprintf(stderr, L"Warning: could not read settings file %s\n", path);
        }
    }

    /* 4) command line overrides everything. */
    for (int i = 1; i < argc; i++)
        apply_arg(&o, argv[i], 0, &fileArgs);

    /* /listtrays: enumerate the printer's bins and exit (no PDF needed). */
    if (o.listtrays) {
        wchar_t pn[512];
        if (o.printerSpecified) wcscpy(pn, o.printername);
        else if (!get_default_printer(pn, 512)) { msg(&o, L"No default printer found."); return EXIT_NO_PRINTER; }
        return list_trays(pn);
    }

    if (fileArgs.n == 0) { msg(&o, L"No [path]filename.pdf provided."); return EXIT_NO_PDF_ARG; }

    /* ----- resolve the printer ------------------------------------------ */
    wchar_t printerName[512];
    if (o.printerSpecified) {
        wcscpy(printerName, o.printername);
    } else if (!o.silent && exe_name_has(L"selectgui")) {
        /* GUI listbox picker (rename EXE to *SelectGUI*) */
        if (!gui_select_printer(printerName, 512)) return EXIT_NO_PRINTER;
    } else if (!o.silent && exe_name_has(L"select")) {
        /* console numbered select menu (rename EXE to *Select*) */
        DWORD needed = 0, count = 0;
        EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 4,
                      NULL, 0, &needed, &count);
        BYTE *pbuf = (BYTE*)malloc(needed ? needed : 1);
        EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 4,
                      pbuf, needed, &needed, &count);
        PRINTER_INFO_4W *pi = (PRINTER_INFO_4W*)pbuf;
        wprintf(L"Select a printer:\n");
        for (DWORD i = 0; i < count; i++) wprintf(L"  %lu) %s\n", i + 1, pi[i].pPrinterName);
        wprintf(L"> "); fflush(stdout);
        wchar_t line[32]; int sel = 0;
        if (fgetws(line, 32, stdin)) sel = _wtoi(line);
        if (sel < 1 || (DWORD)sel > count) { free(pbuf); return EXIT_NO_PRINTER; }
        wcscpy(printerName, pi[sel - 1].pPrinterName);
        free(pbuf);
    } else {
        if (!get_default_printer(printerName, 512)) {
            msg(&o, L"No default printer found.");
            return EXIT_NO_PRINTER;
        }
    }

    /* ----- expand the file list ----------------------------------------- */
    WideVec files; ZeroMemory(&files, sizeof(files));
    if (fileArgs.n > 1) {
        for (int i = 0; i < fileArgs.n; i++) {              /* named files win */
            wchar_t full[MAX_PATH];
            GetFullPathNameW(fileArgs.a[i], MAX_PATH, full, NULL);
            wv_push(&files, full);
        }
    } else {
        expand_one_file_arg(fileArgs.a[0], o.recur, &files);
    }
    if (files.n == 0) { msg(&o, L"No PDF file matched."); return EXIT_NO_PDF_MATCH; }

    /* ----- size->tray: detect the printer's auto-by-size bin if asked --- */
    if (o.autotray) {
        o.autoBin = detect_auto_bin(printerName);
        if (!o.autoBin)
            fwprintf(stderr, L"Warning: \"%s\" reports no auto-by-size bin; /autotray will do nothing.\n", printerName);
    }

    /* ----- open the printer DC (once) ----------------------------------- */
    FPDF_InitLibrary();
    HDC hdc = NULL; DEVMODEW *dm = NULL;
    if (!o.mock) {
        hdc = open_printer_dc(&o, printerName, &dm);
        if (!hdc) {
            fwprintf(stderr, L"Could not open printer \"%s\".\n", printerName);
            FPDF_DestroyLibrary();
            return EXIT_PRINTER_INVALID;
        }
    }

    for (int i = 0; i < files.n; i++)
        print_file(hdc, dm, files.a[i], i + 1, &o);

    if (hdc) DeleteDC(hdc);
    if (dm) free(dm);
    FPDF_DestroyLibrary();

    if (o.csv || o.mock) write_csv(&o);

    /* restore focus if asked (and not silent) */
    if (o.focus[0] && !o.silent) {
        HWND hw = FindWindowW(NULL, o.focus);
        if (hw) SetForegroundWindow(hw);
    }

    if (g_errors[0]) {
        fwprintf(stderr, L"Errors:%s\n", g_errors);
        return EXIT_PRINT_ERRORS;
    }
    return EXIT_OK;
}
