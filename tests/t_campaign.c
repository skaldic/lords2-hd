/* t_campaign.c - tier 2: the campaign game fixes on made-up counties and armies in fake game memory. Our own code.
 * Included by tests.c. These check the patch's arithmetic and bookkeeping only; no game rules are changed here.
 *
 * Counties: records of 0x300 bytes; +0x5b1 wheat fields now (0x0053fbb1), +0x5b2 as planted, +0x5b6 with grain,
 * +0x56c garrison army (0x0053fb6c), +0x572 the "skip" flag the computer's garrison steps read (0x0053fb72).
 * Armies: records of 0x1a4 bytes; besieger link at 0x0052f24a. Players: records of 0x160; castles being built at
 * 0x0057bf4d. */

#define COUNTY_B(c, addr) (*(BYTE *)((addr) + (c) * 0x300))
#define COUNTY_GARRISON(c) (*(int *)(0x0053fb6c + (c) * 0x300))
#define ARMY_B(a, addr) (*(BYTE *)((addr) + (a) * 0x1a4))
#define BUILDING(p) (*(BYTE *)(0x0057bf4d + (p) * 0x160))

/* ================================ wheat fix */

static int __cdecl t_game_wheat_scale(int county, int grain)   // the game's FUN_0044d281: now / as planted
{
	t_calls++;
	return grain * COUNTY_B(county, 0x0053fbb1) / COUNTY_B(county, 0x0053fbb2);
}
TEST(wheat_lost_field_charged_once)
{
	o_wheat_scale = t_game_wheat_scale;
	COUNTY_B(5, 0x0053fbb1) = 10;               // 11 planted, one lost: its share already taken out of the pile
	COUNTY_B(5, 0x0053fbb2) = 11;
	COUNTY_B(5, 0x0053fbb6) = 10;
	CHECK_EQ(h_wheat_scale(5, 1100), 1100);     // not charged again
	g_fixWheat = FALSE;
	CHECK_EQ(h_wheat_scale(5, 1100), 1000);     // the game's own: charged twice
	g_fixWheat = TRUE;
	COUNTY_B(5, 0x0053fbb1) = 9;                // a field gone without its share: counted
	CHECK_EQ(h_wheat_scale(5, 1100), 990);
	MP_SESSION = 1;                             // multiplayer without MultiplayerFixes: the game's own
	CHECK_EQ(h_wheat_scale(5, 1100), 900);
}

/* ================================ castle cap fix */

static int t_starts;
static void __cdecl t_castle_start(int county, int kind) { (void)county; (void)kind; t_starts++; }
static void __cdecl t_ai_build_three(unsigned player) { (void)player; for (int c = 1; c <= 3; c++) h_castle_start(c, 1); }
TEST(castle_cap_counts_each_start)
{
	o_castle_start = t_castle_start; o_ai_build = t_ai_build_three;
	BUILDING(2) = 1;
	t_starts = 0;
	h_ai_build(2);
	CHECK_EQ(t_starts, 3);
	CHECK_EQ(BUILDING(2), 4);                   // each start raises the count the limit is checked against
	CHECK_EQ(g_aiBuilding, 0);
	g_fixCastleCap = FALSE; BUILDING(2) = 1;
	h_ai_build(2);
	CHECK_EQ(BUILDING(2), 1);                   // the game's own: only at the season's end
	g_fixCastleCap = TRUE; BUILDING(3) = 254;
	h_ai_build(3);
	CHECK_EQ(BUILDING(3), 255);                 // a byte: stops at 255
	BUILDING(2) = 1;
	h_castle_start(1, 1);                       // a start outside a computer lord's building step
	CHECK_EQ(BUILDING(2), 1);
}

/* ================================ besieged castles */

static void t_siege(void)   // 10 counties; county 4's castle is held by army 7, which is besieged; county 6's by army 8
{
	*(int *)0x0056d5dc = 10;
	COUNTY_GARRISON(4) = 7; ARMY_B(7, 0x0052f24a) = 1;
	COUNTY_GARRISON(6) = 8;
}
static BYTE t_seen4, t_seen6;
static int __cdecl t_ai_step(unsigned a, int b)   // a computer garrison step, looking at the skip flags
{
	(void)a; (void)b; t_calls++;
	t_seen4 = COUNTY_B(4, 0x0053fb72); t_seen6 = COUNTY_B(6, 0x0053fb72);
	return 5;
}
TEST(siege_besieged_counties)
{
	t_siege();
	CHECK(county_besieged(4));
	CHECK(!county_besieged(6));
	CHECK(!county_besieged(3));                 // no garrison
	COUNTY_GARRISON(3) = 0x97;                  // not an army number
	CHECK(!county_besieged(3));
}

TEST(siege_ai_steps_skip_besieged_castles)
{
	t_siege();
	o_ai_reinforce = t_ai_step; o_ai_pick_castle = t_ai_step; o_ai_march = t_ai_step;
	COUNTY_B(6, 0x0053fb72) = 1;                // (whatever was there is put back)
	CHECK_EQ(h_ai_reinforce(2, 0), 5);
	CHECK_EQ(t_seen4, 1); CHECK_EQ(t_seen6, 1);
	CHECK_EQ(COUNTY_B(4, 0x0053fb72), 0);       // only for the length of the step
	CHECK_EQ(COUNTY_B(6, 0x0053fb72), 1);
	h_ai_pick_castle(2, 3); CHECK_EQ(t_seen4, 1);
	h_ai_march(9, 0); CHECK_EQ(t_seen4, 1);
	g_fixSiegeGarrison = FALSE;
	h_ai_reinforce(2, 0); CHECK_EQ(t_seen4, 0);
	CHECK_EQ(t_calls, 4);
}

static void __cdecl t_castle_enter(unsigned army, int county) { (void)army; (void)county; t_calls++; }
static void __cdecl t_season_enters(void) { h_castle_enter(9, 4); }
TEST(siege_reinforcements_turned_away)
{
	t_siege();
	o_castle_enter = t_castle_enter; o_season_castles = t_season_enters;
	h_castle_enter(9, 4);                       // another army into a besieged castle: kept out
	CHECK_EQ(t_calls, 0);
	CHECK_EQ(ARMY_B(9, 0x0052f200), 1);         // as the game leaves an army the full castle turned away
	CHECK_EQ(ARMY_B(9, 0x0052f0ca), 2);
	CHECK_EQ(ARMY_B(9, 0x0052f1fc), 0);
	h_castle_enter(7, 4);                       // the garrison itself
	CHECK_EQ(t_calls, 1);
	h_castle_enter(9, 6);                       // a castle not under siege
	CHECK_EQ(t_calls, 2);
	h_season_castles();                         // a finished upgrade raises its garrison inside, siege or not
	CHECK_EQ(t_calls, 3);
	CHECK_EQ(g_inUpgrade, 0);
	MP_SESSION = 1;
	h_castle_enter(9, 4);
	CHECK_EQ(t_calls, 4);
}
