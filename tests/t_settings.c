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
	CHECK(g_countyZoom);                        // and the county map out
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
	static const BYTE ration[] = { 0x39, 0x8C, 0x40, 0x34, 0xFB, 0x53, 0x00 }, rationOn[] = { 0x8B, 0x45, 0xE0 },
		msg[] = { 0x0F, 0x85, 0x17, 0x00, 0x00, 0x00 };   // Jen With a B's Mods: the ration cut and the message close, as in the exe
	memcpy((void *)0x0044e4a2, ration, sizeof ration); memcpy((void *)0x0044e523, rationOn, sizeof rationOn);
	memcpy((void *)0x00472f20, msg, sizeof msg);
	for (int i = 0; i < 2; i++) {               // Jen With a B's castle and field mods: the game's code at their sites
		memcpy((void *)kFieldLossSites[i].at, kFieldLossSites[i].game, kFieldLossSites[i].n);
		memcpy((void *)kPillageSites[i].at, kPillageSites[i].game, kPillageSites[i].n);
		*(DWORD *)kDoorInSites[i] = DOOR_IN; *(DWORD *)kDoorOutSites[i] = DOOR_OUT;
	}
	memcpy((void *)0x00449c5e, "\x50\xE8\xC8\xF7\x01\x00", 6);
	for (int i = 0; i < 7; i++) *(DWORD *)kReclaimWorkSites[i] = RECLAIM_WORK;
	for (int i = 0; i < 10; i++) *(DWORD *)kReclaimMenSites[i] = RECLAIM_MEN;
	for (int k = 0; k < 3; k++) *(DWORD *)kReclaimStageSites[k] = 200 * (k + 1);
	memcpy(CASTLE_CAP, kCastleCap, sizeof kCastleCap);   // the castles' room, the AI's read of it, the army limit
	memcpy((void *)AI_CAP_READ, "\x8B\x04\x85\x10\x8A\x4D\x00", 7);
	memcpy((void *)0x004a97c1, "\x81\x7D\xFC\xDC\x05\x00\x00", 7);
	memcpy((void *)0x0047c5e4, "\xE8\xF0\x32\x03\x00", 5);   // castle design: the two siege map reads, the troops' free-cell search
	memcpy((void *)0x0047cf9d, "\xE8\x37\x29\x03\x00", 5);
	memcpy((void *)FREE_SPOT_CALL, "\xE8\x14\x7A\x02\x00", 5);
	// Jen With a B's game, economy and unit mods: the game's code at the hooks and patches, its unit tables
	for (int h = 0; h < JEN_HOOKS; h++) memcpy((void *)kJenHooks[h].at, kJenHooks[h].game, kJenHooks[h].n);
	memcpy((void *)kPlagueOff[0].at, kPlagueOff[0].game, 6);
	for (int i = 0; i < 7; i++) *(BYTE *)kPlagueMild[i].at = kPlagueMild[i].game[0];
	*(BYTE *)(RES_CAP_AT - 3) = 0xC7; *(DWORD *)RES_CAP_AT = RES_CAP;
	*(DWORD *)kArmySites[1] = ARMY_SIZE;   // (the first: the cmp above)
	*(DWORD *)kIconSites[0] = ICON_1; *(DWORD *)kIconSites[1] = ICON_2;
	for (int u = 0; u < UNITS; u++) {
		memcpy((void *)unit_at(u, US_MELEE), kMeleeGame[u], 8);
		BYTE *r = (BYTE *)kUnitRdefAt[u];
		r[-6] = 0xC6; r[-5] = 0x05; *(DWORD *)(r - 4) = 0x005545f2; *r = (BYTE)kUnitGame[u][US_RDEF];
		*(DWORD *)unit_at(u, US_AUTO) = kUnitGame[u][US_AUTO];
		if (kUnitRanged[u] >= 0) {
			*(DWORD *)unit_at(u, US_RANGE) = kUnitGame[u][US_RANGE] * 8; *(DWORD *)unit_at(u, US_SPEED) = kUnitGame[u][US_SPEED];
			*(DWORD *)(unit_at(u, US_RANGE) + 8) = kRangedRest[u]; *(DWORD *)unit_at(u, US_RSTR) = kUnitGame[u][US_RSTR];
		}
		if (kUnitCost[u] >= 0) { *(DWORD *)unit_at(u, UC_WOOD) = kUnitGame[u][UC_WOOD]; *(DWORD *)unit_at(u, UC_IRON) = kUnitGame[u][UC_IRON]; }
		if (kUnitPrice[u] >= 0) { *(DWORD *)unit_at(u, UC_SELL) = kUnitGame[u][UC_SELL]; *(int *)(0x005532a0 + 16 * kUnitPrice[u]) = kUnitGame[u][UC_SELL]; }
	}
	*GAME_SPEED = 50;
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
	g_unitSel = 1;                              // the unit rows show archers, as at start-up
}
/* The panel's value of an item, as an int. */
static int t_panel(int i)
{
	if (menu_design(i)) return g_jenDesign[i - M_JEN_DESIGN];
	if (menu_unit_row(i)) return g_unitSet[g_unitSel][menu_unit_stat(i)];
	switch (i) {
	case M_UPSCALE: return g_upscaleCR;   case M_SCREENS: return g_showScreens; case M_TILES: return g_showTiles;
	case M_TEXT: return g_hdText;         case M_WAVES: return g_shoreWaves;    case M_LIMIT: return g_frameLimit;
	case M_TUTORIAL: return g_tutorialOnce; case M_WASD: return g_wasdOn;      case M_CYCLE: return g_cycleOn;
	case M_WIDE: return g_wantWideSet;    case M_ZOOM: return g_battleZoomSet;  case M_ARMY: return g_armyPanel;
	case M_LETTERS: return g_letters;     case M_RESOUND: return g_resounded;   case M_RENDERER: return g_gpuWantedSet;
	case M_FPS: return g_statsOn;         case M_MINIPOP: return g_miniPop;     case M_ARROWS: return g_arrowsOn;
	case M_WHEEL: return g_wheelZoom;     case M_AI: return g_aiLevelSet;       case M_COUNTY: return g_countyZoom;
	case M_SEASONS: return g_seasons;
	case M_JEN: return g_jenMods;         case M_JEN_RATION: return g_fixRation; case M_JEN_MPMSG: return g_mpMsgFix;
	case M_JEN_FIELDS: return g_jenFieldLoss; case M_JEN_PILLAGE: return g_jenPillage;
	case M_JEN_DOOR_IN: return g_jenDoorIn; case M_JEN_DOOR_OUT: return g_jenDoorOut;
	case M_JEN_WORK: return g_jenWork;    case M_JEN_MEN: return g_jenMen;      case M_JEN_GARRISON: return g_jenGarrison;
	case M_JEN_SPEED: return g_jenSpeed;  case M_JEN_PLAGUE: return g_jenPlague;  case M_JEN_POP: return g_jenPop;
	case M_JEN_UNCAP: return g_jenUncap;  case M_JEN_RESMUL: return g_jenResMul; case M_JEN_ARMY: return g_jenArmy;
	case M_JEN_ICONS: return g_jenIcons;  case M_JEN_UNIT: case M_JEN_UNIT_C: return g_unitSel;
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
		int states = menu_design(i) ? cd_count(i - M_JEN_DESIGN) + 1 : i == M_UPSCALE || i == M_FPS || i == M_AI || i == M_WHEEL ? 3 : i == M_JEN_DOOR_IN ? 7 :
			i == M_JEN_DOOR_OUT ? 6 : i == M_JEN_WORK ? 5 : i == M_JEN_MEN ? 4 : i == M_JEN_PLAGUE ? 3 : i == M_JEN_POP ? 9 :
			i == M_JEN_RESMUL ? RESMUL_MAX : i == M_JEN_ARMY ? 7 : i == M_JEN_UNIT || i == M_JEN_UNIT_C ? UNITS : 2;
		if (menu_unit_row(i)) { int st[8]; states = unit_steps(g_unitSel, menu_unit_stat(i), st); }
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
		const char *sec = menu_unit_row(it) ? "Units" : it >= M_BF_PATH ? "Game" : "Display";
		if (!strncmp(key, "<Unit>", 6)) { char k2[64]; snprintf(k2, sizeof k2, "%s%s", kUnitKey[g_unitSel], key + 6); lstrcpynA(key, k2, sizeof key); }
		if (!key[0]) { CHECK(it == M_JEN_UNIT || it == M_JEN_UNIT_C); t_shipped_settings(); continue; }   // (the unit rows' unit: not saved)
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

TEST(f11_jen_panel_only_with_its_switch)
{
	t_shipped_settings();
	Surface *back = g_back; g_back = NULL;      // (the panel isn't drawn: no game to draw it)
	g_menuOpen = FALSE; g_menuMods = FALSE; g_menuTab = 0;
	CHECK(g_jenMods);                           // on as shipped: the message fix in
	CHECK_EQ(*(BYTE *)0x00472f20, 0x75);
	CHECK(g_rationHooked);
	for (int k = 0; k < MENU_TABS; k++) CHECK_EQ(menu_tab_shown(k), k < MODS_TAB0);   // F10: the patch's 4 tabs
	CHECK(menu_key(VK_F11));                    // F11: the Jen With a B's Mods panel, its own tabs only
	CHECK(g_menuOpen && g_menuMods);
	CHECK_EQ(g_menuTab, TAB_GAME);              // Jen's groups: Game, Economy, Building and Unit Mods, then the fixes
	for (int k = 0; k < MENU_TABS; k++) CHECK_EQ(menu_tab_shown(k), k >= MODS_TAB0);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_ECONOMY);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_BUILDINGS);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_UNITS);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_STATS);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_COSTS);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_FIXES);
	menu_set_tab(g_menuTab + 1); CHECK_EQ(g_menuTab, TAB_GAME);    // Tab wraps within the panel
	menu_set_tab(g_menuTab - 1); CHECK_EQ(g_menuTab, TAB_FIXES);
	CHECK(menu_key(VK_F10));                    // F10 from it: the HD settings, at their last tab
	CHECK(g_menuOpen && !g_menuMods); CHECK_EQ(g_menuTab, 0);
	g_menuTab = 2;
	CHECK(menu_link_shown());                   // Quality of life: the line to the Jen With a B's Mods panel
	CHECK(menu_key(VK_F11));
	CHECK(g_menuMods); CHECK_EQ(g_menuTab, TAB_FIXES);             // back at its last tab
	CHECK(menu_key(VK_F11));                    // its own key closes it
	CHECK(!g_menuOpen);
	CHECK(menu_key(VK_F11)); CHECK_EQ(g_menuTab, TAB_FIXES);       // and opens it again where it was left
	CHECK(menu_key(VK_ESCAPE)); CHECK(!g_menuOpen);
	CHECK(menu_key(VK_F10)); CHECK(!g_menuMods); CHECK_EQ(g_menuTab, 2);   // F10 too
	CHECK(menu_key(VK_F10)); CHECK(!g_menuOpen);
	menu_change(M_JEN);                         // off: no panel (F11 is the game's), the game's own code
	CHECK(!g_jenMods);
	CHECK_EQ(*(BYTE *)0x00472f20, 0x0F);
	CHECK(!menu_key(VK_F11)); CHECK(!g_menuOpen);
	g_menuTab = 2; CHECK(!menu_link_shown());
	menu_change(M_JEN);                         // on: the message fix goes in at once
	CHECK(g_jenMods);
	CHECK_EQ(*(BYTE *)0x00472f20, 0x75); CHECK_EQ(*(BYTE *)0x00472f21, 0x35); CHECK_EQ(*(BYTE *)0x00472f25, 0x90);
	menu_change(M_JEN_MPMSG);                   // its own switch: the game's code back
	CHECK_EQ(*(BYTE *)0x00472f20, 0x0F); CHECK_EQ(*(DWORD *)0x00472f22, 0x17);
	menu_change(M_JEN_MPMSG);
	CHECK_EQ(*(BYTE *)0x00472f20, 0x75);
	g_menuOpen = FALSE; g_menuMods = FALSE; g_menuTab = 0; g_back = back;
	t_shipped_settings();
}
TEST(jen_castle_mods_go_in_and_out)
{
	*(int *)0x00553030 = 0;                     // single player
	t_shipped_settings();
	for (int j = 0; j < JC_COUNT; j++) CHECK(g_jcOk[j]);
	CHECK_EQ(*(BYTE *)0x00449c79, 0x0F);        // as shipped: the game's own everywhere
	CHECK_EQ(*(BYTE *)0x00466814, 0x0F); CHECK_EQ(*(BYTE *)0x0049f95f, 0xE8);
	CHECK_EQ(*(DWORD *)0x00483aeb, DOOR_IN); CHECK_EQ(*(DWORD *)0x00484ac7, DOOR_OUT);
	menu_change(M_JEN_FIELDS);                  // weather: both jne now jmp to where they went
	CHECK_EQ(*(BYTE *)0x00449c79, 0xE9); CHECK_EQ(0x00449c79 + 5 + *(DWORD *)0x00449c7a, 0x00449cc7);
	CHECK_EQ(*(BYTE *)0x00449cd9, 0xE9); CHECK_EQ(0x00449cd9 + 5 + *(DWORD *)0x00449cda, 0x00449d27);
	menu_change(M_JEN_PILLAGE);                 // pillaging: to the function's end; the AI's call nopped
	CHECK_EQ(*(BYTE *)0x00466814, 0xE9); CHECK_EQ(0x00466814 + 5 + *(DWORD *)0x00466815, 0x0046688e);
	CHECK_EQ(*(DWORD *)0x0049f95f, 0x90909090); CHECK_EQ(*(BYTE *)0x0049f963, 0x90);
	menu_change(M_JEN_DOOR_IN);                 // 5000 -> 10000
	CHECK_EQ(g_jenDoorIn, 10000); CHECK_EQ(*(DWORD *)0x00483aeb, 10000); CHECK_EQ(*(DWORD *)0x00484a5c, 10000);
	CHECK_EQ(*(DWORD *)0x00483b41, DOOR_OUT);
	menu_change(M_JEN_WORK);                    // 800 -> 1600: every site, the picture's steps a quarter each
	CHECK_EQ(*(DWORD *)0x0044c187, 1600); CHECK_EQ(*(DWORD *)0x0044c4c6, 1600);
	CHECK_EQ(*(DWORD *)0x0044c19f, 400); CHECK_EQ(*(DWORD *)0x0044c1c1, 800); CHECK_EQ(*(DWORD *)0x0044c1e3, 1200);
	CHECK_EQ(*(DWORD *)0x0044c15a, RECLAIM_MEN);
	menu_change(M_JEN_MEN);                     // 200 -> 400
	CHECK_EQ(*(DWORD *)0x0044c15a, 400); CHECK_EQ(*(DWORD *)0x0044c502, 400);
	CHECK(!g_jenGarrison); CHECK_EQ(CASTLE_CAP[4], 600);
	menu_change(M_JEN_GARRISON);                // every castle as an army; the AI plans from the original sizes
	for (int k = 0; k < 5; k++) { CHECK_EQ(CASTLE_CAP[k], 1500); CHECK_EQ(g_aiCap[k], kCastleCap[k]); }
	CHECK_EQ(*(DWORD *)(AI_CAP_READ + 3), (DWORD)g_aiCap); CHECK_EQ(*(BYTE *)AI_CAP_READ, 0x8B);
	g_jenArmy = 300;                            // a smaller army limit (Jen's Army Size): followed, the AI capped by it
	jen_sync();
	CHECK_EQ(*(DWORD *)0x004a97c4, 300); CHECK_EQ(*(DWORD *)0x004aa1b5, 300);
	CHECK_EQ(CASTLE_CAP[0], 300); CHECK_EQ(g_aiCap[0], 150); CHECK_EQ(g_aiCap[3], 300); CHECK_EQ(g_aiCap[4], 300);
	g_jenArmy = ARMY_SIZE;
	jen_sync();
	CHECK_EQ(CASTLE_CAP[2], 1500); CHECK_EQ(g_aiCap[4], 600);

	*(int *)0x00553030 = 1;                     // a multiplayer game: the game's own, the switches kept
	jen_sync();
	CHECK_EQ(*(BYTE *)0x00449c79, 0x0F); CHECK_EQ(*(BYTE *)0x00466814, 0x0F); CHECK_EQ(*(BYTE *)0x0049f95f, 0xE8);
	CHECK_EQ(*(DWORD *)0x00483aeb, DOOR_IN); CHECK_EQ(*(DWORD *)0x0044c187, RECLAIM_WORK);
	CHECK_EQ(*(DWORD *)0x0044c19f, 200); CHECK_EQ(*(DWORD *)0x0044c15a, RECLAIM_MEN);
	CHECK(memcmp(CASTLE_CAP, kCastleCap, sizeof kCastleCap) == 0); CHECK_EQ(*(DWORD *)(AI_CAP_READ + 3), 0x004d8a10);
	CHECK(!g_jenFieldLoss && g_jenDoorIn == 10000 && g_jenGarrison);
	*(int *)0x00553030 = 0;                     // back to single player: in again
	jen_sync();
	CHECK_EQ(*(BYTE *)0x00449c79, 0xE9); CHECK_EQ(*(DWORD *)0x00483aeb, 10000); CHECK_EQ(*(DWORD *)0x0044c1e3, 1200);
	CHECK_EQ(CASTLE_CAP[1], 1500);
	menu_change(M_JEN);                         // Jen With a B's Mods off: all the game's own
	CHECK_EQ(*(BYTE *)0x00449cd9, 0x0F); CHECK_EQ(*(DWORD *)0x00484a5c, DOOR_IN); CHECK_EQ(*(DWORD *)0x0044c502, RECLAIM_MEN);
	CHECK(memcmp(CASTLE_CAP, kCastleCap, sizeof kCastleCap) == 0); CHECK_EQ(*(DWORD *)(AI_CAP_READ + 3), 0x004d8a10);
	for (int j = 0; j < JC_COUNT; j++) CHECK(g_jcOk[j]);

	t_shipped_settings();                       // code that isn't the game's: that mod stays out, the rest work
	*(BYTE *)0x00466815 = 0x06;
	read_settings(); install_game_fixes();
	CHECK(!g_jcOk[JC_PILLAGE]); CHECK(g_jcOk[JC_FIELD]);
	char v[96]; BOOL rs;
	menu_value(M_JEN_PILLAGE, v, sizeof v, &rs); CHECK(strstr(v, "unavailable") != NULL);
	menu_change(M_JEN_PILLAGE);
	CHECK_EQ(*(BYTE *)0x00466814, 0x0F); CHECK_EQ(*(BYTE *)0x00466815, 0x06);
	WritePrivateProfileStringA("Game", "InnerDoor", "999999", g_iniPath);   // the ini's numbers: kept in range
	WritePrivateProfileStringA("Game", "ReclaimWork", "0", g_iniPath);
	t_fix_sites(); install_game_fixes();
	CHECK_EQ(g_jenDoorIn, DOOR_MAX); CHECK_EQ(g_jenWork, 1);
	CHECK_EQ(*(DWORD *)0x00483aeb, DOOR_MAX); CHECK_EQ(*(DWORD *)0x0044c19f, 1);
	t_shipped_settings();
}
TEST(jen_econ_mods_go_in_and_out)
{
	*(int *)0x00553030 = 0;                     // single player
	t_shipped_settings();
	for (int j = 0; j < JE_COUNT; j++) CHECK(g_jeOk[j]);
	// Uncapped game speed: the game speed's slider only; off again: back to 10
	CHECK(!l2hd_speed_free(GAME_SPEED));
	menu_change(M_JEN_SPEED);
	CHECK(l2hd_speed_free(GAME_SPEED)); CHECK(!l2hd_speed_free((const int *)0x0053f234));
	*GAME_SPEED = 150;
	menu_change(M_JEN_SPEED);
	CHECK(!l2hd_speed_free(GAME_SPEED)); CHECK_EQ(*GAME_SPEED, 100);
	// Black Death: milder (Jen's 7 bytes), off (the "under 100?" jge nopped, the milder bytes back), the original
	menu_change(M_JEN_PLAGUE);
	CHECK_EQ(*(BYTE *)0x00448fc6, 0xEC); CHECK_EQ(*(BYTE *)0x00449037, 0x0A); CHECK_EQ(*(BYTE *)0x00449069, 0x3F); CHECK_EQ(*(BYTE *)0x00448f83, 0x0F);
	menu_change(M_JEN_PLAGUE);
	CHECK_EQ(*(BYTE *)0x00448fc6, 0xD8); CHECK_EQ(*(DWORD *)0x00448f83, 0x90909090); CHECK_EQ(*(WORD *)0x00448f87, 0x9090);
	menu_change(M_JEN_PLAGUE);
	CHECK_EQ(*(BYTE *)0x00448f83, 0x0F); CHECK_EQ(*(BYTE *)0x00448f84, 0x8D); CHECK_EQ(*(BYTE *)0x00449055, 0x19);
	// Resource multiplier 2x: Uncapped resources on with it; wood, iron and stone doubled, not the weapons (industry 2)
	menu_change(M_JEN_RESMUL);
	CHECK(g_jenUncap); CHECK_EQ(*(DWORD *)RES_CAP_AT, RES_UNCAPPED); CHECK_EQ(g_liveResMul, 2);
	int fr[16] = { 0 }; BYTE *ebp = (BYTE *)&fr[8];
	*(int *)(ebp + 0xc) = 0; *(int *)(ebp - 0xc) = 600; l2hd_res_prod(ebp); CHECK_EQ(*(int *)(ebp - 0xc), 1200);
	*(int *)(ebp + 0xc) = 2; *(int *)(ebp - 0xc) = 600; l2hd_res_prod(ebp); CHECK_EQ(*(int *)(ebp - 0xc), 600);
	*(int *)(ebp + 0xc) = 3; *(int *)(ebp - 0x18) = 700; l2hd_res_ui(ebp); CHECK_EQ(*(int *)(ebp - 0x18), 1400);
	menu_change(M_JEN_UNCAP);                   // capped again: 999 a season, the forecast too
	CHECK_EQ(*(DWORD *)RES_CAP_AT, RES_CAP);
	*(int *)(ebp + 0xc) = 1; *(int *)(ebp - 0xc) = 600; l2hd_res_prod(ebp); CHECK_EQ(*(int *)(ebp - 0xc), 999);
	*(int *)(ebp - 0x18) = 700; l2hd_res_ui(ebp); CHECK_EQ(*(int *)(ebp - 0x18), 999);
	// Population average: Jen's formula (worked by hand), and the hook's county
	CHECK_EQ(pop_births(0, 1000, 100, 50, 0, 0), 100);              // off
	CHECK_EQ(pop_births(1000, 1000, 100, 50, 0, 0), 32);            // 100 * 35000 / 111000: fewer
	CHECK_EQ(pop_births(1000, 100, 10, 0, 0, 0), 16);               // 10 * 48500 / 31000: more, at most +250
	CHECK_EQ(pop_births(1000, 100, 10, 0, 0, 5885), 10 + 5);        // more, but under 6000 people
	CHECK_EQ(pop_births(3000, 1000, 20, 0, 0, 0), 20 + 105);        // from 2000: (120000 - 15000 + 500) / 1000 more
	CHECK_EQ(pop_births(3000, 8000 / 1, 20, 0, 0, 0), 20);          // (more than 5999: left alone)
	CHECK_EQ(pop_births(1050, 1000, 100, 50, 0, 0), 100);           // (below 2000 only in hundreds)
	while (g_jenPop != 1000) menu_change(M_JEN_POP);
	CHECK_EQ(g_livePop, 1000);
	*(int *)0x0056d5dc = 3;
	BYTE *k2 = (BYTE *)0x0053f9d4 + 2 * 0x300;
	*(int *)k2 = 1000; *(int *)(k2 + 0xc) = 100; *(int *)(k2 + 0x10) = 50; *(int *)(k2 + 0x18) = 0; *(int *)(k2 + 0x1c) = 0;
	*(int *)(ebp - 4) = 2; l2hd_pop_step(ebp); CHECK_EQ(*(int *)(k2 + 0xc), 32);
	*(int *)(ebp - 4) = 4; *(int *)(k2 + 0xc) = 100; l2hd_pop_step(ebp); CHECK_EQ(*(int *)(k2 + 0xc), 100);   // (past the counties)
	// Army size 2000 and scaled icons (a fifth and two fifths of it); the garrisons follow the army size
	menu_change(M_JEN_ARMY);
	CHECK_EQ(*(DWORD *)0x004a97c4, 2000); CHECK_EQ(*(DWORD *)0x004aa1b5, 2000);
	CHECK_EQ(*(DWORD *)0x00465257, ICON_1);
	menu_change(M_JEN_ICONS);
	CHECK_EQ(*(DWORD *)0x00465257, 400); CHECK_EQ(*(DWORD *)0x004652b7, 800);
	menu_change(M_JEN_GARRISON); CHECK_EQ(CASTLE_CAP[0], 2000);
	// Unit stats and costs (archers): melee scaled as Jen's, the range in cells x 8, the sell price at once, a buy price
	CHECK_EQ(g_unitSel, 1);
	menu_change(M_JEN_STAT + US_MELEE);         // 5 -> 6: 6, 5, 4, 2
	WORD *mw = (WORD *)unit_at(1, US_MELEE);
	CHECK_EQ(mw[0], 6); CHECK_EQ(mw[1], 5); CHECK_EQ(mw[2], 4); CHECK_EQ(mw[3], 2);
	menu_change(M_JEN_STAT + US_RANGE);         // 15 -> 19 cells
	CHECK_EQ(*(DWORD *)0x004d97c4, 19 * 8);
	menu_change(M_JEN_STAT + US_SPEED);         // 50 -> 100: 2x slower
	CHECK_EQ(*(DWORD *)0x004d97c8, 100);
	char v[96]; BOOL rs;
	menu_value(M_JEN_STAT + US_SPEED, v, sizeof v, &rs); CHECK(!strcmp(v, "2x slower"));
	menu_change(M_JEN_COST + UC_SELL - UNIT_STATS);   // 16 -> 20
	CHECK_EQ(*(DWORD *)0x004d8938, 20); CHECK_EQ(*(int *)(0x005532a0 + 16 * 10), 20);
	CHECK_EQ(l2hd_buy_price(10), 0);            // the game's own buy price
	menu_change(M_JEN_COST + UC_BUY - UNIT_STATS);    // a price of its own: round twice the original sell price, half of it first
	CHECK_EQ(g_unitSet[1][UC_BUY], 16); CHECK_EQ(l2hd_buy_price(10), 16); CHECK_EQ(l2hd_buy_price(4), 0);
	char key[48]; snprintf(key, sizeof key, "ArcherMeleeStrength");
	CHECK_EQ((int)GetPrivateProfileIntA("Units", key, -1, g_iniPath), 6);
	g_unitSel = 6;                              // knights: a ranged defence in the code (imm8 at 0x482b56)
	menu_change(M_JEN_STAT + US_RDEF);          // 25 -> 31
	CHECK_EQ(*(BYTE *)0x00482b56, 31);
	menu_value(M_JEN_STAT + US_RANGE, v, sizeof v, &rs); CHECK(!strcmp(v, "-"));   // no ranged stats
	g_unitSel = 0;
	menu_change(M_JEN_COST + UC_SELL - UNIT_STATS); CHECK_EQ(g_unitSet[0][UC_SELL], -1);   // peasants: no price
	g_unitSel = 1;

	*(int *)0x00553030 = 1;                     // a multiplayer game: all the game's own, the settings kept
	jen_sync();
	CHECK(!l2hd_speed_free(GAME_SPEED)); CHECK_EQ(g_liveResMul, 1); CHECK_EQ(g_livePop, 0); CHECK_EQ(l2hd_buy_price(10), 0);
	CHECK_EQ(*(DWORD *)0x004a97c4, ARMY_SIZE); CHECK_EQ(*(DWORD *)0x00465257, ICON_1); CHECK_EQ(*(DWORD *)0x004652b7, ICON_2);
	CHECK_EQ(mw[0], 5); CHECK_EQ(mw[1], 4); CHECK_EQ(*(DWORD *)0x004d97c4, 120); CHECK_EQ(*(BYTE *)0x00482b56, 25);
	CHECK_EQ(*(DWORD *)0x004d8938, 16); CHECK_EQ(*(int *)(0x005532a0 + 16 * 10), 16);
	CHECK_EQ(CASTLE_CAP[0], 150);              // the garrisons: the original sizes
	CHECK(g_jenArmy == 2000 && g_jenIcons && g_unitSet[1][US_MELEE] == 6);
	*(int *)0x00553030 = 0;                     // back to single player: in again
	jen_sync();
	CHECK_EQ(*(DWORD *)0x004a97c4, 2000); CHECK_EQ(mw[0], 6); CHECK_EQ(*(int *)(0x005532a0 + 16 * 10), 20); CHECK_EQ(l2hd_buy_price(10), 16);
	CHECK_EQ(*(DWORD *)0x004652b7, 800);
	t_shipped_settings();                       // the ini's [Units]: read back; anything in range, attack speed one of Jen's three
	WritePrivateProfileStringA("Units", "KnightMeleeStrength", "500", g_iniPath);
	WritePrivateProfileStringA("Units", "CrossbowmanAttackSpeed", "60", g_iniPath);
	WritePrivateProfileStringA("Units", "PeasantSellPrice", "10", g_iniPath);
	t_fix_sites(); install_game_fixes();
	CHECK_EQ(g_unitSet[6][US_MELEE], 100); CHECK_EQ(g_unitSet[5][US_SPEED], 50); CHECK_EQ(g_unitSet[0][UC_SELL], -1);
	CHECK_EQ(*(WORD *)unit_at(6, US_MELEE), 100); CHECK_EQ(*(DWORD *)0x004d97dc, 50);
	t_shipped_settings();                       // data that isn't the game's: the unit mods stay out, the rest work
	*(BYTE *)0x00482b56 = 26;
	read_settings(); install_game_fixes();
	CHECK(!g_jeOk[JE_UNITS]); CHECK(g_jeOk[JE_ARMY]);
	menu_value(M_JEN_STAT + US_MELEE, v, sizeof v, &rs); CHECK(strstr(v, "unavailable") != NULL);
	t_shipped_settings();
}

/* Castle design: a STNFIEL2.PL8 shaped as the game's (30 records at their offsets, record r all r + 1, Expanded Keep's
 * cells as in the game's), the free-cell search played by t_free_spot: (45,26) for (44,27), (43,24) when it is free. */
static int t_spotFree;
static int __cdecl t_free_spot(int x, int y, int ring)
{
	(void)ring;
	if (x == 0x2b && y == 0x18) { if (!t_spotFree) return 0; *(int *)0x00569570 = 0x2b; *(int *)0x00569578 = 0x18; return 1; }
	*(int *)0x00569570 = 0x2d; *(int *)0x00569578 = 0x1a;
	return 1;
}
static void t_write_stnfiel2(void)
{
	long n = 0x1e8 + 30 * CD_REC;
	BYTE *b = calloc(1, n);
	for (int r = 0; r < 30; r++) {
		long at = 0x1e8 + r * CD_REC;
		b[r * 16 + 12] = at & 255; b[r * 16 + 13] = at >> 8 & 255; b[r * 16 + 14] = at >> 16 & 255;
		memset(b + at, r + 1, CD_REC);
	}
	BYTE *a = b + 0x1e8 + 28 * CD_REC, *c = a + CD_REC;
	a[26 * 80 + 44] = 0xdb; c[26 * 80 + 44] = 0x0a; a[27 * 80 + 44] = 0x35; c[27 * 80 + 44] = 0x0a;
	a[24 * 80 + 43] = 0xb9; c[24 * 80 + 43] = 0x28;
	t_write("STNFIEL2.PL8", b, n);
	free(b);
}
static int t_first(BYTE *buf) { return castle_design_read((const void *)0x0047c5e9, "stnfield.pl8", buf, CD_REC); }
static int t_second(BYTE *buf) { return castle_design_read((const void *)0x0047cfa2, "stnfield.pl8", buf, CD_REC); }
TEST(jen_castle_design)
{
	*(int *)0x00553030 = 0; *(int *)0x0057a0f0 = 0;  // a single player campaign game
	char p[MAX_PATH]; t_path(p, sizeof p, "STNFIEL2.PL8"); DeleteFileA(p);
	t_shipped_settings();
	char v[96]; BOOL rs; BYTE buf[CD_REC];
	CHECK(!cd_ok());                                  // no STNFIEL2.PL8: unavailable, the game reads its own
	menu_value(M_JEN_DESIGN + 4, v, sizeof v, &rs); CHECK(strstr(v, "Original") == v && strstr(v, "unavailable"));
	t_write_stnfiel2();
	t_shipped_settings();
	CHECK(cd_ok()); CHECK(g_cdSpotHooked);
	CHECK_EQ(*(BYTE *)FREE_SPOT_CALL, 0xE8);          // the troops' free-cell search calls h_free_spot
	CHECK_EQ(FREE_SPOT_CALL + 5 + *(DWORD *)(FREE_SPOT_CALL + 1), (DWORD)h_free_spot);
	for (int k = 0; k < 5; k++) { menu_value(M_JEN_DESIGN + k, v, sizeof v, &rs); CHECK(!strcmp(v, "Original")); CHECK(!strcmp(kMenuLabel[M_JEN_DESIGN + k], kCastleKind[k])); }
	*(int *)0x00553528 = 4;                           // a royal castle, as shipped: its own map
	CHECK_EQ(t_first(buf), -1); CHECK_EQ(t_second(buf), -1);
	menu_change(M_JEN_DESIGN + 4); menu_change(M_JEN_DESIGN + 4); menu_change(M_JEN_DESIGN + 4);   // Chambers, Island Castle, Goodrich
	menu_value(M_JEN_DESIGN + 4, v, sizeof v, &rs); CHECK(!strcmp(v, "Goodrich"));
	char s[32]; GetPrivateProfileStringA("Game", "RoyalCastleDesign", "", s, sizeof s, g_iniPath); CHECK(!strcmp(s, "Goodrich"));
	memset(buf, 0, sizeof buf);
	CHECK_EQ(t_first(buf), CD_REC); CHECK_EQ(buf[0], 2 * 12 + 1); CHECK_EQ(buf[CD_REC - 1], 2 * 12 + 1);   // Goodrich's ground
	CHECK_EQ(castle_design_read((const void *)0x0047cfa2, "C:\\x\\STNFIELD.PL8", buf, CD_REC), CD_REC);
	CHECK_EQ(buf[0], 2 * 12 + 2);                     // and its troops' layer
	CHECK_EQ(g_cdLastKind, 4); CHECK_EQ(g_cdLastLayout, 12);
	CHECK_EQ(t_second(buf), -1);                      // a second layer without a first: the game's
	CHECK_EQ(castle_design_read((const void *)0x00401234, "stnfield.pl8", buf, CD_REC), -1);   // another caller
	CHECK_EQ(castle_design_read((const void *)0x0047c5e9, "stnfield.pl8", buf, 1000), -1);     // another read
	*(int *)0x0057a0f0 = 1; CHECK_EQ(t_first(buf), -1); CHECK_EQ(g_cdLastKind, -1); *(int *)0x0057a0f0 = 0;   // a skirmish siege
	*(int *)0x00553030 = 1; CHECK_EQ(t_first(buf), -1); *(int *)0x00553030 = 0;                                 // multiplayer
	menu_change(M_JEN); CHECK_EQ(t_first(buf), -1); menu_change(M_JEN);                                         // Jen's mods off
	t_read_back(M_JEN_DESIGN + 4); CHECK_EQ(g_jenDesign[4], 3);   // the ini's name read at start-up
	*(int *)0x00553528 = 3; CHECK_EQ(t_first(buf), -1); t_second(buf);   // the other kinds keep their own
	menu_change(M_JEN_DESIGN + 4);                    // round to Original again
	menu_value(M_JEN_DESIGN + 4, v, sizeof v, &rs); CHECK(!strcmp(v, "Original"));
	*(int *)0x00553528 = 4; CHECK_EQ(t_first(buf), -1);

	free_spot_t was = g_freeSpot; g_freeSpot = t_free_spot;   // Expanded Keep on a Norman keep: (45,26) -> (43,24)
	menu_change(M_JEN_DESIGN + 2); menu_change(M_JEN_DESIGN + 2);
	menu_value(M_JEN_DESIGN + 2, v, sizeof v, &rs); CHECK(!strcmp(v, "Expanded Keep"));
	*(int *)0x00553528 = 2;
	CHECK_EQ(t_first(buf), CD_REC); CHECK_EQ(t_second(buf), CD_REC); CHECK_EQ(g_cdLastLayout, 14);
	BYTE *cell = (BYTE *)0x005440e0;
	enum { C1 = 26 * 80 + 44, C2 = 27 * 80 + 44, C3 = 24 * 80 + 43 };
	cell[C1 * 8 + 3] = 0xdb; cell[C1 * 8 + 1] = 0x10; cell[C2 * 8 + 3] = 0x35; cell[C2 * 8 + 1] = 0x10;
	cell[C3 * 8 + 3] = 0xb9; cell[C3 * 8 + 1] = 0; cell[C3 * 8 + 5] = 0;
	*(int *)0x005530ec = 0x11; *(int *)0x0053f014 = 5;
	t_spotFree = 1;
	CHECK_EQ(h_free_spot(0x2c, 0x1b, 1), 1); CHECK_EQ(*(int *)0x00569570, 0x2b); CHECK_EQ(*(int *)0x00569578, 0x18);
	t_spotFree = 0;                                   // (43,24) taken: where the game put it
	CHECK_EQ(h_free_spot(0x2c, 0x1b, 1), 1); CHECK_EQ(*(int *)0x00569570, 0x2d); CHECK_EQ(*(int *)0x00569578, 0x1a);
	t_spotFree = 1;
	h_free_spot(0x2c, 0x1b, 2); CHECK_EQ(*(int *)0x00569570, 0x2d);   // another ring, another cell: as the game's
	h_free_spot(0x2c, 0x1a, 1); CHECK_EQ(*(int *)0x00569570, 0x2d);
	cell[C1 * 8 + 3] = 0x01;                          // another field (a battle after it): as the game's
	h_free_spot(0x2c, 0x1b, 1); CHECK_EQ(*(int *)0x00569570, 0x2d);
	cell[C1 * 8 + 3] = 0xdb;
	menu_change(M_JEN_DESIGN + 2);                    // its own map again: as the game's
	CHECK_EQ(t_first(buf), -1);
	h_free_spot(0x2c, 0x1b, 1); CHECK_EQ(*(int *)0x00569570, 0x2d);
	g_freeSpot = was;
	memset(cell, 0, 80 * 80 * 8);
	*(int *)0x005530ec = 0; *(int *)0x0053f014 = 0; *(int *)0x00553528 = 0;
	DeleteFileA(p);
	t_shipped_settings();
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
