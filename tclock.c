/*
 * tclock — a pretty little ncurses dashboard for *nix terminals.
 *
 *   ┌ tclock ────────────────────────────────────────────┐
 *   │                                                    │
 *   │              ██ ██  ███     ██ ██  ███             │
 *   │              ██ ██     █    ██ ██     █            │
 *   │              ██ ██  ███     ██ ██  ███             │
 *   │              ██ ██  █          █  █                │
 *   │              ██ ██  ████     ██ ██  ████           │
 *   │                                                    │
 *   │            Sunday, October 27, 2026                │
 *   │        day 300 of 2026 · week 43 · UTC             │
 *   │                                                    │
 *   │   ─────────────── CPU · 8 threads ───────────────  │
 *   │     ALL  [████████░░░░░░░░░░░░]  23.4%             │
 *   │     T0 [██████░░░░] 41%    T1 [███░░░░░] 22%       │
 *   │   ──────────────────── memory ───────────────────  │
 *   │     MEM  [█████████░░░░░░░] 9.4 / 31.3 GiB · 30%   │
 *   │     load 0.42 0.38 0.31 · up 3d 02:14 ...          │
 *   └────────────────────────────────────────────────────┘
 *
 * Big block-glyph clock with blinking colons, full date, per-thread
 * CPU load and memory/swap meters — fits an 80x24 terminal.
 *
 * Build:  make
 * Run:    ./tclock
 * Keys:   q quit · r reset peak markers
 *
 * Statistics come from /proc (Linux). On other *nix the clock still
 * runs; the stats panels show a friendly "unavailable" note instead.
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

#include <ctype.h>
#include <langinfo.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include <ncurses.h>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#define FRAME_MS  500
#define MAX_CORES 64
#define APP_TITLE " tclock "

/* ------------------------------------------------------------------ */
/* color pairs                                                         */
/* ------------------------------------------------------------------ */
enum {
    CP_BORDER = 1, CP_TITLE, CP_CLOCK, CP_COLON, CP_DATE, CP_SUB,
    CP_LABEL, CP_TEXT, CP_HINT, CP_OK, CP_WARN, CP_CRIT
};

/* ------------------------------------------------------------------ */
/* 5x5 block font: one byte per row, bit 4 = leftmost column           */
/* ------------------------------------------------------------------ */
static const uint8_t FONT[10][5] = {
    {0x1F, 0x11, 0x11, 0x11, 0x1F}, /* 0 */
    {0x06, 0x04, 0x04, 0x04, 0x1F}, /* 1 */
    {0x1F, 0x01, 0x1F, 0x10, 0x1F}, /* 2 */
    {0x1F, 0x01, 0x1F, 0x01, 0x1F}, /* 3 */
    {0x11, 0x11, 0x1F, 0x01, 0x01}, /* 4 */
    {0x1F, 0x10, 0x1F, 0x01, 0x1F}, /* 5 */
    {0x1F, 0x10, 0x1F, 0x11, 0x1F}, /* 6 */
    {0x1F, 0x01, 0x01, 0x01, 0x01}, /* 7 */
    {0x1F, 0x11, 0x1F, 0x11, 0x1F}, /* 8 */
    {0x1F, 0x11, 0x1F, 0x01, 0x1F}, /* 9 */
};
static const uint8_t FONT_COLON[5] = {0x00, 0x02, 0x00, 0x02, 0x00};

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */
static bool         g_color = false;
static bool         g_utf8  = false;
static const char  *SEP     = "|";           /* "·" on UTF-8 terminals  */
static double       g_peak_cpu = 0.0;
static double       g_peak_mem = 0.0;

typedef struct { unsigned long long tot, idle; } sample_t;
static sample_t g_prev[MAX_CORES + 1];            /* [0] = aggregate cpu */
static bool     g_have_prev = false;

typedef struct {
    long total, avail, mfree, swaptot, swapfree;  /* kB units */
} mem_t;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

/* display width of a UTF-8 string in terminal cells */
static int uwidth(const char *s)
{
    wchar_t w[256];
    size_t n = mbstowcs(w, s, 256);
    if (n == (size_t)-1)
        return (int)strlen(s);
    int tot = 0;
    for (size_t i = 0; i < n && i < 256; i++) {
        int cw = wcwidth(w[i]);
        tot += cw > 0 ? cw : 0;
    }
    return tot;
}

static void putblock(void)
{
    if (g_utf8) addstr("█");
    else        addch(ACS_BLOCK);
}

static void putempty(void)
{
    if (g_utf8) addstr("░");
    else        addch(' ');
}

static int levelpair(double pct)
{
    return pct >= 80.0 ? CP_CRIT : (pct >= 50.0 ? CP_WARN : CP_OK);
}

static void fmt_pair(long used, long total, char *buf, size_t n)
{
    if (total >= 1024L * 1024)
        snprintf(buf, n, "%.1f / %.1f GiB",
                 used / (1024.0 * 1024.0), total / (1024.0 * 1024.0));
    else if (total >= 1024)
        snprintf(buf, n, "%.0f / %.0f MiB", used / 1024.0, total / 1024.0);
    else
        snprintf(buf, n, "%ld / %ld KiB", used, total);
}

static void fmt_uptime(long s, char *buf, size_t n)
{
    long d = s / 86400;
    s %= 86400;
    int h = (int)(s / 3600), m = (int)((s % 3600) / 60);
    if (d > 0) snprintf(buf, n, "%ldd %02d:%02d", d, h, m);
    else       snprintf(buf, n, "%02d:%02d", h, m);
}

/* ------------------------------------------------------------------ */
/* /proc readers (Linux; graceful stubs elsewhere)                     */
/* ------------------------------------------------------------------ */
#ifdef __linux__

/* per-thread + aggregate CPU busy%, delta-sampled between calls */
static bool read_cpu(double *overall, double *per, int *nper)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return false;

    unsigned long long tot[MAX_CORES + 1] = {0};
    unsigned long long idl[MAX_CORES + 1] = {0};
    int  n = 0;
    bool got_agg = false;
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0)
            continue;
        const char *p = line + 3;
        int slot;
        if (*p == ' ' || *p == '\t') {           /* aggregate "cpu" line */
            slot = 0;
            p++;
        } else if (isdigit((unsigned char)*p)) { /* "cpuN" line */
            char *end;
            long idx = strtol(p, &end, 10);
            if (idx < 0 || idx >= MAX_CORES)
                continue;
            slot = (int)idx + 1;
            p = end;
        } else {
            continue;
        }

        unsigned long long v[10] = {0};
        int nv = 0;
        while (nv < 10) {
            char *end;
            unsigned long long x = strtoull(p, &end, 10);
            if (end == p)
                break;
            v[nv++] = x;
            p = end;
        }
        if (nv == 0)
            continue;

        unsigned long long t = 0;
        for (int i = 0; i < nv; i++)
            t += v[i];
        unsigned long long idle = 0;
        if (nv > 4)      idle = v[3] + v[4];     /* idle + iowait */
        else if (nv > 3) idle = v[3];

        tot[slot] = t;
        idl[slot] = idle;
        if (slot == 0) got_agg = true;
        else if (slot > n) n = slot;
    }
    fclose(f);

    if (!got_agg)
        return false;

    double agg = 0.0;
    if (g_have_prev && tot[0] > g_prev[0].tot) {
        unsigned long long dt = tot[0] - g_prev[0].tot;
        unsigned long long di = (idl[0] >= g_prev[0].idle)
                                ? idl[0] - g_prev[0].idle : 0;
        if (dt > 0)
            agg = 100.0 * (double)(dt - di) / (double)dt;
    }
    if (agg < 0.0)   agg = 0.0;
    if (agg > 100.0) agg = 100.0;

    for (int i = 0; i < n; i++) {
        int s = i + 1;
        double val = 0.0;
        if (g_have_prev && tot[s] > g_prev[s].tot) {
            unsigned long long dt = tot[s] - g_prev[s].tot;
            unsigned long long di = (idl[s] >= g_prev[s].idle)
                                    ? idl[s] - g_prev[s].idle : 0;
            if (dt > 0)
                val = 100.0 * (double)(dt - di) / (double)dt;
        }
        if (val < 0.0)   val = 0.0;
        if (val > 100.0) val = 100.0;
        per[i] = val;
    }

    for (int s = 0; s <= n; s++) {
        g_prev[s].tot  = tot[s];
        g_prev[s].idle = idl[s];
    }
    g_have_prev = true;
    *overall = agg;
    *nper    = n;
    return true;
}

static bool read_mem(mem_t *m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return false;
    char line[256];
    memset(m, 0, sizeof(*m));
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        long val;
        if (sscanf(line, "%63s %ld", key, &val) != 2)
            continue;
        if      (!strcmp(key, "MemTotal:"))    m->total    = val;
        else if (!strcmp(key, "MemAvailable:")) m->avail   = val;
        else if (!strcmp(key, "MemFree:"))      m->mfree   = val;
        else if (!strcmp(key, "SwapTotal:"))    m->swaptot = val;
        else if (!strcmp(key, "SwapFree:"))     m->swapfree = val;
    }
    fclose(f);
    return m->total > 0;
}

static bool read_load(double ld[3], long *trun, long *ttot)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f)
        return false;
    bool ok = fscanf(f, "%lf %lf %lf %ld/%ld",
                     &ld[0], &ld[1], &ld[2], trun, ttot) == 5;
    fclose(f);
    return ok;
}

static bool read_uptime(long *up, int *procs)
{
    struct sysinfo si;
    if (sysinfo(&si) != 0)
        return false;
    *up    = (long)si.uptime;
    *procs = (int)si.procs;
    return true;
}

#else /* !__linux__ — clock-only mode */

static bool read_cpu(double *o, double *p, int *n)
{
    (void)o; (void)p; (void)n; return false;
}
static bool read_mem(mem_t *m)                     { (void)m; return false; }
static bool read_load(double ld[3], long *a, long *b)
{
    (void)ld; (void)a; (void)b; return false;
}
static bool read_uptime(long *u, int *pr)
{
    (void)u; (void)pr; return false;
}

#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */
/* ------------------------------------------------------------------ */

static void text_at(int y, int x, const char *s, int pair, attr_t extra)
{
    if (y < 0 || y >= LINES || x < 0 || x >= COLS)
        return;
    attr_t a = g_color ? (COLOR_PAIR(pair) | extra) : extra;
    if (!g_color && pair == CP_CRIT)
        a |= A_REVERSE;
    attron(a);
    mvaddstr(y, x, s);
    attroff(a);
}

static void label_at(int y, int x, const char *s)
{
    text_at(y, x, s, CP_LABEL, A_BOLD);
}

static void cprint(int y, const char *s, int pair, attr_t extra)
{
    int x = (COLS - uwidth(s)) / 2;
    if (x < 1)
        x = 1;
    text_at(y, x, s, pair, extra);
}

/* solid progress bar with dim texture for the empty part; optional
 * yellow '>' peak marker */
static void draw_bar(int y, int x, int w, double pct, int pair,
                     bool peakmark, double peak)
{
    if (y < 1 || y >= LINES - 1)
        return;
    if (w > COLS - 1 - x)
        w = COLS - 1 - x;
    if (w <= 0)
        return;

    int fill = (int)(pct / 100.0 * w + 0.5);
    if (fill < 0) fill = 0;
    if (fill > w) fill = w;

    if (x >= 1)
        move(y, x);

    attr_t fa = g_color ? (COLOR_PAIR(pair) | A_BOLD) : A_REVERSE;
    attron(fa);
    for (int i = 0; i < fill; i++)
        putblock();
    attroff(fa);

    attr_t ea = g_color ? (COLOR_PAIR(CP_HINT) | A_DIM) : A_NORMAL;
    attron(ea);
    for (int i = fill; i < w; i++)
        putempty();
    attroff(ea);

    if (peakmark && peak > 1.0) {
        int px = (int)(peak / 100.0 * w + 0.5) - 1;
        if (px < 0)     px = 0;
        if (px > w - 1) px = w - 1;
        attr_t pa = g_color ? (COLOR_PAIR(CP_DATE) | A_BOLD) : A_BOLD;
        attron(pa);
        mvaddch(y, x + px, '>');
        attroff(pa);
    }
}

/* horizontal rule with an embedded centered label:
 *   ─────────── CPU · 8 threads ───────────                      */
static void draw_sep(int y, const char *label)
{
    if (y < 1 || y >= LINES - 1)
        return;
    int W = COLS - 6;
    if (W > 76)
        W = 76;
    int lw = uwidth(label) + 2;
    if (W < lw + 4)
        W = lw + 4;
    if (W > COLS - 2)
        W = COLS - 2;
    int x = (COLS - W) / 2;
    if (x < 1)
        x = 1;

    attr_t la = g_color ? (COLOR_PAIR(CP_HINT) | A_DIM) : A_DIM;
    attron(la);
    mvhline(y, x, ACS_HLINE, W);
    attroff(la);

    int lx = x + (W - lw) / 2;
    if (lx >= 1 && lx + lw + 1 < COLS) {
        attr_t ta = g_color ? (COLOR_PAIR(CP_LABEL) | A_BOLD) : A_BOLD;
        attron(ta);
        mvprintw(y, lx, " %s ", label);
        attroff(ta);
    }
}

/* the star of the show: HH:MM:SS in 5x5 block glyphs.
 * cell layout: D D : D D : D D  ->  43 columns wide            */
static void draw_bigtime(int y, int cx, const struct tm *tm,
                         bool colon_on, int hrep, int vrep)
{
    int slots[8] = { tm->tm_hour / 10, tm->tm_hour % 10, 10,
                     tm->tm_min / 10,  tm->tm_min % 10,  10,
                     tm->tm_sec / 10,  tm->tm_sec % 10 };
    int x0 = cx - (43 * hrep) / 2;
    if (x0 < 1)
        x0 = 1;
    if (y < 1 || y + 5 * vrep >= LINES)
        return;

    for (int r = 0; r < 5; r++) {
        for (int v = 0; v < vrep; v++) {
            move(y + r * vrep + v, x0);
            for (int s = 0; s < 8; s++) {
                bool colon = (slots[s] == 10);
                int  w     = colon ? 3 : 5;
                uint8_t bits = colon ? (colon_on ? FONT_COLON[r] : 0)
                                     : FONT[slots[s]][r];
                attr_t a = g_color
                    ? (COLOR_PAIR(colon ? CP_COLON : CP_CLOCK) | A_BOLD)
                    : A_BOLD;
                attron(a);
                for (int c = 0; c < w; c++) {
                    bool on = (bits >> (w - 1 - c)) & 1;
                    for (int h = 0; h < hrep; h++) {
                        if (on) putblock();
                        else    addstr(" ");
                    }
                }
                attroff(a);
                if (s < 7) {                 /* breathing room between glyphs */
                    for (int h = 0; h < hrep; h++)
                        addstr(" ");
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* one full frame                                                      */
/* ------------------------------------------------------------------ */
static void draw_frame(int frame)
{
    erase();

    /* ---- gather data ---- */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    double overall = 0.0;
    double per[MAX_CORES] = {0};
    int    n = 0;
    mem_t  mem = {0};

#ifdef __linux__
    bool cpu_ok  = read_cpu(&overall, per, &n);
    bool mem_ok  = read_mem(&mem);
    double ld[3] = {0.0, 0.0, 0.0};
    long thr_r = 0, thr_t = 0, up_s = 0;
    int  procs = 0;
    bool load_ok = read_load(ld, &thr_r, &thr_t);
    bool up_ok   = read_uptime(&up_s, &procs);
#else
    bool cpu_ok = false, mem_ok = false, load_ok = false, up_ok = false;
    double ld[3] = {0.0, 0.0, 0.0};
    long thr_r = 0, thr_t = 0, up_s = 0;
    int  procs = 0;
#endif

    double mem_pct = 0.0;
    long   mem_used = 0;
    if (mem_ok && mem.total > 0) {
        mem_used = mem.total - (mem.avail > 0 ? mem.avail : mem.mfree);
        if (mem_used < 0)
            mem_used = 0;
        mem_pct = 100.0 * (double)mem_used / (double)mem.total;
    }
    if (cpu_ok && overall > g_peak_cpu)
        g_peak_cpu = overall;
    if (mem_ok && mem.total > 0 && mem_pct > g_peak_mem)
        g_peak_mem = mem_pct;

    /* ---- chrome: border, title chip, hostname, hint ---- */
    attr_t ba = g_color ? (COLOR_PAIR(CP_BORDER) | A_BOLD) : A_NORMAL;
    attron(ba);
    box(stdscr, 0, 0);
    attroff(ba);

    attr_t ta = g_color ? (COLOR_PAIR(CP_TITLE) | A_BOLD) : A_REVERSE;
    attron(ta);
    mvprintw(0, 2, APP_TITLE);
    attroff(ta);

    char hn[128] = "";
    if (gethostname(hn, sizeof(hn) - 1) == 0 && hn[0]) {
        char hlab[140];
        snprintf(hlab, sizeof(hlab), " %s ", hn);
        int hw = uwidth(hlab);
        int hx = COLS - hw - 1;
        if (hx > 12) {
            attron(COLOR_PAIR(CP_HINT) | A_DIM);
            mvaddstr(0, hx, hlab);
            attroff(COLOR_PAIR(CP_HINT) | A_DIM);
        }
    }

    char hint[64];
    snprintf(hint, sizeof(hint), " q quit %s r reset peaks ", SEP);
    attron(COLOR_PAIR(CP_HINT) | A_DIM);
    mvaddstr(LINES - 1, 2, hint);
    attroff(COLOR_PAIR(CP_HINT) | A_DIM);

    /* ---- too small? ---- */
    int H = LINES - 2;
    if (COLS < 46 || H < 10) {
        erase();
        attron(A_BOLD);
        mvaddstr(0, 1, "tclock");
        attroff(A_BOLD);
        mvprintw(1, 1, "terminal too small: need >= 46x12, have %dx%d",
                 COLS, LINES);
        refresh();
        return;
    }

    /* ---- layout budget: drop cosmetic rows before data rows ---- */
    int per_row  = (COLS >= 112) ? 4 : (COLS < 56 ? 1 : 2);
    int barw     = (COLS < 60) ? 7 : (COLS < 76 ? 9 : 12);
    int barw_all = barw + 8;

    int cores_shown = n;
    if (cores_shown > 16)
        cores_shown = 16;
    int core_rows = per_row > 0 ? (cores_shown + per_row - 1) / per_row : 0;

    bool scale2 = (COLS >= 92);            /* big clock, double size */
    int  vrep = scale2 ? 2 : 1, hrep = scale2 ? 2 : 1;

    bool swapline = mem_ok && mem.swaptot > 0;
    bool foot     = load_ok || up_ok;

    bool pad1 = true, pad2 = true, pad3 = true, pad4 = true, pad5 = true;
    bool secbar = true, sub = true, allrow = true;

    int need = pad1 + 5 * vrep + secbar + pad2 + 1 /*date*/ + sub + pad3
             + 1 /*cpu sep*/ + (cpu_ok ? 1 + core_rows : 1)
             + pad4 + 1 /*mem sep*/ + (mem_ok ? 1 + swapline : 1)
             + pad5 + foot;

    while (need > H) {
        if (pad1)                       { pad1 = false; need--; }
        else if (pad5)                  { pad5 = false; need--; }
        else if (scale2)                { scale2 = false; vrep = hrep = 1; need -= 5; }
        else if (secbar)                { secbar = false; need--; }
        else if (sub)                   { sub = false; need--; }
        else if (foot)                  { foot = false; need--; }
        else if (swapline)              { swapline = false; need--; }
        else if (pad4)                  { pad4 = false; need--; }
        else if (pad3)                  { pad3 = false; need--; }
        else if (pad2)                  { pad2 = false; need--; }
        else if (cores_shown > per_row) { cores_shown -= per_row; core_rows--; need--; }
        else if (cores_shown > 0)       { cores_shown = 0; core_rows = 0; need--; }
        else if (allrow)                { allrow = false; need--; }
        else break;
    }

    /* ---- draw (vertically centered when there is spare room) ---- */
    int cx = COLS / 2;
    int y  = 1;
    int spare = H - need;
    if (spare > 0)
        y += spare / 2;
    if (pad1)
        y++;

    char datebuf[128], subbuf[160];
    strftime(datebuf, sizeof(datebuf), "%A, %B %d, %Y", &tmv);
    char wk[8] = "?", tz[16] = "";
    strftime(wk, sizeof(wk), "%V", &tmv);
    strftime(tz, sizeof(tz), "%Z", &tmv);
    if (wk[0] == '\0') snprintf(wk, sizeof(wk), "?");
    if (tz[0])
        snprintf(subbuf, sizeof(subbuf), "day %d of %d %s week %s %s %s",
                 tmv.tm_yday + 1, tmv.tm_year + 1900, SEP, wk, SEP, tz);
    else
        snprintf(subbuf, sizeof(subbuf), "day %d of %d %s week %s",
                 tmv.tm_yday + 1, tmv.tm_year + 1900, SEP, wk);

    draw_bigtime(y, cx, &tmv, (frame & 1) == 0, hrep, vrep);
    y += 5 * vrep;

    /* thin minute-progress bar under the clock */
    if (secbar) {
        int w = 43 * hrep;
        if (w > COLS - 4)
            w = COLS - 4;
        draw_bar(y, cx - w / 2, w, tmv.tm_sec * 100.0 / 60.0,
                 CP_COLON, false, 0.0);
        y++;
    }
    if (pad2) y++;
    cprint(y, datebuf, CP_DATE, A_BOLD);
    y++;
    if (sub) { cprint(y, subbuf, CP_SUB, A_DIM); y++; }
    if (pad3) y++;

    /* ---- cpu section ---- */
    char lbl[96];
    if (n > 0) {
        if (cores_shown > 0 && cores_shown < n)
            snprintf(lbl, sizeof(lbl), "CPU %s %d of %d threads",
                     SEP, cores_shown, n);
        else
            snprintf(lbl, sizeof(lbl), "CPU %s %d threads", SEP, n);
    } else {
        snprintf(lbl, sizeof(lbl), "CPU");
    }
    draw_sep(y, lbl);
    y++;

    if (cpu_ok) {
        if (allrow) {
            char p1[32], p2[32];
            snprintf(p1, sizeof(p1), " %5.1f%%", overall);
            if (g_peak_cpu > 1.0)
                snprintf(p2, sizeof(p2), "  peak %3.0f%%", g_peak_cpu);
            else
                p2[0] = '\0';
            int uw1 = uwidth(p1), uw2 = uwidth(p2);
            int bw = barw_all;
            int avail = COLS - 2 - 5 - uw1 - uw2;
            if (bw > avail) bw = avail;
            if (bw < 0) bw = 0;
            int tw = 5 + bw + uw1 + uw2;
            int x = cx - tw / 2;
            if (x < 1) x = 1;

            label_at(y, x, "ALL  ");
            draw_bar(y, x + 5, bw, overall, levelpair(overall), true, g_peak_cpu);
            text_at(y, x + 5 + bw, p1, levelpair(overall), A_BOLD);
            if (p2[0])
                text_at(y, x + 5 + bw + uw1, p2, CP_HINT, A_DIM);
            y++;
        }
        int cellw = barw + 11;
        int roww  = per_row * cellw - 2;
        int rows  = per_row > 0 ? (cores_shown + per_row - 1) / per_row : 0;
        for (int r = 0; r < rows; r++) {
            int x0 = cx - roww / 2;
            if (x0 < 1) x0 = 1;
            for (int c = 0; c < per_row; c++) {
                int i = r * per_row + c;
                if (i >= cores_shown)
                    break;
                int xx = x0 + c * cellw;
                char lab[16];
                snprintf(lab, sizeof(lab), "T%-2d ", i);
                label_at(y, xx, lab);
                draw_bar(y, xx + 4, barw, per[i], levelpair(per[i]), false, 0.0);
                char pct[8];
                snprintf(pct, sizeof(pct), "%3.0f%%", per[i]);
                text_at(y, xx + 4 + barw + 1, pct, levelpair(per[i]), A_BOLD);
            }
            y++;
        }
    } else {
        cprint(y, "cpu stats need /proc (Linux)", CP_HINT, A_DIM);
        y++;
    }

    if (pad4) y++;

    /* ---- memory section ---- */
    draw_sep(y, "memory");
    y++;

    if (mem_ok && mem.total > 0) {
        char pair[64], p1[96], p2[48];
        fmt_pair(mem_used, mem.total, pair, sizeof(pair));
        snprintf(p1, sizeof(p1), " %s", pair);
        snprintf(p2, sizeof(p2), " %s %d%%", SEP, (int)(mem_pct + 0.5));
        int uw1 = uwidth(p1), uw2 = uwidth(p2);
        int bw = barw_all;
        int avail = COLS - 2 - 5 - uw1 - uw2;
        if (bw > avail) bw = avail;
        if (bw < 0) bw = 0;
        int tw = 5 + bw + uw1 + uw2;
        int x = cx - tw / 2;
        if (x < 1) x = 1;

        label_at(y, x, "MEM  ");
        draw_bar(y, x + 5, bw, mem_pct, levelpair(mem_pct), true, g_peak_mem);
        text_at(y, x + 5 + bw, p1, CP_TEXT, A_NORMAL);
        text_at(y, x + 5 + bw + uw1, p2, levelpair(mem_pct), A_BOLD);
        y++;

        if (swapline) {
            long sused = mem.swaptot - mem.swapfree;
            if (sused < 0) sused = 0;
            double spct = 100.0 * (double)sused / (double)mem.swaptot;
            fmt_pair(sused, mem.swaptot, pair, sizeof(pair));
            snprintf(p1, sizeof(p1), " %s", pair);
            snprintf(p2, sizeof(p2), " %s %d%%", SEP, (int)(spct + 0.5));
            uw1 = uwidth(p1); uw2 = uwidth(p2);
            bw = barw_all;
            avail = COLS - 2 - 5 - uw1 - uw2;
            if (bw > avail) bw = avail;
            if (bw < 0) bw = 0;
            tw = 5 + bw + uw1 + uw2;
            x = cx - tw / 2;
            if (x < 1) x = 1;

            label_at(y, x, "SWAP ");
            draw_bar(y, x + 5, bw, spct, levelpair(spct), false, 0.0);
            text_at(y, x + 5 + bw, p1, CP_TEXT, A_NORMAL);
            text_at(y, x + 5 + bw + uw1, p2, levelpair(spct), A_BOLD);
            y++;
        }
    } else if (mem_ok) {
        cprint(y, "memory: unknown layout", CP_HINT, A_DIM);
        y++;
    } else {
        cprint(y, "memory stats need /proc (Linux)", CP_HINT, A_DIM);
        y++;
    }

    if (pad5) y++;

    /* ---- footer: load, uptime, threads, procs ---- */
    if (foot) {
        char fbuf[192] = "";
        char up[32] = "?";
        if (up_ok)
            fmt_uptime(up_s, up, sizeof(up));
        if (load_ok && up_ok)
            snprintf(fbuf, sizeof(fbuf),
                     "load %.2f %.2f %.2f %s up %s %s %ld/%ld thr %s %d procs",
                     ld[0], ld[1], ld[2], SEP, up, SEP, thr_r, thr_t, SEP, procs);
        else if (load_ok)
            snprintf(fbuf, sizeof(fbuf),
                     "load %.2f %.2f %.2f %s %ld/%ld thr",
                     ld[0], ld[1], ld[2], SEP, thr_r, thr_t);
        else if (up_ok)
            snprintf(fbuf, sizeof(fbuf), "up %s", up);
        if (fbuf[0])
            cprint(y, fbuf, CP_HINT, A_DIM);
    }

    refresh();
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    printf("tclock - a pretty ncurses dashboard: big clock + date, "
           "CPU threads, memory.\n"
           "usage: %s [-h | --help]\n"
           "keys:  q quit %s r reset peak markers\n", prog, "|");
}

static void on_signal(int sig)
{
    endwin();
    _exit(128 + sig);
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(argv[0]);
        return 0;
    }

    setlocale(LC_ALL, "");
    const char *cs = nl_langinfo(CODESET);
    g_utf8 = (cs && !strcmp(cs, "UTF-8"));
    SEP    = g_utf8 ? "·" : "|";

    initscr();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef NCURSES_VERSION
    set_escdelay(25);
#endif

    if (has_colors() && start_color() != ERR) {
        g_color = true;
        bool defok = (use_default_colors() != ERR);
        static const struct { short pair, fg, bg; } defs[] = {
            {CP_BORDER, COLOR_BLUE,    -1},
            {CP_TITLE,  COLOR_BLACK,   COLOR_CYAN},
            {CP_CLOCK,  COLOR_CYAN,    -1},
            {CP_COLON,  COLOR_MAGENTA, -1},
            {CP_DATE,   COLOR_YELLOW,  -1},
            {CP_SUB,    COLOR_WHITE,   -1},
            {CP_LABEL,  COLOR_CYAN,    -1},
            {CP_TEXT,   COLOR_WHITE,   -1},
            {CP_HINT,   COLOR_WHITE,   -1},
            {CP_OK,     COLOR_GREEN,   -1},
            {CP_WARN,   COLOR_YELLOW,  -1},
            {CP_CRIT,   COLOR_RED,     -1},
        };
        for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
            short bg = defok ? defs[i].bg : COLOR_BLACK;
            init_pair(defs[i].pair, defs[i].fg, bg);
        }
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(FRAME_MS);

    int frame = 0;
    for (;;) {
        draw_frame(++frame);
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27)
            break;
        if (ch == 'r' || ch == 'R') {
            g_peak_cpu = 0.0;
            g_peak_mem = 0.0;
        }
        if (ch == KEY_RESIZE)
            clear();
    }

    endwin();
    return 0;
}
