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

#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <dsound.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <limits.h>

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
// An open drop-down menu in the zoomed-out battle (screen 50): the game draws it at battlefield
// scale at g_menuX, g_menuY (h_menu_open); it is shown doubled from that corner, like the top bar
// its title is in, 13 x 12 cells of 16 (the panel FUN_0040c725 draws and refreshes).
static volatile int g_menuX = -10000, g_menuY, g_menuH = 12 * 16;
#define MENU_W (12 * 16)
#define MENU_H g_menuH
static BOOL menu_doubled(void) { return *(BYTE *)0x004eac50 == 0x32 && *(int *)0x00522cb4 != 0 && g_menuX > -10000; }
static BOOL entry_dark(const DWORD *px, int w, int h, int w1);
static void __cdecl h_msg_a(void);
static BOOL g_battleEndKeep;     // testing (remote message 11 with wParam 1): the battle-end scroll where the game puts it
static POINT g_fake;                // the pretend mouse, in game coordinates
static volatile LONG g_shotWanted, g_shot1xWanted;
#define WM_REMOTE (WM_APP + 0x100)
static void shot_save(const DWORD *px, int w, int h);
static BOOL g_wantWide;
static BOOL g_hdScreens = TRUE, g_hdTiles = TRUE, g_hdText = TRUE, g_shoreWaves = TRUE;
static BOOL g_tutorialOnce = TRUE; // [Display] TutorialOnce: each tutorial scroll once (h_tutorial)
static int g_frameLimit; // [Display] FrameLimit: game frames a second (0 = no limit)
static BOOL g_wasdOn;    // [Display] WASD: the keys scroll the map (wasd_edges)
static BOOL g_letters;   // [Display] Letters: fresh diplomatic letters (h_edit_text)
static BOOL g_armyPanel; // [Display] ArmyPanel: the selected army's scroll (army_panel_keep)
static BOOL g_resounded; // [Display] Resounded: the restored voices (Lords2Resounded) instead of the game's (h_mmioOpenA)
static BOOL g_cycleOn;   // [Display] CycleKeys: , and . cycle counties and armies (cycle_key)
static BOOL cycle_key(int dir);
static int army_cell(int id);
static int cycle_button_at(int x, int y);
static BOOL army_panel_at(int x, int y);
static int g_apStyle, g_apSig;
static void cycle_buttons_draw(int sx, int sw);
static volatile LONG g_wasd;
static int wasd_bit(WPARAM vk);
static BOOL wasd_screen(void);
static int g_upscaleCR;  // [Display] Upscale: 1 Catmull-Rom, 0 bilinear (resize_cr), 2 FSR (fsr_easu, fsr_rcas)
static int g_fsrFinal;
static float g_rcasSharp;
static int g_gpuWanted;         // [Display] Renderer: gpu (1, gpu_present) or cpu (0), as this session started
static BOOL g_gpuWantedSet;     // the F10 Drawing line, saved for the next start
static int g_repeatOn;          // [Display] RepeatFrames: refreshes with nothing new get the last picture again (gpu_repeat)
static BOOL gpu_drawing(void);  // the GPU draws the window (or is about to try)
static double gpu_ms(void);     // the card's time for a recent frame
static volatile LONG g_statsOn; // [Debug] Stats / Ctrl+F10: the frame-timing overlay (perf_draw)
static BOOL g_miniPop; // [Display] MinimapPop: battle minimap troops outlined (minimap_dots_paint)
static BOOL g_perfLogOn;        // [Debug] PerfLog: every picture's timings to lords2hd-perf.csv
static volatile LONG g_menuOpen; // the settings panel (F10) is up
static BOOL g_menuEatRUp;       // it was closed by a right-click: its release is not the game's
static BOOL menu_key(int vk);
static void menu_click(int wx, int wy, int gx, int gy);

// The log stays open and is flushed per line, so it is complete if the game crashes.
static void logf_(const char *fmt, ...)
{
	static FILE *f;
	if (!f) f = fopen(g_logPath, "a");
	if (!f) return;
	static DWORD t0;
	if (!t0) t0 = GetTickCount();
	fprintf(f, "%6lu  ", GetTickCount() - t0); // ms since the patch started
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
	g_tutorialOnce = GetPrivateProfileIntA("Display", "TutorialOnce", 1, g_iniPath) != 0;
	g_wasdOn = GetPrivateProfileIntA("Display", "WASD", 1, g_iniPath) != 0;
	g_letters = GetPrivateProfileIntA("Display", "Letters", 1, g_iniPath) != 0;
	g_cycleOn = GetPrivateProfileIntA("Display", "CycleKeys", 1, g_iniPath) != 0;
	g_armyPanel = GetPrivateProfileIntA("Display", "ArmyPanel", 1, g_iniPath) != 0;
	g_resounded = GetPrivateProfileIntA("Display", "Resounded", 1, g_iniPath) != 0;
	GetPrivateProfileStringA("Display", "Renderer", "gpu", s, sizeof s, g_iniPath);
	g_gpuWanted = g_gpuWantedSet = lstrcmpiA(s, "cpu") != 0;
	g_repeatOn = GetPrivateProfileIntA("Display", "RepeatFrames", 1, g_iniPath) != 0;
	GetPrivateProfileStringA("Display", "Upscale", "catmullrom", s, sizeof s, g_iniPath);
	g_upscaleCR = !lstrcmpiA(s, "bilinear") ? 0 : !lstrcmpiA(s, "fsr") ? 2 : 1;
	g_fsrFinal = GetPrivateProfileIntA("Display", "FsrFinal", 0, g_iniPath) != 0;
	GetPrivateProfileStringA("Display", "FsrSharpness", "0.2", s, sizeof s, g_iniPath);
	g_rcasSharp = (float)atof(s);
	logf_("upscaling: %s (FSR sharpness %.2f%s)", g_upscaleCR == 2 ? "FSR" : g_upscaleCR ? "Catmull-Rom" : "bilinear", g_rcasSharp, g_fsrFinal ? ", EASU final" : "");
	g_statsOn = GetPrivateProfileIntA("Display", "FpsCounter", 0, g_iniPath);
	if (g_statsOn < 0 || g_statsOn > 2) g_statsOn = 0;
	if (GetPrivateProfileIntA("Debug", "Stats", 0, g_iniPath)) g_statsOn = 2;
	g_miniPop = GetPrivateProfileIntA("Display", "MinimapPop", 0, g_iniPath) != 0;
	g_perfLogOn = GetPrivateProfileIntA("Debug", "PerfLog", 0, g_iniPath) != 0;
	g_frameLimit = GetPrivateProfileIntA("Display", "FrameLimit", 120, g_iniPath);
	if (g_frameLimit < 0) g_frameLimit = 0;
	logf_("frame limit: %d game frames a second%s", g_frameLimit, g_frameLimit ? "" : " (off: as fast as the CPU allows)");
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
		// the stack's top words: after a call through a null pointer the first is the caller's return address
		const DWORD *sp = (const DWORD *)ep->ContextRecord->Esp;
		MEMORY_BASIC_INFORMATION mi;
		if (VirtualQuery(sp, &mi, sizeof mi) && mi.State == MEM_COMMIT && (BYTE *)sp + 32 <= (BYTE *)mi.BaseAddress + mi.RegionSize) {
			HMODULE rm = NULL; char rn[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)sp[0], &rm))
				GetModuleFileNameA(rm, rn, sizeof rn);
			logf_("  return address %08lx: %s +%lx", sp[0], rn, (unsigned long)(sp[0] - (DWORD)rm));
			logf_("  stack %p: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx (eax %08lx ecx %08lx edx %08lx)", (void *)sp,
				sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7],
				ep->ContextRecord->Eax, ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------------
// Shared frame state

static CRITICAL_SECTION g_lock;   // guards the primary surface's pixels and the palette
// Held by the presenter from reading the frame until its picture is built from the HD layers' copies
// (*_publish): a copy to the screen in between moved the layers on a frame (a scrolling battle showed
// the whole field at 1x for a picture). Copies wait for it; the game's unforced ones are skipped.
static CRITICAL_SECTION g_composeLock;
static volatile LONG g_composing;
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

static volatile LONG g_dirtyCount; // Timing=1: frame copies a second
static volatile LONG g_frameTaken = 1; // the presenter has read the frame since the last copy (h_show_frame)
static HANDLE g_turnEvent; // set with g_frameTaken: wakes the frame limiter's wait to copy a finished frame at once (pending_copy)
static int g_frameSteps;               // Timing=1: logic steps since the last frame copy
static volatile LONG g_presentCount;   // pictures shown (Timing=1)
static double g_presentEma = 6.0;      // presenter: ms a new picture takes, averaged
static volatile LONG g_copySeq;        // every copy to the screen (h_show_frame: did a forced call copy?)
static volatile LONG g_copyPending;    // the game's last finished frame was not copied (pending_copy)
static volatile double g_frameCopyT;   // when the game last copied a whole frame to the screen
static int g_pendingCopies;            // Timing=1: copies made from the frame limiter's wait
static void mark_dirty(void)
{
	InterlockedIncrement(&g_dirtyCount);
	InterlockedIncrement(&g_copySeq);
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

// ---- Catmull-Rom resampling ([Display] Upscale=catmullrom, the default) --------------------------
// A sharper alternative to bilinear doubling and GDI's stretch, for the 2x step (1x frame -> 2x frame
// the HD art is stamped into) and the final step (2x frame -> the window). Separable: a horizontal
// pass into 16-bit rows (value * 64), then a vertical pass in SSE2 (pmulhw with Q10 weights gives the
// value back). The output is cut into bands of 32 rows; worker threads and the caller take bands in
// turn, each with its own strip of horizontal rows, so memory stays small at 5K.
#include <emmintrin.h>
#define CR_MAXTAPS 12
typedef struct { int n, taps; int *idx; short *w; } cr_axis; // per output index: taps source indexes and weights
static int g_upscaleCR = 1;                   // 1 Catmull-Rom, 0 bilinear / GDI (as up to 1.1.1), 2 FSR (Catmull-Rom's final stretch, then RCAS)
static double cr_k(double t) { t = fabs(t); return t < 1 ? 1.5 * t * t * t - 2.5 * t * t + 1 : t < 2 ? -0.5 * t * t * t + 2.5 * t * t - 4 * t + 2 : 0; }
// weights for src length sn -> dst length dn, fixed point "one" (256 or 1024)
static BOOL cr_axis_make(cr_axis *a, int sn, int dn, int one)
{
	double s = (double)dn / sn, k = s < 1 ? s : 1; // shrinking: the kernel widens
	int taps = s < 1 ? (int)ceil(4 / s) : 4;
	if (taps > CR_MAXTAPS) taps = CR_MAXTAPS;
	taps = (taps + 1) & ~1; // even, for the paired SSE2 horizontal pass (an extra tap weighs 0)
	free(a->idx); free(a->w);
	a->idx = malloc(sizeof(int) * dn * taps);
	a->w = malloc(sizeof(short) * dn * taps);
	if (!a->idx || !a->w) return FALSE;
	a->n = dn; a->taps = taps;
	for (int o = 0; o < dn; o++) {
		double c = (o + 0.5) / s - 0.5;
		int first = (int)floor(c) - taps / 2 + 1;
		double wt[CR_MAXTAPS], sum = 0;
		for (int t = 0; t < taps; t++) { wt[t] = cr_k((c - (first + t)) * k); sum += wt[t]; }
		int isum = 0, big = 0;
		for (int t = 0; t < taps; t++) {
			int i = first + t;
			a->idx[o * taps + t] = i < 0 ? 0 : i >= sn ? sn - 1 : i;
			int w = (int)floor(wt[t] / sum * one + 0.5);
			a->w[o * taps + t] = (short)w;
			isum += w;
			if (abs(w) > abs(a->w[o * taps + big])) big = t;
		}
		a->w[o * taps + big] += (short)(one - isum); // exactly "one" in total
	}
	return TRUE;
}
static struct {
	const DWORD *src; int sw, sh, sstride; DWORD *dst; int dw, dh, dstride;
	cr_axis hx, vy; int hsw, hdw, vsh, vdh;    // the tables, kept while the sizes stay
	volatile LONG next; int bands;
	HANDLE go[16], idle[16]; int threads;
	short *strip[17]; int stripRows;           // per worker (and the caller) a strip of horizontal rows
	void (*job)(int band, int me);             // what the workers do with each band (cr_job, easu_job, rcas_job)
} g_cr;
#define CR_BAND 32
// one source row -> 16-bit row of dw pixels (value * 64). Taps in pairs: the two pixels' channels
// interleaved as 16-bit and pmaddwd'ed with the two weights gives four 32-bit sums at once.
__attribute__((target("sse2")))
static void cr_hrow(const DWORD *s, short *o)
{
	int taps = g_cr.hx.taps;
	const __m128i zero = _mm_setzero_si128();
	for (int x = 0; x < g_cr.dw; x++) {
		const int *ix = g_cr.hx.idx + x * taps; const short *w = g_cr.hx.w + x * taps;
		__m128i acc = zero;
		for (int t = 0; t < taps; t += 2) {
			__m128i a = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)s[ix[t]]), zero);     // a0 a1 a2 a3 (16-bit)
			__m128i b = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)s[ix[t + 1]]), zero); // b0 b1 b2 b3
			__m128i ab = _mm_unpacklo_epi16(a, b);                                      // a0 b0 a1 b1 a2 b2 a3 b3
			__m128i wt = _mm_set1_epi32((int)(unsigned short)w[t] | ((int)w[t + 1] << 16));
			acc = _mm_add_epi32(acc, _mm_madd_epi16(ab, wt));
		}
		acc = _mm_srai_epi32(acc, 2);
		_mm_storel_epi64((__m128i *)(o + 4 * x), _mm_packs_epi32(acc, acc));
	}
}
__attribute__((target("sse2")))
static void cr_band(int band, short *strip)
{
	int y0 = band * CR_BAND, y1 = min(g_cr.dh, y0 + CR_BAND), taps = g_cr.vy.taps;
	int r0 = g_cr.vy.idx[y0 * taps], r1 = g_cr.vy.idx[(y1 - 1) * taps + taps - 1]; // source rows this band needs
	int dw = g_cr.dw;
	for (int r = r0; r <= r1; r++) cr_hrow(g_cr.src + (size_t)r * g_cr.sstride, strip + (size_t)(r - r0) * dw * 4);
	const __m128i bias = _mm_set1_epi16(2);
	for (int y = y0; y < y1; y++) {
		const int *iy = g_cr.vy.idx + y * taps; const short *w = g_cr.vy.w + y * taps;
		DWORD *out = g_cr.dst + (size_t)y * g_cr.dstride;
		int x = 0;
		for (; x + 4 <= dw; x += 4) {
			__m128i a0 = bias, a1 = bias;
			for (int t = 0; t < taps; t++) {
				const short *row = strip + (size_t)(iy[t] - r0) * dw * 4 + 4 * x;
				__m128i wt = _mm_set1_epi16(w[t]);
				a0 = _mm_add_epi16(a0, _mm_mulhi_epi16(_mm_loadu_si128((const __m128i *)row), wt));
				a1 = _mm_add_epi16(a1, _mm_mulhi_epi16(_mm_loadu_si128((const __m128i *)(row + 8)), wt));
			}
			_mm_storeu_si128((__m128i *)(out + x), _mm_packus_epi16(a0, a1));
		}
		for (; x < dw; x++) {
			int c[3] = { 2, 2, 2 };
			for (int t = 0; t < taps; t++) { const short *p = strip + (size_t)(iy[t] - r0) * dw * 4 + 4 * x; for (int k = 0; k < 3; k++) c[k] += (p[k] * w[t]) >> 16; }
			for (int k = 0; k < 3; k++) c[k] = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
			out[x] = c[0] | (c[1] << 8) | (c[2] << 16);
		}
	}
}
static void cr_work(int me)
{
	for (;;) {
		LONG b = InterlockedIncrement(&g_cr.next) - 1;
		if (b >= g_cr.bands) return;
		g_cr.job(b, me);
	}
}
static void cr_job(int band, int me) { cr_band(band, g_cr.strip[me]); }
static DWORD WINAPI cr_thread(void *arg)
{
	int me = (int)(INT_PTR)arg;
	for (;;) {
		WaitForSingleObject(g_cr.go[me], INFINITE);
		cr_work(me);
		SetEvent(g_cr.idle[me]);
	}
	return 0;
}
// The worker threads, started once: one fewer than the CPU's threads, the caller being the last.
static void pool_start(void)
{
	if (!g_cr.threads) {
		SYSTEM_INFO si; GetSystemInfo(&si);
		int n = (int)si.dwNumberOfProcessors - 1;
		n = n < 0 ? 0 : n > 15 ? 15 : n;
		for (int i = 0; i < n; i++) {
			g_cr.go[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
			g_cr.idle[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
			HANDLE t = CreateThread(NULL, 0, cr_thread, (void *)(INT_PTR)i, 0, NULL);
			if (t) CloseHandle(t);
		}
		g_cr.threads = n + 1; // the caller works too, with the last strip
		logf_("scaling on %d threads", g_cr.threads);
	}
}
// Runs job over bands 0 .. bands-1 on all the threads; back when every band is done.
// (from the presenter and, for the battle's stamps, the game thread: one at a time, g_poolLock)
static CRITICAL_SECTION g_poolLock;
static void pool_run(int bands, void (*job)(int band, int me))
{
	static LONG init;
	if (InterlockedCompareExchange(&init, 1, 0) == 0) { InitializeCriticalSection(&g_poolLock); InterlockedExchange(&init, 2); }
	while (init != 2) Sleep(0);
	EnterCriticalSection(&g_poolLock);
	pool_start();
	g_cr.job = job;
	g_cr.bands = bands;
	g_cr.next = 0;
	int helpers = g_cr.threads - 1;
	for (int i = 0; i < helpers; i++) SetEvent(g_cr.go[i]);
	cr_work(g_cr.threads - 1);
	if (helpers) WaitForMultipleObjects(helpers, g_cr.idle, TRUE, INFINITE);
	LeaveCriticalSection(&g_poolLock);
}
// src (sw x sh, stride sstride pixels) -> dst (dw x dh, stride dstride), BGRX
static BOOL resize_cr(const DWORD *src, int sw, int sh, int sstride, DWORD *dst, int dw, int dh, int dstride)
{
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return FALSE;
	pool_start();
	if (g_cr.hsw != sw || g_cr.hdw != dw) { if (!cr_axis_make(&g_cr.hx, sw, dw, 256)) return FALSE; g_cr.hsw = sw; g_cr.hdw = dw; }
	if (g_cr.vsh != sh || g_cr.vdh != dh) { if (!cr_axis_make(&g_cr.vy, sh, dh, 1024)) return FALSE; g_cr.vsh = sh; g_cr.vdh = dh; }
	int rows = (int)ceil(CR_BAND * (double)sh / dh) + g_cr.vy.taps + 2;
	if (rows * dw > g_cr.stripRows) {
		for (int i = 0; i < g_cr.threads; i++) { free(g_cr.strip[i]); g_cr.strip[i] = malloc((size_t)rows * dw * 8); if (!g_cr.strip[i]) { g_cr.stripRows = 0; return FALSE; } }
		g_cr.stripRows = rows * dw;
	}
	g_cr.src = src; g_cr.sw = sw; g_cr.sh = sh; g_cr.sstride = sstride;
	g_cr.dst = dst; g_cr.dw = dw; g_cr.dh = dh; g_cr.dstride = dstride;
	pool_run((dh + CR_BAND - 1) / CR_BAND, cr_job);
	return TRUE;
}

// ---- AMD FidelityFX Super Resolution 1 ([Display] Upscale=fsr) -----------------------------------
// A CPU port of FSR 1.0's two passes (ffx_fsr1.h, MIT licence, (c) Advanced Micro Devices):
// EASU, an edge-adaptive upscale (a Lanczos-like 12-tap kernel stretched along the local edge, the
// result held within the four nearest pixels so it never rings), and RCAS, a sharpening pass at the
// output size that backs off where it would clip or where the area is noise. EASU does the 2x step
// (the game's own pixels -> the frame the HD art is stamped into); the final stretch to the window
// stays Catmull-Rom (EASU per pixel at 5K would cost too much on the CPU, see FsrFinal) and RCAS
// sharpens the result. Colours stay in the frame's gamma space, as the shader expects.
static int g_fsrFinal;              // [Display] FsrFinal=1: EASU for the final stretch too (benchmarks)
static float g_rcasSharp = 0.2f;    // [Display] FsrSharpness: stops, 0 = sharpest, 2 = little; below 0 no RCAS
static struct {
	const DWORD *src; int sw, sh, sstride; DWORD *dst; int dw, dh, dstride;
	float *lin; int linCap;            // the source as r, g, b, luma floats (0 .. 1; luma = r/2 + g + b/2)
	float sx, sy;                      // source pixels per output pixel
	float scale;                       // RCAS: exp2(-sharpness)
} g_fsr;
#define FSR_BAND 16
static void easu_prep_job(int band, int me)
{
	(void)me;
	int y0 = band * FSR_BAND, y1 = min(g_fsr.sh, y0 + FSR_BAND);
	for (int y = y0; y < y1; y++) {
		const DWORD *s = g_fsr.src + (size_t)y * g_fsr.sstride;
		float *o = g_fsr.lin + (size_t)y * g_fsr.sw * 4;
		for (int x = 0; x < g_fsr.sw; x++, o += 4) {
			DWORD p = s[x];
			float r = ((p >> 16) & 255) * (1.0f / 255), g = ((p >> 8) & 255) * (1.0f / 255), b = (p & 255) * (1.0f / 255);
			o[0] = r; o[1] = g; o[2] = b; o[3] = 0.5f * r + g + 0.5f * b;
		}
	}
}
// direction and edge length from one of the four nearest pixels (c), weighted w; a above, b left,
// d right, e below (FsrEasuSetF)
static inline void easu_set(float *dx, float *dy, float *len, float w, float la, float lb, float lc, float ld, float le)
{
	float dc = ld - lc, cb = lc - lb, lenX = fmaxf(fabsf(dc), fabsf(cb));
	float dirX = ld - lb;
	lenX = lenX > 0 ? fminf(fabsf(dirX) / lenX, 1.0f) : 0;
	*dx += dirX * w;
	*len += lenX * lenX * w;
	float ec = le - lc, ca = lc - la, lenY = fmaxf(fabsf(ec), fabsf(ca));
	float dirY = le - la;
	lenY = lenY > 0 ? fminf(fabsf(dirY) / lenY, 1.0f) : 0;
	*dy += dirY * w;
	*len += lenY * lenY * w;
}
static inline void easu_tap(float *ac, float *aw, float ox, float oy, float dx, float dy, float l2x, float l2y, float lob, float clp, const float *c)
{
	float vx = (ox * dx + oy * dy) * l2x, vy = (ox * -dy + oy * dx) * l2y;
	float d2 = fminf(vx * vx + vy * vy, clp);
	float wb = 0.4f * d2 - 1.0f, wa = lob * d2 - 1.0f;
	wb *= wb; wa *= wa;
	float w = (25.0f / 16.0f * wb - (25.0f / 16.0f - 1.0f)) * wa;
	*aw += w;
	ac[0] += c[0] * w; ac[1] += c[1] * w; ac[2] += c[2] * w;
}
static void easu_job(int band, int me)
{
	(void)me;
	int y0 = band * FSR_BAND, y1 = min(g_fsr.dh, y0 + FSR_BAND), sw = g_fsr.sw, sh = g_fsr.sh;
	const float *L = g_fsr.lin;
	for (int y = y0; y < y1; y++) {
		float py = (y + 0.5f) * g_fsr.sy - 0.5f, fy = floorf(py);
		int iy = (int)fy; py -= fy;
		int ry[4];                                      // rows -1 .. +2 around the pixel, clamped
		for (int k = 0; k < 4; k++) { int r = iy - 1 + k; ry[k] = r < 0 ? 0 : r >= sh ? sh - 1 : r; }
		DWORD *out = g_fsr.dst + (size_t)y * g_fsr.dstride;
		for (int x = 0; x < g_fsr.dw; x++) {
			float px = (x + 0.5f) * g_fsr.sx - 0.5f, fx = floorf(px);
			int ix = (int)fx; px -= fx;
			int cx[4];
			for (int k = 0; k < 4; k++) { int c = ix - 1 + k; cx[k] = c < 0 ? 0 : c >= sw ? sw - 1 : c; }
#define EP(col, row) (L + ((size_t)ry[row] * sw + cx[col]) * 4)
			//    b c        columns 0..3 = x-1 .. x+2, rows 0..3 = y-1 .. y+2
			//  e f g h
			//  i j k l
			//    n o
			const float *b = EP(1, 0), *c = EP(2, 0), *e = EP(0, 1), *f = EP(1, 1), *g = EP(2, 1), *h = EP(3, 1);
			const float *i = EP(0, 2), *j = EP(1, 2), *k = EP(2, 2), *l = EP(3, 2), *n = EP(1, 3), *o = EP(2, 3);
#undef EP
			float dx = 0, dy = 0, len = 0;
			easu_set(&dx, &dy, &len, (1 - px) * (1 - py), b[3], e[3], f[3], g[3], j[3]);
			easu_set(&dx, &dy, &len, px * (1 - py), c[3], f[3], g[3], h[3], k[3]);
			easu_set(&dx, &dy, &len, (1 - px) * py, f[3], i[3], j[3], k[3], n[3]);
			easu_set(&dx, &dy, &len, px * py, g[3], j[3], k[3], l[3], o[3]);
			float dr = dx * dx + dy * dy;
			if (dr < 1.0f / 32768) { dx = 1; dr = 1; } else dr = 1.0f / sqrtf(dr);
			dx *= dr; dy *= dr;
			len *= 0.5f; len *= len;
			float stretch = (dx * dx + dy * dy) / fmaxf(fabsf(dx), fabsf(dy));
			float l2x = 1 + (stretch - 1) * len, l2y = 1 - 0.5f * len;
			float lob = 0.5f + (0.25f - 0.04f - 0.5f) * len, clp = 1.0f / lob;
			float ac[3] = { 0, 0, 0 }, aw = 0;
			easu_tap(ac, &aw, 0 - px, -1 - py, dx, dy, l2x, l2y, lob, clp, b);
			easu_tap(ac, &aw, 1 - px, -1 - py, dx, dy, l2x, l2y, lob, clp, c);
			easu_tap(ac, &aw, -1 - px, 1 - py, dx, dy, l2x, l2y, lob, clp, i);
			easu_tap(ac, &aw, 0 - px, 1 - py, dx, dy, l2x, l2y, lob, clp, j);
			easu_tap(ac, &aw, 0 - px, 0 - py, dx, dy, l2x, l2y, lob, clp, f);
			easu_tap(ac, &aw, -1 - px, 0 - py, dx, dy, l2x, l2y, lob, clp, e);
			easu_tap(ac, &aw, 1 - px, 1 - py, dx, dy, l2x, l2y, lob, clp, k);
			easu_tap(ac, &aw, 2 - px, 1 - py, dx, dy, l2x, l2y, lob, clp, l);
			easu_tap(ac, &aw, 2 - px, 0 - py, dx, dy, l2x, l2y, lob, clp, h);
			easu_tap(ac, &aw, 1 - px, 0 - py, dx, dy, l2x, l2y, lob, clp, g);
			easu_tap(ac, &aw, 1 - px, 2 - py, dx, dy, l2x, l2y, lob, clp, o);
			easu_tap(ac, &aw, 0 - px, 2 - py, dx, dy, l2x, l2y, lob, clp, n);
			float rw = aw != 0 ? 1.0f / aw : 0, rgb[3];
			for (int q = 0; q < 3; q++) {
				float mn = fminf(fminf(f[q], g[q]), fminf(j[q], k[q])), mx = fmaxf(fmaxf(f[q], g[q]), fmaxf(j[q], k[q]));
				float v = ac[q] * rw;
				v = v < mn ? mn : v > mx ? mx : v;      // dering: within the four nearest
				rgb[q] = v * 255 + 0.5f;
			}
			out[x] = ((DWORD)rgb[0] << 16) | ((DWORD)rgb[1] << 8) | (DWORD)rgb[2];
		}
	}
}
// src (sw x sh) -> dst (dw x dh), BGRX, both with strides in pixels
static BOOL fsr_easu(const DWORD *src, int sw, int sh, int sstride, DWORD *dst, int dw, int dh, int dstride)
{
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return FALSE;
	if (sw * sh > g_fsr.linCap) {
		free(g_fsr.lin);
		g_fsr.lin = malloc((size_t)sw * sh * 4 * sizeof(float));
		g_fsr.linCap = g_fsr.lin ? sw * sh : 0;
		if (!g_fsr.lin) return FALSE;
	}
	g_fsr.src = src; g_fsr.sw = sw; g_fsr.sh = sh; g_fsr.sstride = sstride;
	g_fsr.dst = dst; g_fsr.dw = dw; g_fsr.dh = dh; g_fsr.dstride = dstride;
	g_fsr.sx = (float)sw / dw; g_fsr.sy = (float)sh / dh;
	pool_run((sh + FSR_BAND - 1) / FSR_BAND, easu_prep_job);
	pool_run((dh + FSR_BAND - 1) / FSR_BAND, easu_job);
	return TRUE;
}
// RCAS, four pixels at a time in SSE2: each pixel from itself and the four around it (b above,
// d left, f right, h below), as FsrRcasF with its noise removal (FSR_RCAS_DENOISE).
__attribute__((target("sse2")))
static inline void rcas_split(__m128i p, __m128 *r, __m128 *g, __m128 *b)
{
	const __m128i m = _mm_set1_epi32(255);
	const __m128 k = _mm_set1_ps(1.0f / 255);
	*r = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(p, 16), m)), k);
	*g = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(p, 8), m)), k);
	*b = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(p, m)), k);
}
__attribute__((target("sse2")))
static inline __m128 rcas_lobe(__m128 bR, __m128 dR, __m128 eR, __m128 fR, __m128 hR)
{
	__m128 mn = _mm_min_ps(_mm_min_ps(bR, dR), _mm_min_ps(fR, hR));
	__m128 mx = _mm_max_ps(_mm_max_ps(bR, dR), _mm_max_ps(fR, hR));
	const __m128 four = _mm_set1_ps(4.0f), one = _mm_set1_ps(1.0f), tiny = _mm_set1_ps(1.0f / 4096);
	__m128 hitMin = _mm_div_ps(_mm_min_ps(mn, eR), _mm_add_ps(_mm_mul_ps(four, mx), tiny));
	__m128 hitMax = _mm_div_ps(_mm_sub_ps(one, _mm_max_ps(mx, eR)), _mm_sub_ps(_mm_mul_ps(four, mn), _mm_add_ps(four, tiny)));
	return _mm_max_ps(_mm_sub_ps(_mm_setzero_ps(), hitMin), hitMax);
}
__attribute__((target("sse2")))
static void rcas_job(int band, int me)
{
	(void)me;
	int y0 = band * FSR_BAND, y1 = min(g_fsr.dh, y0 + FSR_BAND), w = g_fsr.dw;
	const __m128 half = _mm_set1_ps(0.5f), quarter = _mm_set1_ps(0.25f), one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
	const __m128 limit = _mm_set1_ps(-(0.25f - 1.0f / 16)), scale = _mm_set1_ps(g_fsr.scale), k255 = _mm_set1_ps(255.0f), tiny = _mm_set1_ps(1.0f / 4096);
	for (int y = y0; y < y1; y++) {
		const DWORD *row = g_fsr.src + (size_t)y * g_fsr.sstride;
		const DWORD *up = g_fsr.src + (size_t)(y > 0 ? y - 1 : y) * g_fsr.sstride;
		const DWORD *dn = g_fsr.src + (size_t)(y < g_fsr.dh - 1 ? y + 1 : y) * g_fsr.sstride;
		DWORD *out = g_fsr.dst + (size_t)y * g_fsr.dstride;
		out[0] = row[0];                       // the outermost columns as they are
		out[w - 1] = row[w - 1];
		int x = 1;
		for (; x + 4 <= w - 1; x += 4) {
			__m128 bR, bG, bB, dR, dG, dB, eR, eG, eB, fR, fG, fB, hR, hG, hB;
			rcas_split(_mm_loadu_si128((const __m128i *)(up + x)), &bR, &bG, &bB);
			rcas_split(_mm_loadu_si128((const __m128i *)(row + x - 1)), &dR, &dG, &dB);
			rcas_split(_mm_loadu_si128((const __m128i *)(row + x)), &eR, &eG, &eB);
			rcas_split(_mm_loadu_si128((const __m128i *)(row + x + 1)), &fR, &fG, &fB);
			rcas_split(_mm_loadu_si128((const __m128i *)(dn + x)), &hR, &hG, &hB);
#define LUMA(R, G, B) _mm_add_ps(_mm_mul_ps(_mm_add_ps(R, B), half), G)
			__m128 bL = LUMA(bR, bG, bB), dL = LUMA(dR, dG, dB), eL = LUMA(eR, eG, eB), fL = LUMA(fR, fG, fB), hL = LUMA(hR, hG, hB);
#undef LUMA
			__m128 nz = _mm_sub_ps(_mm_mul_ps(quarter, _mm_add_ps(_mm_add_ps(bL, dL), _mm_add_ps(fL, hL))), eL);
			__m128 lmx = _mm_max_ps(_mm_max_ps(_mm_max_ps(bL, dL), eL), _mm_max_ps(fL, hL));
			__m128 lmn = _mm_min_ps(_mm_min_ps(_mm_min_ps(bL, dL), eL), _mm_min_ps(fL, hL));
			nz = _mm_div_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), nz), _mm_add_ps(_mm_sub_ps(lmx, lmn), tiny));
			nz = _mm_min_ps(nz, one);
			nz = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(-0.5f), nz), one);
			__m128 lobe = _mm_max_ps(_mm_max_ps(rcas_lobe(bR, dR, eR, fR, hR), rcas_lobe(bG, dG, eG, fG, hG)), rcas_lobe(bB, dB, eB, fB, hB));
			lobe = _mm_mul_ps(_mm_max_ps(limit, _mm_min_ps(lobe, zero)), scale);
			lobe = _mm_mul_ps(lobe, nz);
			__m128 rcp = _mm_div_ps(one, _mm_add_ps(_mm_mul_ps(_mm_set1_ps(4.0f), lobe), one));
#define RES(b_, d_, e_, f_, h_) _mm_cvtps_epi32(_mm_min_ps(_mm_max_ps(_mm_mul_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(lobe, _mm_add_ps(_mm_add_ps(b_, d_), _mm_add_ps(f_, h_))), e_), rcp), k255), zero), k255))
			__m128i r = RES(bR, dR, eR, fR, hR), g = RES(bG, dG, eG, fG, hG), bb = RES(bB, dB, eB, fB, hB);
#undef RES
			__m128i px = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(r, 16), _mm_slli_epi32(g, 8)), bb);
			_mm_storeu_si128((__m128i *)(out + x), px);
		}
		for (; x < w - 1; x++) out[x] = row[x];
	}
}
// src -> dst at the same size (dw x dh), sharpened; dst must not be src
static BOOL fsr_rcas(const DWORD *src, int sstride, DWORD *dst, int dw, int dh, int dstride)
{
	if (dw < 8 || dh < 1) return FALSE;
	g_fsr.src = src; g_fsr.sstride = sstride; g_fsr.dst = dst; g_fsr.dw = dw; g_fsr.dh = dh; g_fsr.dstride = dstride;
	g_fsr.scale = exp2f(-g_rcasSharp);
	pool_run((dh + FSR_BAND - 1) / FSR_BAND, rcas_job);
	return TRUE;
}

// ---- Frame timing ([Debug] Stats=1 or Ctrl+F10 on screen; [Debug] PerfLog=1 to lords2hd-perf.csv) --
// Each picture shown is timed in stages: compose (the game's frame read, coloured, doubled and the HD
// art stamped), scale (to the window), sharpen (RCAS) and blit (to the window). The presenter keeps
// the last 256; the overlay shows the last second. Latency: from a click or key to the end of the
// next picture that differs (the game's reaction plus ours; the compositor then shows it at its next
// refresh, which is added as an estimate from DWM's timing). Remote message 22 benchmarks: n
// pictures composed and shown back to back, whatever changed, the averages written to the log.
enum { PS_COMPOSE, PS_SCALE, PS_SHARPEN, PS_BLIT, PS_N };
#define PERF_HIST 256
static struct {
	FILE *csv;
	LARGE_INTEGER qpf;
	double t0, mark, st[PS_N];          // this picture: its start, the last mark, its stages
	struct { double t, interval, st[PS_N], total; int rep; } h[PERF_HIST]; // rep: a repeat (gpu_repeat)
	double lastAny;                     // the last picture handed to the screen, new or repeat
	int hn, hi;                         // entries, next slot
	double lastShown;
	volatile LONG inputPending; double tInput;
	double lat[16], latLast, latScreen; int latN, latI;
	double refresh;                     // ms per display refresh (DWM)
	volatile LONG bench; int benchN, benchDone; double benchSt[PS_N], benchTotal, benchWorst;
} g_perf;
static double perf_now(void)
{
	LARGE_INTEGER t;
	if (!g_perf.qpf.QuadPart) QueryPerformanceFrequency(&g_perf.qpf);
	QueryPerformanceCounter(&t);
	return t.QuadPart * 1000.0 / g_perf.qpf.QuadPart;
}
// Timing=1: the map picture's steps (double, sprites, tiles, letters, scrolls), summed per 5 s
static double g_ct[8], g_ctWorst[8]; static int g_ctN;
static void ct_add(int k, double *t) { double n = perf_now(), d = n - *t; g_ct[k] += d; if (d > g_ctWorst[k]) g_ctWorst[k] = d; *t = n; if (!k) g_ctN++; }
// ---- Frame pacing check (Timing=1) ----------------------------------------------------------------
// Every refresh of the display is one of: new (a picture not shown before reached it), idle (nothing
// newer existed) or late (the game had a changed picture at least one refresh before, and it did not
// reach this refresh). "Locked 60" means late = 0. The game thread hashes its whole frame at each copy
// (pace_change), each copy carries the newest change's number to the screen surface, and the
// presenter notes which number each picture it shows carried. Logged per second, with the first late
// refreshes in detail: when the change came, and what the presenter did in that refresh.
static volatile LONG g_paceChangeSeq, g_paceCopying, g_pacePrimarySeq;
static LONG g_paceReadSeq; // the number of the frame the presenter last read
#define PACE_RING 1024
static struct { double t; LONG seq; } g_paceChg[PACE_RING];
static volatile LONG g_paceChgN;
static struct { double t; LONG seq; int shown; double present; } g_paceWake[PACE_RING]; // presenter reads: shown or unchanged; present: ms the hand-off took
static LONG g_paceWakeN;
static void pace_change(void)
{
	LONG s = InterlockedIncrement(&g_paceChangeSeq);
	int i = g_paceChgN % PACE_RING;
	g_paceChg[i].t = perf_now(); g_paceChg[i].seq = s;
	InterlockedIncrement(&g_paceChgN);
}
// Timing=1: both threads' events, for the late-refresh trace: T turn given, H changed frame finished,
// C copied, K skipped (composing), N no turn, P copied from the wait
static struct { double t; char k; LONG seq; } g_paceEv[PACE_RING];
static volatile LONG g_paceEvN;
static void pace_event(char k, LONG seq)
{
	if (!g_timing) return;
	LONG n = InterlockedIncrement(&g_paceEvN) - 1;
	int i = n % PACE_RING;
	g_paceEv[i].t = perf_now(); g_paceEv[i].k = k; g_paceEv[i].seq = seq;
}
// The game may copy its next frame (h_show_frame, pending_copy); a finished frame waiting in the frame
// limiter is copied at once (it slept in 2 ms slices and came 3-5 ms into the refresh, too late for it).
static void give_turn(void)
{
	InterlockedExchange(&g_frameTaken, 1); pace_event('T', 0);
	if (g_turnEvent) SetEvent(g_turnEvent);
}
// While a picture waits for its refresh, every frame the game finishes is copied, each replacing the
// last, until the next picture is started (g_preTurn cleared): a busy battle picture takes ~8 ms, so the
// frame must be in before the refresh, and on the map a copy that used up the turn early left the newer
// frame finished just before the refresh out (a refresh late).
static volatile LONG g_preTurn;
static void give_pre_turn(void)
{
	InterlockedExchange(&g_preTurn, 1); pace_event('T', 0);
	if (g_turnEvent) SetEvent(g_turnEvent);
}
static void pace_trace(double from, double to, double base)
{
	char w[900]; int wl = 0; w[0] = 0;
	LONG n = g_paceEvN;
	for (int k = PACE_RING - 1; k >= 0; k--) {
		if (k >= n) continue;
		int i = (n - 1 - k) % PACE_RING;
		if (g_paceEv[i].t < from || g_paceEv[i].t > to) continue;
		wl += snprintf(w + wl, sizeof w - wl, " %+.1f%c%ld", g_paceEv[i].t - base, g_paceEv[i].k, (long)g_paceEv[i].seq);
		if (wl > (int)sizeof w - 30) break;
	}
	logf_("pace:   events:%s", w);
}
static void pace_wake(int shown) // 0 read unchanged, 1 new picture shown, 2 a repeat
{
	int i = g_paceWakeN % PACE_RING;
	g_paceWake[i].t = perf_now(); g_paceWake[i].seq = g_paceReadSeq; g_paceWake[i].shown = shown;
	g_paceWake[i].present = g_perf.st[PS_BLIT];
	g_paceWakeN++;
}
static double perf_next_refresh(double t);
static void pace_classify(void)
{
	static double lastV, t0; static int nNew, nLate, nIdle, nEmpty, details; static LONG shownSeq;
	double now = perf_now(), vNext = perf_next_refresh(now), period = g_perf.refresh;
	if (period <= 0) return;
	double vNow = vNext - period; // the last refresh at or before now
	if (!lastV) { lastV = vNow; t0 = now; return; }
	for (double V = lastV + period; V <= vNow + period * 0.25; V += period) {
		LONG best = -1; BOOL any = FALSE; // any: a picture, new or repeated, reached this refresh
		for (int k = 0; k < PACE_RING && k < g_paceWakeN; k++) {
			int i = (g_paceWakeN - 1 - k) % PACE_RING;
			if (g_paceWake[i].t <= V - period) break;
			if (g_paceWake[i].t > V) continue;
			if (g_paceWake[i].shown == 1 || g_paceWake[i].shown == 2) any = TRUE; // (3: a refresh wait, not a picture)
			if (g_paceWake[i].shown == 1 && g_paceWake[i].seq > best) best = g_paceWake[i].seq;
		}
		if (!any) {
			nEmpty++;
			static int emptyDetails;
			if (emptyDetails < 40) { // what the presenter did in this refresh and the one before (times from the previous refresh)
				emptyDetails++;
				char w[600]; int wl = 0; w[0] = 0;
				for (int k = PACE_RING - 1; k >= 0; k--) {
					if (k >= g_paceWakeN) continue;
					int i = (g_paceWakeN - 1 - k) % PACE_RING;
					double dt = g_paceWake[i].t - (V - period);
					if (dt < -period || dt > period + 2) continue;
					wl += snprintf(w + wl, sizeof w - wl, " %+.1f%s(%.1f)", dt, g_paceWake[i].shown == 3 ? "W" : g_paceWake[i].shown == 2 ? "R" : g_paceWake[i].shown ? "S" : "u", g_paceWake[i].present);
					if (wl > (int)sizeof w - 30) break;
				}
				logf_("pace: empty refresh; presenter around it (ms from the refresh before, S new, R repeat, u unchanged, (hand-off ms)):%s", w[0] ? w : " nothing");
			}
		}
		if (best >= 0 && best != shownSeq) { nNew++; shownSeq = best; continue; }
		LONG latest = -1; double latestT = 0;
		LONG cn = g_paceChgN;
		for (int k = 0; k < PACE_RING && k < cn; k++) {
			int i = (cn - 1 - k) % PACE_RING;
			if (g_paceChg[i].t <= V - period) { latest = g_paceChg[i].seq; latestT = g_paceChg[i].t; break; }
		}
		if (latest > shownSeq) {
			nLate++;
			static double firstT; if (!firstT) firstT = now;
			if (details < 300 && now - firstT > 40000) { // what happened in this refresh (after the first 30 s: not the menus)
				details++;
				char w[400]; int wl = 0; w[0] = 0;
				for (int k = PACE_RING - 1; k >= 0; k--) {
					if (k >= g_paceWakeN) continue;
					int i = (g_paceWakeN - 1 - k) % PACE_RING;
					double dt = g_paceWake[i].t - (V - period);
					if (dt < 0 || dt > period) continue;
					wl += snprintf(w + wl, sizeof w - wl, " %.1f%s#%ld", dt, g_paceWake[i].shown == 3 ? "W" : g_paceWake[i].shown == 2 ? "R" : g_paceWake[i].shown ? "S" : "u", (long)g_paceWake[i].seq);
					if (wl > (int)sizeof w - 30) break;
				}
				logf_("pace: late refresh: change #%ld made %.1f ms before the previous refresh, shown #%ld; reads in it:%s", (long)latest, (V - period) - latestT, (long)shownSeq, w[0] ? w : " none");
				pace_trace(V - 2 * period, V, V - period);
			}
		}
		else nIdle++;
	}
	lastV = vNow;
	if (now - t0 >= 1000) {
		logf_("pace: 1 s: %d new, %d late, %d idle refreshes; %d got no picture at all", nNew, nLate, nIdle, nEmpty);
		nNew = nLate = nIdle = nEmpty = 0; t0 = now;
	}
}
static void perf_begin(void) { g_perf.t0 = g_perf.mark = perf_now(); memset(g_perf.st, 0, sizeof g_perf.st); }
static void perf_mark(int stage) { double t = perf_now(); g_perf.st[stage] += t - g_perf.mark; g_perf.mark = t; }
static void perf_input(void)
{
	if (InterlockedCompareExchange(&g_perf.inputPending, 1, 0) == 0) g_perf.tInput = perf_now();
}
static const char *upscale_name(void);
// The display's refresh period and the time of its next refresh after t (ms), from DWM.
static double perf_next_refresh(double t)
{
	static HRESULT (WINAPI *get)(HWND, DWM_TIMING_INFO *);
	static BOOL tried;
	if (!tried) { tried = TRUE; HMODULE d = LoadLibraryA("dwmapi.dll"); if (d) get = (void *)GetProcAddress(d, "DwmGetCompositionTimingInfo"); }
	DWM_TIMING_INFO ti; memset(&ti, 0, sizeof ti); ti.cbSize = sizeof ti;
	if (!get || FAILED(get(NULL, &ti)) || !ti.qpcRefreshPeriod) { g_perf.refresh = 0; return t; }
	double period = ti.qpcRefreshPeriod * 1000.0 / g_perf.qpf.QuadPart, vb = ti.qpcVBlank * 1000.0 / g_perf.qpf.QuadPart;
	g_perf.refresh = period;
	double k = ceil((t - vb) / period);
	return vb + (k < 0 ? 0 : k) * period;
}
// After a present: shown says whether a picture went to the window.
static void perf_end(BOOL shown, int vw, int vh)
{
	if (!shown) return;
	double t = perf_now(), total = t - g_perf.t0;
	if (g_perf.bench > 0) { // benchmark: only the sums
		for (int i = 0; i < PS_N; i++) g_perf.benchSt[i] += g_perf.st[i];
		g_perf.benchTotal += total;
		if (total > g_perf.benchWorst) g_perf.benchWorst = total;
		g_perf.benchDone++;
		if (InterlockedDecrement(&g_perf.bench) == 0) {
			int n = g_perf.benchDone ? g_perf.benchDone : 1;
			char line[300];
			BOOL gpu = gpu_drawing();
			snprintf(line, sizeof line, "bench: %d pictures, %s on the %s%s, %dx%d: compose %.2f, %s %.2f, %s %.2f, %s %.2f, total %.2f ms (worst %.1f)%s",
				g_perf.benchDone, upscale_name(), gpu ? "GPU" : "CPU", g_fsrFinal && !gpu ? " (EASU final)" : "", vw, vh, g_perf.benchSt[0] / n,
				gpu ? "upload" : "scale", g_perf.benchSt[1] / n, gpu ? "submit" : "sharpen", g_perf.benchSt[2] / n, gpu ? "present" : "blit",
				g_perf.benchSt[3] / n, g_perf.benchTotal / n, g_perf.benchWorst, "");
			if (gpu) snprintf(line + strlen(line), sizeof line - strlen(line), "; GPU %.2f ms", gpu_ms());
			logf_("%s", line);
		}
		return;
	}
	int slot = g_perf.hi;
	g_perf.h[slot].t = t;
	g_perf.h[slot].interval = g_perf.lastAny ? t - g_perf.lastAny : 0;
	memcpy(g_perf.h[slot].st, g_perf.st, sizeof g_perf.st);
	g_perf.h[slot].total = total;
	g_perf.h[slot].rep = 0;
	g_perf.hi = (slot + 1) % PERF_HIST;
	if (g_perf.hn < PERF_HIST) g_perf.hn++;
	g_perf.lastShown = g_perf.lastAny = t;
	double lat = -1, onScreen = -1;
	if (g_perf.inputPending && g_perf.t0 >= g_perf.tInput) { // a picture read after the input
		lat = t - g_perf.tInput;
		onScreen = perf_next_refresh(t) - g_perf.tInput;
		g_perf.latLast = lat; g_perf.latScreen = onScreen;
		g_perf.lat[g_perf.latI] = onScreen; g_perf.latI = (g_perf.latI + 1) % 16;
		if (g_perf.latN < 16) g_perf.latN++;
		InterlockedExchange(&g_perf.inputPending, 0);
	}
	if (g_perfLogOn) {
		if (!g_perf.csv) {
			char path[MAX_PATH];
			snprintf(path, sizeof path, "%s\\lords2hd-perf.csv", g_gameDir);
			g_perf.csv = fopen(path, "w");
			if (g_perf.csv) fprintf(g_perf.csv, "time_ms,interval_ms,compose_ms,scale_ms,sharpen_ms,blit_ms,total_ms,upscale,view_w,view_h,input_to_blit_ms,input_to_screen_ms\n");
		}
		if (g_perf.csv) {
			fprintf(g_perf.csv, "%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%d,%d,", t, g_perf.h[slot].interval, g_perf.st[0], g_perf.st[1], g_perf.st[2], g_perf.st[3], total, upscale_name(), vw, vh);
			if (lat >= 0) fprintf(g_perf.csv, "%.2f,%.2f\n", lat, onScreen); else fputs(",\n", g_perf.csv);
			static int lines;
			if (++lines % 30 == 0) fflush(g_perf.csv);
		}
	}
}
// After a repeat (gpu_repeat): counted as a refresh served, never as a new picture, a latency sample
// or a recorded frame.
static void perf_repeat(void)
{
	double t = perf_now();
	int slot = g_perf.hi;
	g_perf.h[slot].t = t;
	g_perf.h[slot].interval = g_perf.lastAny ? t - g_perf.lastAny : 0;
	memcpy(g_perf.h[slot].st, g_perf.st, sizeof g_perf.st);
	g_perf.h[slot].total = t - g_perf.t0;
	g_perf.h[slot].rep = 1;
	g_perf.hi = (slot + 1) % PERF_HIST;
	if (g_perf.hn < PERF_HIST) g_perf.hn++;
	g_perf.lastAny = t;
}
// The overlay, top left of the picture (drawn into the window-sized buffer before it is shown).
static void perf_draw(HDC dc, int vw, int vh)
{
	double now = perf_now(), st[PS_N] = { 0 }, tot = 0, worst = 0, ivs[PERF_HIST];
	int n = 0, nNew = 0; // n: pictures handed to the screen (new and repeats), nNew: new ones
	for (int k = 0; k < g_perf.hn; k++) {
		int i = (g_perf.hi - 1 - k + PERF_HIST) % PERF_HIST;
		if (now - g_perf.h[i].t > 1000) break;
		if (!g_perf.h[i].rep) {
			for (int s = 0; s < PS_N; s++) st[s] += g_perf.h[i].st[s];
			tot += g_perf.h[i].total;
			if (g_perf.h[i].total > worst) worst = g_perf.h[i].total;
			nNew++;
		}
		if (g_perf.h[i].interval > 0) ivs[n] = g_perf.h[i].interval;
		else ivs[n] = 0;
		n++;
	}
	BOOL repeats = g_repeatOn && gpu_drawing();
	// the slowest 1% of frame intervals (here the slowest of the last second)
	double ivMax = 0;
	for (int i = 0; i < n; i++) if (ivs[i] > ivMax) ivMax = ivs[i];
	double latAvg = 0;
	for (int i = 0; i < g_perf.latN; i++) latAvg += g_perf.lat[i];
	if (g_perf.latN) latAvg /= g_perf.latN;
	if (g_statsOn == 1) { // the simple counter: new pictures in the last second
		char s[32];
		BOOL still = !repeats && (!g_perf.lastShown || now - g_perf.lastShown > 500); // nothing moving: no new pictures to count (without repeats)
		if (still) snprintf(s, sizeof s, "still");
		else snprintf(s, sizeof s, "%d fps", n);
		int sfh = max(14, vh / 60);
		HFONT sf = CreateFontA(-sfh, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
		HGDIOBJ so = SelectObject(dc, sf);
		SIZE sz; GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
		int sp = sfh / 3;
		RECT sb = { sp, sp, sp + sz.cx + 2 * sp, sp + sz.cy + sp };
		HBRUSH sbg = CreateSolidBrush(RGB(16, 12, 8));
		FillRect(dc, &sb, sbg);
		DeleteObject(sbg);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, still ? RGB(150, 140, 120) : n >= 55 ? RGB(150, 230, 120) : n >= 30 ? RGB(240, 220, 160) : RGB(240, 140, 110));
		TextOutA(dc, sb.left + sp, sb.top + sp / 2, s, (int)strlen(s));
		SelectObject(dc, so);
		DeleteObject(sf);
		return;
	}
	char lines[5][160];
	int d = nNew ? nNew : 1;
	if (repeats) snprintf(lines[0], sizeof lines[0], "%d fps (%d new, %d repeated)   slowest gap %.1f ms", n, nNew, n - nNew, ivMax);
	else snprintf(lines[0], sizeof lines[0], "%d fps   frame %.1f ms   slowest gap %.1f ms", n, n ? 1000.0 / n : 0.0, ivMax);
	if (gpu_drawing())
		snprintf(lines[1], sizeof lines[1], "CPU %.1f ms (worst %.1f): compose %.1f  upload %.1f  submit %.1f  present %.1f   GPU %.2f ms",
			tot / d, worst, st[0] / d, st[1] / d, st[2] / d, st[3] / d, gpu_ms());
	else
		snprintf(lines[1], sizeof lines[1], "CPU %.1f ms (worst %.1f): compose %.1f  scale %.1f  sharpen %.1f  blit %.1f",
			tot / d, worst, st[0] / d, st[1] / d, st[2] / d, st[3] / d);
	if (g_perf.latN) snprintf(lines[2], sizeof lines[2], "input to screen %.0f ms (avg of %d: %.0f)   to blit %.0f ms", g_perf.latScreen, g_perf.latN, latAvg, g_perf.latLast);
	else snprintf(lines[2], sizeof lines[2], "input to screen: click or press a key");
	snprintf(lines[3], sizeof lines[3], "%s on the %s   %dx%d", upscale_name(), gpu_drawing() ? "GPU" : "CPU", vw, vh);
	if (g_perf.refresh > 0) snprintf(lines[3] + strlen(lines[3]), sizeof lines[3] - strlen(lines[3]), "   %.0f Hz", 1000.0 / g_perf.refresh);
	snprintf(lines[4], sizeof lines[4], "Ctrl+F10 hides this (F10, Performance: FPS counter)");
	int fh = max(14, vh / 70);
	HFONT font = CreateFontA(-fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	HGDIOBJ old = SelectObject(dc, font);
	int w = 0;
	for (int i = 0; i < 5; i++) { SIZE s; GetTextExtentPoint32A(dc, lines[i], (int)strlen(lines[i]), &s); if (s.cx > w) w = s.cx; }
	int pad = fh / 2, lh = fh + fh / 4;
	RECT box = { pad, pad, pad + w + 2 * pad, pad + 5 * lh + 2 * pad };
	HBRUSH bg = CreateSolidBrush(RGB(16, 12, 8));
	FillRect(dc, &box, bg);
	DeleteObject(bg);
	SetBkMode(dc, TRANSPARENT);
	for (int i = 0; i < 5; i++) {
		SetTextColor(dc, i == 4 ? RGB(150, 140, 120) : RGB(240, 220, 160));
		TextOutA(dc, box.left + pad, box.top + pad + i * lh, lines[i], (int)strlen(lines[i]));
	}
	SelectObject(dc, old);
	DeleteObject(font);
}

// Doubles the frame with bilinear interpolation: each output pixel sits a quarter of a source
// pixel from its nearest source pixel, so it is 9:3:3:1 of the four closest. With Catmull-Rom on,
// the same doubling with its weights (sharper, no blocks).
static void double_frame(int w)
{
	if (g_upscaleCR == 2 && !gpu_drawing() && fsr_easu(g_frame32, w, GAME_H, w, g_frame2x, 2 * w, 2 * GAME_H, 2 * w)) return;
	if (g_upscaleCR && resize_cr(g_frame32, w, GAME_H, w, g_frame2x, 2 * w, 2 * GAME_H, 2 * w)) return;
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
// Rows of the HD sprite, lettering and battle layers written since their last copy for the
// presenter (*_publish): a frame copy takes only those (all of every layer came to ~50 MB a copy,
// 3.5 ms of the game's frame, which slowed battles once every frame went to the screen).
#define DIRTY_ROWS 4096
static BYTE g_sprDirty[DIRTY_ROWS], g_txtDirty[DIRTY_ROWS], g_btDirty[DIRTY_ROWS];
static void rows_dirty(BYTE *d, int y0, int y1) { y0 = max(0, y0); y1 = min(DIRTY_ROWS, y1); if (y1 > y0) memset(d + y0, 1, y1 - y0); }
static struct { BOOL on, seen; BYTE shadow[GAME_W * GAME_H]; BYTE pal[768]; DWORD *px; } g_hdPic;
static BYTE *g_sprValid; // the HD sprite layer's, see stamp_sprite
static LONG g_layerGen;  // counts writes to the HD tile and sprite layers outside a map redraw (stamp_record)
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

typedef struct AFILE AFILE;
static size_t afread(void *buf, size_t sz, size_t n, AFILE *a);
static void keys_load(AFILE *f)
{
	for (int k = 0; k < g_keyCount; k++) {
		free(g_keys[k].bg); free(g_keys[k].mark); g_keys[k].bg = g_keys[k].mark = NULL;
		for (int f = 0; f < g_keys[k].frames; f++) { free(g_keys[k].pix[f]); free(g_keys[k].hd[f]); }
		g_keys[k].frames = 0;
	}
	g_keyCount = 0;
	int n;
	if (afread(&n, 4, 1, f) != 1) return;
	for (int k = 0; k < n && k < MAX_KEYS; k++) {
		int wh[2];
		if (afread(wh, 4, 2, f) != 2 || wh[0] <= 0 || wh[1] <= 0 || wh[0] > GAME_W || wh[1] > GAME_H) return;
		int size = wh[0] * wh[1];
		BYTE *bg = malloc(size), *mark = malloc(size);
		if (!bg || !mark || afread(bg, 1, size, f) != (size_t)size || afread(mark, 1, size, f) != (size_t)size) { free(bg); free(mark); return; }
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
		if (afread(&frames, 4, 1, f) != 1) return;
		for (int fr = 0; fr < frames && fr < MAX_KEY_FRAMES; fr++) {
			BYTE *pix = malloc(size);
			DWORD *hd = malloc(size * 4 * 4);
			if (!pix || !hd || afread(pix, 1, size, f) != (size_t)size || afread(hd, 4, size * 4, f) != (size_t)(size * 4)) {
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

// ---- The art pack (lords2hd.pak) -----------------------------------------------------------------
// The HD art and the restored voices ship as one file next to the game: a header (magic, count, index
// offset, version), the files' bytes one after another, and an index of their names, offsets and
// sizes. Plain, not scrambled: a packer rewriting every byte of a thousand files looked like
// ransomware to antivirus behaviour monitors. A loose file in lords2hd\ is read first, so the art can
// still be replaced by hand. Made by tools\l2pak.c.
#define PAK_MAGIC 0x50483243 // "C2HP"
#define PAK_VERSION 2        // 1 was scrambled: ignored
typedef struct { char name[48]; DWORD off, size; } PAK_ENTRY;
static PAK_ENTRY *g_pak;
static int g_pakN;
static char g_pakPath[MAX_PATH];
static void pak_open(void)
{
	snprintf(g_pakPath, sizeof g_pakPath, "%s\\lords2hd.pak", g_gameDir);
	HANDLE h = CreateFileA(g_pakPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD head[4], got = 0; // magic, count, index offset, version
	if (ReadFile(h, head, sizeof head, &got, NULL) && got == sizeof head && head[0] == PAK_MAGIC && head[3] == PAK_VERSION &&
		head[1] > 0 && head[1] < 100000) {
		PAK_ENTRY *e = malloc(head[1] * sizeof *e);
		LONG hi = 0;
		if (e && SetFilePointer(h, (LONG)head[2], &hi, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
			ReadFile(h, e, head[1] * sizeof *e, &got, NULL) && got == head[1] * sizeof *e) {
			for (DWORD i = 0; i < head[1]; i++) e[i].name[sizeof e[i].name - 1] = 0;
			g_pak = e; g_pakN = (int)head[1];
			logf_("art pack: %d files", g_pakN);
		}
		else free(e);
	}
	else if (got == sizeof head && head[0] == PAK_MAGIC) logf_("art pack: version %lu, not %d: ignored (make it again with tools\\make_pak.ps1)", head[3], PAK_VERSION);
	CloseHandle(h);
}// A file of the art: loose, or from the pack (read on demand through its own handle).
struct AFILE { FILE *f; HANDLE h; DWORD base, size, pos; };
static const PAK_ENTRY *pak_find(const char *path)
{
	char prefix[MAX_PATH];
	int n = snprintf(prefix, sizeof prefix, "%s\\lords2hd\\", g_gameDir);
	if (!g_pak || _strnicmp(path, prefix, n)) return NULL;
	char name[64];
	lstrcpynA(name, path + n, sizeof name);
	for (char *c = name; *c; c++) *c = *c == '\\' ? '/' : (char)tolower((BYTE)*c);
	for (int i = 0; i < g_pakN; i++) if (!strcmp(g_pak[i].name, name)) return &g_pak[i];
	return NULL;
}
static AFILE *afopen(const char *path)
{
	FILE *f = fopen(path, "rb");
	const PAK_ENTRY *e = f ? NULL : pak_find(path);
	if (!f && !e) return NULL;
	AFILE *a = calloc(1, sizeof *a);
	if (!a) { if (f) fclose(f); return NULL; }
	a->f = f;
	if (e) {
		a->h = CreateFileA(g_pakPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		if (a->h == INVALID_HANDLE_VALUE) { free(a); return NULL; }
		a->base = e->off; a->size = e->size;
	}
	return a;
}
static BOOL aexists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || pak_find(path); }
static size_t afread(void *buf, size_t sz, size_t n, AFILE *a)
{
	if (a->f) return fread(buf, sz, n, a->f);
	DWORD want = (DWORD)(sz * n), left = a->size - a->pos, got = 0;
	if (want > left) want = left - left % (DWORD)sz;
	LONG hi = 0;
	if (!want || SetFilePointer(a->h, (LONG)(a->base + a->pos), &hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
		!ReadFile(a->h, buf, want, &got, NULL)) return 0;
	a->pos += got;
	return got / sz;
}
static int afseek(AFILE *a, long off, int whence)
{
	if (a->f) return fseek(a->f, off, whence);
	long p = whence == SEEK_SET ? off : whence == SEEK_CUR ? (long)a->pos + off : (long)a->size + off;
	if (p < 0 || p > (long)a->size) return -1;
	a->pos = (DWORD)p;
	return 0;
}
static long aftell(AFILE *a) { return a->f ? ftell(a->f) : (long)a->pos; }
static void afclose(AFILE *a) { if (!a) return; if (a->f) fclose(a->f); if (a->h) CloseHandle(a->h); free(a); }

static void room_load(const char *base)
{
	char path[MAX_PATH];
	g_room.on = FALSE;
	snprintf(path, sizeof path, "%s\\lords2hd\\wide\\%s.bin", g_gameDir, base);
	AFILE *f = afopen(path);
	if (!f) return;
	int head[5];
	if (afread(head, 4, 5, f) == 5 && head[0] > GAME_W && head[0] <= 2 * GAME_W) {
		free(g_room.px); free(g_room.out);
		g_room.px = malloc(head[0] * 2 * GAME_H * 2 * 4);
		g_room.out = malloc(head[0] * 2 * GAME_H * 2 * 4);
		if (g_room.px && g_room.out && afread(g_room.px, 4, head[0] * 2 * GAME_H * 2, f) == (size_t)(head[0] * 2 * GAME_H * 2)) {
			g_room.w = head[0]; g_room.cutL = head[1]; g_room.cutR = head[2];
			g_room.floorY = head[3]; g_room.barY = head[4];
			g_room.on = TRUE;
			keys_load(f);
		}
	}
	afclose(f);
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
	// Some screens load their picture again every frame (the skirmish score screen: 60 times a
	// second). The same picture again changes nothing: reading its HD files each time (several MB)
	// and freeing the buffers the presenter was showing broke that screen.
	static char shownBase[64];
	{
		char b[64];
		lstrcpynA(b, name, sizeof b);
		char *d = strrchr(b, '.');
		if (d) *d = 0;
		CharLowerA(b);
		if (rows == GAME_H && g_hdPic.on && !strcmp(b, shownBase) && !memcmp(pic, g_hdPic.shadow, GAME_W * GAME_H)) return;
		lstrcpynA(shownBase, b, sizeof shownBase);
	}
	EnterCriticalSection(&g_lock);
	g_hdPic.on = FALSE;
	g_room.on = FALSE;
	if (g_sprValid) { memset(g_sprValid, 0, MAX_W * GAME_H); rows_dirty(g_sprDirty, 0, GAME_H); } // a new screen: nothing stamped on it yet
	g_layerGen++;
	LeaveCriticalSection(&g_lock);
	if (rows != GAME_H) return;
	char base[64], path[MAX_PATH];
	lstrcpynA(base, name, sizeof base);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;
	CharLowerA(base);
	snprintf(path, sizeof path, "%s\\lords2hd\\screens\\%s.bin", g_gameDir, base);
	AFILE *f = afopen(path);
	if (!f) return;
	if (!g_hdPic.px) g_hdPic.px = malloc(GAME_W * 2 * GAME_H * 2 * 4);
	EnterCriticalSection(&g_lock);
	BOOL ok = g_hdPic.px && afread(g_hdPic.pal, 1, 768, f) == 768 &&
		afread(g_hdPic.px, 4, GAME_W * 2 * GAME_H * 2, f) == GAME_W * 2 * GAME_H * 2;
	if (ok) {
		memcpy(g_hdPic.shadow, pic, GAME_W * GAME_H);
		g_hdPic.on = TRUE;
		g_hdPic.seen = FALSE;
		room_load(base);
	}
	LeaveCriticalSection(&g_lock);
	afclose(f);
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
static BOOL g_ownScroll; // the settings scroll is being drawn: its HD art is stamped whatever the palette
static BYTE *g_txt, *g_txtShadow, *g_txtValid;     // the sharp letters' layer (text_overlay)
static BYTE *volatile g_txtS, *volatile g_txtShadowS, *volatile g_txtValidS;  // ... as of the frame on screen (volatile: g_txtS is read first, set last)
static int g_txtW, g_txtH; // the size all of them were made for (the back buffer's then)
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
	g_layerGen++; // this buffer's art changes (a battle's ground replaces the county's, and back)
	if (g_tileSets[slot].px) {
		free(g_tileSets[slot].px);
		free(g_tileSets[slot].wave);
		g_tileSets[slot].px = NULL;
		g_tileSets[slot].wave = NULL;
		g_tileSets[slot].buf = NULL;
	}
	snprintf(path, sizeof path, "%s\\lords2hd\\tiles\\%s.bin", g_gameDir, base);
	AFILE *f = afopen(path);
	if (!f) return;
	afseek(f, 0, SEEK_END);
	long bytes = aftell(f);
	afseek(f, 0, SEEK_SET);
	DWORD *px = malloc(bytes);
	if (px && afread(px, 1, bytes, f) == (size_t)bytes) {
		g_tileSets[slot].px = px;
		g_tileSets[slot].frames = bytes / (TILE_W * 2 * TILE_H * 2 * 4);
		g_tileSets[slot].buf = buffer;
		if (g_shoreWaves && !strncmp(base, "base1", 5)) g_tileSets[slot].wave = wave_build(px, g_tileSets[slot].frames);
	}
	else free(px);
	afclose(f);
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
	g_layerGen++; // new art for these tiles: the next map redraw stamps again, even if it looks the same
	logf_("HD tiles %s: %d frames", base, g_tileSets[slot].frames);
}

// The game redraws the whole county map every frame, mostly the same tiles and sprites in the same
// places. During a map redraw (h_redraw_map) the stamps are only listed; afterwards, if the list is
// the one of the last redraw and nothing else has written the HD layers since (g_layerGen), the
// layers already hold this picture and the stamping is skipped; otherwise the list is stamped in
// order, which ends the same as stamping each piece as it is drawn (each stamp checks the pixels the
// frame holds at the end). Stamping every frame took 2 ms of the redraw's 2.1.
typedef struct { const BYTE *buf; int index, x, y, sprite; } stamp_call;
#define MAX_STAMPS 16384
static stamp_call *g_stampCur, *g_stampPrev;
static int g_stampN, g_stampPrevN = -1;
static BOOL g_stampRec, g_stampOverflow;
static LONG g_stampPrevGen = -1;
static int g_stampSkipped, g_stampDone; // Timing=1
static BOOL stamp_record(const BYTE *buf, int index, int x, int y, int sprite)
{
	if (!g_stampRec) { g_layerGen++; return FALSE; }
	if (g_stampN < MAX_STAMPS) g_stampCur[g_stampN++] = (stamp_call){ buf, index, x, y, sprite };
	else g_stampOverflow = TRUE;
	return TRUE;
}

static DWORD *g_layerS;
static BYTE *g_layerShadowS, *g_layerValidS, *g_waveDS;
static BYTE g_layerDirty[GAME_H];
static void tile_dirty(int y0, int y1);
// Stamps the tile just drawn: frame "index" of the file at "buf", at game position x, y.
static void stamp_tile(const BYTE *buf, int index, int x, int y)
{
	if (!g_layer || !g_back || !buf || index < 0) return;
	if (stamp_record(buf, index, x, y, 0)) return;
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
	tile_dirty(y, y + TILE_H);
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
static struct { const BYTE *buf; BYTE *data; BYTE pal[768]; char name[32]; } g_spriteSets[MAX_SPRITE_SETS];

static void read_palette(const char *path, BYTE *pal)
{
	AFILE *f = afopen(path);
	if (!f || afread(pal, 1, 768, f) != 768) memset(pal, 0, 768);
	if (f) afclose(f);
}

static void sprites_loaded(const char *name, const void *buffer)
{
	if (!g_hdTiles) return;
	for (int i = 0; i < MAX_SPRITE_SETS; i++) // the same file into the same buffer again: already have it
		if (g_spriteSets[i].buf == buffer && g_spriteSets[i].data && !lstrcmpiA(g_spriteSets[i].name, name)) return;
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
	AFILE *f = afopen(path);
	if (!f) return;
	afseek(f, 0, SEEK_END);
	long bytes = aftell(f);
	afseek(f, 0, SEEK_SET);
	BYTE *data = slot >= 0 ? malloc(bytes) : NULL;
	if (data && afread(data, 1, bytes, f) == (size_t)bytes) {
		g_spriteSets[slot].buf = buffer;
		g_spriteSets[slot].data = data;
		lstrcpynA(g_spriteSets[slot].name, name, sizeof g_spriteSets[slot].name);
		g_layerGen++;
		snprintf(path, sizeof path, "%s\\lords2hd\\sprites\\%s.pal", g_gameDir, base);
		if (!aexists(path)) snprintf(path, sizeof path, "%s\\Base01.256", g_gameDir);
		read_palette(path, g_spriteSets[slot].pal);
		logf_("HD sprites %s", base);
	}
	else free(data);
	afclose(f);
}

// The zoomed-out battle's HD stamping, batched: while the game redraws the battlefield (h_battle_draw)
// every stamp and every cell's clearing is queued, then all of them are done at the end on all the
// CPU's threads, each taking a band of rows and going through the queue in order. Each stamp checks the
// finished frame instead of the frame as it stood then, which gives the same layer: a pixel drawn
// over later fails the check, and its new owner stamps it. A scroll redraws ~1500 cells: stamped one
// by one they took ~9 ms of every game frame and the battle ran 30% slow.
typedef struct { const BYTE *idx; const DWORD *hd; int x, y, w, h; } bt_job; // idx NULL: clear w x h
#define BT_QUEUE 16384
static bt_job g_btQ[BT_QUEUE];
static int g_btQN;
static BOOL g_btDefer;
static void bt_apply(const bt_job *j, int r0, int r1)
{
	int y0 = max(max(j->y, 0), r0), y1 = min(min(j->y + j->h, g_frameH), r1);
	if (!j->idx) { // a cell about to be redrawn: its HD pixels forgotten
		int x0 = max(j->x, 0), x1 = min(j->x + j->w, g_frameW);
		for (int r = y0; r < y1; r++) if (x1 > x0) memset(g_btValid + r * g_frameW + x0, 0, x1 - x0);
		return;
	}
	int w = j->w;
	for (int fy = y0; fy < y1; fy++) {
		int r = fy - j->y;
		const BYTE *row = g_back->bits + fy * g_back->pitch, *src = j->idx + r * w;
		const DWORD *h0row = j->hd + (2 * r) * (2 * w), *h1row = h0row + 2 * w;
		DWORD *o = g_btLayer + (size_t)fy * g_frameW * 4;
		BYTE *sh = g_btShadow + fy * g_frameW, *va = g_btValid + fy * g_frameW;
		int c0 = max(0, -(j->x + g_back->org)), c1 = min(w, g_frameW - (j->x + g_back->org));
		for (int c = c0; c < c1; c++) {
			BYTE v = src[c];
			int fx = j->x + g_back->org + c;
			if (!v || row[fx] != v) continue;
			DWORD *p = o + fx * 4;
			p[0] = h0row[2 * c]; p[1] = h0row[2 * c + 1]; p[2] = h1row[2 * c]; p[3] = h1row[2 * c + 1];
			sh[fx] = v; va[fx] = 1;
		}
	}
}
#define BT_FLUSH_BAND 24
static void bt_flush_job(int band, int me)
{
	(void)me;
	int r0 = band * BT_FLUSH_BAND, r1 = min(g_frameH, r0 + BT_FLUSH_BAND);
	for (int i = 0; i < g_btQN; i++) if (g_btQ[i].y < r1 && g_btQ[i].y + g_btQ[i].h > r0) bt_apply(&g_btQ[i], r0, r1);
}
static void pool_run(int bands, void (*job)(int band, int me));
static void bt_flush(void)
{
	if (!g_btQN) return;
	// (marked at queue time too, but a copy to the screen in between would have cleared the marks)
	for (int i = 0; i < g_btQN; i++) rows_dirty(g_btDirty, g_btQ[i].y, g_btQ[i].y + g_btQ[i].h);
	if (g_btLayer && g_back) pool_run((g_frameH + BT_FLUSH_BAND - 1) / BT_FLUSH_BAND, bt_flush_job);
	g_btQN = 0;
}
static void bt_queue(const BYTE *idx, const DWORD *hd, int x, int y, int w, int h)
{
	if (g_btQN >= BT_QUEUE) bt_flush(); // (in order: what was queued first is done first)
	g_btQ[g_btQN++] = (bt_job){ idx, hd, x, y, w, h };
}

static void stamp_sprite(const BYTE *buf, int index, int x, int y)
{
	if (stamp_record(buf, index, x, y, 1)) return;
	if (battle_zoom_on() && !g_btLayer) {
		// The presenter thread reads these as soon as g_btLayer is set: it is set last, and only
		// when all three exist (it once saw the layer before the table and read through null).
		DWORD *layer = malloc(g_frameW * g_frameH * 4 * 4);
		BYTE *shadow = calloc(g_frameW, g_frameH), *valid = calloc(g_frameW, g_frameH);
		if (layer && shadow && valid) {
			g_btShadow = shadow;
			g_btValid = valid;
			MemoryBarrier();
			g_btLayer = layer;
		}
		else { free(layer); free(shadow); free(valid); }
	}
	if (battle_zoom_on() && !g_btLayer) return;
	const BYTE *data = NULL, *setPal = NULL;
	for (int i = 0; i < MAX_SPRITE_SETS; i++)
		if (g_spriteSets[i].buf == buf) { data = g_spriteSets[i].data; setPal = g_spriteSets[i].pal; break; }
	if (!data || index < 0 || index >= *(const int *)data || !g_back) return;
	// Off the map a sprite may be drawn under another screen's palette than its art was made with
	// (map portraits in the Armoury's scroll): then it stays as the game drew it.
	if (!g_ownScroll && !county_map_on() && !IN_BATTLE_NOW() && g_primary && g_primary->pal) {
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
	if (battle_zoom_on() && g_btLayer && !g_ownScroll) { // (the settings scroll is not the battle's: its ground keeps its HD art)
		rows_dirty(g_btDirty, y, y + h);
		if (g_btDefer) { bt_queue(idx, hd, x, y, w, h); return; } // during a battlefield redraw: batched (bt_flush)
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
			g_sprDirty[fy] = 1;
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
	AFILE *f = afopen(path);
	if (!f) return;
	int frame = (which & 3) * 5 + 1, count = 0, off = 0;
	WORD wh[2];
	if (afread(&count, 4, 1, f) == 1 && frame < count && !afseek(f, 4 + 4 * frame, SEEK_SET) && afread(&off, 4, 1, f) == 1 && off &&
		!afseek(f, off, SEEK_SET) && afread(wh, 2, 2, f) == 2 && wh[0] == 128 && wh[1] == 128 && !afseek(f, 128 * 128, SEEK_CUR)) {
		g_miniHD = malloc(256 * 256 * 4);
		if (g_miniHD && afread(g_miniHD, 4, 256 * 256, f) != 256 * 256) { free(g_miniHD); g_miniHD = NULL; }
	}
	afclose(f);
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
	g_layerGen++;
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
			g_sprDirty[fy] = 1;
			stamped++;
		}
	}
	static int logged;
	if (logged++ < 6) logf_("minimap at %d,%d mode %d: %d of 16384 pixels HD (back %dx%d org %d)", x, y, mode, stamped, g_back->w, g_back->h, g_back->org);
}

static void text_clear(int x, int y, int w, int h);
static void text_stale(void);
static void text_publish(int y0, int y1);
static void bt_publish(int y0, int y1);
static void spr_publish(int y0, int y1);
static BOOL battle_zoom_on(void);
// the sprite layer as of the frame on screen (spr_publish); the live one until the first copy
static DWORD *g_sprLayerS;
static BYTE *g_sprShadowS, *g_sprValidS;
#define SPR_L (g_sprLayerS ? g_sprLayerS : g_sprLayer)
#define SPR_S (g_sprLayerS ? g_sprShadowS : g_sprShadow)
#define SPR_V (g_sprLayerS ? g_sprValidS : g_sprValid)

static void forget_tiles(void)
{
	g_layerGen++;
	if (g_layerValid) memset(g_layerValid, 0, MAX_W * GAME_H);
	tile_dirty(0, GAME_H);
	// The side panel (minimap, county box) is not redrawn with the map: its sprites stay.
	if (g_sprValid) {
		int keep = g_off + PANEL_X;
		// Only remembered (2), as the letters: a scroll's icons come back from the game's saved copy.
		if (keep <= 0 || keep >= MAX_W || IN_BATTLE_NOW()) keep = MAX_W;
		for (int y = 0; y < GAME_H; y++) {
			BYTE *v = g_sprValid + y * MAX_W;
			g_sprDirty[y] = 1;
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
		const BYTE *ss = SPR_S + y * MAX_W + sx, *sv = SPR_V + y * MAX_W + sx;
		const DWORD *s0 = SPR_L + (2 * y) * (2 * MAX_W) + 2 * sx, *s1 = s0 + 2 * MAX_W;
		DWORD *o0 = g_frame2x + (2 * y) * (2 * sw), *o1 = o0 + 2 * sw;
		for (int x = 0; x < sw; x++) {
			if (!sv[x] || fr[x] != ss[x]) continue;
			o0[2 * x] = s0[2 * x]; o0[2 * x + 1] = s0[2 * x + 1];
			o1[2 * x] = s1[2 * x]; o1[2 * x + 1] = s1[2 * x + 1];
		}
	}
}

// Videos at the size of the game's discs. GOG ships the event clips (cart, jail, hang, axmen, pillage,
// win_game) at 296x184; the disc versions players put in the game folder (the "SMK_HIGH" pack) are
// 500x312, and the game draws them at the small clip's fixed place, so they spilled out of their frame.
// Such a clip is decoded into its own buffer; the game's frame gets it shrunk to 296x184 in its place,
// and the screen shows the full frame smoothly scaled over that area (video_overlay).
#define VID_SLOT_W 296
#define VID_SLOT_H 184
static struct {
	void *smk;                  // the clip being played at a larger size than its slot
	int w, h, fx, fy, pitch;    // its size; where its slot is in the frame, and the frame pitch
	BYTE *dest;                 // where the game asked for it (frame buffer + its x, y)
	BYTE *buf;                  // the decoded frame, w x h palette indices
	BYTE *pub, shadow[VID_SLOT_W * VID_SLOT_H]; // as of the last frame shown; what went into the slot
	volatile LONG on;
} g_vid;

typedef void *(__stdcall *smk_open_t)(const char *, DWORD, DWORD);
typedef DWORD (__stdcall *smk_tobuf_t)(void *, DWORD, DWORD, DWORD, DWORD, void *, DWORD);
typedef DWORD (__stdcall *smk_frame_t)(void *);
typedef void (__stdcall *smk_close_t)(void *);
static smk_open_t o_SmackOpen;
static smk_tobuf_t o_SmackToBuffer;
static smk_frame_t o_SmackDoFrame;
static smk_close_t o_SmackClose;

// the pack handles videos are playing from, closed with them (h_SmackClose)
static struct { void *smk; HANDLE h; } g_vidHandles[4];
static void video_handle_add(void *s, HANDLE h)
{
	for (int i = 0; i < 4; i++) if (!g_vidHandles[i].smk) { g_vidHandles[i].smk = s; g_vidHandles[i].h = h; return; }
	CloseHandle(g_vidHandles[0].h); g_vidHandles[0].smk = s; g_vidHandles[0].h = h; // (never more than a couple open)
}
static void video_handle_close(void *s)
{
	for (int i = 0; i < 4; i++) if (g_vidHandles[i].smk == s) { CloseHandle(g_vidHandles[i].h); g_vidHandles[i].smk = NULL; }
}
static volatile LONG g_videoOpened; // a video has been opened since launch (the music hold, h_stream_play)
static void *__stdcall h_SmackOpen(const char *name, DWORD flags, DWORD extra)
{
	InterlockedExchange(&g_videoOpened, 1);
	// The GOG videos are stored interlaced (header flag 2: half the rows, played on every other
	// line, the lines between left black). Opened with SMACKYDOUBLE (0x200000) Smacker draws each
	// row twice instead: same size and place, no black lines.
	BOOL doubled = FALSE;
	// The disc-quality videos ride in the art pack (videos/<name>.smk): such a video is played from
	// the pack - Smacker is given the pack's handle at its place (SMACKFILEHANDLE, 0x1000) - in place
	// of the game's own file.
	HANDLE packed = INVALID_HANDLE_VALUE;
	if (name && !(flags & 0x1000) && g_pak) {
		const char *base = strrchr(name, '\\'), *b2 = strrchr(name, '/');
		base = max(base, b2); base = base ? base + 1 : name;
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\lords2hd\\videos\\%s", g_gameDir, base);
		const PAK_ENTRY *e = pak_find(path);
		if (e) {
			packed = CreateFileA(g_pakPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			LONG hi = 0;
			if (packed != INVALID_HANDLE_VALUE && SetFilePointer(packed, (LONG)e->off, &hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER) { CloseHandle(packed); packed = INVALID_HANDLE_VALUE; }
		}
	}
	if (name && !(flags & 0x1000) && !(flags & 0x300000)) { // a file name, no Y mode asked for
		DWORD head[6], got = 0;
		if (packed != INVALID_HANDLE_VALUE) {
			LONG hi = 0; DWORD at = SetFilePointer(packed, 0, &hi, FILE_CURRENT);
			BOOL ok = ReadFile(packed, head, sizeof head, &got, NULL) && got == sizeof head;
			hi = 0; SetFilePointer(packed, (LONG)at, &hi, FILE_BEGIN);
			if (ok && (head[0] & 0xffffff) == 0x4b4d53 && (head[5] & 2)) { flags |= 0x200000; doubled = TRUE; }
		}
		else {
			FILE *f = fopen(name, "rb");
			if (f) {
				if (fread(head, 4, 6, f) == 6 && (head[0] & 0xffffff) == 0x4b4d53 && (head[5] & 2)) { flags |= 0x200000; doubled = TRUE; }
				fclose(f);
			}
		}
	}
	void *s = packed != INVALID_HANDLE_VALUE ? o_SmackOpen((const char *)packed, flags | 0x1000, extra) : o_SmackOpen(name, flags, extra);
	if (packed != INVALID_HANDLE_VALUE) {
		if (s) { video_handle_add(s, packed); logf_("video %s: from the pack", name); }
		else { CloseHandle(packed); s = o_SmackOpen(name, flags, extra); } // the game's own file then
	}
	if (s && doubled) logf_("video %s: interlaced, shown line-doubled", name);
	if (s) {
		int w = *(int *)((BYTE *)s + 4), h = *(int *)((BYTE *)s + 8);
		logf_("video %s: %dx%d, %d frames, flags %08lx", name, w, h, *(int *)((BYTE *)s + 12), (unsigned long)flags);
		if (w == 500 && h == 312) { // the disc version of a 296x184 event clip
			BYTE *b = realloc(g_vid.buf, w * h), *p = realloc(g_vid.pub, w * h);
			if (b && p) {
				g_vid.buf = b; g_vid.pub = p;
				g_vid.smk = s; g_vid.w = w; g_vid.h = h;
				memset(b, 0, w * h);
			}
			else { g_vid.buf = b; g_vid.pub = p; }
		}
	}
	return s;
}

static DWORD __stdcall h_SmackToBuffer(void *s, DWORD x, DWORD y, DWORD pitch, DWORD height, void *buf, DWORD flags)
{
	if (!s || s != g_vid.smk) return o_SmackToBuffer(s, x, y, pitch, height, buf, flags);
	g_vid.dest = (BYTE *)buf + y * pitch + x;
	g_vid.pitch = (int)pitch;
	g_vid.fx = g_back && (BYTE *)buf >= g_back->bits ? (int)(((BYTE *)buf - g_back->bits) % pitch) + (int)x : (int)x;
	g_vid.fy = (int)y;
	return o_SmackToBuffer(s, 0, 0, g_vid.w, g_vid.h, g_vid.buf, flags);
}

static DWORD __stdcall h_SmackDoFrame(void *s)
{
	DWORD r = o_SmackDoFrame(s);
	if (s && s == g_vid.smk && g_vid.dest) {
		for (int oy = 0; oy < VID_SLOT_H; oy++) {
			const BYTE *src = g_vid.buf + (oy * g_vid.h / VID_SLOT_H) * g_vid.w;
			BYTE *d = g_vid.dest + oy * g_vid.pitch, *sh = g_vid.shadow + oy * VID_SLOT_W;
			for (int ox = 0; ox < VID_SLOT_W; ox++) d[ox] = sh[ox] = src[ox * g_vid.w / VID_SLOT_W];
		}
		EnterCriticalSection(&g_lock);
		memcpy(g_vid.pub, g_vid.buf, g_vid.w * g_vid.h);
		LeaveCriticalSection(&g_lock);
		InterlockedExchange(&g_vid.on, 1);
	}
	return r;
}

static void __stdcall h_SmackClose(void *s)
{
	if (s && s == g_vid.smk) {
		g_vid.smk = NULL;
		g_vid.dest = NULL;
		InterlockedExchange(&g_vid.on, 0);
	}
	o_SmackClose(s);
	video_handle_close(s);
}

// ---- Better music --------------------------------------------------------------------------------
// The game streams its music (Battle1-5, Scroll1-5, Setup, Setup2, SETUP3) on DirectSound stream 0:
// FUN_00426534 opens the file through FUN_004b92e0 -> mmioOpenA, with the name copied to
// 0x522b60 + stream * 16. If lords2hd\music\<name> exists, that file is opened instead, in any PCM
// format (the remastered soundtrack: 44.1 kHz 16-bit). The game creates each stream's sound buffer
// once, in the format of the first file it plays, and reuses it; so when the next file's format
// differs, the old buffer is released here and the game makes a new one (FUN_004263ad has already
// stopped the stream and its timer before it opens the file).
#define STREAM_NAMES 0x00522b60
#define STREAM_BUFS ((IDirectSoundBuffer **)0x00522b80)
typedef HMMIO (WINAPI *mmioOpenA_t)(LPSTR, LPMMIOINFO, DWORD);
static mmioOpenA_t o_mmioOpenA;

static BOOL wav_format(const char *path, WAVEFORMATEX *wf)
{
	HMMIO h = o_mmioOpenA((LPSTR)path, NULL, MMIO_READ);
	if (!h) return FALSE;
	MMCKINFO riff = {0}, ck = {0};
	BOOL ok = FALSE;
	memset(wf, 0, sizeof *wf);
	if (mmioDescend(h, &riff, NULL, 0) == 0 && riff.ckid == FOURCC_RIFF && riff.fccType == mmioFOURCC('W', 'A', 'V', 'E')) {
		ck.ckid = mmioFOURCC('f', 'm', 't', ' ');
		if (mmioDescend(h, &ck, &riff, MMIO_FINDCHUNK) == 0 && ck.cksize >= 16)
			ok = mmioRead(h, (HPSTR)wf, 16) == 16;
	}
	mmioClose(h, 0);
	return ok;
}

// a sound from the pack as a file for mmio: adwInfo[0] its bytes, adwInfo[1] their count
static LRESULT CALLBACK pak_ioproc(LPSTR info, UINT msg, LPARAM a, LPARAM b)
{
	MMIOINFO *mi = (MMIOINFO *)info;
	LONG size = (LONG)mi->adwInfo[1];
	switch (msg) {
	case MMIOM_OPEN: mi->lDiskOffset = 0; return MMSYSERR_NOERROR;
	case MMIOM_CLOSE: return 0;
	case MMIOM_READ: {
		LONG n = (LONG)b, left = size - mi->lDiskOffset;
		if (n > left) n = left;
		if (n <= 0) return 0;
		memcpy((void *)a, (const BYTE *)mi->adwInfo[0] + mi->lDiskOffset, n);
		mi->lDiskOffset += n;
		return n;
	}
	case MMIOM_SEEK: {
		LONG to = (int)b == SEEK_CUR ? mi->lDiskOffset + (LONG)a : (int)b == SEEK_END ? size + (LONG)a : (LONG)a;
		if (to < 0 || to > size) return -1;
		return mi->lDiskOffset = to;
	}
	}
	return -1; // (writing: never)
}// A sound from the art pack (sound/<name>.wav): read whole and opened as a memory file. The game
// reads its sounds whole as soon as it opens them, so the last few buffers are kept and reused.
static HMMIO pak_sound(const char *loosePath, const char *base, DWORD flags)
{
	if (!g_pak || (flags & (MMIO_WRITE | MMIO_READWRITE | MMIO_CREATE))) return NULL;
	AFILE *f = afopen(loosePath);
	if (!f) return NULL;
	static BYTE *ring[8];
	static int next;
	DWORD size = f->f ? 0 : f->size;
	BYTE *buf = size ? malloc(size) : NULL;
	HMMIO h = NULL;
	if (buf && afread(buf, 1, size, f) == size) {
		// not a plain memory file (FOURCC_MEM): its whole data comes back in one piece, and the game's
		// reader (FUN_004b9652) refuses pieces over 200,000 bytes - a long restored line did not play.
		// Through pak_ioproc it is read in the usual 8 KB buffer, as from a file.
		MMIOINFO mi = { 0 };
		mi.pIOProc = pak_ioproc;
		mi.adwInfo[0] = (DWORD)buf;
		mi.adwInfo[1] = size;
		h = o_mmioOpenA((LPSTR)base, &mi, MMIO_READ | MMIO_ALLOCBUF);
	}
	afclose(f);
	if (!h) { free(buf); return NULL; }
	free(ring[next]); ring[next] = buf; next = (next + 1) % 8;
	static char last[32];
	if (lstrcmpiA(last, base)) { logf_("sound: pack %s", base); lstrcpynA(last, base, sizeof last); }
	return h;
}static HMMIO WINAPI h_mmioOpenA(LPSTR name, LPMMIOINFO info, DWORD flags)
{
	DWORD a = (DWORD)name;
	if (!name) return o_mmioOpenA(name, info, flags);
	char alt[MAX_PATH];
	const char *base = strrchr(name, '\\');
	base = base ? base + 1 : name;
	// Any other sound (dialogue, unit voices, effects: FUN_004b984f loads them whole, in their own
	// format) from lords2hd\sound when a file of that name is there - e.g. Lords2Resounded's
	// restored speech, without overwriting the game's files.
	if (a < STREAM_NAMES || a >= STREAM_NAMES + 2 * 16) {
		const char *dot = strrchr(base, '.');
		if (dot && !lstrcmpiA(dot, ".wav") && g_logLoads) logf_("sound opened: %s", base);
		if (dot && !lstrcmpiA(dot, ".wav") && g_resounded) { // Off: the game's own voices
			snprintf(alt, sizeof alt, "%s\\lords2hd\\sound\\%s", g_gameDir, base);
			if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) {
				HMMIO h = o_mmioOpenA(alt, info, flags);
				if (h) {
					static char last[32];
					if (lstrcmpiA(last, base)) { logf_("sound: lords2hd\\sound\\%s", base); lstrcpynA(last, base, sizeof last); }
					return h;
				}
			}
			HMMIO h = pak_sound(alt, base, flags); // else from the pack
			if (h) return h;
		}
		return o_mmioOpenA(name, info, flags);
	}
	int n = (a - STREAM_NAMES) / 16;
	snprintf(alt, sizeof alt, "%s\\lords2hd\\music\\%s", g_gameDir, base);
	BOOL replaced = GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES;
	if (!replaced) { // music from the sound folder too: one folder for a Lords2Resounded output
		snprintf(alt, sizeof alt, "%s\\lords2hd\\sound\\%s", g_gameDir, base);
		replaced = GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES;
	}
	const char *path = replaced ? alt : name;
	WAVEFORMATEX want, have = {0};
	if (wav_format(path, &want)) {
		IDirectSoundBuffer *b = STREAM_BUFS[n];
		DWORD got = 0;
		if (b && (IDirectSoundBuffer_GetFormat(b, &have, sizeof have, &got) != DS_OK ||
				have.nSamplesPerSec != want.nSamplesPerSec || have.wBitsPerSample != want.wBitsPerSample ||
				have.nChannels != want.nChannels)) {
			IDirectSoundBuffer_Stop(b);
			IDirectSoundBuffer_Release(b);
			STREAM_BUFS[n] = NULL;
			logf_("music: stream %d buffer was %lu Hz %u-bit %u ch, rebuilt for the next file", n,
				have.nSamplesPerSec, have.wBitsPerSample, have.nChannels);
		}
		logf_("music: %s%s (%lu Hz, %u-bit, %u ch; stream timer %d ms x %d)", replaced ? "lords2hd\\music\\" : "", base,
			want.nSamplesPerSec, want.wBitsPerSample, want.nChannels, *(int *)0x004d4758, *(int *)0x004d475c);
	}
	HMMIO h = replaced ? o_mmioOpenA(alt, info, flags) : NULL;
	if (!h) h = o_mmioOpenA(name, info, flags);
	return h;
}

// The game starts the setup music at launch and the intro video stops it a moment later. Who wins
// that race varies: when the music gets going first, ~20 ms of it slips out, a pop at launch. So a
// music stream started (FUN_00426b92) before any video has opened is started silent; the game starts
// it again after the intro, and that start (or 5 s without a video) gives it its volume back.
typedef int (__cdecl *stream_play_t)(DWORD);
static stream_play_t o_stream_play;
static LONG g_streamHeld[2], g_streamVol[2];
static DWORD g_streamT0;
// after every game frame: a held stream gets its volume back 5 s after launch at the latest
static void stream_hold_tick(void)
{
	for (int n = 0; n < 2; n++)
		if (g_streamHeld[n] && GetTickCount() - g_streamT0 >= 5000) {
			g_streamHeld[n] = 0;
			if (STREAM_BUFS[n]) IDirectSoundBuffer_SetVolume(STREAM_BUFS[n], g_streamVol[n]);
			logf_("music: stream %d heard again (5 s)", n);
		}
}
static int __cdecl h_stream_play(DWORD n)
{
	IDirectSoundBuffer *b = n < 2 ? STREAM_BUFS[n] : NULL;
	if (!g_streamT0) g_streamT0 = GetTickCount();
	if (b) {
		if (!g_videoOpened && GetTickCount() - g_streamT0 < 5000) {
			if (!g_streamHeld[n] && IDirectSoundBuffer_GetVolume(b, &g_streamVol[n]) == DS_OK) {
				g_streamHeld[n] = 1;
				IDirectSoundBuffer_SetVolume(b, DSBVOLUME_MIN);
				logf_("music: stream %lu held silent until the intro has started", n);
			}
		}
		else if (g_streamHeld[n]) {
			g_streamHeld[n] = 0;
			IDirectSoundBuffer_SetVolume(b, g_streamVol[n]);
			logf_("music: stream %lu heard again", n);
		}
	}
	return o_stream_play(n);
}
// Players get the remastered soundtrack as MP3s with names like "07. Scroll 1 (1996, Remastered).mp3".
// Dropped into lords2hd\music, they are turned into the WAVs above on a low-priority thread at start,
// by Windows' own decoder (Media Foundation, loaded by hand so the DLL still loads where it's missing).
// Each file is matched to a track by its name; files already converted are skipped.
static const char *music_track(const char *file)
{
	char s[MAX_PATH];
	int i = 0;
	for (; file[i] && i < MAX_PATH - 1; i++) s[i] = (char)tolower((unsigned char)file[i]);
	s[i] = 0;
	static const struct { const char *word; const char *out[6]; } kinds[] = {
		{ "battle", { NULL, "battle1.wav", "battle2.wav", "battle3.wav", "battle4.wav", "battle5.wav" } },
		{ "scroll", { NULL, "scroll1.wav", "scroll2.wav", "scroll3.wav", "scroll4.wav", "scroll5.wav" } },
		// the remaster has no Setup2; its "Siege Pack Setup 2" is the game's SETUP3 (same length, 1:57)
		{ "setup",  { "setup.wav", NULL, "setup3.wav", "setup3.wav", NULL, NULL } },
	};
	for (int k = 0; k < 3; k++) {
		const char *p = strstr(s, kinds[k].word);
		if (!p) continue;
		p += strlen(kinds[k].word);
		while (*p == ' ' || *p == '_' || *p == '-') p++;
		int d = (*p >= '1' && *p <= '5' && !(p[1] >= '0' && p[1] <= '9')) ? *p - '0' : 0;
		return kinds[k].out[d];
	}
	return NULL;
}

typedef HRESULT (WINAPI *MFStartup_t)(ULONG, DWORD);
typedef HRESULT (WINAPI *MFShutdown_t)(void);
typedef HRESULT (WINAPI *MFCreateMediaType_t)(IMFMediaType **);
typedef HRESULT (WINAPI *MFCreateSourceReaderFromURL_t)(LPCWSTR, IMFAttributes *, IMFSourceReader **);

static BOOL music_decode(MFCreateSourceReaderFromURL_t open, MFCreateMediaType_t newtype, const char *src, const char *dst)
{
	WCHAR wsrc[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, src, -1, wsrc, MAX_PATH);
	IMFSourceReader *r = NULL;
	IMFMediaType *t = NULL, *got = NULL;
	FILE *f = NULL;
	BOOL ok = FALSE;
	DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
	UINT32 rate = 0, ch = 0, bits = 0, bytes = 0;
	char tmp[MAX_PATH];
	snprintf(tmp, sizeof tmp, "%s.part", dst);
	if (FAILED(open(wsrc, NULL, &r))) goto done;
	IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	IMFSourceReader_SetStreamSelection(r, stream, TRUE);
	if (FAILED(newtype(&t))) goto done;
	IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
	IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
	IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	if (FAILED(IMFSourceReader_SetCurrentMediaType(r, stream, NULL, t))) goto done;
	if (FAILED(IMFSourceReader_GetCurrentMediaType(r, stream, &got))) goto done;
	IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
	IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
	IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
	if (!rate || !ch || bits != 16) goto done;
	f = fopen(tmp, "wb");
	if (!f) goto done;
	BYTE hdr[44] = {0};
	fwrite(hdr, 1, 44, f);
	for (;;) {
		DWORD flags = 0;
		IMFSample *smp = NULL;
		if (FAILED(IMFSourceReader_ReadSample(r, stream, 0, NULL, &flags, NULL, &smp))) break;
		if (smp) {
			IMFMediaBuffer *mb = NULL;
			if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &mb))) {
				BYTE *p; DWORD n = 0;
				if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, &n))) {
					fwrite(p, 1, n, f);
					bytes += n;
					IMFMediaBuffer_Unlock(mb);
				}
				IMFMediaBuffer_Release(mb);
			}
			IMFSample_Release(smp);
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ok = bytes > 0; break; }
		if (flags & MF_SOURCE_READERF_ERROR) break;
	}
	// RIFF header: PCM, rate, channels, 16-bit
	UINT32 align = ch * 2, v;
	memcpy(hdr, "RIFF", 4); v = 36 + bytes; memcpy(hdr + 4, &v, 4);
	memcpy(hdr + 8, "WAVEfmt ", 8); v = 16; memcpy(hdr + 16, &v, 4);
	hdr[20] = 1; hdr[22] = (BYTE)ch; memcpy(hdr + 24, &rate, 4);
	v = rate * align; memcpy(hdr + 28, &v, 4); hdr[32] = (BYTE)align; hdr[34] = 16;
	memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &bytes, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(hdr, 1, 44, f);
done:
	if (f) fclose(f);
	if (got) IMFMediaType_Release(got);
	if (t) IMFMediaType_Release(t);
	if (r) IMFSourceReader_Release(r);
	if (ok) ok = MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING);
	if (!ok) DeleteFileA(tmp);
	logf_("music: %s %s -> %s (%u Hz, %u ch, %.0f s)", ok ? "converted" : "could not convert", strrchr(src, '\\') + 1,
		strrchr(dst, '\\') + 1, rate, ch, (rate && ch) ? bytes / (double)(rate * ch * 2) : 0.0);
	return ok;
}

// The remaster's tracks hold the game's own loop plus a little lead-in and, in places, a fade-out:
// played as is they do not loop seamlessly. Each is cut to the game's own track (its 11 kHz WAV next
// to the exe): where the game's first two seconds sit in it (searched in its first three) to where the
// game's last two seconds end (searched near there). Same recordings, so the match is exact; the cut
// WAV then plays and loops as the game's own. Done once: a WAV within 10 ms of the game's is left.
// Lords2Resounded's edits of these recordings (github.com/gqzomer/Lords2Resounded): its cut points,
// hand-picked so each track loops without the remaster's fade-out, given here in seconds from where
// the game's own track starts in the recording (measured on its YouTube source), and for Battle 1 a
// passage it appends to bridge the loop. So the same edit is made on a player's MP3 of them.
static const struct { const char *track; double from, to, bridgeFrom, bridgeTo; } kMusicEdit[] = {
	{ "battle1.wav", 0.0326, 139.0898, 3.6461, 6.0400 }, { "battle2.wav", -0.1374, 120.0259, 0, 0 },
	{ "battle3.wav", 0.0026, 128.7591, 0, 0 }, { "battle4.wav", 0.0022, 122.5225, 0, 0 },
	{ "battle5.wav", -0.0001, 123.4372, 0, 0 }, { "scroll1.wav", -0.1119, 172.4704, 0, 0 },
	{ "scroll2.wav", -0.0970, 135.5192, 0, 0 }, { "scroll3.wav", 0.0037, 121.0690, 0, 0 },
	{ "scroll4.wav", 0.0036, 119.0767, 0, 0 }, { "scroll5.wav", 0.0028, 128.0492, 0, 0 },
	{ "setup.wav", 0.0033, 39.2855, 0, 0 }, { "setup3.wav", 0.0005, 108.5304, 0, 0 },
};
static int music_edit(const char *track) { for (int i = 0; i < (int)(sizeof kMusicEdit / sizeof *kMusicEdit); i++) if (!lstrcmpiA(kMusicEdit[i].track, track)) return i; return -1; }
// A WAV not to convert or cut again: one the patch cut (a "<name>.cut" marker beside it, newer than
// it), or one it did not make at all (not 44.1 kHz 16-bit, e.g. Lords2Resounded's own 48 kHz output).
static BOOL music_is_cut(const char *wav, const char *track)
{
	(void)track;
	FILE *f = fopen(wav, "rb");
	if (!f) return FALSE;
	BYTE h[36];
	BOOL got = fread(h, 1, 36, f) == 36;
	fclose(f);
	DWORD rate = 0; WORD bits = 0;
	if (got) { memcpy(&rate, h + 24, 4); memcpy(&bits, h + 34, 2); }
	if (got && (rate != 44100 || bits != 16)) return TRUE; // not ours
	char mark[MAX_PATH];
	snprintf(mark, sizeof mark, "%s.cut", wav);
	WIN32_FILE_ATTRIBUTE_DATA a, m;
	return GetFileAttributesExA(wav, GetFileExInfoStandard, &a) && GetFileAttributesExA(mark, GetFileExInfoStandard, &m) &&
		CompareFileTime(&m.ftLastWriteTime, &a.ftLastWriteTime) >= 0;
}
static BOOL wav_data(const char *path, BYTE **data, DWORD *bytes, WORD *ch, WORD *bits, DWORD *rate)
{
	FILE *f = fopen(path, "rb");
	if (!f) return FALSE;
	BYTE h[12];
	BOOL ok = fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4), fmt = FALSE;
	*data = NULL;
	while (ok) {
		BYTE c[8];
		if (fread(c, 1, 8, f) != 8) { ok = FALSE; break; }
		DWORD n; memcpy(&n, c + 4, 4);
		if (!memcmp(c, "fmt ", 4)) {
			BYTE w[16];
			if (n < 16 || fread(w, 1, 16, f) != 16) { ok = FALSE; break; }
			memcpy(ch, w + 2, 2); memcpy(rate, w + 4, 4); memcpy(bits, w + 14, 2);
			fmt = TRUE;
			fseek(f, (long)(n - 16 + (n & 1)), SEEK_CUR);
		}
		else if (!memcmp(c, "data", 4)) {
			*data = malloc(n ? n : 1);
			ok = fmt && *data && fread(*data, 1, n, f) == n;
			*bytes = n;
			break;
		}
		else fseek(f, (long)(n + (n & 1)), SEEK_CUR);
	}
	fclose(f);
	if (!ok) { free(*data); *data = NULL; }
	return ok;
}
// best place of needle (n samples) in hay[from .. to), by normalised correlation
static int best_offset(const float *needle, int n, const float *hay, int from, int to, float *score)
{
	double nm = 0, nn = 0;
	for (int i = 0; i < n; i++) nm += needle[i];
	nm /= n;
	float *nd = malloc(n * sizeof *nd);
	if (!nd) return -1;
	for (int i = 0; i < n; i++) { nd[i] = (float)(needle[i] - nm); nn += nd[i] * nd[i]; }
	int best = -1; double bs = -2;
	for (int o = from; o < to; o++) {
		double s = 0, hs = 0, hh = 0;
		const float *h = hay + o;
		for (int i = 0; i < n; i++) { s += nd[i] * h[i]; hs += h[i]; hh += h[i] * h[i]; }
		double var = hh - hs * hs / n, r = var > 0 && nn > 0 ? s / sqrt(var * nn) : 0;
		if (r > bs) { bs = r; best = o; }
	}
	free(nd);
	*score = (float)bs;
	return best;
}
static void music_trim(const char *wav, const char *track)
{
	char ref[MAX_PATH];
	snprintf(ref, sizeof ref, "%s\\%s", g_gameDir, track);
	BYTE *gd = NULL, *rd = NULL; DWORD gb = 0, rb = 0, grate = 0, rrate = 0; WORD gch = 0, gbits = 0, rch = 0, rbits = 0;
	if (!wav_data(ref, &gd, &gb, &gch, &gbits, &grate) || !wav_data(wav, &rd, &rb, &rch, &rbits, &rrate)) goto out;
	if (gbits != 8 || grate != 11025 || !gch || rbits != 16 || rrate != 44100 || !rch) { logf_("music: %s not cut (formats %lu/%u, %lu/%u)", track, grate, gbits, rrate, rbits); goto out; }
	int gn = (int)(gb / gch), rn = (int)(rb / (2 * rch)), qn = rn / 4; // game frames; remaster frames, and at 11025
	int e = music_edit(track);
	if (e < 0 || music_is_cut(wav, track)) goto out;
	float *g = malloc(gn * sizeof *g), *q = malloc(qn * sizeof *q);
	if (!g || !q) { free(g); free(q); goto out; }
	for (int i = 0; i < gn; i++) { int s = 0; for (int c = 0; c < gch; c++) s += gd[i * gch + c] - 128; g[i] = (float)s / gch; }
	const short *rs = (const short *)rd;
	for (int i = 0; i < qn; i++) { int s = 0; for (int k = 0; k < 4; k++) for (int c = 0; c < rch; c++) s += rs[(i * 4 + k) * rch + c]; q[i] = (float)s / (4 * rch * 256); }
	int W = 2 * 11025;
	float sa = 0, st = 0;
	int a = gn > W && qn > W ? best_offset(g, W, q, 0, min(3 * 11025, qn - W), &sa) : -1;
	int exp = a + gn - W, t = a >= 0 ? best_offset(g + gn - W, W, q, max(0, exp - 11025 / 2), min(qn - W, exp + 11025 / 2), &st) : -1;
	free(g); free(q);
	if (a < 0 || t < 0 || sa < 0.5f || st < 0.5f) { logf_("music: %s not cut (no match: %.2f, %.2f)", track, sa, st); goto out; }
	// Lords2Resounded's edit, placed by where the game's track starts in this recording (a)
	double at = a / 11025.0;
	DWORD frame = 2 * rch, total = rb / frame;
#define AT(s) ((DWORD)max(0.0, min((double)total, (at + (s)) * rrate + 0.5)))
	DWORD from = AT(max(kMusicEdit[e].from, 0.0)) * frame, to = AT(kMusicEdit[e].to) * frame; // (no lead-in: the game's track has none)
	DWORD bFrom = kMusicEdit[e].bridgeTo > 0 ? AT(kMusicEdit[e].bridgeFrom) * frame : 0, bTo = kMusicEdit[e].bridgeTo > 0 ? AT(kMusicEdit[e].bridgeTo) * frame : 0;
#undef AT
	if (to <= from) goto out;
	char tmp[MAX_PATH];
	snprintf(tmp, sizeof tmp, "%s.part", wav);
	FILE *f = fopen(tmp, "wb");
	if (!f) goto out;
	DWORD bytes = to - from + (bTo - bFrom), v;
	BYTE hdr[44] = { 0 };
	memcpy(hdr, "RIFF", 4); v = 36 + bytes; memcpy(hdr + 4, &v, 4);
	memcpy(hdr + 8, "WAVEfmt ", 8); v = 16; memcpy(hdr + 16, &v, 4);
	hdr[20] = 1; hdr[22] = (BYTE)rch; memcpy(hdr + 24, &rrate, 4);
	v = rrate * rch * 2; memcpy(hdr + 28, &v, 4); hdr[32] = (BYTE)(rch * 2); hdr[34] = 16;
	memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &bytes, 4);
	{ // 3 ms fades at both ends: the cut falls on the music, and a jump there clicks as it starts or loops
		short *s0 = (short *)(rd + from); DWORD n0 = (to - from) / frame, nb = (bTo - bFrom) / frame, ramp = rrate * 3 / 1000;
		short *last = nb ? (short *)(rd + bFrom) : s0; DWORD nl = nb ? nb : n0;
		for (DWORD i = 0; i < ramp && i < n0 && i < nl; i++)
			for (int c = 0; c < rch; c++) {
				s0[i * rch + c] = (short)(s0[i * rch + c] * (double)i / ramp);
				last[(nl - 1 - i) * rch + c] = (short)(last[(nl - 1 - i) * rch + c] * (double)i / ramp);
			}
	}
	BOOL ok = fwrite(hdr, 1, 44, f) == 44 && fwrite(rd + from, 1, to - from, f) == to - from &&
		(bTo == bFrom || fwrite(rd + bFrom, 1, bTo - bFrom, f) == bTo - bFrom);
	fclose(f);
	if (ok) ok = MoveFileExA(tmp, wav, MOVEFILE_REPLACE_EXISTING);
	if (!ok) DeleteFileA(tmp);
	if (ok) { char mark[MAX_PATH]; snprintf(mark, sizeof mark, "%s.cut", wav); FILE *m = fopen(mark, "w"); if (m) { fputs("cut as Lords2Resounded's edit\n", m); fclose(m); } }
	logf_("music: %s cut as Lords2Resounded's edit: the game's track at %.3f s, %.2f s long (match %.2f / %.2f)%s", track, a / 11025.0,
		bytes / (double)(rrate * rch * 2), sa, st, ok ? "" : " - could not write");
out:
	free(gd); free(rd);
}
static DWORD WINAPI music_convert_thread(void *arg)
{
	(void)arg;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
	char dir[MAX_PATH], pat[MAX_PATH];
	snprintf(dir, sizeof dir, "%s\\lords2hd\\music", g_gameDir);
	snprintf(pat, sizeof pat, "%s\\*", dir);
	WIN32_FIND_DATAA fd;
	HANDLE fh = FindFirstFileA(pat, &fd);
	if (fh == INVALID_HANDLE_VALUE) return 0;
	HMODULE plat = NULL, rw = NULL;
	MFStartup_t start = NULL; MFShutdown_t stop = NULL; MFCreateMediaType_t newtype = NULL;
	MFCreateSourceReaderFromURL_t open = NULL;
	BOOL com = FALSE, mf = FALSE;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		const char *ext = strrchr(fd.cFileName, '.');
		if (!ext || !lstrcmpiA(ext, ".wav") || !lstrcmpiA(ext, ".part")) continue;
		if (lstrcmpiA(ext, ".mp3") && lstrcmpiA(ext, ".flac") && lstrcmpiA(ext, ".m4a") && lstrcmpiA(ext, ".wma")) continue;
		const char *track = music_track(fd.cFileName);
		if (!track) { logf_("music: %s is not one of the game's tracks, left alone", fd.cFileName); continue; }
		char src[MAX_PATH], dst[MAX_PATH];
		snprintf(src, sizeof src, "%s\\%s", dir, fd.cFileName);
		snprintf(dst, sizeof dst, "%s\\%s", dir, track);
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (GetFileAttributesExA(dst, GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &fd.ftLastWriteTime) >= 0 &&
			(music_is_cut(dst, track) || music_edit(track) < 0))
			continue; // converted and cut before (a WAV not cut yet is decoded again: an earlier cut lost the lead-in)
		if (!open) {
			if (!com) com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
			plat = LoadLibraryA("mfplat.dll");
			rw = LoadLibraryA("mfreadwrite.dll");
			if (plat && rw) {
				start = (MFStartup_t)GetProcAddress(plat, "MFStartup");
				stop = (MFShutdown_t)GetProcAddress(plat, "MFShutdown");
				newtype = (MFCreateMediaType_t)GetProcAddress(plat, "MFCreateMediaType");
				open = (MFCreateSourceReaderFromURL_t)GetProcAddress(rw, "MFCreateSourceReaderFromURL");
			}
			if (!start || !stop || !newtype || !open || FAILED(start(MF_VERSION, MFSTARTUP_LITE))) {
				logf_("music: Windows Media Foundation is not available here, so MP3s can't be converted (WAVs still work)");
				open = NULL;
				break;
			}
			mf = TRUE;
		}
		if (music_decode(open, newtype, src, dst)) music_trim(dst, track);
	} while (FindNextFileA(fh, &fd));
	FindClose(fh);
	if (mf) stop();
	if (com) CoUninitialize();
	return 0;
}

static void music_status(void)
{
	IDirectSoundBuffer *b = STREAM_BUFS[0];
	DWORD st = 0, play = 0, write = 0, got = 0;
	WAVEFORMATEX wf = {0};
	if (!b) { logf_("music status: no stream buffer"); return; }
	IDirectSoundBuffer_GetStatus(b, &st);
	IDirectSoundBuffer_GetCurrentPosition(b, &play, &write);
	IDirectSoundBuffer_GetFormat(b, &wf, sizeof wf, &got);
	logf_("music status: %s, %lu Hz %u-bit, play cursor %lu, file %s", (st & DSBSTATUS_PLAYING) ? "playing" : "stopped",
		wf.nSamplesPerSec, wf.wBitsPerSample, play, (char *)STREAM_NAMES);
}

// The large clip over its slot in the doubled picture, wherever the slot still shows its last frame.
static void video_overlay(const PALETTEENTRY *pal, int sx, int sw)
{
	if (!g_vid.on || !g_vid.pub) return;
	int X0 = 2 * (g_vid.fx - sx), Y0 = 2 * g_vid.fy, W2 = 2 * VID_SLOT_W, H2 = 2 * VID_SLOT_H;
	if (g_vid.fx < sx || g_vid.fx + VID_SLOT_W > sx + sw || g_vid.fy < 0 || g_vid.fy + VID_SLOT_H > GAME_H) return;
	for (int Y = 0; Y < H2; Y++) {
		int oy = Y / 2;
		const BYTE *fr = g_frame8 + (g_vid.fy + oy) * sw + (g_vid.fx - sx), *sh = g_vid.shadow + oy * VID_SLOT_W;
		DWORD *o = g_frame2x + (Y0 + Y) * (2 * sw) + X0;
		// source row (bilinear): the centre of output row Y in the clip's rows
		double v = (Y + 0.5) * g_vid.h / H2 - 0.5;
		int y0 = v < 0 ? 0 : (int)v, y1 = y0 + 1 < g_vid.h ? y0 + 1 : y0;
		int fyw = (int)((v - y0) * 256); if (fyw < 0) fyw = 0;
		const BYTE *r0 = g_vid.pub + y0 * g_vid.w, *r1 = g_vid.pub + y1 * g_vid.w;
		for (int Xo = 0; Xo < W2; Xo++) {
			int ox = Xo / 2;
			if (fr[ox] != sh[ox]) continue; // something is drawn over the clip here
			double u = (Xo + 0.5) * g_vid.w / W2 - 0.5;
			int x0 = u < 0 ? 0 : (int)u, x1 = x0 + 1 < g_vid.w ? x0 + 1 : x0;
			int fxw = (int)((u - x0) * 256); if (fxw < 0) fxw = 0;
			const PALETTEENTRY *a = &pal[r0[x0]], *b = &pal[r0[x1]], *c = &pal[r1[x0]], *d = &pal[r1[x1]];
			int wa = (256 - fxw) * (256 - fyw), wb = fxw * (256 - fyw), wc = (256 - fxw) * fyw, wd = fxw * fyw;
			int R = (a->peRed * wa + b->peRed * wb + c->peRed * wc + d->peRed * wd) >> 16;
			int G = (a->peGreen * wa + b->peGreen * wb + c->peGreen * wc + d->peGreen * wd) >> 16;
			int B = (a->peBlue * wa + b->peBlue * wb + c->peBlue * wc + d->peBlue * wd) >> 16;
			o[Xo] = (R << 16) | (G << 8) | B;
		}
	}
}

// Over the doubled picture (sw wide, starting at frame column sx), the ground the layer still owns.
// the tile layer as of the frame on screen (tile_publish); the live one until the first copy
#define TL_ON (g_layerS != NULL)
#define TL_L (TL_ON ? g_layerS : g_layer)
#define TL_S (TL_ON ? g_layerShadowS : g_layerShadow)
#define TL_V (TL_ON ? g_layerValidS : g_layerValid)
#define TL_W (TL_ON && g_waveDS ? g_waveDS : g_waveD)
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
		const BYTE *sh = TL_S + y * MAX_W + sx, *va = TL_V + y * MAX_W + sx;
		const DWORD *h0 = TL_L + (2 * y) * (2 * MAX_W) + 2 * sx, *h1 = h0 + 2 * MAX_W;
		const BYTE *ss = SPR_S + y * MAX_W + sx, *sv = SPR_V + y * MAX_W + sx;
		const DWORD *s0 = SPR_L + (2 * y) * (2 * MAX_W) + 2 * sx, *s1 = s0 + 2 * MAX_W;
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
					const BYTE *w0 = TL_W + Y * (2 * MAX_W) + X, *w1 = w0 + 2 * MAX_W;
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
// The layer as of the frame on screen: the live one follows the game's back buffer, where a scroll
// being redrawn has lost its letters for a moment while the screen still shows the finished frame
// (all text blinked soft). Rows are copied over with the frame, under g_lock.

// The zoomed battle's HD layer as of the frame on screen, for the same reason as the letters: the
// game redraws the ground under the troops in its back buffer (which forgets their HD blocks) and
// draws them again, while the screen still shows the finished frame - every troop blinked between
// the HD and the original sprite. Rows are copied over with the frame, under g_lock.
static DWORD *g_btLayerS;
static BYTE *g_btShadowS, *g_btValidS;

// The same for the HD sprites of the map and the screens (dialogs, scrolls, the side panel): while
// the army division dialog counts troops across, the game rubs out and redraws its icons in the
// back buffer at every step, and each icon blinked between HD and the original.
static void spr_publish(int y0, int y1)
{
	if (!g_sprLayer) return;
	if (!g_sprLayerS) {
		// the presenter reads the tables as soon as the layer pointer is set: it is set last
		DWORD *l = malloc((size_t)MAX_W * 2 * GAME_H * 2 * 4);
		BYTE *s = calloc(MAX_W, GAME_H), *v = calloc(MAX_W, GAME_H);
		if (!l || !s || !v) { free(l); free(s); free(v); return; }
		g_sprShadowS = s; g_sprValidS = v;
		MemoryBarrier();
		g_sprLayerS = l;
		rows_dirty(g_sprDirty, 0, GAME_H);
	}
	int cw = min(MAX_W, g_frameW); // only the frame's columns (as tile_publish)
	for (int y = max(0, y0); y < min(GAME_H, y1); y++) {
		if (!g_sprDirty[y]) continue;
		g_sprDirty[y] = 0;
		for (int r = 0; r < 2; r++) {
			size_t o = (size_t)(2 * y + r) * 2 * MAX_W;
			memcpy(g_sprLayerS + o, g_sprLayer + o, (size_t)cw * 8);
		}
		memcpy(g_sprShadowS + (size_t)y * MAX_W, g_sprShadow + (size_t)y * MAX_W, cw);
		memcpy(g_sprValidS + (size_t)y * MAX_W, g_sprValid + (size_t)y * MAX_W, cw);
	}
}

// The ground tile layer as of the frame on screen, like the sprites (spr_publish). It was shown
// live: while the game redraws the next frame (a scroll moves every tile; a full redraw clears the
// layer first and stamps it at the end) the frame on screen and the layer disagreed, and the map
// showed its 1x pixels for a frame. Only rows written since their last copy are copied.
static void tile_dirty(int y0, int y1)
{
	for (int y = max(0, y0); y < min(GAME_H, y1); y++) g_layerDirty[y] = 1;
}
static void tile_publish(int y0, int y1)
{
	if (!g_layer || !g_layerShadow || !g_layerValid) return;
	if (!g_layerS) {
		DWORD *l = malloc((size_t)MAX_W * 2 * GAME_H * 2 * 4);
		BYTE *s = calloc(MAX_W, GAME_H), *v = calloc(MAX_W, GAME_H), *w = g_waveD ? calloc(MAX_W * 2, GAME_H * 2) : NULL;
		if (!l || !s || !v || (g_waveD && !w)) { free(l); free(s); free(v); free(w); return; }
		g_layerShadowS = s; g_layerValidS = v; g_waveDS = w;
		MemoryBarrier();
		g_layerS = l;
		tile_dirty(0, GAME_H);
	}
	int cw = min(MAX_W, g_frameW); // only the frame's columns: the rows are MAX_W long (a scroll copies every row)
	for (int y = max(0, y0); y < min(GAME_H, y1); y++) {
		if (!g_layerDirty[y]) continue;
		g_layerDirty[y] = 0;
		for (int r = 0; r < 2; r++) {
			size_t o = (size_t)(2 * y + r) * 2 * MAX_W;
			memcpy(g_layerS + o, g_layer + o, (size_t)cw * 8);
			if (g_waveDS) memcpy(g_waveDS + o, g_waveD + o, (size_t)cw * 2);
		}
		memcpy(g_layerShadowS + (size_t)y * MAX_W, g_layerShadow + (size_t)y * MAX_W, cw);
		memcpy(g_layerValidS + (size_t)y * MAX_W, g_layerValid + (size_t)y * MAX_W, cw);
	}
}

static void bt_publish(int y0, int y1)
{
	if (!g_btLayer || !battle_zoom_on()) return;
	if (!g_btLayerS) {
		DWORD *l = malloc(g_frameW * g_frameH * 4 * 4); // (set last, as in spr_publish)
		BYTE *s = calloc(g_frameW, g_frameH), *v = calloc(g_frameW, g_frameH);
		if (!l || !s || !v) { free(l); free(s); free(v); return; }
		g_btShadowS = s; g_btValidS = v;
		MemoryBarrier();
		g_btLayerS = l;
		rows_dirty(g_btDirty, 0, g_frameH);
	}
	for (int y = max(0, y0); y < min(min(g_frameH, DIRTY_ROWS), y1); y++) {
		if (!g_btDirty[y]) continue;
		g_btDirty[y] = 0;
		memcpy(g_btLayerS + (size_t)y * g_frameW * 4, g_btLayer + (size_t)y * g_frameW * 4, (size_t)g_frameW * 16);
		memcpy(g_btShadowS + (size_t)y * g_frameW, g_btShadow + (size_t)y * g_frameW, g_frameW);
		memcpy(g_btValidS + (size_t)y * g_frameW, g_btValid + (size_t)y * g_frameW, g_frameW);
	}
}

static void text_publish(int y0, int y1)
{
	if (!g_txt || !g_back) return;
	int W = g_txtW, H = g_txtH;
	if (!g_txtS) {
		BYTE *l = malloc(W * H * 4), *s = calloc(W, H), *v = calloc(W, H); // (set last, as in spr_publish)
		if (!l || !s || !v) { free(l); free(s); free(v); return; }
		g_txtShadowS = s; g_txtValidS = v;
		MemoryBarrier();
		g_txtS = l;
		rows_dirty(g_txtDirty, 0, H);
	}
	for (int y = max(0, y0); y < min(min(H, DIRTY_ROWS), y1); y++) {
		if (!g_txtDirty[y]) continue;
		g_txtDirty[y] = 0;
		memcpy(g_txtS + y * W * 4, g_txt + y * W * 4, W * 4);
		memcpy(g_txtShadowS + y * W, g_txtShadow + y * W, W);
		memcpy(g_txtValidS + y * W, g_txtValid + y * W, W);
	}
}

static void text_clear(int x, int y, int w, int h)
{
	if (!g_txtValid || !g_back) return;
	x += g_back->org;
	int x0 = max(x, 0), x1 = min(x + w, g_back->w);
	if (x1 <= x0) return;
	for (int r = max(y, 0); r < min(y + h, g_back->h); r++) memset(g_txtValid + r * g_back->w + x0, 0, x1 - x0);
	rows_dirty(g_txtDirty, y, y + h);
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
	rows_dirty(g_txtDirty, 0, g_back->h);
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
		g_txtW = W; g_txtH = H;
	}
	if (W != g_txtW || H > g_txtH) return; // (a back buffer of another size: the layers are not its)
#define ON(c, r) ((c) >= 0 && (r) >= 0 && (c) < w && (r) < h && src[(r) * w + (c)] != 0)
	rows_dirty(g_txtDirty, y, y + h);
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

// The patch's own scrolls (the army scroll, the settings scroll) are drawn by the game's routines into
// its picture, where the game may draw over them (troops, flags) in the moment before a frame is
// shown. So once a frame shows one intact, its finished HD pixels are kept and laid over every frame
// after, above everything: on = up, gen = what it shows (a new gen is caught again).
typedef struct { volatile LONG on, gen, mapPal; int x, y, w, h; const BYTE *snap, *under; DWORD *cache; int cacheW, cacheH; LONG cached; } SCROLL_OVL;
static SCROLL_OVL g_ovl[2]; // 0 the army scroll, 1 the settings scroll
// The settings scroll on a screen with its own palette (the main menu): its art and lettering are the
// county map's, so its pixels are made here in the map's colours (Base01.256): the HD sprite pixels
// (made in those colours already), the sharp letters' layer, and else the game's pixel doubled.
static DWORD g_mapLut[256];
static BOOL map_lut(void)
{
	static int have;
	if (have) return have > 0;
	BYTE pal[768];
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%s\\Base01.256", g_gameDir);
	FILE *f = fopen(path, "rb");
	have = f && fread(pal, 1, 768, f) == 768 ? 1 : -1;
	if (f) fclose(f);
	for (int i = 0; have > 0 && i < 256; i++)
		g_mapLut[i] = (pal[i * 3] * 255 / 63 << 16) | (pal[i * 3 + 1] * 255 / 63 << 8) | pal[i * 3 + 2] * 255 / 63;
	return have > 0;
}
static void ovl_build_mappal(SCROLL_OVL *s, int sx, int sw, int c0)
{
	if (!map_lut()) return;
	int W = g_back->w;
	if (g_txt && W != g_txtW) return; // (text layers made for another back buffer size)
	const BYTE *tx = g_txtS ? g_txtS : g_txt, *tsh = g_txtS ? g_txtShadowS : g_txtShadow, *tv = g_txtS ? g_txtValidS : g_txtValid;
	for (int r = 0; r < s->h; r++) {
		int y = s->y + r;
		const BYTE *fr = g_frame8 + y * sw + c0;
		DWORD *o0 = s->cache + (2 * r) * (2 * s->w), *o1 = o0 + 2 * s->w;
		for (int c = 0; c < s->w; c++) {
			int x = c0 + c; // frame column
			BOOL spr = g_sprLayer && SPR_V[y * MAX_W + sx + x] && fr[c] == SPR_S[y * MAX_W + sx + x];
			if (spr) {
				const DWORD *h0 = SPR_L + (2 * y) * (2 * MAX_W) + 2 * (sx + x), *h1 = h0 + 2 * MAX_W;
				o0[2 * c] = h0[0]; o0[2 * c + 1] = h0[1]; o1[2 * c] = h1[0]; o1[2 * c + 1] = h1[1];
				continue;
			}
			int f = y * W + sx + x;
			if (tx && g_hdText && tv[f] && fr[c] == tsh[f]) {
				const BYTE *q = tx + f * 4;
				o0[2 * c] = g_mapLut[q[0]]; o0[2 * c + 1] = g_mapLut[q[1]]; o1[2 * c] = g_mapLut[q[2]]; o1[2 * c + 1] = g_mapLut[q[3]];
				continue;
			}
			o0[2 * c] = o0[2 * c + 1] = o1[2 * c] = o1[2 * c + 1] = g_mapLut[fr[c]];
		}
	}
}
static void scroll_overlays(int sx, int sw)
{
	BOOL show[2] = { FALSE, FALSE };
	int col[2] = { 0, 0 };
	for (int i = 0; i < 2; i++) { // first catch each from the frame as the game made it...
		SCROLL_OVL *s = &g_ovl[i];
		if (!s->on || !s->snap) continue;
		int c0 = s->x + g_off - sx;
		if (c0 < 0 || c0 + s->w > sw || s->y < 0 || s->y + s->h > GAME_H) continue;
		LONG gen = s->gen;
		BOOL whole = TRUE; // whole in this frame? Then caught again (so fades and palette changes reach it too)
		for (int r = 0; r < s->h && whole; r++) whole = !memcmp(g_frame8 + (s->y + r) * sw + c0, s->snap + r * s->w, s->w);
		if (!whole && !s->cached) continue; // never seen whole: the game's own drawing stands (else the last copy, until the new one is seen: no flicker on a change)
		if (whole) {
			if (!s->cache || s->cacheW < s->w || s->cacheH < s->h) {
				free(s->cache);
				s->cache = malloc((size_t)s->w * s->h * 16);
				if (!s->cache) continue;
				s->cacheW = s->w; s->cacheH = s->h;
			}
			if (s->mapPal) ovl_build_mappal(s, sx, sw, c0);
			else for (int r = 0; r < 2 * s->h; r++)
				memcpy(s->cache + r * 2 * s->w, g_frame2x + (2 * s->y + r) * (2 * sw) + 2 * c0, (size_t)s->w * 8);
			s->cached = gen + 1;
		}
		show[i] = TRUE; col[i] = c0;
	}
	for (int i = 0; i < 2; i++) { // ... then lay them on, the settings scroll last: only the scroll's own
		SCROLL_OVL *s = &g_ovl[i];   // pixels (where it differs from what it covers), so the live map
		if (!show[i]) continue;      // shows through its ragged edges instead of a frozen strip of it
		for (int r = 0; r < s->h; r++) {
			const BYTE *sn = s->snap + r * s->w, *un = s->under ? s->under + r * s->w : NULL;
			DWORD *o0 = g_frame2x + (2 * (s->y + r)) * (2 * sw) + 2 * col[i], *o1 = o0 + 2 * sw;
			const DWORD *c0 = s->cache + (2 * r) * (2 * s->w), *c1 = c0 + 2 * s->w;
			for (int c = 0; c < s->w; c++) {
				if (un && sn[c] == un[c]) continue;
				o0[2 * c] = c0[2 * c]; o0[2 * c + 1] = c0[2 * c + 1];
				o1[2 * c] = c1[2 * c]; o1[2 * c + 1] = c1[2 * c + 1];
			}
		}
	}
}
static void text_overlay(int sx, int sw)
{
	if (!g_txt || !g_hdText) return;
	int W = g_txtW;
	if (sx < 0 || sx + sw > W || GAME_H > g_txtH || W != g_back->w) return; // (layers made for another back buffer size)
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

// ---- The settings panel (F10) ---------------------------------------------------------------------
// Drawn by the patch over the picture; keys and clicks go to it while it is open, not to the game.
// Changes apply at once where they can and are written to lords2hd.ini. Two tabs: the picture, and
// the conveniences the patch adds to the game. (F8 is the game's own: it goes back to the last turn.)
static int g_menuSel, g_menuTab;
static BOOL g_showScreens = TRUE, g_showTiles = TRUE; // the AI art shown (loaded only if on at start)
#define MENU_TABS 3
static RECT g_menuRow[16], g_menuTabRect[MENU_TABS], g_menuBox; // in game coordinates, as last drawn
static int g_menuRows;
static BOOL g_wantWideSet, g_battleZoomSet;
enum { M_UPSCALE, M_SCREENS, M_TILES, M_TEXT, M_WAVES, M_LIMIT, M_TUTORIAL, M_WASD, M_CYCLE, M_WIDE, M_ZOOM, M_ARMY, M_LETTERS, M_RESOUND, M_RENDERER, M_FPS, M_MINIPOP, M_COUNT };
#define MENU_ROWS 8 // the most rows a tab has: the panel's height
static const char *const kMenuTab[MENU_TABS] = { "Display", "Performance", "Quality of life" };
static const int kMenuTabItems[MENU_TABS][MENU_ROWS + 1] = { // -1 ends a tab
	{ M_SCREENS, M_TILES, M_TEXT, M_WAVES, M_WIDE, M_ZOOM, M_MINIPOP, -1 },
	{ M_UPSCALE, M_RENDERER, M_LIMIT, M_FPS, -1 },
	{ M_TUTORIAL, M_WASD, M_CYCLE, M_ARMY, M_LETTERS, M_RESOUND, -1 },
};
static int menu_tab_count(int tab) { int n = 0; while (n < MENU_ROWS && kMenuTabItems[tab][n] >= 0) n++; return n; }
static BOOL menu_new(int i) { return i == M_MINIPOP; } // marked "new"
static void menu_value(int i, char *v, int n, BOOL *restart)
{
	*restart = FALSE;
	switch (i) {
	case M_UPSCALE: snprintf(v, n, "%s", g_upscaleCR == 2 ? "AMD FSR 1 (sharpest)" : g_upscaleCR ? "Catmull-Rom (sharp)" : "Bilinear (soft)"); break;
	case M_SCREENS: snprintf(v, n, "%s", g_showScreens ? "On" : "Off"); *restart = g_showScreens && !g_hdScreens; break;
	case M_TILES: snprintf(v, n, "%s", g_showTiles ? "On" : "Off"); *restart = g_showTiles && !g_hdTiles; break;
	case M_TEXT: snprintf(v, n, "%s", g_hdText ? "On" : "Off"); break;
	case M_WAVES: snprintf(v, n, "%s", g_shoreWaves ? "On" : "Off"); break;
	case M_LIMIT: snprintf(v, n, "%s", g_frameLimit ? "120 a second" : "Off (as the original)"); break;
	case M_TUTORIAL: snprintf(v, n, "%s", !*(int *)0x0053f24c ? "Off" : g_tutorialOnce ? "Once" : "Every time"); break; // Off: the game's own option (0x53f24c)
	case M_WASD: snprintf(v, n, "%s", g_wasdOn ? "On" : "Off"); break;
	case M_CYCLE: snprintf(v, n, "%s", g_cycleOn ? "On" : "Off"); break;
	case M_WIDE: snprintf(v, n, "%s", g_wantWideSet ? "On" : "Off"); *restart = g_wantWideSet != g_wantWide; break;
	case M_ZOOM: snprintf(v, n, "%s", g_battleZoomSet ? "On" : "Off"); *restart = g_battleZoomSet != g_battleZoom; break;
	case M_ARMY: snprintf(v, n, "%s", g_armyPanel ? "On" : "Off"); break;
	case M_LETTERS: snprintf(v, n, "%s", g_letters ? "On" : "Off"); break;
	case M_RESOUND: snprintf(v, n, "%s", g_resounded ? "On" : "Off"); break;
	case M_RENDERER: // a restart: once the GPU has drawn into the window, Windows no longer shows the CPU's drawing there
		snprintf(v, n, "%s", g_gpuWantedSet ? (g_gpuWanted && !gpu_drawing() ? "Graphics card (failed)" : "Graphics card") : "Processor");
		*restart = g_gpuWantedSet != g_gpuWanted;
		break;
	case M_FPS: snprintf(v, n, "%s", g_statsOn == 2 ? "Detailed" : g_statsOn ? "Simple" : "Off"); break;
	case M_MINIPOP: snprintf(v, n, "%s", g_miniPop ? "On" : "Off"); break;
	}
}
static const char *const kMenuLabel[M_COUNT] = { "Upscaling", "AI art: paintings", "AI art: map, units", "Sharp lettering",
	"Shore waves", "Frame limit", "Tutorial scrolls", "WASD scrolling", ", and . cycle", "Widescreen *", "Battle zoom-out *",
	"Army panel", "Fresh letters", "Lords2Resounded", "Drawing", "FPS counter", "Minimap outlines" };
static void ini_set(const char *sec, const char *key, int v)
{
	char s[16]; snprintf(s, sizeof s, "%d", v);
	WritePrivateProfileStringA(sec, key, s, g_iniPath);
}
// Turning Lords2Resounded on or off says a maceman's "We'll enjoy this!": the restored line (loose or
// from the pack) when on, the game's own when off, so the difference is heard at once.
static void resound_sample(void)
{
	static const char kLine[] = "Mace_u1.wav";
	static BYTE *mem;
	char path[MAX_PATH];
	if (g_remote) return; // muted while testing
	PlaySoundA(NULL, NULL, 0);
	if (g_resounded) {
		snprintf(path, sizeof path, "%s\\lords2hd\\sound\\%s", g_gameDir, kLine);
		if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT); return; }
		AFILE *f = afopen(path);
		if (f) {
			DWORD size = f->f ? 0 : f->size;
			BYTE *buf = size ? malloc(size) : NULL;
			BOOL ok = buf && afread(buf, 1, size, f) == size;
			afclose(f);
			if (ok) { free(mem); mem = buf; PlaySoundA((LPCSTR)mem, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); return; }
			free(buf);
		}
	}
	snprintf(path, sizeof path, "%s\\%s", g_gameDir, kLine);
	PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}static void menu_change(int i)
{
	switch (i) {
	case M_UPSCALE: g_upscaleCR = (g_upscaleCR + 1) % 3; WritePrivateProfileStringA("Display", "Upscale", g_upscaleCR == 2 ? "fsr" : g_upscaleCR ? "catmullrom" : "bilinear", g_iniPath); break;
	case M_SCREENS: g_showScreens = !g_showScreens; ini_set("Display", "HDScreens", g_showScreens); break;
	case M_TILES: g_showTiles = !g_showTiles; ini_set("Display", "HDTiles", g_showTiles); break;
	case M_TEXT: g_hdText = !g_hdText; ini_set("Display", "SharpText", g_hdText); break;
	case M_WAVES: g_shoreWaves = !g_shoreWaves; ini_set("Display", "ShoreWaves", g_shoreWaves); break;
	case M_LIMIT: g_frameLimit = g_frameLimit ? 0 : 120; ini_set("Display", "FrameLimit", g_frameLimit); break;
	case M_TUTORIAL: // Off (the game's own tutorial option, 0x53f24c, saved in lords2.inf) -> Once -> Every time
		if (!*(int *)0x0053f24c) { *(int *)0x0053f24c = 1; g_tutorialOnce = TRUE; ((void (__cdecl *)(void))0x00476a5d)(); } // on, as the game's own switch does (its marks cleared)
		else if (g_tutorialOnce) g_tutorialOnce = FALSE;
		else { *(int *)0x0053f24c = 0; g_tutorialOnce = TRUE; }
		ini_set("Display", "TutorialOnce", g_tutorialOnce);
		if (*(int *)0x0053f24c && !g_tutorialOnce) { // Every time: shown again from now on - the game's marks for this session cleared too
			memset((BYTE *)0x004f0298 + 200, 0, 32);
			WritePrivateProfileStringA("Tutorial", "Seen", NULL, g_iniPath);
		}
		break;
	case M_WASD: g_wasdOn = !g_wasdOn; ini_set("Display", "WASD", g_wasdOn); break;
	case M_CYCLE: g_cycleOn = !g_cycleOn; ini_set("Display", "CycleKeys", g_cycleOn); break;
	case M_WIDE: g_wantWideSet = !g_wantWideSet; ini_set("Display", "Wide", g_wantWideSet); break;
	case M_ZOOM: g_battleZoomSet = !g_battleZoomSet; ini_set("Display", "BattleZoom", g_battleZoomSet); break;
	case M_ARMY: g_armyPanel = !g_armyPanel; ini_set("Display", "ArmyPanel", g_armyPanel); break;
	case M_LETTERS: g_letters = !g_letters; ini_set("Display", "Letters", g_letters); break;
	case M_RESOUND: g_resounded = !g_resounded; ini_set("Display", "Resounded", g_resounded); resound_sample(); break;
	case M_RENDERER:
		g_gpuWantedSet = !g_gpuWantedSet;
		WritePrivateProfileStringA("Display", "Renderer", g_gpuWantedSet ? "gpu" : "cpu", g_iniPath);
		break;
	case M_FPS: InterlockedExchange(&g_statsOn, (g_statsOn + 1) % 3); ini_set("Display", "FpsCounter", g_statsOn); break;
	case M_MINIPOP: g_miniPop = !g_miniPop; ini_set("Display", "MinimapPop", g_miniPop); break;
	}
	logf_("settings panel: %s changed", kMenuLabel[i]);
}
static volatile LONG g_menuGen; // bumped at every change: the scroll is drawn again
static void menu_scroll_keep(void);
// at once, not at the game's next frame (the main menu shows few)
static void menu_redraw(void) { InterlockedIncrement(&g_menuGen); menu_scroll_keep(); InterlockedExchange(&g_forcePresent, 1); mark_dirty(); }
static void menu_toggle(void)
{
	if (!g_menuOpen) {
		g_wantWideSet = GetPrivateProfileIntA("Display", "Wide", 0, g_iniPath) != 0;
		g_battleZoomSet = GetPrivateProfileIntA("Display", "BattleZoom", 1, g_iniPath) != 0;
	}
	InterlockedExchange(&g_menuOpen, !g_menuOpen);
	if (g_menuOpen) InterlockedExchange(&g_ovl[0].on, 0); // the army scroll goes while the panel is up (army_panel_keep)
	menu_redraw();
}
static void menu_set_tab(int tab) { g_menuTab = (tab + MENU_TABS) % MENU_TABS; g_menuSel = 0; }
// A key while the panel is open (or F10 to open it). TRUE: it was the panel's.
static const char *upscale_name(void) { return g_upscaleCR == 2 ? "FSR 1" : g_upscaleCR ? "Catmull-Rom" : "bilinear"; }
static BOOL menu_key(int vk)
{
	if (vk == VK_F10 && GetKeyState(VK_CONTROL) < 0) { // Ctrl+F10: the FPS counter, off / simple / detailed
		InterlockedExchange(&g_statsOn, (g_statsOn + 1) % 3);
		ini_set("Display", "FpsCounter", g_statsOn);
		logf_("FPS counter: %d", (int)g_statsOn);
		if (g_menuOpen) menu_redraw();
		InterlockedExchange(&g_forcePresent, 1); mark_dirty();
		return TRUE;
	}
	if (vk == VK_F10) { menu_toggle(); return TRUE; }
	if (!g_menuOpen) return FALSE;
	int n = menu_tab_count(g_menuTab);
	switch (vk) {
	case VK_ESCAPE: menu_toggle(); break;
	case VK_TAB: menu_set_tab(g_menuTab + (GetKeyState(VK_SHIFT) < 0 ? -1 : 1)); break;
	case VK_UP: g_menuSel = (g_menuSel + n - 1) % n; break;
	case VK_DOWN: g_menuSel = (g_menuSel + 1) % n; break;
	case VK_RETURN: case VK_SPACE: case VK_LEFT: case VK_RIGHT: menu_change(kMenuTabItems[g_menuTab][g_menuSel]); break;
	}
	menu_redraw();
	return TRUE;
}
// Where the panel is drawn as a game scroll (menu_scroll_keep): everywhere but in battles, which have
// a drawing layer of their own; there it is the plain panel, drawn over the picture (menu_draw).
static BOOL menu_as_scroll(void) { return TRUE; }
static int g_msDrawn; // the scroll is up (menu_scroll_keep)
static RECT g_menuRowW[16], g_menuTabRectW[MENU_TABS], g_menuBoxW; // the plain panel's, in window coordinates
// A click while the panel is open: window and game coordinates.
static void menu_click(int wx, int wy, int gx, int gy)
{
	BOOL sc = g_msDrawn;
	POINT p = { sc ? gx : wx, sc ? gy : wy };
	const RECT *tabs = sc ? g_menuTabRect : g_menuTabRectW, *rows = sc ? g_menuRow : g_menuRowW, *box = sc ? &g_menuBox : &g_menuBoxW;
	for (int k = 0; k < MENU_TABS; k++) if (PtInRect(&tabs[k], p)) { if (k != g_menuTab) menu_set_tab(k); menu_redraw(); return; }
	for (int i = 0; i < g_menuRows; i++)
		if (PtInRect(&rows[i], p)) { g_menuSel = i; menu_change(kMenuTabItems[g_menuTab][i]); menu_redraw(); return; }
	if (!PtInRect(box, p)) menu_toggle(); // outside closes it
}
// The plain panel, where the game's colours are not the county map's (menus, battles): onto a DC
// holding the picture (vw x vh), at window offset ox, oy.
static void menu_draw(HDC dc, int vw, int vh, int ox, int oy)
{
	int fh = max(16, vh / 34), pad = fh, rowH = fh * 3 / 2, labelW = fh * 10, valueW = fh * 12;
	int bw = pad * 2 + labelW + valueW, bh = pad * 2 + rowH * (MENU_ROWS + 6);
	int bx = (vw - bw) / 2, by = (vh - bh) / 2;
	COLORREF cPaper = RGB(222, 196, 140), cHi = RGB(244, 226, 180), cLine = RGB(120, 80, 40);
	HBRUSH paper = CreateSolidBrush(cPaper), edge = CreateSolidBrush(RGB(92, 52, 24)), hi = CreateSolidBrush(cHi);
	HBRUSH back = CreateSolidBrush(RGB(200, 172, 118)), line = CreateSolidBrush(cLine);
	RECT r = { bx - fh / 4, by - fh / 4, bx + bw + fh / 4, by + bh + fh / 4 };
	FillRect(dc, &r, edge);
	r = (RECT){ bx, by, bx + bw, by + bh };
	FillRect(dc, &r, paper);
	SetRect(&g_menuBoxW, ox + r.left, oy + r.top, ox + r.right, oy + r.bottom);
	HFONT title = CreateFontA(fh * 3 / 2, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Georgia");
	HFONT font = CreateFontA(fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Georgia");
	HFONT bold = CreateFontA(fh, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Georgia");
	HFONT small = CreateFontA(fh * 4 / 5, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Georgia");
	HGDIOBJ old = SelectObject(dc, title);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(120, 20, 10));
	RECT t = { bx, by + pad / 2, bx + bw, by + pad / 2 + rowH * 3 / 2 };
	DrawTextA(dc, "Lords of the Realm II HD", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	// the tabs, on a line across the panel; the open one joins the paper below it
	int ty = by + pad / 2 + rowH * 2, th = rowH * 5 / 4, tw = fh * 9, gap = fh / 2, tx0 = bx + (bw - MENU_TABS * tw - (MENU_TABS - 1) * gap) / 2, lw = max(1, fh / 12);
	RECT ln = { bx + pad / 2, ty + th - lw, bx + bw - pad / 2, ty + th };
	FillRect(dc, &ln, line);
	for (int k = 0; k < MENU_TABS; k++) {
		RECT tr = { tx0 + k * (tw + gap), ty, tx0 + k * (tw + gap) + tw, ty + th };
		SetRect(&g_menuTabRectW[k], ox + tr.left, oy + tr.top, ox + tr.right, oy + tr.bottom);
		if (k == g_menuTab) {
			RECT o = { tr.left - lw, tr.top - lw, tr.right + lw, tr.bottom };
			FillRect(dc, &o, line);
			RECT in = { tr.left, tr.top, tr.right, tr.bottom + lw };
			FillRect(dc, &in, hi);
			SelectObject(dc, bold);
			SetTextColor(dc, RGB(120, 20, 10));
		}
		else {
			RECT in = { tr.left, tr.top, tr.right, tr.bottom - lw };
			FillRect(dc, &in, back);
			SelectObject(dc, font);
			SetTextColor(dc, RGB(110, 80, 45));
		}
		DrawTextA(dc, kMenuTab[k], -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	int n = menu_tab_count(g_menuTab);
	if (g_menuSel >= n) g_menuSel = 0;
	g_menuRows = n;
	for (int i = 0; i < n; i++) {
		int item = kMenuTabItems[g_menuTab][i];
		int y = ty + th + rowH / 2 + rowH * i;
		RECT row = { bx + pad / 2, y, bx + bw - pad / 2, y + rowH };
		if (i == g_menuSel) FillRect(dc, &row, hi);
		SetRect(&g_menuRowW[i], ox + row.left, oy + row.top, ox + row.right, oy + row.bottom);
		char v[64]; BOOL restart;
		menu_value(item, v, sizeof v, &restart);
		SelectObject(dc, font);
		SetTextColor(dc, RGB(40, 24, 10));
		RECT a = { bx + pad, y, bx + pad + labelW, y + rowH };
		DrawTextA(dc, kMenuLabel[item], -1, &a, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		if (restart) strncat(v, "  (restart)", sizeof v - strlen(v) - 1);
		SetTextColor(dc, restart ? RGB(150, 60, 20) : RGB(20, 60, 20));
		RECT b = { bx + pad + labelW, y, bx + bw - pad, y + rowH };
		DrawTextA(dc, v, -1, &b, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		if (menu_new(item)) {
			SelectObject(dc, small);
			SetTextColor(dc, RGB(120, 20, 10));
			DrawTextA(dc, "new", -1, &b, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		}
	}
	SelectObject(dc, small);
	SetTextColor(dc, RGB(80, 56, 30));
	if (g_menuTab == 0) {
		RECT f1 = { bx, by + bh - pad / 2 - 2 * rowH, bx + bw, by + bh - pad / 2 - rowH };
		DrawTextA(dc, "* takes effect after a restart; everything else at once", -1, &f1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	RECT f = { bx, by + bh - pad / 2 - rowH, bx + bw, by + bh - pad / 2 };
	DrawTextA(dc, "Tab switches tabs  -  Click or Enter to change  -  F10 or Esc to close", -1, &f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(dc, old);
	DeleteObject(title); DeleteObject(font); DeleteObject(bold); DeleteObject(small);
	DeleteObject(paper); DeleteObject(edge); DeleteObject(hi); DeleteObject(back); DeleteObject(line);
}


// The picture onto the window: src (sw x sh, BGRX) stretched to the view v. Catmull-Rom into a
// window-sized buffer, or GDI with gdiMode (HALFTONE / COLORONCOLOR) as before; the settings panel
// on top when open. The buffer is a DIB section, so GDI can draw the panel straight into it.
static HDC g_outDC;
static HBITMAP g_outBmp;
static DWORD *g_outBits;
static int g_outW, g_outH;
static volatile LONG g_shotViewWanted;

// ---- The GPU path ([Display] Renderer=gpu, the default) -----------------------------------------
// The picture the CPU composed (the 2x frame with the HD art in it) goes up as a texture and the
// graphics card stretches it to the window: bilinear, Catmull-Rom, or FSR 1 (EASU to the window's
// size, then RCAS), in shaders compiled at start with Windows' own d3dcompiler_47.dll. A flip-model
// swap chain on the game's window; the presenter thread owns the device. Anything failing (no
// Direct3D 11, a lost device) falls back to the CPU path for the rest of the session.
static int g_gpuWanted = 1;
static const char kShaders[] =
"Texture2D<float4> T : register(t0);\n"
"SamplerState S : register(s0);\n"
"cbuffer C : register(b0) { float4 A; float4 B; };\n" // A: source w, h, output w, h; B: output origin x, y, RCAS scale
"float3 L3(int2 p) { return T.Load(int3(clamp(p, int2(0, 0), int2(A.xy) - 1), 0)).rgb; }\n"
"float LU(float3 c) { return 0.5 * c.r + c.g + 0.5 * c.b; }\n"
"float4 vs(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"void setF(inout float2 dir, inout float len, float w, float la, float lb, float lc, float ld, float le) {\n"
"  float lx = max(abs(ld - lc), abs(lc - lb)); float dx = ld - lb; dir.x += dx * w;\n"
"  lx = lx > 0 ? saturate(abs(dx) / lx) : 0; len += lx * lx * w;\n"
"  float ly = max(abs(le - lc), abs(lc - la)); float dy = le - la; dir.y += dy * w;\n"
"  ly = ly > 0 ? saturate(abs(dy) / ly) : 0; len += ly * ly * w; }\n"
"void tapF(inout float3 ac, inout float aw, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c) {\n"
"  float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;\n"
"  float d2 = min(dot(v, v), clp); float wb = 0.4 * d2 - 1; float wa = lob * d2 - 1; wb *= wb; wa *= wa;\n"
"  float w = (1.5625 * wb - 0.5625) * wa; aw += w; ac += c * w; }\n"
"float4 easu(float4 pos : SV_Position) : SV_Target {\n"
"  float2 pp = (pos.xy - B.xy) * (A.xy / A.zw) - 0.5; float2 fp = floor(pp); pp -= fp; int2 q = int2(fp);\n"
"  float3 cb = L3(q + int2(0, -1)), cc = L3(q + int2(1, -1)), ce = L3(q + int2(-1, 0)), cf = L3(q), cg = L3(q + int2(1, 0)), ch = L3(q + int2(2, 0));\n"
"  float3 ci = L3(q + int2(-1, 1)), cj = L3(q + int2(0, 1)), ck = L3(q + int2(1, 1)), cl = L3(q + int2(2, 1)), cn = L3(q + int2(0, 2)), co = L3(q + int2(1, 2));\n"
"  float2 dir = 0; float len = 0;\n"
"  setF(dir, len, (1 - pp.x) * (1 - pp.y), LU(cb), LU(ce), LU(cf), LU(cg), LU(cj));\n"
"  setF(dir, len, pp.x * (1 - pp.y), LU(cc), LU(cf), LU(cg), LU(ch), LU(ck));\n"
"  setF(dir, len, (1 - pp.x) * pp.y, LU(cf), LU(ci), LU(cj), LU(ck), LU(cn));\n"
"  setF(dir, len, pp.x * pp.y, LU(cg), LU(cj), LU(ck), LU(cl), LU(co));\n"
"  float dr = dot(dir, dir); bool z = dr < 1.0 / 32768; dr = z ? 1 : rsqrt(dr); if (z) dir.x = 1; dir *= dr;\n"
"  len *= 0.5; len *= len;\n"
"  float st = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
"  float2 len2 = float2(1 + (st - 1) * len, 1 - 0.5 * len); float lob = 0.5 + (0.25 - 0.04 - 0.5) * len; float clp = 1 / lob;\n"
"  float3 ac = 0; float aw = 0;\n"
"  tapF(ac, aw, float2(0, -1) - pp, dir, len2, lob, clp, cb); tapF(ac, aw, float2(1, -1) - pp, dir, len2, lob, clp, cc);\n"
"  tapF(ac, aw, float2(-1, 1) - pp, dir, len2, lob, clp, ci); tapF(ac, aw, float2(0, 1) - pp, dir, len2, lob, clp, cj);\n"
"  tapF(ac, aw, float2(0, 0) - pp, dir, len2, lob, clp, cf); tapF(ac, aw, float2(-1, 0) - pp, dir, len2, lob, clp, ce);\n"
"  tapF(ac, aw, float2(1, 1) - pp, dir, len2, lob, clp, ck); tapF(ac, aw, float2(2, 1) - pp, dir, len2, lob, clp, cl);\n"
"  tapF(ac, aw, float2(2, 0) - pp, dir, len2, lob, clp, ch); tapF(ac, aw, float2(1, 0) - pp, dir, len2, lob, clp, cg);\n"
"  tapF(ac, aw, float2(1, 2) - pp, dir, len2, lob, clp, co); tapF(ac, aw, float2(0, 2) - pp, dir, len2, lob, clp, cn);\n"
"  float3 mn = min(min(cf, cg), min(cj, ck)), mx = max(max(cf, cg), max(cj, ck));\n"
"  return float4(clamp(ac / aw, mn, mx), 1); }\n"
"float4 rcas(float4 pos : SV_Position) : SV_Target {\n"
"  int2 q = int2(pos.xy - B.xy);\n"
"  float3 b = L3(q + int2(0, -1)), d = L3(q + int2(-1, 0)), e = L3(q), f = L3(q + int2(1, 0)), h = L3(q + int2(0, 1));\n"
"  float bl = LU(b), dl = LU(d), el = LU(e), fl = LU(f), hl = LU(h);\n"
"  float nz = 0.25 * (bl + dl + fl + hl) - el;\n"
"  float rg = max(max(max(bl, dl), max(el, fl)), hl) - min(min(min(bl, dl), min(el, fl)), hl);\n"
"  nz = -0.5 * saturate(abs(nz) / max(rg, 1.0 / 4096)) + 1;\n"
"  float3 mn = min(min(b, d), min(f, h)), mx = max(max(b, d), max(f, h));\n"
"  float3 hmin = min(mn, e) / (4 * mx + 1.0 / 4096), hmax = (1 - max(mx, e)) / (4 * mn - 4 - 1.0 / 4096);\n"
"  float3 lb = max(-hmin, hmax); float lobe = max(-0.1875, min(max(lb.r, max(lb.g, lb.b)), 0)) * B.z * nz;\n"
"  return float4(saturate((lobe * (b + d + f + h) + e) / (4 * lobe + 1)), 1); }\n"
"float4 wts(float t) { float t2 = t * t, t3 = t2 * t; return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2); }\n"
"float4 cr(float4 pos : SV_Position) : SV_Target {\n"
"  float2 pp = (pos.xy - B.xy) * (A.xy / A.zw) - 0.5; float2 fp = floor(pp); float2 t = pp - fp; int2 q = int2(fp);\n"
"  float4 wx = wts(t.x), wy = wts(t.y); float3 c = 0;\n"
"  [unroll] for (int j = 0; j < 4; j++) { float3 r = 0; [unroll] for (int i = 0; i < 4; i++) r += L3(q + int2(i - 1, j - 1)) * wx[i]; c += r * wy[j]; }\n"
"  return float4(saturate(c), 1); }\n"
"float4 bil(float4 pos : SV_Position) : SV_Target { return float4(T.SampleLevel(S, (pos.xy - B.xy) / A.zw, 0).rgb, 1); }\n"
"float4 key(float4 pos : SV_Position) : SV_Target {\n" // the overlay: magenta is see-through
"  float3 c = T.Load(int3(int2(pos.xy - B.xy), 0)).rgb; if (c.r > 0.99 && c.b > 0.99 && c.g < 0.01) discard; return float4(c, 1); }\n";

static const GUID kIID_Tex2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID kIID_DXGIFactory = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const GUID kIID_DXGIDevice1 = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
enum { GP_EASU, GP_RCAS, GP_CR, GP_BIL, GP_KEY, GP_N };
#define GPU_Q 4
static struct {
	int state;                                  // 0 not tried, 1 on, -1 off (the CPU path)
	ID3D11Device *dev; ID3D11DeviceContext *ctx; IDXGISwapChain *sc;
	ID3D11VertexShader *vs; ID3D11PixelShader *ps[GP_N];
	ID3D11Buffer *cb; ID3D11SamplerState *lin;
	ID3D11Texture2D *src; ID3D11ShaderResourceView *srcV; int srcW, srcH;
	ID3D11Texture2D *mid; ID3D11RenderTargetView *midR; ID3D11ShaderResourceView *midV; int midW, midH;
	ID3D11Texture2D *ovl; ID3D11ShaderResourceView *ovlV; int ovlW, ovlH;
	HDC ovlDC; HBITMAP ovlBmp; DWORD *ovlBits;
	ID3D11Texture2D *stage; int stageW, stageH; // screenshots read back
	int bufW, bufH;
	ID3D11Query *qd[GPU_Q], *qa[GPU_Q], *qb[GPU_Q]; int qn;
	double gpuMs;                               // the card's time for the last frame measured
	BOOL flip;
	RECT lastV; BOOL have;                      // the last picture's place (repeats)
} g_gpu;
static IDXGIOutput *g_gpuOut;                   // the display the window is on (gpu_wait_vblank)
static BOOL gpu_drawing(void) { return g_gpuWanted && g_gpu.state >= 0 && g_scale == SCALE_SMOOTH; }
static double gpu_ms(void) { return g_gpu.gpuMs; }
#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
static void gpu_teardown(int state);
static void gpu_fail(const char *why, HRESULT hr)
{
	logf_("GPU drawing off (%s, %08lx): the CPU draws", why, (unsigned long)hr);
	gpu_teardown(-1);
}
// Everything released; state -1 (failed: the CPU draws) or 0 (switched off, may start again).
static void gpu_teardown(int state)
{
	g_gpu.state = state;
	if (g_gpu.ctx) ID3D11DeviceContext_ClearState(g_gpu.ctx);
	REL(g_gpu.srcV); REL(g_gpu.src); REL(g_gpu.midV); REL(g_gpu.midR); REL(g_gpu.mid); REL(g_gpu.ovlV); REL(g_gpu.ovl); REL(g_gpu.stage);
	for (int i = 0; i < GP_N; i++) REL(g_gpu.ps[i]);
	for (int i = 0; i < GPU_Q; i++) { REL(g_gpu.qd[i]); REL(g_gpu.qa[i]); REL(g_gpu.qb[i]); }
	REL(g_gpu.vs); REL(g_gpu.cb); REL(g_gpu.lin); REL(g_gpu.sc); REL(g_gpu.ctx); REL(g_gpu.dev);
	g_gpu.have = FALSE;
	REL(g_gpuOut);
	g_gpu.srcW = g_gpu.srcH = g_gpu.midW = g_gpu.midH = g_gpu.ovlW = g_gpu.ovlH = g_gpu.stageW = g_gpu.stageH = 0;
	g_gpu.bufW = g_gpu.bufH = 0; g_gpu.qn = 0; g_gpu.gpuMs = 0;
	InterlockedExchange(&g_forcePresent, 1);
}
static BOOL gpu_tex(ID3D11Texture2D **t, ID3D11ShaderResourceView **v, ID3D11RenderTargetView **r, int w, int h, BOOL dynamic);
static void gpu_pass(int ps, ID3D11ShaderResourceView *srv, int sw, int sh, ID3D11RenderTargetView *rtv, int x, int y, int w, int h, float rcas);
static BOOL gpu_init(int cw, int ch)
{
	typedef HRESULT (WINAPI *create_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
	typedef HRESULT (WINAPI *compile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
	HMODULE d3d = LoadLibraryA("d3d11.dll"), dc = LoadLibraryA("d3dcompiler_47.dll");
	create_t create = d3d ? (create_t)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : NULL;
	compile_t compile = dc ? (compile_t)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!create || !compile) { gpu_fail("no d3d11.dll or d3dcompiler_47.dll", 0); return FALSE; }
	DXGI_SWAP_CHAIN_DESC sd;
	memset(&sd, 0, sizeof sd);
	sd.BufferDesc.Width = cw; sd.BufferDesc.Height = ch;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.OutputWindow = g_hwnd;
	sd.Windowed = TRUE;
	sd.SwapEffect = 4; // DXGI_SWAP_EFFECT_FLIP_DISCARD
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
	HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &g_gpu.sc, &g_gpu.dev, &got, &g_gpu.ctx);
	g_gpu.flip = SUCCEEDED(hr);
	if (FAILED(hr)) { // (an older blit-model chain, as on a layered window)
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.BufferCount = 1;
		hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &g_gpu.sc, &g_gpu.dev, &got, &g_gpu.ctx);
	}
	if (FAILED(hr)) { gpu_fail("no Direct3D 11 device", hr); return FALSE; }
	IDXGIFactory *fac = NULL;
	if (SUCCEEDED(IDXGISwapChain_GetParent(g_gpu.sc, &kIID_DXGIFactory, (void **)&fac))) {
		IDXGIFactory_MakeWindowAssociation(fac, g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		IDXGIFactory_Release(fac);
	}
	IDXGIDevice1 *xd = NULL;
	if (SUCCEEDED(ID3D11Device_QueryInterface(g_gpu.dev, &kIID_DXGIDevice1, (void **)&xd))) { IDXGIDevice1_SetMaximumFrameLatency(xd, 1); IDXGIDevice1_Release(xd); }
	static const char *const entry[GP_N] = { "easu", "rcas", "cr", "bil", "key" };
	ID3DBlob *code = NULL, *err = NULL;
	hr = compile(kShaders, sizeof kShaders - 1, "lords2hd", NULL, NULL, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr)) { if (err) logf_("shader: %s", (char *)ID3D10Blob_GetBufferPointer(err)); REL(err); gpu_fail("vertex shader", hr); return FALSE; }
	hr = ID3D11Device_CreateVertexShader(g_gpu.dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_gpu.vs);
	REL(code);
	if (FAILED(hr)) { gpu_fail("vertex shader", hr); return FALSE; }
	for (int i = 0; i < GP_N; i++) {
		hr = compile(kShaders, sizeof kShaders - 1, "lords2hd", NULL, NULL, entry[i], "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
		if (FAILED(hr)) { if (err) logf_("shader %s: %s", entry[i], (char *)ID3D10Blob_GetBufferPointer(err)); REL(err); gpu_fail("pixel shader", hr); return FALSE; }
		hr = ID3D11Device_CreatePixelShader(g_gpu.dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_gpu.ps[i]);
		REL(code);
		if (FAILED(hr)) { gpu_fail("pixel shader", hr); return FALSE; }
	}
	D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	hr = ID3D11Device_CreateBuffer(g_gpu.dev, &bd, NULL, &g_gpu.cb);
	if (FAILED(hr)) { gpu_fail("constant buffer", hr); return FALSE; }
	D3D11_SAMPLER_DESC smp;
	memset(&smp, 0, sizeof smp);
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	hr = ID3D11Device_CreateSamplerState(g_gpu.dev, &smp, &g_gpu.lin);
	if (FAILED(hr)) { gpu_fail("sampler", hr); return FALSE; }
	for (int i = 0; i < GPU_Q; i++) {
		D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
		ID3D11Device_CreateQuery(g_gpu.dev, &qd, &g_gpu.qd[i]);
		ID3D11Device_CreateQuery(g_gpu.dev, &qt, &g_gpu.qa[i]);
		ID3D11Device_CreateQuery(g_gpu.dev, &qt, &g_gpu.qb[i]);
	}
	// Every shader drawn once into a small texture now: the driver finishes a shader at its first draw,
	// and the first Catmull-Rom picture (a battle's zoomed-out view) took 110 ms (six empty refreshes).
	{
		double w0 = perf_now();
		ID3D11Texture2D *wt = NULL, *ws = NULL; ID3D11ShaderResourceView *wv = NULL, *wsv = NULL; ID3D11RenderTargetView *wr = NULL;
		if (gpu_tex(&wt, &wv, &wr, 16, 16, FALSE) && gpu_tex(&ws, &wsv, NULL, 16, 16, TRUE)) {
			ID3D11DeviceContext_IASetPrimitiveTopology(g_gpu.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ID3D11DeviceContext_IASetInputLayout(g_gpu.ctx, NULL);
			ID3D11DeviceContext_VSSetShader(g_gpu.ctx, g_gpu.vs, NULL, 0);
			ID3D11DeviceContext_PSSetConstantBuffers(g_gpu.ctx, 0, 1, &g_gpu.cb);
			ID3D11DeviceContext_PSSetSamplers(g_gpu.ctx, 0, 1, &g_gpu.lin);
			for (int i = 0; i < GP_N; i++) gpu_pass(i, wsv, 16, 16, wr, 0, 0, 16, 16, 0.5f);
			ID3D11ShaderResourceView *none = NULL;
			ID3D11DeviceContext_PSSetShaderResources(g_gpu.ctx, 0, 1, &none);
			ID3D11DeviceContext_OMSetRenderTargets(g_gpu.ctx, 0, NULL, NULL);
			ID3D11DeviceContext_Flush(g_gpu.ctx);
		}
		REL(wr); REL(wv); REL(wt); REL(wsv); REL(ws);
		logf_("GPU shaders warmed up in %.1f ms", perf_now() - w0);
	}
	g_gpu.bufW = cw; g_gpu.bufH = ch;
	g_gpu.state = 1;
	logf_("GPU drawing on (%s swap chain, %dx%d)", g_gpu.flip ? "flip" : "blit", cw, ch);
	return TRUE;
}
// A texture of w x h (BGRA), dynamic for uploads or a render target.
static BOOL gpu_tex(ID3D11Texture2D **t, ID3D11ShaderResourceView **v, ID3D11RenderTargetView **r, int w, int h, BOOL dynamic)
{
	REL(*v); if (r) REL(*r); REL(*t);
	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof td);
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
	td.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (r ? D3D11_BIND_RENDER_TARGET : 0);
	td.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
	if (FAILED(ID3D11Device_CreateTexture2D(g_gpu.dev, &td, NULL, t))) return FALSE;
	if (FAILED(ID3D11Device_CreateShaderResourceView(g_gpu.dev, (ID3D11Resource *)*t, NULL, v))) return FALSE;
	if (r && FAILED(ID3D11Device_CreateRenderTargetView(g_gpu.dev, (ID3D11Resource *)*t, NULL, r))) return FALSE;
	return TRUE;
}
static BOOL gpu_upload(ID3D11Texture2D *t, const DWORD *px, int w, int h, int stride)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ID3D11DeviceContext_Map(g_gpu.ctx, (ID3D11Resource *)t, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return FALSE;
	for (int y = 0; y < h; y++) memcpy((BYTE *)m.pData + (size_t)y * m.RowPitch, px + (size_t)y * stride, (size_t)w * 4);
	ID3D11DeviceContext_Unmap(g_gpu.ctx, (ID3D11Resource *)t, 0);
	return TRUE;
}
// One pass: shader ps reading srv (sw x sh) into rtv over the viewport x, y, w, h.
static void gpu_pass(int ps, ID3D11ShaderResourceView *srv, int sw, int sh, ID3D11RenderTargetView *rtv, int x, int y, int w, int h, float rcas)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(ID3D11DeviceContext_Map(g_gpu.ctx, (ID3D11Resource *)g_gpu.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		float *c = m.pData;
		c[0] = (float)sw; c[1] = (float)sh; c[2] = (float)w; c[3] = (float)h;
		c[4] = (float)x; c[5] = (float)y; c[6] = rcas; c[7] = 0;
		ID3D11DeviceContext_Unmap(g_gpu.ctx, (ID3D11Resource *)g_gpu.cb, 0);
	}
	ID3D11ShaderResourceView *none = NULL;
	ID3D11DeviceContext_PSSetShaderResources(g_gpu.ctx, 0, 1, &none);
	ID3D11DeviceContext_OMSetRenderTargets(g_gpu.ctx, 1, &rtv, NULL);
	D3D11_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0, 1 };
	ID3D11DeviceContext_RSSetViewports(g_gpu.ctx, 1, &vp);
	ID3D11DeviceContext_PSSetShader(g_gpu.ctx, g_gpu.ps[ps], NULL, 0);
	ID3D11DeviceContext_PSSetShaderResources(g_gpu.ctx, 0, 1, &srv);
	ID3D11DeviceContext_Draw(g_gpu.ctx, 3, 0);
}
static void perf_draw(HDC dc, int vw, int vh);
static void gpu_record(ID3D11Texture2D *back, const RECT *v);
// The picture to the window by the GPU; FALSE: draw it on the CPU instead.
static BOOL gpu_present(const RECT *v, const DWORD *src, int sw, int sh)
{
	RECT c;
	GetClientRect(g_hwnd, &c);
	int cw = c.right, ch = c.bottom, vw = v->right - v->left, vh = v->bottom - v->top;
	if (cw <= 0 || ch <= 0 || vw <= 0 || vh <= 0) return FALSE;
	if (!g_gpu.state && !gpu_init(cw, ch)) return FALSE;
	if (g_gpu.state != 1) return FALSE;
	HRESULT hr;
	if (cw != g_gpu.bufW || ch != g_gpu.bufH) {
		ID3D11DeviceContext_OMSetRenderTargets(g_gpu.ctx, 0, NULL, NULL);
		hr = IDXGISwapChain_ResizeBuffers(g_gpu.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
		if (FAILED(hr)) { gpu_fail("resize", hr); return FALSE; }
		g_gpu.bufW = cw; g_gpu.bufH = ch;
	}
	if (!src) { // a repeat (gpu_repeat): the last picture again, from the texture the card already holds
		if (!g_gpu.srcV || g_gpu.srcW != sw || g_gpu.srcH != sh) return FALSE;
	}
	else {
		if ((sw != g_gpu.srcW || sh != g_gpu.srcH) && !gpu_tex(&g_gpu.src, &g_gpu.srcV, NULL, sw, sh, TRUE)) { gpu_fail("texture", 0); return FALSE; }
		g_gpu.srcW = sw; g_gpu.srcH = sh;
		if (!gpu_upload(g_gpu.src, src, sw, sh, sw)) { gpu_fail("upload", 0); return FALSE; }
		g_gpu.lastV = *v; g_gpu.have = TRUE;
	}
	perf_mark(PS_SCALE); // (the upload)
	double tSub0 = perf_now(), tSub1, tSub2; // Timing=1: a slow draw's parts
	ID3D11Texture2D *back = NULL;
	ID3D11RenderTargetView *bv = NULL;
	if (FAILED(hr = IDXGISwapChain_GetBuffer(g_gpu.sc, 0, &kIID_Tex2D, (void **)&back)) ||
		FAILED(hr = ID3D11Device_CreateRenderTargetView(g_gpu.dev, (ID3D11Resource *)back, NULL, &bv))) { REL(back); gpu_fail("back buffer", hr); return FALSE; }
	int q = g_gpu.qn % GPU_Q;
	if (g_gpu.qd[q]) { ID3D11DeviceContext_Begin(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qd[q]); ID3D11DeviceContext_End(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qa[q]); }
	static const float black[4] = { 0, 0, 0, 1 };
	ID3D11DeviceContext_ClearRenderTargetView(g_gpu.ctx, bv, black);
	ID3D11DeviceContext_IASetPrimitiveTopology(g_gpu.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout(g_gpu.ctx, NULL);
	ID3D11DeviceContext_VSSetShader(g_gpu.ctx, g_gpu.vs, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(g_gpu.ctx, 0, 1, &g_gpu.cb);
	ID3D11DeviceContext_PSSetSamplers(g_gpu.ctx, 0, 1, &g_gpu.lin);
	int mode = g_upscaleCR;
	tSub1 = perf_now();
	if (mode == 2) { // FSR: EASU (Catmull-Rom when not enlarging) into a window-sized texture, RCAS onto the screen
		if ((vw != g_gpu.midW || vh != g_gpu.midH) && !gpu_tex(&g_gpu.mid, &g_gpu.midV, &g_gpu.midR, vw, vh, FALSE)) { REL(bv); REL(back); gpu_fail("texture", 0); return FALSE; }
		g_gpu.midW = vw; g_gpu.midH = vh;
		gpu_pass(vh > sh ? GP_EASU : GP_CR, g_gpu.srcV, sw, sh, g_gpu.midR, 0, 0, vw, vh, 0);
		gpu_pass(GP_RCAS, g_gpu.midV, vw, vh, bv, v->left, v->top, vw, vh, g_rcasSharp >= 0 ? exp2f(-g_rcasSharp) : 0);
	}
	else gpu_pass(mode ? GP_CR : GP_BIL, g_gpu.srcV, sw, sh, bv, v->left, v->top, vw, vh, 0);
	tSub2 = perf_now();
	if (g_statsOn) { // the overlay: drawn by GDI into a small bitmap, magenta where it is see-through
		int ow = min(vw, max(640, vw * 6 / 10)), oh = min(vh, max(120, vh / 7));
		if (ow != g_gpu.ovlW || oh != g_gpu.ovlH) {
			if (g_gpu.ovlDC) { DeleteDC(g_gpu.ovlDC); DeleteObject(g_gpu.ovlBmp); g_gpu.ovlDC = NULL; }
			BITMAPINFOHEADER ob = { sizeof ob, ow, -oh, 1, 32, BI_RGB };
			g_gpu.ovlBmp = CreateDIBSection(NULL, (BITMAPINFO *)&ob, DIB_RGB_COLORS, (void **)&g_gpu.ovlBits, NULL, 0);
			if (g_gpu.ovlBmp) { g_gpu.ovlDC = CreateCompatibleDC(NULL); SelectObject(g_gpu.ovlDC, g_gpu.ovlBmp); }
			if (!g_gpu.ovlBmp || !gpu_tex(&g_gpu.ovl, &g_gpu.ovlV, NULL, ow, oh, TRUE)) g_gpu.ovlW = 0;
			else { g_gpu.ovlW = ow; g_gpu.ovlH = oh; }
		}
		if (g_gpu.ovlW) {
			for (int i = 0; i < ow * oh; i++) g_gpu.ovlBits[i] = 0xff00ff;
			perf_draw(g_gpu.ovlDC, vw, vh);
			GdiFlush();
			if (gpu_upload(g_gpu.ovl, g_gpu.ovlBits, ow, oh, ow)) gpu_pass(GP_KEY, g_gpu.ovlV, ow, oh, bv, v->left, v->top, ow, oh, 0);
		}
	}
	ID3D11ShaderResourceView *none = NULL;
	ID3D11DeviceContext_PSSetShaderResources(g_gpu.ctx, 0, 1, &none);
	if (g_timing && perf_now() - tSub0 > 30) logf_("gpu: slow draw: back buffer %.1f, passes %.1f, overlay %.1f ms (view %dx%d, source %dx%d)", tSub1 - tSub0, tSub2 - tSub1, perf_now() - tSub2, vw, vh, sw, sh);
	if (g_gpu.qd[q]) { ID3D11DeviceContext_End(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qb[q]); ID3D11DeviceContext_End(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qd[q]); }
	g_gpu.qn++;
	if (g_shotViewWanted && InterlockedExchange(&g_shotViewWanted, 0)) { // testing: the picture as on screen, read back
		if (vw != g_gpu.stageW || vh != g_gpu.stageH) {
			REL(g_gpu.stage);
			D3D11_TEXTURE2D_DESC td;
			memset(&td, 0, sizeof td);
			td.Width = vw; td.Height = vh; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_gpu.dev, &td, NULL, &g_gpu.stage))) { g_gpu.stageW = vw; g_gpu.stageH = vh; }
		}
		D3D11_BOX box = { v->left, v->top, 0, v->right, v->bottom, 1 };
		D3D11_MAPPED_SUBRESOURCE m;
		if (g_gpu.stage) {
			ID3D11DeviceContext_CopySubresourceRegion(g_gpu.ctx, (ID3D11Resource *)g_gpu.stage, 0, 0, 0, 0, (ID3D11Resource *)back, 0, &box);
			if (SUCCEEDED(ID3D11DeviceContext_Map(g_gpu.ctx, (ID3D11Resource *)g_gpu.stage, 0, D3D11_MAP_READ, 0, &m))) {
				char path[MAX_PATH];
				snprintf(path, sizeof path, "%s\\lords2hd-shotview.bmp", g_gameDir);
				FILE *f = fopen(path, "wb");
				if (f) {
					BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + vw * vh * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
					BITMAPINFOHEADER ih = { sizeof ih, vw, -vh, 1, 32, BI_RGB };
					fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
					for (int y = 0; y < vh; y++) fwrite((BYTE *)m.pData + (size_t)y * m.RowPitch, 4, vw, f);
					fclose(f);
				}
				ID3D11DeviceContext_Unmap(g_gpu.ctx, (ID3D11Resource *)g_gpu.stage, 0);
			}
		}
	}
	if (src) gpu_record(back, v); // (a repeat is not a recorded frame)
	REL(bv); REL(back);
	perf_mark(PS_SHARPEN); // (the commands sent)
	hr = IDXGISwapChain_Present(g_gpu.sc, 0, 0);
	perf_mark(PS_BLIT);
	if (FAILED(hr)) { gpu_fail("present", hr); return TRUE; }
	if (g_timing && g_gpu.flip) { // Timing=1: whether pictures go to the display directly or through the compositor
		static const GUID kIID_SCMedia = { 0xdd95b90b, 0xf05f, 0x4f6a, { 0xbd, 0x65, 0x25, 0xbf, 0xb2, 0x64, 0xbd, 0x84 } };
		typedef struct { UINT presentCount, presentRefreshCount, syncRefreshCount; LARGE_INTEGER syncQPC; UINT syncGPU, mode, approved; } stats_t;
		typedef struct { void *qi; void *addRef; ULONG (WINAPI *release)(void *); HRESULT (WINAPI *stats)(void *, stats_t *); } media_vt;
		static void *media; static BOOL tried; static int lastMode = -1;
		if (!tried) { tried = TRUE; if (FAILED(IDXGISwapChain_QueryInterface(g_gpu.sc, &kIID_SCMedia, &media))) media = NULL; }
		stats_t st;
		if (media && SUCCEEDED((*(media_vt **)media)->stats(media, &st)) && (int)st.mode != lastMode) {
			static const char *const names[] = { "composed", "overlay", "independent flip", "composition failure" };
			logf_("display mode: %s", st.mode < 4 ? names[st.mode] : "?");
			lastMode = st.mode;
		}
	}
	// the card's time for a frame drawn a few frames ago (its queries are read without waiting)
	if (g_gpu.qn >= GPU_Q) {
		int r = g_gpu.qn % GPU_Q; // the oldest
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
		if (g_gpu.qd[r] && ID3D11DeviceContext_GetData(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qd[r], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			ID3D11DeviceContext_GetData(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qa[r], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			ID3D11DeviceContext_GetData(g_gpu.ctx, (ID3D11Asynchronous *)g_gpu.qb[r], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			!dj.Disjoint && dj.Frequency)
			g_gpu.gpuMs = (double)(b - a) * 1000.0 / dj.Frequency;
	}
	g_outW = vw; g_outH = vh;
	return TRUE;
}

// Waits for the display's next vertical blank (the GPU path): DwmFlush follows the compositor, which a
// fullscreen flip-model window bypasses (independent flip), so pictures were paced to the wrong clock
// and a present now and then waited for a queued one (a refresh with no picture). FALSE: not available.
static BOOL gpu_wait_vblank(void)
{
	if (g_gpu.state != 1 || !g_gpu.sc) return FALSE;
	if (!g_gpuOut && FAILED(IDXGISwapChain_GetContainingOutput(g_gpu.sc, &g_gpuOut))) { g_gpuOut = NULL; return FALSE; }
	if (FAILED(IDXGIOutput_WaitForVBlank(g_gpuOut))) { REL(g_gpuOut); return FALSE; }
	return TRUE;
}
// A refresh with nothing new from the game: the last picture handed to the screen again, drawn by the
// card from the textures it holds (no compose, no upload), so every refresh gets a picture. Only with
// the GPU, and not while the game is minimised or in the background.
static BOOL gpu_repeat(void)
{
	if (!g_repeatOn || !gpu_drawing() || g_gpu.state != 1 || !g_gpu.have || !g_hwnd) return FALSE;
	if (IsIconic(g_hwnd) || (!g_remote && GetForegroundWindow() != g_hwnd)) return FALSE;
	RECT v;
	view_rect(&v);
	if (!EqualRect(&v, &g_gpu.lastV)) return FALSE; // the window changed: a real picture is due
	return gpu_present(&g_gpu.lastV, NULL, g_gpu.srcW, g_gpu.srcH);
}

static void put_picture(HDC dc, const RECT *v, const DWORD *src, int sw, int sh, int gdiMode)
{
	int vw = v->right - v->left, vh = v->bottom - v->top;
	BITMAPINFOHEADER bi = { sizeof bi, sw, -sh, 1, 32, BI_RGB };
	BOOL cr = g_upscaleCR, menu = g_menuOpen, shot = g_shotViewWanted, stats = g_statsOn;
	perf_mark(PS_COMPOSE);
	if (g_gpuWanted && g_gpu.state >= 0 && gpu_present(v, src, sw, sh)) return;
	if (!cr && !menu && !shot && !stats) {
		SetStretchBltMode(dc, gdiMode);
		if (gdiMode == HALFTONE) SetBrushOrgEx(dc, 0, 0, NULL);
		StretchDIBits(dc, v->left, v->top, vw, vh, 0, 0, sw, sh, src, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
		perf_mark(PS_SCALE);
		return;
	}
	static DWORD *tmp;       // FSR: the stretched picture before RCAS
	static int tmpCap;
	if (!g_outDC || g_outW != vw || g_outH != vh) {
		if (g_outDC) { DeleteDC(g_outDC); DeleteObject(g_outBmp); g_outDC = NULL; }
		BITMAPINFOHEADER ob = { sizeof ob, vw, -vh, 1, 32, BI_RGB };
		g_outBmp = CreateDIBSection(dc, (BITMAPINFO *)&ob, DIB_RGB_COLORS, (void **)&g_outBits, NULL, 0);
		if (!g_outBmp) { StretchDIBits(dc, v->left, v->top, vw, vh, 0, 0, sw, sh, src, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY); return; }
		g_outDC = CreateCompatibleDC(dc);
		SelectObject(g_outDC, g_outBmp);
		g_outW = vw; g_outH = vh;
	}
	GdiFlush();
	BOOL fsr = cr == 2, sharpen = fsr && g_rcasSharp >= 0;
	if (sharpen && vw * vh > tmpCap) { free(tmp); tmp = malloc((size_t)vw * vh * 4); tmpCap = tmp ? vw * vh : 0; }
	if (sharpen && !tmp) sharpen = FALSE;
	DWORD *to = sharpen ? tmp : g_outBits;
	BOOL scaled = FALSE;
	if (fsr && g_fsrFinal && vh > sh) scaled = fsr_easu(src, sw, sh, sw, to, vw, vh, vw);
	if (!scaled && cr) scaled = resize_cr(src, sw, sh, sw, to, vw, vh, vw);
	if (!scaled) {
		sharpen = FALSE;
		SetStretchBltMode(g_outDC, gdiMode);
		if (gdiMode == HALFTONE) SetBrushOrgEx(g_outDC, 0, 0, NULL);
		StretchDIBits(g_outDC, 0, 0, vw, vh, 0, 0, sw, sh, src, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
	perf_mark(PS_SCALE);
	if (sharpen && !fsr_rcas(tmp, vw, g_outBits, vw, vh, vw)) memcpy(g_outBits, tmp, (size_t)vw * vh * 4);
	perf_mark(PS_SHARPEN);
	if (menu && !menu_as_scroll()) menu_draw(g_outDC, vw, vh, v->left, v->top);
	if (stats) perf_draw(g_outDC, vw, vh);
	GdiFlush();
	if (shot && InterlockedExchange(&g_shotViewWanted, 0)) { // testing: the picture as on screen
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\lords2hd-shotview.bmp", g_gameDir);
		FILE *f = fopen(path, "wb");
		if (f) {
			BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + vw * vh * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
			BITMAPINFOHEADER ih = { sizeof ih, vw, -vh, 1, 32, BI_RGB };
			fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(g_outBits, 4, (size_t)vw * vh, f);
			fclose(f);
		}
	}
	BitBlt(dc, v->left, v->top, vw, vh, g_outDC, 0, 0, SRCCOPY);
	GdiFlush();
	perf_mark(PS_BLIT);
}

// The zoomed-out battle's picture, twice the size of the normal one: the battlefield 1:1 from the
// frame's tall area, the side panel and the top bar doubled. Above 2x on screen it is smoothed
// once more; below (1080p is 1.125x) GDI's smooth mode does the rest.
// The zoomed-out battle's picture is built in bands of rows on all the CPU's threads (pool_run):
// bt_rows_job makes the picture's rows from the frame (every step only writes its own row), then
// bt_double_job doubles them and stamps the HD troops at 2x. One thread took ~19 ms at 5K.
#define BT_BAND 16
static volatile LONG g_btMiss; // Timing=1: battlefield pixels whose HD art did not match this picture
static struct {
	DWORD *pic, *pic2; const BYTE *copy, *frame; int pitch, lw, lh, mapW;
	const DWORD *btL; const BYTE *btS, *btV; BOOL btOn, big, menu, easu;
	int mx0, mx1, my1, lx, fy0;
} g_bt;
#define BT_IN_MENU(sx, sy) ((sx) >= g_bt.mx0 && (sx) < g_bt.mx1 && (sy) >= 48 && (sy) < g_bt.my1)
static void bt_row(int sy)
{
	int lw = g_bt.lw, mapW = g_bt.mapW, pitch = g_bt.pitch;
	const BYTE *frame = g_bt.frame, *copy = g_bt.copy;
	DWORD *o = g_bt.pic + sy * lw;
	if (sy >= 48) {
		const BYTE *row = frame + (24 + sy - 48) * pitch + PANEL_X - mapW;
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
	else {
		const BYTE *bar = frame + (sy / 2) * pitch - g_extra;
		for (int sx = 0; sx < lw; sx++) o[sx] = g_lut[bar[sx / 2]];
	}
	int from = sy < 48 ? 0 : mapW;
	// Letters in the doubled top bar and side panel.
	if (g_txt && g_hdText && g_back->w == g_txtW) {
		int W = g_back->w;
		const BYTE *tv = g_txtS ? g_txtValidS : g_txtValid, *ts = g_txtS ? g_txtShadowS : g_txtShadow, *tx = g_txtS ? g_txtS : g_txt;
		int gy = sy < 48 ? sy / 2 : 24 + (sy - 48) / 2;
		for (int sx = from; sx < lw; sx++) {
			int gx = sy < 48 ? g_off - g_extra + sx / 2 : g_off + PANEL_X + (sx - mapW) / 2;
			int f = gy * W + gx;
			if (tv[f] && copy[gy * pitch + gx] == ts[f]) o[sx] = g_lut[tx[f * 4 + (sy & 1) * 2 + ((sx - from) & 1)]];
		}
	}
	// HD sprites in the doubled top bar and side panel: already at 2x there.
	if (g_bt.btOn) {
		int gy = sy < 48 ? sy / 2 : 24 + (sy - 48) / 2;
		for (int sx = from; sx < lw; sx++) {
			int gx = sy < 48 ? g_off - g_extra + sx / 2 : g_off + PANEL_X + (sx - mapW) / 2;
			int f = gy * g_frameW + gx;
			if (gx < 0 || gx >= g_frameW || !g_bt.btV[f] || copy[gy * pitch + gx] != g_bt.btS[f]) continue;
			o[sx] = g_bt.btL[f * 4 + (sy & 1) * 2 + ((sx - from) & 1)];
		}
	}
	// the open drop-down, doubled
	if (g_bt.menu && sy >= 48 && sy < g_bt.my1) {
		int W = g_back ? g_back->w : 0, gy = g_bt.fy0 + (sy - 48) / 2;
		for (int sx = max(0, g_bt.mx0); sx < g_bt.mx1; sx++) {
			int gx = g_off + g_bt.lx + (sx - g_bt.mx0) / 2;
			if (gx < 0 || gx >= pitch) continue;
			BYTE v8 = copy[gy * pitch + gx];
			DWORD p = g_lut[v8];
			int q = (sy & 1) * 2 + ((sx - g_bt.mx0) & 1);
			if (g_bt.btOn && gx < g_frameW) { int f = gy * g_frameW + gx; if (g_bt.btV[f] && g_bt.btS[f] == v8) p = g_bt.btL[f * 4 + q]; }
			if (g_txt && g_hdText && W && W == g_txtW && gx < W && gy < g_txtH) { int f = gy * W + gx; if ((g_txtS ? g_txtValidS : g_txtValid)[f] && v8 == (g_txtS ? g_txtShadowS : g_txtShadow)[f]) p = g_lut[(g_txtS ? g_txtS : g_txt)[f * 4 + q]]; }
			o[sx] = p;
		}
	}
	// too small for the 2x art: each HD block averaged into its one pixel
	if (g_bt.btOn && !g_bt.big && sy >= 48) {
		int fy = 24 + sy - 48, f0 = fy * g_frameW + g_off + PANEL_X - mapW;
		const BYTE *row = copy + fy * pitch + g_off + PANEL_X - mapW;
		for (int sx = 0; sx < mapW; sx++) {
			int f = f0 + sx;
			if (!g_bt.btV[f] || row[sx] != g_bt.btS[f] || BT_IN_MENU(sx, sy)) continue;
			const DWORD *h = g_bt.btL + f * 4;
			DWORD rb = (h[0] & 0xff00ff) + (h[1] & 0xff00ff) + (h[2] & 0xff00ff) + (h[3] & 0xff00ff);
			DWORD gg = (h[0] & 0xff00) + (h[1] & 0xff00) + (h[2] & 0xff00) + (h[3] & 0xff00);
			o[sx] = ((rb >> 2) & 0xff00ff) | ((gg >> 2) & 0xff00);
		}
	}
}
static void bt_rows_job(int band, int me)
{
	(void)me;
	int y1 = min(g_bt.lh, (band + 1) * BT_BAND);
	for (int sy = band * BT_BAND; sy < y1; sy++) bt_row(sy);
}
static void bt_double_job(int band, int me)
{
	(void)me;
	int lw = g_bt.lw, lh = g_bt.lh, mapW = g_bt.mapW, pitch = g_bt.pitch;
	const DWORD *pic = g_bt.pic;
	DWORD *pic2 = g_bt.pic2;
	int y1 = min(lh, (band + 1) * BT_BAND);
	for (int y = band * BT_BAND; y < y1; y++) {
		if (!g_bt.easu) {
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
		if (g_bt.btOn && y >= 48) {
			int fy = 24 + y - 48, f0 = fy * g_frameW + g_off + PANEL_X - mapW;
			const BYTE *row = g_bt.copy + fy * pitch + g_off + PANEL_X - mapW;
			DWORD *o0 = pic2 + 2 * y * 2 * lw, *o1 = o0 + 2 * lw;
			int miss = 0;
			for (int sx = 0; sx < mapW; sx++) {
				int f = f0 + sx;
				if (g_bt.btV[f] && row[sx] != g_bt.btS[f]) miss++; // (Timing: HD art known but not matching the frame: shown 1x)
				if (!g_bt.btV[f] || row[sx] != g_bt.btS[f] || BT_IN_MENU(sx, y)) continue;
				const DWORD *h = g_bt.btL + f * 4;
				o0[2 * sx] = h[0]; o0[2 * sx + 1] = h[1];
				o1[2 * sx] = h[2]; o1[2 * sx + 1] = h[3];
			}
			if (miss) InterlockedExchangeAdd(&g_btMiss, miss);
		}
	}
}
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
	g_paceReadSeq = g_pacePrimarySeq;
	LeaveCriticalSection(&g_lock);
	RECT v, c;
	view_rect(&v);
	BOOL forced = InterlockedExchange(&g_forcePresent, 0) || !EqualRect(&v, &lastView);
	if (!forced && palSame && memcmp(copy, shown, pitch * fh) == 0) return FALSE;
	memcpy(shown, copy, pitch * fh);
	lastView = v;

	// the HD layer as of the frame on screen (bt_publish); the live one until the first copy
	g_bt.btL = g_btLayerS ? g_btLayerS : g_btLayer;
	g_bt.btS = g_btLayerS ? g_btShadowS : g_btShadow; g_bt.btV = g_btLayerS ? g_btValidS : g_btValid;
	g_bt.btOn = g_bt.btL && g_bt.btS && g_bt.btV && g_showTiles; // all three, or the HD battle layer is left out of this picture
	for (int i = 0; i < 256; i++) g_lut[i] = (pal[i].peRed << 16) | (pal[i].peGreen << 8) | pal[i].peBlue;
	g_bt.pic = pic; g_bt.pic2 = pic2; g_bt.copy = copy; g_bt.frame = copy + g_off; // the game's x = 0
	g_bt.pitch = pitch; g_bt.lw = lw; g_bt.lh = lh; g_bt.mapW = mapW;
	// An open drop-down: doubled from its corner like the bar above it (it covers its 1:1 self).
	g_bt.mx0 = g_bt.mx1 = g_bt.my1 = 0; // its doubled rectangle in this picture, rows 48 .. my1
	g_bt.menu = menu_doubled();
	if (g_bt.menu) {
		g_bt.lx = g_menuX; g_bt.fy0 = max(24, g_menuY); int fy1 = g_menuY + MENU_H;
		g_bt.mx0 = g_bt.lx - (PANEL_X - mapW); g_bt.mx1 = min(mapW, g_bt.mx0 + 2 * MENU_W); g_bt.my1 = min(lh, 48 + 2 * (fy1 - g_bt.fy0));
	}
	// Too small for the 2x art (1080p): each HD block averaged into its one pixel.
	g_bt.big = (v.bottom - v.top) >= lh * 2;
	pool_run((lh + BT_BAND - 1) / BT_BAND, bt_rows_job); // the picture's rows, on all threads
	GetClientRect(g_hwnd, &c);
	HDC dc = GetDC(g_hwnd);
	if (!dc) return FALSE;
	if (!gpu_drawing()) PatBlt(dc, 0, 0, c.right, v.top, BLACKNESS);
	if (!gpu_drawing()) PatBlt(dc, 0, v.bottom, c.right, c.bottom - v.bottom, BLACKNESS);
	if (!gpu_drawing()) PatBlt(dc, 0, v.top, v.left, v.bottom - v.top, BLACKNESS);
	if (!gpu_drawing()) PatBlt(dc, v.right, v.top, c.right - v.right, v.bottom - v.top, BLACKNESS);
	BITMAPINFOHEADER bi = { sizeof bi };
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;
	const DWORD *src = pic;
	int sw = lw, sh = lh;
	if (g_bt.big) {
		// (the doubling stays bilinear even with Catmull-Rom: at 3392x1920 two Catmull-Rom passes
		// took 25-44 ms a picture; the final stretch is Catmull-Rom). With FSR on the CPU: EASU,
		// then the HD layer stamped.
		g_bt.easu = g_upscaleCR == 2 && !gpu_drawing() && fsr_easu(pic, lw, lh, lw, pic2, 2 * lw, 2 * lh, 2 * lw);
		InterlockedExchange(&g_btMiss, 0);
		pool_run((lh + BT_BAND - 1) / BT_BAND, bt_double_job);
		if (g_timing) { // pictures where many HD pixels fell back to 1x (a flicker)
			static int n, bad; static double t0; static LONG worst;
			LONG m = g_btMiss; n++;
			if (m > 20000) bad++;
			if (m > worst) worst = m;
			double now = perf_now();
			if (!t0) t0 = now;
			if (now - t0 > 1000) { if (bad || worst > 5000) logf_("timing: battle HD art: %d of %d pictures with over 20000 pixels shown 1x (worst %ld)", bad, n, (long)worst); n = bad = 0; worst = 0; t0 = now; }
		}
		src = pic2;
		sw = lw * 2;
		sh = lh * 2;
	}
	if (entry_dark(src, sw, sh, sw * 3 / 4)) { ReleaseDC(g_hwnd, dc); InterlockedExchange(&g_forcePresent, 1); mark_dirty(); return FALSE; }
	bi.biWidth = sw;
	bi.biHeight = -sh;
	shot_save(src, sw, sh);
	put_picture(dc, &v, src, sw, sh, src == pic2 ? COLORONCOLOR : HALFTONE);
	ReleaseDC(g_hwnd, dc);
	return TRUE;
}

// Remote mode's screenshot: the picture about to be shown (BGRX, w x h, top-down) as a BMP.
// Remote message 9 (wParam = n): keep the next n frames shown, at half size, each with its time in ms
// since the request, then write them as lords2hd-burst-NN-<ms>.bmp. Shows what a player sees frame by
// frame after a click (late HD art, flicker).
#define BURST_MAX 60
static volatile LONG g_burstWant, g_burstFlush; // message 10: write what was kept so far
static int g_burstN;
static LARGE_INTEGER g_burstT0;
static struct { DWORD *px; int w, h; double ms; } g_burst[BURST_MAX];

static void burst_frame(const DWORD *px, int w, int h)
{
	if (!g_burstWant) return;
	if (g_burstFlush) { InterlockedExchange(&g_burstFlush, 0); goto write; }
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
	int i = g_burstN, hw = w / 2, hh = h / 2;
	g_burst[i].px = malloc((size_t)hw * hh * 4);
	if (g_burst[i].px) {
		for (int y = 0; y < hh; y++)
			for (int x = 0; x < hw; x++) g_burst[i].px[y * hw + x] = px[(y * 2) * w + x * 2];
		g_burst[i].w = hw; g_burst[i].h = hh;
		g_burst[i].ms = (t.QuadPart - g_burstT0.QuadPart) * 1000.0 / f.QuadPart;
		g_burstN++;
	}
	if (g_burstN < g_burstWant && g_burstN < BURST_MAX) return;
write:
	for (int k = 0; k < g_burstN; k++) {
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\lords2hd-burst-%02d-%05.0f.bmp", g_gameDir, k, g_burst[k].ms);
		FILE *o = fopen(path, "wb");
		if (o) {
			int bw = g_burst[k].w, bh = g_burst[k].h;
			BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + bw * bh * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
			BITMAPINFOHEADER ih = { sizeof ih, bw, -bh, 1, 32, BI_RGB };
			fwrite(&fh, sizeof fh, 1, o); fwrite(&ih, sizeof ih, 1, o);
			fwrite(g_burst[k].px, 4, (size_t)bw * bh, o);
			fclose(o);
		}
		free(g_burst[k].px);
		g_burst[k].px = NULL;
	}
	logf_("burst: %d frames written", g_burstN);
	g_burstN = 0;
	InterlockedExchange(&g_burstWant, 0);
}

// testing: recording (remote message 21, wParam = frames a second, 0 stops): the picture as it goes to the
// screen, written as lords2hd-rec-NNNNN-<ms>.bmp at most that often (halved when wider than 2000)
static volatile LONG g_recFps;
static LARGE_INTEGER g_recT0, g_recLast;
static int g_recN;
static volatile LONG g_recCrop; // with the GPU: 0 the whole picture at half size, else a 1:1 1920x1080 crop at x | y << 16
static void record_frame(const DWORD *px, int w, int h)
{
	if (!g_recFps || gpu_drawing()) return; // (the GPU path records its own finished picture: gpu_record)
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
	if (!g_recN) g_recT0 = t;
	else if ((t.QuadPart - g_recLast.QuadPart) * g_recFps < f.QuadPart) return;
	g_recLast = t;
	int half = w > 2000, ow = half ? w / 2 : w, oh = half ? h / 2 : h;
	char path[MAX_PATH];
	char dir[MAX_PATH]; // the temp folder: the game's may be on a slow disk
	if (!GetTempPathA(sizeof dir, dir)) lstrcpynA(dir, g_gameDir, sizeof dir);
	snprintf(path, sizeof path, "%s\\lords2hd-rec-%05d-%07.0f.bmp", dir, g_recN++, (t.QuadPart - g_recT0.QuadPart) * 1000.0 / f.QuadPart);
	FILE *o = fopen(path, "wb");
	if (!o) return;
	BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + ow * oh * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
	BITMAPINFOHEADER ih = { sizeof ih, ow, -oh, 1, 32, BI_RGB };
	fwrite(&fh, sizeof fh, 1, o); fwrite(&ih, sizeof ih, 1, o);
	if (!half) fwrite(px, 4, (size_t)ow * oh, o);
	else for (int y = 0; y < oh; y++) {
		static DWORD row[4096];
		for (int x = 0; x < ow && x < 4096; x++) row[x] = px[(y * 2) * w + x * 2];
		fwrite(row, 4, ow, o);
	}
	fclose(o);
}

// Recording with the GPU (remote message 21): the finished picture read back from the back buffer, so
// the frames show what the player sees (FSR and all): the whole picture at half size, or a 1:1
// 1920x1080 crop (g_recCrop).
#define REC_RING 3
#define REC_QUEUE 8
static struct {
	ID3D11Texture2D *src; ID3D11ShaderResourceView *srcV; int srcW, srcH;   // the picture, copied where a shader can read it
	ID3D11Texture2D *rt; ID3D11RenderTargetView *rtV; ID3D11ShaderResourceView *rtS; int rtW, rtH; // half size
	ID3D11Texture2D *st[REC_RING]; int stW, stH, ring; double stMs[REC_RING]; BOOL stUsed[REC_RING];
	struct { DWORD *px; int w, h, n; double ms; } q[REC_QUEUE];
	volatile LONG qHead, qTail; HANDLE ev; int dropped, taken;
} g_rec;
static DWORD WINAPI rec_writer(void *unused)
{
	char dir[MAX_PATH];
	if (!GetTempPathA(sizeof dir, dir)) lstrcpynA(dir, g_gameDir, sizeof dir);
	for (;;) {
		WaitForSingleObject(g_rec.ev, 200);
		while (g_rec.qTail != g_rec.qHead) {
			int k = g_rec.qTail % REC_QUEUE;
			char path[MAX_PATH];
			snprintf(path, sizeof path, "%s\\lords2hd-rec-%05d-%07.0f.bmp", dir, g_rec.q[k].n, g_rec.q[k].ms);
			FILE *o = fopen(path, "wb");
			if (o) {
				int w = g_rec.q[k].w, h = g_rec.q[k].h;
				BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + w * h * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
				BITMAPINFOHEADER ih = { sizeof ih, w, -h, 1, 32, BI_RGB };
				fwrite(&fh, sizeof fh, 1, o); fwrite(&ih, sizeof ih, 1, o); fwrite(g_rec.q[k].px, 4, (size_t)w * h, o);
				fclose(o);
			}
			InterlockedIncrement(&g_rec.qTail);
		}
	}
	return 0;
}
// Recording with the GPU (remote message 21): the finished picture as the player sees it (FSR and
// all), shrunk to half size by the card (or a 1:1 1920x1080 crop, g_recCrop), read back two frames
// later so nothing waits for the card, and written by a thread of its own: the recording barely
// slows the game (reading back and writing 15 MB a frame here took it to 15 frames a second).
static void gpu_record(ID3D11Texture2D *back, const RECT *v)
{
	if (!g_recFps) {
		for (int i = 0; i < REC_RING; i++) g_rec.stUsed[i] = FALSE;
		if (g_rec.taken && g_rec.dropped) logf_("recording: %d frames dropped (the writer fell behind)", g_rec.dropped);
		g_rec.taken = 0;
		return;
	}
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
	if (!g_rec.taken) { g_recT0 = t; g_rec.dropped = 0; g_recN = 0; }
	else if ((t.QuadPart - g_recLast.QuadPart) * g_recFps < f.QuadPart) return;
	g_rec.taken++;
	g_recLast = t;
	if (!g_rec.ev) {
		g_rec.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
		HANDLE th = CreateThread(NULL, 0, rec_writer, NULL, 0, NULL);
		if (th) CloseHandle(th);
	}
	int vw = v->right - v->left, vh = v->bottom - v->top;
	LONG crop = g_recCrop;
	int cx = crop & 0xffff, cy = (crop >> 16) & 0xffff, ow = crop ? 1920 : vw / 2, oh = crop ? 1080 : vh / 2;
	if (crop) { cx = max(0, min(vw - ow, cx)); cy = max(0, min(vh - oh, cy)); }
	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof td);
	td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
	if (ow != g_rec.stW || oh != g_rec.stH) {
		for (int i = 0; i < REC_RING; i++) { REL(g_rec.st[i]); g_rec.stUsed[i] = FALSE; }
		td.Width = ow; td.Height = oh; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int i = 0; i < REC_RING; i++) if (FAILED(ID3D11Device_CreateTexture2D(g_gpu.dev, &td, NULL, &g_rec.st[i]))) { g_rec.stW = 0; return; }
		g_rec.stW = ow; g_rec.stH = oh;
	}
	int slot = g_rec.ring % REC_RING;
	if (crop) {
		D3D11_BOX box = { v->left + cx, v->top + cy, 0, v->left + cx + ow, v->top + cy + oh, 1 };
		ID3D11DeviceContext_CopySubresourceRegion(g_gpu.ctx, (ID3D11Resource *)g_rec.st[slot], 0, 0, 0, 0, (ID3D11Resource *)back, 0, &box);
	}
	else {
		if ((vw != g_rec.srcW || vh != g_rec.srcH) && !gpu_tex(&g_rec.src, &g_rec.srcV, NULL, vw, vh, FALSE)) return;
		g_rec.srcW = vw; g_rec.srcH = vh;
		if ((ow != g_rec.rtW || oh != g_rec.rtH) && !gpu_tex(&g_rec.rt, &g_rec.rtS, &g_rec.rtV, ow, oh, FALSE)) return;
		g_rec.rtW = ow; g_rec.rtH = oh;
		D3D11_BOX box = { v->left, v->top, 0, v->right, v->bottom, 1 };
		ID3D11DeviceContext_CopySubresourceRegion(g_gpu.ctx, (ID3D11Resource *)g_rec.src, 0, 0, 0, 0, (ID3D11Resource *)back, 0, &box);
		gpu_pass(GP_BIL, g_rec.srcV, vw, vh, g_rec.rtV, 0, 0, ow, oh, 0); // each output pixel between four: their average
		ID3D11ShaderResourceView *none = NULL;
		ID3D11DeviceContext_PSSetShaderResources(g_gpu.ctx, 0, 1, &none);
		ID3D11DeviceContext_CopyResource(g_gpu.ctx, (ID3D11Resource *)g_rec.st[slot], (ID3D11Resource *)g_rec.rt);
	}
	g_rec.stMs[slot] = (t.QuadPart - g_recT0.QuadPart) * 1000.0 / f.QuadPart;
	g_rec.stUsed[slot] = TRUE;
	g_rec.ring++;
	// the oldest slot (filled two recorded frames ago) to the writer
	int old = g_rec.ring % REC_RING;
	if (!g_rec.stUsed[old]) return;
	if (g_rec.qHead - g_rec.qTail >= REC_QUEUE) { g_rec.dropped++; g_rec.stUsed[old] = FALSE; return; }
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ID3D11DeviceContext_Map(g_gpu.ctx, (ID3D11Resource *)g_rec.st[old], 0, D3D11_MAP_READ, 0, &m))) return;
	int k = g_rec.qHead % REC_QUEUE;
	if (!g_rec.q[k].px || g_rec.q[k].w != ow || g_rec.q[k].h != oh) { free(g_rec.q[k].px); g_rec.q[k].px = malloc((size_t)ow * oh * 4); g_rec.q[k].w = ow; g_rec.q[k].h = oh; }
	if (g_rec.q[k].px) {
		for (int y = 0; y < oh; y++) memcpy(g_rec.q[k].px + (size_t)y * ow, (BYTE *)m.pData + (size_t)y * m.RowPitch, (size_t)ow * 4);
		g_rec.q[k].n = g_recN++; g_rec.q[k].ms = g_rec.stMs[old];
		InterlockedIncrement(&g_rec.qHead);
		SetEvent(g_rec.ev);
	}
	ID3D11DeviceContext_Unmap(g_gpu.ctx, (ID3D11Resource *)g_rec.st[old], 0);
	g_rec.stUsed[old] = FALSE;
}

static void shot_save(const DWORD *px, int w, int h)
{
	burst_frame(px, w, h);
	record_frame(px, w, h);
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

// A frame caught half drawn: the top bar is there but much of the field below it is still black
// (entering a battle the game shows its frame before the battlefield is drawn into it). Such a
// picture is held back for up to half a second; a fade, which darkens the bar too, never is.
static BOOL entry_dark(const DWORD *px, int w, int h, int w1)
{
	static DWORD since;
	int bar = 0, nb = 0, black = 0, n = 0;
	for (int x = 8; x < w; x += 16, nb++) bar += (px[(h / 40) * w + x] & 0xf0f0f0) != 0;
	for (int y = h / 10; y < h; y += 16)
		for (int x = 0; x < w1; x += 16, n++) black += (px[y * w + x] & 0xf0f0f0) == 0;
	BOOL hold = nb && bar * 10 > nb * 6 && n && black * 4 > n; // bar 60% drawn (letters are dark), field 25% black
	DWORD now = GetTickCount();
	if (g_remote && n && black * 4 > n) logf_("dark frame: bar %d/%d field %d/%d (%dx%d)", bar, nb, black, n, w, h);
	if (!hold) { since = 0; return FALSE; }
	if (!since) since = now;
	if (g_remote) logf_("half-drawn frame held (%d of %d field samples black)", black, n);
	return now - since < 500;
}
// Windows over the wide map (the Town Center and its info scrolls, the boxes a click on the map
// opens) sit where the 4:3 screen had them, right of the middle. Their positions are written all
// through the game's code, clicks and drags included, so they are moved here instead, in the
// picture: when such a window opens, the last picture of the map alone is kept (g_snap*); while it
// is up, the part of the frame that differs from that map - the window - is shown shifted to the
// middle of the map the player sees, and the map from before fills the place it left. Clicks on
// the moved window are mapped back (win_click); the game never knows.
static BYTE g_snap8[MAX_W * GAME_H];
static DWORD g_snap2x[MAX_W * 2 * GAME_H * 2];
static int g_snapW;              // width of the snapshot in frame columns, 0 = none
static BYTE g_snapPend8[MAX_W * GAME_H]; // the map as the game just redrew it under a new window
static int g_snapPendW;
static volatile LONG g_snapPending;
static volatile LONG g_winShift; // the window's shift to the left in game pixels (0 = none)
static volatile LONG g_winX0, g_winX1, g_winY0, g_winY1; // its place in the game, before the shift
static BOOL window_screen(int screen)
{
	if (screen == 39 || screen == 57) return *(int *)0x004f0350 != 0; // a scroll over a window, not over the map
	return screen == 2 || screen == 4 || screen == 5 || screen == 6 || screen == 15 ||
		screen == 11 || screen == 26; // diplomacy, and the letter it writes
}
// Called with the frame composed (g_frame8 / g_frame2x, sw columns from game x sx - g_off).
static void window_shift(int screen, int sx, int sw)
{
	InterlockedExchange(&g_winShift, 0);
	if (!g_snapW || g_snapW != sw || !window_screen(screen)) return;
	int x0g = g_off - sx, x1g = x0g + PANEL_X; // frame columns of game x 0 and PANEL_X
	if (x0g < 0 || x1g > sw) return;
	static int colN[MAX_W], rowN[GAME_H];
	memset(colN, 0, sizeof colN); memset(rowN, 0, sizeof rowN);
	for (int y = 24; y < GAME_H; y++) {
		const BYTE *a = g_frame8 + y * sw, *b = g_snap8 + y * sw;
		for (int x = x0g; x < x1g; x++) if (a[x] != b[x]) { colN[x]++; rowN[y]++; }
	}
	int bx0 = -1, bx1 = -1, by0 = -1, by1 = -1;
	for (int x = x0g; x < x1g; x++) if (colN[x] >= 8) { if (bx0 < 0) bx0 = x; bx1 = x + 1; }
	for (int y = 24; y < GAME_H; y++) if (rowN[y] >= 8) { if (by0 < 0) by0 = y; by1 = y + 1; }
	if (bx0 < 0 || by0 < 0 || bx1 - bx0 < 64 || by1 - by0 < 48) return;
	// The map behind must match the kept one: if (nearly) everything differs, the map moved and
	// the kept picture is of somewhere else - then the window stays where the game put it.
	if (bx1 - bx0 > (x1g - x0g) * 9 / 10 && by1 - by0 > (GAME_H - 24) * 9 / 10) return;
	{
		int out = 0, n = 0; // pixels outside the window that differ
		for (int y = 24; y < GAME_H; y += 3) {
			const BYTE *a = g_frame8 + y * sw, *b = g_snap8 + y * sw;
			for (int x = x0g; x < x1g; x += 3) {
				if (y >= by0 && y < by1 && x >= bx0 && x < bx1) continue;
				n++; out += a[x] != b[x];
			}
		}
		if (n && out * 10 > n) return; // more than 10% of the map around it changed
	}
	// the window's middle onto the middle of the map on screen (frame columns 0 .. x1g)
	int s = (bx0 + bx1) / 2 - x1g / 2;
	s = min(s, bx0); // never past the left edge
	if (s <= 2) return;
	int w = bx1 - bx0, W2 = 2 * sw;
	static DWORD row[MAX_W * 2];
	for (int y = by0; y < by1; y++)
		for (int r = 0; r < 2; r++) {
			DWORD *o = g_frame2x + (2 * y + r) * W2;
			const DWORD *m = g_snap2x + (2 * y + r) * W2;
			memcpy(row, o + 2 * bx0, w * 2 * sizeof(DWORD));             // the window
			memcpy(o + 2 * bx0, m + 2 * bx0, w * 2 * sizeof(DWORD));     // the map where it was
			memcpy(o + 2 * (bx0 - s), row, w * 2 * sizeof(DWORD));       // the window in the middle
		}
	g_winX0 = bx0 - x0g; g_winX1 = bx1 - x0g; g_winY0 = by0; g_winY1 = by1;
	InterlockedExchange(&g_winShift, s);
}
// A click on the picture at game x, y: over the moved window it is the window's own place; over
// the map shown where the window was, a spot just outside the window.
static void win_click(int *x, int *y)
{
	int s = g_winShift;
	if (!s || *y < g_winY0 || *y >= g_winY1) return;
	if (*x >= g_winX0 - s && *x < g_winX1 - s) *x += s;
	else if (*x >= g_winX1 - s && *x < g_winX1) *x = g_winX1 + 1 < PANEL_X ? g_winX1 + 1 : g_winX0 - s - 1;
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
		// A window opens over the map: keep the last picture of the map alone (window_shift).
		if (lastScreen == 0 && window_screen(screen) && g_wideScreen && g_showW > 0) {
			memcpy(g_snap8, g_shown8, g_showW * GAME_H);
			memcpy(g_snap2x, g_frame2x, (size_t)g_showW * 2 * GAME_H * 2 * sizeof(DWORD));
			g_snapW = g_showW;
			// better: the map as the game last redrew it (after any scroll the click caused)
			if (g_snapPendW == g_showW) InterlockedExchange(&g_snapPending, 1);
		}
		else if (screen == 0 || !county_map_on()) g_snapW = 0;
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
	BOOL room = g_room.on && g_hdPic.on && g_showScreens && !g_wideScreen && g_off && g_scale == SCALE_SMOOTH;
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
	g_paceReadSeq = g_pacePrimarySeq;
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
	if (!gpu_drawing()) PatBlt(dc, 0, 0, c.right, v.top, BLACKNESS);
	if (!gpu_drawing()) PatBlt(dc, 0, v.bottom, c.right, c.bottom - v.bottom, BLACKNESS);
	if (!gpu_drawing()) PatBlt(dc, 0, v.top, v.left, v.bottom - v.top, BLACKNESS);
	if (!gpu_drawing()) PatBlt(dc, v.right, v.top, c.right - v.right, v.bottom - v.top, BLACKNESS);

	struct { BITMAPINFOHEADER h; RGBQUAD colours[256]; } bi = { { sizeof(BITMAPINFOHEADER) } };
	bi.h.biPlanes = 1;
	bi.h.biCompression = BI_RGB;
	SetStretchBltMode(dc, COLORONCOLOR);
	int vw = v.right - v.left, vh = v.bottom - v.top;
	if (g_scale == SCALE_SMOOTH) {
		for (int i = 0; i < 256; i++) g_lut[i] = (pal[i].peRed << 16) | (pal[i].peGreen << 8) | pal[i].peBlue;
		// A window just opened over the map: compose the map as the game redrew it underneath
		// (g_snapPend8, kept by h_redraw_map) into g_snap2x first, HD layers and all.
		if (InterlockedExchange(&g_snapPending, 0) && g_wideScreen && g_snapPendW == sw) {
			static BYTE keep[MAX_W * GAME_H];
			memcpy(keep, g_frame8, sw * GAME_H);
			memcpy(g_frame8, g_snapPend8, sw * GAME_H);
			for (int i = 0; i < sw * GAME_H; i++) g_frame32[i] = g_lut[g_frame8[i]];
			double_frame(sw);
			screen_sprites(pal, sx, sw);
			tile_overlay(pal, sx, sw);
			text_overlay(sx, sw);
			memcpy(g_snap8, g_frame8, sw * GAME_H);
			memcpy(g_snap2x, g_frame2x, (size_t)sw * 2 * GAME_H * 2 * sizeof(DWORD));
			g_snapW = sw;
			memcpy(g_frame8, keep, sw * GAME_H);
		}
		for (int i = 0; i < sw * GAME_H; i++) g_frame32[i] = g_lut[g_frame8[i]];
		if (InterlockedExchange(&g_shot1xWanted, 0)) { // testing: the game's own frame, 1x, no HD art
			char path[MAX_PATH];
			snprintf(path, sizeof path, "%s\\lords2hd-shot1x.bmp", g_gameDir);
			FILE *f = fopen(path, "wb");
			if (f) {
				BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + sw * GAME_H * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
				BITMAPINFOHEADER ih = { sizeof ih, sw, -GAME_H, 1, 32, BI_RGB };
				fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
				fwrite(g_frame32, 4, (size_t)sw * GAME_H, f);
				fclose(f);
			}
		}
		double ct = perf_now(); // Timing=1: each step's share (compose_log)
		double_frame(sw); ct_add(0, &ct);
		if (sw == GAME_W && g_showScreens) hd_overlay(pal);
		if (g_showTiles) screen_sprites(pal, sx, sw); ct_add(1, &ct);
		if (g_showTiles) tile_overlay(pal, sx, sw); ct_add(2, &ct);
		text_overlay(sx, sw); ct_add(3, &ct);
		scroll_overlays(sx, sw);
		cycle_buttons_draw(sx, sw); ct_add(4, &ct);
		EnterCriticalSection(&g_lock);
		video_overlay(pal, sx, sw);
		if (g_wideScreen) window_shift(screen, sx, sw); else InterlockedExchange(&g_winShift, 0);
		room = room && g_room.on && g_hdPic.on && g_showScreens;
		if (room) room_compose(pal, roomW);
		LeaveCriticalSection(&g_lock);
		// Below 4x, as at 1080p (2.25x), a plain stretch of the 2x frame drops rows and columns
		// unevenly: use GDI's smooth mode, affordable at those sizes. Larger stays fast.
		// (With Catmull-Rom on, put_picture does its own resampling at every size.)
		int gdiMode = vh < GAME_H * 4 && vh != GAME_H * 2 ? HALFTONE : COLORONCOLOR;
		int dw = room ? roomW * 2 : sw * 2;
		if (entry_dark(room ? g_room.out : g_frame2x, dw, GAME_H * 2, dw * 3 / 4)) { ReleaseDC(g_hwnd, dc); InterlockedExchange(&g_forcePresent, 1); mark_dirty(); return FALSE; } // retried until drawn or shown
		shot_save(room ? g_room.out : g_frame2x, dw, GAME_H * 2);
		put_picture(dc, &v, room ? g_room.out : g_frame2x, dw, GAME_H * 2, gdiMode);
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

// A picture must be handed to the swap chain this long before a refresh to reach it: a fullscreen window
// goes to the display without the compositor (independent flip), and on the player's screen a hand-off
// 1-4 ms before the refresh missed it, left the refresh empty and blocked the next present 4-8 ms.
#define SUBMIT_LEAD 5.0  // ms: a new frame that can't be handed over this early gets a repeat first
#define REPEAT_LEAD 7    // ms before the refresh: no new frame by then, the last picture again
// The wait for the next refresh after a picture: the display's vertical blank with the GPU path, the
// compositor's next frame (DwmFlush) on the CPU path.
static double g_lastVblank; // when the last refresh wait ended
static void wait_refresh(HRESULT (WINAPI *dwm_flush)(void))
{
	double t = perf_now();
	static int said;
	if (gpu_drawing() && gpu_wait_vblank()) {
		if (!said) { said = 1; logf_("refresh wait: the display's vertical blank"); }
	}
	else {
		if (said != 2 && gpu_drawing()) { said = 2; logf_("refresh wait: the compositor (no vertical blank wait)"); }
		if (!dwm_flush || FAILED(dwm_flush())) Sleep(8);
	}
	g_lastVblank = perf_now();
	if (g_timing) { // in the pacing record: W, with how long the wait was
		int i = g_paceWakeN % PACE_RING;
		g_paceWake[i].t = perf_now(); g_paceWake[i].seq = g_paceReadSeq; g_paceWake[i].shown = 3; g_paceWake[i].present = perf_now() - t;
		g_paceWakeN++;
	}
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
	double busy = 0, worst = 0, flushMs = 0;
	int presents = 0, skipped = 0;

	for (;;) {
		BOOL bench = g_perf.bench > 0;
		// Repeats (gpu_repeat): wait for a new frame until just before the next refresh; if none came,
		// the last picture goes to that refresh again.
		static double repeatPause; // a repeat could not be drawn (minimised, in the background): wait normally for a while
		BOOL repeatCap = g_repeatOn && gpu_drawing() && g_gpu.state == 1 && g_gpu.have && !bench && perf_now() >= repeatPause;
		static int dbgB1, dbgB2, dbgRepFail; static double dbgT; // Timing=1: the repeat paths taken
		if (g_timing && perf_now() - dbgT > 5000) {
			logf_("presenter: %d idle repeats, %d repeats before a late frame, %d repeats not drawn; a new picture takes %.1f ms", dbgB1, dbgB2, dbgRepFail, g_presentEma);
			dbgB1 = dbgB2 = dbgRepFail = 0; dbgT = perf_now();
		}
		double nextV = 0, period = 0;
		DWORD timeout = g_statsOn ? 25 : 50;
		if (repeatCap) {
			double n0 = perf_now();
			nextV = perf_next_refresh(n0); period = g_perf.refresh;
			if (period > 0) { double ms = nextV - n0 - REPEAT_LEAD; timeout = ms < 0 ? 0 : (DWORD)ms; }
			else repeatCap = FALSE;
		}
		if (!bench) WaitForSingleObject(g_dirtyEvent, timeout);
		BOOL dirty = InterlockedExchange(&g_dirty, 0) != 0;
		if (repeatCap && !dirty && !g_waveActive && g_perf.lastAny <= nextV - period) {
			perf_begin();
			if (gpu_repeat()) {
				perf_repeat(); if (g_timing) pace_wake(2);
				if (g_timing) pace_classify();
				if (g_frameLimit > 0) give_pre_turn(); // (see g_preTurn)
				wait_refresh(dwm_flush);
			}
			else { repeatPause = perf_now() + 250; dbgRepFail++; }
			dbgB1++;
			InterlockedExchange(&g_frameTaken, 1); // the game may copy its newest frame for the next refresh
			continue;
		}
		// The overlay's numbers kept fresh on a still screen: redrawn 4 times a second, not counted.
		static double lastStats;
		double nowMs = perf_now();
		BOOL statsOnly = !repeatCap && !dirty && !bench && g_statsOn && nowMs - g_perf.lastShown > 250 && nowMs - lastStats > 250;
		if (statsOnly) lastStats = nowMs;
		if (bench || statsOnly) InterlockedExchange(&g_forcePresent, 1);
		if (!dirty && !bench && !statsOnly && !g_dumpPalettes && !g_waveActive) continue; // DumpPalettes: lets a palette that stays put be logged
		// A picture for the shore waves alone, while the game has a finished frame on its way (it copies it
		// within 2 ms of being allowed, pending_copy): that frame is waited for, or it reached the screen a
		// refresh late (a scrolling map).
		// (the game may copy only after the refresh, and needs a few ms: up to 6 ms after it, the picture
		// can still be drawn in time)
		// (any wake counts: the game's small updates to the screen, the shore waves; what is waited for
		// is a whole frame copied since the refresh, while one is waiting to be copied)
		// (only while the game may copy: without its turn there is a copy on the screen surface not read yet,
		// and waiting for another would wait for this presenter)
		// (a whole frame copied since the last picture was started, before the refresh too, needs no wait)
		static double composeT;
		if (!bench && g_frameTaken && g_copyPending && g_frameCopyT < g_lastVblank && g_frameCopyT < composeT) {
			for (;;) {
				double wait = g_lastVblank + 6 - perf_now();
				if (wait <= 0 || wait > 6 || g_frameCopyT >= g_lastVblank || !g_copyPending || !g_frameTaken) break;
				WaitForSingleObject(g_dirtyEvent, (DWORD)(wait + 0.5));
			}
			if (InterlockedExchange(&g_dirty, 0)) dirty = TRUE;
		}
		// A new frame too late to be drawn before the coming refresh: that refresh gets the last
		// picture again first (it would otherwise get nothing), and the new one goes to the next.
		if (repeatCap && dirty) {
			double n1 = perf_now();
			if (nextV - n1 < g_presentEma + SUBMIT_LEAD && nextV - n1 > 0 && g_perf.lastAny <= nextV - period) {
				perf_begin();
				if (gpu_repeat()) {
					perf_repeat(); if (g_timing) pace_wake(2);
					wait_refresh(dwm_flush);
				}
				dbgB2++;
				InterlockedExchange(&g_dirty, 1); // the new frame, at once after the refresh (the event too: the
				SetEvent(g_dirtyEvent);           // wait would otherwise sleep to the next refresh's edge again)
				continue;
			}
		}
		QueryPerformanceCounter(&t0);
		perf_begin();
		EnterCriticalSection(&g_composeLock); // (see g_composeLock)
		InterlockedExchange(&g_composing, 1);
		InterlockedExchange(&g_preTurn, 0);
		composeT = perf_now();
		if (g_timing) { // Timing=1: a long wait for the game to finish a copy
			double lw = (composeT - (t0.QuadPart * 1000.0 / freq.QuadPart));
			static int lwLogs;
			if (lw > 6 && lwLogs < 40) { lwLogs++; logf_("presenter: waited %.1f ms for the game's copy to finish", lw); }
		}
		BOOL shown = present();
		InterlockedExchange(&g_composing, 0);
		LeaveCriticalSection(&g_composeLock);
		QueryPerformanceCounter(&t1);
		if (!statsOnly) perf_end(shown, g_outW, g_outH); // (a redraw for the overlay is not a new picture)
		if (g_timing && !statsOnly && !bench) pace_wake(shown);
		if (shown && !bench) {
			InterlockedIncrement(&g_presentCount);
			double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
			// how long a new picture takes (the late check above); a slow one-off (the first picture starts
			// the GPU, a screenshot reads 58 MB back) is left out, and the estimate stays within 10 ms
			if (ms < 20) g_presentEma = min(10.0, g_presentEma * 0.9 + ms * 0.1);
			if (g_timing && ms > 15) { // Timing=1: where a slow picture's time went
				static int slowLogs;
				if (slowLogs < 60) { slowLogs++; logf_("presenter: slow picture %.1f ms: compose %.1f, upload %.1f, submit %.1f, present %.1f", ms, g_perf.st[PS_COMPOSE], g_perf.st[PS_SCALE], g_perf.st[PS_SHARPEN], g_perf.st[PS_BLIT]); }
			}
			busy += ms;
			if (ms > worst) worst = ms;
			presents++;
			// The game may copy its next frame from now on, while this picture waits for its refresh: a busy
			// battle picture takes ~8 ms to draw, and a copy allowed only after the refresh came 2-5 ms into it,
			// too late for the next one (that refresh got the last picture again). Copied before the refresh,
			// the next picture is drawn as soon as it passes.
			if (g_repeatOn && gpu_drawing() && g_frameLimit > 0) give_pre_turn(); // (see g_preTurn)
			// Wait for the compositor's next frame, so the picture changes at most once per refresh.
			double w0 = perf_now();
			wait_refresh(dwm_flush);
			flushMs += perf_now() - w0;
		}
		// The game may copy its next frame now (h_show_frame): after the refresh, so the copy taken
		// is its newest, not one made just after the last picture and a refresh old by the time it
		// is read (changed frames waited unseen behind it).
		give_turn();
		if (!shown && dirty) skipped++;
		if (g_timing) pace_classify();
		if (g_timing && (t1.QuadPart - since.QuadPart) > 5 * freq.QuadPart) {
			logf_("timing: %d presents in 5 s, %.1f ms average, %.1f ms worst; %d unchanged frames skipped; %ld frame copies; %.1f ms average wait for the compositor",
				presents, presents ? busy / presents : 0.0, worst, skipped, (long)InterlockedExchange(&g_dirtyCount, 0), presents ? flushMs / presents : 0.0);
			flushMs = 0;
			if (g_ct[5] > 0 || g_ct[6] > 0) {
				logf_("timing: battle ground cells in 5 s: game's drawing %.0f ms, HD stamping %.0f ms (worst cell %.2f / %.2f)", g_ct[5], g_ct[6], g_ctWorst[5], g_ctWorst[6]);
				g_ct[5] = g_ct[6] = g_ctWorst[5] = g_ctWorst[6] = 0;
			}
			if (g_ctN) {
				int n = g_ctN;
				logf_("timing: map picture %d: double %.2f (%.1f), sprites %.2f (%.1f), tiles %.2f (%.1f), letters %.2f (%.1f), scrolls %.2f (%.1f) ms average (worst)",
					n, g_ct[0] / n, g_ctWorst[0], g_ct[1] / n, g_ctWorst[1], g_ct[2] / n, g_ctWorst[2], g_ct[3] / n, g_ctWorst[3], g_ct[4] / n, g_ctWorst[4]);
				memset(g_ct, 0, sizeof g_ct); memset(g_ctWorst, 0, sizeof g_ctWorst); g_ctN = 0;
			}
			for (int i = 0; i < 2; i++) if (g_draw[i].n) {
				logf_("timing: %d %s redraws, %.2f ms average, %.1f ms worst%s", g_draw[i].n, i ? "battlefield" : "map", g_draw[i].ms / g_draw[i].n, g_draw[i].worst,
					i ? "" : g_stampSkipped + g_stampDone ? " (HD stamping skipped as unchanged in some)" : "");
				if (!i) { logf_("timing: map HD stamping: %d redraws unchanged (skipped), %d stamped", g_stampSkipped, g_stampDone); g_stampSkipped = g_stampDone = 0; }
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
	g_turnEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
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
	if (dst->primary && src == g_back && dy == sy) {
		g_pacePrimarySeq = g_paceCopying;
		text_publish(dy, dy + h);
		bt_publish(dy, dy + h);
		spr_publish(dy, dy + h);
		tile_publish(dy, dy + h);
	}
}

static HRESULT WINAPI surf_blt(void *this_, RECT *dr, void *src_, RECT *sr, DWORD flags, DDBLTFX *fx)
{
	SELF(Surface);
	Surface *src = src_;
	RECT d = dr ? *dr : (RECT){ -self->org, 0, self->w - self->org, self->h }; // no rectangle: the whole surface
	if (self->primary) { EnterCriticalSection(&g_composeLock); EnterCriticalSection(&g_lock); } // (not while the presenter builds a picture from the HD layers)
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
		LeaveCriticalSection(&g_lock); LeaveCriticalSection(&g_composeLock);
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
	if (self->primary) { EnterCriticalSection(&g_composeLock); EnterCriticalSection(&g_lock); } // (not while the presenter builds a picture from the HD layers)
	copy_rect(self, (int)x, (int)y, src, s.left, s.top, s.right - s.left, s.bottom - s.top);
	if (self->primary) {
		LeaveCriticalSection(&g_lock); LeaveCriticalSection(&g_composeLock);
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
		int mx0 = g_menuX - (PANEL_X - mapW), fy0 = max(24, g_menuY);
		if (menu_doubled() && sy >= 48 && sx >= mx0 && sx < mx0 + 2 * MENU_W && sy < 48 + 2 * (g_menuY + MENU_H - fy0)) {
			*x = g_menuX + (sx - mx0) / 2; *y = fy0 + (sy - 48) / 2; // over the doubled drop-down
		}
		else if (sy < 48) { *x = sx / 2 - g_extra; *y = sy / 2; }
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
	win_click(x, y);
}

// Remote mode: the driver's messages become the game's mouse and keys, in game coordinates.
static int g_fakeEdge;
static LRESULT remote_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	int n = msg - WM_REMOTE;
	if (n == 2 || n == 4 || n == 5) perf_input(); // the driver's clicks and keys: latency as a player's
	if (n == 0x10) {
		music_status(); // sound is muted in remote mode: the log says whether the music stream is playing
		logf_("view: map column %d, first x %d, selected county %d", *(int *)0x005651b4, *(int *)0x0052afdc, *(int *)0x0057c93c);
		InterlockedExchange(&g_shotWanted, 1);
		InterlockedExchange(&g_forcePresent, 1);
		mark_dirty();
		return 0;
	}
	if (n == 9) { // keep the next wParam frames shown (burst_frame)
		if (!g_burstWant) {
			g_burstN = 0;
			QueryPerformanceCounter(&g_burstT0);
			InterlockedExchange(&g_burstWant, (LONG)min(wp, BURST_MAX));
		}
		return 0;
	}
	if (n == 10) {
		if (g_burstWant) {
			InterlockedExchange(&g_burstFlush, 1);
			InterlockedExchange(&g_forcePresent, 1);
			mark_dirty();
		}
		return 0;
	}
	if (n == 18) { // testing: centre the view on map object wParam
		int r = army_cell((int)wp);
		if (r) ((int (__cdecl *)(int))0x0043278b)(r);
		logf_("centre on object %d: cell %d", (int)wp, r);
		return 0;
	}
	if (n == 19) { // testing: select army wParam, as a click on it does
		*(int *)0x0052afbc = (int)wp;
		((void (__cdecl *)(void))0x004371ce)();
		logf_("select object %d: screen %d", (int)wp, *(BYTE *)0x004eac50);
		return 0;
	}
	if (n == 21) { // testing: recording, wParam frames a second (0 stops); lParam as g_recCrop
		if (wp) g_recN = 0;
		if (wp) InterlockedExchange(&g_recCrop, (LONG)lp);
		InterlockedExchange(&g_recFps, (LONG)wp);
		InterlockedExchange(&g_forcePresent, 1);
		logf_("recording: %s", wp ? "on" : "off");
		return 0;
	}
	if (n == 22) { // testing: benchmark wParam pictures shown back to back (perf_end logs the averages)
		memset(g_perf.benchSt, 0, sizeof g_perf.benchSt);
		g_perf.benchTotal = g_perf.benchWorst = 0; g_perf.benchDone = 0;
		InterlockedExchange(&g_perf.bench, (LONG)(wp ? wp : 120));
		mark_dirty();
		return 0;
	}
	if (n == 23) { // testing: upscaling (wParam 0 bilinear, 1 Catmull-Rom, 2 FSR; +4 EASU final; lParam sharpness x100, or -1 none)
		g_upscaleCR = (int)(wp & 3) % 3;
		g_fsrFinal = (wp & 4) != 0;
		g_rcasSharp = (int)lp / 100.0f;
		logf_("upscaling: %s, sharpness %.2f%s", upscale_name(), g_rcasSharp, g_fsrFinal ? ", EASU final" : "");
		InterlockedExchange(&g_forcePresent, 1); mark_dirty();
		return 0;
	}
	if (n == 24) { // testing: the FPS counter: 0 off, 1 simple, 2 detailed
		InterlockedExchange(&g_statsOn, (LONG)(wp > 2 ? 2 : wp));
		InterlockedExchange(&g_forcePresent, 1); mark_dirty();
		return 0;
	}
	if (n == 20) { // testing: the army panel's frame kind
		g_apStyle = (int)wp;
		g_apSig = 2; // drawn again
		return 0;
	}
	if (n == 17) { // testing: list the map objects (table 0x52f0b0, 0x1a4 each) and their cells
		for (int i = 1; i <= 150; i++) {
			const BYTE *o = (const BYTE *)0x0052f0b0 + i * 0x1a4;
			if (!o[8]) continue;
			int row = -1, col = -1;
			for (int r = 0; r <= 0x80 && row < 0; r++)
				for (int c = 0; c < 0x41; c++) if (*(int *)(0x0055cea0 + r * 0x104 + c * 4) == i) { row = r; col = c; break; }
			logf_("object %d: type %d owner %d b2 %d b7 %d at row %d col %d", i, o[8], (signed char)o[0], o[2], o[7], row, col);
		}
		logf_("player %d, selected county %d, view col %d row %d, clicked object %d (afb4 %d), screen %d", *(int *)0x0057c8cc, *(int *)0x0057c93c, *(int *)0x005651b4, *(int *)0x005651b8,
			*(int *)0x0052afbc, *(int *)0x0052afb4, *(BYTE *)0x004eac50);
		return 0;
	}
	if (n == 15) { // testing: WASD keys held (wParam = bits as g_wasd)
		InterlockedExchange(&g_wasd, (LONG)wp);
		return 0;
	}
	if (n == 14) { // testing: save the next picture as on screen (lords2hd-shotview.bmp)
		InterlockedExchange(&g_shotViewWanted, 1);
		InterlockedExchange(&g_forcePresent, 1);
		mark_dirty();
		return 0;
	}
	if (n == 13) { // testing: save the next frame 1x as lords2hd-shot1x.bmp
		InterlockedExchange(&g_shot1xWanted, 1);
		InterlockedExchange(&g_forcePresent, 1);
		mark_dirty();
		return 0;
	}
	if (n == 12) { // testing: the captured-county scroll and its video (FUN_0047309e, type 13)
		*(int *)0x005440a4 = 0xd;
		*(int *)0x0057c954 = 2000;
		*(int *)0x0053e910 = (int)wp ? (int)wp : 0x72;
		*(int *)0x0055ce54 = 0;
		*(BYTE *)0x004eac50 = 0x27;
		h_msg_a();
		mark_dirty();
		return 0;
	}
	if (n == 11) { // testing: the end-of-battle scroll and its video over a running battle (screen 43)
		g_battleEndKeep = wp == 1;
		*(BYTE *)0x004eac50 = 0x2b;
		*(int *)0x005533d0 = 1;
		((void (__cdecl *)(void))0x00423241)();
		((BOOL (__cdecl *)(const char *, int, int, int, int))0x0042d91b)("bat_win1.smk", 0x27, 0x49, 0, 0x2b);
		mark_dirty();
		return 0;
	}
	if (n == 8) { // play a video now (testing): wParam = index in the table, at the place the game uses
		static const struct { const char *name; int x, y; } v[] = {
			{ "hang.smk", 25, 81 }, { "jail.smk", 25, 81 }, { "axmen.smk", 25, 81 }, { "cart_kgt.smk", 25, 81 },
			{ "win_game.smk", 25, 81 }, { "intro.smk", 0x27, 0x49 }, { "credits.smk", 0, 0 },
			{ "castle1.smk", 0x9e, 0x14 }, { "bat_win1.smk", 0x27, 0x49 }, { "imptitle.smk", 0x50, 0x50 },
		};
		if (wp < sizeof v / sizeof v[0]) {
			static char name[16];
			lstrcpynA(name, v[wp].name, sizeof name);
			((BOOL (__cdecl *)(const char *, int, int, int, int))0x0042d91b)(name, v[wp].x, v[wp].y, 0, *(BYTE *)0x004eac50);
		}
		return 0;
	}
	if (n == 7) { // hold the mouse at screen edges: 1 left, 2 right, 4 top, 8 bottom, 0 = let go
		g_fakeEdge = (int)wp;
		return 0;
	}
	if (n == 4) {
		if (menu_key((int)wp)) return 0;
		if ((wp == VK_OEM_COMMA || wp == VK_OEM_PERIOD) && cycle_key(wp == VK_OEM_PERIOD ? 1 : -1)) return 0;
		CallWindowProcA(g_gameProc, hwnd, WM_KEYDOWN, wp, 1);
		return CallWindowProcA(g_gameProc, hwnd, WM_KEYUP, wp, 0xC0000001);
	}
	static const UINT mouse[] = { 0, WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, 0, WM_RBUTTONDOWN, WM_RBUTTONUP };
	static WPARAM buttons;
	if (n < 1 || n > 6 || !mouse[n]) return 0;
	int fx = (short)LOWORD(lp), fy = (short)HIWORD(lp);
	win_click(&fx, &fy); // the driver aims at the picture, like a player
	g_fake.x = fx;
	g_fake.y = fy;
	lp = MAKELPARAM((WORD)(short)fx, (WORD)(short)fy);
	if (n == 6 && g_menuEatRUp) { g_menuEatRUp = FALSE; return 0; }
	if (g_menuOpen) { if (n == 2) menu_click(-1, -1, fx, fy); if (n == 5) { menu_toggle(); g_menuEatRUp = TRUE; } return 0; } // the scroll only
	{
		static BOOL btnDown;
		int btn = cycle_button_at(fx, fy);
		if (n == 2 && btn) { cycle_key(btn); btnDown = TRUE; InterlockedExchange(&g_forcePresent, 1); mark_dirty(); return 0; }
		if (n == 3 && btnDown) { btnDown = FALSE; return 0; }
		if (army_panel_at(fx, fy)) return 0; // nor its moves: the map would point the army there
	}
	if (n == 2) buttons |= MK_LBUTTON; else if (n == 3) buttons &= ~MK_LBUTTON;
	if (n == 5) buttons |= MK_RBUTTON; else if (n == 6) buttons &= ~MK_RBUTTON;
	return CallWindowProcA(g_gameProc, hwnd, mouse[n], buttons, lp);
}

static void remote_hide(HWND hwnd)
{
	// [Debug] RemoteVisible=1: the window stays on screen (a fullscreen window goes to the display
	// without the compositor, as for a player: frame pacing can only be measured so), still taking its
	// input from the driver only.
	if (GetPrivateProfileIntA("Debug", "RemoteVisible", 0, g_iniPath)) {
		// RemoteVisible=2: also in front, as a player's game is (only then may the window go to the display
		// directly; behind another window it is composed)
		if (GetPrivateProfileIntA("Debug", "RemoteVisible", 0, g_iniPath) == 2) {
			BOOL ok = SetForegroundWindow(hwnd);
			logf_("remote testing mode: visible, in front: %s", ok ? "yes" : "refused");
		}
		else logf_("remote testing mode: visible");
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\lords2hd-remote.txt", g_gameDir);
		FILE *f = fopen(path, "w");
		if (f) { fprintf(f, "%lu %lu\n", (unsigned long)(ULONG_PTR)hwnd, GetCurrentProcessId()); fclose(f); }
		return;
	}
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

// Running in the background ([Game] RunInBackground): 0 = the game pauses when another window is
// active (original), 1 = it always keeps running, 2 = it keeps running in a multiplayer game only
// (DAT_00553030 is 1 while a DirectPlay session is open), where a paused player stalls everyone.
static int g_runBackground = 2;
static BOOL keep_running(void)
{
	if (g_runBackground == 1) return TRUE;
	if (g_runBackground != 2) return FALSE;
	if (*(int *)0x00553030) return TRUE;
	// A replacement multiplayer layer (Wolfe's dplayx.dll) knows about its lobby too.
	static BOOL (__cdecl *sessionOpen)(void); static BOOL looked;
	if (!looked) { HMODULE m = GetModuleHandleA("dplayx.dll"); if (m) sessionOpen = (BOOL (__cdecl *)(void))GetProcAddress(m, "LF_NetSessionOpen"); looked = TRUE; }
	return sessionOpen && sessionOpen();
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || (msg == WM_KEYDOWN && !(lp & 0x40000000))) perf_input(); // latency (perf_end)
	if (g_remote) {
		if (msg > WM_REMOTE && msg <= WM_REMOTE + 0x20) return remote_message(hwnd, msg, wp, lp);
		if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
		if (msg == WM_ACTIVATEAPP && lp != 0x4C32) wp = TRUE; // the game would pause, thinking it is in the background (0x4C32: a test's real one)
		if (msg == WM_ACTIVATE) wp = WA_ACTIVE;
		if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0; // only the driver's mouse
	}
	// The settings panel: F10 opens it; while open, keys and the mouse are its own.
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && menu_key((int)wp)) return 0;
	if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wp == VK_F10) return 0; // not Windows' menu key
	// WASD: held keys scroll (wasd_edges); their letters are not typed on the map or battlefield
	if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wasd_bit(wp)) {
		if (msg == WM_KEYDOWN) InterlockedOr(&g_wasd, wasd_bit(wp)); else InterlockedAnd(&g_wasd, ~wasd_bit(wp));
		if (wasd_screen()) return 0;
	}
	if (msg == WM_CHAR && wasd_screen() && wasd_bit((WPARAM)toupper((int)wp))) return 0;
	if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp)) InterlockedExchange(&g_wasd, 0);
	if (msg == WM_KEYDOWN && (wp == VK_OEM_COMMA || wp == VK_OEM_PERIOD) && cycle_key(wp == VK_OEM_PERIOD ? 1 : -1)) return 0;
	if (msg == WM_CHAR && (wp == ',' || wp == '.') && (*(BYTE *)0x004eac50 == 0 || *(BYTE *)0x004eac50 == 16) && *(int *)0x0052af94 == 3 && g_cycleOn) return 0;
	if (msg == WM_RBUTTONUP && g_menuEatRUp) { g_menuEatRUp = FALSE; return 0; } // the release of the right-click that closed it
	if (g_menuOpen) {
		if (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR) return 0;
		if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
			if (msg == WM_LBUTTONDOWN) { int mx = (short)LOWORD(lp), my = (short)HIWORD(lp); client_to_game(&mx, &my); menu_click((short)LOWORD(lp), (short)HIWORD(lp), mx, my); }
			if (msg == WM_RBUTTONDOWN) { menu_toggle(); g_menuEatRUp = TRUE; } // a right-click closes it, as the game's own menus
			return 0;
		}
	}
	if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL) {
		int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
		client_to_game(&x, &y);
		// the < > buttons on the name plate: theirs, not the game's
		static BOOL btnDown;
		int btn = cycle_button_at(x, y);
		if (msg == WM_LBUTTONDOWN && btn) { cycle_key(btn); btnDown = TRUE; InterlockedExchange(&g_forcePresent, 1); mark_dirty(); return 0; }
		if (msg == WM_LBUTTONUP && btnDown) { btnDown = FALSE; return 0; }
		if (army_panel_at(x, y)) return 0; // nor its moves: the map would point the army there
		lp = MAKELPARAM(x, y);
	}
	else if (msg == WM_PAINT || msg == WM_SIZE || msg == WM_ACTIVATEAPP) {
		InterlockedExchange(&g_forcePresent, 1);
		mark_dirty();
	}
	if (msg == WM_SIZE && wp == SIZE_MINIMIZED && keep_running()) wp = SIZE_RESTORED; // see h_IsIconic
	if (msg == WM_ACTIVATEAPP && !wp && keep_running()) {
		// The game clears its active flag (DAT_004e65a8) and its main loop then only sleeps
		// (Sleep(200) instead of a frame), so in multiplayer every other player waits too.
		LRESULT r = CallWindowProcA(g_gameProc, hwnd, msg, wp, lp);
		*(int *)0x004e65a8 = 1;
		return r;
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

// WASD scrolls the map and the battlefield ([Display] WASD=1): while a key is held the game is told
// the cursor sits at that screen edge, so it scrolls exactly as with the mouse (its own speed,
// diagonals, battles). Only on the county map (screen 0) and the battlefield (41), where nothing
// is typed; the keys are tracked from the window's own key messages (and the test driver).
static volatile LONG g_wasd;      // 1 left, 2 right, 4 up, 8 down
static BOOL g_wasdOn = TRUE;
static BOOL wasd_screen(void) { BYTE s = *(BYTE *)0x004eac50; return g_wasdOn && !g_menuOpen && *(int *)0x0052af94 == 3 && (s == 0 || s == 41); }
static int wasd_bit(WPARAM vk) { return vk == 'A' ? 1 : vk == 'D' ? 2 : vk == 'W' ? 4 : vk == 'S' ? 8 : 0; }
static void wasd_edges(POINT *p, int right, int bottom)
{
	int k = g_wasd;
	if (!k || !wasd_screen()) return;
	if (k & 1) p->x = 0; else if (k & 2) p->x = right;
	if (k & 4) p->y = 0; else if (k & 8) p->y = bottom;
}
// , and . cycle through the player's counties and then armies on the county map, centring the
// view on each as the minimap does ([Display] CycleKeys=1). Counties: owner 0x53f9b5 + c*0x300,
// town cell record 0x53fa20 + c*0x300 (FUN_0043253a selects one this way). Armies: map objects
// (0x52f0b0 + i*0x1a4, 150) of type 1 owned by the player; an object's cell is found through the
// cell grid (0x55cea0, 0x81 rows of 0x41 records) and each record's object list (head at byte
// 0x522f95 + record, next at +4 of the object). FUN_0043278b(record) centres the view on it.
static BOOL g_cycleOn = TRUE;
static int army_cell(int id)
{
	for (int r = 0; r <= 0x80; r++)
		for (int c = 0; c < 0x41; c++) {
			int rec = *(int *)(0x0055cea0 + r * 0x104 + c * 4);
			if (rec <= 0 || rec >= 0xfff0000) continue;
			for (int o = ((BYTE *)0x00522f95)[rec], guard = 0; o && guard < 160; o = *(short *)(0x0052f0b4 + o * 0x1a4), guard++)
				if (o == id) return rec;
		}
	return 0;
}
// With one of the player's armies selected (screen 16, the army in 0x57c8c4) the keys go through
// the armies, each selected as if clicked (FUN_004371ce: it asks first, as a click does, when the
// army already has orders); otherwise through the counties, from the selected one (0x57c93c).
static BOOL cycle_key(int dir)
{
	BYTE screen = *(BYTE *)0x004eac50;
	if (!g_cycleOn || g_menuOpen || *(int *)0x0052af94 != 3 || (screen != 0 && screen != 16) || *(int *)0x0057c8fc != 0 || *(int *)0x0057cb18 == 2) return FALSE;
	int me = *(int *)0x0057c8cc;
	int sel = *(int *)0x0057c8c4;
	BOOL armies = screen == 16 && sel > 0 && sel <= 150 && ((BYTE *)0x0052f0b8)[sel * 0x1a4] == 1 && ((signed char *)0x0052f0b0)[sel * 0x1a4] == me;
	int list[160], rec[160], n = 0, at = -1;
	if (armies) {
		for (int i = 1; i <= 150 && n < 160; i++) {
			const BYTE *o = (const BYTE *)0x0052f0b0 + i * 0x1a4;
			if (o[8] != 1 || (signed char)o[0] != me) continue;
			int r = army_cell(i);
			if (!r) continue; // not standing on the map (in a castle, at sea)
			if (i == sel) at = n;
			list[n] = i; rec[n++] = r;
		}
	}
	else {
		int cur = *(int *)0x0057c93c;
		for (int c = 1; c < 32 && n < 160; c++) {
			int r = *(int *)(0x0053fa20 + c * 0x300);
			if (((BYTE *)0x0053f9b5)[c * 0x300] != me || !r) continue;
			if (c == cur) at = n;
			list[n] = c; rec[n++] = r;
		}
	}
	if (!n) return TRUE;
	int k = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
	((int (__cdecl *)(int))0x0043278b)(rec[k]); // centre the view on it
	if (armies) {
		*(int *)0x0052afbc = list[k];
		((void (__cdecl *)(void))0x004371ce)(); // select it, as a click on it does
	}
	else {
		*(int *)0x0057c93c = list[k];
		*(int *)0x00567d0c = 1;
	}
	logf_("cycle: %s %d", armies ? "army" : "county", list[k]);
	return TRUE;
}
// Two arrow buttons on the side panel's name plate, either side of the county's name, or at the
// foot of the army panel (army_panel_keep) when one of the player's armies is selected: clicking
// them does what , and . do. Drawn into the 2x picture; their clicks never reach the game.
#define AP_W (8 * 16)
#define AP_H (16 * 16)
#define AP_X (0x1dc - AP_W)
#define AP_Y (GAME_H - 8 - AP_H) // on the map's bottom edge: the rows under it are not the map's
static int panel_army(void);
static int player_armies(void);
static const RECT kCycleBtn[2] = { { 487, 162, 503, 180 }, { 615, 162, 631, 180 } }; // game coords: < and >
static BOOL cycle_btn_rect(int b, RECT *r)
{
	if (!g_cycleOn || g_menuOpen || *(int *)0x0052af94 != 3 || *(int *)0x0057c8fc != 0 || *(int *)0x0057cb18 == 2) return FALSE;
	BYTE s = *(BYTE *)0x004eac50;
	if (s == 16) {
		if (!panel_army() || player_armies() < 2) return FALSE; // one army: nothing to go to
		SetRect(r, b ? AP_X + AP_W - 24 : AP_X + 8, AP_Y + AP_H - 40, b ? AP_X + AP_W - 8 : AP_X + 24, AP_Y + AP_H - 22);
		return TRUE;
	}
	int c = *(int *)0x0057c93c; // the name plate is up for the player's own county
	if (s != 0 || c <= 0 || c >= 32 || ((BYTE *)0x0053f9b5)[c * 0x300] != *(int *)0x0057c8cc) return FALSE;
	*r = kCycleBtn[b];
	return TRUE;
}
static int cycle_button_at(int x, int y)
{
	for (int b = 0; b < 2; b++) {
		RECT r;
		if (!cycle_btn_rect(b, &r)) return 0;
		InflateRect(&r, 3, 3);
		if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return b ? 1 : -1;
	}
	return 0;
}
// a click on the army panel elsewhere than its buttons does nothing (it must not send the army there)
static BOOL army_panel_at(int x, int y) { return panel_army() && x >= AP_X && x < AP_X + AP_W && y >= AP_Y && y < AP_Y + AP_H; }
// The game reads the mouse for the map too (the path and cursor of a move): over the scroll it is
// told the last place outside it, so hovering or tapping the scroll never aims the army.
static int g_apOutX = 1, g_apOutY = 1;
static BOOL g_apOver; // the mouse is over the scroll: the game's plain arrow is shown (h_SetCursor)
static HCURSOR WINAPI h_SetCursor(HCURSOR cur);
static void ap_mouse(LONG *x, LONG *y)
{
	BOOL over = army_panel_at(*x, *y);
	if (over) { *x = g_apOutX; *y = g_apOutY; }
	else { g_apOutX = *x; g_apOutY = *y; }
	if (over != g_apOver) { // the cursor changes as it crosses the scroll's edge, not at the next move
		g_apOver = over;
		HCURSOR c = *(HCURSOR *)0x004ea514; // the game's current one (FUN_004b1cf3)
		if (c) h_SetCursor(c);
	}
}
// how many of the player's armies stand on the map (counted at most 4 times a second)
static int player_armies(void)
{
	static DWORD at; static int n;
	DWORD now = GetTickCount();
	if (at && now - at < 250) return n;
	at = now; n = 0;
	int me = *(int *)0x0057c8cc;
	for (int r = 0; r <= 0x80; r++)
		for (int c = 0; c < 0x41; c++) {
			int rec = *(int *)(0x0055cea0 + r * 0x104 + c * 4);
			if (rec <= 0 || rec >= 0xfff0000) continue;
			for (int o = ((BYTE *)0x00522f95)[rec], guard = 0; o && guard < 160; o = *(short *)(0x0052f0b4 + o * 0x1a4), guard++)
				if (o > 0 && o <= 150 && ((BYTE *)0x0052f0b8)[o * 0x1a4] == 1 && ((signed char *)0x0052f0b0)[o * 0x1a4] == me) n++;
		}
	return n;
}
// into the 2x frame (sw columns from game x sx - g_off)
static void cycle_buttons_draw(int sx, int sw)
{
	for (int b = 0; b < 2; b++) {
		RECT r;
		if (!cycle_btn_rect(b, &r)) return;
		int X0 = 2 * (r.left + g_off - sx), Y0 = 2 * r.top, W = 2 * (r.right - r.left), H = 2 * (r.bottom - r.top);
		if (X0 < 0 || X0 + W > 2 * sw) continue;
		for (int y = 0; y < H; y++) {
			// a triangle pointing out from the name: its width grows to the middle row
			int half = H / 2, d = y < half ? y : H - 1 - y, len = d * W / half;
			for (int x = 0; x < W; x++) {
				int t = b ? x : W - 1 - x; // distance from the tip
				if (t > len) continue;
				BOOL edge = t >= len - 2 || d < 2;
				g_frame2x[(Y0 + y) * 2 * sw + X0 + x] = edge ? 0x3a1e0c : 0x8a2a14; // dark brown edge, red body
			}
		}
	}
}
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
		wasd_edges(p, right, bottom);
		ap_mouse(&p->x, &p->y);
		return TRUE;
	}
	if (!o_GetCursorPos(p)) return FALSE;
	if (g_menuOpen) { p->x = GAME_W / 3; p->y = GAME_H / 2; return TRUE; } // the map must not scroll under the panel
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
		if (g_off) wasd_edges(p, max(2, SCROLL_EDGE_W - 1), max(2, *(int *)0x004ea838 - 1));
		ap_mouse(&p->x, &p->y);
	}
	return TRUE;
}

// Remote mode: the game believes it is in front, and never brings itself there.
static HWND (WINAPI *o_GetForegroundWindow)(void);
static BOOL (WINAPI *o_SetForegroundWindow)(HWND);
static HWND WINAPI h_GetForegroundWindow(void) { return (g_remote || keep_running()) && g_hwnd ? g_hwnd : o_GetForegroundWindow(); }
// Running in the background also covers a minimised window: the game's WM_SIZE / WM_MOVE and
// activation handlers ask IsIconic and, minimised, stop instead of setting their active flag.
static BOOL (WINAPI *o_IsIconic)(HWND);
static BOOL WINAPI h_IsIconic(HWND hwnd) { return hwnd && hwnd == g_hwnd && keep_running() ? FALSE : o_IsIconic(hwnd); }
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
	if (g_apOver && *(HCURSOR *)0x004ea834) cur = *(HCURSOR *)0x004ea834; // the plain arrow over the army scroll
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
			// "#23": an import by number (smackw32.dll's are all by number)
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
				if (name[0] != '#' || IMAGE_ORDINAL(names->u1.Ordinal) != (WORD)atoi(name + 1)) continue;
			}
			else {
				IMAGE_IMPORT_BY_NAME *imp = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
				if (strcmp((char *)imp->Name, name) != 0) continue;
			}
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
// FUN_0040477b(top, bottom), the same for rows: a scroll's tiles stop at row 480. The zoomed-out
// battlefield is taller; the settings scroll in its middle reaches below that (g_drawingLow).
static clip_x_t o_clip_y;
static BOOL g_drawingLow;
static void __cdecl h_clip_y(int top, int bottom)
{
	if (g_drawingLow && bottom == GAME_H && battle_zoom_on()) bottom = g_frameH;
	o_clip_y(top, bottom);
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
	if (zoomed) {
		title[0] = (short)menu_list_x(saved);
		g_menuX = title[0];
		g_menuY = title[2] + 0x12;
		g_menuH = ((title[6] * 21) / 16 + 2) * 16; // FUN_0040c725's panel: 12 cells wide, rows for the items
	}
	else g_menuX = -10000;
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

// Message scrolls over the county map (tutorial pages, events, reports) are placed for the 4:3
// map, centred over game x 0..PANEL_X. On the wide map they move left by half the extra columns,
// to the middle of the map the player sees. Three functions draw them (FUN_0047309e, FUN_00475e07,
// FUN_00476488); each sets the scroll's x (DAT_00553060) from a constant and draws everything from
// it, and the close arrow's click spot follows (FUN_0040d1bc keeps it). The constants, and the
// x of the button tables some scrolls use for drawing and clicks (FUN_0040cfd2 / FUN_0040da1e),
// are rewritten before each draw for the current layout: shifted on the wide county map, as
// shipped anywhere else (a battle, the 4:3 screen). Scrolls that show a video keep their place.
static const DWORD kScrollXSites[] = { // movl $imm, 0x553060: the imm's address
	0x47313b, 0x4732fb, 0x4734c5, 0x47372c, 0x473bbe, 0x473eb9, 0x474051, 0x4741f8, 0x474212,
	0x4744e7, 0x47473d, 0x474a4e, 0x474e23, 0x4753b0, 0x4755cd, 0x4755fa, 0x475e9a, 0x4764e7,
};
static const DWORD kScrollButtons[] = { 0x4dda90, 0x4ddac0, 0x4ddaf0, 0x4ddb20, 0x4ddb50 }; // 2 buttons of 24 bytes: x, y, picture, size, ...
#define BTN_FIELD(i, b) ((short *)(kScrollButtons[i] + 24 * (b))) // button b's x
static LONG g_scrollXOrig[sizeof kScrollXSites / sizeof kScrollXSites[0]];
static short g_scrollBtnOrig[sizeof kScrollButtons / sizeof kScrollButtons[0]][2];
static int g_scrollShift, g_scrollReady;
// The three scrolls that show a video (a captured county: cap_cty at 40,105; event clips at 25,81
// and 89,105) are drawn from x values written into the code as push operands, mostly single
// signed bytes. They move with the others when every value still fits (s <= 136: monitors up to
// about 16:9); otherwise these three stay where the game puts them, whole.
static const struct { DWORD addr; BYTE size; short orig; } kVideoScrollX[] = {
	{ 0x474cc9, 1, 0x10 }, { 0x474cdf, 1, 0x27 }, { 0x474cf0, 4, 0x1a0 }, { 0x474d0b, 1, 0x10 }, { 0x474d43, 1, 0x30 },
	{ 0x474f8b, 1, 0x08 }, { 0x474fa1, 1, 0x18 }, { 0x474fb2, 4, 0x1f8 }, { 0x474fdd, 1, 0x08 }, { 0x475012, 1, 0x08 }, { 0x47504e, 1, 0x18 },
	{ 0x475070, 1, 0x10 }, { 0x475086, 1, 0x58 }, { 0x475097, 4, 0x1a0 }, { 0x4750c2, 1, 0x10 }, { 0x4750f7, 1, 0x10 }, { 0x47512a, 1, 0x10 }, { 0x475156, 1, 0x30 },
};
static int g_videoScrollShift, g_videoScrollReady;
static void video_scroll_shift(int s)
{
	int n = sizeof kVideoScrollX / sizeof kVideoScrollX[0];
	if (!g_videoScrollReady) {
		g_videoScrollReady = 1;
		for (int i = 0; i < n; i++) {
			BYTE *p = (BYTE *)kVideoScrollX[i].addr;
			LONG v = kVideoScrollX[i].size == 1 ? (signed char)p[0] : *(LONG *)p;
			if (p[-1] != (kVideoScrollX[i].size == 1 ? 0x6a : 0x68) || v != kVideoScrollX[i].orig) {
				logf_("video scrolls: unexpected code at %p, left in place", p);
				g_videoScrollReady = -1;
				return;
			}
		}
		for (int i = 0; i < n; i++) { DWORD old; VirtualProtect((void *)kVideoScrollX[i].addr, 4, PAGE_EXECUTE_READWRITE, &old); }
	}
	if (g_videoScrollReady < 0) return;
	if (s > 136) s = 0; // a push byte holds -128..127: the smallest value (8) would not fit
	if (s == g_videoScrollShift) return;
	for (int i = 0; i < n; i++) {
		BYTE *p = (BYTE *)kVideoScrollX[i].addr;
		if (kVideoScrollX[i].size == 1) p[0] = (BYTE)(signed char)(kVideoScrollX[i].orig - s);
		else *(LONG *)p = kVideoScrollX[i].orig - s;
	}
	FlushInstructionCache(GetCurrentProcess(), NULL, 0);
	g_videoScrollShift = s;
}

static void scroll_shift(int s)
{
	if (!g_scrollReady) {
		for (int i = 0; i < (int)(sizeof kScrollXSites / sizeof kScrollXSites[0]); i++) {
			BYTE *p = (BYTE *)kScrollXSites[i];
			if (p[0] != 0xC7 || p[1] != 0x05 || *(DWORD *)(p + 2) != 0x553060) { logf_("message scrolls: unexpected code at %p, left in place", p); g_scrollReady = -1; return; }
			g_scrollXOrig[i] = *(LONG *)(p + 6);
			DWORD old;
			VirtualProtect(p + 6, 4, PAGE_EXECUTE_READWRITE, &old);
		}
		for (int i = 0; i < (int)(sizeof kScrollButtons / sizeof kScrollButtons[0]); i++) {
			DWORD old;
			VirtualProtect((void *)kScrollButtons[i], 48, PAGE_READWRITE, &old);
			for (int b = 0; b < 2; b++) g_scrollBtnOrig[i][b] = *BTN_FIELD(i, b);
		}
		g_scrollReady = 1;
	}
	if (g_scrollReady < 0 || s == g_scrollShift) return;
	for (int i = 0; i < (int)(sizeof kScrollXSites / sizeof kScrollXSites[0]); i++)
		*(LONG *)(kScrollXSites[i] + 6) = g_scrollXOrig[i] - s;
	for (int i = 0; i < (int)(sizeof kScrollButtons / sizeof kScrollButtons[0]); i++)
		for (int b = 0; b < 2; b++) *BTN_FIELD(i, b) = (short)(g_scrollBtnOrig[i][b] - s);
	logf_("message scrolls moved %d left; first table's buttons at x %d and %d", s, *BTN_FIELD(0, 0), *BTN_FIELD(0, 1));
	FlushInstructionCache(GetCurrentProcess(), NULL, 0);
	g_scrollShift = s;
}

typedef void (__cdecl *draw_msg_t)(void);
static draw_msg_t o_msg_a, o_msg_b, o_msg_c;
static BOOL county_map_on(void);
static void draw_msg(draw_msg_t orig)
{
	// Only over the map itself: a scroll opened from a window on the map (the Town Center: screen 2)
	// stays lined up with that window, which keeps its own place. 0x4f0350 = the screen under it.
	BOOL wide = g_off && county_map_on() && *(int *)0x0057c8fc != 2 && *(int *)0x004f0350 == 0;
	scroll_shift(wide ? g_extra / 2 : 0);
	video_scroll_shift(wide ? g_extra / 2 : 0);
	g_drawingLeft = wide;
	orig();
	g_drawingLeft = FALSE;
}
static void __cdecl h_msg_a(void) { draw_msg(o_msg_a); }
static void __cdecl h_msg_b(void) { draw_msg(o_msg_b); }
static void __cdecl h_msg_c(void) { draw_msg(o_msg_c); }

// The yes/no box ("Autocalc battle?", "Exit the game?"): FUN_0040e6f2(text, x, y, answer) keeps x, y
// in DAT_0052af90/98; the box (FUN_0040ccfa, drawn every frame by FUN_004ba26e) and its two buttons
// (drawn and hit-tested by FUN_004ba26e/FUN_004ba9c8 from those) all start there. Every caller asks
// for 160,160, which sits right of centre on the wide map and the zoomed-out battlefield: there x
// is moved so the box (x - 16 .. x + 208) is centred over what the player sees.
typedef void (__cdecl *yesno_open_t)(int, int, int, int);
static yesno_open_t o_yesno_open;
static draw_msg_t o_yesno_draw;
static BOOL g_yesnoWide;
static void __cdecl h_yesno_open(int text, int x, int y, int answer)
{
	// Old-game bug: Esc while "Exit the game?" is up opens it again, and the box then remembers its
	// own screen (30) as the one to go back to (0x57c90c): "No" keeps returning to it, the game is
	// stuck behind the box. A second open while one is up answers "No" instead, as the thumbs-down
	// (the right one of the box's two buttons, table 0x4dd310: 24-byte entries, x first, action at +8).
	if (*(BYTE *)0x004eac50 == 0x1e) {
		const BYTE *t = (const BYTE *)0x004dd310;
		const BYTE *no = *(const short *)(t + 24) > *(const short *)t ? t + 24 : t;
		void (__cdecl *act)(void) = *(void (**)(void))(no + 8);
		logf_("yes/no box: opened again while up, answered No");
		if (act) {
			*(int *)0x0059154c = *(const int *)(no + 16);
			*(int *)0x00591550 = *(const int *)(no + 20);
			*(int *)0x00591558 = 0;
			act();
		}
		return;
	}
	g_yesnoWide = FALSE;
	if (g_off && x == 160 && county_map_on() && (battle_zoom_on() || *(int *)0x0057c8fc != 2)) {
		int centre = battle_zoom_on() ? PANEL_X - battle_map_w() / 2 : (PANEL_X - g_extra) / 2;
		x = (centre - 96) & ~1;
		g_yesnoWide = TRUE;
	}
	o_yesno_open(text, x, y, answer);
}
static void __cdecl h_yesno_draw(void)
{
	BOOL wide = g_yesnoWide && *(int *)0x0052af90 != 160;
	g_drawingLeft = wide;
	// Not hooked in 4:3 (the wide hooks are installed only with Wide=1): the game's own, unchanged
	// FUN_0040ccfa. yesno_keep calls here in every mode; calling the empty trampoline crashed.
	if (o_yesno_draw) o_yesno_draw(); else ((draw_msg_t)0x0040ccfa)();
	g_drawingLeft = FALSE;
}

// The game draws the yes/no box once, when it opens (and on some refreshes). If the screen behind
// is redrawn afterwards - closing the drop-down menu the question came from redraws the map where
// the menu was - part of the box is gone and stays gone ("torn"). While the box is up (screen 30)
// its pixels are checked after every game frame and it is drawn again when anything changed them.
#define YN_W (14 * 16)
#define YN_H (8 * 16)
static BYTE g_ynSnap[YN_W * YN_H];
static int g_ynHave, g_ynX, g_ynY;
static BOOL yn_region(BOOL save)
{
	if (!g_back) return FALSE;
	int x0 = *(int *)0x0052af90 - 16, y0 = *(int *)0x0052af98 - 16;
	BOOL same = TRUE;
	for (int r = 0; r < YN_H; r++) {
		int y = y0 + r;
		if (y < 0 || y >= GAME_H) continue;
		for (int c = 0; c < YN_W; c++) {
			int x = g_back->org + x0 + c;
			if (x < 0 || x >= g_back->w) continue;
			BYTE *p = g_back->bits + y * g_back->pitch + x;
			if (save) g_ynSnap[r * YN_W + c] = *p;
			else if (g_ynSnap[r * YN_W + c] != *p) { same = FALSE; r = YN_H; break; }
		}
	}
	return same;
}
static void yesno_keep(void)
{
	if (*(BYTE *)0x004eac50 != 0x1e || !g_back) { g_ynHave = 0; return; }
	int x = *(int *)0x0052af90, y = *(int *)0x0052af98;
	if (g_ynHave && x == g_ynX && y == g_ynY && yn_region(FALSE)) return;
	h_yesno_draw();                                                                       // FUN_0040ccfa
	((void (__cdecl *)(int, int, short *, int))0x0040cfd2)(x, y, (short *)0x004dd310, 2); // its two buttons
	((void (__cdecl *)(void))0x0045240a)();                                               // to the screen
	yn_region(TRUE);
	g_ynHave = 1; g_ynX = x; g_ynY = y;
}

// The end of a battle (screen 43): FUN_00423241 draws a scroll with the result, the video's frame
// and a close arrow, all at fixed x (the scroll 16 .. 464, centred over the 4:3 battlefield), and
// FUN_00477dfc starts the result video (bat_winN.smk) at 39,73. In the zoomed-out battle both move
// left by the same amount, to the middle of the battlefield the player sees. Most of the x values
// are single signed bytes in the code, too small for the move, so FUN_00423241 is done here in
// full: the same calls with x shifted. The close arrow's click spot follows (FUN_0040d1bc).
static int battle_end_shift(void) { return battle_zoom_on() && !g_battleEndKeep ? 240 - (PANEL_X - battle_map_w() / 2) : 0; }
static void frame_box(int x, int y, int w, int h);
typedef void (__cdecl *battle_end_draw_t)(void);
static battle_end_draw_t o_battle_end_draw;
static void __cdecl h_battle_end_draw(void)
{
	void (__cdecl *prepare)(void) = (void *)0x004b1de0, (__cdecl *redraw)(void) = (void *)0x0045240a;
	void (__cdecl *video_frame_on)(void) = (void *)0x0047703a, (__cdecl *video_prepare)(void) = (void *)0x004b1310;
	void (__cdecl *scroll)(int, int, int, int) = (void *)0x004093e0;
	void (__cdecl *arrow)(int, int, int) = (void *)0x0040d1bc;
	void (__cdecl *line)(int, int, int, int, void *, int) = (void *)0x00402d37;
	void (__cdecl *para)(int, int, int, int, int, int, int, int, void *, int) = (void *)0x0040328e;
	void (__cdecl *box)(int, int, int, int) = (void *)0x00403deb;
	void *title = (void *)0x005b2fa0, *body = (void *)0x005af8f0;
	int s = battle_end_shift(), result = *(int *)0x00553fb8;
	prepare();
	*(int *)0x004eb260 = 2;
	*(int *)0x004e65f8 = 1;
	redraw();
	g_drawingLeft = s != 0;
	if (*(int *)0x005533d0 == 0) {
		scroll(0x10 - s, 0x90, 0x1c, 10);
		arrow(0x1a0 - s, 0x100, 0);
		line(0x52, 0xc, 0x30 - s, 0xa8, title, 0x3f);
		para(0x52, 0xd, 0x30 - s, 0xe0, 0x180, 100, 0, 0, body, 0x3f);
	}
	else if (*(int *)0x0053f248 == 0) {
		scroll(0x10 - s, 0x90, 0x1c, 10);
		arrow(0x1a0 - s, 0x100, 0);
		line(0x52, result * 2, 0x30 - s, 0xa8, title, 0x3f);
		para(0x52, result * 2 + 1, 0x30 - s, 0xe0, 0x180, 100, 0, 0, body, 0x3f);
	}
	else {
		video_frame_on();
		video_prepare();
		scroll(0x10 - s, 0x30, 0x1c, 0x16);
		if (0x27 - s >= 1) box(0x27 - s, 0x48, 0x192, 0xc2);
		else frame_box(0x27 - s, 0x48, 0x192, 0xc2);
		arrow(0x1a0 - s, 0x160, 0);
		line(0x52, result * 2, 0x30 - s, 0x138, title, 0x3f);
		para(0x52, result * 2 + 1, 0x30 - s, 0x158, 0x180, 100, 0x20 - s, 0x1a0, body, 0x3f);
	}
	g_drawingLeft = FALSE;
}
// FUN_00403deb's frame (top and right lines colour 0x10, bottom and left 0x1f) for an x left of 0,
// which it clamps to 0 while keeping the width (the frame landed at 0 .. 401, apart from the video).
static void frame_px(int x, int y, BYTE colour)
{
	if (y >= 0 && y < GAME_H && x + g_back->org >= 0 && x + g_back->org < g_back->w)
		g_back->bits[y * g_back->pitch + g_back->org + x] = colour;
}
static void frame_box(int x, int y, int w, int h)
{
	if (!g_back) return;
	int x1 = x + w - 1, y1 = y + h - 1;
	for (int c = x; c <= x1; c++) frame_px(c, y, 0x10);   // top
	for (int r = y; r <= y1; r++) frame_px(x1, r, 0x10);  // right
	for (int c = x; c <= x1; c++) frame_px(c, y1, 0x1f);  // bottom
	for (int r = y; r <= y1; r++) frame_px(x, r, 0x1f);   // left
}
// FUN_00403deb(x, y, w, h), a sunken frame: it clamps x to 0 and keeps the width. While a moved
// scroll or box is drawn (g_drawingLeft) a frame starting left of 0 is drawn where it belongs.
typedef void (__cdecl *frame_t)(int, int, int, int);
static frame_t o_frame;
static void __cdecl h_frame(int x, int y, int w, int h)
{
	if (g_drawingLeft && x < 1) frame_box(x, y, w, h);
	else o_frame(x, y, w, h);
}
typedef BOOL (__cdecl *play_video_t)(const char *, int, int, int, int);
static play_video_t o_play_video;
static BOOL __cdecl h_play_video(const char *name, int x, int y, int mode, int next)
{
	if (x == 0x27 && y == 0x49 && SCREEN_ID == 0x2b) x -= battle_end_shift(); // the result video, in its frame
	// a message scroll's video, in its moved frame (video_scroll_shift)
	else if (g_videoScrollShift && ((x == 25 && y == 81) || (x == 89 && y == 105) || (x == 0x28 && y == 0x69))) x -= g_videoScrollShift;
	return o_play_video(name, x, y, mode, next);
}

// The tutorial scrolls (Game Objectives, Getting started, ...: message ids 200..218) are shown by
// FUN_00476da9(id), which marks each as seen in 0x4f0298 + id - in memory only, so every start and
// every loaded game showed them all again. [Display] TutorialOnce=1 (default) keeps the ids shown
// in lords2hd.ini ([Tutorial] Seen, a bit per id) and marks those as seen without showing them.
typedef void (__cdecl *tutorial_t)(int);
static tutorial_t o_tutorial;
static void __cdecl h_tutorial(int id)
{
	if (g_tutorialOnce && id >= 200 && id < 232) {
		char v[32];
		GetPrivateProfileStringA("Tutorial", "Seen", "0", v, sizeof v, g_iniPath);
		DWORD seen = strtoul(v, NULL, 16), bit = 1u << (id - 200);
		if (seen & bit) { ((BYTE *)0x004f0298)[id] = 1; return; }
		snprintf(v, sizeof v, "%lx", (unsigned long)(seen | bit));
		WritePrivateProfileStringA("Tutorial", "Seen", v, g_iniPath);
	}
	o_tutorial(id);
}

// Diplomatic letters. The text of the compliment, insult and alliance letters is an editable field
// kept in the options (lords2.inf, 0x53f2b8 / 0x53f380 / 0x53f448 / 0x53f510, 200 characters
// each); the game fills it with its default only when there is no options file, so once a field
// was cleared every later letter was blank. FUN_00402009(text, max, ...) copies a field into the
// editor when the letter opens: an empty field, or one still holding a letter put there by the
// patch, gets a new one at random from these ([Display] Letters=1). Text the player typed stays.
static const char *const kLetterCompliment[] = {
	"My lord, word of your wise rule has reached even our distant halls. Your people speak of you with warmth, and so, I confess, do I.",
	"Your fields are green and your granaries full. Such good husbandry deserves praise, and you shall have mine.",
	"I have heard that your soldiers are the best drilled in all the land. A realm is lucky to have so able a master.",
	"Your generosity to your people is the talk of every tavern from here to the sea. Long may your house prosper.",
	"It is rare to find a ruler both just and bold. I count myself fortunate to call you neighbour.",
	"The bards sing of your victories, my lord, and for once I believe every word of it.",
	"Your castle is the envy of the realm, and your hospitality, I am told, is finer still. My compliments.",
	"Few nobles keep their word in these troubled times. You have kept yours, and I shall not forget it.",
	"Your taxes are fair, your markets busy and your roads safe. A finer example of rule I have yet to see.",
	"May your harvests be plentiful and your enemies few. You have my highest regard, my lord.",
};
static const char *const kLetterInsult[] = {
	"Your castle is a heap of stones that a goat could storm. I have seen pigsties better defended.",
	"They say your soldiers run at the sight of their own shadows. I look forward to testing the rumour.",
	"Your people starve while you feast. Mark me well: one day they will open your gates to me.",
	"I have met turnips with more wit than your counsellors, and more courage than your knights.",
	"Your banner is so faded that my men mistook it for a dishrag. It suits you well.",
	"Enjoy your little county while you can. My army grows, and it is hungry for your land.",
	"Your grandfather was a cattle thief, your father a coward, and you have outdone them both.",
	"I would challenge you to single combat, but I hear you have not left your bedchamber in a year.",
	"The crown will never sit on a head as empty as yours. Go back to counting your sheep.",
	"Your taxes would shame a highwayman. Small wonder your peasants flee to my lands.",
};
static const char *const kLetterAlliance[] = {
	"Dark times are upon the land, my lord. Let us stand together, and let our enemies break upon our shields.",
	"Our borders touch and our foes are the same. An alliance would serve us both. What say you?",
	"Alone we are reeds in the wind; together we are an oak. Join me, and let us share the spoils of peace.",
	"I offer you my friendship and my sword. Accept them, and no rival will dare to march on either of us.",
	"Let there be peace between our houses, and let our armies march as one against those who would take the crown.",
	"A wise ruler chooses his friends before the battle, not after. I choose you, my lord.",
};
static const char *const kLetterEndAlliance[] = {
	"Our paths must part, my lord. I thank you for your loyalty, but our pact has run its course.",
	"Times have changed, and so must our arrangements. From this day our alliance is at an end.",
	"I release you from our pact. Remember our friendship kindly, as I shall, until the crown decides between us.",
	"Our alliance served us both well, but I now walk my own road. Farewell, and may we meet again as friends.",
	"The winds have turned, my lord. Our pact is ended; let each of us now look to his own lands.",
};
static BOOL g_letters = TRUE;
static char g_letterLast[4][200]; // the letter last put into each field
typedef void (__cdecl *edit_text_t)(char *, int, int, int);
static edit_text_t o_edit_text;
static void __cdecl h_edit_text(char *text, int max, int a, int b)
{
	static const DWORD field[4] = { 0x0053f2b8, 0x0053f380, 0x0053f448, 0x0053f510 };
	static const char *const *const sets[4] = { kLetterCompliment, kLetterInsult, kLetterAlliance, kLetterEndAlliance };
	static const int counts[4] = { 10, 10, 6, 5 };
	for (int i = 0; g_letters && i < 4; i++) {
		if ((DWORD)text != field[i]) continue;
		if (text[0] && strcmp(text, g_letterLast[i])) break; // the player's own text
		// dealt like a deck: every letter once, in a new shuffled order each round, and a round
		// never starts with the letter just used
		static int deck[4][16], left[4];
		if (!left[i]) {
			int n = counts[i];
			for (int k = 0; k < n; k++) deck[i][k] = k;
			for (int k = n - 1; k > 0; k--) { int j = rand() % (k + 1), t = deck[i][k]; deck[i][k] = deck[i][j]; deck[i][j] = t; }
			if (n > 1 && !strcmp(sets[i][deck[i][n - 1]], g_letterLast[i])) { int t = deck[i][n - 1]; deck[i][n - 1] = deck[i][0]; deck[i][0] = t; }
			left[i] = n;
		}
		const char *pick = sets[i][deck[i][--left[i]]];
		lstrcpynA(text, pick, min(max, 200));
		lstrcpynA(g_letterLast[i], text, sizeof g_letterLast[i]);
		break;
	}
	o_edit_text(text, max, a, b);
}

// The army panel: while one of the player's armies is selected (screen 16) a narrow scroll stands
// on the bottom of the map, left of the side panel's buttons, with the army's name, its troops one
// kind a row and its total men, and the < > buttons under them. The game draws nothing there of its
// own, so like the yes/no box (yesno_keep) it is drawn with the game's routines after every game
// frame in which anything changed its pixels, and the HD art and lettering apply to it as to the
// rest. Army fields: map object 0x52f0b0 + id*0x1a4 (as the army's info box, FUN_0041b19d).
static int g_apStyle = 1; // the frame (FUN_00409934's kinds): 0 framed panel, 1 scroll, 2 ...
static int panel_army(void)
{
	if (!g_armyPanel || *(BYTE *)0x004eac50 != 16 || *(int *)0x0052af94 != 3 || *(int *)0x0057c8fc != 0 || *(int *)0x0057cb18 == 2) return 0;
	int id = *(int *)0x0057c8c4;
	if (id <= 0 || id > 150) return 0;
	const BYTE *o = (const BYTE *)0x0052f0b0 + id * 0x1a4;
	return o[8] == 1 && (signed char)o[0] == *(int *)0x0057c8cc ? id : 0;
}
// string idx of the game's text table (as FUN_00402d37 finds it)
static const char *game_str(int table, int idx)
{
	BYTE *p = (BYTE *)0x00591580 + ((int (__cdecl *)(int))0x0040187a)(table);
	if (p > (BYTE *)0x005aea40) return "";
	for (; idx > 0; p++)
		if (!*p && (p[-1] > 0x1f || !p[-1])) idx--;
	while (*p && *p < 0x20) p++;
	return (const char *)p;
}
static BYTE g_apSnap[AP_W * AP_H], g_apUnder[AP_W * AP_H];
static int g_apSig; // what is drawn (2 = draw again)
static BOOL g_apDrawn;
static BOOL ap_region(int save) // 0 compare, 1 save, 2 keep what is under, 3 put it back
{
	if (!g_back) return FALSE;
	for (int r = 0; r < AP_H; r++) {
		BYTE *p = g_back->bits + (AP_Y + r) * g_back->pitch + g_back->org + AP_X;
		if (save == 1) memcpy(g_apSnap + r * AP_W, p, AP_W);
		else if (save == 2) memcpy(g_apUnder + r * AP_W, p, AP_W);
		else if (save == 3) memcpy(p, g_apUnder + r * AP_W, AP_W);
		else if (memcmp(g_apSnap + r * AP_W, p, AP_W)) return FALSE;
	}
	return TRUE;
}
static void army_panel_draw(int id)
{
	typedef void (__cdecl *sprite_t)(int, int, int, int);
	typedef void (__cdecl *text_t)(const char *, int, int, void *, int);
	typedef void (__cdecl *ctext_t)(const char *, int, int, int, void *, int);
	typedef void (__cdecl *num_t)(int, char, const char *, int, int, void *, int);
	sprite_t icon = (sprite_t)0x0040a21a;
	text_t text = (text_t)0x00402637;
	ctext_t ctext = (ctext_t)0x004025d7;
	num_t num = (num_t)0x00402f64;
	void *big = (void *)0x005af8f0, *small = (void *)0x005c9a90;
	int *adv = (int *)0x005cd404; // width of the text just drawn
	const BYTE *o = (const BYTE *)0x0052f0b0 + id * 0x1a4;
	int sheet = *(int *)0x005530c8;
	static const DWORD frame[3] = { 0x00409397, 0x004093e0, 0x00409429 }; // framed panel, scroll, ...
	((void (__cdecl *)(int, int, int, int))frame[g_apStyle % 3])(AP_X, AP_Y, AP_W / 16, AP_H / 16);
	// the name under the scroll's roll; too wide ("The Panthers." in quotes), it loses its quotes
	// and full stop, and then takes the small lettering
	int (__cdecl *width)(const char *, void *) = (void *)0x004014f0;
	char name[64];
	lstrcpynA(name, game_str((signed char)o[0] + 0x5d, o[0x14f]), sizeof name);
	void *nf = big;
	if (width(name, big) > AP_W - 12) {
		char *a = name, *e;
		while (*a && (strchr("\"'.", *a) || (BYTE)*a >= 0x80)) a++;
		e = a + lstrlenA(a);
		while (e > a && (strchr("\"'. ", e[-1]) || (BYTE)e[-1] >= 0x80)) e--;
		*e = 0;
		memmove(name, a, e - a + 1);
		if (width(name, big) > AP_W - 12) nf = small;
	}
	ctext(name, AP_X + 2, AP_Y + 18, AP_W - 4, nf, 0x3f);
	for (int t = 0; t < 7; t++) { // a row a kind, mercenaries with their kind
		int n = *(const short *)(o + 0x16c + t * 2) + (o[0x195] == t ? o[0x196] : 0), y = AP_Y + 40 + t * 24;
		icon(sheet, 0x2f + t, AP_X + AP_W / 2 - 30, y);
		num(n, ' ', "", AP_X + AP_W / 2 + 2, y + 4, small, 0x3f);
	}
	// total men between the < > buttons (cycle_btn_rect): the number and the game's "men"
	char s[64], word[32];
	const char *men = game_str(8, 0x49), *w = strrchr(men, ' ');
	lstrcpynA(word, w ? w + 1 : men, sizeof word);
	wsprintfA(s, "%d %s", *(const int *)(o + 0x168), word);
	ctext(s, AP_X + 24, AP_Y + AP_H - 38, AP_W - 48, big, 0x3f); // above the bottom roll
	(void)text; (void)adv;
}
static void __cdecl h_redraw_map(void);
// Before the game shows its back buffer (h_show_frame): the game redraws the map under the panel
// every frame, so the panel is drawn again whenever its pixels are not what it left there.
static void army_panel_keep(void)
{
	if (g_menuOpen) return; // the settings scroll is over everything (menu_scroll_keep)
	int id = g_back ? panel_army() : 0;
	if (!id) {
		if (g_apDrawn) { // gone: what it covered back (the game draws the map on over it)
			g_apDrawn = 0;
			InterlockedExchange(&g_ovl[0].on, 0);
			ap_region(3);
			((void (__cdecl *)(int, int, int, int))0x00452306)(AP_X, AP_Y, AP_X + AP_W - 1, AP_Y + AP_H - 1);
		}
		g_apSig = 0;
		return;
	}
	const BYTE *o = (const BYTE *)0x0052f0b0 + id * 0x1a4;
	int sig = id * 131 + *(const int *)(o + 0x168) * 7 + o[0x196] * 3 + o[0x195];
	for (int t = 0; t < 7; t++) sig = sig * 31 + *(const short *)(o + 0x16c + t * 2);
	sig |= 1;
	BOOL same = g_apDrawn && ap_region(0);
	if (sig == g_apSig && same) return;
	if (!same) ap_region(2); // what the game has there now
	army_panel_draw(id);
	((void (__cdecl *)(int, int, int, int))0x00452306)(AP_X, AP_Y, AP_X + AP_W - 1, AP_Y + AP_H - 1); // to be shown
	ap_region(1);
	g_apDrawn = 1;
	g_ovl[0].x = AP_X; g_ovl[0].y = AP_Y; g_ovl[0].w = AP_W; g_ovl[0].h = AP_H; g_ovl[0].snap = g_apSnap; g_ovl[0].under = g_apUnder;
	InterlockedExchange(&g_ovl[0].gen, sig);
	InterlockedExchange(&g_ovl[0].on, 1);
	g_apSig = sig;
}

// The settings panel (F10) as one of the game's scrolls: drawn with its own scroll frame and
// lettering into its picture, like the army scroll, before every flush that would show the place
// without it. What it covered is kept and put back when it closes.
#define MS_W (24 * 16)
#define MS_H (17 * 16)
static BYTE g_msSnap[MS_W * MS_H], g_msUnder[MS_W * MS_H];
static int g_msX, g_msY, g_msGen = -1;
static BOOL ms_region(BYTE *buf, int op) // 0 compare, 1 save, 2 put back
{
	for (int r = 0; r < MS_H; r++) {
		BYTE *p = g_back->bits + (g_msY + r) * g_back->pitch + g_back->org + g_msX;
		if (op == 1) memcpy(buf + r * MS_W, p, MS_W);
		else if (op == 2) memcpy(p, buf + r * MS_W, MS_W);
		else if (memcmp(buf + r * MS_W, p, MS_W)) return FALSE;
	}
	return TRUE;
}
static void menu_scroll_draw(void)
{
	typedef void (__cdecl *text_t)(const char *, int, int, void *, int);
	typedef void (__cdecl *ctext_t)(const char *, int, int, int, void *, int);
	typedef void (__cdecl *line_t)(int, int, int, int, int);
	text_t text = (text_t)0x00402637;
	ctext_t ctext = (ctext_t)0x004025d7;
	line_t line = (line_t)0x00403a8f;
	int (__cdecl *width)(const char *, void *) = (void *)0x004014f0;
	void *title = (void *)0x005b2fa0, *body = (void *)0x005af8f0, *small = (void *)0x005c9a90;
	const int ink = 0x3f, red = 0xf9; // the game's black and its red (the side panel's changed rations)
	int x0 = g_msX, y0 = g_msY;
	g_ownScroll = TRUE; // its HD art whatever the screen's palette (stamp_sprite)
	// the lettering engraved as on the map: the game's text routine (FUN_00402637) shades it with
	// slots 0x36 / 0x2c on screens 0x1c and 0x1f (the main menu), grey in the map's colours
	BYTE savedScreen = *(BYTE *)0x004eac50;
	if (savedScreen == 0x1c || savedScreen == 0x1f) *(BYTE *)0x004eac50 = 0;
	((void (__cdecl *)(int, int, int, int))0x004093e0)(x0, y0, MS_W / 16, MS_H / 16);
	ctext("Lords of the Realm II HD", x0, y0 + 18, MS_W, title, ink);
	{ // the tabs side by side, spaced by their own widths; the open one red and underlined
		int tw[MENU_TABS], total = 0, gap = 18;
		for (int k = 0; k < MENU_TABS; k++) { tw[k] = width(kMenuTab[k], body); total += tw[k] + (k ? gap : 0); }
		int tx = x0 + (MS_W - total) / 2;
		for (int k = 0; k < MENU_TABS; k++) {
			int w = tw[k], cx = tx + w / 2;
			ctext(kMenuTab[k], cx - w / 2 - 4, y0 + 44, w + 8, body, k == g_menuTab ? red : ink);
			if (k == g_menuTab) { line(cx - w / 2, y0 + 57, cx + w / 2, y0 + 57, red); line(cx - w / 2, y0 + 58, cx + w / 2, y0 + 58, red); }
			SetRect(&g_menuTabRect[k], cx - w / 2 - gap / 2, y0 + 42, cx + w / 2 + gap / 2, y0 + 61);
			tx += w + gap;
		}
	}
	line(x0 + 24, y0 + 64, x0 + MS_W - 24, y0 + 64, ink);
	int n = menu_tab_count(g_menuTab);
	if (g_menuSel >= n) g_menuSel = 0;
	g_menuRows = n;
	for (int i = 0; i < n; i++) {
		int item = kMenuTabItems[g_menuTab][i], y = y0 + 74 + i * 15, c = i == g_menuSel ? red : ink;
		char v[64]; BOOL restart;
		menu_value(item, v, sizeof v, &restart);
		if (restart) strncat(v, " (restart)", sizeof v - strlen(v) - 1);
		if (i == g_menuSel) // a pointer before the chosen line
			for (int k = 0; k < 4; k++) line(x0 + 22 + k, y + 4 - (3 - k), x0 + 22 + k, y + 4 + (3 - k), red);
		text(kMenuLabel[item], x0 + 32, y, small, c); // the small lettering: the body's is too large for a list
		text(v, x0 + 190, y, small, restart ? red : c);
		if (menu_new(item)) text("new", x0 + MS_W - 48, y, small, red);
		SetRect(&g_menuRow[i], x0 + 16, y - 2, x0 + MS_W - 16, y + 13);
	}
	if (g_menuTab == 0) ctext("* after a restart, everything else at once", x0, y0 + 202, MS_W, small, ink); // under the last of 8 rows
	const char *keys = "Click to change, right-click to close";
	ctext(keys, x0, y0 + MS_H - 38, MS_W, (void *)0x005cbfb0, ink); // the keys apart, in the game's fourth lettering (its battle numbers)
	SetRect(&g_menuBox, x0, y0, x0 + MS_W, y0 + MS_H);
	*(BYTE *)0x004eac50 = savedScreen;
	g_ownScroll = FALSE;
}
// before each flush (h_show_frame)
static void menu_scroll_keep(void)
{
	if (!g_back) return;
	void (__cdecl *dirty)(int, int, int, int) = (void *)0x00452306;
	if (!g_menuOpen || !menu_as_scroll()) {
		if (g_msDrawn) { // closed, or the screen changed: what it covered back
			g_msDrawn = 0;
			InterlockedExchange(&g_ovl[1].on, 0);
			ms_region(g_msUnder, 2);
			dirty(g_msX, g_msY, g_msX + MS_W - 1, g_msY + MS_H - 1);
			g_apSig = 2; // the army scroll again
		}
		return;
	}
	if (!g_msDrawn) { // centred on the picture: over the map or battlefield left of the side panel
		BOOL map = county_map_on();
		int left = map && g_off ? -g_off : 0, right = map ? 0x1dc : GAME_W;
		g_msX = max(0, min(GAME_W - MS_W, (left + right) / 2 - MS_W / 2));
		g_msY = (GAME_H - MS_H) / 2;
		if (battle_zoom_on()) { // the middle of the zoomed-out field, as the end-of-battle scroll (battle_end_shift)
			g_msX = PANEL_X - battle_map_w() / 2 - MS_W / 2;
			g_msY = GAME_H - MS_H / 2;
		}
	}
	if (g_msDrawn && g_msGen == g_menuGen && ms_region(g_msSnap, 0)) return;
	if (!g_msDrawn) ms_region(g_msUnder, 1); // what the game had there when it opened (later it may be the scroll itself)
	g_msGen = g_menuGen;
	g_drawingLeft = g_msX < 0; // left of the game's x = 0 (h_clip_x, h_frame)
	g_drawingLow = g_msY + MS_H > GAME_H; // and below row 480 (h_clip_y)
	menu_scroll_draw();
	g_drawingLeft = g_drawingLow = FALSE;
	ms_region(g_msSnap, 1);
	g_msDrawn = 1;
	g_ovl[1].x = g_msX; g_ovl[1].y = g_msY; g_ovl[1].w = MS_W; g_ovl[1].h = MS_H; g_ovl[1].snap = g_msSnap; g_ovl[1].under = g_msUnder;
	g_ovl[1].mapPal = 1; // always in the map's colours, the screen's palette whatever it is (ovl_build_mappal)
	InterlockedExchange(&g_ovl[1].gen, g_menuGen);
	InterlockedExchange(&g_ovl[1].on, 1);
	dirty(g_msX, g_msY, g_msX + MS_W - 1, g_msY + MS_H - 1);
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
	g_btDefer = battle_zoom_on() && g_btLayer && g_btShadow && g_btValid; // the stamps batched (bt_queue)
	o_battle_draw(col, row);
	g_btDefer = FALSE;
	bt_flush();
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
	int r;
	if (begin_wide_view()) { r = o_pick_cell(arg); end_wide_view(); PICKED_COL -= g_view.k; }
	else r = o_pick_cell(arg);
	return r;
}

// A cell's ground: FUN_004063c1(mode) picks the file from the cell's record (the same choice as the
// game's), and the special cells' FUN_0042a7f1 / FUN_0042a8d1 (sea, shore) always use the base file.
typedef void (__cdecl *cell_t)(int);
static cell_t o_cell_ground, o_cell_special, o_cell_special2;
#define TILE_FRAME (*(int *)0x005c9288)
static BOOL g_wideRedraw; // inside a widened map redraw (h_redraw_map)
static void __cdecl h_cell_ground(int mode)
{
	int x = BLOCK_X, y = BLOCK_Y;
	o_cell_ground(mode);
	// After a redraw the game makes the county with the most cells on screen the current one
	// (FUN_004050f6, counts in 0x52f020 added after each mode-0 cell). The spare columns are not
	// counted, so the choice is the 4:3 screen's: they made a neighbouring county current - its
	// name in the panel, its Town Center opened from the clicked county's town.
	if (g_wideRedraw && mode == 0 && x < g_view.firstX) {
		int county = *(BYTE *)0x0057d384;
		((int *)0x0052f020)[county] -= (*(BYTE *)0x0057d374 & 0x40) ? 5 : 1;
	}
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
	// The box is erased by redrawing the cells under it next frame: FUN_0041298a marks them
	// (FUN_0046c4a3) for its height cut at row 472, so in the taller view the box's lower part stayed
	// on the field. The cells under the whole box are marked here too.
	if (zoomed) {
		int size = ((int (__cdecl *)(void))0x004bc505)();
		if (size > 0) {
			int cx = BATTLE_COL + ((int (__cdecl *)(int))0x004bc2a5)(left), cy = BATTLE_ROW + ((int (__cdecl *)(int))0x004bc2c5)(top);
			((void (__cdecl *)(int, int, int, int))0x0046c4a3)(cx, cy, (right - left + 1) / size + 2, (bottom - top + 1) / size + 2);
		}
	}
}

typedef void (__cdecl *cell_32_t)(const BYTE *);
static cell_32_t o_cell_32;
static void __cdecl h_cell_32(const BYTE *buf)
{
	if (g_btValid && battle_zoom_on() && g_back && g_btDefer) { // queued, in order with the stamps (bt_flush)
		bt_queue(NULL, NULL, BLOCK_X + g_back->org, BLOCK_Y, *(short *)0x005c9258, *(short *)0x005aeb70);
		rows_dirty(g_btDirty, BLOCK_Y, BLOCK_Y + *(short *)0x005aeb70);
	}
	else if (g_btValid && battle_zoom_on() && g_back) {
		int x = BLOCK_X + g_back->org, y = BLOCK_Y, w = *(short *)0x005c9258, h = *(short *)0x005aeb70;
		for (int r = max(y, 0); r < min(y + h, g_frameH); r++) {
			int x0 = max(x, 0), x1 = min(x + w, g_frameW);
			if (x1 > x0) memset(g_btValid + r * g_frameW + x0, 0, x1 - x0);
		}
		rows_dirty(g_btDirty, y, y + h);
	}
	int x = BLOCK_X, y = BLOCK_Y, frame = TILE_FRAME;
	double t = g_timing ? perf_now() : 0;
	o_cell_32(buf);
	if (g_timing) ct_add(5, &t);
	if (battle_zoom_on()) stamp_sprite(buf, frame, x, y); // the ground itself
	if (g_timing) ct_add(6, &t);
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
	if (!g_stampCur) {
		g_stampCur = malloc(MAX_STAMPS * sizeof(stamp_call));
		g_stampPrev = malloc(MAX_STAMPS * sizeof(stamp_call));
	}
	BOOL rec = g_stampCur && g_stampPrev;
	if (rec) { g_stampN = 0; g_stampOverflow = FALSE; g_stampRec = TRUE; }
	g_wideRedraw = TRUE;
	o_redraw_map();
	g_wideRedraw = FALSE;
	if (rec) {
		g_stampRec = FALSE;
		BOOL same = !g_stampOverflow && g_stampN == g_stampPrevN && g_layerGen == g_stampPrevGen &&
			memcmp(g_stampCur, g_stampPrev, g_stampN * sizeof(stamp_call)) == 0;
		if (same) g_stampSkipped++;
		else {
			for (int i = 0; i < g_stampN; i++) {
				const stamp_call *s = &g_stampCur[i];
				if (s->sprite) stamp_sprite(s->buf, s->index, s->x, s->y);
				else stamp_tile(s->buf, s->index, s->x, s->y);
			}
			g_stampDone++;
			stamp_call *swap = g_stampPrev; g_stampPrev = g_stampCur; g_stampCur = swap;
			g_stampPrevN = g_stampOverflow ? -1 : g_stampN;
		}
		g_stampPrevGen = g_layerGen;
	}
	end_wide_view();
	if (g_timing) draw_timed(0, t0);
	// The kingdom overview runs out of map columns before the screen's left edge: the strip left
	// is filled with the land 4 columns to its right (the map repeats every column), HD layer too.
	int strip = g_extra - g_view.k * MAP_STEP - g_view.firstX, shift = 4 * MAP_STEP;
	if (MAP_ZOOM == 2 && strip > 0 && strip <= shift) {
		g_layerGen++;
		int x0 = g_back->org - g_extra;
		for (int y = 24; y < bottom; y++) {
			BYTE *row = g_back->bits + y * g_back->pitch + x0;
			memcpy(row, row + shift, strip);
			if (!g_sprLayer) continue;
			memcpy(g_sprShadow + y * MAX_W + x0, g_sprShadow + y * MAX_W + x0 + shift, strip);
			memcpy(g_sprValid + y * MAX_W + x0, g_sprValid + y * MAX_W + x0 + shift, strip);
			g_sprDirty[y] = 1;
			for (int r = 0; r < 2; r++) {
				DWORD *o = g_sprLayer + (2 * y + r) * (2 * MAX_W) + 2 * x0;
				memcpy(o, o + 2 * shift, strip * 2 * sizeof(DWORD));
			}
		}
	}
	// The map as just redrawn, before any window is drawn over it, for window_shift: a window
	// opening over the map often scrolls or recentres it first (the Town Center; a castle near the
	// edge), so the last picture shown before the click is not the map behind the window.
	if ((SCREEN_ID == 0 || window_screen(SCREEN_ID)) && g_showW > 0 && g_showX >= 0 && g_showX + g_showW <= g_back->w) {
		for (int y = 0; y < GAME_H; y++)
			memcpy(g_snapPend8 + y * g_showW, g_back->bits + y * g_back->pitch + g_showX, g_showW);
		g_snapPendW = g_showW;
		if (window_screen(SCREEN_ID)) InterlockedExchange(&g_snapPending, 1);
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
	hook_code((BYTE *)0x0040477b, kBlockPrologue, sizeof kBlockPrologue, h_clip_y, &o_clip_y);
	hook_code((BYTE *)0x00426b92, kBlockPrologue, sizeof kBlockPrologue, h_stream_play, &o_stream_play);
	hook_code((BYTE *)0x004bc020, kBlockPrologue, sizeof kBlockPrologue, h_battle_view, &o_battle_view);
	hook_code((BYTE *)0x0047ed34, kBlockPrologue, sizeof kBlockPrologue, h_battle_clamp, &o_battle_clamp);
	hook_code((BYTE *)0x004bc142, kBlockPrologue, sizeof kBlockPrologue, h_battle_draw, &o_battle_draw);
	hook_code((BYTE *)0x00409e09, kRedrawPrologue, sizeof kRedrawPrologue, h_bar_tiles, &o_bar_tiles);
	hook_code((BYTE *)0x00409397, kBlockPrologue, sizeof kBlockPrologue, h_framed_panel, &o_framed_panel);
	static const BYTE kMsgPrologueA[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 }; // push ebp; mov ebp,esp; sub esp,24h
	static const BYTE kMsgPrologueB[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 }; // push ebp; mov ebp,esp; sub esp,8
	hook_code((BYTE *)0x0047309e, kMsgPrologueA, sizeof kMsgPrologueA, h_msg_a, &o_msg_a);
	hook_code((BYTE *)0x00475e07, kMsgPrologueB, sizeof kMsgPrologueB, h_msg_b, &o_msg_b);
	hook_code((BYTE *)0x00476488, kMsgPrologueB, sizeof kMsgPrologueB, h_msg_c, &o_msg_c);
	hook_code((BYTE *)0x0040e6f2, kBlockPrologue, sizeof kBlockPrologue, h_yesno_open, &o_yesno_open);
	hook_code((BYTE *)0x0040ccfa, kBlockPrologue, sizeof kBlockPrologue, h_yesno_draw, &o_yesno_draw);
	static const BYTE kTutorialPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 }; // push ebp; mov ebp,esp; sub esp,4
	hook_code((BYTE *)0x00476da9, kTutorialPrologue, sizeof kTutorialPrologue, h_tutorial, &o_tutorial);
	hook_code((BYTE *)0x00402009, kTutorialPrologue, sizeof kTutorialPrologue, h_edit_text, &o_edit_text); // same prologue
	srand(GetTickCount());
	static const BYTE kEndPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 }; // push ebp; mov ebp,esp; sub esp,4
	hook_code((BYTE *)0x00423241, kEndPrologue, sizeof kEndPrologue, h_battle_end_draw, &o_battle_end_draw);
	hook_code((BYTE *)0x0042d91b, kBlockPrologue, sizeof kBlockPrologue, h_play_video, &o_play_video);
	hook_code((BYTE *)0x00403deb, kBlockPrologue, sizeof kBlockPrologue, h_frame, &o_frame);
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

// The battle minimap's troops ([Display] MinimapPop): the game draws each as a 2x2 dot of its owner's
// colour (FUN_004bc51a, frames of t2_spri.pl8), and the black player's is mid-grey (index 0x31), the
// grey of rock and walls. Here every troop gets a one-pixel black ring (black troops: true black with a
// white ring), painted into the game's frame only for its copy to the screen and taken out right after,
// so the game's own minimap, which redraws only changed cells, never sees it (no trails).
static BYTE g_miniSave[200 * 200];
static int g_miniX, g_miniY, g_miniW, g_miniH; // the saved rectangle (game pixels); g_miniW 0: none
static void minimap_dots_paint(void)
{
	g_miniW = 0;
	if (!g_miniPop || !IN_BATTLE_NOW() || *(int *)0x004e5d58 != 2) return;
	BYTE *fb = *(BYTE **)0x004ea1a8, *cells = *(BYTE **)0x004e6574, *spr = *(BYTE **)0x004e5d40;
	int pitch = *(int *)0x004eb274, stride = *(int *)0x004e5d50, cw = *(int *)0x004e6578, ch = *(int *)0x004e6570;
	int ox = *(int *)0x004e5d68, oy = *(int *)0x004e5d60;
	if (!fb || !cells || !spr || pitch <= 0 || stride <= 0 || cw <= 0 || ch <= 0 || cw > 96 || ch > 96 || ox < 0 || oy < 0) return;
	int w = 2 * cw, h = 2 * ch;
	if (ox + w > pitch || oy + h > GAME_H) return;
	for (int y = 0; y < h; y++) memcpy(g_miniSave + y * w, fb + (oy + y) * pitch + ox, w);
	g_miniX = ox; g_miniY = oy; g_miniW = w; g_miniH = h;
	for (int pass = 0; pass < 2; pass++) // rings first, then the dots, so a formation gets one outline
		for (int r = 0; r < ch; r++) for (int c = 0; c < cw; c++) {
			BYTE unit = cells[(r * cw + c) * stride + 5];
			if (!unit) continue;
			BYTE owner = *(BYTE *)(0x005544ac + unit * 0x1b0);
			int frame = owner == 6 ? 6 : *(BYTE *)(0x0057bf0a + owner * 0x160); // as FUN_004bc51a
			if (frame <= 0 || frame > 6) continue;
			BYTE col = spr[*(int *)(spr + frame * 16 + 12)], ring = 0x3f; // the frame's colour; black ring
			if (col == 0x31) { col = 0x3f; ring = 0x20; } // the black player: black, white ring
			int x0 = 2 * c, y0 = 2 * r, e = pass ? 0 : 1;
			for (int y = y0 - e; y <= y0 + 1 + e; y++) for (int x = x0 - e; x <= x0 + 1 + e; x++) {
				if (x < 0 || x >= w || y < 0 || y >= h) continue;
				BOOL hidden = FALSE; // under the patch's scrolls (settings, army): those must stay as drawn
				for (int k = 0; k < 2 && !hidden; k++) {
					const SCROLL_OVL *s = &g_ovl[k];
					hidden = s->on && ox + x >= s->x && ox + x < s->x + s->w && oy + y >= s->y && oy + y < s->y + s->h;
				}
				if (!hidden) fb[(oy + y) * pitch + ox + x] = pass ? col : ring;
			}
		}
}
static void minimap_dots_restore(void)
{
	if (!g_miniW) return;
	BYTE *fb = *(BYTE **)0x004ea1a8; int pitch = *(int *)0x004eb274;
	if (fb) for (int y = 0; y < g_miniH; y++) memcpy(fb + (g_miniY + y) * pitch + g_miniX, g_miniSave + y * g_miniW, g_miniW);
	g_miniW = 0;
}
static void show_frame_dots(int force) { minimap_dots_paint(); o_show_frame(force); minimap_dots_restore(); }

static void __cdecl h_show_frame(int force)
{
	if (!*(void **)0x004eabd0) return;
	stream_hold_tick();
	army_panel_keep();
	menu_scroll_keep();
	// Old-game limit: unforced copies (the battle loop's, FUN_004b0000's and FUN_00430000's) go to the
	// screen only once 50 ms have passed since the last (DAT_004eabe8), so battles showed at most 20
	// frames a second while the game drew ~80. With the frame limit on, every frame goes: the copy is
	// a memcpy here and the presenter shows at most one picture per refresh.
	// Forced only once the presenter has taken the last copy (g_frameTaken): the game draws ~140 frames
	// a second, the screen shows 60, and each copy carries the HD layers' changed rows (a scroll
	// changes them all), so copies nobody would see only slowed the game and held the presenter up.
	if (g_timing && !IN_BATTLE_NOW() && g_back) { // elsewhere: the change count for the pacing check only
		static DWORD lastHashM;
		DWORD h = 2166136261u;
		const DWORD *p = (const DWORD *)g_back->bits; int words = g_back->pitch * g_frameH / 4;
		for (int i = 0; i < words; i++) h = (h ^ p[i]) * 16777619u;
		if (h != lastHashM && *(int *)0x0050d09c > 0) pace_change(); // (only what the game has marked for the screen)
		lastHashM = h;
	}
	if (g_timing && IN_BATTLE_NOW() && g_back) { // what makes a battle picture change: steps, animation ticks, scrolls
		static DWORD lastHash, lastAnim, lastScroll, t0; static int n[3][2]; // [0 nothing, 1 step, 2 tick or scroll only][changed]
		static int lastCol, lastRow, moves; static LONG lastPresents;
		if (BATTLE_COL != lastCol || BATTLE_ROW != lastRow) { moves++; lastCol = BATTLE_COL; lastRow = BATTLE_ROW; }
		DWORD h = 2166136261u;
		const DWORD *p = (const DWORD *)g_back->bits; int words = g_back->pitch * g_frameH / 4;
		for (int i = 0; i < words; i++) h = (h ^ p[i]) * 16777619u; // every word: a change of one pixel counts
		if (h != lastHash && *(int *)0x0050d09c > 0) { pace_change(); pace_event('H', g_paceChangeSeq); } // (changes the game has not marked for the screen are never shown, even by the original)
		DWORD anim = *(DWORD *)0x004e5b10, scroll = *(DWORD *)0x004e5b14;
		int kind = g_frameSteps ? 1 : (anim != lastAnim || scroll != lastScroll) ? 2 : 0;
		n[kind][h != lastHash]++;
		lastHash = h; lastAnim = anim; lastScroll = scroll; g_frameSteps = 0;
		DWORD now = GetTickCount();
		if (!t0) t0 = now;
		if (now - t0 >= 1000) { // per second: changed game frames, pictures shown, view moves (a scroll step)
			LONG pr = g_presentCount;
			logf_("timing: battle 1 s: %d changed frames of %d (steps %d/%d, tick or scroll %d/%d, neither %d/%d), %ld shown, %d view moves",
				n[0][1] + n[1][1] + n[2][1], n[0][0] + n[0][1] + n[1][0] + n[1][1] + n[2][0] + n[2][1],
				n[1][1], n[1][0] + n[1][1], n[2][1], n[2][0] + n[2][1], n[0][1], n[0][0] + n[0][1], (long)(pr - lastPresents), moves);
			memset(n, 0, sizeof n); t0 = now; moves = 0; lastPresents = pr;
		}
	}
	// The presenter is building a picture from this frame's HD layers: an unforced copy now would only
	// wait for it (g_composeLock), and nobody would see that frame; the next call copies.
	static int sfCalls, sfCompSkip, sfOurs, sfOursNoCopy, sfOther, sfOtherCopy; static double sfT; // Timing=1: why copies did or did not happen
	if (g_timing) {
		sfCalls++;
		if (perf_now() - sfT > 1000) {
			if (IN_BATTLE_NOW()) logf_("copies 1 s: %d calls, %d skipped (composing), %d with the presenter's turn (%d copied nothing), %d without (%d copied), %d in-wait copies",
				sfCalls, sfCompSkip, sfOurs, sfOursNoCopy, sfOther, sfOtherCopy, g_pendingCopies);
			sfCalls = sfCompSkip = sfOurs = sfOursNoCopy = sfOther = sfOtherCopy = 0; g_pendingCopies = 0; sfT = perf_now();
		}
	}
	if (!force && g_composing) { g_copyPending = 1; sfCompSkip++; pace_event('K', g_paceChangeSeq); return; }
	BOOL ours = FALSE;
	LONG hadTurn = 0;
	if (g_frameLimit > 0 && !force) { hadTurn = InterlockedExchange(&g_frameTaken, 0); if (hadTurn || g_preTurn) force = ours = 1; } // (g_preTurn: see there)
	LONG seq = g_copySeq;
	g_paceCopying = g_paceChangeSeq; // the change this copy carries (copy_rect: g_pacePrimarySeq)
	if (g_timing) {
		static double sum, worst, t0; static int n;
		double a = perf_now();
		show_frame_dots(force);
		double b = perf_now(), ms = b - a;
		sum += ms; n++; if (ms > worst) worst = ms;
		if (!t0) t0 = b;
		if (b - t0 > 5000) { logf_("timing: %d frame copies, %.2f ms average, %.1f ms worst (game thread)", n, sum / n, worst); sum = worst = 0; n = 0; t0 = b; }
	}
	else show_frame_dots(force);
	// nothing was copied (nothing changed, or the game's own gate): the presenter's turn is kept
	if (ours && g_copySeq == seq && hadTurn) InterlockedExchange(&g_frameTaken, 1);
	if (ours) { sfOurs++; if (g_copySeq == seq) sfOursNoCopy++; } else { sfOther++; if (g_copySeq != seq) sfOtherCopy++; }
	if (g_timing) {
		pace_event(g_copySeq != seq ? 'C' : ours ? 'c' : 'N', g_paceChangeSeq);
		static int gateLogs;
		if (ours && g_copySeq == seq && *(int *)0x0050d09c > 0 && gateLogs < 30) { // marked for the screen, our turn, and still no copy: the copy routine's own gates
			gateLogs++;
			logf_("copy refused: marks %d, 4df28c %d, 56d650 %d, 553008 %d, eabe8 %d, 5651ac %d", *(int *)0x0050d09c, *(int *)0x004df28c, *(int *)0x0056d650, *(int *)0x00553008, *(int *)0x004eabe8, *(int *)0x005651ac);
		}
	}
	g_copyPending = g_copySeq == seq && g_frameLimit > 0; // a finished frame not on the screen yet (pending_copy)
	if (g_copySeq != seq) g_frameCopyT = perf_now();
}
// From the frame limiter's wait (h_game_frame): the last finished frame copied once the presenter
// is ready for it. The game is between frames there, so its back buffer is complete.
static void pending_copy(void)
{
	if (!g_copyPending || !(g_frameTaken || g_preTurn) || g_composing || !*(void **)0x004eabd0) return;
	LONG hadTurn = InterlockedExchange(&g_frameTaken, 0);
	if (!hadTurn && !g_preTurn) return;
	g_copyPending = 0;
	LONG seq = g_copySeq;
	g_paceCopying = g_paceChangeSeq;
	show_frame_dots(1);
	if (g_copySeq == seq) { if (hadTurn) InterlockedExchange(&g_frameTaken, 1); } // (nothing to copy after all)
	else { g_frameCopyT = perf_now(); g_pendingCopies++; pace_event('P', g_paceChangeSeq); }
}

// Old-game habit: when no message is waiting, the main loop (FUN_0040e9ab) runs the next game frame
// (FUN_0040e8bb) at once, so the game redraws its map thousands of times a second and keeps a CPU
// core busy. Each frame is spaced to at least 1/FrameLimit s here: the screen is shown at most once
// per display refresh anyway, and the game times its animations and scrolling by the clock (the
// waits below keep those clocks exact).
typedef char (__cdecl *game_frame_t)(void);
static game_frame_t o_game_frame;
static char __cdecl h_game_frame(void)
{
	static LARGE_INTEGER next, freq;
	static BOOL fine;
	if (g_timing) { // how often each gate below opened, to compare with FrameLimit=0
		static const DWORD addr[5] = { 0x004e5b0c, 0x004e5b14, 0x004e5b10, 0x004e59f0, 0x004e59ec };
		static DWORD prev[5], t0; static int opened[5];
		DWORD t = GetTickCount();
		if (!t0) t0 = t;
		for (int i = 0; i < 5; i++) { DWORD v = *(DWORD *)addr[i]; if (v != prev[i]) { opened[i]++; prev[i] = v; } }
		if (t - t0 >= 5000) {
			logf_("timing: gates opened in 5 s: steps %d, scroll %d, animation %d, screen 35 %d, elapsed %d",
				opened[0], opened[1], opened[2], opened[3], opened[4]);
			memset(opened, 0, sizeof opened);
			t0 = t;
		}
	}
	if (g_frameLimit > 0) {
		if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
		if (!fine) { timeBeginPeriod(1); fine = TRUE; } // Sleep to the millisecond
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		LONGLONG period = freq.QuadPart / g_frameLimit;
		// The game does its timed work behind gates of one kind: act once N ms have passed since the
		// last time, then count again from the frame that acted. A frame that comes late makes every
		// later one late too (battles ran at 60 logic steps a second instead of 82, scrolling 20%
		// slower), so the wait also ends the moment the next gate opens:
		//   logic steps (FUN_004bb978): 10 * (10 - game speed / 10) + 2 ms, last time DAT_004e5b0c
		//   map scrolling (FUN_004bbbe3): 12 * (10 - scroll speed / 10) + 2 ms (2 more steps on
		//     screen 16), DAT_004e5b14
		//   animation tick (FUN_004bbc80) and screen 35's effect (FUN_004b0e03): 20 ms, DAT_004e5b10 /
		//     DAT_004e59f0
		//   elapsed time for the frame (FUN_004b20ed): 30 ms, DAT_004e59ec
		// A gate long overdue isn't in use (paused, no scrolling, another screen): it is left out.
		int periodMs = 1000 / g_frameLimit;
		DWORD gateLast[5] = { *(DWORD *)0x004e5b0c, *(DWORD *)0x004e5b14, *(DWORD *)0x004e5b10, *(DWORD *)0x004e59f0, *(DWORD *)0x004e59ec };
		int gateWait[5] = { -1, -1, 20, 20, 30 };
		int slow = (100 - *(int *)0x0053f230) / 10;
		// At speed 10 in a multiplayer game the stock pacer skips its 2 ms gate; a multiplayer fix
		// that patches that skip out (jmp at 0x4bbb92, Wolfe's dplayx) puts the gate back.
		BOOL fastGated = *(int *)0x00553030 == 0 || *(BYTE *)0x004bbb92 == 0xE9;
		if (slow >= 0 && slow < 10 && (slow != 0 || fastGated)) gateWait[0] = slow * 10 + 2;
		int scroll = (100 - *(int *)0x0053f234) / 10;
		if (scroll >= 0 && scroll < 10) gateWait[1] = (scroll + (SCREEN_ID == 0x10 ? 2 : 0)) * 12 + 2;
		DWORD t = timeGetTime();
		for (int i = 0; i < 5; i++)
			if (gateWait[i] >= 0 && (int)(t - gateLast[i]) > 2 * gateWait[i] + periodMs + 2) gateWait[i] = -1;
		// A frame slot that comes less than a frame period before a gate opens would only show the
		// same picture again: then the wait runs to the gate (in battle and on the map frames come
		// with the logic steps, about 82 a second; 120 where nothing is timed).
		if (next.QuadPart) {
			for (;;) {
				QueryPerformanceCounter(&now);
				t = timeGetTime();
				int stepIn = INT_MAX;
				for (int i = 0; i < 5; i++)
					if (gateWait[i] >= 0) stepIn = min(stepIn, (int)(gateLast[i] + gateWait[i] - t));
				if (stepIn <= 0) break;
				int frameIn = (int)((next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart);
				if (stepIn != INT_MAX && stepIn - frameIn < periodMs) frameIn = INT_MAX; // the step is close: wait for it
				else if (now.QuadPart >= next.QuadPart) break;
				int ms = min(stepIn, frameIn) - 1;
				// In slices of 2 ms: a finished frame the presenter has not had (it was busy when the
				// frame was made) goes to the screen as soon as the presenter is ready, not when the
				// next logic step wakes the game (up to 12 ms later, one refresh late).
				// (a finished frame waiting: woken by the presenter's turn, g_turnEvent)
				if (g_copyPending && g_turnEvent && !g_composing) { if (!g_frameTaken && !g_preTurn) WaitForSingleObject(g_turnEvent, ms > 0 ? ms : 0); }
				else Sleep(ms > 2 ? 2 : ms > 0 ? ms : 0);
				pending_copy();
			}
		}
		// the next slot is a frame period after this frame, whatever woke it
		next.QuadPart = now.QuadPart + period;
	}
	char r = o_game_frame();
	yesno_keep();
	return r;
}

// Timing=1: how many logic steps the game runs a second (FUN_004bb978 works them out from the
// clock each frame), to check the frame limit leaves the game's own speed alone.
typedef int (__cdecl *logic_steps_t)(void);
static logic_steps_t o_logic_steps;
static int __cdecl h_logic_steps(void)
{
	// Old-game habit: "Exit the game?" (screen 30) does not pause a battle (its entry in the
	// pause table 0x4e31b0 is 0): the armies fight on behind the question. No steps while it is up.
	// Not in a multiplayer game (DAT_00553030): there a player who stops stepping stalls everyone.
	if (*(BYTE *)0x004eac50 == 0x1e && *(int *)0x0057c8fc == 2 && !*(int *)0x00553030) return 0;
	int n = o_logic_steps();
	g_frameSteps += n > 0 ? n : 0;
	if (!g_timing) return n;
	static DWORD t0; static int steps, frames;
	DWORD now = GetTickCount();
	if (!t0) t0 = now;
	steps += n > 0 ? n : 0; frames++;
	if (now - t0 >= 5000) {
		logf_("timing: %d logic steps in %.1f s over %d frames (battle %d, screen %d)", steps, (now - t0) / 1000.0, frames,
			*(int *)0x0057c8fc == 2, *(BYTE *)0x004eac50);
		t0 = now; steps = frames = 0;
	}
	return n;
}

// ---------------------------------------------------------------------------------------------
// Multiplayer safe mode: the game fixes below change what happens in the game, and nothing checks that
// every player runs them, so in a multiplayer game (DAT_00553030) they stay off unless every player
// has [Game] MultiplayerFixes=1; a player with the original game, or other settings, would drift apart.
static BOOL g_mpFixes;
static BOOL fix_on(BOOL fix) { return fix && (g_mpFixes || !*(int *)0x00553030); }

// Mop-Up fix. Mop-Up (FUN_0047a76d) marks each unit's group as hunting (DAT_0056654a, the flag the
// AI's charge FUN_0048c8af also sets) and puts the unit in the chase state 8. The chase handler
// FUN_00483e55 looks for a new enemy only when the unit has no target, and only afterwards checks
// whether the target is dying (state 2); a unit whose target falls drops it and goes idle (state 5),
// so after its first kill it stands still. For a hunting group the dying target is let go first, so
// the handler's own search (FUN_004954dd: nearest live, non-siege enemy) picks the next one.
// Figures: 80 slots of 0x1b0 bytes from index 1; side at 0x005544ac (0 = empty slot), state at
// 0x005544b1, target slot at 0x005545f4, group at 0x005545f8; a group's hunting flag is
// 0x0056654a + group * 0x34.
#define FIG_B(i, addr) (*(BYTE *)((addr) + (i) * 0x1b0))
#define FIG_CUR        (*(int *)0x0053e8f8)
static BOOL g_fixMopUp;
typedef void (__cdecl *chase_t)(void);
static chase_t o_chase;

static void __cdecl h_chase(void)
{
	int me = FIG_CUR;
	if (fix_on(g_fixMopUp) && me > 0 && me < 0x51) {
		int group = *(short *)(0x005545f8 + me * 0x1b0);
		int target = FIG_B(me, 0x005545f4);
		if (group >= 0 && *(BYTE *)(0x0056654a + group * 0x34) && target > 0 && target < 0x51 &&
			FIG_B(target, 0x005544ac) && FIG_B(target, 0x005544b1) == 2) {
			FIG_B(me, 0x005545f4) = 0;
			static int logged;
			if (logged < 5) logged++, logf_("Mop-Up fix: unit %d (side %d) let go of fallen target %d", me,
				FIG_B(me, 0x005544ac), target);
		}
	}
	o_chase();
}

// AI castle cap. A computer lord's building step (FUN_0049edc7(player)) starts a castle
// (FUN_00436d02(county, kind)) in each of its counties that has none, is big enough and while
// its count of castles under construction (player record +0x4d, 0x0057bf4d + player * 0x160) is
// below its difficulty's limit. That count is only rebuilt at the end of the season (FUN_004508de),
// so a start doesn't raise it and one pass starts a castle in every eligible county at once (the
// Bishop's rash of royal castles). Each start made by that step now adds one to the count.
static BOOL g_fixCastleCap;
static int g_aiBuilding;   // the player whose building step is running, 0 = none
typedef void (__cdecl *ai_build_t)(unsigned);
typedef void (__cdecl *castle_start_t)(int, int);
static ai_build_t o_ai_build;
static castle_start_t o_castle_start;

static int g_aiStarts;

static void __cdecl h_ai_build(unsigned player)
{
	int before = player < 6 ? *(BYTE *)(0x0057bf4d + player * 0x160) : 0;
	g_aiBuilding = player;
	g_aiStarts = 0;
	o_ai_build(player);
	g_aiBuilding = 0;
	static int logged;
	if (g_aiStarts && logged < 10) {
		int level = *(BYTE *)(0x0057bf07 + player * 0x160); // the limit table's row (1 and up)
		logged++, logf_("AI player %u started %d castle(s); %d were building, limit %d", player, g_aiStarts, before,
			level >= 1 ? *(int *)(0x004d8ae8 + (level * 3 - 3) * 0x50) : -1);
	}
}

static void __cdecl h_castle_start(int county, int kind)
{
	o_castle_start(county, kind);
	if (g_aiBuilding > 0 && g_aiBuilding < 6) {
		g_aiStarts++;
		BYTE *building = (BYTE *)(0x0057bf4d + g_aiBuilding * 0x160);
		if (fix_on(g_fixCastleCap) && *building < 255) ++*building;
	}
}

// Wheat fix. Each spring a county plants one grain pile (0x0053fbf4 + county * 0x300) and notes its
// wheat fields twice: 0x0053fbb2 (kept as planted) and 0x0053fbb6 (kept in step with the pile).
// Whenever a wheat field is lost (FUN_00469e5b, FUN_0046965a) the game takes that field's share
// out of the pile and lowers 0x0053fbb6. Tending and harvesting (FUN_0044d15a, FUN_0044d1e5) then
// scale the pile by FUN_0044d281: wheat fields now (0x0053fbb1) over 0x0053fbb2, as planted, so a
// lost field is charged twice, and painting one more wheat field brings back grain it never
// grew. Measured against 0x0053fbb6 instead, only a field taken without its share is counted.
static BOOL g_fixWheat;
typedef int (__cdecl *wheat_scale_t)(int, int);
static wheat_scale_t o_wheat_scale;

static int __cdecl h_wheat_scale(int county, int grain)
{
	if (!fix_on(g_fixWheat)) return o_wheat_scale(county, grain);
	BYTE now = *(BYTE *)(0x0053fbb1 + county * 0x300), live = *(BYTE *)(0x0053fbb6 + county * 0x300);
	int asWas = o_wheat_scale(county, grain);
	if (now < live) grain = grain * (now * 100 / live) / 100; // the game's own percent steps (FUN_00404dc1, FUN_00404d6b)
	static int logged, lastCounty, lastGrain;
	if (grain != asWas && logged < 5 && (county != lastCounty || grain != lastGrain))
		logged++, lastCounty = county, lastGrain = grain, logf_("wheat fix: county %d, %d wheat fields (%d as planted, %d with grain): %d grain counted, %d before",
		county, now, *(BYTE *)(0x0053fbb2 + county * 0x300), live, grain, asWas);
	return grain;
}

// Besieged castles. The game's rule (Readme, "Besieged Castles"): no army may enter a castle or
// strengthen its garrison until the siege is lifted. The human order checks it (FUN_004a9372 refuses
// when the garrison army's besieger link 0x0052f24a is set), but the arrival itself, FUN_004a79a3
// (army, county), which every army stepping onto its own castle goes through, only checks room. So
// a computer lord's reinforcements (FUN_0049f12f, FUN_004a6270, FUN_004a64ca) merged into a
// besieged garrison. Now the arrival is turned away there as when the castle is full.
static BOOL g_fixSiegeGarrison;
typedef void (__cdecl *castle_enter_t)(unsigned, int);
static castle_enter_t o_castle_enter;

static BOOL county_besieged(int county)
{
	int garrison = *(int *)(0x0053fb6c + county * 0x300);
	return garrison > 0 && garrison < 0x97 && *(BYTE *)(0x0052f24a + garrison * 0x1a4) != 0;
}

// The computer's garrison steps (reinforce FUN_0049f12f, pick a castle FUN_004a07f2, march there
// FUN_004a6270) skip a county whose 0x0053fb72 is set, a flag the game never sets. For their length it
// is set on besieged counties, so they neither raise troops for a castle they can't enter (with
// room that never fills, a new army every turn) nor march there.
static int g_inUpgrade; // the season's castle step (FUN_004508de): a finished upgrade's garrison is raised inside
typedef int (__cdecl *county_step_t)(unsigned, int);
static county_step_t o_ai_reinforce, o_ai_pick_castle, o_ai_march;
typedef void (__cdecl *season_castles_t)(void);
static season_castles_t o_season_castles;

static int siege_marks(BYTE *saved)
{
	int n = *(int *)0x0056d5dc, marked = 0;
	for (int c = 1; c <= n && c < 64; c++) {
		saved[c] = *(BYTE *)(0x0053fb72 + c * 0x300);
		if (county_besieged(c)) *(BYTE *)(0x0053fb72 + c * 0x300) = 1, marked++;
	}
	return marked;
}
static void siege_unmark(const BYTE *saved)
{
	int n = *(int *)0x0056d5dc;
	for (int c = 1; c <= n && c < 64; c++) *(BYTE *)(0x0053fb72 + c * 0x300) = saved[c];
}
static int marked_call(county_step_t f, unsigned a, int b)
{
	if (!fix_on(g_fixSiegeGarrison)) return f(a, b);
	BYTE saved[64];
	int marked = siege_marks(saved), r = f(a, b);
	if (marked) siege_unmark(saved);
	return r;
}
static int __cdecl h_ai_reinforce(unsigned player, int unused) { return marked_call(o_ai_reinforce, player, unused); }
static int __cdecl h_ai_pick_castle(unsigned player, int army) { return marked_call(o_ai_pick_castle, player, army); }
static int __cdecl h_ai_march(unsigned army, int unused) { return marked_call(o_ai_march, army, unused); }
static void __cdecl h_season_castles(void) { g_inUpgrade++; o_season_castles(); g_inUpgrade--; }

static void __cdecl h_castle_enter(unsigned army, int county)
{
	if (fix_on(g_fixSiegeGarrison) && !g_inUpgrade && army > 0 && army < 0x97 && county > 0) {
		if (county_besieged(county) && (unsigned)*(int *)(0x0053fb6c + county * 0x300) != army) {
			*(BYTE *)(0x0052f200 + army * 0x1a4) = 1;   // as the full-castle branch leaves it
			*(BYTE *)(0x0052f0ca + army * 0x1a4) = 2;
			*(BYTE *)(0x0052f1fc + army * 0x1a4) = 0;
			static int logged;
			if (logged < 5) logged++, logf_("besieged castle: army %u kept out of county %d's castle", army, county);
			return;
		}
	}
	o_castle_enter(army, county);
}

// Multiplayer checks ([Debug] SyncLog=1): three seconds after each change of season (DAT_0057c92c,
// DAT_0057c934) the log gets a checksum of the county records, the army table and the player records,
// so two players' logs can be compared line by line; the first line that differs is where they
// drifted apart.
static DWORD crc_block(DWORD h, const BYTE *p, size_t n)
{
	for (size_t i = 0; i < n; i++) { h ^= p[i]; for (int k = 0; k < 8; k++) h = (h >> 1) ^ (0xEDB88320u & (0u - (h & 1))); }
	return h;
}
// Multiplayer session logs ([Debug] MpLogs=1, the default): each multiplayer game (DAT_00553030 set)
// gets a folder lords2hd-mp\<date and time>\ that a player can zip and post when something goes wrong:
//   session.txt  versions (this DLL, the exe, a replacement dplayx.dll) and the settings that matter
//   sync.txt     per-season checksums of the county, army and player records (compare two players')
//   copies of lords2hd.log and, if present, dplayx.log and desync.txt, refreshed each season and at the end
// Only the ten newest folders are kept.
static BOOL g_mpLogs = TRUE, g_syncLog;
static DWORD crc_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	BYTE buf[65536]; size_t n; DWORD h = ~0u;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = crc_block(h, buf, n);
	fclose(f);
	return ~h;
}
static void mp_copy_logs(const char *dir)
{
	static const char *names[] = { "lords2hd.log", "dplayx.log", "desync.txt" };
	char from[MAX_PATH], to[MAX_PATH];
	for (int i = 0; i < 3; i++) {
		snprintf(from, sizeof from, "%s\\%s", g_gameDir, names[i]);
		snprintf(to, sizeof to, "%s\\%s", dir, names[i]);
		if (GetFileAttributesA(from) != INVALID_FILE_ATTRIBUTES) CopyFileA(from, to, FALSE);
	}
}
static void mp_prune(const char *root)
{
	char pat[MAX_PATH], names[64][MAX_PATH]; int n = 0;
	snprintf(pat, sizeof pat, "%s\\*", root);
	WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && n < 64) lstrcpynA(names[n++], fd.cFileName, MAX_PATH); } while (FindNextFileA(h, &fd));
	FindClose(h);
	while (n > 10) { // names are dates: the smallest is the oldest
		int old = 0;
		for (int i = 1; i < n; i++) if (strcmp(names[i], names[old]) < 0) old = i;
		char dir[MAX_PATH], file[MAX_PATH];
		snprintf(dir, sizeof dir, "%s\\%s", root, names[old]);
		snprintf(pat, sizeof pat, "%s\\*", dir);
		h = FindFirstFileA(pat, &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { snprintf(file, sizeof file, "%s\\%s", dir, fd.cFileName); DeleteFileA(file); } } while (FindNextFileA(h, &fd));
			FindClose(h);
		}
		RemoveDirectoryA(dir);
		lstrcpynA(names[old], names[--n], MAX_PATH);
	}
}
static void mp_session_start(char *dir, size_t size)
{
	char root[MAX_PATH], path[MAX_PATH];
	snprintf(root, sizeof root, "%s\\lords2hd-mp", g_gameDir);
	CreateDirectoryA(root, NULL);
	SYSTEMTIME t; GetLocalTime(&t);
	snprintf(dir, size, "%s\\%04d-%02d-%02d_%02d-%02d-%02d", root, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	CreateDirectoryA(dir, NULL);
	mp_prune(root);
	snprintf(path, sizeof path, "%s\\session.txt", dir);
	FILE *f = fopen(path, "w");
	if (!f) return;
	char me[MAX_PATH], exe[MAX_PATH];
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)mp_session_start, &self);
	GetModuleFileNameA(self, me, sizeof me);
	GetModuleFileNameA(NULL, exe, sizeof exe);
	fprintf(f, "Lords of the Realm II HD multiplayer session, %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	fprintf(f, "ddraw.dll (HD): built %s %s, crc %08lx\n", __DATE__, __TIME__, (unsigned long)crc_file(me));
	fprintf(f, "Lords2.exe: crc %08lx\n", (unsigned long)crc_file(exe));
	HMODULE dp = GetModuleHandleA("dplayx.dll");
	if (dp) {
		char dpPath[MAX_PATH]; GetModuleFileNameA(dp, dpPath, sizeof dpPath);
		BOOL repl = GetProcAddress(dp, "LF_NetSessionOpen") != NULL;
		fprintf(f, "dplayx.dll: %s, crc %08lx (%s)\n", dpPath, (unsigned long)crc_file(dpPath), repl ? "replacement multiplayer layer" : "Windows DirectPlay");
	}
	fprintf(f, "game fixes in this game: %s (MultiplayerFixes=%d)\n", g_mpFixes ? "on" : "off (multiplayer safe mode)", g_mpFixes);
	fprintf(f, "  MopUpFix=%d CastleCapFix=%d WheatFix=%d SiegeGarrisonFix=%d RunInBackground=%d\n",
		g_fixMopUp, g_fixCastleCap, g_fixWheat, g_fixSiegeGarrison, g_runBackground);
	fprintf(f, "this player: %d; game speed %d\n", *(int *)0x0057c8cc, *(int *)0x0053f230);
	fclose(f);
	logf_("multiplayer session log: %s", dir);
}

static DWORD WINAPI sync_log_thread(LPVOID unused)
{
	(void)unused;
	int lastSeason = -1; DWORD changed = 0; int pending = 0, n = 0;
	char req[MAX_PATH], mpDir[MAX_PATH] = "";
	BOOL inSession = FALSE;
	snprintf(req, sizeof req, "%s\\lords2hd-sync.req", g_gameDir);
	for (;;) {
		Sleep(250);
		BOOL mp = *(int *)0x00553030 != 0;
		if (g_mpLogs && mp && !inSession) { mp_session_start(mpDir, sizeof mpDir); n = 0; lastSeason = -1; }
		if (inSession && !mp && mpDir[0]) { mp_copy_logs(mpDir); logf_("multiplayer session ended"); }
		inSession = mp;
		// On request (a test drops lords2hd-sync.req, holding a name): every record's checksum to lords2hd-sync-<name>.txt
		FILE *rq = fopen(req, "r");
		if (rq) {
			char name[64] = "x";
			if (fscanf(rq, "%63s", name) != 1) strcpy(name, "x");
			fclose(rq); DeleteFileA(req);
			char path[MAX_PATH];
			snprintf(path, sizeof path, "%s\\lords2hd-sync-%s.txt", g_gameDir, name);
			FILE *f = fopen(path, "w");
			if (f) {
				int counties = *(int *)0x0056d5dc;
				if (counties < 1 || counties > 60) counties = 0;
				fprintf(f, "season %d/%d\n", *(int *)0x0057c92c, *(int *)0x0057c934);
				for (int i = 1; i <= counties; i++) {
					fprintf(f, "county %d %08lx", i, (unsigned long)crc_block(~0u, (const BYTE *)(0x0053f9b0 + i * 0x300), 0x300));
					for (int k = 0; k < 0x300; k += 0x40) fprintf(f, " %08lx", (unsigned long)crc_block(~0u, (const BYTE *)(0x0053f9b0 + i * 0x300 + k), 0x40));
					fprintf(f, "\n  head");
					for (int k = 0; k < 0x40; k++) fprintf(f, " %02x", *(const BYTE *)(0x0053f9b0 + i * 0x300 + k));
					fprintf(f, "\n");
				}
				for (int i = 1; i <= 0x96; i++) fprintf(f, "army %d %08lx\n", i, (unsigned long)crc_block(~0u, (const BYTE *)(0x0052f0b0 + i * 0x1a4), 0x1a4));
				for (int i = 1; i <= 5; i++) {
					fprintf(f, "player %d %08lx", i, (unsigned long)crc_block(~0u, (const BYTE *)(0x0057bf00 + i * 0x160), 0x160));
					for (int k = 0; k < 0x160; k += 0x20) fprintf(f, " %08lx", (unsigned long)crc_block(~0u, (const BYTE *)(0x0057bf00 + i * 0x160 + k), 0x20));
					fprintf(f, "\n");
				}
				fclose(f);
			}
		}
		int season =*(int *)0x0057c92c * 16 + *(int *)0x0057c934;
		if (season != lastSeason) { lastSeason = season; changed = GetTickCount(); pending = 1; }
		if (!pending || GetTickCount() - changed < 3000) continue;
		pending = 0;
		int counties = *(int *)0x0056d5dc;
		if (counties < 1 || counties > 60) continue;
		DWORD c = crc_block(~0u, (const BYTE *)(0x0053f9b0 + 0x300), (size_t)counties * 0x300);
		DWORD a = crc_block(~0u, (const BYTE *)(0x0052f0b0 + 0x1a4), (size_t)0x96 * 0x1a4);
		DWORD p = crc_block(~0u, (const BYTE *)(0x0057bf00 + 0x160), (size_t)5 * 0x160);
		++n;
		if (g_syncLog) logf_("sync %d: season %d/%d, counties %08lx, armies %08lx, players %08lx, multiplayer %d", n,
			*(int *)0x0057c92c, *(int *)0x0057c934, (unsigned long)c, (unsigned long)a, (unsigned long)p, *(int *)0x00553030);
		if (inSession && mpDir[0]) {
			char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\sync.txt", mpDir);
			FILE *f = fopen(path, "a");
			if (f) {
				SYSTEMTIME t; GetLocalTime(&t);
				fprintf(f, "%02d:%02d:%02d  check %d  season %d/%d  counties %08lx  armies %08lx  players %08lx\n", t.wHour, t.wMinute, t.wSecond, n,
					*(int *)0x0057c92c, *(int *)0x0057c934, (unsigned long)c, (unsigned long)a, (unsigned long)p);
				fclose(f);
			}
			mp_copy_logs(mpDir);
		}
	}
	return 0;
}

static void install_game_fixes(void)
{
	g_syncLog = GetPrivateProfileIntA("Debug", "SyncLog", 0, g_iniPath) != 0;
	g_mpLogs = GetPrivateProfileIntA("Debug", "MpLogs", 1, g_iniPath) != 0;
	if (g_syncLog || g_mpLogs) CloseHandle(CreateThread(NULL, 0, sync_log_thread, NULL, 0, NULL));
	g_fixSiegeGarrison = GetPrivateProfileIntA("Game", "SiegeGarrisonFix", 1, g_iniPath) != 0;
	static const BYTE kEnterPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C }; // push ebp; mov ebp,esp; sub esp,0Ch
	static const BYTE kStep1C[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C }, kStep10[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 },
		kStep18[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 }; // push ebp; mov ebp,esp; sub esp,n
	if (!hook_code((BYTE *)0x004a79a3, kEnterPrologue, sizeof kEnterPrologue, h_castle_enter, &o_castle_enter) ||
		!hook_code((BYTE *)0x004508de, kStep1C, sizeof kStep1C, h_season_castles, &o_season_castles) ||
		!hook_code((BYTE *)0x0049f12f, kStep1C, sizeof kStep1C, h_ai_reinforce, &o_ai_reinforce) ||
		!hook_code((BYTE *)0x004a07f2, kStep10, sizeof kStep10, h_ai_pick_castle, &o_ai_pick_castle) ||
		!hook_code((BYTE *)0x004a6270, kStep18, sizeof kStep18, h_ai_march, &o_ai_march))
		g_fixSiegeGarrison = FALSE;
	logf_("siege garrison fix %s", g_fixSiegeGarrison ? "on" : "off");
	g_runBackground = GetPrivateProfileIntA("Game", "RunInBackground", 2, g_iniPath);
	g_mpFixes = GetPrivateProfileIntA("Game", "MultiplayerFixes", 0, g_iniPath) != 0;
	g_fixWheat =GetPrivateProfileIntA("Game", "WheatFix", 1, g_iniPath) != 0;
	static const BYTE kWheatPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 }; // push ebp; mov ebp,esp; sub esp,8
	if (!hook_code((BYTE *)0x0044d281, kWheatPrologue, sizeof kWheatPrologue, h_wheat_scale, &o_wheat_scale))
		g_fixWheat = FALSE;
	logf_("wheat fix %s", g_fixWheat ? "on" : "off");

	g_fixCastleCap = GetPrivateProfileIntA("Game", "CastleCapFix", 1, g_iniPath) != 0;
	static const BYTE kAiBuildPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 }; // push ebp; mov ebp,esp; sub esp,8
	static const BYTE kCastlePrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };  // push ebp; mov ebp,esp; sub esp,0Ch
	if ((!hook_code((BYTE *)0x0049edc7, kAiBuildPrologue, sizeof kAiBuildPrologue, h_ai_build, &o_ai_build) ||
		!hook_code((BYTE *)0x00436d02, kCastlePrologue, sizeof kCastlePrologue, h_castle_start, &o_castle_start)))
		g_fixCastleCap = FALSE;
	logf_("castle cap fix %s", g_fixCastleCap ? "on" : "off");

	g_fixMopUp = GetPrivateProfileIntA("Game", "MopUpFix", 1, g_iniPath) != 0;
	static const BYTE kChasePrologue[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 }; // push ebp; mov ebp,esp; push ebx/esi/edi
	if (!hook_code((BYTE *)0x00483e55, kChasePrologue, sizeof kChasePrologue, h_chase, &o_chase))
		g_fixMopUp = FALSE;
	logf_("Mop-Up fix %s", g_fixMopUp ? "on" : "off");
}

static void install(void)
{
	install_game_fixes();
	static const BYTE kStepsPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 }; // push ebp; mov ebp,esp; sub esp,18h
	hook_code((BYTE *)0x004bb978, kStepsPrologue, sizeof kStepsPrologue, h_logic_steps, &o_logic_steps);
	if (memcmp((BYTE *)0x0040e8bb, kBlockPrologue, sizeof kBlockPrologue) == 0)
		hook_code((BYTE *)0x0040e8bb, kBlockPrologue, sizeof kBlockPrologue, h_game_frame, &o_game_frame);
	hook_import("smackw32.DLL", "#14", h_SmackOpen, &o_SmackOpen);
	hook_import("smackw32.DLL", "#23", h_SmackToBuffer, &o_SmackToBuffer);
	hook_import("smackw32.DLL", "#19", h_SmackDoFrame, &o_SmackDoFrame);
	hook_import("smackw32.DLL", "#18", h_SmackClose, &o_SmackClose);
	hook_import("WINMM.dll", "mmioOpenA", h_mmioOpenA, &o_mmioOpenA);
	char musicDir[MAX_PATH];
	snprintf(musicDir, sizeof musicDir, "%s\\lords2hd\\music", g_gameDir);
	if (GetFileAttributesA(musicDir) != INVALID_FILE_ATTRIBUTES) {
		HANDLE t = CreateThread(NULL, 0, music_convert_thread, NULL, 0, NULL);
		if (t) CloseHandle(t);
	}
	hook_import("USER32.dll", "GetCursorPos", h_GetCursorPos, &o_GetCursorPos);
	hook_import("USER32.dll", "GetForegroundWindow", h_GetForegroundWindow, &o_GetForegroundWindow);
	hook_import("USER32.dll", "IsIconic", h_IsIconic, &o_IsIconic);
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
	InitializeCriticalSection(&g_composeLock);
	InitializeCriticalSection(&g_recentLock);
	load_real_ddraw();

	SYSTEMTIME t;
	GetLocalTime(&t);
	logf_("Lords of the Realm II HD, %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	pak_open();
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
