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
