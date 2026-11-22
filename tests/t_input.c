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
	g_off = 960; g_battleZoom = TRUE; g_wheelZoom = TRUE; g_mapSticky = TRUE; g_menuOpen = 0; g_fieldDrag = FALSE;
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
	g_menuOpen = 1;                      // the F10 panel
	CHECK(!wz_shown());
	g_menuOpen = 0;
	g_wheelZoom = FALSE;                 // switched off
	CHECK(!wz_shown());
	CHECK(!wz_wheel(MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(100, 200)));
	g_wheelZoom = TRUE;
	g_battleZoom = FALSE;                // battles at normal size: nothing to zoom
	CHECK(!wz_shown());
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
	window_shift(2, 0, TW_SW);   // the Town Center (screen 2)
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
	window_shift(2, 0, TW_SW);                           // up: moved, its box remembered
	int s = g_winShift;
	REQUIRE(s > 2);
	g_winLeftT = GetTickCount();                         // then closed with a right-click
	t_window_scene(TRUE, FALSE, FALSE);                  // the game's frame still holds it
	window_shift(0, 0, TW_SW);
	CHECK_EQ(g_winShift, s);
	CHECK_EQ(TW_PX(900 - s, 120), TW_WIN);
	t_window_scene(FALSE, FALSE, TRUE);                  // the map redrawn; only a flag waving
	window_shift(0, 0, TW_SW);
	CHECK_EQ(g_winShift, 0);
	CHECK_EQ(g_snapW, 0);
	CHECK_EQ(TW_PX(300, 45), 0x00ee00);                  // the flag where it is
	g_off = off; g_snapW = snap; g_winLockS = 0; g_winLeftT = 0;
}
