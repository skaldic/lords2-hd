/* game_mem.h - helpers to build a made-up battle and campaign in the fake game memory (see host.c). Our own code.
 * Included by tests.c after the patch's source, so FIG, FIG_S and BCELL are the patch's own macros.
 *
 * Battle: 80 figures FIG(1..80) of 0x1b0 bytes; +0x2c side (0 = empty), +0x12 type (6 knight, 7+ siege), +0x31 state
 * (1 wait, 2 dying, 3 march, 4 melee, 5 idle, 8 chase), +0x2f saved state, +0x20/+0x22 cell, +0x24/+0x26 destination,
 * +0x174 target, +0x178 group, +0x16a ranged. The battlefield: 80x80 cells of 8 bytes at 0x005440e0: +1 flags (0x10
 * blocked, 0x80 siege, 0x40 water, 0x20 moat, 0x08 gate), +4 height, +5 the figure standing there. */
#pragma once

enum { ST_WAIT = 1, ST_DYING = 2, ST_MARCH = 3, ST_MELEE = 4, ST_IDLE = 5, ST_CHASE = 8 };
#define FL_BLOCKED 0x10
#define FL_WATER   0x40
#define FL_GATE    0x08
#define FL_SIEGE   0x80
#define FL_MOAT    0x20
#define GROUP_HUNTING(g) (*(BYTE *)(0x0056654a + (g) * 0x34))
#define GROUP_DEST_X(g)  (*(short *)(0x00566542 + (g) * 0x34))
#define GROUP_DEST_Y(g)  (*(short *)(0x00566544 + (g) * 0x34))
#define MP_SESSION       (*(int *)0x00553030)   // a multiplayer game is open

static void t_unit_at(int i, int x, int y)   // moves a figure, keeping the cells' occupant bytes right
{
	int ox = FIG_S(i, 0x20), oy = FIG_S(i, 0x22);
	if (ox >= 0 && ox < 80 && oy >= 0 && oy < 80 && BCELL(ox, oy)[5] == i) BCELL(ox, oy)[5] = 0;
	FIG_S(i, 0x20) = (short)x; FIG_S(i, 0x22) = (short)y;
	if (x >= 0 && x < 80 && y >= 0 && y < 80) BCELL(x, y)[5] = (BYTE)i;
}
/* An idle foot soldier of side at x,y in group (its group's destination is where it stands). */
static BYTE *t_unit(int i, int side, int x, int y, int group)
{
	BYTE *f = FIG(i);
	memset(f, 0, 0x1b0);
	FIG_S(i, 0x20) = -1; FIG_S(i, 0x22) = -1;
	f[0x2c] = (BYTE)side; f[0x12] = 1; f[0x31] = ST_IDLE;
	FIG_S(i, 0x178) = (short)group;
	t_unit_at(i, x, y);
	FIG_S(i, 0x24) = (short)x; FIG_S(i, 0x26) = (short)y;
	GROUP_DEST_X(group) = (short)x; GROUP_DEST_Y(group) = (short)y;
	return f;
}
static BYTE *t_archer(int i, int side, int x, int y, int group) { BYTE *f = t_unit(i, side, x, y, group); f[0x16a] = 1; return f; }
static void t_flags(int x0, int y0, int x1, int y1, BYTE fl)   // flags on every cell of the rectangle
{
	for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) BCELL(x, y)[1] |= fl;
}
static void t_height(int x0, int y0, int x1, int y1, BYTE h)
{
	for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) BCELL(x, y)[4] = h;
}
/* Every step of a waypoint list legal (dm_step_ok) from the unit's cell; returns the number of steps, -1 if not. */
static int t_route_legal(int me)
{
	BYTE *f = FIG(me);
	int n = FIG_S(me, 0x36), px = FIG_S(me, 0x20), py = FIG_S(me, 0x22);
	for (int k = n - 1; k >= 0; k--) {   // entry count-1 is the next step, entry 0 the last
		int x = f[0x38 + 2 * k], y = f[0x39 + 2 * k];
		if (cheb(px, py, x, y) != 1 || !dm_step_ok(py * 80 + px, y * 80 + x)) return -1;
		px = x; py = y;
	}
	return n;
}

/* ---- stand-ins for the game's own functions the patch's hooks call on to (o_*) ---- */
static int t_calls;                  // how many times a stand-in ran
static int t_stub_ret;               // what the next stand-in returns
static void __cdecl t_stub_void(void) { t_calls++; }
static int __cdecl t_stub_int(void) { t_calls++; return t_stub_ret; }
static int __cdecl t_stub_int1(int a) { (void)a; t_calls++; return t_stub_ret; }

/* the patch's per-battle state and switches back to a battle's start, every fix on, single player */
static void t_battle_reset(void)
{
	for (int f = 0; f < BF_COUNT; f++) g_bfSet[f] = g_bfOk[f] = TRUE;
	battle_reset();
	g_mpFixes = FALSE;
	g_dmMaxDepth = 0x7fff; g_dmFullThisStep = 0;
	g_grSolo = FALSE; g_grAvoid = -1; g_grOut = NULL; g_grLeaderSet = FALSE; g_grLeadN = 0;
	memset(g_grCong, 0, sizeof g_grCong); memset(g_grCorr, 0, sizeof g_grCorr);
	g_fixWheat = g_fixCastleCap = g_fixSiegeGarrison = TRUE;
	g_aiBuilding = 0; g_inUpgrade = 0;
	t_calls = 0; t_stub_ret = 0;
	o_chase = t_stub_void; o_idle = t_stub_void; o_melee = t_stub_void; o_target = t_stub_int;
	o_swapq = t_stub_int; o_mover = t_stub_int1;
}
