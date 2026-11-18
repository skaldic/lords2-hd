/* mpmenu.c - Multiplayer drawn inside the game's own main menu (Matthew 2026-10-06: "built into the menu system,
 * like how Jen did it", no Win32 popups). Our own code. The technique - redraw the title screen's four buttons
 * through the game's own button, frame and text routines and take their clicks ourselves - we learned from
 * "Jen with a B"'s Mod Menu v1.1; credit to Jen.
 *
 * Main menu (UiState 0x1f, screen 1) Multiplayer -> our screen "choose" (Create a game / Search local games / Connect
 * by address / Back), laid out like the stock "Your options" screen (FUN_0041ec8a: panel at 0xb0, big red-capital
 * header, buttons at 0x72 + 0x24k: 0x17 lower than stock, under our hint). "Search local games" -> screen "local" (dplayx LF_NetFind: a UDP broadcast on every
 * LAN / Hamachi / TAP-VPN adapter; up to 3 games listed, click one to join). "Connect by address" -> screen "connect"
 * (Host / Port fields typed in place, Connect, Back). Create or Connect then runs the
 * game's own Multiplayer handler (FUN_00432b05 id 2) with its dialogs answered: Connection method is skipped by
 * dplayx, "Connect or Create" (0x6c) and the session name (0x74) are answered here in DialogBoxParamA, and the
 * session list (0x82) runs invisible in dplayx menu mode (LF_NetMenu), which joins the first session or cancels
 * with a reason we show on the menu. Needs our dplayx (its LF_NetMenu exports); without it the stock path runs.
 *
 * Draw: hook on FUN_0041eaa3 (the four title buttons, run each frame and inside every full redraw) and FUN_0041ea14
 * (the title panel, full redraws only: our panel and header).
 * Input: mpm_tick() runs from the core's frame hook (the dispatcher call site 0x4ba1a2) after the game's mouse update
 * and before its button tables; it reads the game's own logical mouse and click edges and clears the ones it used.
 * Keys: mpm_key() from the window procedure (LF_MODULE.msg). All on the game thread.
 * Module: "mpmenu" (lf_mod_mpmenu at the end). Without our dplayx.dll only the Escape "Exit the game?" screen works. */
#include "lotr2fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R32(a) (*(volatile DWORD *)(a))
#define R8(a)  (*(volatile BYTE *)(a))

#define A_UISTATE  0x4eac50
#define A_SUBSCR   0x5530f0
#define A_REDRAW   0x57d340     /* 2 = full screen redraw next frame */
#define A_MX       0x4e65fc     /* logical mouse */
#define A_MY       0x4e65f4
#define A_LDOWN    0x4eafb4     /* edges, set per frame by FUN_004b191e */
#define A_LUP      0x4e65d8
#define A_LDBL     0x4eabc5
#define A_RDOWN    0x4eabe0
#define A_RUP      0x4e6900
#define A_LHELD    0x4e65cc     /* left held, recomputed each frame from 0x4eabc2 (FUN_004b191e) */
#define A_TEXTMODE 0x5aea40
#define A_CAPRED   0x58fe2c     /* 1: the big font draws capitals red (stock headers) */
#define FONT       ((void *)0x5af8f0)
#define BIGFONT    ((void *)0x5b2fa0)
#define PX 0xb0      /* panel, as the stock "Your options" screen: x 0xb0, 0x12 x 0x16 tiles of 16 px */
#define PW 0x120
#define BX 0xe0
#define BW 0xc0
#define BH 0x18

typedef void (__cdecl *pfn_v)(void);
typedef void (__cdecl *pfn_box)(int, int, int, int);
typedef void (__cdecl *pfn_frame)(int, int, int, int, int);
typedef void (__cdecl *pfn_text)(const char *, int, int, int, void *, int);
typedef int (__cdecl *pfn_tw)(const char *, int);
#define DrawOutline ((pfn_box)0x403ee4)
#define DrawFrame   ((pfn_frame)0x4527a6)
#define DrawTextC   ((pfn_text)0x4025d7)
#define TextWidth   ((pfn_tw)0x4014f0)

typedef void (WINAPI *pfn_menu)(int, const char *, int);
typedef void (WINAPI *pfn_get)(char *, int, int *);
typedef int (WINAPI *pfn_msg)(char *, int);
typedef int (WINAPI *pfn_find)(int);
typedef int (WINAPI *pfn_findget)(int, char *, int, char *, int, int *, int *);
static pfn_menu net_menu;
static pfn_get net_get;
static pfn_msg net_msg;
static pfn_find net_find;          /* optional: an older dplayx has no local search */
static pfn_findget net_findget;

#define NROW 5
#define NRES 3       /* local results shown */
/* 0x17 below the stock 0x5b: room for the two-line hint under the header (Matthew 2026-10-07: the hint "at the top", in
 * the game's style, "not a black box with white text") */
static const int ROW_Y[NROW] = {0x72, 0x96, 0xba, 0xde, 0x102};
enum { OFF, CHOOSE, CONNECT, LOCAL, QUIT };   /* QUIT: Escape on the title asks here (BACKLOG #15), not in a MessageBoxA */
static int g_on, g_focus, g_go, g_go_wait, g_auto, g_swallow, g_hover;
static char g_host[64], g_port[8], g_msg[96];
static pfn_v orig_title, orig_panel;
static int g_ready;
/* Every screen change starts a guard of one double-click time: the second press of a double-click must not act on
 * the NEW screen (review 2026-10-07: double-click Back -> stock Exit game quits; double-click Multiplayer -> Create). */
static DWORD g_guard;
static void guard(void) { g_guard = GetTickCount() + GetDoubleClickTime(); }
static int guarded(void) { return g_guard && (int)(g_guard - GetTickCount()) > 0; }

static void redraw(void) { R32(A_REDRAW) = 2; }
/* Only a screen change asks for the game's full redraw: that reloads the title art and fades the palette in from
 * black (FUN_004b0ab5 -> FUN_0042f1df), which flickered at every key (Matthew 2026-10-06). Fields, results and the
 * message box repaint themselves each frame in hook_title. */
static void say(const char *m) { lstrcpynA(g_msg, m, sizeof g_msg); }

static void enter(int scr)
{
    g_on = scr;
    g_msg[0] = 0;
    guard();
    if (scr == CONNECT) {
        int port = 47700;
        if (!g_host[0]) {
            net_get(g_host, sizeof g_host, &port);
            if (port < 1024 || port > 65535) port = 47700;   /* a hand-edited lotr2net.ini never shows a bad port */
            snprintf(g_port, sizeof g_port, "%d", port);
        }
        g_focus = 0;
    }
    if (scr == LOCAL) net_find(1);
    redraw();
    lf_log("mp menu: screen %d", scr);
}

static void eat_clicks(void)
{
    R8(A_LDOWN) = 0; R8(A_LUP) = 0; R8(A_LDBL) = 0; R8(A_RDOWN) = 0; R8(A_RUP) = 0;
}

/* Back to the main menu happens on a PRESS; the stock title buttons would then fire on the release (type 3) or the
 * hold (type 2, FUN_0040e3ee) under the same mouse - one of them is the Lords of Magic trailer (Matthew 2026-10-06).
 * g_swallow keeps eating clicks, and the held flag, until both buttons are up. */
static void back(void)
{
    if (g_on == CONNECT || g_on == LOCAL) enter(CHOOSE);
    else { g_on = OFF; g_swallow = 2; guard(); redraw(); lf_log("mp menu: back to the main menu"); }
}

/* Host must be an IPv4 address (4 numbers 0-255) or a computer name (labels of letters, digits, '-', not starting or
 * ending with '-'). Digits and dots only = must be a full IPv4: "192.168.18.14192.168.18.14" was tried as a name and
 * failed late with a garbled "No game found" (Matthew 2026-10-07). */
static int host_ok(const char *h)
{
    int digits_only = 1;
    for (const char *s = h; *s; s++) if (!(*s == '.' || (*s >= '0' && *s <= '9'))) digits_only = 0;
    if (!*h) return 0;
    if (digits_only) {
        int parts = 0;
        for (const char *s = h; ; ) {
            int n = 0, v = 0;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); n++; s++; }
            if (n < 1 || n > 3 || v > 255) return 0;
            parts++;
            if (!*s) break;
            if (*s != '.' || parts == 4) return 0;
            s++;
        }
        return parts == 4;
    }
    for (const char *s = h; ; ) {
        const char *b = s;
        while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-') s++;
        if (s == b || s - b > 63 || *b == '-' || s[-1] == '-') return 0;
        if (!*s) return 1;
        if (*s != '.') return 0;
        s++;
    }
}

static void start(int what)   /* 1 create, 2 connect: runs next frames, after "..." is on screen */
{
    if (what == 2) {
        int p = atoi(g_port);
        if (!g_host[0]) { say("Type the host's address first"); g_focus = 0; return; }
        if (!host_ok(g_host)) { say("Not a valid address (example: 192.168.1.20)"); g_focus = 0; return; }
        /* 1-1023 are reserved system ports, never a game's (Matthew 2026-10-07); typing passes through them, so check here */
        if (p < 1024 || p > 65535) { say("Port must be 1024 to 65535"); g_focus = 1; return; }
        char m[96];
        snprintf(m, sizeof m, "Connecting to %s:%d ...", g_host, p);
        say(m);
    } else say("Starting a game ...");
    g_go = what;
    g_go_wait = 3;
}

static void run(void)
{
    int what = g_go;
    g_go = 0;
    if (what == 2) net_menu(1, g_host, atoi(g_port));
    else net_menu(2, NULL, 0);
    g_auto = what;
    lf_log("mp menu: %s -> game's Multiplayer path", what == 2 ? "connect" : "create");
    R32(0x59154c) = 2; R32(0x591550) = 0; R32(0x591558) = 0;
    DWORD rd = R32(A_REDRAW);
    ((pfn_v)0x432b05)();
    g_auto = 0;
    char m[128];
    int got = net_msg(m, sizeof m);
    net_menu(0, NULL, 0);
    if (R8(A_UISTATE) != 0x1f || R32(A_SUBSCR) != 1) {
        lf_log("mp menu: left for screen %lu", (unsigned long)R32(A_SUBSCR));
        g_on = OFF;
        g_msg[0] = 0;
        return;
    }
    R32(A_REDRAW) = rd;   /* the handler asked for a full redraw (a palette fade from black); the menu never changed */
    say(got ? m : what == 2 ? "Could not join that game" : "Could not start a game");
    lf_log("mp menu: back on the menu: %s", g_msg);
}

/* The game's DialogBoxParamA, while our menu drives its Multiplayer path: answer without showing anything. */
int mpm_dialog(DWORD tmpl, INT_PTR *r)
{
    if (!g_auto) return 0;
    if (tmpl == 0x6c) { *r = g_auto == 2 ? 0x3ea : 0x3ec; lf_log("mp menu: Connect or Create -> %s", g_auto == 2 ? "Connect" : "Create"); return 1; }
    if (tmpl == 0x74) { *r = 0; lf_log("mp menu: session name -> default"); return 1; }
    return 0;
}

static int row_at(int x, int y)
{
    for (int i = 0; i < NROW; i++)
        if (x >= BX && x < BX + BW && y >= ROW_Y[i] && y < ROW_Y[i] + BH) return i + 1;
    return 0;
}

/* dplayx may map after us: its exports are looked up on first use, once. */
static int api(void)
{
    static int looked;
    if (!looked && g_ready) {
        HMODULE dp = GetModuleHandleA("dplayx.dll");
        looked = 1;
        net_menu = dp ? (pfn_menu)GetProcAddress(dp, "LF_NetMenu") : NULL;
        net_get = dp ? (pfn_get)GetProcAddress(dp, "LF_NetMenuGet") : NULL;
        net_msg = dp ? (pfn_msg)GetProcAddress(dp, "LF_NetMenuMsg") : NULL;
        net_find = dp ? (pfn_find)GetProcAddress(dp, "LF_NetFind") : NULL;
        net_findget = dp ? (pfn_findget)GetProcAddress(dp, "LF_NetFindGet") : NULL;
        if (!net_find || !net_findget) net_find = NULL;
        if (!net_menu || !net_get || !net_msg) net_menu = NULL;
        lf_log(net_menu ? "mp menu: on (in-game Multiplayer screens)"
                        : "mp menu: off (no dplayx.dll with our menu exports) - stock Multiplayer dialogs");
        if (net_menu && !net_find) lf_log("mp menu: dplayx has no local search (LF_NetFind)");
    }
    return net_menu != NULL;
}

/* A local search result: fill the address and join it, or say why not. */
static void pick(int i)
{
    char label[64], host[64];
    int port, state;
    if (net_find(0) < 0 || !net_findget(i, label, sizeof label, host, sizeof host, &port, &state)) return;
    if (state == 1) { say("That game has already started"); return; }
    if (state == 2) { say("That game runs a different patch version"); return; }
    lstrcpynA(g_host, host, sizeof g_host);
    snprintf(g_port, sizeof g_port, "%d", port);
    lf_log("mp menu: local pick %s:%d", g_host, port);
    start(2);
}

/* Escape on the title screen: our "Exit the game?" screen. 0 = not on the title or the hooks are off (the caller
 * falls back to its MessageBoxA). The stock in-game yes/no popup (0x40e6f2) was tried first: on the title it draws
 * with the campaign's frame colours, purple on this palette (2026-10-07). */
int mpm_quit(void)
{
    if (!g_ready || R8(A_UISTATE) != 0x1f || R32(A_SUBSCR) != 1) return 0;
    if (g_on == QUIT) return 1;
    if (g_on) return 0;
    enter(QUIT);
    return 1;
}

static void quit_yes(void)
{
    eat_clicks();
    R8(0x5aeb68) = 1;   /* the game's quit flag: game_main posts WM_CLOSE (the stock Escape outside the campaign) */
    lf_log("Escape: exit confirmed (title)");
}

int mpm_tick(void)
{
    if (!api() && g_on != QUIT && !g_swallow && !guarded()) return 0;   /* the swallow after QUIT's Back runs without dplayx too */
    if (R8(A_UISTATE) != 0x1f || R32(A_SUBSCR) != 1) {
        if (g_on) { g_on = OFF; g_go = 0; lf_log("mp menu: screen left, menu off"); }
        g_swallow = 0;
        return 0;
    }
    int x = (short)R32(A_MX), y = (short)R32(A_MY), row = row_at(x, y);
    if (!g_on && !g_swallow && guarded() && (R8(A_LDOWN) || R8(A_RDOWN) || R8(A_LDBL))) g_swallow = 2;   /* 2nd press of a double-click */
    if (!g_on && g_swallow) {
        if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000 || R8(A_LHELD)) g_swallow = 2;
        else g_swallow--;
        R8(A_LHELD) = 0;   /* the next frame then sees a press edge: eaten here too, while still held */
        eat_clicks();
        return 1;
    }
    if (!g_on) {
        if (!net_menu) return 0;
        /* the title's own Multiplayer button: the stock rows, 0x5b + 0x24k (ours sit 0x17 lower) */
        if (R8(A_LDOWN) && x >= BX && x < BX + BW && y >= 0x7f && y < 0x7f + BH && !R32(0x568464)) {
            eat_clicks(); enter(CHOOSE); return 1;
        }
        return 0;
    }
    if (g_go) {
        eat_clicks();
        if (--g_go_wait <= 0) run();
        return 1;
    }
    if (guarded() && (R8(A_LDOWN) || R8(A_LDBL))) { eat_clicks(); return 1; }   /* 2nd press of a double-click */
    if (R8(A_RDOWN)) back();
    else if (R8(A_LDOWN)) {
        if (g_on == QUIT) {
            if (row == 1) quit_yes();
            else if (row == 2) back();
        } else if (g_on == CHOOSE) {
            if (row == 1) start(1);
            else if (row == 2) { if (net_find) enter(LOCAL); else say("Search needs the newer dplayx.dll"); }
            else if (row == 3) enter(CONNECT);
            else if (row == 4) back();
        } else if (g_on == LOCAL) {
            if (row >= 1 && row <= NRES) pick(row - 1);
            else if (row == 4) { g_msg[0] = 0; net_find(1); lf_log("mp menu: search again"); }
            else if (row == 5) back();
        } else {
            if (row == 1 || row == 2) g_focus = row - 1;
            else if (row == 3) start(2);
            else if (row == 4) back();
        }
    }
    eat_clicks();
    return 1;
}

int mpm_key(UINT m, WPARAM wp, LPARAM lp)
{
    if (!g_on || (!net_menu && g_on != QUIT)) return 0;
    if (g_go) return 1;
    if (m == WM_KEYDOWN) {
        if (lp & 0x40000000 && wp == VK_ESCAPE) return 1;
        if (wp == VK_ESCAPE) back();
        else if (g_on == QUIT) { if (wp == VK_RETURN || wp == 'Y') quit_yes(); else if (wp == 'N') back(); }
        else if (g_on == CONNECT) {
            if (wp == VK_RETURN) start(2);
            else if (wp == VK_TAB || wp == VK_UP || wp == VK_DOWN) g_focus ^= 1;
            else if (wp == VK_BACK) {
                char *f = g_focus ? g_port : g_host;
                size_t n = strlen(f);
                if (n) { f[n - 1] = 0; g_msg[0] = 0; }
            }
        }
        return 1;
    }
    if (m == WM_CHAR) {
        if (g_on != CONNECT || wp < 0x21 || wp > 0x7e) return 1;
        char *f = g_focus ? g_port : g_host;
        size_t cap = g_focus ? 5 : sizeof g_host - 1, n = strlen(f);
        if (g_focus && (wp < '0' || wp > '9')) return 1;
        if (!g_focus && wp == ':') { g_focus = 1; g_port[0] = 0; return 1; }   /* "addr:port" typed */
        if (!g_focus && !((wp >= '0' && wp <= '9') || (wp >= 'a' && wp <= 'z') || (wp >= 'A' && wp <= 'Z') ||
                          wp == '.' || wp == '-')) return 1;   /* only characters an address or computer name can hold */
        if (g_focus && ((n == 0 && wp == '0') || (n > 0 && atoi(f) * 10 + (int)(wp - '0') > 65535)))
            return 1;   /* port: a key that would make it 0, lead with 0 or pass 65535 is refused (Matthew 2026-10-07) */
        if (n < cap) { f[n] = (char)wp; f[n + 1] = 0; g_msg[0] = 0; }
        return 1;
    }
    return m == WM_KEYUP;
}

typedef void (__cdecl *pfn_line)(int, int, int, int, int);
#define DrawLine ((pfn_line)0x403a8f)
#define Dirty    ((pfn_box)0x452306)    /* x1, y1, x2, y2: the rect the next present copies */
#define PAL8     ((const BYTE *)0x4ea1b0)   /* current palette, 8-bit RGB (FUN_004b0ab5) */
#define MSG_X 0xc0   /* under the buttons, inside the panel (x 0xb0..0x1d0, y 10..0x16a) */
#define MSG_Y 0x122
#define MSG_W 0x100
#define MSG_H 0x36   /* 3 lines, MSG_LH apart */
#define MSG_LH 0x10  /* the font is ~14 px tall: 0xe made the lines touch (Matthew 2026-10-06) */
#define MSG_PAD 12   /* side margin: TextWidth runs a few px short of the drawn text */
#define HINT_Y 0x47  /* the hint: up to 2 lines on the stone under the header, MSG_LH apart */
#define A_SCREEN 0x4ea1a8   /* the 640-pitch frame the game draws into */
static BYTE g_under[MSG_H][MSG_W];   /* the stone under the message box, from the last full redraw */
static BYTE g_rowbg[3][BH][BW];      /* ... and under rows 0..2 (typed fields, search results) */
static int g_under_ok;

static int pal_near(int r, int g, int b)
{
    int best = 0, bd = 1 << 30;
    for (int i = 1; i < 256; i++) {
        int dr = PAL8[i * 3] - r, dg = PAL8[i * 3 + 1] - g, db = PAL8[i * 3 + 2] - b, d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void fill(int x, int y, int w, int h, int c)
{
    for (int j = 0; j < h; j++) DrawLine(x, y + j, x + w - 1, y + j, c);
    Dirty(x, y, x + w, y + h);
}

static void button(int i, const char *text)
{
    int c = g_hover == i + 1 ? 0xf9 : 0x3f;   /* our own hover: the stock 0x553114 knows only the title's 4 rows */
    DrawOutline(BX, ROW_Y[i], BW, BH);
    DrawFrame(BX, ROW_Y[i], 0xe, 3, 1);
    DrawTextC(text, BX, ROW_Y[i] + 5, BW, FONT, c);
}

/* the stone saved at the last full redraw back under row i (0..2): a shorter text leaves no old letters behind */
static void row_clear(int i)
{
    BYTE *s = (BYTE *)R32(A_SCREEN);
    if (!g_under_ok || !s || i < 0 || i > 2) return;
    for (int y = 0; y < BH; y++) memcpy(s + (ROW_Y[i] + y) * 640 + BX, g_rowbg[i][y], BW);
    Dirty(BX, ROW_Y[i], BX + BW, ROW_Y[i] + BH);
}

/* A typed field, in the game's style (Matthew 2026-10-07: no black box with white text): the stone, an outline, the
 * game's dark text, red while it has the focus. The game font has no '_' (it prints a space), so the caret is a
 * drawn bar. */
static void field(int i, const char *label, const char *val, int focus)
{
    char t[96];
    const char *v = val;
    int y = ROW_Y[i];
    for (;;) {   /* a long address shows its end */
        snprintf(t, sizeof t, "%s %s%s", label, v != val ? ".." : "", v);
        if (TextWidth(t, (int)FONT) <= BW - 16 || !*v) break;
        v++;
    }
    row_clear(i);
    DrawOutline(BX, y, BW, BH);
    int tw = TextWidth(t, (int)FONT), x0 = BX + (BW - tw) / 2;
    DrawTextC(t, BX, y + 5, BW, FONT, focus ? 0xf9 : 0x3f);
    if (focus && (GetTickCount() / 500) & 1) DrawLine(x0 + tw + 2, y + 4, x0 + tw + 2, y + BH - 5, 0xf9);
}

/* A local search result: stone and outline like the fields (its text changes without a full redraw), the label cut
 * to fit; red under the mouse when it can be joined. */
static void result(int i, const char *text, int live)
{
    char t[80];
    int y = ROW_Y[i];
    lstrcpynA(t, text, sizeof t);
    for (size_t n = strlen(t); n && TextWidth(t, (int)FONT) > BW - 24; ) t[--n] = 0;
    row_clear(i);
    DrawOutline(BX, y, BW, BH);
    if (t[0]) DrawTextC(t, BX, y + 5, BW, FONT, live && g_hover == i + 1 ? 0xf9 : 0x3f);
}

/* The status line under the buttons: a dark box, light text (red on stone was hard to read), word-wrapped to
 * three lines. Drawn each frame, so a new message needs no full redraw. Only for the search status and messages;
 * with neither, the stone saved at the last full redraw goes back over it. */
/* word-wrap m into up to `max` lines of at most w px; the last line takes as much as fits */
static int wrap(const char *m, int w, char ln[][96], int max)
{
    int n = 0;
    const char *p = m;
    while (*p && n < max) {
        int len = 0, cut = 0;
        while (p[len]) {
            char t[96];
            int k = len + 1 < 95 ? len + 1 : 95;
            memcpy(t, p, k); t[k] = 0;
            if (TextWidth(t, (int)FONT) > w) break;
            len++;
            if (p[len] == ' ' || !p[len]) cut = len;
        }
        if (!p[len]) cut = len;
        if (!cut) cut = len ? len : 1;   /* one word wider than the box: hard break */
        if (n == max - 1 && p[cut]) cut = len;   /* last line: as much as fits */
        if (cut > 95) cut = 95;
        memcpy(ln[n], p, cut); ln[n][cut] = 0; n++;
        p += cut;
        while (*p == ' ') p++;
    }
    return n;
}

static void msg_box(const char *m, int err, int dark, int light, int hi)
{
    char ln[3][96];
    int n = wrap(m, MSG_W - 2 * MSG_PAD, ln, 3);
    DrawOutline(MSG_X, MSG_Y, MSG_W, MSG_H);
    fill(MSG_X + 1, MSG_Y + 1, MSG_W - 2, MSG_H - 2, dark);
    int c = err ? hi : light;
    int y = MSG_Y + (MSG_H - n * MSG_LH) / 2 + 1;   /* the lines centred in the box */
    for (int i = 0; i < n; i++) DrawTextC(ln[i], MSG_X, y + i * MSG_LH, MSG_W, FONT, c);
}

static void msg_clear(void)
{
    BYTE *s = (BYTE *)R32(A_SCREEN);
    if (!g_under_ok || !s) return;
    for (int y = 0; y < MSG_H; y++) memcpy(s + (MSG_Y + y) * 640 + MSG_X, g_under[y], MSG_W);
    Dirty(MSG_X, MSG_Y, MSG_X + MSG_W, MSG_Y + MSG_H);
}

/* the screen's hint, on the stone under the header in the game's own dark text, like its own option screens */
static void hint_top(const char *m)
{
    char ln[2][96];
    int n = wrap(m, PW - 2 * 0x18, ln, 2);
    for (int i = 0; i < n; i++) DrawTextC(ln[i], PX, HINT_Y + i * MSG_LH, PW, FONT, 0x3f);
}

static void __cdecl hook_title(void)
{
    if (!g_on) { if (orig_title) orig_title(); return; }
    int dark = pal_near(24, 20, 16), light = pal_near(240, 232, 208), hi = pal_near(255, 214, 90);
    char hint[96];
    g_hover = g_go ? 0 : row_at((short)R32(A_MX), (short)R32(A_MY));
    R32(A_TEXTMODE) = 1;
    if (g_on == QUIT) lstrcpynA(hint, "Enter or Y exits, Escape or N stays", sizeof hint);
    else if (g_on == CHOOSE) lstrcpynA(hint, "Play over your home network, Hamachi or a VPN", sizeof hint);
    else if (g_on == CONNECT) lstrcpynA(hint, "Type the host's address or computer name, and the port", sizeof hint);
    else {
        int n = net_find(0);
        if (n < 0) lstrcpynA(hint, "Searching your network ...", sizeof hint);
        else if (!n) lstrcpynA(hint, "No games found. Over a VPN or the internet, use Connect by address", sizeof hint);
        else if (n > NRES) snprintf(hint, sizeof hint, "Found %d (first %d shown). Click one to join", n, NRES);
        else snprintf(hint, sizeof hint, "Found %d. Click one to join", n);
    }
    if (g_on == LOCAL || g_msg[0]) msg_box(g_msg[0] ? g_msg : hint, g_msg[0] != 0, dark, light, hi);
    else msg_clear();
    if (g_on != LOCAL) hint_top(hint);
    if (g_on == QUIT) {
        button(0, "Exit");
        button(1, "Back");
    } else if (g_on == CHOOSE) {
        button(0, "Create a game");
        button(1, "Search local games");
        button(2, "Connect by address");
        button(3, "Back");
    } else if (g_on == LOCAL) {
        int n = net_find(0);
        for (int i = 0; i < NRES; i++) {
            char label[64], host[64], t[80];
            int port, state;
            if (n > i && net_findget(i, label, sizeof label, host, sizeof host, &port, &state)) {
                snprintf(t, sizeof t, "%s%s", label, state == 1 ? " started" : state == 2 ? " other ver" : "");
                result(i, t, state == 0);
            } else result(i, "", 0);
        }
        button(3, "Search again");
        button(4, "Back");
    } else {
        field(0, "Host:", g_host, g_focus == 0);
        field(1, "Port:", g_port, g_focus == 1);
        button(2, "Connect");
        button(3, "Back");
    }
    R32(A_TEXTMODE) = 0;
}

/* FUN_0041ea14, the title panel (full redraws only): in our screens, the panel and header of the stock "Your options"
 * screen (FUN_0041ec8a: 0x409346 at 0xb0, big font, capitals red, embossed) with our own header, 4 tiles taller for
 * the message box under the buttons. */
static void __cdecl hook_panel(void)
{
    if (!g_on) { if (orig_panel) orig_panel(); return; }
    R32(A_CAPRED) = 1;
    ((pfn_frame)0x409346)(R32(0x4eabec), PX, 10, 0x12, g_on == QUIT ? 0xc : 0x16);   /* QUIT: 2 rows, no message box */
    R32(A_TEXTMODE) = 0;
    DrawTextC(g_on == QUIT ? "Exit the game?" : g_on == CHOOSE ? "Multiplayer" : g_on == LOCAL ? "Games nearby" : "Join a game", PX, 0x2d, PW, BIGFONT, 0x3f);
    R32(A_CAPRED) = 0;
    BYTE *scr = (BYTE *)R32(A_SCREEN);
    if (scr) {
        for (int y = 0; y < MSG_H; y++) memcpy(g_under[y], scr + (MSG_Y + y) * 640 + MSG_X, MSG_W);
        for (int i = 0; i < 3; i++)
            for (int y = 0; y < BH; y++) memcpy(g_rowbg[i][y], scr + (ROW_Y[i] + y) * 640 + BX, BW);
        g_under_ok = 1;
    }
    hook_title();
}

/* jmp from a 6-byte prologue at `at` to `fn`; returns a trampoline that runs the original, or NULL */
static pfn_v hook6(DWORD at, const BYTE *pro, void *fn)
{
    DWORD old;
    if (memcmp((void *)at, pro, 6)) return NULL;
    BYTE *t = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, pro, 6);
    t[6] = 0xe9; *(DWORD *)(t + 7) = at + 6 - (DWORD)(t + 11);
    BYTE j[6] = {0xe9, 0, 0, 0, 0, 0x90};
    *(DWORD *)(j + 1) = (DWORD)fn - (at + 5);
    VirtualProtect((void *)at, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)at, j, 6);
    VirtualProtect((void *)at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 6);
    return (pfn_v)t;
}

void mpm_init(void)
{
    static const BYTE pro_b[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x04}, pro_p[6] = {0x55, 0x8b, 0xec, 0x53, 0x56, 0x57};
    if (lf_ini_int("debug", "stock_mp", 0)) {   /* rig: drive the stock dialogs the multiplayer-only edition gets */
        lf_log("mp menu: off ([debug] stock_mp=1) - stock Multiplayer dialogs");
        return;
    }
    if (memcmp((void *)0x41eaa3, pro_b, 6) || memcmp((void *)0x41ea14, pro_p, 6)) {
        lf_log("WARN mp menu: title prologues differ - off");
        return;
    }
    orig_title = hook6(0x41eaa3, pro_b, (void *)hook_title);
    orig_panel = hook6(0x41ea14, pro_p, (void *)hook_panel);
    g_ready = orig_title && orig_panel;   /* else api() stays off: g_on never leaves OFF, the hooks only pass through */
    if (!orig_title || !orig_panel) lf_log("WARN mp menu: hook install failed - off");
}

/* ------------------------------------------------------------------ the module */

static void mpm_tick_v(void) { mpm_tick(); }
static int mpm_msg(HWND h, UINT m, WPARAM wp, LPARAM lp, LRESULT *r)
{
    (void)h;
    *r = 0;
    if (m != WM_KEYDOWN && m != WM_KEYUP && m != WM_CHAR) return 0;
    return g_exe_ok && mpm_key(m, wp, lp);
}

const LF_MODULE lf_mod_mpmenu = {
    .name = "mpmenu",
    .init = mpm_init,
    .msg = mpm_msg,
    .tick = mpm_tick_v,
    .dialog = mpm_dialog,
    .ask_quit = mpm_quit,
};
