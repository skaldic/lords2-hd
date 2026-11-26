/* t_hooks.c - tier 3: the patch's start-up (install) on a real Lords2.exe copied into the fake memory. Our own code.
 * Included by tests.c, last. Needs a copy of the game's exe (tests\run.ps1 -GameExe, or L2HD_GAME_EXE): it is read,
 * never changed, and nothing of it goes into the repository. Without one these tests say so and pass.
 *
 * - every hook site holds the code the patch expects and every hook goes in, with Wide=0 and Wide=1;
 * - a trampoline (o_*) left NULL in a mode is never called, unguarded, from code that runs without its hook: the
 *   1.4.0 crash, where every yes/no box in 4:3 called o_yesno_draw (only made with the wide hooks) through
 *   yesno_keep. */

static BOOL t_map_game(void)
{
	const char *path = getenv("L2HD_GAME_EXE");
	if (!path || !*path) { printf("    skipped: no game exe given (tests\\run.ps1 -GameExe <copy of Lords2.exe>)\n"); return FALSE; }
	FILE *f = fopen(path, "rb");
	if (!f) { printf("    skipped: can't read %s\n", path); return FALSE; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	BYTE *b = malloc(n);
	BOOL ok = b && fread(b, 1, n, f) == (size_t)n;
	fclose(f);
	IMAGE_NT_HEADERS *nt = ok && n > 0x400 ? (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew) : NULL;
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.ImageBase != 0x00400000 ||
		nt->OptionalHeader.SizeOfImage > 0x001e9000) {
		printf("    skipped: %s is not the game's exe\n", path); free(b); return FALSE;
	}
	BYTE *base = (BYTE *)0x00400000;
	memset(base, 0, nt->OptionalHeader.SizeOfImage);
	memcpy(base, b, nt->OptionalHeader.SizeOfHeaders);
	IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
		DWORD raw = min(s->SizeOfRawData, s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData);
		if (s->PointerToRawData + raw <= (DWORD)n) memcpy(base + s->VirtualAddress, b + s->PointerToRawData, raw);
	}
	free(b);
	t_gameModule = (HMODULE)base;
	return TRUE;
}
static void t_unmap_game(void) { t_gameModule = NULL; memset((void *)0x00400000, 0, 0x00410000 - 0x00400000); }

static long t_log_size(void) { FILE *f = fopen(g_logPath, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
static char *t_log_since(long at)
{
	FILE *f = fopen(g_logPath, "rb");
	if (!f) return calloc(1, 1);
	fseek(f, 0, SEEK_END); long n = ftell(f) - at; fseek(f, at, SEEK_SET);
	char *s = calloc(1, n > 0 ? n + 1 : 1);
	if (n > 0) fread(s, 1, n, f);
	fclose(f);
	return s;
}
static int t_count_lines(const char *log, const char *what)   // prints them
{
	int k = 0;
	for (const char *p = strstr(log, what); p; p = strstr(p + 1, what)) {
		const char *a = p; while (a > log && a[-1] != '\n') a--;
		const char *e = strchr(p, '\n'); int len = e ? (int)(e - a) : (int)strlen(a);
		printf("    log: %.*s\n", len > 160 ? 160 : len, a);
		k++;
	}
	return k;
}

/* The patch's start-up on the copied exe with Wide=wide; the log lines it wrote. NULL: no exe (skipped). */
static char *t_install_with(int wide, const char *debug, BOOL *wideOn)
{
	if (!t_map_game()) return NULL;
	char ini[256];
	snprintf(ini, sizeof ini, "[Display]\r\nWide=%d\r\nBattleZoom=1\r\n[Debug]\r\nMpLogs=0\r\nRemote=0\r\n%s", wide, debug);
	t_write("lords2hd.ini", ini, strlen(ini));
	read_settings();
	for (int i = 0; i < (int)(sizeof t_tramps / sizeof *t_tramps); i++)
		if (strcmp(t_tramps[i].name, "o_setup_call")) memset(t_tramps[i].p, 0, t_tramps[i].n * sizeof(void *));
	g_chaseHookFailed = FALSE; g_dmapOn = FALSE;
	g_off = 0; g_frameW = GAME_W; g_frameH = GAME_H; g_extra = 0;
	if (!known_build()) { printf("    skipped: not the GOG build the patch was made for\n"); t_unmap_game(); return NULL; }
	long at = t_log_size();
	install();
	*wideOn = g_off > 0;
	return t_log_since(at);
}
static char *t_install(int wide, BOOL *wideOn) { return t_install_with(wide, "", wideOn); }

TEST(hooks_battle_log_goes_in)   // [Debug] BattleLog=1: its own hooks too, the computer AI's group tick among them
{
	BOOL on;
	char *log = t_install_with(0, "BattleLog=1\r\n", &on);
	if (!log) return;
	CHECK_EQ(t_count_lines(log, "not hooked"), 0);
	CHECK_EQ(t_count_lines(log, "battle log on"), 1);
	CHECK(o_group_tick != NULL);
	CHECK(o_path_search != NULL);
	free(log);
	t_unmap_game();
}

TEST(hooks_all_go_in_4_3)
{
	BOOL on;
	char *log = t_install(0, &on);
	if (!log) return;
	CHECK_EQ(t_count_lines(log, "not hooked"), 0);
	CHECK_EQ(t_count_lines(log, "import not found"), 0);
	CHECK(!on);
	for (int f = 0; f < BF_COUNT; f++) CHECK(g_bfOk[f]);   // every battle fix can be switched in F10
	CHECK(o_yesno_open != NULL);                            // the Esc-twice yes/no fix in 4:3 too
	CHECK_EQ(*(BYTE *)0x00472f20, 0x75);                    // Jen With a B's Mods on by default: messages kept up
	CHECK(g_rationHooked && g_jenMods);
	free(log);
	t_unmap_game();
}

TEST(hooks_all_go_in_widescreen)
{
	BOOL on;
	char *log = t_install(1, &on);
	if (!log) return;
	CHECK_EQ(t_count_lines(log, "not hooked"), 0);
	CHECK_EQ(t_count_lines(log, "import not found"), 0);
	CHECK_EQ(t_count_lines(log, "wide mode off"), 0);       // every pitch, frame and y*640 site as expected
	if (t_count_lines(log, "is not wider than 4:3")) printf("    (this PC's monitor is 4:3: the wide hooks weren't tried)\n");
	else CHECK(on);
	free(log);
	t_unmap_game();
}

TEST(hooks_jen_fixes_go_in)   // Jen With a B's Mods (on by default): the ration and multiplayer message fixes
{
	BOOL on;
	char *log = t_install(0, &on);
	if (!log) return;
	CHECK_EQ(t_count_lines(log, "Jen With a B's Mods on; ration fix on"), 1);
	CHECK_EQ(t_count_lines(log, "multiplayer message fix on"), 1);
	CHECK_EQ(*(BYTE *)0x0044e4a2, 0xE9);                   // jumps to ration_stub
	CHECK_EQ(*(DWORD *)0x0044e4a3 + 0x0044e4a7, (DWORD)ration_stub);
	CHECK_EQ(*(BYTE *)0x0044e4a7, 0x90); CHECK_EQ(*(BYTE *)0x0044e4a8, 0x90);
	static const BYTE keep[] = { 0x75, 0x35, 0x90, 0x90, 0x90, 0x90 };
	CHECK(memcmp((BYTE *)0x00472f20, keep, sizeof keep) == 0);
	CHECK_EQ(0x00472f22 + 0x35, 0x00472f13 + 6 + 0x3e);    // where kind 14's je goes (0x472f13: je +3Eh)
	CHECK_EQ(*(WORD *)0x00472f13, 0x840F);
	CHECK_EQ(*(DWORD *)0x00472f15, 0x3e);
	free(log);
	t_unmap_game();
}

/* ---- the 1.4.0 crash: a NULL trampoline called from code that runs without its hook ---- */

static BOOL t_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static const char *t_line_start(const char *src, const char *p) { while (p > src && p[-1] != '\n') p--; return p; }
/* The function a position in the source is in: its header line ("static ... name(" at the start of a line, not a
 * prototype). Writes its name; returns the header's position. */
static const char *t_enclosing(const char *src, const char *p, char *name, int n)
{
	for (const char *l = t_line_start(src, p); l > src; l = t_line_start(src, l - 1)) {
		if (strncmp(l, "static ", 7) && strncmp(l, "BOOL WINAPI ", 12) && strncmp(l, "HRESULT WINAPI ", 15)) continue;
		const char *e = strchr(l, '\n'), *paren = strchr(l, '(');
		if (!paren || (e && paren > e)) continue;
		const char *last = e ? e - 1 : l + strlen(l) - 1;
		while (last > l && (*last == '\r' || *last == ' ')) last--;
		if (*last == ';') continue;                 // a prototype or a variable
		const char *a = paren; while (a > l && a[-1] == ' ') a--;
		const char *b = a; while (b > l && t_ident(b[-1])) b--;
		int k = (int)(a - b); if (k >= n) k = n - 1;
		memcpy(name, b, k); name[k] = 0;
		return l;
	}
	name[0] = 0;
	return src;
}
static BOOL t_called_directly(const char *src, const char *fn)   // fn( anywhere but its own header and prototypes
{
	size_t n = strlen(fn);
	for (const char *p = strstr(src, fn); p; p = strstr(p + 1, fn)) {
		if ((p > src && t_ident(p[-1])) || p[n] != '(') continue;
		const char *l = t_line_start(src, p);
		if (!strncmp(l, "static ", 7)) continue;
		return TRUE;
	}
	return FALSE;
}
/* Calls of trampoline tr (NULL in this mode) not guarded by a check of it, in functions the patch calls itself. */
static int t_unguarded(const char *src, const char *tr, int wide)
{
	size_t n = strlen(tr);
	int bad = 0;
	for (const char *p = strstr(src, tr); p; p = strstr(p + 1, tr)) {
		if ((p > src && t_ident(p[-1])) || t_ident(p[n])) continue;
		const char *q = p + n;
		if (*q == '[') { while (*q && *q != ']') q++; if (*q) q++; }
		if (*q != '(') continue;                    // not a call
		char fn[64];
		const char *h = t_enclosing(src, p, fn, sizeof fn);
		if (!fn[0] || !t_called_directly(src, fn)) continue;   // runs only from the game, through its hook
		char g[5][80];
		snprintf(g[0], 80, "if (%s)", tr); snprintf(g[1], 80, "if (!%s)", tr); snprintf(g[2], 80, "%s ?", tr);
		snprintf(g[3], 80, "%s &&", tr); snprintf(g[4], 80, "!%s ||", tr);
		BOOL guarded = FALSE;
		for (int k = 0; k < 5 && !guarded; k++) { const char *x = strstr(h, g[k]); guarded = x && x < p; }
		if (guarded) continue;
		const char *l = t_line_start(src, p);
		int line = 1; for (const char *c = src; c < l; c++) if (*c == '\n') line++;
		printf("    Wide=%d: %s is NULL, and %s (which the patch calls itself) calls it unguarded at lords2hd.c:%d\n", wide, tr, fn, line);
		bad++;
	}
	return bad;
}
static int t_null_calls(int wide)
{
	BOOL on;
	char *log = t_install(wide, &on);
	if (!log) return -1;
	free(log);
	char *src = t_slurp("hd\\lords2hd.c");
	int bad = 0, nulls = 0;
	for (int i = 0; i < (int)(sizeof t_tramps / sizeof *t_tramps); i++)
		for (int k = 0; k < t_tramps[i].n; k++)
			if (!t_tramps[i].p[k]) { nulls++; bad += t_unguarded(src, t_tramps[i].name, wide); break; }
	printf("    Wide=%d: %d trampolines NULL after start-up\n", wide, nulls);
	free(src);
	t_unmap_game();
	return bad;
}
TEST(hooks_no_null_trampoline_called_4_3) { int bad = t_null_calls(0); if (bad >= 0) CHECK_EQ(bad, 0); }
TEST(hooks_no_null_trampoline_called_widescreen) { int bad = t_null_calls(1); if (bad >= 0) CHECK_EQ(bad, 0); }
