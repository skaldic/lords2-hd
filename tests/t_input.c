/* t_input.c - 1.5.1 and 1.5.2 fixes: the scroll keys, the zoomed battle's frame numbers and start-up hold, and the
 * windows moved to the middle of the wide map (the Town Center). Our own code. Included by tests.c. */

/* WASD and the arrow keys, each on its own switch; only the letters are kept from the game as typed characters. */
TEST(scroll_keys_letters_and_arrows)
{
	BOOL w = g_wasdOn, a = g_arrowsOn;
	g_wasdOn = TRUE; g_arrowsOn = TRUE;
	CHECK_EQ(wasd_bit('A'), 1); CHECK_EQ(wasd_bit('D'), 2); CHECK_EQ(wasd_bit('W'), 4); CHECK_EQ(wasd_bit('S'), 8);
	CHECK_EQ(wasd_bit(VK_LEFT), 1); CHECK_EQ(wasd_bit(VK_RIGHT), 2); CHECK_EQ(wasd_bit(VK_UP), 4); CHECK_EQ(wasd_bit(VK_DOWN), 8);
	CHECK_EQ(wasd_bit('Q'), 0);
	g_arrowsOn = FALSE;
	CHECK_EQ(wasd_bit(VK_LEFT), 0); CHECK_EQ(wasd_bit('A'), 1);
	g_wasdOn = FALSE; g_arrowsOn = TRUE;
	CHECK_EQ(wasd_bit('A'), 0); CHECK_EQ(wasd_bit(VK_UP), 4);
	// a typed character is only swallowed for the letters: VK_LEFT is 0x25, the code of '%'
	g_wasdOn = TRUE;
	CHECK_EQ(wasd_letter('%'), 0); CHECK_EQ(wasd_letter('A'), 1);
	g_wasdOn = w; g_arrowsOn = a;
}

/* Left in a battle: the field starts left of the game's x = 0, so the cursor goes to the field's real edge and the
 * edge scroll is told (g_battleLeftEdge). 1.5.1 put it at x = 0, the middle of the field: A didn't scroll left. */
TEST(scroll_left_in_a_battle_reaches_the_field_edge)
{
	int off = g_off; BOOL w = g_wasdOn; LONG menu = g_menuOpen;
	g_off = 960; g_wasdOn = TRUE; g_menuOpen = 0;
	*(int *)0x0052af94 = 3;          // the map mode
	*(BYTE *)0x004eac50 = 41;        // the battlefield
	*(int *)0x0057c8fc = 2;          // in a battle
	*(int *)0x004e6558 = -896;       // the view's left edge
	POINT p = { 300, 200 };
	g_wasd = 1; g_battleLeftEdge = FALSE;
	wasd_edges(&p, 600, 400);
	CHECK_EQ(p.x, -896);
	CHECK(g_battleLeftEdge);
	g_wasd = 2; p.x = 300; g_battleLeftEdge = FALSE;
	wasd_edges(&p, 600, 400);
	CHECK_EQ(p.x, 600);
	// on the county map x = 0 is the left edge, as before
	*(BYTE *)0x004eac50 = 0; *(int *)0x0057c8fc = 0;
	g_wasd = 1; p.x = 300;
	wasd_edges(&p, 600, 400);
	CHECK_EQ(p.x, 0);
	g_wasd = 0; g_off = off; g_wasdOn = w; g_menuOpen = menu; g_battleLeftEdge = FALSE;
}

/* Every frame copy needs a new number when Timing is off: the zoomed battle's presenter skips a read while the
 * number stays the same. 1.5.0 numbered copies only with Timing=1, and zoomed battles froze on their first picture. */
TEST(frame_copies_get_new_numbers_without_timing)
{
	BOOL t = g_timing;
	g_timing = FALSE;
	LONG a = pace_copy_seq(), b = pace_copy_seq();
	CHECK(b != a);
	g_timing = TRUE;   // (with Timing the frame's hash counts the changes: no change, the same number)
	a = pace_copy_seq(); b = pace_copy_seq();
	CHECK_EQ(a, b);
	g_timing = t;
}

/* The half-drawn first picture of a zoomed battle is held only while entering the battle, and never long. */
TEST(battle_half_drawn_hold_only_while_entering)
{
	int off = g_off, extra = g_extra;
	g_off = 960; g_extra = 0;
	static BYTE frame[1600 * 480];   // all black: nothing drawn yet
	PALETTEENTRY pal[256] = { { 0 } };
	int mapW = battle_map_w();
	CHECK(!battle_half_drawn(frame, 1600, 480, pal, mapW, FALSE));   // after the start: never held
	CHECK(battle_half_drawn(frame, 1600, 480, pal, mapW, TRUE));     // entering: held
	Sleep(550);
	CHECK(!battle_half_drawn(frame, 1600, 480, pal, mapW, TRUE));    // but at most half a second
	battle_half_drawn(frame, 1600, 480, pal, mapW, FALSE);           // (its clock back to the start)
	g_off = off; g_extra = extra;
}

/* ================================ the wheel zoom in battles (WheelZoom) */

/* Zooming keeps the field point under the cursor where it is, and the shown part stays inside the field. */
TEST(wheel_zoom_keeps_the_point_under_the_cursor)
{
	int fw = 1000, fh = 900;
	float x = 0, y = 0;
	wz_zoom_about(1.0f, 2.0f, 300, 600, fw, fh, &x, &y);
	CHECK(fabsf(x - 150.25f) < 0.01f); CHECK(fabsf(y - 300.25f) < 0.01f);
	int px = 300, py = 600;                    // the same screen point shows the same field point
	wz_map(2.0f, x, y, fw, fh, &px, &py);
	CHECK_EQ(px, 300); CHECK_EQ(py, 600);
	x = 700; y = 600;                          // near the field's corner: kept inside it
	wz_zoom_about(2.0f, 4.0f, 999, 899, fw, fh, &x, &y);
	CHECK(fabsf(x - 750) < 0.01f); CHECK(fabsf(y - 675) < 0.01f);
	wz_zoom_about(4.0f, 1.0f, 500, 400, fw, fh, &x, &y);   // back out: the whole field
	CHECK_EQ(x, 0.0f); CHECK_EQ(y, 0.0f);
	x = -50; y = 2000;
	wz_clamp(2.0f, fw, fh, &x, &y);
	CHECK_EQ(x, 0.0f); CHECK_EQ(y, 450.0f);
}

/* A zoomed-out battle on a 640x480 window (no wide columns): the picture is 1280x960, the field 1:1 left of the
 * side panel from row 48 down. */
static struct { HWND hwnd; int extra, showW, scale; BOOL integer; } t_wzKeep;
static void t_zoomed_battle(void)
{
	t_wzKeep.hwnd = g_hwnd; t_wzKeep.extra = g_extra; t_wzKeep.showW = g_showW; t_wzKeep.scale = g_scale; t_wzKeep.integer = g_integerScale;
	g_hwnd = NULL; g_extra = 0; g_showW = GAME_W; g_scale = SCALE_SMOOTH; g_integerScale = FALSE;
	g_off = 960; g_battleZoom = TRUE; g_wheelZoom = 2; g_mapSticky = TRUE;   // (up to 4x) g_menuOpen = 0; g_fieldDrag = FALSE;
	*(int *)0x0052af94 = 3;          // the map mode
	*(BYTE *)0x004eac50 = 41;        // the battlefield
	*(int *)0x0057c8fc = 2;          // in a battle
	InterlockedExchange(&g_wzLevel, 0); g_wzAcc = 0; g_wzX = g_wzY = 0;
}
static void t_zoomed_battle_end(void)
{
	InterlockedExchange(&g_wzLevel, 0); g_wzAcc = 0;
	g_off = 0; g_battleZoom = FALSE; g_mapSticky = FALSE;
	*(BYTE *)0x004eac50 = 0; *(int *)0x0057c8fc = 0;
	g_hwnd = t_wzKeep.hwnd; g_extra = t_wzKeep.extra; g_showW = t_wzKeep.showW; g_scale = t_wzKeep.scale; g_integerScale = t_wzKeep.integer;
}

/* The wheel over the field: a notch a step (x1.25, x1.5, x2 ...), the field point under the cursor still under it
 * for the game's mouse, and back out to the whole field. Half notches add up. */
TEST(wheel_zoom_clicks_reach_the_point_shown)
{
	t_zoomed_battle();
	int mapW = battle_map_w();
	int cx = 100, cy = 200, x = cx, y = cy;
	client_to_game(&x, &y);
	CHECK_EQ(x, PANEL_X - mapW + 200); CHECK_EQ(y, 24 + 352);   // 1x: the picture's field point 200, 352
	REQUIRE(wz_wheel(MAKEWPARAM(0, 3 * WHEEL_DELTA), MAKELPARAM(cx, cy)));   // three notches in: x2
	CHECK_EQ(g_wzLevel, 3);
	CHECK(wz_shown());
	int zx = cx, zy = cy;
	client_to_game(&zx, &zy);
	CHECK(abs(zx - x) <= 1 && abs(zy - y) <= 1);   // the point under the cursor didn't move
	zx = cx + 20; zy = cy;                         // 20 window px right: 40 picture px, 20 field px at x2
	client_to_game(&zx, &zy);
	CHECK(abs(zx - (x + 20)) <= 1);
	wz_wheel(MAKEWPARAM(0, -WHEEL_DELTA / 2), MAKELPARAM(cx, cy));   // half a notch: nothing yet
	CHECK_EQ(g_wzLevel, 3);
	wz_wheel(MAKEWPARAM(0, -WHEEL_DELTA / 2), MAKELPARAM(cx, cy));
	CHECK_EQ(g_wzLevel, 2);
	wz_wheel(MAKEWPARAM(0, 20 * WHEEL_DELTA), MAKELPARAM(cx, cy));   // far in: stops at x4
	CHECK_EQ(g_wzLevel, WZ_STEPS - 1);
	wz_wheel(MAKEWPARAM(0, -20 * WHEEL_DELTA), MAKELPARAM(cx, cy));  // far out: the whole field
	CHECK_EQ(g_wzLevel, 0);
	int bx = cx, by = cy;
	client_to_game(&bx, &by);
	CHECK_EQ(bx, x); CHECK_EQ(by, y);
	t_zoomed_battle_end();
}

/* Over anything but the bare battlefield the zoom isn't shown and the wheel is the game's; with the switch off too. */
TEST(wheel_zoom_only_on_the_bare_battlefield)
{
	t_zoomed_battle();
	wz_wheel(MAKEWPARAM(0, 3 * WHEEL_DELTA), MAKELPARAM(100, 200));
	REQUIRE(g_wzLevel == 3);
	*(BYTE *)0x004eac50 = 0x1e;          // a yes/no question over the battle
	CHECK(!wz_shown());
	CHECK(!wz_wheel(MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(100, 200)));
	int x = 100, y = 200;
	client_to_game(&x, &y);
	CHECK_EQ(x, PANEL_X - battle_map_w() + 200); CHECK_EQ(y, 24 + 352);   // clicks as at 1x
	*(BYTE *)0x004eac50 = 41;            // gone: the zoom is back
	CHECK(wz_shown());
	int zx = 300, zy = 300;
	client_to_game(&zx, &zy);
	*(BYTE *)0x004eac50 = 42;            // a mouse button held on the field (a selection box): still zoomed
	CHECK(wz_shown());
	int dx = 300, dy = 300;
	client_to_game(&dx, &dy);
	CHECK_EQ(dx, zx); CHECK_EQ(dy, zy);  // (mapped as on 41, not as at 1x)
	*(BYTE *)0x004eac50 = 41;
	g_menuOpen = 1;                      // the F10 panel
	CHECK(!wz_shown());
	g_menuOpen = 0;
	g_wheelZoom = 0;                     // switched off
	CHECK(!wz_shown());
	CHECK(!wz_wheel(MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(100, 200)));
	g_wheelZoom = 2;
	g_battleZoom = FALSE;                // battles at normal size: nothing to zoom
	CHECK(!wz_shown());
	t_zoomed_battle_end();
}

/* WheelZoom=1 (the default) stops at x2, the game's normal size; 2 goes on to x4. The F10 switch goes 2x -> 4x -> off
 * -> 2x, and from 4x down to 2x a part zoomed in further is shown at x2. */
TEST(wheel_zoom_stops_at_the_setting)
{
	t_zoomed_battle();
	g_wheelZoom = 1;
	wz_wheel(MAKEWPARAM(0, 20 * WHEEL_DELTA), MAKELPARAM(100, 200));
	CHECK_EQ(kWzStep[g_wzLevel], 2.0f);
	g_wheelZoom = 2;
	wz_wheel(MAKEWPARAM(0, 20 * WHEEL_DELTA), MAKELPARAM(100, 200));
	CHECK_EQ(kWzStep[g_wzLevel], 4.0f);
	menu_change(M_WHEEL);                // 4x -> off
	CHECK_EQ(g_wheelZoom, 0);
	CHECK(!wz_shown());
	menu_change(M_WHEEL);                // off -> 2x
	CHECK_EQ(g_wheelZoom, 1);
	CHECK_EQ(kWzStep[g_wzLevel], 2.0f);
	CHECK(wz_shown());
	menu_change(M_WHEEL);                // 2x -> 4x
	CHECK_EQ(g_wheelZoom, 2);
	t_zoomed_battle_end();
}

/* A new battle starts with the whole field. */
TEST(wheel_zoom_new_battle_starts_whole)
{
	t_zoomed_battle();
	*(int *)0x0057c998 = 500; wz_battle_check();
	wz_wheel(MAKEWPARAM(0, 2 * WHEEL_DELTA), MAKELPARAM(100, 200));
	*(int *)0x0057c998 = 900; wz_battle_check();
	CHECK_EQ(g_wzLevel, 2);              // the same battle going on
	*(int *)0x0057c998 = 3; wz_battle_check();
	CHECK_EQ(g_wzLevel, 0);              // the next one
	t_zoomed_battle_end();
}

/* ================================ the county map zoomed out (CountyZoom) */

/* The wide county map, nothing over it. */
static void t_county_map(void)
{
	g_off = 960; g_extra = 208; g_battleZoom = TRUE; g_countyZoom = TRUE; g_mapSticky = TRUE; g_menuOpen = 0;
	g_wideScreen = 1; g_scale = SCALE_SMOOTH;
	*(int *)0x0052af94 = 3;          // the map mode
	*(BYTE *)0x004eac50 = 0;         // the county map's screen
	*(int *)0x0057c8fc = 0;          // not in a battle
	*(int *)0x0057cb18 = 0;          // the map at its normal scale
	InterlockedExchange(&g_czOut, 0);
}
static void t_county_map_end(void)
{
	InterlockedExchange(&g_czOut, 0); g_czShowing = 0;
	g_off = 0; g_extra = 0; g_battleZoom = FALSE; g_countyZoom = TRUE; g_mapSticky = FALSE; g_wideScreen = 0;
	*(int *)0x0052af94 = 0;
}

/* The wheel back (down) shows it zoomed out, forward the normal view; half notches add up. Each zoom out starts a
 * new picture (an older one is not shown). */
TEST(county_zoom_wheel_out_and_back)
{
	t_county_map();
	LONG gen = g_czGen;
	CHECK(cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA / 2), 0));   // half a notch: ours, nothing yet
	CHECK_EQ(g_czOut, 0);
	CHECK(cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA / 2), 0));
	CHECK_EQ(g_czOut, 1);
	CHECK_EQ(g_czGen, gen + 1);
	cz_wheel(MAKEWPARAM(0, -3 * WHEEL_DELTA), 0);           // further out: as far as it goes
	CHECK_EQ(g_czOut, 1);
	CHECK_EQ(g_czGen, gen + 1);
	CHECK(cz_wheel(MAKEWPARAM(0, WHEEL_DELTA), 0));
	CHECK_EQ(g_czOut, 0);
	cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0);
	CHECK_EQ(g_czGen, gen + 2);
	t_county_map_end();
}

/* Only on the bare county map at its normal scale, with BattleZoom (the frame as wide as the doubled field) and the
 * switch on; the F10 switch turned off shows the normal view at once. */
TEST(county_zoom_only_on_the_bare_county_map)
{
	t_county_map();
	*(BYTE *)0x004eac50 = 0x1e;          // a yes/no question over the map
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	*(BYTE *)0x004eac50 = 0;
	*(int *)0x0057cb18 = 2;              // the kingdom overview
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	*(int *)0x0057cb18 = 0;
	g_menuOpen = 1;                      // the F10 panel
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	g_menuOpen = 0;
	g_battleZoom = FALSE;
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	g_battleZoom = TRUE;
	g_off = 0;                           // 4:3
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	g_off = 960;
	g_scale = SCALE_SHARP;               // square pixels: the picture isn't built, so the wheel stays the game's
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	g_scale = SCALE_SMOOTH;
	CHECK_EQ(g_czOut, 0);
	REQUIRE(cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	REQUIRE(g_czOut == 1);
	menu_change(M_COUNTY);               // off in the F10 panel: the normal view, and the wheel is the game's
	CHECK(!g_countyZoom);
	CHECK_EQ(g_czOut, 0);
	CHECK(!cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	CHECK_EQ(g_czOut, 0);
	menu_change(M_COUNTY);
	CHECK(g_countyZoom);
	CHECK(cz_wheel(MAKEWPARAM(0, -WHEEL_DELTA), 0));
	CHECK_EQ(g_czOut, 1);
	t_county_map_end();
}

/* A click on the zoomed-out map: the game's pick runs with the picture's view and the mouse's point in it, and what
 * it leaves is turned back into the game's own view; the game's view and mouse are as before. */
static struct { int cols, firstX, firstCol, row, firstY, rows, mx, my; } t_czSaw;
static int __cdecl t_cz_pick_stub(int arg)
{
	t_czSaw.cols = MAP_COLUMNS; t_czSaw.firstX = MAP_FIRST_X; t_czSaw.firstCol = MAP_FIRST_COL;
	t_czSaw.row = *(int *)0x005651b8; t_czSaw.firstY = *(int *)0x00553244; t_czSaw.rows = *(int *)0x0056d67c;
	t_czSaw.mx = *(int *)0x004e65fc; t_czSaw.my = *(int *)0x004e65f4;
	PICKED_COL = 7; *(int *)0x005681f4 = 9;     // column 7, half-row 9 of the picture's view
	*(int *)0x00553ff8 = 600; *(int *)0x00553fe8 = 400;
	*(int *)0x00568974 = 42;                    // the cell's record
	return arg + 1;
}
TEST(county_zoom_click_picks_the_cell_shown)
{
	t_county_map();
	static BYTE pix[1];
	cz_buf v = { 0 };
	v.pix = pix; v.gen = g_czGen; v.nc = 23; v.nr = 66; v.fc = 10; v.fr = 20; v.fx = -30; v.fy = -5; v.dx = 500; v.dy = 228;
	cz_buf *keepShown = g_czShown;
	pick_cell_t keepPick = o_pick_cell;
	g_czShown = &v; o_pick_cell = t_cz_pick_stub;
	InterlockedExchange(&g_czOut, 1); g_czShowing = 1; g_czMouse.on = FALSE;
	MAP_COLUMNS = 12; MAP_FIRST_X = -10; MAP_FIRST_COL = 15; *(int *)0x005651b8 = 30; *(int *)0x00553244 = 7; *(int *)0x0056d67c = 34;
	*(int *)0x004e65fc = 100; *(int *)0x004e65f4 = 200;   // the game's mouse
	int r = h_pick_cell(1);
	CHECK_EQ(r, 2);
	CHECK_EQ(t_czSaw.cols, 23); CHECK_EQ(t_czSaw.firstX, -30); CHECK_EQ(t_czSaw.firstCol, 10);
	CHECK_EQ(t_czSaw.row, 20); CHECK_EQ(t_czSaw.firstY, -5); CHECK_EQ(t_czSaw.rows, 66);
	CHECK_EQ(t_czSaw.mx, 2 * (100 + 208) + 1); CHECK_EQ(t_czSaw.my, 2 * 200 - 24 + 1);   // its point in the picture
	CHECK_EQ(MAP_COLUMNS, 12); CHECK_EQ(MAP_FIRST_X, -10); CHECK_EQ(MAP_FIRST_COL, 15);   // the game's view as it was
	CHECK_EQ(*(int *)0x005651b8, 30); CHECK_EQ(*(int *)0x00553244, 7); CHECK_EQ(*(int *)0x0056d67c, 34);
	CHECK_EQ(*(int *)0x004e65fc, 100); CHECK_EQ(*(int *)0x004e65f4, 200);
	CHECK_EQ(MAP_FIRST_COL + PICKED_COL, 10 + 7);                  // the same cell, from the game's view
	CHECK_EQ(*(int *)0x005651b8 + *(int *)0x005681f4, 20 + 9);
	CHECK_EQ(*(int *)0x00553ff8, 100); CHECK_EQ(*(int *)0x00553fe8, 172);
	CHECK_EQ(*(int *)0x00568974, 42);
	// the window's precise point (client_to_game) when it is for the same game point
	g_czMouse.on = TRUE; g_czMouse.x = 700; g_czMouse.y = 300; g_czMouse.gx = 100; g_czMouse.gy = 200;
	h_pick_cell(0);
	CHECK_EQ(t_czSaw.mx, 700); CHECK_EQ(t_czSaw.my, 300);
	g_czMouse.gx = 101;                  // a stale one: from the game's mouse
	h_pick_cell(0);
	CHECK_EQ(t_czSaw.mx, 2 * (100 + 208) + 1);
	// over the side panel, the normal view, or an old picture: the game's own pick
	t_czSaw.mx = 0;
	*(int *)0x004e65fc = PANEL_X + 10;
	h_pick_cell(0);
	CHECK_EQ(t_czSaw.mx, PANEL_X + 10);
	*(int *)0x004e65fc = 100;
	g_czShowing = 0;
	h_pick_cell(0);
	CHECK_EQ(t_czSaw.mx, 100);
	g_czShowing = 1; v.gen = g_czGen - 1;
	h_pick_cell(0);
	CHECK_EQ(t_czSaw.mx, 100);
	g_czShown = keepShown; o_pick_cell = keepPick;
	t_county_map_end();
}

/* The zoomed picture: the field enlarged from the shown part, the top bar and the side panel untouched. A field
 * of 400x300 picture px, its left half one colour and its right half another, under a bar of 48 rows. */
TEST(wheel_zoom_picture_enlarges_only_the_field)
{
	t_zoomed_battle();
	enum { W = 500, H = 348, FW = 400, A = 0x112233, B = 0x445566, BAR = 0x778899, PANEL = 0xaabbcc };
	static DWORD pic[W * H];
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
		pic[y * W + x] = y < 48 ? BAR : x >= FW ? PANEL : x < FW / 2 ? A : B;
	InterlockedExchange(&g_wzLevel, 3); g_wzX = 0; g_wzY = 0;   // x2 from the field's top-left: its left half
	const DWORD *out = wz_apply(pic, W, H, 1, FW);
	REQUIRE(out != pic);
	CHECK_EQ(out[10 * W + 250], (DWORD)BAR);
	CHECK_EQ(out[100 * W + 10], (DWORD)A); CHECK_EQ(out[100 * W + 390], (DWORD)A);   // all of it the left half
	CHECK_EQ(out[300 * W + 450], (DWORD)PANEL);
	g_wzX = 200;                                                // its right half
	out = wz_apply(pic, W, H, 1, FW);
	CHECK_EQ(out[100 * W + 10], (DWORD)B); CHECK_EQ(out[340 * W + 399], (DWORD)B);
	g_wzX = 900; g_wzY = -40;                                   // asked beyond the field: kept inside it
	out = wz_apply(pic, W, H, 1, FW);
	CHECK_EQ(out[60 * W + 5], (DWORD)B);
	t_zoomed_battle_end();
}

/* Zoomed in, a screen edge or a scroll key first slides the shown part across the field; the game scrolls the
 * battlefield only once the part is at the field's edge on that side. Before, the part stayed where it was and the
 * field beside it couldn't be reached where the game's view was already at the end of the battlefield. */
/* The battle minimap's frame: zoomed in, the marks go round the shown part; the game's frame round the whole field
 * and the part's last frame get their ground back (bit 0, for the minimap's call only). A battle of 60 x 50 cells, the
 * view's 43 x 29 cells from cell 10, 5. */
enum { T_CW = 60, T_CH = 50, T_ST = 8 };
static BYTE t_cells[T_CW * T_CH * T_ST], t_seen[T_CW * T_CH];   // t_seen: byte 2 of each cell as the minimap saw it
static void __cdecl t_mini_rows(int row0, int n)
{
	for (int row = row0; row < row0 + n; row++) for (int c = 0; c < T_CW; c++) {
		t_seen[row * T_CW + c] = t_cells[(row * T_CW + c) * T_ST + 2];
		t_cells[(row * T_CW + c) * T_ST + 2] &= ~2;   // (as the game: a frame drawn, its mark taken off)
	}
}
static BYTE t_flag(int c, int row) { return t_cells[(row * T_CW + c) * T_ST + 2]; }
static void t_game_frame(void)   // the game's battlefield marks the edge of the view it drew, every frame
{
	for (int c = 10; c < 53; c++) { t_cells[(5 * T_CW + c) * T_ST + 2] |= 2; t_cells[(33 * T_CW + c) * T_ST + 2] |= 2; }
	for (int row = 5; row < 34; row++) { t_cells[(row * T_CW + 10) * T_ST + 2] |= 2; t_cells[(row * T_CW + 52) * T_ST + 2] |= 2; }
}
static void t_frame(void) { t_game_frame(); h_mini_rows(0, T_CH); }
TEST(wheel_zoom_minimap_frame_round_the_shown_part)
{
	t_zoomed_battle();
	mini_rows_t keep = o_mini_rows;
	o_mini_rows = t_mini_rows;
	memset(t_cells, 0, sizeof t_cells);
	*(BYTE **)0x004e6574 = t_cells; *(int *)0x004e5d50 = T_ST; *(int *)0x004e6578 = T_CW; *(int *)0x004e6570 = T_CH;
	*(int *)0x004e5d5c = 43; *(int *)0x004e5d4c = 29; *(int *)0x004e657c = 10; *(int *)0x004e6580 = 5;
	int fw = battle_map_w();
	*(int *)0x004e6558 = PANEL_X - fw; *(int *)0x004e6554 = 24;   // the view's first cell at the field's top-left
	t_frame();                                      // not zoomed: the game's frame as it is
	CHECK_EQ(t_seen[5 * T_CW + 52] & 3, 2);
	InterlockedExchange(&g_wzLevel, 3); g_wzX = 0; g_wzY = 0;  // x2 at the field's top-left
	int r[4];
	wz_mini_rect(r);
	CHECK_EQ(r[0], 10); CHECK_EQ(r[1], 5);
	CHECK(abs((r[2] - r[0] + 1) - (fw / 2 + 31) / 32) <= 1);   // half the field's width in cells
	CHECK(abs((r[3] - r[1] + 1) - ((2 * GAME_H - 48) / 2 + 31) / 32) <= 1);
	t_frame();
	CHECK_EQ(t_seen[5 * T_CW + 10] & 2, 2);                    // the part's corner (also the field's): framed
	CHECK_EQ(t_seen[r[3] * T_CW + r[2]] & 2, 2);               // its far corner, inside the field: framed
	CHECK_EQ(t_seen[5 * T_CW + 52] & 3, 1);                    // the field's far corner: its ground drawn, no frame
	CHECK_EQ(t_flag(52, 5) & 3, 0);                            // (bit 0 only for the minimap's call)
	CHECK_EQ(t_seen[20 * T_CW + 30] & 3, 0);                   // inside the part: nothing
	g_wzX = 300;                                               // the part moves right: its last frame gets its ground
	t_frame();
	CHECK_EQ(t_seen[r[3] * T_CW + 10] & 3, 1);
	int r2[4];
	wz_mini_rect(r2);
	REQUIRE(r2[0] > r[0]);
	InterlockedExchange(&g_wzLevel, 0);                        // zoomed out: the part's frame gets its ground, then as the game
	t_frame();
	CHECK_EQ(t_seen[r2[3] * T_CW + r2[2]] & 3, 1);             // (it stayed beside the game's frame at first)
	memset(t_seen, 0, sizeof t_seen);
	t_frame();
	CHECK_EQ(t_seen[20 * T_CW + 30] & 3, 0);
	*(BYTE **)0x004e6574 = NULL;
	o_mini_rows = keep;
	t_zoomed_battle_end();
}

static int t_due = 1;   // the game's scroll clock, played by the test
static int __cdecl t_scroll_due(void) { return t_due; }
TEST(wheel_zoom_edges_slide_the_shown_part_first)
{
	t_zoomed_battle();
	int (__cdecl *due)(void) = wz_scroll_due;
	wz_scroll_due = t_scroll_due; t_due = 1;
	int fw = battle_map_w();
	float maxX = fw - fw / 4.0f;
	InterlockedExchange(&g_wzLevel, WZ_STEPS - 1); g_wzX = 0; g_wzY = 0;   // x4, at the field's top-left
	*(int *)0x0055ce70 = 10; *(int *)0x005678a0 = 5;            // the game's view (col, row)
	CHECK_EQ(wz_pan(1), 1);                                      // left: already there, the game scrolls
	CHECK_EQ(g_wzHeld, 0);
	CHECK_EQ(wz_pan(2), 0);                                      // right: the part slides, the game doesn't scroll
	CHECK_EQ(g_wzHeld, 2);
	wz_pan_frame(0, 0);
	CHECK(fabsf(g_wzX - 8) < 0.01f);                             // a cell on the screen a frame: 32 / 4 field px
	t_due = 0;                                                   // between the game's scroll steps: still
	wz_pan_frame(0, 0);
	CHECK(fabsf(g_wzX - 8) < 0.01f);
	t_due = 1;
	CHECK_EQ(wz_pan(2 | 8), 0);                                  // diagonal
	wz_pan_frame(0, 0);
	CHECK(fabsf(g_wzX - 16) < 0.01f); CHECK(fabsf(g_wzY - 8) < 0.01f);
	// at the right edge the game scrolls a cell; the part goes back a cell less a frame's slide, so the field goes on
	// at a cell on the screen a frame and the game scrolls again every 4th frame (it rushed past at x4 before)
	g_wzX = maxX; g_wzY = 0;
	CHECK_EQ(wz_pan(2), 2);
	*(int *)0x0055ce70 = 11;
	wz_pan_frame(1, 0);
	CHECK(fabsf(g_wzX - (maxX - 24)) < 0.01f);
	CHECK_EQ(g_wzPendView, 11 + 5 * 256);                        // frames of the old view: the part where it was
	CHECK(fabsf(g_wzPendX - 32) < 0.01f);
	for (int f = 0; f < 3; f++) { CHECK_EQ(wz_pan(2), 0); wz_pan_frame(0, 0); }
	CHECK(fabsf(g_wzX - maxX) < 0.01f);
	CHECK_EQ(wz_pan(2), 2);                                      // the 4th frame: the game's scroll again
	enum { W = 500, H = 348 };                                   // a frame of the new view reaches the screen: caught up
	static DWORD pic[W * H];
	InterlockedExchange(&g_primaryView, 11 + 5 * 256);
	wz_apply(pic, W, H, 1, 400);
	CHECK_EQ(g_wzPendView, -1); CHECK_EQ(g_wzPendX, 0.0f);
	InterlockedExchange(&g_primaryView, -1);
	// the scroll keys: a key panning the part isn't passed on as an edge
	BOOL w = g_wasdOn; g_wasdOn = TRUE;
	POINT p = { 300, 200 };
	g_wasd = 1; wz_pan(1);
	REQUIRE(g_wzHeld == 1);
	wasd_edges(&p, 600, 400);
	CHECK_EQ(p.x, 300);
	g_wasd = 2; wz_pan(2);                                       // at the right edge: the game's scroll again
	wasd_edges(&p, 600, 400);
	CHECK_EQ(p.x, 600);
	g_wasd = 0; g_wasdOn = w;
	InterlockedExchange(&g_wzLevel, 0);                          // not zoomed: every edge is the game's
	CHECK_EQ(wz_pan(15), 15);
	*(int *)0x0055ce70 = 0; *(int *)0x005678a0 = 0;
	wz_scroll_due = due;
	t_zoomed_battle_end();
}

/* Zoomed in, a minimap click (or a group's number key) shows the spot jumped to: the game puts it in the middle of
 * its view, and the shown part goes round it. Before, the part kept its place in the field and showed somewhere
 * else. A click that sends the selected units there moves nothing. */
static int t_clickCol, t_clickRow, t_clickOrders;
static void __cdecl t_game_clamp(void)   // the game's scroll limit (FUN_0047ed34)
{
	if (BATTLE_COL < 0) BATTLE_COL = 0;
	if (BATTLE_ROW < 0) BATTLE_ROW = 0;
	if (BATTLE_COL > 0x41) BATTLE_COL = 0x41;
	if (BATTLE_ROW > 0x42) BATTLE_ROW = 0x42;
}
static int __cdecl t_mini_click(void)   // the game's minimap click (FUN_00432443), with the jump moved to the middle
{
	if (t_clickOrders) return 1;
	BATTLE_COL = t_clickCol - (7 + BATTLE_MORE_COLS / 2);
	BATTLE_ROW = t_clickRow - (7 + BATTLE_MORE_ROWS / 2);
	h_battle_clamp();
	return 1;
}
TEST(wheel_zoom_minimap_click_shows_the_spot)
{
	t_zoomed_battle();
	mini_click_t keepClick = o_mini_click;
	battle_clamp_t keepClamp = o_battle_clamp;
	int k = g_battleK, r = g_battleR;
	o_mini_click = t_mini_click; o_battle_clamp = t_game_clamp;
	g_battleK = BATTLE_MORE_COLS; g_battleR = BATTLE_MORE_ROWS;
	int fw = battle_map_w(), fh = 2 * GAME_H - 48;
	*(int *)0x004e6558 = PANEL_X - fw; *(int *)0x004e6554 = 24;   // the view's first cell at the field's top-left
	InterlockedExchange(&g_wzLevel, WZ_STEPS - 1); g_wzX = 0; g_wzY = 0;   // x4, at the field's top-left
	t_clickCol = 40; t_clickRow = 40; t_clickOrders = 0;
	CHECK_EQ(h_mini_click(), 1);
	float cx = (40 - BATTLE_COL) * 32 + 16, cy = (40 - BATTLE_ROW) * 32 + 16;   // the cell's middle in the field
	CHECK(fabsf(g_wzX + fw / 8.0f - cx) < 0.5f); CHECK(fabsf(g_wzY + fh / 8.0f - cy) < 0.5f);   // the part round it
	CHECK_EQ(g_wzPendView, BATTLE_VIEW_NOW());                     // the old view's frames: the part where it was
	CHECK(fabsf(g_wzPendX + g_wzX) < 0.01f);
	CHECK_EQ(g_wzJump, -1);
	t_clickCol = 79; t_clickRow = 79;                              // the battlefield's far corner: its view stops short
	h_mini_click();
	CHECK_EQ(BATTLE_COL, 0x41 - BATTLE_MORE_COLS);
	cx = (79 - BATTLE_COL) * 32; cy = (79 - BATTLE_ROW) * 32;     // the cell's top-left (the last row is half drawn)
	CHECK(cx >= g_wzX && cx < g_wzX + fw / 4.0f); CHECK(cy >= g_wzY && cy < g_wzY + fh / 4.0f);   // still shown
	float x = g_wzX, y = g_wzY;
	t_clickOrders = 1; t_clickCol = 10; t_clickRow = 10;           // the units sent there: nothing moves
	h_mini_click();
	CHECK_EQ(g_wzX, x); CHECK_EQ(g_wzY, y);
	h_battle_clamp();                                              // the game's scroll limit every frame: nothing
	CHECK_EQ(g_wzX, x);
	InterlockedExchange(&g_wzLevel, 0);                            // not zoomed: nothing to move
	t_clickOrders = 0;
	h_mini_click();
	CHECK_EQ(g_wzX, x);
	InterlockedExchange(&g_wzPendView, -1); g_wzPendX = g_wzPendY = 0;
	BATTLE_COL = 0; BATTLE_ROW = 0;
	o_mini_click = keepClick; o_battle_clamp = keepClamp; g_battleK = k; g_battleR = r;
	t_zoomed_battle_end();
}

/* A made-up wide map 1100 columns wide (game x 0 at column 600): a window at columns 900..999, rows 100..299, and a
 * wider scroll over it (columns 850..1049, rows 150..199). The 2x picture holds each map pixel's own column number,
 * so a map pixel moved to the wrong place shows. */
enum { TW_SW = 1100, TW_WIN = 0xab0000, TW_SCROLL = 0xcd0000 };
static void t_window_scene(BOOL window, BOOL scroll, BOOL flag)
{
	memset(g_frame8, 0, TW_SW * GAME_H); memset(g_snap8, 0, TW_SW * GAME_H);
	for (int y = 0; y < 2 * GAME_H; y++) for (int x = 0; x < 2 * TW_SW; x++)
		g_frame2x[y * 2 * TW_SW + x] = g_snap2x[y * 2 * TW_SW + x] = (DWORD)x;
	for (int y = 0; y < GAME_H; y++) for (int x = 0; x < TW_SW; x++) {
		DWORD v = 0;
		if (window && y >= 100 && y < 300 && x >= 900 && x < 1000) v = TW_WIN;
		if (scroll && y >= 150 && y < 200 && x >= 850 && x < 1050) v = TW_SCROLL;
		if (flag && y >= 40 && y < 52 && x >= 300 && x < 312) v = 0x00ee00;   // a flag waving elsewhere on the map
		if (!v) continue;
		g_frame8[y * TW_SW + x] = (BYTE)(v >> 16 | v >> 8);
		for (int r = 0; r < 2; r++) for (int c = 0; c < 2; c++) g_frame2x[(2 * y + r) * 2 * TW_SW + 2 * x + c] = v;
	}
}
#define TW_PX(x, y) g_frame2x[(2 * (y)) * 2 * TW_SW + 2 * (x)]

TEST(moved_window_moves_only_its_own_rows)
{
	int off = g_off, snap = g_snapW;
	g_off = 600; g_snapW = TW_SW; g_winLockS = 0; g_winLeftT = 0;
	t_window_scene(TRUE, TRUE, FALSE);
	window_shift(2, 0, TW_SW, NULL);   // the Town Center (screen 2)
	int s = g_winShift;
	REQUIRE(s > 2);
	CHECK_EQ(s, (850 + 1050) / 2 - (600 + 478) / 2);    // the box's middle onto the middle of the map on screen
	CHECK_EQ(TW_PX(900 - s, 120), TW_WIN);              // the window, moved
	CHECK_EQ(TW_PX(850 - s, 170), TW_SCROLL);           // the scroll, moved with it
	// a row with only the window: the map left of the window stays where it is (1.5.1 moved it with the window)
	CHECK_EQ(TW_PX(850 - s, 120), (DWORD)(2 * (850 - s)));
	CHECK_EQ(TW_PX(1020, 120), 2040);                   // and right of it the map is the map
	CHECK_EQ(TW_PX(950, 120), 1900);                    // where the window was: the map from before
	g_off = off; g_snapW = snap; g_winLockS = 0; InterlockedExchange(&g_winShift, 0);
}

/* Closed back to the map: moved on while the frame still holds the window, and stopped at once when what differs is
 * no longer the window (flags waving elsewhere were moved and cut in pieces). */
TEST(moved_window_after_closing_only_while_still_there)
{
	int off = g_off, snap = g_snapW;
	g_off = 600; g_snapW = TW_SW; g_winLockS = 0; g_winLeftT = 0;
	t_window_scene(TRUE, FALSE, FALSE);
	window_shift(2, 0, TW_SW, NULL);                           // up: moved, its box remembered
	int s = g_winShift;
	REQUIRE(s > 2);
	g_winLeftT = GetTickCount();                         // then closed with a right-click
	t_window_scene(TRUE, FALSE, FALSE);                  // the game's frame still holds it
	window_shift(0, 0, TW_SW, NULL);
	CHECK_EQ(g_winShift, s);
	CHECK_EQ(TW_PX(900 - s, 120), TW_WIN);
	t_window_scene(FALSE, FALSE, TRUE);                  // the map redrawn; only a flag waving
	window_shift(0, 0, TW_SW, NULL);
	CHECK_EQ(g_winShift, 0);
	CHECK_EQ(g_snapW, 0);
	CHECK_EQ(TW_PX(300, 45), 0x00ee00);                  // the flag where it is
	g_off = off; g_snapW = snap; g_winLockS = 0; g_winLeftT = 0;
}

/* The county map zoomed out with a window over it (the Town Center, a field's box): the window as the game drew it,
 * moved to the middle as ever, and the zoomed-out map round it and where it was - not the kept map at normal size. */
TEST(county_zoom_window_keeps_the_zoomed_map_round_it)
{
	int off = g_off, snap = g_snapW; BOOL tiles = g_showTiles; DWORD lut7 = g_lut[7]; Surface *back = g_back;
	static Surface sb;
	sb.pitch = 2300; g_back = &sb;
	cz_buf v = { 0 };
	v.w = 2 * (600 + PANEL_X); v.h = 2 * (GAME_H - 24); v.sw = TW_SW;    // the field: frame columns 0 .. game x PANEL_X
	REQUIRE((v.pix = malloc((size_t)sb.pitch * (CZ_M + 24 + v.h + CZ_BELOW))) != NULL);
	memset(v.pix, 7, (size_t)sb.pitch * (CZ_M + 24 + v.h + CZ_BELOW));
	g_lut[7] = 0x123456; g_showTiles = FALSE;
	g_off = 600; g_snapW = TW_SW; g_winLockS = 0; g_winLeftT = 0;
	t_window_scene(TRUE, FALSE, FALSE);
	REQUIRE(cz_win_spans(0, TW_SW));
	CHECK_EQ(g_czWinA[120], 900); CHECK_EQ(g_czWinB[120], 1000);
	CHECK(g_czWinA[50] >= g_czWinB[50]);                 // a row without it
	cz_field(&v, TW_SW, TRUE);
	CHECK_EQ(TW_PX(950, 120), TW_WIN);                   // the window left as drawn
	CHECK_EQ(TW_PX(500, 120), 0x123456);                 // the zoomed-out map beside it
	CHECK_EQ(TW_PX(950, 400), 0x123456);                 // and below it
	window_shift(2, 0, TW_SW, &v);
	int s = g_winShift;
	REQUIRE(s > 2);
	CHECK_EQ(TW_PX(900 - s, 120), TW_WIN);               // moved to the middle
	CHECK_EQ(TW_PX(990, 120), 0x123456);                 // where it was: the zoomed-out map (not the kept map's 1980)
	// the map behind it moved (nearly everything differs): no window found, the normal view
	g_winLockS = 0; InterlockedExchange(&g_winShift, 0);
	memset(g_frame8, 5, TW_SW * GAME_H);
	CHECK(!cz_win_spans(0, TW_SW));
	free(v.pix);
	g_back = back; g_lut[7] = lut7; g_showTiles = tiles;
	g_off = off; g_snapW = snap; g_winLockS = 0; g_winLeftT = 0;
}
