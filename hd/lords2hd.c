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

#define GAME_W 640
#define GAME_H 480

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
static BOOL g_logLoads, g_dumpPalettes;

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
	g_logLoads = GetPrivateProfileIntA("Debug", "LogLoads", 0, g_iniPath) != 0;
	g_dumpPalettes = GetPrivateProfileIntA("Debug", "DumpPalettes", 0, g_iniPath) != 0;
	logf_("settings: Enabled=%d Scale=%s Fullscreen=%d WindowScale=%d IntegerScale=%d LogLoads=%d DumpPalettes=%d",
		g_emulate, g_scale == SCALE_NONE ? "none" : g_scale == SCALE_SHARP ? "sharp" : "smooth",
		g_fullscreen, g_windowScale, g_integerScale, g_logLoads, g_dumpPalettes);
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
};

static Surface *g_primary;

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
		w = GAME_W;
		h = GAME_H;
	}
	else {
		double s = min((double)cw / GAME_W, (double)ch / GAME_H);
		if (g_integerScale && s >= 1.0) s = (double)(int)s;
		w = (int)(GAME_W * s + 0.5);
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

static BYTE g_frame8[GAME_W * GAME_H];
static DWORD g_lut[256];
static DWORD g_frame32[GAME_W * GAME_H];
static DWORD g_frame2x[GAME_W * 2 * GAME_H * 2];

// 0x00RRGGBB pixels: (9a + 3b + 3c + d) / 16 per channel, red and blue in one pass.
static inline DWORD mix4(DWORD a, DWORD b, DWORD c, DWORD d)
{
	DWORD rb = (a & 0xff00ff) * 9 + ((b & 0xff00ff) + (c & 0xff00ff)) * 3 + (d & 0xff00ff);
	DWORD g = (a & 0x00ff00) * 9 + ((b & 0x00ff00) + (c & 0x00ff00)) * 3 + (d & 0x00ff00);
	return ((rb >> 4) & 0xff00ff) | ((g >> 4) & 0x00ff00);
}

// Doubles the frame with bilinear interpolation: each output pixel sits a quarter of a source
// pixel from its nearest source pixel, so it is 9:3:3:1 of the four closest.
static void double_frame(void)
{
	for (int y = 0; y < GAME_H; y++) {
		const DWORD *row = g_frame32 + y * GAME_W;
		const DWORD *up = g_frame32 + (y > 0 ? y - 1 : y) * GAME_W;
		const DWORD *dn = g_frame32 + (y < GAME_H - 1 ? y + 1 : y) * GAME_W;
		DWORD *o0 = g_frame2x + (2 * y) * (2 * GAME_W);
		DWORD *o1 = o0 + 2 * GAME_W;
		for (int x = 0; x < GAME_W; x++) {
			int l = x > 0 ? x - 1 : x, r = x < GAME_W - 1 ? x + 1 : x;
			DWORD p = row[x];
			o0[2 * x] = mix4(p, row[l], up[x], up[l]);
			o0[2 * x + 1] = mix4(p, row[r], up[x], up[r]);
			o1[2 * x] = mix4(p, row[l], dn[x], dn[l]);
			o1[2 * x + 1] = mix4(p, row[r], dn[x], dn[r]);
		}
	}
}

static void present(void)
{
	if (!g_hwnd || !g_primary) return;
	static PALETTEENTRY lastPal[256];
	static DWORD palChanged;
	static BOOL palDumped = TRUE;

	EnterCriticalSection(&g_lock);
	memcpy(g_frame8, g_primary->bits, sizeof g_frame8);
	PALETTEENTRY pal[256];
	if (g_primary->pal) memcpy(pal, g_primary->pal->e, sizeof pal);
	else memset(pal, 0, sizeof pal);
	LeaveCriticalSection(&g_lock);

	if (memcmp(pal, lastPal, sizeof pal) != 0) {
		memcpy(lastPal, pal, sizeof pal);
		palChanged = GetTickCount();
		palDumped = FALSE;
	}
	if (g_dumpPalettes && !palDumped && GetTickCount() - palChanged > 500) {
		dump_palette_if_new(pal);
		palDumped = TRUE;
	}

	RECT v, c;
	view_rect(&v);
	GetClientRect(g_hwnd, &c);
	HDC dc = GetDC(g_hwnd);
	if (!dc) return;
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
		for (int i = 0; i < GAME_W * GAME_H; i++) g_frame32[i] = g_lut[g_frame8[i]];
		double_frame();
		bi.h.biWidth = GAME_W * 2;
		bi.h.biHeight = -GAME_H * 2;
		bi.h.biBitCount = 32;
		StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, GAME_W * 2, GAME_H * 2, g_frame2x,
			(BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
	else {
		for (int i = 0; i < 256; i++) {
			bi.colours[i].rgbRed = pal[i].peRed;
			bi.colours[i].rgbGreen = pal[i].peGreen;
			bi.colours[i].rgbBlue = pal[i].peBlue;
		}
		bi.h.biWidth = GAME_W;
		bi.h.biHeight = -GAME_H;
		bi.h.biBitCount = 8;
		bi.h.biClrUsed = 256;
		StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, GAME_W, GAME_H, g_frame8,
			(BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
	ReleaseDC(g_hwnd, dc);
}

// Presents whenever the frame changed, at most once per few milliseconds: the game copies its
// frame in pieces (one start-up effect uses 1200 16x16 blocks), and each copy only marks it dirty.
static DWORD WINAPI presenter(void *unused)
{
	for (;;) {
		WaitForSingleObject(g_dirtyEvent, 50);
		if (InterlockedExchange(&g_dirty, 0)) {
			present();
			Sleep(4);
		}
		else if (g_dumpPalettes) {
			present(); // lets a palette that stays put be logged
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

// Clips a copy of w x h pixels from (sx, sy) to (dx, dy) against both surfaces.
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

static void copy_rect(Surface *dst, int dx, int dy, Surface *src, int sx, int sy, int w, int h)
{
	if (!clip_copy(dst, src, &dx, &dy, &sx, &sy, &w, &h)) return;
	for (int y = 0; y < h; y++)
		memmove(dst->bits + (dy + y) * dst->pitch + dx, src->bits + (sy + y) * src->pitch + sx, w);
}

static HRESULT WINAPI surf_blt(void *this_, RECT *dr, void *src_, RECT *sr, DWORD flags, DDBLTFX *fx)
{
	SELF(Surface);
	Surface *src = src_;
	RECT d = dr ? *dr : (RECT){ 0, 0, self->w, self->h };
	if (self->primary) EnterCriticalSection(&g_lock);
	if (flags & DDBLT_COLORFILL) {
		BYTE colour = fx ? (BYTE)fx->dwFillColor : 0;
		int x0 = max(0, d.left), x1 = min(self->w, d.right), y0 = max(0, d.top), y1 = min(self->h, d.bottom);
		for (int y = y0; y < y1; y++) memset(self->bits + y * self->pitch + x0, colour, max(0, x1 - x0));
	}
	else if (src) {
		RECT s = sr ? *sr : (RECT){ 0, 0, src->w, src->h };
		int w = d.right - d.left, h = d.bottom - d.top;
		if (w == s.right - s.left && h == s.bottom - s.top) {
			copy_rect(self, d.left, d.top, src, s.left, s.top, w, h);
		}
		else {
			// Never seen from the game; nearest-pixel stretch so it still shows something.
			static BOOL said;
			if (!said) { said = TRUE; logf_("stretching Blt %ldx%ld -> %dx%d", s.right - s.left, s.bottom - s.top, w, h); }
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
	RECT s = sr ? *sr : (RECT){ 0, 0, src->w, src->h };
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
	BYTE *p = self->bits;
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
	s->w = s->primary || !(d->dwFlags & DDSD_WIDTH) ? GAME_W : (int)d->dwWidth;
	s->h = s->primary || !(d->dwFlags & DDSD_HEIGHT) ? GAME_H : (int)d->dwHeight;
	s->pitch = (s->w + 3) & ~3;
	s->bits = calloc(1, s->pitch * s->h);
	if (!s->bits) { free(s); return DDERR_OUTOFMEMORY; }
	if (d->dwFlags & DDSD_BACKBUFFERCOUNT) unexpected("CreateSurface with back buffers");
	logf_("surface %p: %dx%d caps %08lx%s", (void *)s, s->w, s->h, (unsigned long)s->caps, s->primary ? " (primary)" : "");
	if (s->primary) {
		EnterCriticalSection(&g_lock);
		g_primary = s;
		LeaveCriticalSection(&g_lock);
		start_presenter();
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
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
	else {
		DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
		RECT r = { 0, 0, GAME_W * g_windowScale, GAME_H * g_windowScale };
		AdjustWindowRect(&r, style, FALSE);
		int ww = r.right - r.left, wh = r.bottom - r.top;
		int x = mi.rcWork.left + max(0, (int)(mi.rcWork.right - mi.rcWork.left - ww) / 2);
		int y = mi.rcWork.top + max(0, (int)(mi.rcWork.bottom - mi.rcWork.top - wh) / 2);
		SetWindowLongA(g_hwnd, GWL_STYLE, style);
		SetWindowLongA(g_hwnd, GWL_EXSTYLE, GetWindowLongA(g_hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
		SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
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
	int gx = MulDiv(*x - v.left, GAME_W, vw), gy = MulDiv(*y - v.top, GAME_H, vh);
	*x = max(0, min(GAME_W - 1, gx));
	*y = max(0, min(GAME_H - 1, gy));
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL) {
		int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
		client_to_game(&x, &y);
		lp = MAKELPARAM(x, y);
	}
	else if (msg == WM_PAINT || msg == WM_SIZE) {
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
}

static BOOL (WINAPI *o_GetCursorPos)(POINT *);
static BOOL (WINAPI *o_GetClientRect)(HWND, RECT *);
static BOOL (WINAPI *o_ClientToScreen)(HWND, POINT *);
static HCURSOR (WINAPI *o_SetCursor)(HCURSOR);

static BOOL WINAPI h_GetCursorPos(POINT *p)
{
	if (!o_GetCursorPos(p)) return FALSE;
	if (g_hwnd) {
		ScreenToClient(g_hwnd, p);
		int x = p->x, y = p->y;
		client_to_game(&x, &y);
		p->x = x;
		p->y = y;
	}
	return TRUE;
}

static BOOL WINAPI h_GetClientRect(HWND hwnd, RECT *r)
{
	if (hwnd && hwnd == g_hwnd && r) {
		SetRect(r, 0, 0, GAME_W, GAME_H);
		return TRUE;
	}
	return o_GetClientRect(hwnd, r);
}

static BOOL WINAPI h_ClientToScreen(HWND hwnd, POINT *p)
{
	if (hwnd && hwnd == g_hwnd) return TRUE;
	return o_ClientToScreen(hwnd, p);
}

// Cursors: the game's are 32x32 one-bit; scaled by the system they turn blocky and their
// inverting pixels vanish, so each is rebuilt once as an alpha cursor enlarged by whole pixels.
static struct { HCURSOR from, to; int k; } g_cursors[64];
static int g_cursorCount;

static HCURSOR rebuild_cursor(HCURSOR src, int k)
{
	ICONINFO ii;
	BITMAP bm;
	if (k <= 1 || !GetIconInfo(src, &ii)) return src;
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

	int W = w * k, H = h * k;
	bi.bmiHeader.biWidth = W;
	bi.bmiHeader.biHeight = -H;
	HBITMAP colourBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&out, NULL, 0);
	if (!colourBmp) goto done;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int i = (y / k) * w + (x / k);
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
	ICONINFO made = { FALSE, ii.xHotspot * k, ii.yHotspot * k, maskBmp, colourBmp };
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
	int k = max(1, ((v.bottom - v.top) + GAME_H / 2) / GAME_H);
	for (int i = 0; i < g_cursorCount; i++) {
		if (g_cursors[i].from == cur && g_cursors[i].k == k) return o_SetCursor(g_cursors[i].to);
		if (g_cursors[i].to == cur) return o_SetCursor(cur); // one of ours, handed back to us
	}
	// A rebuilt cursor is never destroyed (the game may hand it back at any time), so only as
	// many are made as can be remembered.
	if (g_cursorCount >= (int)(sizeof g_cursors / sizeof g_cursors[0])) return o_SetCursor(cur);
	HCURSOR made = rebuild_cursor(cur, k);
	g_cursors[g_cursorCount].from = cur;
	g_cursors[g_cursorCount].to = made;
	g_cursors[g_cursorCount].k = k;
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
	if (g_logLoads) logf_("load %s -> %p, %d bytes", name, buffer, n);
	note_load(name);
	return n;
}

// The one build this was made for: the GOG release, 1997-06-05.
static BOOL known_build(void)
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	return nt->FileHeader.TimeDateStamp == 0x3396EFAA && nt->OptionalHeader.SizeOfImage == 0x1E9000 &&
		memcmp(LOAD_FILE, kLoadPrologue, sizeof kLoadPrologue) == 0;
}

static void install(void)
{
	hook_import("USER32.dll", "GetCursorPos", h_GetCursorPos, &o_GetCursorPos);
	hook_import("USER32.dll", "GetClientRect", h_GetClientRect, &o_GetClientRect);
	hook_import("USER32.dll", "ClientToScreen", h_ClientToScreen, &o_ClientToScreen);
	hook_import("USER32.dll", "SetCursor", h_SetCursor, &o_SetCursor);
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
