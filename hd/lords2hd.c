// Lords of the Realm II HD - stage 1: show the fixed 640x480 game at the display's resolution.
//
// Built as ddraw.dll and dropped next to the game, so the exe loads it in place of the system
// DirectDraw. Nothing on disk is patched; deleting ddraw.dll restores the stock game.
//
// How it works: on a modern desktop the game always takes its fullscreen path, switching the
// monitor to 640x480 with 256 colours. It only uses a handful of DirectDraw calls (one primary
// surface, one system-memory back buffer it keeps a pointer into, a palette, a clipper), so
// instead of forwarding them we hand the game our own objects: surfaces are plain 8-bit memory,
// the mode switch succeeds without touching the monitor, and every copy to the primary marks
// the frame dirty. A presenter thread turns the 8-bit frame into colour, doubles it with
// interpolation and stretches it into the game's window, which covers the monitor.
// Mouse positions are converted back to 640x480 on their way into the game.

#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#define GAME_W 640  // the width every screen was designed for
#define GAME_H 480
#define MAX_W 2560  // widest frame wide mode makes (32:9 with the zoomed-out battle view)
// Wide mode keeps this many columns at the frame's left edge out of sight: the county map draws
// whole tiles (up to 60 pixels) past the visible edge, which would otherwise land on the row above.
#define GUARD 64

// The frame's width: GAME_W, or in wide mode as wide as the monitor's shape at 480 lines. The
// part of it shown is g_showW wide from g_showX: screens that have not been widened are drawn in
// the first 640 columns and shown alone, centred.
static int g_frameW = GAME_W;
static int g_showX = 0, g_showW = GAME_W;
static int g_frameH = GAME_H; // taller with the zoomed-out battle view
static int g_extra;          // columns the county map shows left of the game's x = 0
#define PANEL_X 478          // where the side panel starts, in the game's coordinates
static BOOL g_battleZoom;    // [Display] BattleZoom: battles show twice as far
// The zoomed-out view has 28 more columns and 15 more rows of 32-pixel cells (at 16:9).
#define BATTLE_MORE_COLS ((battle_map_w() - PANEL_X + 31) / 32)
#define BATTLE_MORE_ROWS 15
static int battle_map_w(void) { return 2 * (GAME_W + g_extra) - 2 * (GAME_W - PANEL_X); }
static BOOL battle_zoom_on(void);
// ---------------------------------------------------------------------------------------------
// Forwarding to the real ddraw.dll

#define DDRAW_EXPORTS(X) \
	X(AcquireDDThreadLock) X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand) \
	X(DDGetAttachedSurfaceLcl) X(DDInternalLock) X(DDInternalUnlock) X(DSoundHelp) \
	X(DirectDrawCreate) X(DirectDrawCreateClipper) X(DirectDrawCreateEx) \
	X(DirectDrawEnumerateA) X(DirectDrawEnumerateExA) X(DirectDrawEnumerateExW) \
	X(DirectDrawEnumerateW) X(DllCanUnloadNow) X(DllGetClassObject) X(GetDDSurfaceLocal) \
	X(GetOLEThunkData) X(GetSurfaceFromDC) X(RegisterSpecialCase) X(ReleaseDDThreadLock) \
	X(SetAppCompatData)

#define DECLARE_FORWARD(name) \
	FARPROC real_##name; \
	__attribute__((naked)) void fw_##name(void) { __asm__("jmp *_real_" #name); }
DDRAW_EXPORTS(DECLARE_FORWARD)

static void load_real_ddraw(void)
{
	char path[MAX_PATH];
	GetSystemDirectoryA(path, sizeof path);
	strcat(path, "\\ddraw.dll");
	HMODULE real = LoadLibraryA(path);
#define RESOLVE_FORWARD(name) real_##name = real ? GetProcAddress(real, #name) : NULL;
	DDRAW_EXPORTS(RESOLVE_FORWARD)
}

// ---------------------------------------------------------------------------------------------
// Settings and log

enum { SCALE_NONE, SCALE_SHARP, SCALE_SMOOTH };

static char g_gameDir[MAX_PATH], g_iniPath[MAX_PATH], g_logPath[MAX_PATH];
static BOOL g_emulate;          // our DirectDraw is in use (known exe build and [Display] Enabled=1)
static int g_scale = SCALE_SMOOTH;
static BOOL g_fullscreen = TRUE, g_integerScale;
static int g_windowScale = 2;
static int g_cursorScale = 50; // percent of the cursor's original size relative to the screen
static BOOL g_logLoads, g_dumpPalettes, g_timing;
// Timing=1: how long the game's own map and battlefield redraws take with the hooks in them.
static struct { double ms, worst; int n; } g_draw[2]; // 0 county map, 1 battlefield
static void draw_timed(int which, LARGE_INTEGER t0)
{
	LARGE_INTEGER t1, f;
	QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
	double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
	g_draw[which].ms += ms; g_draw[which].n++;
	if (ms > g_draw[which].worst) g_draw[which].worst = ms;
}
// [Debug] Remote=1: testing without the user's mouse, screen or speakers. The window is cloaked
// and click-through, the sound muted, and hd\bgdrive.ps1 posts WM_REMOTE + n messages to it:
// 1 move, 2/3 left down/up, 5/6 right down/up (lParam = game x, y), 4 key (wParam = virtual key),
// 0x10 screenshot (the 2x picture as lords2hd-shot.bmp next to the game).
static BOOL g_remote;
static POINT g_fake;                // the pretend mouse, in game coordinates
static volatile LONG g_shotWanted;
#define WM_REMOTE (WM_APP + 0x100)
static void shot_save(const DWORD *px, int w, int h);
static BOOL g_wantWide;
static BOOL g_hdScreens = TRUE, g_hdTiles = TRUE, g_hdText = TRUE, g_shoreWaves = TRUE;

// The log stays open and is flushed per line, so it is complete if the game crashes.
static void logf_(const char *fmt, ...)
{
	static FILE *f;
	if (!f) f = fopen(g_logPath, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

static void read_settings(void)
{
	char s[32];
	g_emulate = GetPrivateProfileIntA("Display", "Enabled", 1, g_iniPath) != 0;
	GetPrivateProfileStringA("Display", "Scale", "smooth", s, sizeof s, g_iniPath);
	g_scale = !lstrcmpiA(s, "none") ? SCALE_NONE : !lstrcmpiA(s, "sharp") ? SCALE_SHARP : SCALE_SMOOTH;
	g_fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", 1, g_iniPath) != 0;
	g_windowScale = GetPrivateProfileIntA("Display", "WindowScale", 2, g_iniPath);
	if (g_windowScale < 1) g_windowScale = 1;
	g_integerScale = GetPrivateProfileIntA("Display", "IntegerScale", 0, g_iniPath) != 0;
	g_wantWide = GetPrivateProfileIntA("Display", "Wide", 0, g_iniPath) != 0;
	g_battleZoom = g_wantWide && GetPrivateProfileIntA("Display", "BattleZoom", 1, g_iniPath) != 0;
	g_hdScreens = GetPrivateProfileIntA("Display", "HDScreens", 1, g_iniPath) != 0;
	g_hdTiles = GetPrivateProfileIntA("Display", "HDTiles", 1, g_iniPath) != 0;
	g_hdText = GetPrivateProfileIntA("Display", "SharpText", 1, g_iniPath) != 0;
	g_shoreWaves = GetPrivateProfileIntA("Display", "ShoreWaves", 1, g_iniPath) != 0;
	g_cursorScale = GetPrivateProfileIntA("Display", "CursorScale", 50, g_iniPath);
	if (g_cursorScale < 10) g_cursorScale = 10;
	g_logLoads = GetPrivateProfileIntA("Debug", "LogLoads", 0, g_iniPath) != 0;
	g_dumpPalettes = GetPrivateProfileIntA("Debug", "DumpPalettes", 0, g_iniPath) != 0;
	g_timing = GetPrivateProfileIntA("Debug", "Timing", 0, g_iniPath) != 0;
	g_remote = GetPrivateProfileIntA("Debug", "Remote", 0, g_iniPath) != 0;
	if (g_remote) logf_("remote testing mode: hidden, muted, input from hd\\bgdrive.ps1");
	logf_("settings: Enabled=%d Scale=%s Fullscreen=%d WindowScale=%d IntegerScale=%d Wide=%d CursorScale=%d LogLoads=%d DumpPalettes=%d Timing=%d",
		g_emulate, g_scale == SCALE_NONE ? "none" : g_scale == SCALE_SHARP ? "sharp" : "smooth",
		g_fullscreen, g_windowScale, g_integerScale, g_wantWide, g_cursorScale, g_logLoads, g_dumpPalettes, g_timing);
}

// Crashes: the first thing to ask a player for is lords2hd.log, straight after the crash.
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *ep)
{
	static int count;
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
		code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) && count++ < 20) {
		void *at = ep->ExceptionRecord->ExceptionAddress;
		HMODULE mod = NULL;
		char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)at, &mod))
			GetModuleFileNameA(mod, name, sizeof name);
		logf_("exception %08lx at %p (%s +%lx), info %08lx %08lx", (unsigned long)code, at, name,
			(unsigned long)((BYTE *)at - (BYTE *)mod),
			(unsigned long)ep->ExceptionRecord->ExceptionInformation[0],
			(unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------------
// Shared frame state

static CRITICAL_SECTION g_lock;   // guards the primary surface's pixels and the palette
static HANDLE g_dirtyEvent;
static volatile LONG g_dirty;
static HWND g_hwnd;               // the game's window (WinLords2Class)
static WNDPROC g_gameProc;

typedef struct Palette Palette;
typedef struct Surface Surface;

struct Palette {
	void **vtbl;
	LONG refs;
	PALETTEENTRY e[256];
};

struct Surface {
	void **vtbl;
	LONG refs;
	int w, h, pitch;
	BYTE *bits;
	BOOL primary;
	Palette *pal;
	DWORD caps;
	int org; // where the game's x = 0 is in the surface: the frame's spare columns are on the left
};

// Wide mode puts the game's 640 columns at the right of the frame (x = 0 is g_off pixels in), so
// the county and battle maps can grow into the columns on the left while their side panels stay
// at the right edge. g_off is 0 at 640.
static int g_off;
static volatile LONG g_wideScreen; // the screen on now uses the whole frame (the county map)
static volatile LONG g_roomShown;  // a wider room is on screen (see room_col)

static Surface *g_primary;
static Surface *g_back; // the system-memory surface the game draws into
static BOOL county_map_on(void);
static volatile BOOL g_mapSticky; // the wide map view carries on under dialogs (see county_map_on)

static void mark_dirty(void)
{
	InterlockedExchange(&g_dirty, 1);
	if (g_dirtyEvent) SetEvent(g_dirtyEvent);
}

// Where the 640x480 picture sits in the window's client area: as large as fits, 4:3, centred.
static void view_rect(RECT *out)
{
	RECT c = { 0, 0, GAME_W, GAME_H };
	if (g_hwnd) GetClientRect(g_hwnd, &c);
	int cw = c.right, ch = c.bottom, w, h;
	if (g_scale == SCALE_NONE) {
		w = g_showW;
		h = GAME_H;
	}
	else {
		double s = min((double)cw / g_showW, (double)ch / GAME_H);
		if (g_integerScale && s >= 1.0) s = (double)(int)s;
		w = (int)(g_showW * s + 0.5);
		h = (int)(GAME_H * s + 0.5);
	}
	out->left = (cw - w) / 2;
	out->top = (ch - h) / 2;
	out->right = out->left + w;
	out->bottom = out->top + h;
}

// ---------------------------------------------------------------------------------------------
// Palette logging ([Debug] DumpPalettes): which palette is on screen when each picture is used.
// Each palette that stays on screen for half a second is written to lords2hd_dump\pal_NNN.256
// (the game's own 6-bit format), with the files loaded since the previous one.

#define MAX_RECENT 48
static char g_recent[MAX_RECENT][24];
static int g_recentCount;
static CRITICAL_SECTION g_recentLock;

static void note_load(const char *name)
{
	if (!g_dumpPalettes) return;
	EnterCriticalSection(&g_recentLock);
	if (g_recentCount < MAX_RECENT) lstrcpynA(g_recent[g_recentCount++], name, sizeof g_recent[0]);
	LeaveCriticalSection(&g_recentLock);
}

static void dump_palette_if_new(const PALETTEENTRY *e)
{
	static DWORD seen[256];
	static int seenCount, fileNo;
	DWORD h = 2166136261u;
	for (int i = 0; i < 256; i++) {
		h = (h ^ e[i].peRed) * 16777619u;
		h = (h ^ e[i].peGreen) * 16777619u;
		h = (h ^ e[i].peBlue) * 16777619u;
	}
	for (int i = 0; i < seenCount; i++) if (seen[i] == h) return;
	int black = 0;
	for (int i = 0; i < 256; i++) black += !e[i].peRed && !e[i].peGreen && !e[i].peBlue;
	if (black > 200) return; // mid-fade
	if (seenCount < 256) seen[seenCount++] = h;

	char dir[MAX_PATH], path[MAX_PATH];
	snprintf(dir, sizeof dir, "%s\\lords2hd_dump", g_gameDir);
	CreateDirectoryA(dir, NULL);
	snprintf(path, sizeof path, "%s\\pal_%03d.256", dir, fileNo);
	FILE *f = fopen(path, "wb");
	if (f) {
		for (int i = 0; i < 256; i++) {
			fputc(e[i].peRed >> 2, f);
			fputc(e[i].peGreen >> 2, f);
			fputc(e[i].peBlue >> 2, f);
		}
		fclose(f);
	}
	char list[MAX_RECENT * 26] = "";
	EnterCriticalSection(&g_recentLock);
	for (int i = 0; i < g_recentCount; i++) {
		strcat(list, " ");
		strcat(list, g_recent[i]);
	}
	g_recentCount = 0;
	LeaveCriticalSection(&g_recentLock);
	logf_("palette pal_%03d.256 (%08lx) on screen; loaded since the previous one:%s", fileNo, (unsigned long)h, list);
	fileNo++;
}

// ---------------------------------------------------------------------------------------------
// Presenter

// The part of the frame being shown (g_showW x GAME_H, rows packed), and its colour versions.
static BYTE g_frame8[MAX_W * GAME_H];
static DWORD g_lut[256];
static DWORD g_frame32[MAX_W * GAME_H];
static DWORD g_frame2x[MAX_W * 2 * GAME_H * 2];

// 0x00RRGGBB pixels: (9a + 3b + 3c + d) / 16 per channel, red and blue in one pass.
static inline DWORD mix4(DWORD a, DWORD b, DWORD c, DWORD d)
{
	DWORD rb = (a & 0xff00ff) * 9 + ((b & 0xff00ff) + (c & 0xff00ff)) * 3 + (d & 0xff00ff);
	DWORD g = (a & 0x00ff00) * 9 + ((b & 0x00ff00) + (c & 0x00ff00)) * 3 + (d & 0x00ff00);
	return ((rb >> 4) & 0xff00ff) | ((g >> 4) & 0x00ff00);
}

// Doubles the frame with bilinear interpolation: each output pixel sits a quarter of a source
// pixel from its nearest source pixel, so it is 9:3:3:1 of the four closest.
static void double_frame(int w)
{
	for (int y = 0; y < GAME_H; y++) {
		const DWORD *row = g_frame32 + y * w;
		const DWORD *up = g_frame32 + (y > 0 ? y - 1 : y) * w;
		const DWORD *dn = g_frame32 + (y < GAME_H - 1 ? y + 1 : y) * w;
		DWORD *o0 = g_frame2x + (2 * y) * (2 * w);
		DWORD *o1 = o0 + 2 * w;
		for (int x = 0; x < w; x++) {
			int l = x > 0 ? x - 1 : x, r = x < w - 1 ? x + 1 : x;
			DWORD p = row[x];
			o0[2 * x] = mix4(p, row[l], up[x], up[l]);
			o0[2 * x + 1] = mix4(p, row[r], up[x], up[r]);
			o1[2 * x] = mix4(p, row[l], dn[x], dn[l]);
			o1[2 * x + 1] = mix4(p, row[r], dn[x], dn[r]);
		}
	}
}

// HD paintings. When a full-screen picture is loaded its 2x version is read from
// lords2hd\screens\<name>.bin, with the original 1x picture as a "shadow": a pixel is shown from the
// 2x picture while the frame still holds what the picture put there and the palette still gives it
// the picture's colour. Anything drawn on top, or a fade, shows the smoothed 1x frame.
// HD troops in the zoomed-out battle: 2x blocks per frame pixel, 4 DWORDs each, with the same shadow
// check as the county layers. FUN_004b5333 redrawing a ground cell forgets what stood on it.
static DWORD *g_btLayer;
static BYTE *g_btShadow, *g_btValid;
static struct { BOOL on, seen; BYTE shadow[GAME_W * GAME_H]; BYTE pal[768]; DWORD *px; } g_hdPic;
static BYTE *g_sprValid; // the HD sprite layer's, see stamp_sprite
#define IN_BATTLE_NOW() (*(int *)0x0057c8fc == 2)

// Wider rooms. A painting with a file in lords2hd\wide (four ints: width, the columns where the wide
// art's left and right new sections start, floor row, bar row; then 2w x 960 BGRX) fills the frame
// instead of standing in black bars. In the art the side walls stand at the frame's edges with new
// wall between them. What the game draws on the picture is never split: it shows as one centred
// block over that art, and the bottom bar's as one block against the right edge.
static struct { int w, cutL, cutR, floorY, barY, on, extra; DWORD *px, *out; } g_room;

// Animations with their own background (see Room.AppendKey): where a key's frames are drawn, a pixel
// still holding the frames' background shows the room behind it. Each key is found on screen by
// its pixels that are background in every frame, and remembered until they stop matching.
#define MAX_KEYS 4
#define MAX_KEY_FRAMES 16
static struct {
	int w, h, x, y, found; BYTE *bg, *mark; int probe[24], probes;
	int frames, shown;                       // the frame on screen now (-1 = none matches)
	BYTE *pix[MAX_KEY_FRAMES]; DWORD *hd[MAX_KEY_FRAMES];
} g_keys[MAX_KEYS];
static int g_keyCount;

static BOOL key_at(int k, int x, int y)
{
	for (int i = 0; i < g_keys[k].probes; i++) {
		int p = g_keys[k].probe[i], px = x + p % g_keys[k].w, py = y + p / g_keys[k].w;
		if (g_frame8[py * GAME_W + px] != g_keys[k].bg[p]) return FALSE;
	}
	return TRUE;
}

static void keys_find(void)
{
	for (int k = 0; k < g_keyCount; k++) {
		int w = g_keys[k].w, h = g_keys[k].h;
		if (!g_keys[k].found || !key_at(k, g_keys[k].x, g_keys[k].y)) g_keys[k].found = FALSE;
		for (int y = 0; y + h <= GAME_H && !g_keys[k].found; y++)
			for (int x = 0; x + w <= GAME_W; x++)
				if (key_at(k, x, y)) {
					g_keys[k].x = x; g_keys[k].y = y; g_keys[k].found = TRUE;
					break;
				}
		// Which frame: the one whose figure pixels the screen holds (most of them, as the mouse
		// may be over it).
		g_keys[k].shown = -1;
		if (!g_keys[k].found) continue;
		int best = 0;
		for (int f = 0; f < g_keys[k].frames; f++) {
			int same = 0, all = 0;
			for (int t = 0; t < h; t += 2)
				for (int u = t & 2 ? 1 : 0; u < w; u += 3) {
					int i = t * w + u;
					if (g_keys[k].mark[i] && g_keys[k].pix[f][i] == g_keys[k].bg[i]) continue;
					all++;
					same += g_frame8[(g_keys[k].y + t) * GAME_W + g_keys[k].x + u] == g_keys[k].pix[f][i];
				}
			if (all && same * 10 >= all * 9 && same > best) { best = same; g_keys[k].shown = f; }
		}
	}
}

// The game's pixel at x, y within an animation: 1 = its own background, 2 = its figure, with *hd
// set to the frame's 2x pixels for it; 0 = neither.
static int key_px(int x, int y, BYTE v, const DWORD **hd, int *hw)
{
	for (int k = 0; k < g_keyCount; k++) {
		int u = x - g_keys[k].x, t = y - g_keys[k].y;
		if (!g_keys[k].found || u < 0 || t < 0 || u >= g_keys[k].w || t >= g_keys[k].h) continue;
		int i = t * g_keys[k].w + u, w = g_keys[k].w, f = g_keys[k].shown;
		if (g_keys[k].mark[i] && g_keys[k].bg[i] == v) {
			// Specks of background inside the figure (between dithered legs) take its 2x frame.
			if (f < 0 || u == 0 || t == 0 || u == w - 1 || t == g_keys[k].h - 1) return 1;
			const BYTE *p = g_keys[k].pix[f], *bg = g_keys[k].bg, *m = g_keys[k].mark;
			int fig = 0;
			for (int d = 0; d < 4; d++) {
				int j = i + (d == 0 ? -1 : d == 1 ? 1 : d == 2 ? -w : w);
				fig += !(m[j] && p[j] == bg[j]);
			}
			if (fig < 3) return 1;
		}
		else if (f < 0 || g_keys[k].pix[f][i] != v) return 0;
		*hw = 2 * w;
		*hd = g_keys[k].hd[f] + 2 * t * *hw + 2 * u;
		return 2;
	}
	return 0;
}

static void keys_load(FILE *f)
{
	for (int k = 0; k < g_keyCount; k++) {
		free(g_keys[k].bg); free(g_keys[k].mark); g_keys[k].bg = g_keys[k].mark = NULL;
		for (int f = 0; f < g_keys[k].frames; f++) { free(g_keys[k].pix[f]); free(g_keys[k].hd[f]); }
		g_keys[k].frames = 0;
	}
	g_keyCount = 0;
	int n;
	if (fread(&n, 4, 1, f) != 1) return;
	for (int k = 0; k < n && k < MAX_KEYS; k++) {
		int wh[2];
		if (fread(wh, 4, 2, f) != 2 || wh[0] <= 0 || wh[1] <= 0 || wh[0] > GAME_W || wh[1] > GAME_H) return;
		int size = wh[0] * wh[1];
		BYTE *bg = malloc(size), *mark = malloc(size);
		if (!bg || !mark || fread(bg, 1, size, f) != (size_t)size || fread(mark, 1, size, f) != (size_t)size) { free(bg); free(mark); return; }
		g_keys[k].w = wh[0]; g_keys[k].h = wh[1]; g_keys[k].bg = bg; g_keys[k].mark = mark; g_keys[k].found = FALSE;
		// Probes: up to 24 always-background pixels spread over the frame.
		int all = 0;
		for (int i = 0; i < size; i++) all += mark[i] == 2;
		g_keys[k].probes = 0;
		for (int i = 0, seen = 0; i < size && g_keys[k].probes < 24; i++)
			if (mark[i] == 2 && seen++ % max(1, all / 24) == 0) g_keys[k].probe[g_keys[k].probes++] = i;
		g_keys[k].shown = -1;
		g_keyCount = k + 1;
		int frames;
		if (fread(&frames, 4, 1, f) != 1) return;
		for (int fr = 0; fr < frames && fr < MAX_KEY_FRAMES; fr++) {
			BYTE *pix = malloc(size);
			DWORD *hd = malloc(size * 4 * 4);
			if (!pix || !hd || fread(pix, 1, size, f) != (size_t)size || fread(hd, 4, size * 4, f) != (size_t)(size * 4)) {
				free(pix); free(hd);
				return;
			}
			g_keys[k].pix[fr] = pix; g_keys[k].hd[fr] = hd;
			g_keys[k].frames = fr + 1;
		}
	}
}

// The game column shown at frame column x (-1 for new art), and the art column.
static int room_col(int x, int y, int *art)
{
	int more = g_room.w - GAME_W, left = g_room.extra / 2, ex = g_room.extra;
	if (y >= g_room.barY) *art = x + more - ex;
	else if (y >= g_room.floorY || x < g_room.cutL + left) *art = x;
	else if (x < g_room.cutR + left) *art = x - left + more / 2;
	else if (x < g_room.cutR + ex) *art = g_room.cutR + more / 2 + x - g_room.cutR - left;
	else *art = x - ex + more;
	int g = y >= g_room.barY ? x - ex : x - left;
	return g >= 0 && g < GAME_W ? g : -1;
}

static void room_load(const char *base)
{
	char path[MAX_PATH];
	g_room.on = FALSE;
	snprintf(path, sizeof path, "%s\\lords2hd\\wide\\%s.bin", g_gameDir, base);
	FILE *f = fopen(path, "rb");
	if (!f) return;
	int head[5];
	if (fread(head, 4, 5, f) == 5 && head[0] > GAME_W && head[0] <= 2 * GAME_W) {
		free(g_room.px); free(g_room.out);
		g_room.px = malloc(head[0] * 2 * GAME_H * 2 * 4);
		g_room.out = malloc(head[0] * 2 * GAME_H * 2 * 4);
		if (g_room.px && g_room.out && fread(g_room.px, 4, head[0] * 2 * GAME_H * 2, f) == (size_t)(head[0] * 2 * GAME_H * 2)) {
			g_room.w = head[0]; g_room.cutL = head[1]; g_room.cutR = head[2];
			g_room.floorY = head[3]; g_room.barY = head[4];
			g_room.on = TRUE;
			keys_load(f);
		}
	}
	fclose(f);
	logf_("wider room %s: %s", base, g_room.on ? "loaded" : "bad file");
}

// The frame as the room: the art where the painting is still showing, dimmed along with any fade,
// and the doubled frame wherever the game has drawn on it.
static void room_compose(const PALETTEENTRY *pal, int sw)
{
	int have = 0, want = 0, match = 1;
	for (int i = 0; i < 768; i++) want += g_hdPic.pal[i];
	for (int i = 0; i < 256; i++) {
		have += (pal[i].peRed >> 2) + (pal[i].peGreen >> 2) + (pal[i].peBlue >> 2);
		match &= pal[i].peRed >> 2 == g_hdPic.pal[i * 3] && pal[i].peGreen >> 2 == g_hdPic.pal[i * 3 + 1] &&
			pal[i].peBlue >> 2 == g_hdPic.pal[i * 3 + 2];
	}
	int k = match || !want ? 256 : min(256, have * 256 / want);
	int lw = 2 * g_room.w, ow = 2 * sw;
	keys_find();
	for (int y = 0; y < GAME_H; y++) {
		const BYTE *fr = g_frame8 + y * GAME_W, *sh = g_hdPic.shadow + y * GAME_W;
		for (int x = 0; x < sw; x++) {
			int art, g = room_col(x, y, &art);
			for (int b = 0; b < 2; b++) {
				const DWORD *a = g_room.px + 2 * y * lw + 2 * art + b * lw;
				const DWORD *f = g >= 0 ? g_frame2x + 2 * y * 2 * GAME_W + 2 * g + b * 2 * GAME_W : NULL;
				DWORD *o = g_room.out + (2 * y + b) * ow + 2 * x;
				// Within an animation with its own background: its background shows the room, its
				// figure its own 2x frame.
				int kp = 0, hw = 0;
				const DWORD *kh = NULL;
				if (g >= 0 && fr[g] != sh[g] && g_keyCount) kp = key_px(g, y, fr[g], &kh, &hw);
				for (int c = 0; c < 2; c++) {
					DWORD v;
					if (kp == 2) v = kh[b * hw + c];
					else if (g >= 0 && fr[g] != sh[g] && kp != 1) v = f[c];
					else if (k == 256) v = a[c];
					else v = (((a[c] & 0xff00ff) * k >> 8) & 0xff00ff) | (((a[c] & 0xff00) * k >> 8) & 0xff00);
					o[c] = v;
				}
			}
		}
	}
}

static void hd_picture_loaded(const char *name, const BYTE *pic, int rows)
{
	if (!g_hdScreens) return;
	EnterCriticalSection(&g_lock);
	g_hdPic.on = FALSE;
	g_room.on = FALSE;
	if (g_sprValid) memset(g_sprValid, 0, MAX_W * GAME_H); // a new screen: nothing stamped on it yet
	LeaveCriticalSection(&g_lock);
	if (rows != GAME_H) return;
	char base[64], path[MAX_PATH];
	lstrcpynA(base, name, sizeof base);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;
	CharLowerA(base);
	snprintf(path, sizeof path, "%s\\lords2hd\\screens\\%s.bin", g_gameDir, base);
	FILE *f = fopen(path, "rb");
	if (!f) return;
	if (!g_hdPic.px) g_hdPic.px = malloc(GAME_W * 2 * GAME_H * 2 * 4);
	EnterCriticalSection(&g_lock);
	BOOL ok = g_hdPic.px && fread(g_hdPic.pal, 1, 768, f) == 768 &&
		fread(g_hdPic.px, 4, GAME_W * 2 * GAME_H * 2, f) == GAME_W * 2 * GAME_H * 2;
	if (ok) {
		memcpy(g_hdPic.shadow, pic, GAME_W * GAME_H);
		g_hdPic.on = TRUE;
		g_hdPic.seen = FALSE;
		room_load(base);
	}
	LeaveCriticalSection(&g_lock);
	fclose(f);
	logf_("HD painting %s%s", base, ok ? "" : ": bad file");
	mark_dirty();
}

// Over the doubled frame, the pixels the painting still owns. The palette is compared index by
// index. The picture is forgotten once most of the frame is something else.
static void hd_overlay(const PALETTEENTRY *pal)
{
	EnterCriticalSection(&g_lock);
	if (g_hdPic.on) {
		BYTE ok[256];
		for (int i = 0; i < 256; i++)
			ok[i] = pal[i].peRed >> 2 == g_hdPic.pal[i * 3] && pal[i].peGreen >> 2 == g_hdPic.pal[i * 3 + 1] &&
				pal[i].peBlue >> 2 == g_hdPic.pal[i * 3 + 2];
		int same = 0;
		for (int y = 0; y < GAME_H; y++) {
			const BYTE *fr = g_frame8 + y * GAME_W, *sh = g_hdPic.shadow + y * GAME_W;
			const DWORD *h0 = g_hdPic.px + 2 * y * 2 * GAME_W, *h1 = h0 + 2 * GAME_W;
			DWORD *o0 = g_frame2x + 2 * y * 2 * GAME_W, *o1 = o0 + 2 * GAME_W;
			for (int x = 0; x < GAME_W; x++) {
				if (fr[x] != sh[x]) continue;
				same++;
				if (!ok[fr[x]]) continue;
				o0[2 * x] = h0[2 * x]; o0[2 * x + 1] = h0[2 * x + 1];
				o1[2 * x] = h1[2 * x]; o1[2 * x + 1] = h1[2 * x + 1];
			}
		}
		static int lastOk = -1, lastSame = -1;
		int okCount = 0;
		for (int i = 0; i < 256; i++) okCount += ok[i];
		if (okCount != lastOk || abs(same - lastSame) > 20000) {
			logf_("HD painting: %d of 256 palette entries match, %d pixels unchanged", okCount, same);
			if (okCount < 256) logf_("  entry 100: screen %d,%d,%d picture %d,%d,%d", pal[100].peRed, pal[100].peGreen, pal[100].peBlue,
				g_hdPic.pal[300], g_hdPic.pal[301], g_hdPic.pal[302]);
			lastOk = okCount;
			lastSame = same;
		}
		// The frame reaches the screen a moment after the load: only a picture that has been seen
		// can be gone.
		if (same >= GAME_W * GAME_H / 8) g_hdPic.seen = TRUE;
		else if (g_hdPic.seen) g_hdPic.on = FALSE;
	}
	LeaveCriticalSection(&g_lock);
}

// HD terrain. Ground files (lords2hd\tiles\<set>.bin: one 116x60 slot per frame) are noted against
// the buffer the game loads them into. After a map cell is drawn, each pixel of the tile that the
// frame now holds is stamped into a 2x layer the size of the frame, with its 1x value as a shadow.
// A pixel is shown from the layer while the screen still holds that value, so sprites and text
// over the ground simply show through as the smoothed 1x frame. Partly clipped tiles need no
// special case: pixels the game did not draw do not match.
#define TILE_W 58
#define TILE_H 30
#define MAX_TILE_SETS 16
static struct { const BYTE *buf; DWORD *px; BYTE *wave; int frames; } g_tileSets[MAX_TILE_SETS];
static DWORD *g_layer;
static BYTE *g_layerShadow, *g_layerValid;
// Shore waves: each 2x ground pixel's distance to the shore (0 = not water near a shore).
#define WAVE_BAND 14
static BYTE *g_waveD;
static volatile BOOL g_waveActive;
// Sprites and overhang have their own layer: when one moves away, the ground under it is still
// known, so the screen shows HD ground again instead of a soft patch.
static DWORD *g_sprLayer;
static BYTE *g_sprShadow;
static PALETTEENTRY g_sprPal[256]; // the palette when sprites were last stamped off the map
static BYTE g_tilePal[768];

static BOOL wave_is_water(DWORD p)
{
	int b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;
	return b > g + 15 && b > r + 25;
}

// The base files' coast frames. Pure sea has no shore; rivers and blue fields are left alone.
static BOOL wave_frame(int i)
{
	return (i >= 38 && i <= 53) || (i >= 80 && i <= 92) || (i >= 96 && i <= 121);
}

// For one set: land is any patch of non-water big enough not to be a glint on the sea, and each
// water pixel gets its distance from land. A winter set's sea is ice, so it gets no waves.
static BYTE *wave_build(const DWORD *px, int frames)
{
	enum { W = TILE_W * 2, H = TILE_H * 2, N = W * H, FAR_ = 9999 };
	if (frames <= 121) return NULL;
	const DWORD *sea = px + 22 * N;
	int wet = 0;
	for (int i = 0; i < N; i++) wet += wave_is_water(sea[i]);
	BYTE *out = calloc(frames, N);
	static BYTE land[N], seen[N], inside[N];
	static int stack[N], dist[N];
	if (!out || wet < N / 4) { free(out); return NULL; } // the diamond is half of the slot
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
		int r = y / 2, k = r < TILE_H / 2 ? r : TILE_H - 1 - r, w = 2 + 4 * k, x0 = (TILE_W - w) / 2;
		inside[y * W + x] = x / 2 >= x0 && x / 2 < x0 + w;
	}
	for (int f = 0; f < frames; f++) {
		if (!wave_frame(f)) continue;
		const DWORD *t = px + f * N;
		memset(land, 0, N);
		memset(seen, 0, N);
		for (int i = 0; i < N; i++) {
			if (seen[i] || !inside[i] || wave_is_water(t[i])) continue;
			int sp = 0, head = 0;
			stack[sp++] = i;
			seen[i] = 1;
			while (head < sp) {
				int c = stack[head++], x = c % W;
				int nb[4] = { x > 0 ? c - 1 : -1, x < W - 1 ? c + 1 : -1, c - W, c + W };
				for (int n = 0; n < 4; n++) {
					int o = nb[n];
					if (o < 0 || o >= N || seen[o] || !inside[o] || wave_is_water(t[o])) continue;
					seen[o] = 1;
					stack[sp++] = o;
				}
			}
			if (sp >= 150) for (int k = 0; k < sp; k++) land[stack[k]] = 1;
		}
		// Whatever is left in small pieces is stray pixels on the land's side, not sea.
		memset(seen, 0, N);
		for (int i = 0; i < N; i++) {
			if (seen[i] || !inside[i] || land[i]) continue;
			int sp = 0, head = 0;
			stack[sp++] = i;
			seen[i] = 1;
			while (head < sp) {
				int c = stack[head++], x = c % W;
				int nb[4] = { x > 0 ? c - 1 : -1, x < W - 1 ? c + 1 : -1, c - W, c + W };
				for (int n = 0; n < 4; n++) {
					int o = nb[n];
					if (o < 0 || o >= N || seen[o] || !inside[o] || land[o]) continue;
					seen[o] = 1;
					stack[sp++] = o;
				}
			}
			if (sp < 150) for (int k = 0; k < sp; k++) land[stack[k]] = 2;
		}
		for (int i = 0; i < N; i++) dist[i] = land[i] == 1 ? 0 : FAR_;
		for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { // chamfer distance, 3 = one pixel
			int i = y * W + x, d = dist[i];
			if (x > 0) d = min(d, dist[i - 1] + 3);
			if (y > 0) {
				d = min(d, dist[i - W] + 3);
				if (x > 0) d = min(d, dist[i - W - 1] + 4);
				if (x < W - 1) d = min(d, dist[i - W + 1] + 4);
			}
			dist[i] = d;
		}
		for (int y = H - 1; y >= 0; y--) for (int x = W - 1; x >= 0; x--) {
			int i = y * W + x, d = dist[i];
			if (x < W - 1) d = min(d, dist[i + 1] + 3);
			if (y < H - 1) {
				d = min(d, dist[i + W] + 3);
				if (x < W - 1) d = min(d, dist[i + W + 1] + 4);
				if (x > 0) d = min(d, dist[i + W - 1] + 4);
			}
			dist[i] = d;
		}
		BYTE *o = out + f * N;
		for (int i = 0; i < N; i++) {
			int d = dist[i] / 3;
			if (inside[i] && !land[i] && d >= 1 && d <= WAVE_BAND) o[i] = (BYTE)d;
		}
	}
	return out;
}

static void tiles_loaded(const char *name, void *buffer, int size)
{
	if (!g_hdTiles || !buffer || size < 8) return;
	char base[32], path[MAX_PATH];
	lstrcpynA(base, name, sizeof base);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;
	CharLowerA(base);
	int slot = -1;
	for (int i = 0; i < MAX_TILE_SETS; i++) {
		if (g_tileSets[i].buf == buffer) { slot = i; break; }
		if (!g_tileSets[i].buf && slot < 0) slot = i;
	}
	if (slot < 0) return;
	if (g_tileSets[slot].px) {
		free(g_tileSets[slot].px);
		free(g_tileSets[slot].wave);
		g_tileSets[slot].px = NULL;
		g_tileSets[slot].wave = NULL;
		g_tileSets[slot].buf = NULL;
	}
	snprintf(path, sizeof path, "%s\\lords2hd\\tiles\\%s.bin", g_gameDir, base);
	FILE *f = fopen(path, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	DWORD *px = malloc(bytes);
	if (px && fread(px, 1, bytes, f) == (size_t)bytes) {
		g_tileSets[slot].px = px;
		g_tileSets[slot].frames = bytes / (TILE_W * 2 * TILE_H * 2 * 4);
		g_tileSets[slot].buf = buffer;
		if (g_shoreWaves && !strncmp(base, "base1", 5)) g_tileSets[slot].wave = wave_build(px, g_tileSets[slot].frames);
	}
	else free(px);
	fclose(f);
	if (!g_layer) {
		g_layer = malloc(MAX_W * 2 * GAME_H * 2 * 4);
		g_waveD = calloc(MAX_W * 2, GAME_H * 2);
		g_layerShadow = calloc(MAX_W, GAME_H);
		g_layerValid = calloc(MAX_W, GAME_H);
		g_sprLayer = malloc(MAX_W * 2 * GAME_H * 2 * 4);
		g_sprShadow = calloc(MAX_W, GAME_H);
		g_sprValid = calloc(MAX_W, GAME_H);
		char pal[MAX_PATH];
		snprintf(pal, sizeof pal, "%s\\Base01.256", g_gameDir);
		FILE *p = fopen(pal, "rb");
		if (p) { if (fread(g_tilePal, 1, 768, p) != 768) memset(g_tilePal, 0, 768); fclose(p); }
	}
	logf_("HD tiles %s: %d frames", base, g_tileSets[slot].frames);
}

// Stamps the tile just drawn: frame "index" of the file at "buf", at game position x, y.
static void stamp_tile(const BYTE *buf, int index, int x, int y)
{
	if (!g_layer || !g_back || !buf || index < 0) return;
	DWORD *px = NULL;
	const BYTE *wave = NULL;
	int frames = 0;
	for (int i = 0; i < MAX_TILE_SETS; i++)
		if (g_tileSets[i].buf == buf) { px = g_tileSets[i].px; wave = g_tileSets[i].wave; frames = g_tileSets[i].frames; break; }
	if (!px || index >= frames) return;
	if (wave) wave += index * (TILE_W * 2 * TILE_H * 2);
	const short *rec = (const short *)(buf + 8 + index * 16);
	if (rec[0] != TILE_W || rec[1] != TILE_H) return;
	const BYTE *src = buf + *(const int *)(rec + 2);
	const DWORD *hd = px + index * (TILE_W * 2 * TILE_H * 2);
	int W = g_back->w;
	for (int r = 0; r < TILE_H; r++) {
		int k = r < TILE_H / 2 ? r : TILE_H - 1 - r;
		int w = 2 + 4 * k, x0 = (TILE_W - w) / 2;
		int fy = y + r;
		if (fy < 0 || fy >= GAME_H) { src += w; continue; }
		const BYTE *row = g_back->bits + fy * g_back->pitch;
		for (int i = 0; i < w; i++, src++) {
			int tx = x0 + i, fx = x + g_back->org + tx;
			if (fx < 0 || fx >= W || row[fx] != *src) continue;
			const DWORD *h0 = hd + (2 * r) * (2 * TILE_W) + 2 * tx, *h1 = h0 + 2 * TILE_W;
			DWORD *o0 = g_layer + (2 * fy) * (2 * MAX_W) + 2 * fx, *o1 = o0 + 2 * MAX_W;
			o0[0] = h0[0]; o0[1] = h0[1];
			o1[0] = h1[0]; o1[1] = h1[1];
			if (g_waveD) {
				BYTE *wd = g_waveD + (2 * fy) * (2 * MAX_W) + 2 * fx;
				const BYTE *ws = wave ? wave + (2 * r) * (2 * TILE_W) + 2 * tx : NULL;
				wd[0] = ws ? ws[0] : 0; wd[1] = ws ? ws[1] : 0;
				wd[2 * MAX_W] = ws ? ws[2 * TILE_W] : 0; wd[2 * MAX_W + 1] = ws ? ws[2 * TILE_W + 1] : 0;
			}
			g_layerShadow[fy * MAX_W + fx] = *src;
			g_layerValid[fy * MAX_W + fx] = 1;
		}
	}
}

// HD sprites: lords2hd\sprites\<file>.bin, noted against the buffer the game loaded the file into.
// Each hooked blitter knows the buffer and frame; afterwards every opaque pixel of the frame that
// the screen now holds is stamped into the layer. A wrong guess of position only fails the check.
#define MAX_SPRITE_SETS 64
// pal: the palette the 2x art was made with (lords2hd\sprites\<file>.pal, else Base01.256), 6-bit.
static struct { const BYTE *buf; BYTE *data; BYTE pal[768]; } g_spriteSets[MAX_SPRITE_SETS];

static void read_palette(const char *path, BYTE *pal)
{
	FILE *f = fopen(path, "rb");
	if (!f || fread(pal, 1, 768, f) != 768) memset(pal, 0, 768);
	if (f) fclose(f);
}

static void sprites_loaded(const char *name, const void *buffer)
{
	if (!g_hdTiles) return;
	int slot = -1;
	for (int i = 0; i < MAX_SPRITE_SETS; i++) {
		if (g_spriteSets[i].buf == buffer) {
			free(g_spriteSets[i].data);
			g_spriteSets[i].buf = NULL;
			g_spriteSets[i].data = NULL;
		}
		if (!g_spriteSets[i].buf && slot < 0) slot = i;
	}
	char base[32], path[MAX_PATH];
	lstrcpynA(base, name, sizeof base);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;
	CharLowerA(base);
	snprintf(path, sizeof path, "%s\\lords2hd\\sprites\\%s.bin", g_gameDir, base);
	FILE *f = fopen(path, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	BYTE *data = slot >= 0 ? malloc(bytes) : NULL;
	if (data && fread(data, 1, bytes, f) == (size_t)bytes) {
		g_spriteSets[slot].buf = buffer;
		g_spriteSets[slot].data = data;
		snprintf(path, sizeof path, "%s\\lords2hd\\sprites\\%s.pal", g_gameDir, base);
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) snprintf(path, sizeof path, "%s\\Base01.256", g_gameDir);
		read_palette(path, g_spriteSets[slot].pal);
		logf_("HD sprites %s", base);
	}
	else free(data);
	fclose(f);
}

static void stamp_sprite(const BYTE *buf, int index, int x, int y)
{
	if (battle_zoom_on() && !g_btLayer) {
		g_btLayer = malloc(g_frameW * g_frameH * 4 * 4);
		g_btShadow = calloc(g_frameW, g_frameH);
		g_btValid = calloc(g_frameW, g_frameH);
	}
	const BYTE *data = NULL, *setPal = NULL;
	for (int i = 0; i < MAX_SPRITE_SETS; i++)
		if (g_spriteSets[i].buf == buf) { data = g_spriteSets[i].data; setPal = g_spriteSets[i].pal; break; }
	if (!data || index < 0 || index >= *(const int *)data || !g_back) return;
	// Off the map a sprite may be drawn under another screen's palette than its art was made with
	// (map portraits in the Armoury's scroll): then it stays as the game drew it.
	if (!county_map_on() && !IN_BATTLE_NOW() && g_primary && g_primary->pal) {
		const PALETTEENTRY *e = g_primary->pal->e;
		const BYTE *idx0 = data + ((const int *)data)[1 + index] + 4;
		int w0 = *(const WORD *)(idx0 - 4), h0 = *(const WORD *)(idx0 - 2);
		BYTE used[256] = { 0 };
		if (((const int *)data)[1 + index]) for (int i = 0; i < w0 * h0; i++) used[idx0[i]] = 1;
		for (int i = 1; i < 256; i++)
			if (used[i] && (e[i].peRed >> 2 != setPal[i * 3] || e[i].peGreen >> 2 != setPal[i * 3 + 1] || e[i].peBlue >> 2 != setPal[i * 3 + 2])) return;
	}
	if (!g_sprLayer) { // screens before any map: the layer on its own
		g_sprLayer = malloc(MAX_W * 2 * GAME_H * 2 * 4);
		g_sprShadow = calloc(MAX_W, GAME_H);
		g_sprValid = calloc(MAX_W, GAME_H);
		if (!g_sprLayer || !g_sprShadow || !g_sprValid) return;
	}
	// Off the map the screen's own palette is what the sprite was made for (see screen_sprites).
	if (!county_map_on() && g_primary && g_primary->pal) memcpy(g_sprPal, g_primary->pal->e, sizeof g_sprPal);
	int off = ((const int *)data)[1 + index];
	if (!off) return;
	const BYTE *p = data + off;
	int w = *(const WORD *)p, h = *(const WORD *)(p + 2);
	const BYTE *idx = p + 4;
	const DWORD *hd = (const DWORD *)(idx + w * h);
	if (battle_zoom_on() && g_btLayer) {
		for (int r = 0; r < h; r++) {
			int fy = y + r;
			if (fy < 0 || fy >= g_frameH) continue;
			const BYTE *row = g_back->bits + fy * g_back->pitch;
			for (int c = 0; c < w; c++) {
				BYTE v = idx[r * w + c];
				int fx = x + g_back->org + c;
				if (!v || fx < 0 || fx >= g_frameW || row[fx] != v) continue;
				const DWORD *h0 = hd + (2 * r) * (2 * w) + 2 * c, *h1 = h0 + 2 * w;
				DWORD *o = g_btLayer + (fy * g_frameW + fx) * 4;
				o[0] = h0[0]; o[1] = h0[1]; o[2] = h1[0]; o[3] = h1[1];
				g_btShadow[fy * g_frameW + fx] = v;
				g_btValid[fy * g_frameW + fx] = 1;
			}
		}
		return;
	}
	static int logged;
	if (g_timing && logged < 60) {
		int total = 0, hit = 0;
		for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) {
			int fy = y + r, fx = x + g_back->org + c;
			if (!idx[r * w + c]) continue;
			total++;
			if (fy >= 0 && fy < GAME_H && fx >= 0 && fx < g_back->w && g_back->bits[fy * g_back->pitch + fx] == idx[r * w + c]) hit++;
		}
		logf_("sprite frame %d %dx%d at %d,%d: %d of %d pixels match", index, w, h, x, y, hit, total);
		logged++;
	}
	for (int r = 0; r < h; r++) {
		int fy = y + r;
		if (fy < 0 || fy >= GAME_H) continue;
		const BYTE *row = g_back->bits + fy * g_back->pitch;
		for (int c = 0; c < w; c++) {
			BYTE v = idx[r * w + c];
			int fx = x + g_back->org + c;
			if (!v || fx < 0 || fx >= g_back->w || row[fx] != v) continue;
			const DWORD *h0 = hd + (2 * r) * (2 * w) + 2 * c, *h1 = h0 + 2 * w;
			DWORD *o0 = g_sprLayer + (2 * fy) * (2 * MAX_W) + 2 * fx, *o1 = o0 + 2 * MAX_W;
			o0[0] = h0[0]; o0[1] = h0[1];
			o1[0] = h1[0]; o1[1] = h1[1];
			g_sprShadow[fy * MAX_W + fx] = v;
			g_sprValid[fy * MAX_W + fx] = 1;
		}
	}
}

// The minimap: FUN_0046a037 loads a 128x128 picture of the counties (map file frame variant*5+1)
// into 0x569590; FUN_00410cbd copies it and repaints every pixel holding 10..13 (four placeholder
// shades) in its county's colour (0x52aff0: county per pixel). In HD: the picture's 2x version, its
// placeholder pixels recoloured by their shade from the colours the game just used for that county.
#define MINI_PIC ((const BYTE *)0x00569590)
#define MINI_COUNTY ((const BYTE *)0x0052aff0)
static DWORD *g_miniHD; // 256x256 BGRX, NULL when the map has no HD file

typedef void (__cdecl *mini_load_t)(unsigned);
static mini_load_t o_mini_load;
static void __cdecl h_mini_load(unsigned which)
{
	o_mini_load(which);
	free(g_miniHD);
	g_miniHD = NULL;
	char base[16], path[MAX_PATH];
	lstrcpynA(base, (const char *)0x004d6af8 + ((int)which >> 2) * 0x10, sizeof base);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;
	CharLowerA(base);
	snprintf(path, sizeof path, "%s\\lords2hd\\sprites\\%s.bin", g_gameDir, base);
	FILE *f = fopen(path, "rb");
	if (!f) return;
	int frame = (which & 3) * 5 + 1, count = 0, off = 0;
	WORD wh[2];
	if (fread(&count, 4, 1, f) == 1 && frame < count && !fseek(f, 4 + 4 * frame, SEEK_SET) && fread(&off, 4, 1, f) == 1 && off &&
		!fseek(f, off, SEEK_SET) && fread(wh, 2, 2, f) == 2 && wh[0] == 128 && wh[1] == 128 && !fseek(f, 128 * 128, SEEK_CUR)) {
		g_miniHD = malloc(256 * 256 * 4);
		if (g_miniHD && fread(g_miniHD, 4, 256 * 256, f) != 256 * 256) { free(g_miniHD); g_miniHD = NULL; }
	}
	fclose(f);
	logf_("minimap %s variant %u%s", base, which & 3, g_miniHD ? ": HD" : "");
}

static DWORD mix_rgb(DWORD a, DWORD b, int t) // t of 256 towards b
{
	DWORD rb = ((a & 0xff00ff) * (256 - t) + (b & 0xff00ff) * t) >> 8 & 0xff00ff;
	DWORD g = ((a & 0xff00) * (256 - t) + (b & 0xff00) * t) >> 8 & 0xff00;
	return rb | g;
}

typedef void (__cdecl *mini_paint_t)(unsigned, int, int, int);
static mini_paint_t o_mini_paint;
static void __cdecl h_mini_paint(unsigned player, int x, int y, int mode)
{
	o_mini_paint(player, x, y, mode);
	if (!g_miniHD || !g_sprLayer || !g_back || !g_layer) return; // g_layer: g_tilePal is loaded
	// The map's own palette, not the screen's: coming back from another screen the screen is still
	// faded to black when the minimap is drawn, and its colours would be baked in black.
	DWORD pal[256];
	for (int i = 0; i < 256; i++)
		pal[i] = (g_tilePal[i * 3] * 255 / 63 << 16) | (g_tilePal[i * 3 + 1] * 255 / 63 << 8) | g_tilePal[i * 3 + 2] * 255 / 63;
	// The colour each county got for each shade, read back from what was drawn.
	static short shade[256][4];
	for (int i = 0; i < 256; i++) shade[i][0] = shade[i][1] = shade[i][2] = shade[i][3] = -1;
	for (int r = 0; r < 128; r++) {
		int fy = y + r;
		if (fy < 0 || fy >= GAME_H) continue;
		const BYTE *row = g_back->bits + fy * g_back->pitch + g_back->org + x;
		for (int c = 0; c < 128; c++) {
			BYTE v = MINI_PIC[r * 128 + c];
			if (v >= 10 && v <= 13) shade[MINI_COUNTY[r * 128 + c]][v - 10] = row[c];
		}
	}
	// The placeholder shades' own colours, to tell an HD pixel's shade.
	DWORD ref[4];
	int stamped = 0;
	for (int s = 0; s < 4; s++) ref[s] = pal[10 + s];
	for (int r = 0; r < 128; r++) {
		int fy = y + r;
		if (fy < 0 || fy >= GAME_H) continue;
		const BYTE *row = g_back->bits + fy * g_back->pitch;
		for (int c = 0; c < 128; c++) {
			int fx = x + g_back->org + c;
			if (fx < 0 || fx >= g_back->w) continue;
			BYTE v = MINI_PIC[r * 128 + c], now = row[fx];
			BOOL placeholder = v >= 10 && v <= 13;
			if (!placeholder && now != v) continue;        // something else drawn there
			const short *cs = placeholder ? shade[MINI_COUNTY[r * 128 + c]] : NULL;
			if (placeholder && (cs[v - 10] < 0 || cs[v - 10] != now)) continue; // not what we read back
			DWORD *o0 = g_sprLayer + (2 * fy) * (2 * MAX_W) + 2 * fx, *o1 = o0 + 2 * MAX_W;
			for (int b = 0; b < 4; b++) {
				DWORD h = g_miniHD[(2 * r + (b >> 1)) * 256 + 2 * c + (b & 1)];
				if (placeholder && v == 10) h = pal[now]; // shade 0 is the county's border line (white for the selected one)
				else if (placeholder) {
					// The HD pixel's shade: between the two nearest placeholder colours by brightness.
					int lum = ((h >> 16) & 255) + ((h >> 8) & 255) + (h & 255), L[4];
					for (int s = 0; s < 4; s++) L[s] = ((ref[s] >> 16) & 255) + ((ref[s] >> 8) & 255) + (ref[s] & 255);
					int lo = 1, hi = 1, best = 1 << 30;
					for (int s = 1; s < 4; s++) if (abs(L[s] - lum) < best) { best = abs(L[s] - lum); lo = s; }
					hi = lo;
					for (int s = 1; s < 4; s++) if (s != lo && cs[s] >= 0 && (lum - L[lo]) * (L[s] - L[lo]) > 0 && (hi == lo || abs(L[s] - L[lo]) < abs(L[hi] - L[lo]))) hi = s;
					DWORD a = pal[cs[lo] >= 0 ? cs[lo] : now], z = pal[cs[hi] >= 0 ? cs[hi] : now];
					int t = hi == lo || L[hi] == L[lo] ? 0 : max(0, min(256, (lum - L[lo]) * 256 / (L[hi] - L[lo])));
					h = mix_rgb(a, z, t);
				}
				(b < 2 ? o0 : o1)[b & 1] = h;
			}
			g_sprShadow[fy * MAX_W + fx] = now;
			g_sprValid[fy * MAX_W + fx] = 1;
			stamped++;
		}
	}
	static int logged;
	if (logged++ < 6) logf_("minimap at %d,%d mode %d: %d of 16384 pixels HD (back %dx%d org %d)", x, y, mode, stamped, g_back->w, g_back->h, g_back->org);
}

static void text_clear(int x, int y, int w, int h);
static void text_stale(void);
static void text_publish(int y0, int y1);

static void forget_tiles(void)
{
	if (g_layerValid) memset(g_layerValid, 0, MAX_W * GAME_H);
	// The side panel (minimap, county box) is not redrawn with the map: its sprites stay.
	if (g_sprValid) {
		int keep = g_off + PANEL_X;
		// Only remembered (2), as the letters: a scroll's icons come back from the game's saved copy.
		if (keep <= 0 || keep >= MAX_W || IN_BATTLE_NOW()) keep = MAX_W;
		for (int y = 0; y < GAME_H; y++) {
			BYTE *v = g_sprValid + y * MAX_W;
			for (int x = 0; x < keep; x++) if (v[x]) v[x] = 2;
		}
	}
	text_stale(); // a scroll over the map comes back from the game's saved copy, letters and all
}

// One 2x pixel of water near a shore: crests roll in towards the land and fade up as they arrive,
// broken by drifting noise, with a wash pulsing on the waterline itself.
static BYTE g_waveSin[256], g_waveNoise[64 * 64];

static void wave_tables(void)
{
	static BOOL done;
	if (done) return;
	done = TRUE;
	for (int i = 0; i < 256; i++) g_waveSin[i] = (BYTE)(127.5 + 127.5 * sin(i * 6.2831853 / 256));
	unsigned seed = 12345;
	for (int i = 0; i < 64 * 64; i++) {
		seed = seed * 1103515245 + 12345;
		g_waveNoise[i] = (BYTE)((seed >> 16) & 255);
	}
	for (int pass = 0; pass < 2; pass++) { // soften to blobs a few pixels across
		for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
			int s = 0;
			for (int a = -1; a <= 1; a++) for (int b = -1; b <= 1; b++) s += g_waveNoise[((y + a) & 63) * 64 + ((x + b) & 63)];
			g_waveNoise[y * 64 + x] = (BYTE)(s / 9);
		}
	}
}

static __inline void wave_px(DWORD *o, int d, int X, int Y, DWORD t)
{
	int wobble = (g_waveSin[(X / 6) & 255] + g_waveSin[(Y / 3 + X / 13) & 255]) / 3;
	int c = d * 256 / WAVE_BAND + (int)(t / 20) + wobble;
	int s = g_waveSin[c & 255];
	int a = s > 150 ? (s - 150) * 255 / 105 : 0;
	a = a * (WAVE_BAND + 1 - d) / WAVE_BAND;
	if (d <= 2) a = max(a, 70 + g_waveSin[(t / 12 + X / 9) & 255] / 3);
	int n = g_waveNoise[(((Y + t / 90) & 63) * 64) + ((X / 2 + t / 160) & 63)];
	a = a * max(n * 3 - 200, 0) / 256; // the noise averages about 128
	if (a <= 0) return;
	if (a > 215) a = 215;
	DWORD p = *o;
	int b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;
	b += (250 - b) * a / 256; g += (245 - g) * a / 256; r += (235 - r) * a / 256;
	*o = (r << 16) | (g << 8) | b;
}

// Off the map (painted screens, scrolls): the HD sprites the screen still shows, while the palette is
// the one they were drawn under, so a fade shows the smoothed frame.
static void screen_sprites(const PALETTEENTRY *pal, int sx, int sw)
{
	if (!g_sprLayer || county_map_on() || IN_BATTLE_NOW() || memcmp(pal, g_sprPal, sizeof g_sprPal)) return;
	for (int y = 0; y < GAME_H; y++) {
		const BYTE *fr = g_frame8 + y * sw;
		const BYTE *ss = g_sprShadow + y * MAX_W + sx, *sv = g_sprValid + y * MAX_W + sx;
		const DWORD *s0 = g_sprLayer + (2 * y) * (2 * MAX_W) + 2 * sx, *s1 = s0 + 2 * MAX_W;
		DWORD *o0 = g_frame2x + (2 * y) * (2 * sw), *o1 = o0 + 2 * sw;
		for (int x = 0; x < sw; x++) {
			if (!sv[x] || fr[x] != ss[x]) continue;
			o0[2 * x] = s0[2 * x]; o0[2 * x + 1] = s0[2 * x + 1];
			o1[2 * x] = s1[2 * x]; o1[2 * x + 1] = s1[2 * x + 1];
		}
	}
}

// Over the doubled picture (sw wide, starting at frame column sx), the ground the layer still owns.
static void tile_overlay(const PALETTEENTRY *pal, int sx, int sw)
{
	int zoom = *(int *)0x0057cb18, gameMode = *(int *)0x0052af94;
	BOOL overview = zoom == 2 && gameMode == 3 && *(int *)0x0057c8fc != 2;
	BOOL waved = FALSE;
	DWORD waveT = GetTickCount();
	g_waveActive = FALSE;
	wave_tables();
	if (!g_layer || !g_hdTiles || (!county_map_on() && !overview) || (zoom != 0 && !overview)) return;
	BYTE ok[256];
	for (int i = 0; i < 256; i++)
		ok[i] = pal[i].peRed >> 2 == g_tilePal[i * 3] && pal[i].peGreen >> 2 == g_tilePal[i * 3 + 1] &&
			pal[i].peBlue >> 2 == g_tilePal[i * 3 + 2];
	for (int y = 0; y < GAME_H; y++) {
		const BYTE *fr = g_frame8 + y * sw;
		const BYTE *sh = g_layerShadow + y * MAX_W + sx, *va = g_layerValid + y * MAX_W + sx;
		const DWORD *h0 = g_layer + (2 * y) * (2 * MAX_W) + 2 * sx, *h1 = h0 + 2 * MAX_W;
		const BYTE *ss = g_sprShadow + y * MAX_W + sx, *sv = g_sprValid + y * MAX_W + sx;
		const DWORD *s0 = g_sprLayer + (2 * y) * (2 * MAX_W) + 2 * sx, *s1 = s0 + 2 * MAX_W;
		DWORD *o0 = g_frame2x + (2 * y) * (2 * sw), *o1 = o0 + 2 * sw;
		for (int x = 0; x < sw; x++) {
			if (!ok[fr[x]]) continue;
			BOOL spr = sv[x] && fr[x] == ss[x];
			if (spr && sv[x] == 2) // remembered only: inside a cluster of matching pixels
				spr = x > 0 && x < sw - 1 && sv[x - 1] && fr[x - 1] == ss[x - 1] && sv[x + 1] && fr[x + 1] == ss[x + 1];
			if (spr) {
				o0[2 * x] = s0[2 * x]; o0[2 * x + 1] = s0[2 * x + 1];
				o1[2 * x] = s1[2 * x]; o1[2 * x + 1] = s1[2 * x + 1];
			}
			else if (va[x] && fr[x] == sh[x]) {
				o0[2 * x] = h0[2 * x]; o0[2 * x + 1] = h0[2 * x + 1];
				o1[2 * x] = h1[2 * x]; o1[2 * x + 1] = h1[2 * x + 1];
				if (g_shoreWaves && zoom == 0 && g_waveD) {
					int X = 2 * (x + sx), Y = 2 * y;
					const BYTE *w0 = g_waveD + Y * (2 * MAX_W) + X, *w1 = w0 + 2 * MAX_W;
					if (w0[0] | w0[1] | w1[0] | w1[1]) {
						waved = TRUE;
						if (w0[0]) wave_px(&o0[2 * x], w0[0], X, Y, waveT);
						if (w0[1]) wave_px(&o0[2 * x + 1], w0[1], X + 1, Y, waveT);
						if (w1[0]) wave_px(&o1[2 * x], w1[0], X, Y + 1, waveT);
						if (w1[1]) wave_px(&o1[2 * x + 1], w1[1], X + 1, Y + 1, waveT);
					}
				}
			}
		}
	}
	g_waveActive = waved;
}

// Sharp text: each letter is enlarged by the EPX rule from its own shape into a layer of 2x palette
// indices (so fades and palette changes still apply), over whatever was under it, which keeps the
// game's three-pass engraved letters. A frame pixel shows from the layer while it still holds what
// the letter left there.
static BYTE *g_txt, *g_txtShadow, *g_txtValid;
// The layer as of the frame on screen: the live one follows the game's back buffer, where a scroll
// being redrawn has lost its letters for a moment while the screen still shows the finished frame
// (all text blinked soft). Rows are copied over with the frame, under g_lock.
static BYTE *g_txtS, *g_txtShadowS, *g_txtValidS;

static void text_publish(int y0, int y1)
{
	if (!g_txt || !g_back) return;
	int W = g_back->w, H = g_back->h;
	if (!g_txtS) {
		g_txtS = malloc(W * H * 4);
		g_txtShadowS = malloc(W * H);
		g_txtValidS = calloc(W, H);
		if (!g_txtS || !g_txtShadowS || !g_txtValidS) return;
	}
	y0 = max(0, y0); y1 = min(H, y1);
	if (y1 <= y0) return;
	memcpy(g_txtS + y0 * W * 4, g_txt + y0 * W * 4, (y1 - y0) * W * 4);
	memcpy(g_txtShadowS + y0 * W, g_txtShadow + y0 * W, (y1 - y0) * W);
	memcpy(g_txtValidS + y0 * W, g_txtValid + y0 * W, (y1 - y0) * W);
}

static void text_clear(int x, int y, int w, int h)
{
	if (!g_txtValid || !g_back) return;
	x += g_back->org;
	int x0 = max(x, 0), x1 = min(x + w, g_back->w);
	if (x1 <= x0) return;
	for (int r = max(y, 0); r < min(y + h, g_back->h); r++) memset(g_txtValid + r * g_back->w + x0, 0, x1 - x0);
}
static void text_stale(void);

// After a full map redraw the letters are only remembered (2): the game often puts a scroll back
// from its saved copy without drawing the text again. A remembered pixel shows only inside a
// cluster of matching ones, so map pixels that happen to have a letter's colour stay plain.
static void text_stale(void)
{
	if (!g_txtValid || !g_back) return;
	int n = g_back->w * g_back->h;
	for (int i = 0; i < n; i++) if (g_txtValid[i]) g_txtValid[i] = 2;
}

// w x h letter at game x, y, drawn in "colour" (-1: its own pixels); before = the frame under it.
static void text_stamp(const BYTE *src, int w, int h, int x, int y, int colour, const BYTE *before)
{
	int W = g_back->w, H = g_back->h;
	if (!g_txt) {
		g_txt = malloc(W * H * 4);
		g_txtShadow = malloc(W * H);
		g_txtValid = calloc(W, H);
		if (!g_txt || !g_txtShadow || !g_txtValid) { g_hdText = FALSE; return; }
	}
#define ON(c, r) ((c) >= 0 && (r) >= 0 && (c) < w && (r) < h && src[(r) * w + (c)] != 0)
	for (int r = 0; r < h; r++) {
		int fy = y + r;
		if (fy < 0 || fy >= H) continue;
		for (int c = 0; c < w; c++) {
			int fx = x + g_back->org + c;
			if (fx < 0 || fx >= W) continue;
			BOOL P = ON(c, r), A = ON(c, r - 1), B = ON(c + 1, r), C = ON(c - 1, r), D = ON(c, r + 1);
			BOOL s[4] = { P, P, P, P };
			if (C == A && C != D && A != B) s[0] = A;
			if (A == B && A != C && B != D) s[1] = B;
			if (D == C && D != B && C != A) s[2] = C;
			if (B == D && B != A && D != C) s[3] = D;
			if (!s[0] && !s[1] && !s[2] && !s[3]) continue;
			int f = fy * W + fx;
			BYTE old = before[r * w + c], now = g_back->bits[fy * g_back->pitch + fx];
			BYTE *t = g_txt + f * 4;
			if (!(g_txtValid[f] && g_txtShadow[f] == old)) t[0] = t[1] = t[2] = t[3] = old;
			for (int i = 0; i < 4; i++) if (s[i]) t[i] = colour >= 0 ? (BYTE)colour : now;
			g_txtShadow[f] = now;
			g_txtValid[f] = 1;
		}
	}
#undef ON
}

static void text_overlay(int sx, int sw)
{
	if (!g_txt || !g_hdText) return;
	int W = g_back->w;
	// what the frame on screen had (see text_publish); the live layer until the first copy
	const BYTE *g_txt_ = g_txtS ? g_txtS : g_txt, *shadow = g_txtS ? g_txtShadowS : g_txtShadow, *valid = g_txtS ? g_txtValidS : g_txtValid;
	for (int y = 0; y < GAME_H; y++) {
		const BYTE *fr = g_frame8 + y * sw;
		int f0 = y * W + sx;
		DWORD *o0 = g_frame2x + (2 * y) * (2 * sw), *o1 = o0 + 2 * sw;
		for (int x = 0; x < sw; x++) {
			int f = f0 + x;
			if (!valid[f] || fr[x] != shadow[f]) continue;
			if (valid[f] == 2) {
				int around = 0;
				if (x > 0 && valid[f - 1] && fr[x - 1] == shadow[f - 1]) around++;
				if (x < sw - 1 && valid[f + 1] && fr[x + 1] == shadow[f + 1]) around++;
				if (y > 0 && valid[f - W] && g_frame8[(y - 1) * sw + x] == shadow[f - W]) around++;
				if (y < GAME_H - 1 && valid[f + W] && g_frame8[(y + 1) * sw + x] == shadow[f + W]) around++;
				if (around < 2) continue;
			}
			const BYTE *t = g_txt_ + f * 4;
			o0[2 * x] = g_lut[t[0]]; o0[2 * x + 1] = g_lut[t[1]];
			o1[2 * x] = g_lut[t[2]]; o1[2 * x + 1] = g_lut[t[3]];
		}
	}
}

static volatile LONG g_forcePresent; // the window needs repainting even if the frame is the same
static BYTE g_shown8[MAX_W * GAME_H];

// The zoomed-out battle's picture, twice the size of the normal one: the battlefield 1:1 from the
// frame's tall area, the side panel and the top bar doubled. Above 2x on screen it is smoothed
// once more; below (1080p is 1.125x) GDI's smooth mode does the rest.
static BOOL present_battle(const PALETTEENTRY *pal, BOOL palSame)
{
	static DWORD *pic, *pic2;
	static BYTE *copy, *shown;
	static RECT lastView;
	int lw = 2 * (GAME_W + g_extra), lh = 2 * GAME_H, mapW = battle_map_w();
	int fh = g_primary->h, pitch = g_primary->pitch;
	if (!pic) {
		pic = malloc(lw * lh * 4);
		pic2 = malloc(lw * 2 * lh * 2 * 4);
		copy = malloc(pitch * fh);
		shown = malloc(pitch * fh);
		if (!pic || !pic2 || !copy || !shown) return FALSE;
	}
	EnterCriticalSection(&g_lock);
	memcpy(copy, g_primary->bits, pitch * fh);
	LeaveCriticalSection(&g_lock);
	RECT v, c;
	view_rect(&v);
	BOOL forced = InterlockedExchange(&g_forcePresent, 0) || !EqualRect(&v, &lastView);
	if (!forced && palSame && memcmp(copy, shown, pitch * fh) == 0) return FALSE;
	memcpy(shown, copy, pitch * fh);
	lastView = v;

	for (int i = 0; i < 256; i++) g_lut[i] = (pal[i].peRed << 16) | (pal[i].peGreen << 8) | pal[i].peBlue;
	const BYTE *frame = copy + g_off; // the game's x = 0
	for (int sy = 48; sy < lh; sy++) {
		const BYTE *row = frame + (24 + sy - 48) * pitch + PANEL_X - mapW;
		DWORD *o = pic + sy * lw;
		for (int sx = 0; sx < mapW; sx++) o[sx] = g_lut[row[sx]];
		// The panel doubled smoothly (3:1 between neighbours), not as square blocks: its stone and
		// the battle overview have no 2x art.
		int py = sy - 48, y0 = py / 2, y1 = max(0, min(GAME_H - 25, y0 + ((py & 1) ? 1 : -1)));
		const BYTE *pan = frame + (24 + y0) * pitch + PANEL_X, *pan1 = frame + (24 + y1) * pitch + PANEL_X;
		int pw = (lw - mapW) / 2;
		for (int sx = mapW; sx < lw; sx++) {
			int px = sx - mapW, x0 = px / 2, x1 = max(0, min(pw - 1, x0 + ((px & 1) ? 1 : -1)));
			DWORD a = g_lut[pan[x0]], b = g_lut[pan[x1]], c2 = g_lut[pan1[x0]], d = g_lut[pan1[x1]];
			DWORD rb = (a & 0xff00ff) * 9 + (b & 0xff00ff) * 3 + (c2 & 0xff00ff) * 3 + (d & 0xff00ff);
			DWORD gg = (a & 0xff00) * 9 + (b & 0xff00) * 3 + (c2 & 0xff00) * 3 + (d & 0xff00);
			o[sx] = ((rb >> 4) & 0xff00ff) | ((gg >> 4) & 0xff00);
		}
	}
	for (int sy = 0; sy < 48; sy++) {
		const BYTE *bar = frame + (sy / 2) * pitch - g_extra;
		DWORD *o = pic + sy * lw;
		for (int sx = 0; sx < lw; sx++) o[sx] = g_lut[bar[sx / 2]];
	}

	// Letters in the doubled top bar and side panel.
	if (g_txt && g_hdText) {
		int W = g_back->w;
		for (int sy = 0; sy < lh; sy++) {
			DWORD *o = pic + sy * lw;
			int from = sy < 48 ? 0 : mapW;
			for (int sx = from; sx < lw; sx++) {
				int gx = sy < 48 ? g_off - g_extra + sx / 2 : g_off + PANEL_X + (sx - mapW) / 2;
				int gy = sy < 48 ? sy / 2 : 24 + (sy - 48) / 2;
				int f = gy * W + gx;
				if ((g_txtS ? g_txtValidS : g_txtValid)[f] && copy[gy * pitch + gx] == (g_txtS ? g_txtShadowS : g_txtShadow)[f])
					o[sx] = g_lut[(g_txtS ? g_txtS : g_txt)[f * 4 + (sy & 1) * 2 + ((sx - from) & 1)]];
			}
		}
	}
	// HD sprites in the doubled top bar and side panel: already at 2x there.
	if (g_btLayer) {
		for (int sy = 0; sy < lh; sy++) {
			DWORD *o = pic + sy * lw;
			int from = sy < 48 ? 0 : mapW;
			for (int sx = from; sx < lw; sx++) {
				int gx = sy < 48 ? g_off - g_extra + sx / 2 : g_off + PANEL_X + (sx - mapW) / 2;
				int gy = sy < 48 ? sy / 2 : 24 + (sy - 48) / 2;
				int f = gy * g_frameW + gx;
				if (gx < 0 || gx >= g_frameW || !g_btValid[f] || copy[gy * pitch + gx] != g_btShadow[f]) continue;
				o[sx] = g_btLayer[f * 4 + (sy & 1) * 2 + ((sx - from) & 1)];
			}
		}
	}
	// Too small for the 2x art (1080p): each HD block averaged into its one pixel.
	BOOL big = (v.bottom - v.top) >= lh * 2;
	if (g_btLayer && !big) {
		for (int sy = 48; sy < lh; sy++) {
			int fy = 24 + sy - 48, f0 = fy * g_frameW + g_off + PANEL_X - mapW;
			const BYTE *row = copy + fy * pitch + g_off + PANEL_X - mapW;
			DWORD *o = pic + sy * lw;
			for (int sx = 0; sx < mapW; sx++) {
				int f = f0 + sx;
				if (!g_btValid[f] || row[sx] != g_btShadow[f]) continue;
				const DWORD *h = g_btLayer + f * 4;
				DWORD rb = (h[0] & 0xff00ff) + (h[1] & 0xff00ff) + (h[2] & 0xff00ff) + (h[3] & 0xff00ff);
				DWORD gg = (h[0] & 0xff00) + (h[1] & 0xff00) + (h[2] & 0xff00) + (h[3] & 0xff00);
				o[sx] = ((rb >> 2) & 0xff00ff) | ((gg >> 2) & 0xff00);
			}
		}
	}
	GetClientRect(g_hwnd, &c);
	HDC dc = GetDC(g_hwnd);
	if (!dc) return FALSE;
	PatBlt(dc, 0, 0, c.right, v.top, BLACKNESS);
	PatBlt(dc, 0, v.bottom, c.right, c.bottom - v.bottom, BLACKNESS);
	PatBlt(dc, 0, v.top, v.left, v.bottom - v.top, BLACKNESS);
	PatBlt(dc, v.right, v.top, c.right - v.right, v.bottom - v.top, BLACKNESS);
	BITMAPINFOHEADER bi = { sizeof bi };
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;
	int vw = v.right - v.left, vh = v.bottom - v.top;
	const DWORD *src = pic;
	int sw = lw, sh = lh;
	if (vh >= lh * 2) {
		for (int y = 0; y < lh; y++) {
			const DWORD *row = pic + y * lw, *up = pic + (y > 0 ? y - 1 : y) * lw, *dn = pic + (y < lh - 1 ? y + 1 : y) * lw;
			DWORD *o0 = pic2 + 2 * y * 2 * lw, *o1 = o0 + 2 * lw;
			for (int x = 0; x < lw; x++) {
				int l = x > 0 ? x - 1 : x, r = x < lw - 1 ? x + 1 : x;
				DWORD p = row[x];
				o0[2 * x] = mix4(p, row[l], up[x], up[l]);
				o0[2 * x + 1] = mix4(p, row[r], up[x], up[r]);
				o1[2 * x] = mix4(p, row[l], dn[x], dn[l]);
				o1[2 * x + 1] = mix4(p, row[r], dn[x], dn[r]);
			}
		}
		if (g_btLayer) {
			for (int sy = 48; sy < lh; sy++) {
				int fy = 24 + sy - 48, f0 = fy * g_frameW + g_off + PANEL_X - mapW;
				const BYTE *row = copy + fy * pitch + g_off + PANEL_X - mapW;
				DWORD *o0 = pic2 + 2 * sy * 2 * lw, *o1 = o0 + 2 * lw;
				for (int sx = 0; sx < mapW; sx++) {
					int f = f0 + sx;
					if (!g_btValid[f] || row[sx] != g_btShadow[f]) continue;
					const DWORD *h = g_btLayer + f * 4;
					o0[2 * sx] = h[0]; o0[2 * sx + 1] = h[1];
					o1[2 * sx] = h[2]; o1[2 * sx + 1] = h[3];
				}
			}
		}
		src = pic2;
		sw = lw * 2;
		sh = lh * 2;
		SetStretchBltMode(dc, COLORONCOLOR);
	}
	else {
		SetStretchBltMode(dc, HALFTONE);
		SetBrushOrgEx(dc, 0, 0, NULL);
	}
	bi.biWidth = sw;
	bi.biHeight = -sh;
	shot_save(src, sw, sh);
	StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, sw, sh, src, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(g_hwnd, dc);
	return TRUE;
}

// Remote mode's screenshot: the picture about to be shown (BGRX, w x h, top-down) as a BMP.
static void shot_save(const DWORD *px, int w, int h)
{
	if (!InterlockedExchange(&g_shotWanted, 0)) return;
	char path[MAX_PATH], tmp[MAX_PATH];
	snprintf(path, sizeof path, "%s\\lords2hd-shot.bmp", g_gameDir);
	snprintf(tmp, sizeof tmp, "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return;
	BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + w * h * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
	BITMAPINFOHEADER ih = { sizeof ih, w, -h, 1, 32, BI_RGB };
	fwrite(&fh, sizeof fh, 1, f);
	fwrite(&ih, sizeof ih, 1, f);
	fwrite(px, 4, (size_t)w * h, f);
	fclose(f);
	MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

// Returns FALSE when nothing changed since the last present: the game copies its whole frame to
// the screen on every pass of its loop, paused or not.
static BOOL present(void)
{
	if (!g_hwnd || !g_primary) return FALSE;
	static PALETTEENTRY lastPal[256];
	static DWORD palChanged;
	static BOOL palDumped = TRUE;
	static RECT lastView;

	// The screen ids, logged when they change, to tell the county map from everything else.
	static int lastScreen = -1, lastMode = -1;
	int screen = *(BYTE *)0x004eac50, mode = *(int *)0x0052af94;
	if (screen != lastScreen || mode != lastMode) {
		logf_("screen %d, mode %d", screen, mode);
		lastScreen = screen;
		lastMode = mode;
	}
	// The county map (mode 3; screen 0, or 39 with a message scroll on it) uses the whole frame.
	InterlockedExchange(&g_wideScreen, g_off && county_map_on());

	EnterCriticalSection(&g_lock);
	g_showX = g_wideScreen ? g_off - g_extra : g_off;
	g_showW = g_wideScreen ? GAME_W + g_extra : GAME_W;
	int sx = g_showX, sw = g_showW;
	// A wider room takes the frame, built from the game's 640 columns.
	BOOL room = g_room.on && g_hdPic.on && !g_wideScreen && g_off && g_scale == SCALE_SMOOTH;
	if (room && !g_hdPic.seen) {
		// The painting's first frame on screen goes wide at once (waiting for hd_overlay to have
		// seen it showed one narrow frame: a flicker on every painted screen).
		int same = 0;
		for (int y = 0; y < GAME_H; y += 4) {
			const BYTE *fr = g_primary->bits + y * g_primary->pitch + g_off, *sh = g_hdPic.shadow + y * GAME_W;
			for (int x = 0; x < GAME_W; x += 4) same += fr[x] == sh[x];
		}
		room = same >= (GAME_W / 4) * (GAME_H / 4) / 8;
	}
	int roomW = 0;
	if (room) {
		g_room.extra = min(g_extra, g_room.w - GAME_W);
		roomW = (GAME_W + g_room.extra) & ~1;
		g_room.extra = roomW - GAME_W;
		g_showX = g_off - g_room.extra;
		g_showW = roomW;
	}
	InterlockedExchange(&g_roomShown, room);
	if (sx + sw > g_primary->w) sx = 0, sw = min(sw, g_primary->w);
	for (int y = 0; y < GAME_H; y++) memcpy(g_frame8 + y * sw, g_primary->bits + y * g_primary->pitch + sx, sw);
	PALETTEENTRY pal[256];
	if (g_primary->pal) memcpy(pal, g_primary->pal->e, sizeof pal);
	else memset(pal, 0, sizeof pal);
	LeaveCriticalSection(&g_lock);

	BOOL palSame = memcmp(pal, lastPal, sizeof pal) == 0;
	if (!palSame) {
		memcpy(lastPal, pal, sizeof pal);
		palChanged = GetTickCount();
		palDumped = FALSE;
	}
	if (g_dumpPalettes && !palDumped && GetTickCount() - palChanged > 500) {
		dump_palette_if_new(pal);
		palDumped = TRUE;
	}

	if (battle_zoom_on()) return present_battle(pal, palSame);

	RECT v, c;
	view_rect(&v);
	BOOL forced = InterlockedExchange(&g_forcePresent, 0) || !EqualRect(&v, &lastView) || g_waveActive;
	if (!forced && palSame && memcmp(g_frame8, g_shown8, sw * GAME_H) == 0) return FALSE;
	memcpy(g_shown8, g_frame8, sw * GAME_H);
	lastView = v;
	GetClientRect(g_hwnd, &c);
	HDC dc = GetDC(g_hwnd);
	if (!dc) return FALSE;
	// Black around the picture.
	PatBlt(dc, 0, 0, c.right, v.top, BLACKNESS);
	PatBlt(dc, 0, v.bottom, c.right, c.bottom - v.bottom, BLACKNESS);
	PatBlt(dc, 0, v.top, v.left, v.bottom - v.top, BLACKNESS);
	PatBlt(dc, v.right, v.top, c.right - v.right, v.bottom - v.top, BLACKNESS);

	struct { BITMAPINFOHEADER h; RGBQUAD colours[256]; } bi = { { sizeof(BITMAPINFOHEADER) } };
	bi.h.biPlanes = 1;
	bi.h.biCompression = BI_RGB;
	SetStretchBltMode(dc, COLORONCOLOR);
	int vw = v.right - v.left, vh = v.bottom - v.top;
	if (g_scale == SCALE_SMOOTH) {
		for (int i = 0; i < 256; i++) g_lut[i] = (pal[i].peRed << 16) | (pal[i].peGreen << 8) | pal[i].peBlue;
		for (int i = 0; i < sw * GAME_H; i++) g_frame32[i] = g_lut[g_frame8[i]];
		double_frame(sw);
		if (sw == GAME_W) hd_overlay(pal);
		screen_sprites(pal, sx, sw);
		tile_overlay(pal, sx, sw);
		text_overlay(sx, sw);
		EnterCriticalSection(&g_lock);
		room = room && g_room.on && g_hdPic.on;
		if (room) room_compose(pal, roomW);
		LeaveCriticalSection(&g_lock);
		// Below 4x, as at 1080p (2.25x), a plain stretch of the 2x frame drops rows and columns
		// unevenly: use GDI's smooth mode, affordable at those sizes. Larger stays fast.
		if (vh < GAME_H * 4 && vh != GAME_H * 2) {
			SetStretchBltMode(dc, HALFTONE);
			SetBrushOrgEx(dc, 0, 0, NULL);
		}
		int dw = room ? roomW * 2 : sw * 2;
		bi.h.biWidth = dw;
		bi.h.biHeight = -GAME_H * 2;
		bi.h.biBitCount = 32;
		shot_save(room ? g_room.out : g_frame2x, dw, GAME_H * 2);
		StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, dw, GAME_H * 2, room ? g_room.out : g_frame2x,
			(BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
	else {
		for (int i = 0; i < 256; i++) {
			bi.colours[i].rgbRed = pal[i].peRed;
			bi.colours[i].rgbGreen = pal[i].peGreen;
			bi.colours[i].rgbBlue = pal[i].peBlue;
		}
		bi.h.biWidth = sw; // a multiple of 4, so DIB rows need no padding
		bi.h.biHeight = -GAME_H;
		bi.h.biBitCount = 8;
		bi.h.biClrUsed = 256;
		StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, sw, GAME_H, g_frame8,
			(BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
	ReleaseDC(g_hwnd, dc);
	return TRUE;
}

// Presents whenever the frame changed, at most once per display refresh: the game copies its
// frame in pieces (one start-up effect uses 1200 16x16 blocks), and each copy only marks it dirty.
static DWORD WINAPI presenter(void *unused)
{
	HRESULT (WINAPI *dwm_flush)(void) = NULL;
	HMODULE dwm = LoadLibraryA("dwmapi.dll");
	if (dwm) dwm_flush = (void *)GetProcAddress(dwm, "DwmFlush");
	LARGE_INTEGER freq, t0, t1, since;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&since);
	double busy = 0, worst = 0;
	int presents = 0, skipped = 0;

	for (;;) {
		WaitForSingleObject(g_dirtyEvent, 50);
		BOOL dirty = InterlockedExchange(&g_dirty, 0) != 0;
		if (!dirty && !g_dumpPalettes && !g_waveActive) continue; // DumpPalettes: lets a palette that stays put be logged
		QueryPerformanceCounter(&t0);
		BOOL shown = present();
		QueryPerformanceCounter(&t1);
		if (shown) {
			double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
			busy += ms;
			if (ms > worst) worst = ms;
			presents++;
			// Wait for the compositor's next frame, so the picture changes at most once per refresh.
			if (!dwm_flush || FAILED(dwm_flush())) Sleep(8);
		}
		else if (dirty) {
			skipped++;
		}
		if (g_timing && (t1.QuadPart - since.QuadPart) > 5 * freq.QuadPart) {
			logf_("timing: %d presents in 5 s, %.1f ms average, %.1f ms worst; %d unchanged frames skipped",
				presents, presents ? busy / presents : 0.0, worst, skipped);
			for (int i = 0; i < 2; i++) if (g_draw[i].n) {
				logf_("timing: %d %s redraws, %.1f ms average, %.1f ms worst", g_draw[i].n, i ? "battlefield" : "map", g_draw[i].ms / g_draw[i].n, g_draw[i].worst);
				g_draw[i].n = 0; g_draw[i].ms = g_draw[i].worst = 0;
			}
			since = t1;
			busy = worst = 0;
			presents = skipped = 0;
		}
	}
	return 0;
}

static void start_presenter(void)
{
	static BOOL started;
	if (started) return;
	started = TRUE;
	g_dirtyEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
	HANDLE t = CreateThread(NULL, 0, presenter, NULL, 0, NULL);
	if (t) CloseHandle(t);
}

// ---------------------------------------------------------------------------------------------
// The emulated DirectDraw objects. Only what the game calls does anything; every other method
// logs its name, so a call we did not expect shows up in the log.

#define SELF(T) T *self = (T *)this_
static void unexpected(const char *what)
{
	static int count;
	if (count++ < 50) logf_("unexpected DirectDraw call: %s", what);
}
#define UNSUPPORTED(name) { unexpected(name); return DDERR_UNSUPPORTED; }

static HRESULT WINAPI no_interface(void *this_, REFIID riid, void **out)
{
	unexpected("QueryInterface");
	if (out) *out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI add_ref(void *this_)
{
	return InterlockedIncrement(&((Palette *)this_)->refs); // refs follows vtbl in every object
}

// Palette

static ULONG WINAPI pal_release(void *this_)
{
	SELF(Palette);
	LONG n = InterlockedDecrement(&self->refs);
	if (n == 0) {
		EnterCriticalSection(&g_lock);
		if (g_primary && g_primary->pal == self) g_primary->pal = NULL;
		LeaveCriticalSection(&g_lock);
		free(self);
	}
	return n;
}
static HRESULT WINAPI pal_get_caps(void *this_, DWORD *caps)
{
	if (caps) *caps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
	return DD_OK;
}
static HRESULT WINAPI pal_get_entries(void *this_, DWORD flags, DWORD start, DWORD count, PALETTEENTRY *e)
{
	SELF(Palette);
	if (start > 256 || count > 256 - start || !e) return DDERR_INVALIDPARAMS;
	EnterCriticalSection(&g_lock);
	memcpy(e, self->e + start, count * sizeof *e);
	LeaveCriticalSection(&g_lock);
	return DD_OK;
}
static HRESULT WINAPI pal_initialize(void *this_, void *dd, DWORD flags, PALETTEENTRY *e) UNSUPPORTED("Palette::Initialize")
static HRESULT WINAPI pal_set_entries(void *this_, DWORD flags, DWORD start, DWORD count, PALETTEENTRY *e)
{
	SELF(Palette);
	if (start > 256 || count > 256 - start || !e) return DDERR_INVALIDPARAMS;
	EnterCriticalSection(&g_lock);
	memcpy(self->e + start, e, count * sizeof *e);
	BOOL onScreen = g_primary && g_primary->pal == self;
	LeaveCriticalSection(&g_lock);
	if (onScreen) mark_dirty();
	return DD_OK;
}
static void *vt_palette[] = { no_interface, add_ref, pal_release, pal_get_caps, pal_get_entries,
	pal_initialize, pal_set_entries };

// Clipper: the game attaches one to its window and never asks it anything.

typedef struct { void **vtbl; LONG refs; HWND hwnd; } Clipper;
static ULONG WINAPI clip_release(void *this_)
{
	SELF(Clipper);
	LONG n = InterlockedDecrement(&self->refs);
	if (n == 0) free(self);
	return n;
}
static HRESULT WINAPI clip_get_clip_list(void *this_, RECT *r, RGNDATA *d, DWORD *size) UNSUPPORTED("Clipper::GetClipList")
static HRESULT WINAPI clip_get_hwnd(void *this_, HWND *h)
{
	SELF(Clipper);
	if (h) *h = self->hwnd;
	return DD_OK;
}
static HRESULT WINAPI clip_initialize(void *this_, void *dd, DWORD flags) { return DD_OK; }
static HRESULT WINAPI clip_is_changed(void *this_, BOOL *changed)
{
	if (changed) *changed = FALSE;
	return DD_OK;
}
static HRESULT WINAPI clip_set_clip_list(void *this_, RGNDATA *d, DWORD flags) { return DD_OK; }
static HRESULT WINAPI clip_set_hwnd(void *this_, DWORD flags, HWND h)
{
	SELF(Clipper);
	self->hwnd = h;
	return DD_OK;
}
static void *vt_clipper[] = { no_interface, add_ref, clip_release, clip_get_clip_list, clip_get_hwnd,
	clip_initialize, clip_is_changed, clip_set_clip_list, clip_set_hwnd };

// Surface

static ULONG WINAPI surf_release(void *this_)
{
	SELF(Surface);
	LONG n = InterlockedDecrement(&self->refs);
	if (n == 0) {
		EnterCriticalSection(&g_lock);
		if (g_primary == self) g_primary = NULL;
		LeaveCriticalSection(&g_lock);
		if (self->pal) pal_release(self->pal);
		free(self->bits);
		free(self);
	}
	return n;
}

static void fill_pixel_format(DDPIXELFORMAT *pf)
{
	memset(pf, 0, sizeof *pf);
	pf->dwSize = sizeof *pf;
	pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
	pf->dwRGBBitCount = 8;
}

static void fill_desc(Surface *s, DDSURFACEDESC *d, BYTE *bits)
{
	memset(d, 0, sizeof *d);
	d->dwSize = sizeof *d;
	d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (bits ? DDSD_LPSURFACE : 0);
	d->dwWidth = s->w;
	d->dwHeight = s->h;
	d->lPitch = s->pitch;
	d->lpSurface = bits;
	fill_pixel_format(&d->ddpfPixelFormat);
	d->ddsCaps.dwCaps = s->caps;
}

// Clips a copy of w x h pixels from (sx, sy) to (dx, dy) against both surfaces (surface pixels,
// not game coordinates).
static BOOL clip_copy(Surface *dst, Surface *src, int *dx, int *dy, int *sx, int *sy, int *w, int *h)
{
	if (*dx < 0) { *sx -= *dx; *w += *dx; *dx = 0; }
	if (*dy < 0) { *sy -= *dy; *h += *dy; *dy = 0; }
	if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
	if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
	if (*dx + *w > dst->w) *w = dst->w - *dx;
	if (*dy + *h > dst->h) *h = dst->h - *dy;
	if (*sx + *w > src->w) *w = src->w - *sx;
	if (*sy + *h > src->h) *h = src->h - *sy;
	return *w > 0 && *h > 0;
}

// Coordinates are the game's. On a wide screen a copy from the frame that starts at the game's
// left edge also takes the spare columns to its left, where the wider map was drawn: the game
// only knows about its own 640.
static void copy_rect(Surface *dst, int dx, int dy, Surface *src, int sx, int sy, int w, int h)
{
	dx += dst->org;
	sx += src->org;
	// Battles redraw units in small rectangles and the game clamps those to its x = 0, so on a wide
	// screen the spare columns of the rows being copied always go along.
	if (g_wideScreen && dst->org && src->org && dx >= dst->org && sx >= src->org && dx - dst->org == sx - src->org) {
		int y0 = max(0, dy), y1 = min(min(dst->h, src->h), dy + h);
		for (int y = y0; y < y1; y++) memcpy(dst->bits + y * dst->pitch, src->bits + y * src->pitch, dst->org);
	}
	if (!clip_copy(dst, src, &dx, &dy, &sx, &sy, &w, &h)) return;
	for (int y = 0; y < h; y++)
		memmove(dst->bits + (dy + y) * dst->pitch + dx, src->bits + (sy + y) * src->pitch + sx, w);
	if (dst->primary && src == g_back && dy == sy) text_publish(dy, dy + h);
}

static HRESULT WINAPI surf_blt(void *this_, RECT *dr, void *src_, RECT *sr, DWORD flags, DDBLTFX *fx)
{
	SELF(Surface);
	Surface *src = src_;
	RECT d = dr ? *dr : (RECT){ -self->org, 0, self->w - self->org, self->h }; // no rectangle: the whole surface
	if (self->primary) EnterCriticalSection(&g_lock);
	if (flags & DDBLT_COLORFILL) {
		BYTE colour = fx ? (BYTE)fx->dwFillColor : 0;
		OffsetRect(&d, self->org, 0);
		int x0 = max(0, d.left), x1 = min(self->w, d.right), y0 = max(0, d.top), y1 = min(self->h, d.bottom);
		for (int y = y0; y < y1; y++) memset(self->bits + y * self->pitch + x0, colour, max(0, x1 - x0));
	}
	else if (src) {
		RECT s = sr ? *sr : (RECT){ -src->org, 0, src->w - src->org, src->h };
		int w = d.right - d.left, h = d.bottom - d.top;
		if (w == s.right - s.left && h == s.bottom - s.top) {
			copy_rect(self, d.left, d.top, src, s.left, s.top, w, h);
		}
		else {
			// Never seen from the game; nearest-pixel stretch so it still shows something.
			static BOOL said;
			if (!said) { said = TRUE; logf_("stretching Blt %ldx%ld -> %dx%d", s.right - s.left, s.bottom - s.top, w, h); }
			OffsetRect(&d, self->org, 0);
			OffsetRect(&s, src->org, 0);
			for (int y = max(0, d.top); y < min(self->h, d.bottom); y++)
				for (int x = max(0, d.left); x < min(self->w, d.right); x++) {
					int u = s.left + (x - d.left) * (s.right - s.left) / w;
					int v = s.top + (y - d.top) * (s.bottom - s.top) / h;
					if (u >= 0 && v >= 0 && u < src->w && v < src->h)
						self->bits[y * self->pitch + x] = src->bits[v * src->pitch + u];
				}
		}
	}
	if (self->primary) {
		LeaveCriticalSection(&g_lock);
		mark_dirty();
	}
	return DD_OK;
}

static HRESULT WINAPI surf_blt_fast(void *this_, DWORD x, DWORD y, void *src_, RECT *sr, DWORD trans)
{
	SELF(Surface);
	Surface *src = src_;
	if (!src) return DDERR_INVALIDPARAMS;
	if (trans & (DDBLTFAST_SRCCOLORKEY | DDBLTFAST_DESTCOLORKEY)) unexpected("BltFast with a colour key");
	RECT s = sr ? *sr : (RECT){ -src->org, 0, src->w - src->org, src->h };
	if (self->primary) EnterCriticalSection(&g_lock);
	copy_rect(self, (int)x, (int)y, src, s.left, s.top, s.right - s.left, s.bottom - s.top);
	if (self->primary) {
		LeaveCriticalSection(&g_lock);
		mark_dirty();
	}
	return DD_OK;
}

static HRESULT WINAPI surf_lock(void *this_, RECT *r, DDSURFACEDESC *d, DWORD flags, HANDLE ev)
{
	SELF(Surface);
	if (!d) return DDERR_INVALIDPARAMS;
	BYTE *p = self->bits + self->org; // the game keeps this pointer as its frame (DAT_004ea1a8)
	if (r) p += r->top * self->pitch + r->left;
	fill_desc(self, d, p);
	return DD_OK;
}

static HRESULT WINAPI surf_unlock(void *this_, void *p)
{
	SELF(Surface);
	if (self->primary) mark_dirty();
	return DD_OK;
}

static HRESULT WINAPI surf_get_surface_desc(void *this_, DDSURFACEDESC *d)
{
	SELF(Surface);
	if (!d) return DDERR_INVALIDPARAMS;
	fill_desc(self, d, NULL);
	return DD_OK;
}

static HRESULT WINAPI surf_get_pixel_format(void *this_, DDPIXELFORMAT *pf)
{
	if (!pf) return DDERR_INVALIDPARAMS;
	fill_pixel_format(pf);
	return DD_OK;
}

static HRESULT WINAPI surf_get_caps(void *this_, DDSCAPS *caps)
{
	SELF(Surface);
	if (caps) caps->dwCaps = self->caps;
	return DD_OK;
}

static HRESULT WINAPI surf_set_palette(void *this_, void *pal_)
{
	SELF(Surface);
	Palette *pal = pal_;
	if (pal) add_ref(pal);
	EnterCriticalSection(&g_lock);
	Palette *old = self->pal;
	self->pal = pal;
	LeaveCriticalSection(&g_lock);
	if (old) pal_release(old);
	if (self->primary) mark_dirty();
	return DD_OK;
}

static HRESULT WINAPI surf_get_palette(void *this_, void **out)
{
	SELF(Surface);
	if (!out) return DDERR_INVALIDPARAMS;
	*out = self->pal;
	if (!self->pal) return DDERR_NOPALETTEATTACHED;
	add_ref(self->pal);
	return DD_OK;
}

static HRESULT WINAPI surf_ok0(void *this_) { return DD_OK; }                   // IsLost, Restore
static HRESULT WINAPI surf_ok1(void *this_, void *a) { return DD_OK; }          // SetClipper, GetBltStatus, GetFlipStatus
static HRESULT WINAPI surf_flip(void *this_, void *target, DWORD flags)
{
	mark_dirty();
	return DD_OK;
}
static HRESULT WINAPI surf_set_color_key(void *this_, DWORD flags, DDCOLORKEY *key) { return DD_OK; }

static HRESULT WINAPI surf_add_attached(void *this_, void *s) UNSUPPORTED("Surface::AddAttachedSurface")
static HRESULT WINAPI surf_add_overlay_dirty(void *this_, RECT *r) UNSUPPORTED("Surface::AddOverlayDirtyRect")
static HRESULT WINAPI surf_blt_batch(void *this_, void *b, DWORD n, DWORD f) UNSUPPORTED("Surface::BltBatch")
static HRESULT WINAPI surf_delete_attached(void *this_, DWORD f, void *s) UNSUPPORTED("Surface::DeleteAttachedSurface")
static HRESULT WINAPI surf_enum_attached(void *this_, void *c, void *cb) UNSUPPORTED("Surface::EnumAttachedSurfaces")
static HRESULT WINAPI surf_enum_overlay(void *this_, DWORD f, void *c, void *cb) UNSUPPORTED("Surface::EnumOverlayZOrders")
static HRESULT WINAPI surf_get_attached(void *this_, DDSCAPS *caps, void **s)
{
	unexpected("Surface::GetAttachedSurface");
	if (s) *s = NULL;
	return DDERR_NOTFOUND;
}
static HRESULT WINAPI surf_get_clipper(void *this_, void **c) UNSUPPORTED("Surface::GetClipper")
static HRESULT WINAPI surf_get_color_key(void *this_, DWORD f, DDCOLORKEY *k) UNSUPPORTED("Surface::GetColorKey")
static HRESULT WINAPI surf_get_dc(void *this_, HDC *dc) UNSUPPORTED("Surface::GetDC")
static HRESULT WINAPI surf_get_overlay_pos(void *this_, LONG *x, LONG *y) UNSUPPORTED("Surface::GetOverlayPosition")
static HRESULT WINAPI surf_initialize(void *this_, void *dd, DDSURFACEDESC *d) UNSUPPORTED("Surface::Initialize")
static HRESULT WINAPI surf_release_dc(void *this_, HDC dc) UNSUPPORTED("Surface::ReleaseDC")
static HRESULT WINAPI surf_set_overlay_pos(void *this_, LONG x, LONG y) UNSUPPORTED("Surface::SetOverlayPosition")
static HRESULT WINAPI surf_update_overlay(void *this_, RECT *a, void *s, RECT *b, DWORD f, void *fx) UNSUPPORTED("Surface::UpdateOverlay")
static HRESULT WINAPI surf_update_overlay_display(void *this_, DWORD f) UNSUPPORTED("Surface::UpdateOverlayDisplay")
static HRESULT WINAPI surf_update_overlay_z(void *this_, DWORD f, void *s) UNSUPPORTED("Surface::UpdateOverlayZOrder")

static void *vt_surface[] = {
	no_interface, add_ref, surf_release, surf_add_attached, surf_add_overlay_dirty, surf_blt,
	surf_blt_batch, surf_blt_fast, surf_delete_attached, surf_enum_attached, surf_enum_overlay,
	surf_flip, surf_get_attached, surf_ok1 /* GetBltStatus */, surf_get_caps, surf_get_clipper,
	surf_get_color_key, surf_get_dc, surf_ok1 /* GetFlipStatus */, surf_get_overlay_pos,
	surf_get_palette, surf_get_pixel_format, surf_get_surface_desc, surf_initialize,
	surf_ok0 /* IsLost */, surf_lock, surf_release_dc, surf_ok0 /* Restore */,
	surf_ok1 /* SetClipper */, surf_set_color_key, surf_set_overlay_pos, surf_set_palette,
	surf_unlock, surf_update_overlay, surf_update_overlay_display, surf_update_overlay_z,
};

// DirectDraw

typedef struct { void **vtbl; LONG refs; } Dd;

static ULONG WINAPI dd_release(void *this_)
{
	SELF(Dd);
	LONG n = InterlockedDecrement(&self->refs);
	if (n == 0) free(self);
	return n;
}

static HRESULT WINAPI dd_create_clipper(void *this_, DWORD flags, void **out, IUnknown *outer)
{
	Clipper *c = calloc(1, sizeof *c);
	if (!c || !out) return DDERR_OUTOFMEMORY;
	c->vtbl = vt_clipper;
	c->refs = 1;
	*out = c;
	return DD_OK;
}

static HRESULT WINAPI dd_create_palette(void *this_, DWORD flags, PALETTEENTRY *e, void **out, IUnknown *outer)
{
	Palette *p = calloc(1, sizeof *p);
	if (!p || !out) return DDERR_OUTOFMEMORY;
	p->vtbl = vt_palette;
	p->refs = 1;
	if (e) memcpy(p->e, e, sizeof p->e);
	*out = p;
	return DD_OK;
}

static HRESULT WINAPI dd_create_surface(void *this_, DDSURFACEDESC *d, void **out, IUnknown *outer)
{
	if (!d || !out) return DDERR_INVALIDPARAMS;
	Surface *s = calloc(1, sizeof *s);
	if (!s) return DDERR_OUTOFMEMORY;
	s->vtbl = vt_surface;
	s->refs = 1;
	s->caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : DDSCAPS_OFFSCREENPLAIN;
	s->primary = (s->caps & DDSCAPS_PRIMARYSURFACE) != 0;
	s->w = s->primary || !(d->dwFlags & DDSD_WIDTH) ? g_frameW : (int)d->dwWidth;
	s->h = s->primary || !(d->dwFlags & DDSD_HEIGHT) ? g_frameH : (int)d->dwHeight;
	s->pitch = (s->w + 3) & ~3;
	// One spare row: the game's whole-frame loops start at its x = 0, g_off bytes into the frame.
	s->bits = calloc(1, s->pitch * (s->h + 1));
	if (!s->bits) { free(s); return DDERR_OUTOFMEMORY; }
	if (s->w == g_frameW && s->h == g_frameH) s->org = g_off;
	if (d->dwFlags & DDSD_BACKBUFFERCOUNT) unexpected("CreateSurface with back buffers");
	logf_("surface %p: %dx%d caps %08lx, game x = 0 at %d%s", (void *)s, s->w, s->h, (unsigned long)s->caps, s->org,
		s->primary ? " (primary)" : "");
	if (s->primary) {
		EnterCriticalSection(&g_lock);
		g_primary = s;
		LeaveCriticalSection(&g_lock);
		start_presenter();
	}
	else if (s->w == g_frameW && s->h == g_frameH && (s->caps & DDSCAPS_SYSTEMMEMORY)) {
		g_back = s;
	}
	*out = s;
	return DD_OK;
}

static HRESULT WINAPI dd_ok0(void *this_) { return DD_OK; } // Compact, FlipToGDISurface, RestoreDisplayMode
static HRESULT WINAPI dd_duplicate(void *this_, void *s, void **out) UNSUPPORTED("DirectDraw::DuplicateSurface")
static HRESULT WINAPI dd_enum_modes(void *this_, DWORD f, DDSURFACEDESC *d, void *c, void *cb) UNSUPPORTED("DirectDraw::EnumDisplayModes")
static HRESULT WINAPI dd_enum_surfaces(void *this_, DWORD f, DDSURFACEDESC *d, void *c, void *cb) UNSUPPORTED("DirectDraw::EnumSurfaces")
static HRESULT WINAPI dd_get_caps(void *this_, DDCAPS *drv, DDCAPS *hel)
{
	if (drv) { DWORD n = drv->dwSize; memset(drv, 0, n); drv->dwSize = n; drv->dwPalCaps = DDPCAPS_8BIT; }
	if (hel) { DWORD n = hel->dwSize; memset(hel, 0, n); hel->dwSize = n; hel->dwPalCaps = DDPCAPS_8BIT; }
	return DD_OK;
}
static HRESULT WINAPI dd_get_display_mode(void *this_, DDSURFACEDESC *d)
{
	if (!d) return DDERR_INVALIDPARAMS;
	memset(d, 0, sizeof *d);
	d->dwSize = sizeof *d;
	d->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
	d->dwWidth = GAME_W;
	d->dwHeight = GAME_H;
	d->lPitch = GAME_W;
	fill_pixel_format(&d->ddpfPixelFormat);
	return DD_OK;
}
static HRESULT WINAPI dd_get_fourcc(void *this_, DWORD *n, DWORD *codes)
{
	if (n) *n = 0;
	return DD_OK;
}
static HRESULT WINAPI dd_get_gdi_surface(void *this_, void **s) UNSUPPORTED("DirectDraw::GetGDISurface")
static HRESULT WINAPI dd_get_monitor_freq(void *this_, DWORD *f)
{
	if (f) *f = 60;
	return DD_OK;
}
static HRESULT WINAPI dd_get_scanline(void *this_, DWORD *l) UNSUPPORTED("DirectDraw::GetScanLine")
static HRESULT WINAPI dd_get_vblank_status(void *this_, BOOL *b)
{
	if (b) *b = TRUE;
	return DD_OK;
}
static HRESULT WINAPI dd_initialize(void *this_, GUID *g) { return DD_OK; }

static void subclass_window(HWND hwnd);

static HRESULT WINAPI dd_set_cooperative_level(void *this_, HWND hwnd, DWORD flags)
{
	logf_("SetCooperativeLevel(%p, %08lx)", (void *)hwnd, (unsigned long)flags);
	if (hwnd) subclass_window(hwnd);
	return DD_OK;
}

// The mode switch: the monitor stays as it is. The game's window becomes either a borderless
// window over the whole monitor or, with Fullscreen=0, a normal window WindowScale times 640x480.
static HRESULT WINAPI dd_set_display_mode(void *this_, DWORD w, DWORD h, DWORD bpp)
{
	logf_("SetDisplayMode(%lu, %lu, %lu): keeping the desktop mode", (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
	if (!g_hwnd) return DD_OK;
	MONITORINFO mi = { sizeof mi };
	GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
	if (g_fullscreen) {
		SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_SYSMENU);
		SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | (g_remote ? SWP_NOACTIVATE : 0));
	}
	else {
		DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
		RECT r = { 0, 0, GAME_W * g_windowScale, GAME_H * g_windowScale };
		int fixedH = GetPrivateProfileIntA("Display", "WindowHeight", 0, g_iniPath); // e.g. 1080, to try that size
		if (fixedH > 0) SetRect(&r, 0, 0, MulDiv(GAME_W + g_extra, fixedH, GAME_H), fixedH);
		AdjustWindowRect(&r, style, FALSE);
		int ww = r.right - r.left, wh = r.bottom - r.top;
		int x = mi.rcWork.left + max(0, (int)(mi.rcWork.right - mi.rcWork.left - ww) / 2);
		int y = mi.rcWork.top + max(0, (int)(mi.rcWork.bottom - mi.rcWork.top - wh) / 2);
		SetWindowLongA(g_hwnd, GWL_STYLE, style);
		SetWindowLongA(g_hwnd, GWL_EXSTYLE, GetWindowLongA(g_hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
		SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW | (g_remote ? SWP_NOACTIVATE : 0));
	}
	RECT v;
	view_rect(&v);
	logf_("picture at %ld,%ld %ldx%ld", v.left, v.top, v.right - v.left, v.bottom - v.top);
	mark_dirty();
	return DD_OK;
}

static HRESULT WINAPI dd_wait_vblank(void *this_, DWORD flags, HANDLE ev) { return DD_OK; }

static void *vt_dd[] = {
	no_interface, add_ref, dd_release, dd_ok0 /* Compact */, dd_create_clipper, dd_create_palette,
	dd_create_surface, dd_duplicate, dd_enum_modes, dd_enum_surfaces, dd_ok0 /* FlipToGDISurface */,
	dd_get_caps, dd_get_display_mode, dd_get_fourcc, dd_get_gdi_surface, dd_get_monitor_freq,
	dd_get_scanline, dd_get_vblank_status, dd_initialize, dd_ok0 /* RestoreDisplayMode */,
	dd_set_cooperative_level, dd_set_display_mode, dd_wait_vblank,
};

typedef HRESULT (WINAPI *DirectDrawCreate_t)(GUID *, void **, IUnknown *);

HRESULT WINAPI hd_DirectDrawCreate(GUID *guid, void **out, IUnknown *outer)
{
	if (!g_emulate) return ((DirectDrawCreate_t)real_DirectDrawCreate)(guid, out, outer);
	if (!out) return DDERR_INVALIDPARAMS;
	Dd *dd = calloc(1, sizeof *dd);
	if (!dd) return DDERR_OUTOFMEMORY;
	dd->vtbl = vt_dd;
	dd->refs = 1;
	*out = dd;
	logf_("DirectDrawCreate: emulated");
	return DD_OK;
}

// ---------------------------------------------------------------------------------------------
// Input: the game sees a 640x480 window at the top-left of the screen.

static void client_to_game(int *x, int *y)
{
	RECT v;
	view_rect(&v);
	int vw = max(1, v.right - v.left), vh = max(1, v.bottom - v.top);
	if (battle_zoom_on()) {
		// The zoomed-out battle's picture: top bar and side panel doubled, battlefield 1:1.
		int lw = 2 * (GAME_W + g_extra), lh = 2 * GAME_H, mapW = battle_map_w();
		int sx = max(0, min(lw - 1, MulDiv(*x - v.left, lw, vw))), sy = max(0, min(lh - 1, MulDiv(*y - v.top, lh, vh)));
		if (sy < 48) { *x = sx / 2 - g_extra; *y = sy / 2; }
		else if (sx >= mapW) { *x = PANEL_X + (sx - mapW) / 2; *y = 24 + (sy - 48) / 2; }
		else { *x = PANEL_X - mapW + sx; *y = 24 + sy - 48; }
		return;
	}
	int gx = MulDiv(*x - v.left, g_showW, vw), gy = MulDiv(*y - v.top, GAME_H, vh);
	if (g_roomShown) {
		// New wall answers as the nearest column the game knows, and has nothing on it to click.
		int art, row = max(0, min(GAME_H - 1, gy)), cx = max(0, min(g_showW - 1, gx));
		int g = room_col(cx, row, &art);
		if (g < 0) g = cx < g_showW / 2 ? 0 : GAME_W - 1; // beyond the game's columns: the nearest one
		*x = g;
		*y = row;
		return;
	}
	*x = g_showX + max(0, min(g_showW - 1, gx)) - g_off; // negative over the spare columns
	*y = max(0, min(GAME_H - 1, gy));
}

// Remote mode: the driver's messages become the game's mouse and keys, in game coordinates.
static int g_fakeEdge;
static LRESULT remote_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	int n = msg - WM_REMOTE;
	if (n == 0x10) {
		InterlockedExchange(&g_shotWanted, 1);
		InterlockedExchange(&g_forcePresent, 1);
		mark_dirty();
		return 0;
	}
	if (n == 7) { // hold the mouse at screen edges: 1 left, 2 right, 4 top, 8 bottom, 0 = let go
		g_fakeEdge = (int)wp;
		return 0;
	}
	if (n == 4) {
		CallWindowProcA(g_gameProc, hwnd, WM_KEYDOWN, wp, 1);
		return CallWindowProcA(g_gameProc, hwnd, WM_KEYUP, wp, 0xC0000001);
	}
	static const UINT mouse[] = { 0, WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, 0, WM_RBUTTONDOWN, WM_RBUTTONUP };
	static WPARAM buttons;
	if (n < 1 || n > 6 || !mouse[n]) return 0;
	g_fake.x = (short)LOWORD(lp);
	g_fake.y = (short)HIWORD(lp);
	if (n == 2) buttons |= MK_LBUTTON; else if (n == 3) buttons &= ~MK_LBUTTON;
	if (n == 5) buttons |= MK_RBUTTON; else if (n == 6) buttons &= ~MK_RBUTTON;
	return CallWindowProcA(g_gameProc, hwnd, mouse[n], buttons, lp);
}

static void remote_hide(HWND hwnd)
{
	HMODULE dwm = LoadLibraryA("dwmapi.dll");
	HRESULT (WINAPI *set_attr)(HWND, DWORD, const void *, DWORD) = dwm ? (void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
	BOOL cloak = TRUE;
	if (set_attr) set_attr(hwnd, 13 /* DWMWA_CLOAK */, &cloak, sizeof cloak);
	SetWindowLongA(hwnd, GWL_EXSTYLE, GetWindowLongA(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
	SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%s\\lords2hd-remote.txt", g_gameDir);
	FILE *f = fopen(path, "w");
	if (f) { fprintf(f, "%lu %lu\n", (unsigned long)(ULONG_PTR)hwnd, GetCurrentProcessId()); fclose(f); }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (g_remote) {
		if (msg > WM_REMOTE && msg <= WM_REMOTE + 0x10) return remote_message(hwnd, msg, wp, lp);
		if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
		if (msg == WM_ACTIVATEAPP) wp = TRUE;            // the game would pause, thinking it is in the background
		if (msg == WM_ACTIVATE) wp = WA_ACTIVE;
		if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0; // only the driver's mouse
	}
	if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL) {
		int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
		client_to_game(&x, &y);
		lp = MAKELPARAM(x, y);
	}
	else if (msg == WM_PAINT || msg == WM_SIZE || msg == WM_ACTIVATEAPP) {
		InterlockedExchange(&g_forcePresent, 1);
		mark_dirty();
	}
	return CallWindowProcA(g_gameProc, hwnd, msg, wp, lp);
}

static void subclass_window(HWND hwnd)
{
	if (g_hwnd == hwnd) return;
	g_hwnd = hwnd;
	g_gameProc = (WNDPROC)SetWindowLongA(hwnd, GWLP_WNDPROC, (LONG)window_proc);
	logf_("window %p subclassed", (void *)hwnd);
	if (g_remote) remote_hide(hwnd);
}

static BOOL (WINAPI *o_GetCursorPos)(POINT *);
static BOOL (WINAPI *o_GetClientRect)(HWND, RECT *);
static BOOL (WINAPI *o_ClientToScreen)(HWND, POINT *);
static HCURSOR (WINAPI *o_SetCursor)(HCURSOR);

// The game reads the cursor for one thing besides the pointer while it scrolls: the map scrolls
// when the cursor is at x <= 0 or x >= screen width - 1 (FUN_00432221, the width DAT_004e65a0
// from GetSystemMetrics). In wide mode game x is negative over the spare columns and the width is
// the frame's, so the edges are reported as those two values only when the cursor really is at
// the edge of the picture, and everything in between stays clear of them.
#define SCROLL_EDGE_W (*(int *)0x004e65a0)

static BOOL WINAPI h_GetCursorPos(POINT *p)
{
	if (g_remote) { // the driver's mouse, kept off the scrolling edges unless it asks for one (step "edge")
		int right = max(2, SCROLL_EDGE_W - 1), bottom = max(2, *(int *)0x004ea838 - 1);
		p->x = max(1, min(right - 1, g_fake.x));
		p->y = max(1, min(bottom - 1, g_fake.y));
		if (g_fakeEdge & 1) p->x = 0;
		if (g_fakeEdge & 2) p->x = right;
		if (g_fakeEdge & 4) p->y = 0;
		if (g_fakeEdge & 8) p->y = bottom;
		return TRUE;
	}
	if (!o_GetCursorPos(p)) return FALSE;
	if (g_hwnd) {
		ScreenToClient(g_hwnd, p);
		RECT v;
		view_rect(&v);
		int cx = p->x, x = p->x, y = p->y;
		client_to_game(&x, &y);
		if (g_off) {
			int right = max(2, SCROLL_EDGE_W - 1);
			if (cx <= v.left) x = 0;
			else if (cx >= v.right - 1) x = right;
			else x = max(1, min(right - 1, x));
			// The same for the bottom edge, measured against the frame's height (DAT_004ea838).
			int bottom = max(2, *(int *)0x004ea838 - 1), cy = p->y;
			if (cy <= v.top) y = 0;
			else if (cy >= v.bottom - 1) y = bottom;
			else y = max(1, min(bottom - 1, y));
		}
		p->x = x;
		p->y = y;
	}
	return TRUE;
}

// Remote mode: the game believes it is in front, and never brings itself there.
static HWND (WINAPI *o_GetForegroundWindow)(void);
static BOOL (WINAPI *o_SetForegroundWindow)(HWND);
static HWND WINAPI h_GetForegroundWindow(void) { return g_remote && g_hwnd ? g_hwnd : o_GetForegroundWindow(); }
static BOOL WINAPI h_SetForegroundWindow(HWND hwnd) { return g_remote ? TRUE : o_SetForegroundWindow(hwnd); }
// ... and its window is hidden from the moment it exists, never taking the focus.
static HWND (WINAPI *o_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *o_ShowWindow)(HWND, int);
static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
	HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
	if (!g_remote || parent) return o_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
	HWND hwnd = o_CreateWindowExA(ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, name, style & ~WS_VISIBLE, x, y, w, h, parent, menu, inst, param);
	if (hwnd) {
		remote_hide(hwnd);
		if (style & WS_VISIBLE) o_ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	}
	return hwnd;
}
static BOOL WINAPI h_ShowWindow(HWND hwnd, int cmd)
{
	if (g_remote && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_SHOWMAXIMIZED || cmd == SW_RESTORE)) cmd = SW_SHOWNOACTIVATE;
	return o_ShowWindow(hwnd, cmd);
}

static BOOL WINAPI h_GetClientRect(HWND hwnd, RECT *r)
{
	if (hwnd && hwnd == g_hwnd && r) {
		SetRect(r, 0, 0, g_frameW, g_frameH);
		return TRUE;
	}
	return o_GetClientRect(hwnd, r);
}

static BOOL WINAPI h_ClientToScreen(HWND hwnd, POINT *p)
{
	if (hwnd && hwnd == g_hwnd) return TRUE;
	return o_ClientToScreen(hwnd, p);
}

// After its mode switch the stock game saw a 640x480 screen: in fullscreen it takes the screen
// size as its window rectangle, which scales mouse positions and places every frame copy.
// The game checks the desktop's colour depth and warns "Non 256 colour display" when it is not
// 8 bits. Our DirectDraw is 8-bit whatever the desktop is, so it sees the display it wants. (The
// check cannot flip it to its windowed mode: the game starts in fullscreen mode and only ever
// switches the other way.)
static int (WINAPI *o_GetDeviceCaps)(HDC, int);
static int WINAPI h_GetDeviceCaps(HDC dc, int i)
{
	if (i == BITSPIXEL) return 8;
	if (i == PLANES) return 1;
	if (i == NUMCOLORS) return 20;
	return o_GetDeviceCaps(dc, i);
}

static int (WINAPI *o_GetSystemMetrics)(int);
static int WINAPI h_GetSystemMetrics(int i)
{
	if (i == SM_CXSCREEN) return g_frameW; // the game scales the mouse by frame width / screen width
	if (i == SM_CYSCREEN) return g_frameH;
	return o_GetSystemMetrics(i);
}

// Cursors: the game's are 32x32 one-bit; scaled by the system they turn blocky and their
// inverting pixels vanish, so each is rebuilt once as an alpha cursor enlarged by whole pixels.
static struct { HCURSOR from, to; int viewH; } g_cursors[64];
static int g_cursorCount;

static HCURSOR rebuild_cursor(HCURSOR src, int target)
{
	ICONINFO ii;
	BITMAP bm;
	if (target < 8 || !GetIconInfo(src, &ii)) return src;
	HCURSOR result = src;
	DWORD *mask = NULL, *colour = NULL, *out = NULL;
	HDC dc = GetDC(NULL);

	if (!GetObjectA(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm)) goto done;
	int w = bm.bmWidth;
	int maskH = bm.bmHeight; // monochrome: AND plane above XOR plane
	int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
	if (w <= 0 || h <= 0 || w > 256 || h > 256) goto done;

	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	mask = malloc(w * maskH * 4);
	bi.bmiHeader.biHeight = -maskH;
	if (!mask || !GetDIBits(dc, ii.hbmMask, 0, maskH, mask, &bi, DIB_RGB_COLORS)) goto done;
	if (ii.hbmColor) {
		colour = malloc(w * h * 4);
		bi.bmiHeader.biHeight = -h;
		if (!colour || !GetDIBits(dc, ii.hbmColor, 0, h, colour, &bi, DIB_RGB_COLORS)) goto done;
	}

	int W = target, H = MulDiv(h, target, w);
	bi.bmiHeader.biWidth = W;
	bi.bmiHeader.biHeight = -H;
	HBITMAP colourBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&out, NULL, 0);
	if (!colourBmp) goto done;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int i = (y * h / H) * w + (x * w / W);
			BOOL transparent = (mask[i] & 0xffffff) != 0;
			DWORD rgb = colour ? (colour[i] & 0xffffff) : ((mask[i + w * h] & 0xffffff) ? 0xffffff : 0);
			DWORD px;
			if (!transparent) px = 0xff000000 | rgb;
			else if (rgb == 0) px = 0;
			else px = colour ? 0 : 0xffffffff; // "invert the screen" has no alpha equivalent: white
			out[y * W + x] = px;
		}
	}
	HBITMAP maskBmp = CreateBitmap(W, H, 1, 1, NULL);
	ICONINFO made = { FALSE, MulDiv(ii.xHotspot, W, w), MulDiv(ii.yHotspot, H, h), maskBmp, colourBmp };
	HCURSOR cur = (HCURSOR)CreateIconIndirect(&made);
	DeleteObject(maskBmp);
	DeleteObject(colourBmp);
	if (cur) result = cur;
	logf_("cursor %p: %dx%d -> %p at %dx%d", (void *)src, w, h, (void *)result, W, H);

done:
	free(mask);
	free(colour);
	ReleaseDC(NULL, dc);
	if (ii.hbmColor) DeleteObject(ii.hbmColor);
	if (ii.hbmMask) DeleteObject(ii.hbmMask);
	return result;
}

static HCURSOR WINAPI h_SetCursor(HCURSOR cur)
{
	if (!cur) return o_SetCursor(cur);
	RECT v;
	view_rect(&v);
	int viewH = v.bottom - v.top;
	for (int i = 0; i < g_cursorCount; i++) {
		if (g_cursors[i].from == cur && g_cursors[i].viewH == viewH) return o_SetCursor(g_cursors[i].to);
		if (g_cursors[i].to == cur) return o_SetCursor(cur); // one of ours, handed back to us
	}
	// The game's cursors were 32 pixels on a 480-line screen; CursorScale is a percentage of that
	// share of the screen. Windows may hand them over larger for the desktop's DPI; any size works.
	int target = 32 * viewH / GAME_H * g_cursorScale / 100;
	// A rebuilt cursor is never destroyed (the game may hand it back at any time), so only as
	// many are made as can be remembered.
	if (g_cursorCount >= (int)(sizeof g_cursors / sizeof g_cursors[0])) return o_SetCursor(cur);
	HCURSOR made = rebuild_cursor(cur, target);
	g_cursors[g_cursorCount].from = cur;
	g_cursors[g_cursorCount].to = made;
	g_cursors[g_cursorCount].viewH = viewH;
	g_cursorCount++;
	return o_SetCursor(made);
}

// ---------------------------------------------------------------------------------------------
// Patching the exe

static BOOL hook_import(const char *dll, const char *name, void *hook, void *orig)
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return FALSE;

	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		if (lstrcmpiA((char *)(base + d->Name), dll) != 0) continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
		IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			IMAGE_IMPORT_BY_NAME *imp = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((char *)imp->Name, name) != 0) continue;
			DWORD old;
			VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			*(void **)orig = (void *)slots->u1.Function;
			slots->u1.Function = (DWORD_PTR)hook;
			VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
			return TRUE;
		}
	}
	logf_("import not found: %s!%s", dll, name);
	return FALSE;
}

// Copies the first len bytes of a function (whole, position-independent instructions) into an
// executable stub that then jumps back, so the original can still be called after redirect().
static void *make_trampoline(BYTE *site, int len)
{
	BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t) return NULL;
	memcpy(t, site, len);
	t[len] = 0xE9;
	*(DWORD *)(t + len + 1) = (DWORD)(site + len) - (DWORD)(t + len + 5);
	return t;
}

static BOOL redirect(BYTE *site, int len, void *to)
{
	DWORD old;
	if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
	site[0] = 0xE9;
	*(DWORD *)(site + 1) = (DWORD)to - (DWORD)(site + 5);
	for (int i = 5; i < len; i++) site[i] = 0x90;
	VirtualProtect(site, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, len);
	return TRUE;
}

// The game's file loader (0x004af8d9): int load(name, buffer, max bytes, offset), cdecl. Every
// picture and palette the game uses goes through it.
#define LOAD_FILE ((BYTE *)0x004af8d9)
static const BYTE kLoadPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C }; // push ebp; mov ebp,esp; sub esp,0Ch
typedef int (__cdecl *load_file_t)(const char *, void *, unsigned, long);
static load_file_t o_load_file;

static int __cdecl h_load_file(const char *name, void *buffer, unsigned size, long offset)
{
	int n = o_load_file(name, buffer, size, offset);
	if (n > 0 && offset == 0) sprites_loaded(name, buffer);
	if (n > 0 && offset == 0 && (!_strnicmp(name, "base", 4) || !_strnicmp(name, "roads", 5) || !_strnicmp(name, "mtns", 4) ||
		!_strnicmp(name, "town", 4) || !_strnicmp(name, "castle", 6)))
		tiles_loaded(name, buffer, n);
	if (g_logLoads) logf_("load %s -> %p, %d bytes", name, buffer, n);
	note_load(name);
	return n;
}

// ---------------------------------------------------------------------------------------------
// Wide mode: a frame as wide as the monitor's shape. The frame's pitch is in one variable the
// game sets at start-up, but its blitters also bake it into 700 instructions ("the pixel below"
// is [EDI+0x280]); every one is checked against the original bytes before any is changed, so a
// build that differs anywhere stays at 640.

#include "pitch_sites.h"

static BOOL patch_dword(DWORD addr, LONG from, LONG to)
{
	LONG *p = (LONG *)addr;
	if (*p != from) return FALSE;
	DWORD old;
	if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
	*p = to;
	VirtualProtect(p, 4, old, &old);
	return TRUE;
}

// Other places where the frame's size is written out: { address of the 32-bit field, value,
// what it becomes (0 = width, 1 = width * height) }.
static const struct { DWORD addr; LONG value; int kind; const char *what; } kFrameSites[] = {
	{ 0x004b2371 + 6, 640, 0, "frame pitch at start-up (DAT_004eb274)" },
	{ 0x004b237b + 6, 480, 2, "frame height at start-up (DAT_004eac4c)" },
	{ 0x0042db8c + 1, 640, 0, "Smacker destination pitch" },
	{ 0x0042de58 + 1, 640, 0, "Smacker destination pitch" },
	{ 0x004b13b0 + 3, 640 * 480, 1, "whole-frame darkening loop" },
	{ 0x004b1813 + 3, 640 * 480, 1, "whole-frame colour remap loop" },
	{ 0x004b187f + 1, 640 * 480, 1, "whole-frame clear" },
};

// The LBM picture decoder (0x004b061a: int decode(file data, frame, palette out, size), cdecl)
// writes a 640x480 picture into the frame as one run of bytes. In a wider frame that run has to
// be laid out row by row, so the picture is decoded into a 640-wide buffer first.
#define DECODE_LBM ((BYTE *)0x004b061a)
static const BYTE kDecodePrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30 }; // push ebp; mov ebp,esp; sub esp,30h
typedef int (__cdecl *decode_lbm_t)(void *, BYTE *, BYTE *, int);
static decode_lbm_t o_decode_lbm;

static int __cdecl h_decode_lbm(void *data, BYTE *dst, BYTE *pal, int size)
{
	g_mapSticky = FALSE;
	if (!g_back || dst != g_back->bits + g_back->org || g_back->pitch == GAME_W) return o_decode_lbm(data, dst, pal, size);
	static BYTE tmp[GAME_W * GAME_H];
	memset(tmp, 0, sizeof tmp);
	int r = o_decode_lbm(data, tmp, pal, size);
	for (int y = 0; y < GAME_H; y++) memcpy(dst + y * g_back->pitch, tmp + y * GAME_W, GAME_W);
	return r;
}

// The full-screen picture loader (0x00408fcb: void load_screen(name, rows), cdecl) reads a raw
// 640-wide picture file straight into the frame, pitch * rows bytes from offset 24. The 17 screens
// with a painted background (main menu, castle, armoury...) all come through here.
#define LOAD_SCREEN ((BYTE *)0x00408fcb)
static const BYTE kLoadScreenPrologue[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 }; // push ebp; mov ebp,esp; push ebx/esi/edi
typedef void (__cdecl *load_screen_t)(const char *, int);
static load_screen_t o_load_screen;
static int __cdecl h_load_file(const char *name, void *buffer, unsigned size, long offset);

static void __cdecl h_load_screen(const char *name, int rows)
{
	g_mapSticky = FALSE; // a painted full-screen background replaces the map
	if (!g_back || rows <= 0 || rows > GAME_H) {
		o_load_screen(name, rows);
		return;
	}
	static BYTE tmp[GAME_W * GAME_H];
	if (h_load_file(name, tmp, GAME_W * rows, 0x18) <= 0) {
		o_load_screen(name, rows); // the game reports the missing file its own way
		return;
	}
	BYTE *frame = g_back->bits + g_back->org;
	for (int y = 0; y < rows; y++) memcpy(frame + y * g_back->pitch, tmp + y * GAME_W, GAME_W);
	text_clear(0, 0, GAME_W, rows);
	hd_picture_loaded(name, tmp, rows);
}

// Saving and restoring the frame under a window (FUN_004b3ec0 buffer -> frame, FUN_004b3f0a
// frame -> buffer; cdecl (buffer, row skip)): every caller passes the skip as 640 - block width
// (0xa0, 0xf0, 400, 0x1c0, 0x1e0, 0x200), so the wider frame's extra columns are added here.
static const BYTE kBlockPrologue[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 }; // push ebp; mov ebp,esp; push ebx/esi/edi
typedef void (__cdecl *block_copy_t)(void *, int);
static block_copy_t o_block_to_frame, o_block_from_frame;
// The block under a drop-down menu (FUN_0040c8d1 saves, FUN_0040c928 restores: 400x180 at 0,24,
// skip 0xf0) moves with the menus to the wide map's left edge.
#define BLOCK_X (*(int *)0x00591524)
#define BLOCK_Y (*(int *)0x00591528)
static BOOL county_map_on(void);
static void menu_block(int skip, int *saved)
{
	*saved = BLOCK_X;
	if (g_off && skip == 0xf0 && BLOCK_X == 0 && BLOCK_Y == 0x18 && county_map_on()) BLOCK_X = -g_extra;
}
static void __cdecl h_block_to_frame(void *buffer, int skip)
{
	int x;
	menu_block(skip, &x);
	o_block_to_frame(buffer, skip + g_frameW - GAME_W);
	BLOCK_X = x;
}
static void __cdecl h_block_from_frame(void *buffer, int skip)
{
	int x;
	menu_block(skip, &x);
	o_block_from_frame(buffer, skip + g_frameW - GAME_W);
	BLOCK_X = x;
}

static BOOL hook_code(BYTE *site, const BYTE *prologue, int len, void *hook, void *orig)
{
	if (memcmp(site, prologue, len) != 0) {
		logf_("not hooked: unexpected code at %p", (void *)site);
		return FALSE;
	}
	void *t = make_trampoline(site, len);
	if (!t || !redirect(site, len, hook)) return FALSE;
	*(void **)orig = t;
	return TRUE;
}

// ---------------------------------------------------------------------------------------------
// The wider county map. FUN_004050c0 redraws the whole map (terrain FUN_0040526e, then the layers
// FUN_00405602 and FUN_00405487), as DAT_0053e8ac columns from map column DAT_005651b4, the first
// at screen x DAT_0052afdc, DAT_00568220 pixels apart. For the length of a redraw it starts k
// columns earlier, k * step further left, with k more columns: every tile and unit stays where it
// was and the extra columns land in the frame's spare columns, left of the game's x = 0.

#define MAP_COLUMNS   (*(int *)0x0053e8ac)
#define MAP_FIRST_X   (*(int *)0x0052afdc)
#define MAP_FIRST_COL (*(int *)0x005651b4)
#define MAP_STEP      (*(int *)0x00568220)
#define SCREEN_ID     (*(BYTE *)0x004eac50)
#define GAME_MODE     (*(int *)0x0052af94)

static const BYTE kRedrawPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C }; // push ebp; mov ebp,esp; sub esp,0Ch
typedef void (__cdecl *redraw_map_t)(void);
static redraw_map_t o_redraw_map;

// The blitters clip to the x range FUN_0040464c(left, right) is given; the map passes its first
// tile's x as the left limit. Further left than the frame's spare columns a write would land on
// the end of the row above, so the limit never goes past them.
typedef void (__cdecl *clip_x_t)(int, int);
static clip_x_t o_clip_x;
static BOOL g_drawingLeft; // a widened bar is being drawn: the screen's left edge is in the spare columns
static void __cdecl h_clip_x(int left, int right)
{
	if (g_drawingLeft && left == 0) left = battle_zoom_on() ? PANEL_X - battle_map_w() : -g_extra;
	o_clip_x(max(left, -g_off), right);
}

// The county screen's two strips that end at the game's left edge are drawn longer to the left
// when the map is wide: the top bar, mos_24blank(0, 0, 25, 1) in FUN_00419c78 - 24-pixel tiles -
// and the kingdom overview's caption, FUN_00409397(0, 412, 30, 4) in FUN_0040f5fd - a framed
// panel of 16-pixel tiles. The game writes their text over them as before.
typedef void (__cdecl *strip_t)(int, int, int, int);
static strip_t o_bar_tiles, o_framed_panel;
static strip_t o_bar_outline;
static BOOL county_map_on(void);

// The bar's menu titles (File, Options, Help): a table of {x, x end, y, text, items, count} at
// 0x004dc428, drawn left to right from the first x (10), each 32 pixels after the last. Their drop-
// downs open below them and clicks are matched against the x ranges the drawing wrote back, so
// moving the first x to the wide map's left edge moves all of it.
#define MENU_FIRST_X (*(short *)0x004dc428)

// The titles (FUN_0040c5b0) and an open drop-down (FUN_0040c725: panel and items, every frame)
// are drawn with the clip reaching into the spare columns, where they now start.
typedef void (__cdecl *menu_titles_t)(short *, int);
typedef void (__cdecl *menu_open_t)(void);
static menu_titles_t o_menu_titles;
static menu_open_t o_menu_open;
static void __cdecl h_menu_titles(short *table, int n)
{
	BOOL wide = g_off && county_map_on();
	g_drawingLeft = wide;
	o_menu_titles(table, n);
	g_drawingLeft = FALSE;
}
// In the zoomed-out battle the titles are in the doubled top bar and the list on the 1:1
// battlefield: this is the game x that puts the list right under its title.
static int menu_list_x(int titleX) { return 2 * (titleX + g_extra) - battle_map_w() + PANEL_X; }

static void __cdecl h_menu_open(void)
{
	BOOL wide = g_off && county_map_on();
	short *title = *(short **)0x00522cac;
	short saved = title ? title[0] : 0;
	BOOL zoomed = title && battle_zoom_on();
	if (zoomed) title[0] = (short)menu_list_x(saved);
	g_drawingLeft = wide;
	o_menu_open();
	g_drawingLeft = FALSE;
	if (zoomed) title[0] = saved;
}

// The open list's hit test (FUN_0040e099: items, count, x, y) looks in the same place.
typedef int (__cdecl *menu_hit_t)(short *, int, int, int);
static menu_hit_t o_menu_hit;
static int __cdecl h_menu_hit(short *items, int n, int x, int y)
{
	return o_menu_hit(items, n, battle_zoom_on() ? menu_list_x(x) : x, y);
}

static void __cdecl h_bar_tiles(int x, int y, int nx, int ny)
{
	if (x == 0 && y == 0 && nx == 25 && ny == 1)
		MENU_FIRST_X = g_off && county_map_on() ? 10 - g_extra : 10;
	if (g_off && x == 0 && y == 0 && nx == 25 && ny == 1 && county_map_on()) {
		int n = (g_extra + 23) / 24;
		o_bar_tiles(-24 * n, y, nx + n, ny);
		return;
	}
	o_bar_tiles(x, y, nx, ny);
}

// The top bar's outline, FUN_00403fdd(0, 0, 640, 24): light top line (0x1f), dark bottom and
// left lines (0x10). It cannot start left of 0, so on a wide map the outline is redone here over
// the whole bar and the left line at the old edge goes.
static void __cdecl h_bar_outline(int x, int y, int w, int h)
{
	o_bar_outline(x, y, w, h);
	if (!g_off || !g_back || x != 0 || y != 0 || w != GAME_W || h != 24 || !county_map_on()) return;
	int n = (g_extra + 23) / 24, left = -24 * n;
	BYTE *frame = g_back->bits + g_back->org;
	int pitch = g_back->pitch;
	for (int r = 1; r < h - 1; r++) frame[r * pitch] = frame[r * pitch + 1];
	memset(frame + left, 0x1f, -left);
	memset(frame + (h - 1) * pitch + left, 0x10, -left);
	for (int r = 0; r < h; r++) frame[r * pitch + left] = 0x10;
}

static void __cdecl h_framed_panel(int x, int y, int nx, int ny)
{
	if (g_off && x == 0 && y == 412 && nx == 30 && ny == 4 && county_map_on()) {
		int n = g_extra / 16;
		g_drawingLeft = TRUE;
		o_framed_panel(-16 * n, y, nx + n, ny);
		g_drawingLeft = FALSE;
		return;
	}
	o_framed_panel(x, y, nx, ny);
}

// The county map's zoom (DAT_0057cb18): 0 and 1 scroll. The kingdom overview (2) is one fixed
// picture of the kingdom - the game places it at column 14 and turns scrolling off - widened with
// the map's own columns left of it (flat filler land the 4:3 screen shows a sliver of).
// A drop-down menu from the top bar is screen 0x32, with the screen under it kept in DAT_0056d894.
#define MAP_ZOOM (*(int *)0x0057cb18)
static BOOL g_wideOverview = TRUE;
#define IN_BATTLE (*(int *)0x0057c8fc == 2)
#define SCREEN_UNDER_MENU (*(int *)0x0056d894)
// The wide view is on while the county map is the picture: from the moment the game redraws the
// whole map (h_redraw_map sets g_mapSticky) until something replaces the whole picture - a painted
// full-screen background (the loader hooks clear it), a battle (screen 41), the kingdom overview
// (zoom 2) - or the game leaves the county game (mode 3). Dialogs, drop-down menus and message
// scrolls drawn over the map keep it. Screen ids alone cannot tell: the castle builder, a painted
// screen, runs as screen 39 like the map's message scrolls.
static BOOL battle_zoom_on(void) { return g_battleZoom && g_off && IN_BATTLE && county_map_on(); }
static BOOL county_map_on(void)
{
	if (GAME_MODE != 3 || (!IN_BATTLE && MAP_ZOOM == 2 && !g_wideOverview)) {
		g_mapSticky = FALSE;
		return FALSE;
	}
	return g_mapSticky;
}

// The battlefield is widened for good when its view is set up (FUN_004bc020, x = 0, 15 columns of
// 32 pixels): it starts k columns further left with k more columns. The game's scroll column then
// simply means the view's new first column, and everything working from the view's x - picking,
// units, missiles, clipping - follows; only the scroll limit (FUN_0047ed34) needs k less.
typedef void (__cdecl *battle_view_t)(void *, void *, void *, int, int, int, int, int, int, int, int);
static battle_view_t o_battle_view;
static int g_battleK, g_battleR;
#define BATTLE_ROW (*(int *)0x005678a0)
static void __cdecl h_battle_view(void *a, void *b, void *c, int mapW, int d, int cell, int x, int y, int cols, int rows, int size)
{
	g_battleK = g_off && size > 0 ? (g_extra + size - 1) / size : 0;
	g_battleR = 0;
	if (g_battleZoom) {
		g_battleK = BATTLE_MORE_COLS;
		g_battleR = BATTLE_MORE_ROWS;
	}
	o_battle_view(a, b, c, mapW, d, cell, x - g_battleK * size, y, cols + g_battleK, rows + g_battleR, size);
	logf_("battle view: x %d, %d x %d cells of %d", x - g_battleK * size, cols + g_battleK, rows + g_battleR, size);
}

typedef void (__cdecl *battle_clamp_t)(void);
static battle_clamp_t o_battle_clamp;
#define BATTLE_COL (*(int *)0x0055ce70)
static void __cdecl h_battle_clamp(void)
{
	o_battle_clamp();
	if (BATTLE_COL > 0x41 - g_battleK) BATTLE_COL = 0x41 - g_battleK;
	if (BATTLE_ROW > 0x42 - g_battleR) BATTLE_ROW = 0x42 - g_battleR;
}

// Drawing the battlefield (FUN_004bc142) means it is the picture. On a full redraw the spare
// columns below the field are cleared of the last screen's pixels.
typedef void (__cdecl *battle_draw_t)(int, int);
static battle_draw_t o_battle_draw;
static void __cdecl h_battle_draw(int col, int row)
{
	if (g_off && g_back && GAME_MODE == 3) {
		if (!g_mapSticky || *(int *)0x0053e9ac) {
			BYTE *frame = g_back->bits + g_back->org;
			for (int y = 24; y < g_frameH; y++) memset(frame + y * g_back->pitch - g_off, 0, g_off);
		}
		if (!g_mapSticky) *(int *)0x0056d6a0 = 1; // the top bar was drawn narrow: draw it again
		g_mapSticky = TRUE;
	}
	if (col > 0x41 - g_battleK) BATTLE_COL = col = 0x41 - g_battleK;
	if (row > 0x42 - g_battleR) BATTLE_ROW = row = 0x42 - g_battleR;
	LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
	o_battle_draw(col, row);
	if (g_timing) draw_timed(1, t0);
}

// The view as the game set it, while it is widened for one call.
static struct { int cols, firstX, firstCol, k; } g_view;

// Widens the map view by k columns to the left, for the length of one call into the game.
// Returns FALSE (and changes nothing) when the map is not on a wide screen.
static BOOL begin_wide_view(void)
{
	if (!g_off || !g_back || IN_BATTLE || !county_map_on() || MAP_STEP <= 0) return FALSE;
	int k = (g_extra + MAP_FIRST_X + MAP_STEP - 1) / MAP_STEP; // columns to cover the visible spare pixels
	// The view reaches k columns further left, so the game's scroll stops k columns short of the
	// map's left edge: scrolled fully left, the map's first column is at the screen's edge.
	static int lastCol = -99, lastCols, lastK;
	if (MAP_FIRST_COL != lastCol || MAP_COLUMNS != lastCols || k != lastK) {
		logf_("map view: first column %d, %d columns, first x %d, step %d, widened by %d",
			MAP_FIRST_COL, MAP_COLUMNS, MAP_FIRST_X, MAP_STEP, k);
		lastCol = MAP_FIRST_COL;
		lastCols = MAP_COLUMNS;
		lastK = k;
	}
	// The kingdom overview stays where the game put it: only the map's real columns left of it.
	if (MAP_ZOOM == 2) k = min(k, MAP_FIRST_COL);
	else if (MAP_FIRST_COL < k) MAP_FIRST_COL = k;
	g_view.cols = MAP_COLUMNS;
	g_view.firstX = MAP_FIRST_X;
	g_view.firstCol = MAP_FIRST_COL;
	g_view.k = k;
	MAP_COLUMNS = g_view.cols + k;
	MAP_FIRST_X = g_view.firstX - k * MAP_STEP;
	MAP_FIRST_COL = g_view.firstCol - k;
	return TRUE;
}

static void end_wide_view(void)
{
	MAP_COLUMNS = g_view.cols;
	MAP_FIRST_X = g_view.firstX;
	MAP_FIRST_COL = g_view.firstCol;
}

// The map cell under the mouse (FUN_00429ba4): with the view widened it also finds cells over the
// spare columns. The column it leaves in DAT_00553f04 is relative to the scroll column, and the
// game adds its own scroll column to it, so it is moved back by k (negative over the spare
// columns, which every user of it handles: they add the scroll column or work out a screen x).
#define PICKED_COL (*(int *)0x00553f04)
typedef int (__cdecl *pick_cell_t)(int);
static pick_cell_t o_pick_cell;
static int __cdecl h_pick_cell(int arg)
{
	if (!begin_wide_view()) return o_pick_cell(arg);
	int r = o_pick_cell(arg);
	end_wide_view();
	PICKED_COL -= g_view.k;
	return r;
}

// A cell's ground: FUN_004063c1(mode) picks the file from the cell's record (the same choice as the
// game's), and the special cells' FUN_0042a7f1 / FUN_0042a8d1 (sea, shore) always use the base file.
typedef void (__cdecl *cell_t)(int);
static cell_t o_cell_ground, o_cell_special, o_cell_special2;
#define TILE_FRAME (*(int *)0x005c9288)
static void __cdecl h_cell_ground(int mode)
{
	int x = BLOCK_X, y = BLOCK_Y;
	o_cell_ground(mode);
	BOOL overview = MAP_ZOOM == 2;
	if (MAP_ZOOM != 0 && !overview) return;
	BYTE flags = ((BYTE *)0x00522f92)[*(int *)0x00567954];
	int sel = flags & 0x1c;
	if (*(int *)0x0053f264 == 1 && !(flags & 0x20)) sel = 0;
	const BYTE *buf = NULL;
	switch (sel) {
	case 0x00: buf = *(BYTE **)0x00568208; break;
	case 0x04: buf = *(BYTE **)0x0053e914; break;
	case 0x08: buf = *(BYTE **)0x0057d344; break;
	case 0x0c: buf = *(BYTE **)0x0055409c; break;
	case 0x10: buf = *(BYTE **)0x0056898c; break;
	}
	if (overview) stamp_sprite(buf, TILE_FRAME, x, y); // 10x6 tiles, kept in the sprite format
	else stamp_tile(buf, TILE_FRAME, x, y);
}
static void __cdecl h_cell_special(int mode)
{
	int x = BLOCK_X, y = BLOCK_Y;
	o_cell_special(mode);
	if (MAP_ZOOM == 0) stamp_tile(*(BYTE **)0x00568208, TILE_FRAME, x, y);
	else if (MAP_ZOOM == 2) stamp_sprite(*(BYTE **)0x00568208, TILE_FRAME, x, y);
}
static void __cdecl h_cell_special2(int mode)
{
	int x = BLOCK_X, y = BLOCK_Y;
	o_cell_special2(mode);
	if (MAP_ZOOM == 0) stamp_tile(*(BYTE **)0x00568208, TILE_FRAME, x, y);
	else if (MAP_ZOOM == 2) stamp_sprite(*(BYTE **)0x00568208, TILE_FRAME, x, y);
}

// The six sprite blitters: (file buffer) with the frame and position in globals. x and y have
// already been moved past any clipped part (DAT_0058fd50 columns, DAT_0058feac rows).
typedef int (__cdecl *blit_t)(const BYTE *);
static blit_t o_blit[8]; // 6 and 7: the panels' solid 16x16 and 24x24 tiles
#define SPRITE_STAMP(i) \
	static int __cdecl h_blit##i(const BYTE *buf) { \
		int x = BLOCK_X - *(int *)0x0058fd50, y = BLOCK_Y - *(int *)0x0058feac, frame = TILE_FRAME; \
		text_clear(BLOCK_X, BLOCK_Y, *(short *)0x005c9258, *(short *)0x005aeb70); \
		int r = o_blit[i](buf); \
		stamp_sprite(buf, frame, x, y); \
		return r; \
	}
SPRITE_STAMP(0) SPRITE_STAMP(1) SPRITE_STAMP(2) SPRITE_STAMP(3) SPRITE_STAMP(4) SPRITE_STAMP(5)
SPRITE_STAMP(6) SPRITE_STAMP(7)

// Overhang: trees, mountains, buildings and sheep are extra data after a tile's diamond, drawn
// upwards by FUN_00406673. Their art, tools\build_overhang.ps1, is a tall picture 36 rows above the
// cell, stored like a sprite under the tile file's name.
typedef void (__cdecl *cell_top_t)(int);
static cell_top_t o_cell_top;
static void __cdecl h_cell_top(int mode)
{
	int x = BLOCK_X, y = BLOCK_Y;
	o_cell_top(mode);
	if (MAP_ZOOM != 0) return;
	BYTE flags = ((BYTE *)0x00522f92)[*(int *)0x00567954];
	const BYTE *buf = NULL;
	switch (flags & 0x1c) {
	case 0x00: buf = *(BYTE **)0x00568208; break;
	case 0x04: buf = *(BYTE **)0x0053e914; break;
	case 0x08: buf = *(BYTE **)0x0057d344; break;
	case 0x0c: buf = *(BYTE **)0x0055409c; break;
	case 0x10: buf = *(BYTE **)0x0056898c; break;
	}
	if (buf) stamp_sprite(buf, TILE_FRAME, x, y - 36);
}

// [Debug] DumpOverhang=1: runs the three overhang blitters once on numbered bytes, in a scratch
// frame, and writes where each source byte lands: lords2hd_overhang.txt, "type h src dx dy".
static void dump_overhang(void)
{
	static BOOL done;
	if (done || !GetPrivateProfileIntA("Debug", "DumpOverhang", 0, g_iniPath) || !g_back) return;
	done = TRUE;
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%s\\lords2hd_overhang.txt", g_gameDir);
	FILE *f = fopen(path, "w");
	if (!f) return;
	enum { ROWS = 120, SRC = 4096 };
	int pitch = g_back->pitch;
	BYTE *scratch = malloc(pitch * ROWS), *a = malloc(pitch * ROWS), *src = malloc(SRC);
	BYTE **framePtr = (BYTE **)0x004ea1a8;
	BYTE *savedFrame = *framePtr;
	int savedX = BLOCK_X, savedY = BLOCK_Y, savedOff = *(int *)0x005aeb6c, savedH = *(int *)0x005bb478;
	*framePtr = scratch;
	BLOCK_X = 16;
	BLOCK_Y = 80;
	*(int *)0x005aeb6c = 0;
	typedef void (__cdecl *over_t)(const BYTE *, int, int);
	static const over_t fns[3] = { (over_t)0x00453e71, (over_t)0x004580a9, (over_t)0x00459258 };
	for (int type = 2; type <= 4; type++) for (int h = 1; h <= 40; h++) {
		*(int *)0x005bb478 = h;
		for (int pass = 0; pass < 2; pass++) {
			for (int i = 0; i < SRC; i++) src[i] = pass ? 1 + i / 255 : 1 + i % 255;
			memset(scratch, 0, pitch * ROWS);
			fns[type - 2](src, 1000, 0); // strips drawn in full: a smaller count is for cells near the top
			if (!pass) memcpy(a, scratch, pitch * ROWS);
		}
		for (int i = 0; i < pitch * ROWS; i++) if (a[i])
			fprintf(f, "%d %d %d %d %d\n", type, h, (scratch[i] - 1) * 255 + a[i] - 1, i % pitch - 16, i / pitch - 80);
	}
	*framePtr = savedFrame;
	BLOCK_X = savedX;
	BLOCK_Y = savedY;
	*(int *)0x005aeb6c = savedOff;
	*(int *)0x005bb478 = savedH;
	free(scratch); free(a); free(src);
	fclose(f);
	logf_("overhang layout written");
}

// Tooltips (position clamped by FUN_00477249) would straddle the side panel, half 1:1 and half
// doubled, in the zoomed-out view: they stay wholly on the battlefield, 192 wide with their saved
// background.
typedef void (__cdecl *tip_clamp_t)(void);
static tip_clamp_t o_tip_clamp;
static void __cdecl h_tip_clamp(void)
{
	o_tip_clamp();
	if (battle_zoom_on() && *(int *)0x004eb268 > PANEL_X - 192) *(int *)0x004eb268 = PANEL_X - 192;
}

// "Battle Paused" (FUN_00423b4f) sits near the bottom of the 480-row screen: in the zoomed-out
// view it goes to the same place at the bottom of the taller battlefield.
typedef void (__cdecl *paused_t)(void);
static paused_t o_paused;
static void __cdecl h_paused(void)
{
	// Asked of the battle itself, not of battle_zoom_on(): that is briefly off after the game loads
	// a picture, and the label then went to the 480-row place, in the middle of the field.
	if (!(g_battleZoom && g_off && IN_BATTLE && GAME_MODE == 3)) {
		o_paused();
		return;
	}
	typedef void (__cdecl *text_t)(int, int, int, int, void *, int);
	typedef void (__cdecl *sprite_t)(int, int, int, int);
	int dy = g_frameH - GAME_H - 24;
	*(int *)0x005aea40 = 1;
	((text_t)0x00402d37)(0x20, 0, 0x10e, 0x1ae + dy, (void *)0x005b2fa0, 0x20);
	((sprite_t)0x0040a21a)(0x5bb540, 0x4d, 400, 0x1c4 + dy);
	*(int *)0x005aea40 = 0;
}

// The outline routine (FUN_00403cf4, the battle's selection box) moves a left edge below 0 to 0 but
// keeps the width, which at 640 never happened. In wide mode the box's right side then lands in the
// side panel, which nothing cleans. Boxes reaching into the spare columns are drawn here instead.
typedef void (__cdecl *box_t)(int, int, int, int, int);
static box_t o_box;
static void __cdecl h_box(int x, int y, int w, int h, int colour)
{
	BOOL inZoom = g_battleZoom && g_off && IN_BATTLE && GAME_MODE == 3; // see h_paused
	BOOL zoomed = inZoom && __builtin_return_address(0) == (void *)0x00412a9d; // FUN_0041298a, the selection box
	// A selection box never reaches into the side panel: nothing there would clean it, and it
	// stayed on the battle overview as a second, black rectangle.
	if (zoomed && x + w - 1 >= PANEL_X) w = PANEL_X - x;
	if (zoomed && (x >= PANEL_X || w <= 0)) return;
	if (g_timing && battle_zoom_on()) {
		static void *seen[32];
		void *from = __builtin_return_address(0);
		for (int i = 0; i < 32; i++) {
			if (seen[i] == from) break;
			if (!seen[i]) {
				seen[i] = from;
				logf_("box from %p: %d,%d %dx%d colour %d", from, x, y, w, h, colour);
				break;
			}
		}
	}
	if (!g_off || !g_back || (!county_map_on() && !zoomed) || (x >= 0 && !zoomed)) {
		o_box(x, y, w, h, colour);
		return;
	}
	int left = max(x, -g_extra), right = x + w - 1, top = max(y, 0), bottom = min(y + h - 1, GAME_H - 1);
	if (zoomed) {
		// The game cut the box's height at 480 rows: take it from the mouse again.
		int y0 = *(int *)0x0057a0e8, y1 = *(int *)0x004e65f4;
		left = max(x, PANEL_X - battle_map_w());
		top = max(24, min(y0, y1));
		bottom = min(g_frameH - 25, max(y0, y1));
	}
	if (right < left || bottom < top) return;
	BYTE *frame = g_back->bits + g_back->org;
	int pitch = g_back->pitch;
	memset(frame + top * pitch + left, colour, right - left + 1);
	memset(frame + bottom * pitch + left, colour, right - left + 1);
	for (int r = top; r <= bottom; r++) {
		if (x == left) frame[r * pitch + left] = (BYTE)colour;
		frame[r * pitch + right] = (BYTE)colour;
	}
}

typedef void (__cdecl *cell_32_t)(const BYTE *);
static cell_32_t o_cell_32;
static void __cdecl h_cell_32(const BYTE *buf)
{
	if (g_btValid && battle_zoom_on() && g_back) {
		int x = BLOCK_X + g_back->org, y = BLOCK_Y, w = *(short *)0x005c9258, h = *(short *)0x005aeb70;
		for (int r = max(y, 0); r < min(y + h, g_frameH); r++) {
			int x0 = max(x, 0), x1 = min(x + w, g_frameW);
			if (x1 > x0) memset(g_btValid + r * g_frameW + x0, 0, x1 - x0);
		}
	}
	int x = BLOCK_X, y = BLOCK_Y, frame = TILE_FRAME;
	o_cell_32(buf);
	if (battle_zoom_on()) stamp_sprite(buf, frame, x, y); // the ground itself
}

// The three letter blitters (FUN_004b41b7 and its two clipped versions; only text uses them).
static blit_t o_glyph[3];
static int glyph_common(int i, const BYTE *buf)
{
	int index = TILE_FRAME;
	const short *rec = (const short *)(buf + 8 + index * 16);
	int w = rec[0], h = rec[1];
	enum { MAXG = 128 };
	if (!g_hdText || !g_back || w <= 0 || h <= 0 || w > MAXG || h > MAXG) return o_glyph[i](buf);
	int x = BLOCK_X - *(int *)0x0058fd50, y = BLOCK_Y - *(int *)0x0058feac;
	int colour = *(int *)0x005cd40c == 1 ? -1 : (BYTE)*(int *)0x0057d3bc;
	static BYTE before[MAXG * MAXG];
	for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) {
		int fx = x + g_back->org + c, fy = y + r;
		before[r * w + c] = fx >= 0 && fx < g_back->w && fy >= 0 && fy < g_back->h ? g_back->bits[fy * g_back->pitch + fx] : 0;
	}
	int ret = o_glyph[i](buf);
	text_stamp(buf + *(const int *)(rec + 2), w, h, x, y, colour, before);
	return ret;
}
static int __cdecl h_glyph0(const BYTE *b) { return glyph_common(0, b); }
static int __cdecl h_glyph1(const BYTE *b) { return glyph_common(1, b); }
static int __cdecl h_glyph2(const BYTE *b) { return glyph_common(2, b); }

static void __cdecl h_redraw_map(void)
{
	dump_overhang();
	if (GAME_MODE == 3) g_mapSticky = TRUE; // the map is the picture again
	if (*(int *)0x0053e9ac) forget_tiles(); // a full redraw: everything is stamped again
	if (!g_off || !g_back || !county_map_on() || MAP_STEP <= 0) {
		o_redraw_map();
		return;
	}
	// Whatever is left of the map is black, not the last screen's pixels - between the top bar and
	// the map's bottom edge (DAT_00553254: 408 in the kingdom overview, where the caption panel is
	// below), since the game does not always draw those two again after the map.
	BYTE *frame = g_back->bits + g_back->org;
	int bottom = max(24, min(GAME_H, *(int *)0x00553254));
	// (only the columns on screen: the game calls this hundreds of times a second)
	for (int y = 24; y < bottom; y++) memset(frame + y * g_back->pitch - g_extra, 0, g_extra);
	if (!begin_wide_view()) {
		o_redraw_map();
		return;
	}
	LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
	o_redraw_map();
	end_wide_view();
	if (g_timing) draw_timed(0, t0);
	// The kingdom overview runs out of map columns before the screen's left edge: the strip left
	// is filled with the land 4 columns to its right (the map repeats every column), HD layer too.
	int strip = g_extra - g_view.k * MAP_STEP - g_view.firstX, shift = 4 * MAP_STEP;
	if (MAP_ZOOM == 2 && strip > 0 && strip <= shift) {
		int x0 = g_back->org - g_extra;
		for (int y = 24; y < bottom; y++) {
			BYTE *row = g_back->bits + y * g_back->pitch + x0;
			memcpy(row, row + shift, strip);
			if (!g_sprLayer) continue;
			memcpy(g_sprShadow + y * MAX_W + x0, g_sprShadow + y * MAX_W + x0 + shift, strip);
			memcpy(g_sprValid + y * MAX_W + x0, g_sprValid + y * MAX_W + x0 + shift, strip);
			for (int r = 0; r < 2; r++) {
				DWORD *o = g_sprLayer + (2 * y + r) * (2 * MAX_W) + 2 * x0;
				memcpy(o, o + 2 * shift, strip * 2 * sizeof(DWORD));
			}
		}
	}
}

// "y * 640" compiled as LEA r,[r+r*4] ; SHL r,7 (tools\find_mul640.ps1), in the rectangle
// darkener FUN_004b13ed. Both 6 bytes become IMUL r,r,width (69 /r imm32), also 6 bytes.
static const struct { DWORD addr; BYTE lea[3], shl[3], imul[2]; } kMulSites[] = {
	{ 0x004b14bb, { 0x8D, 0x04, 0x80 }, { 0xC1, 0xE0, 0x07 }, { 0x69, 0xC0 } }, // EAX
	{ 0x004b14de, { 0x8D, 0x0C, 0x89 }, { 0xC1, 0xE1, 0x07 }, { 0x69, 0xC9 } }, // ECX
};

static void install_wide(void)
{
	if (!g_wantWide) return;
	HMONITOR mon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { sizeof mi };
	if (!GetMonitorInfoA(mon, &mi)) return;
	int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
	int w = (GAME_H * mw / max(1, mh)) & ~15; // whole 16-pixel blocks: one start-up effect copies 16x16 tiles
	w = max(GAME_W, min(MAX_W - GUARD, w));
	if (w == GAME_W) {
		logf_("wide mode: the monitor (%dx%d) is not wider than 4:3", mw, mh);
		return;
	}
	g_extra = w - GAME_W;
	if (g_battleZoom) {
		// The zoomed-out battlefield: twice the picture's size less the doubled side panel.
		int mapW = 2 * w - 2 * (GAME_W - PANEL_X);
		w = (GAME_W + mapW - PANEL_X + GUARD + 15) & ~15;
		g_frameH = GAME_H * 2;
	}
	else w += GUARD;
	int n = sizeof kPitchSites / sizeof kPitchSites[0];
	for (int i = 0; i < n; i++) {
		if (*(LONG *)(kPitchSites[i].addr + kPitchSites[i].off) != kPitchSites[i].value) {
			logf_("wide mode off: pitch site %08lx holds %ld, expected %ld",
				(unsigned long)kPitchSites[i].addr, *(LONG *)(kPitchSites[i].addr + kPitchSites[i].off), kPitchSites[i].value);
			return;
		}
	}
	for (int i = 0; i < (int)(sizeof kFrameSites / sizeof kFrameSites[0]); i++) {
		if (*(LONG *)kFrameSites[i].addr != kFrameSites[i].value) {
			logf_("wide mode off: %s at %08lx holds %ld, expected %ld", kFrameSites[i].what,
				(unsigned long)kFrameSites[i].addr, *(LONG *)kFrameSites[i].addr, kFrameSites[i].value);
			return;
		}
	}
	for (int i = 0; i < (int)(sizeof kMulSites / sizeof kMulSites[0]); i++) {
		BYTE *p = (BYTE *)kMulSites[i].addr;
		if (memcmp(p, kMulSites[i].lea, 3) || memcmp(p + 3, kMulSites[i].shl, 3)) {
			logf_("wide mode off: y*640 at %08lx is not the expected LEA/SHL", (unsigned long)kMulSites[i].addr);
			return;
		}
	}
	for (int i = 0; i < n; i++)
		patch_dword(kPitchSites[i].addr + kPitchSites[i].off, kPitchSites[i].value,
			kPitchSites[i].rows * w + kPitchSites[i].col);
	for (int i = 0; i < (int)(sizeof kMulSites / sizeof kMulSites[0]); i++) {
		BYTE *p = (BYTE *)kMulSites[i].addr;
		DWORD old;
		VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
		p[0] = kMulSites[i].imul[0];
		p[1] = kMulSites[i].imul[1];
		*(LONG *)(p + 2) = w;
		VirtualProtect(p, 6, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 6);
	}
	for (int i = 0; i < (int)(sizeof kFrameSites / sizeof kFrameSites[0]); i++)
		patch_dword(kFrameSites[i].addr, kFrameSites[i].value,
			kFrameSites[i].kind == 2 ? g_frameH : kFrameSites[i].kind ? w * g_frameH : w);
	g_frameW = w;
	g_off = w - GAME_W;
	// Jumping to a spot on the battlefield (minimap click, FUN_00432443; a unit, FUN_0043c910) puts
	// it 7 columns in, the middle of 15: move that to the middle of the wider view.
	static const DWORD kCentreSites[] = { 0x00432513, 0x0043ca27, 0x0043251e, 0x0043ca42 }; // SUB EAX,7: column, column, row, row
	for (int i = 0; i < 4; i++) {
		BYTE *p = (BYTE *)kCentreSites[i];
		DWORD old;
		int more = g_battleZoom ? (i < 2 ? BATTLE_MORE_COLS : BATTLE_MORE_ROWS) / 2 : (i < 2 ? (g_extra + 31) / 32 / 2 : 0);
		if (p[0] == 0x83 && p[1] == 0xE8 && p[2] == 7 && VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old)) {
			p[2] = (BYTE)(7 + more);
			VirtualProtect(p, 3, old, &old);
		}
	}
	if (memcmp(DECODE_LBM, kDecodePrologue, sizeof kDecodePrologue) == 0) {
		o_decode_lbm = make_trampoline(DECODE_LBM, sizeof kDecodePrologue);
		if (o_decode_lbm) redirect(DECODE_LBM, sizeof kDecodePrologue, h_decode_lbm);
	}
	hook_code((BYTE *)0x004050c0, kRedrawPrologue, sizeof kRedrawPrologue, h_redraw_map, &o_redraw_map);
	static const BYTE kPickPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28 }; // push ebp; mov ebp,esp; sub esp,28h
	hook_code((BYTE *)0x00429ba4, kPickPrologue, sizeof kPickPrologue, h_pick_cell, &o_pick_cell);
	hook_code((BYTE *)0x0040464c, kBlockPrologue, sizeof kBlockPrologue, h_clip_x, &o_clip_x);
	hook_code((BYTE *)0x004bc020, kBlockPrologue, sizeof kBlockPrologue, h_battle_view, &o_battle_view);
	hook_code((BYTE *)0x0047ed34, kBlockPrologue, sizeof kBlockPrologue, h_battle_clamp, &o_battle_clamp);
	hook_code((BYTE *)0x004bc142, kBlockPrologue, sizeof kBlockPrologue, h_battle_draw, &o_battle_draw);
	hook_code((BYTE *)0x00409e09, kRedrawPrologue, sizeof kRedrawPrologue, h_bar_tiles, &o_bar_tiles);
	hook_code((BYTE *)0x00409397, kBlockPrologue, sizeof kBlockPrologue, h_framed_panel, &o_framed_panel);
	hook_code((BYTE *)0x00403fdd, kBlockPrologue, sizeof kBlockPrologue, h_bar_outline, &o_bar_outline);
	static const BYTE kTitlesPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 }; // push ebp; mov ebp,esp; sub esp,18h
	hook_code((BYTE *)0x0040c5b0, kTitlesPrologue, sizeof kTitlesPrologue, h_menu_titles, &o_menu_titles);
	static const BYTE kDropPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C }; // push ebp; mov ebp,esp; sub esp,1Ch
	hook_code((BYTE *)0x0040c725, kDropPrologue, sizeof kDropPrologue, h_menu_open, &o_menu_open);
	hook_code((BYTE *)0x004b3ec0, kBlockPrologue, sizeof kBlockPrologue, h_block_to_frame, &o_block_to_frame);
	hook_code((BYTE *)0x004b3f0a, kBlockPrologue, sizeof kBlockPrologue, h_block_from_frame, &o_block_from_frame);
	logf_("wide mode: frame %dx%d (%d shown) for a %dx%d monitor; %d pitch sites rewritten", w, GAME_H, w - GUARD, mw, mh, n);
}

// The one build this was made for: the GOG release, 1997-06-05.
static BOOL known_build(void)
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	return nt->FileHeader.TimeDateStamp == 0x3396EFAA && nt->OptionalHeader.SizeOfImage == 0x1E9000 &&
		memcmp(LOAD_FILE, kLoadPrologue, sizeof kLoadPrologue) == 0;
}

// Old-game bug: started without focus, the game gets a paint message before its surfaces exist and
// its frame copy (FUN_00452160) calls through a null primary surface. Nothing to show yet: skip it.
typedef void (__cdecl *show_frame_t)(int);
static show_frame_t o_show_frame;
static void __cdecl h_show_frame(int force)
{
	if (*(void **)0x004eabd0) o_show_frame(force);
}

static void install(void)
{
	hook_import("USER32.dll", "GetCursorPos", h_GetCursorPos, &o_GetCursorPos);
	hook_import("USER32.dll", "GetForegroundWindow", h_GetForegroundWindow, &o_GetForegroundWindow);
	hook_import("USER32.dll", "SetForegroundWindow", h_SetForegroundWindow, &o_SetForegroundWindow);
	hook_import("USER32.dll", "CreateWindowExA", h_CreateWindowExA, &o_CreateWindowExA);
	hook_import("USER32.dll", "ShowWindow", h_ShowWindow, &o_ShowWindow);
	// Remote mode mutes the process's audio session (DirectSound too). Windows remembers that volume
	// for the exe, so the next normal start would be silent: a marker file says to turn it back up.
	char muted[MAX_PATH];
	snprintf(muted, sizeof muted, "%s\\lords2hd-muted.txt", g_gameDir);
	if (g_remote) {
		waveOutSetVolume(0, 0);
		FILE *m = fopen(muted, "w");
		if (m) fclose(m);
	}
	else if (GetFileAttributesA(muted) != INVALID_FILE_ATTRIBUTES) {
		waveOutSetVolume(0, 0xFFFFFFFF);
		DeleteFileA(muted);
		logf_("sound turned back up after remote testing");
	}
	hook_import("USER32.dll", "GetClientRect", h_GetClientRect, &o_GetClientRect);
	hook_import("USER32.dll", "ClientToScreen", h_ClientToScreen, &o_ClientToScreen);
	hook_import("USER32.dll", "SetCursor", h_SetCursor, &o_SetCursor);
	hook_import("USER32.dll", "GetSystemMetrics", h_GetSystemMetrics, &o_GetSystemMetrics);
	hook_import("GDI32.dll", "GetDeviceCaps", h_GetDeviceCaps, &o_GetDeviceCaps);
	static const BYTE kShowPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 }; // push ebp; mov ebp,esp; sub esp,10h
	if (memcmp((BYTE *)0x00452160, kShowPrologue, sizeof kShowPrologue) == 0) {
		o_show_frame = make_trampoline((BYTE *)0x00452160, sizeof kShowPrologue);
		if (o_show_frame) redirect((BYTE *)0x00452160, sizeof kShowPrologue, h_show_frame);
	}
	static const BYTE kCellPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
	static const BYTE kSpecialPrologue[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
	static const BYTE kBlitPrologue[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
	static const BYTE kMiniLoad[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 }, kMiniPaint[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
	hook_code((BYTE *)0x0046a037, kMiniLoad, 6, h_mini_load, &o_mini_load);
	hook_code((BYTE *)0x00410cbd, kMiniPaint, 6, h_mini_paint, &o_mini_paint);
	hook_code((BYTE *)0x004b41b7, kBlitPrologue, 6, h_glyph0, &o_glyph[0]);
	hook_code((BYTE *)0x004b425c, kBlitPrologue, 6, h_glyph1, &o_glyph[1]);
	hook_code((BYTE *)0x004b42c5, kBlitPrologue, 6, h_glyph2, &o_glyph[2]);
	hook_code((BYTE *)0x004b43b1, kBlitPrologue, 6, h_blit0, &o_blit[0]);
	hook_code((BYTE *)0x004b446d, kBlitPrologue, 6, h_blit1, &o_blit[1]);
	hook_code((BYTE *)0x004b44d2, kBlitPrologue, 6, h_blit2, &o_blit[2]);
	hook_code((BYTE *)0x004b4537, kBlitPrologue, 6, h_blit3, &o_blit[3]);
	hook_code((BYTE *)0x004b4814, kBlitPrologue, 6, h_blit4, &o_blit[4]);
	hook_code((BYTE *)0x004b48b8, kBlitPrologue, 6, h_blit5, &o_blit[5]);
	hook_code((BYTE *)0x004b49f7, kBlitPrologue, 6, h_blit6, &o_blit[6]);
	hook_code((BYTE *)0x004b4cb8, kBlitPrologue, 6, h_blit7, &o_blit[7]);
	hook_code((BYTE *)0x00403cf4, kBlitPrologue, 6, h_box, &o_box);
	hook_code((BYTE *)0x00423b4f, kBlitPrologue, 6, h_paused, &o_paused);
	hook_code((BYTE *)0x004b5333, kBlitPrologue, 6, h_cell_32, &o_cell_32);
	static const BYTE kMenuHitPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
	hook_code((BYTE *)0x0040e099, kMenuHitPrologue, 6, h_menu_hit, &o_menu_hit);
	hook_code((BYTE *)0x00477249, kBlitPrologue, 6, h_tip_clamp, &o_tip_clamp);
	hook_code((BYTE *)0x00406673, kCellPrologue, sizeof kCellPrologue, h_cell_top, &o_cell_top);
	hook_code((BYTE *)0x004063c1, kCellPrologue, sizeof kCellPrologue, h_cell_ground, &o_cell_ground);
	hook_code((BYTE *)0x0042a7f1, kSpecialPrologue, sizeof kSpecialPrologue, h_cell_special, &o_cell_special);
	hook_code((BYTE *)0x0042a8d1, kSpecialPrologue, sizeof kSpecialPrologue, h_cell_special2, &o_cell_special2);
	if (memcmp(LOAD_SCREEN, kLoadScreenPrologue, sizeof kLoadScreenPrologue) == 0) {
		o_load_screen = make_trampoline(LOAD_SCREEN, sizeof kLoadScreenPrologue);
		if (o_load_screen) redirect(LOAD_SCREEN, sizeof kLoadScreenPrologue, h_load_screen);
	}
	install_wide();
	o_load_file = make_trampoline(LOAD_FILE, sizeof kLoadPrologue);
	if (o_load_file && redirect(LOAD_FILE, sizeof kLoadPrologue, h_load_file)) logf_("file loader hooked");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(inst);
	GetModuleFileNameA(NULL, g_gameDir, sizeof g_gameDir);
	char *slash = strrchr(g_gameDir, '\\');
	if (slash) *slash = 0;
	snprintf(g_iniPath, sizeof g_iniPath, "%s\\lords2hd.ini", g_gameDir);
	snprintf(g_logPath, sizeof g_logPath, "%s\\lords2hd.log", g_gameDir);
	DeleteFileA(g_logPath);
	InitializeCriticalSection(&g_lock);
	InitializeCriticalSection(&g_recentLock);
	load_real_ddraw();

	SYSTEMTIME t;
	GetLocalTime(&t);
	logf_("Lords of the Realm II HD, %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	read_settings();
	if (!known_build()) {
		logf_("unknown Lords2.exe build: passing DirectDraw through, nothing patched");
		g_emulate = FALSE;
		return TRUE;
	}
	AddVectoredExceptionHandler(1, crash_logger);
	if (!g_emulate) {
		logf_("[Display] Enabled=0: stock DirectDraw");
		return TRUE;
	}
	// The window is drawn at the monitor's real pixels; without this Windows would stretch it
	// again on displays with scaling above 100%.
	BOOL (WINAPI *aware)(void) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
	if (aware) aware();
	install();
	return TRUE;
}
