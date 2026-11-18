/* mpwindow.c - the multiplayer window module: a multiplayer game never stalls because of one player's window.
 * Lockstep multiplayer waits for the slowest peer, so a peer whose game loop stops stalls everyone:
 *   - in a session the game keeps running unfocused / minimised, whatever [display] background says (force_bg);
 *   - a title-bar drag or border resize runs Windows' modal move loop, which stops the game loop (measured: 0
 *     frames in 3 s); in a session we move / size the window ourselves under SetCapture (18489 frames in a 3 s
 *     drag). Outside a session the stock drag (Aero snap) stays.
 * Works with any DirectPlay; with our dplayx.dll it also sees the lobby (LF_NetSessionOpen). Our own code. ASCII only. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "lotr2fix.h"

/* a multiplayer session (lobby or game) is open: dplayx's LF_NetSessionOpen, else the exe's own flag */
static int net_open(void)
{
    static int (WINAPI *f)(void);
    if (!f) {
        HMODULE dp = GetModuleHandleA("dplayx.dll");
        if (dp) f = (int (WINAPI *)(void))GetProcAddress(dp, "LF_NetSessionOpen");
    }
    return (f && f()) || *(volatile int *)EXE_NET_GAME != 0;
}

static int mpw_force_bg(void) { return net_open(); }

/* Multiplayer: a title-bar drag or border resize runs Windows' modal move loop, which stops the game loop and so
 * stalls every peer (lockstep). In a session we move/size the window ourselves under SetCapture instead. */
static int g_ncdrag;                 /* the hit-test code being dragged, 0 = none */
static POINT g_nc_pt;
static RECT g_nc_rc;

static void nc_end(HWND h)
{
    g_ncdrag = 0;
    if (GetCapture() == h) ReleaseCapture();
    lf_sizemove(h, 0);
}

static int nc_drag(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_NCLBUTTONDOWN) {
        if (!net_open() || IsZoomed(h) || !(wp == HTCAPTION || (wp >= HTLEFT && wp <= HTBOTTOMRIGHT))) return 0;
        g_ncdrag = (int)wp;
        GetCursorPos(&g_nc_pt); GetWindowRect(h, &g_nc_rc);
        SetCapture(h);
        lf_sizemove(h, 1);
        return 1;
    }
    if (!g_ncdrag) return 0;
    if (m == WM_CAPTURECHANGED) { if ((HWND)lp != h) nc_end(h); return 1; }
    if (m == WM_LBUTTONUP || m == WM_RBUTTONDOWN || m == WM_CANCELMODE) { nc_end(h); return 1; }
    if (m != WM_MOUSEMOVE) return 0;
    POINT p;
    GetCursorPos(&p);
    int dx = p.x - g_nc_pt.x, dy = p.y - g_nc_pt.y, k = g_ncdrag;
    RECT r = g_nc_rc;
    if (k == HTCAPTION) OffsetRect(&r, dx, dy);
    else {
        if (k == HTLEFT || k == HTTOPLEFT || k == HTBOTTOMLEFT) r.left += dx;
        if (k == HTRIGHT || k == HTTOPRIGHT || k == HTBOTTOMRIGHT) r.right += dx;
        if (k == HTTOP || k == HTTOPLEFT || k == HTTOPRIGHT) r.top += dy;
        if (k == HTBOTTOM || k == HTBOTTOMLEFT || k == HTBOTTOMRIGHT) r.bottom += dy;
        if (r.right - r.left < 360 || r.bottom - r.top < 280) return 1;   /* about the 320x240 client minimum */
    }
    SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE |
                 (k == HTCAPTION ? SWP_NOSIZE : 0));
    return 1;
}

static int mpw_msg(HWND h, UINT m, WPARAM wp, LPARAM lp, LRESULT *r)
{
    *r = 0;
    return nc_drag(h, m, wp, lp);
}

const LF_MODULE lf_mod_mpwindow = {
    .name = "mpwindow",
    .msg = mpw_msg,
    .force_bg = mpw_force_bg,
};
