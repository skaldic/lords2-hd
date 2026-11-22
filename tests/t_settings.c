/* t_settings.c - the settings: lords2hd.ini, the code's defaults, the F10 panel and the README's table of it. Our own
 * code. Included by tests.c (after t_battle.c, whose kFixSites it uses). The shipped ini, the README and the patch's
 * source are read from the repository the tests were built from. */

static void t_repo(char *out, size_t n, const char *rel) { snprintf(out, n, "%s\\..\\..\\%s", t_exeDir, rel); }
static char *t_slurp(const char *rel)   // a repository file as one string (free it)
{
	char p[MAX_PATH];
	t_repo(p, sizeof p, rel);
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *s = malloc(n + 1);
	if (s) { s[fread(s, 1, n, f)] = 0; }
	fclose(f);
	return s;
}

/* Every setting the patch reads: GetPrivateProfileIntA / GetPrivateProfileStringA calls with a literal section, key and
 * default, from its source. */
typedef struct { char sec[16], key[32], def[32]; BOOL isInt; } t_key;
static int t_scan_keys(t_key *out, int max)
{
	char *src = t_slurp("hd\\lords2hd.c");
	if (!src) return -1;
	int n = 0;
	static const char *const pat[2] = { "GetPrivateProfileIntA(\"", "GetPrivateProfileStringA(\"" };
	for (int k = 0; k < 2; k++)
		for (char *p = strstr(src, pat[k]); p; p = strstr(p + 1, pat[k])) {
			t_key t = { { 0 }, { 0 }, { 0 }, k == 0 };
			const char *q = p + strlen(pat[k]);
			int i = 0;
			while (*q && *q != '"' && i < 15) t.sec[i++] = *q++;
			if (strncmp(q, "\", \"", 4)) continue;
			q += 4; i = 0;
			while (*q && *q != '"' && i < 31) t.key[i++] = *q++;
			if (strncmp(q, "\", ", 3)) continue;
			q += 3; i = 0;
			if (!t.isInt) { if (*q != '"') continue; q++; while (*q && *q != '"' && i < 31) t.def[i++] = *q++; }
			else while (*q && *q != ',' && i < 31) t.def[i++] = *q++;
			BOOL dup = FALSE;
			for (int j = 0; j < n; j++) if (!strcmp(out[j].sec, t.sec) && !strcmp(out[j].key, t.key)) dup = TRUE;
			if (!dup && n < max) out[n++] = t;
		}
	free(src);
	return n;
}
/* Read by the patch but left out of the shipped ini on purpose (and the [Debug], [Tutorial] and [Patch] sections). */
static BOOL t_hidden_key(const t_key *k)
{
	static const char *const hidden[] = {
		"SmoothScroll",   // zoomed-out battles' smooth scrolling: off until its frame rate is fixed
		"FsrFinal",       // FSR's EASU for the final stretch too: for benchmarks
		"FsrSharpness",   // RCAS strength: tuning
		"WindowHeight",   // a window of that height, for testing sizes (described in INSTALL.txt)
	};
	if (!strcmp(k->sec, "Debug") || !strcmp(k->sec, "Tutorial") || !strcmp(k->sec, "Patch")) return TRUE;
	for (int i = 0; i < (int)(sizeof hidden / sizeof *hidden); i++) if (!strcmp(k->key, hidden[i])) return TRUE;
	return FALSE;
}
static BOOL t_shipped(const char *sec, const char *key, char *val, int n)   // the shipped ini's value
{
	char p[MAX_PATH];
	t_repo(p, sizeof p, "hd\\lords2hd.ini");
	GetPrivateProfileStringA(sec, key, "\x01", val, n, p);
	return val[0] != '\x01';
}

TEST(settings_found_in_the_source)
{
	t_key k[128];
	int n = t_scan_keys(k, 128);
	CHECK(n >= 50);                             // (the scan itself works: the patch reads about 60)
	BOOL wide = FALSE, mop = FALSE;
	for (int i = 0; i < n; i++) {
		if (!strcmp(k[i].key, "Wide") && k[i].isInt && !strcmp(k[i].def, "1")) wide = TRUE;
		if (!strcmp(k[i].sec, "Game") && !strcmp(k[i].key, "MopUpFix")) mop = TRUE;
	}
	CHECK(wide); CHECK(mop);
}

TEST(settings_every_one_is_in_the_shipped_ini)
{
	t_key k[128];
	int n = t_scan_keys(k, 128), missing = 0;
	for (int i = 0; i < n; i++) {
		char v[64];
		if (t_hidden_key(&k[i]) || t_shipped(k[i].sec, k[i].key, v, sizeof v)) continue;
		printf("    not in hd\\lords2hd.ini: [%s] %s (the code's default %s)\n", k[i].sec, k[i].key, k[i].def);
		missing++;
	}
	CHECK_EQ(missing, 0);
}

TEST(settings_code_defaults_match_the_shipped_ini)
{
	// A player whose ini lacks a key gets the code's default: it must be what the shipped ini (and the README) say.
	t_key k[128];
	int n = t_scan_keys(k, 128), off = 0;
	for (int i = 0; i < n; i++) {
		char v[64];
		if (t_hidden_key(&k[i]) || !t_shipped(k[i].sec, k[i].key, v, sizeof v)) continue;
		BOOL same = k[i].isInt ? atoi(v) == atoi(k[i].def) : !lstrcmpiA(v, k[i].def);
		if (!same) { printf("    [%s] %s: the shipped ini says %s, the code's default is %s\n", k[i].sec, k[i].key, v, k[i].def); off++; }
	}
	CHECK_EQ(off, 0);
}

TEST(settings_without_an_ini_are_the_documented_defaults)
{
	// no lords2hd.ini at all (or keys missing): what the README and INSTALL.txt give as the defaults
	DeleteFileA(g_iniPath);
	read_settings();
	CHECK_EQ(g_upscaleCR, 2);                   // AMD FSR 1 (was Catmull-Rom up to 1.5.1)
	CHECK(g_gpuWanted);                         // the graphics card draws
	CHECK(g_wantWide); CHECK(g_battleZoom);     // widescreen, battle zoom-out
	CHECK_EQ(g_frameLimit, 120);
	CHECK_EQ(g_statsOn, 0);
	CHECK(!g_miniPop);
	CHECK_EQ(g_wheelZoom, 1);                   // the wheel zooms a battle's field, up to 2x
	CHECK(g_hdScreens && g_hdTiles && g_hdVideos && g_hdText && g_shoreWaves);
	CHECK(g_tutorialOnce && g_wasdOn && g_cycleOn && g_armyPanel && g_letters && g_resounded);
	CHECK_EQ(g_scale, SCALE_SMOOTH);
	CHECK(g_fullscreen);
}

TEST(settings_ini_marks_its_version_for_the_installer)
{
	char v[64];
	CHECK(t_shipped("Patch", "IniVersion", v, sizeof v) && v[0]);   // without it the installer resets the ini
	char *iss = t_slurp("tools\\lords2hd.iss");
	REQUIRE(iss);
	CHECK(strstr(iss, "GetIniString('Patch', 'IniVersion'") != NULL);
	free(iss);
}

/* ================================ the F10 panel */

TEST(f10_every_item_on_one_tab_with_a_label)
{
	int seen[M_COUNT] = { 0 };
	for (int t = 0; t < MENU_TABS; t++) {
		int n = menu_tab_count(t);
		CHECK(n >= 1 && n <= MENU_ROWS);
		CHECK_EQ(kMenuTabItems[t][n], -1);      // ends the tab
		for (int r = 0; r < n; r++) {
			int it = kMenuTabItems[t][r];
			REQUIRE(it >= 0 && it < M_COUNT);
			seen[it]++;
		}
	}
	for (int i = 0; i < M_COUNT; i++) {
		if (seen[i] != 1) printf("    item %d (%s) is on %d tabs\n", i, kMenuLabel[i] ? kMenuLabel[i] : "?", seen[i]);
		CHECK_EQ(seen[i], 1);
		REQUIRE(kMenuLabel[i] && kMenuLabel[i][0]);
		for (int j = 0; j < i; j++) CHECK(strcmp(kMenuLabel[i], kMenuLabel[j]) != 0);
	}
	for (int f = 0; f < BF_COUNT; f++) {        // the Battles switches are in the order of their fixes
		char want[64]; snprintf(want, sizeof want, "GetPrivateProfileIntA(\"Game\", \"%s\", 1,", kBfKey[f]);
		char *src = t_slurp("hd\\lords2hd.c");
		CHECK(src && strstr(src, want));         // and read at start-up under the same name
		free(src);
	}
}

/* The settings as a player who just installed has them: the shipped ini, the panel opened. */
static void t_fix_sites(void)
{
	memset((void *)0x00436000, 0, 0x004b0000 - 0x00436000);
	for (int i = 0; i < (int)(sizeof kFixSites / sizeof *kFixSites); i++) memcpy((void *)kFixSites[i].at, kFixSites[i].code, 6);
	g_chaseHookFailed = FALSE; g_dmapOn = FALSE;
}
static void t_shipped_settings(void)
{
	char p[MAX_PATH];
	t_repo(p, sizeof p, "hd\\lords2hd.ini");
	CopyFileA(p, g_iniPath, FALSE);
	SetFileAttributesA(g_iniPath, FILE_ATTRIBUTE_NORMAL);
	WritePrivateProfileStringA("Debug", "MpLogs", "0", g_iniPath);
	WritePrivateProfileStringA("Debug", "Remote", "1", g_iniPath);   // (mutes the Lords2Resounded sample)
	read_settings();
	t_fix_sites();
	install_game_fixes();
	g_showScreens = g_hdScreens; g_showTiles = g_hdTiles;
	g_wantWideSet = g_wantWide; g_battleZoomSet = g_battleZoom;
	*(int *)0x0053f24c = 1;                     // the game's own tutorial option: on, as it ships
}
/* The panel's value of an item, as an int. */
static int t_panel(int i)
{
	switch (i) {
	case M_UPSCALE: return g_upscaleCR;   case M_SCREENS: return g_showScreens; case M_TILES: return g_showTiles;
	case M_TEXT: return g_hdText;         case M_WAVES: return g_shoreWaves;    case M_LIMIT: return g_frameLimit;
	case M_TUTORIAL: return g_tutorialOnce; case M_WASD: return g_wasdOn;      case M_CYCLE: return g_cycleOn;
	case M_WIDE: return g_wantWideSet;    case M_ZOOM: return g_battleZoomSet;  case M_ARMY: return g_armyPanel;
	case M_LETTERS: return g_letters;     case M_RESOUND: return g_resounded;   case M_RENDERER: return g_gpuWantedSet;
	case M_FPS: return g_statsOn;         case M_MINIPOP: return g_miniPop;     case M_ARROWS: return g_arrowsOn;
	case M_WHEEL: return g_wheelZoom;     case M_AI: return g_aiLevelSet;
	default: return g_bfSet[i - M_BF_PATH];
	}
}
/* The same setting as the patch reads it at start-up from the ini the panel wrote. */
static int t_read_back(int i)
{
	read_settings();
	t_fix_sites();
	install_game_fixes();
	switch (i) {
	case M_SCREENS: return g_hdScreens; case M_TILES: return g_hdTiles; case M_WIDE: return g_wantWide;
	case M_ZOOM: return g_battleZoom;   case M_RENDERER: return g_gpuWanted;
	default: return t_panel(i);         // (the same variable: read_settings / install_game_fixes set it)
	}
}

TEST(f10_changes_are_saved_and_read_back)
{
	t_shipped_settings();
	for (int i = 0; i < M_COUNT; i++) {
		int states = i == M_UPSCALE || i == M_FPS || i == M_AI || i == M_WHEEL ? 3 : i == M_TUTORIAL ? 2 : 2;
		int first = t_panel(i);
		for (int s = 0; s < states; s++) {
			if (i == M_TUTORIAL && s == 1) break;   // (Every time -> Off is fine; Off -> Once calls the game)
			menu_change(i);
			int now = t_panel(i);
			if (s == 0 && now == first) printf("    %s: a change didn't change it\n", kMenuLabel[i]);
			CHECK(s != 0 || now != first);
			int back = t_read_back(i);
			if (back != now) printf("    %s: the panel has %d, the ini gives %d at start-up\n", kMenuLabel[i], now, back);
			CHECK_EQ(back, now);
			t_shipped_settings();                // (read_settings above set every variable from the ini)
			for (int k = 0; k <= s; k++) menu_change(i);
		}
		if (i != M_TUTORIAL) {
			t_shipped_settings();
			for (int s = 0; s < states; s++) menu_change(i);
			CHECK_EQ(t_panel(i), first);         // round the choices, back to the start
		}
		t_shipped_settings();
	}
}

/* The README's table of the F10 settings: | Tab | Setting | Choices | Default | `key` | */
TEST(f10_readme_table_matches_the_panel)
{
	t_shipped_settings();
	char *md = t_slurp("README.md");
	REQUIRE(md);
	int rows = 0, tab = 0, row = 0;
	for (char *line = strstr(md, "\n| "); line; line = strstr(line + 1, "\n| ")) {
		char cell[5][96] = { { 0 } };
		const char *q = line + 1;
		int c = 0;
		while (c < 5 && *q == '|') {
			q++;
			while (*q == ' ') q++;
			int i = 0;
			while (*q && *q != '|' && *q != '\n' && i < 95) cell[c][i++] = *q++;
			while (i > 0 && cell[c][i - 1] == ' ') cell[c][--i] = 0;
			c++;
		}
		if (c < 5 || *q != '|') continue;
		int t;
		for (t = 0; t < MENU_TABS && strcmp(cell[0], kMenuTab[t]); t++) {}
		if (t == MENU_TABS) continue;            // the header, or another table
		if (t != tab) { CHECK_EQ(t, tab + 1); CHECK_EQ(row, menu_tab_count(tab)); tab = t; row = 0; }
		REQUIRE(row < menu_tab_count(t));
		int it = kMenuTabItems[t][row++];
		rows++;
		char lab[96]; snprintf(lab, sizeof lab, "%s *", kMenuLabel[it]);
		BOOL labelOk = !strcmp(cell[1], kMenuLabel[it]) || !strcmp(cell[1], lab);
		if (!labelOk) printf("    README row %d: \"%s\", the panel's \"%s\"\n", rows, cell[1], kMenuLabel[it]);
		CHECK(labelOk);
		char v[96]; BOOL rs;
		menu_value(it, v, sizeof v, &rs);
		BOOL defOk = !strncmp(v, cell[3], strlen(cell[3]));
		if (!defOk) printf("    README: %s defaults to \"%s\", the panel shows \"%s\" after an install\n", cell[1], cell[3], v);
		CHECK(defOk);
		// the key: changing the setting in the panel writes it
		char key[64] = { 0 };
		if (cell[4][0] == '`') { lstrcpynA(key, cell[4] + 1, sizeof key); char *e = strchr(key, '`'); if (e) *e = 0; }
		const char *sec = it >= M_BF_PATH ? "Game" : "Display";
		char before[64], after[64];
		GetPrivateProfileStringA(sec, key, "?", before, sizeof before, g_iniPath);
		if (it != M_TUTORIAL || *(int *)0x0053f24c) menu_change(it);
		GetPrivateProfileStringA(sec, key, "?", after, sizeof after, g_iniPath);
		if (!strcmp(before, after)) printf("    README: changing %s didn't write [%s] %s\n", cell[1], sec, key);
		CHECK(strcmp(before, after) != 0);
		t_shipped_settings();
	}
	CHECK_EQ(rows, M_COUNT);
	free(md);
}

TEST(f10_restart_marks)
{
	t_shipped_settings();
	char v[96]; BOOL rs;
	for (int i = 0; i < M_COUNT; i++) { menu_value(i, v, sizeof v, &rs); CHECK(v[0]); CHECK(!rs); }   // nothing pending after an install
	menu_change(M_WIDE); menu_value(M_WIDE, v, sizeof v, &rs); CHECK(rs);       // widescreen: after a restart
	menu_change(M_WIDE); menu_value(M_WIDE, v, sizeof v, &rs); CHECK(!rs);
	menu_change(M_RENDERER); menu_value(M_RENDERER, v, sizeof v, &rs); CHECK(rs);
	menu_change(M_TEXT); menu_value(M_TEXT, v, sizeof v, &rs); CHECK(!rs);       // the rest at once
	g_bfOk[BF_ENGAGE] = FALSE;                  // its hooks never went in: switching it on needs a restart
	menu_value(M_BF_ENGAGE, v, sizeof v, &rs); CHECK(rs);
	menu_change(M_BF_ENGAGE); menu_value(M_BF_ENGAGE, v, sizeof v, &rs); CHECK(!rs);   // off: nothing to wait for
}
