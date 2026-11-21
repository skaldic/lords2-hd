/* t_ai.c - the computer battle AI log ([Debug] BattleLog): the archers' cover count, the text for one group's think,
 * and the wrapped group tick. Made-up battles only (game_mem.h). */

#define G_THINK(g)   (*(short *)(0x0056653c + (g) * 0x34))
#define G_PHASE(g)   (*(short *)(0x0056653a + (g) * 0x34))
#define AI_RATIO     (*(int *)0x004eea98)
#define AI_THRESHOLD (*(int *)0x0057c8b4)
#define AI_AGGR      (*(int *)0x00553070)

static void t_snap_all(fig_snap *n) { for (int i = 1; i < 0x51; i++) fig_take(i, &n[i]); }
static BYTE *t_human(BYTE *f) { f[0x13] = 1; return f; }
static void t_men(int i, int men) { FIG_S(i, 0x1a0) = (short)men; }

TEST(ai_cover_counts_isolated_computer_archers)
{
	static fig_snap n[81];
	t_human(t_unit(1, 1, 40, 40, 1)); t_men(1, 30);        // the player's melee
	t_archer(2, 2, 44, 40, 2); t_men(2, 20);                // computer archers 4 cells from it...
	t_unit(3, 2, 46, 40, 3); t_men(3, 25);                  // ...with their own melee 2 cells away: covered
	t_archer(4, 2, 34, 40, 2); t_men(4, 20);                // 6 cells from the enemy, own melee 12 away: isolated
	t_archer(5, 2, 60, 40, 2); t_men(5, 20);                // no enemy near: not isolated (own melee 14 away)
	t_snap_all(n);
	ai_cover c;
	REQUIRE(ai_archer_cover(n, 2, &c));
	CHECK_EQ(c.archers, 3);
	CHECK_EQ(c.isolated, 1);
	CHECK_EQ(c.ownMax, 14);
	CHECK_EQ(c.ownSum, 2 + 12 + 14);
	CHECK_EQ(c.menMelee, 25); CHECK_EQ(c.menRanged, 60);
	CHECK_EQ(c.enemyMenMelee, 30); CHECK_EQ(c.enemyMenRanged, 0);
	CHECK(!ai_archer_cover(n, 1, &c));                      // the player's side: not the computer's
}

TEST(ai_cover_ignores_the_dying_and_counts_archers_in_melee)
{
	static fig_snap n[81];
	t_human(t_unit(1, 1, 40, 40, 1));
	t_archer(2, 2, 41, 40, 2)[0x31] = ST_MELEE;             // fighting the player's unit
	t_unit(3, 2, 42, 40, 3)[0x31] = ST_DYING;               // dying: no cover
	t_snap_all(n);
	ai_cover c;
	REQUIRE(ai_archer_cover(n, 2, &c));
	CHECK_EQ(c.archers, 1);
	CHECK_EQ(c.inMelee, 1);
	CHECK_EQ(c.isolated, 1);
	CHECK_EQ(c.ownMax, 80);                                 // no live melee of its own at all
}

TEST(ai_think_text_only_for_a_computer_group_that_thought)
{
	ai_snap b = { 0 }, a = { 0 };
	ai_glob g = { 0 };
	char s[400];
	b.side = a.side = 2; b.kind = a.kind = 3; b.count = a.count = 4; b.think = 200; a.think = 0; b.phase = 13;
	g.ratio = 12; g.threshold = 5; g.limit = 21;
	a.hunting = 1;
	ai_think_text(7, &b, &a, &g, &g, s, sizeof s);
	CHECK(strstr(s, "ai g7 side 2 maces/swords (4) turn 13 ATTACK") != NULL);
	CHECK(strstr(s, "did: charge") != NULL);
	a.think = 5;                                            // no think this tick
	ai_think_text(7, &b, &a, &g, &g, s, sizeof s);
	CHECK_EQ(s[0], 0);
	a.think = 0; a.human = 1;                               // a player's group never thinks
	ai_think_text(7, &b, &a, &g, &g, s, sizeof s);
	CHECK_EQ(s[0], 0);
}

TEST(ai_think_text_names_moves_steps_and_defence)
{
	ai_snap b = { 0 }, a = { 0 };
	ai_glob g = { 0 };
	char s[400];
	b.side = a.side = 2; b.kind = a.kind = 1; b.think = 200; a.think = 0;
	g.ratio = 5; g.threshold = 5;                           // not above the threshold: defence
	a.cx = 10; a.cy = 10; b.dx = 10; b.dy = 10; a.dx = 16; a.dy = 12;
	a.presses = 1; a.attacker = 3; a.attacked = 50;
	ai_think_text(4, &b, &a, &g, &g, s, sizeof s);
	CHECK(strstr(s, "archers") != NULL);
	CHECK(strstr(s, "DEFEND") != NULL);
	CHECK(strstr(s, "under attack by g3") != NULL);
	CHECK(strstr(s, "go 10,10 -> 16,12 (6 cells from its centre)") != NULL);
	CHECK(strstr(s, "step 2 away from group 3") != NULL);
	b.dx = a.dx; b.dy = a.dy; b.presses = a.presses;
	ai_think_text(4, &b, &a, &g, &g, s, sizeof s);
	CHECK(strstr(s, "did: nothing") != NULL);
}

/* the game's group tick stand-in: group 3 (computer) thinks and charges, the shared aggression rises */
static void __cdecl t_tick_charge(void)
{
	t_calls++;
	G_THINK(3) = 0; GROUP_HUNTING(3) = 1; AI_AGGR = 20;
}

TEST(ai_group_tick_writes_the_think_and_the_aggression)
{
	static char buf[1 << 16];
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	*(BYTE *)(0x00566520 + 3 * 0x34) = 2;                   // group 3: side 2, computer
	*(BYTE *)(0x00566522 + 3 * 0x34) = 5;
	*(BYTE *)(0x00566528 + 3 * 0x34) = 4;                   // knights
	G_THINK(3) = 200; G_PHASE(3) = 13;
	AI_RATIO = 30; AI_THRESHOLD = 5;
	o_group_tick = t_tick_charge;
	h_group_tick();
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK_EQ(t_calls, 1);
	CHECK(strstr(buf, "ai groups at the start (strength +30, threshold 5: attack") != NULL);
	CHECK(strstr(buf, "g3 side 2 computer knights, 5 units") != NULL);
	CHECK(strstr(buf, "ai aggression 0 -> 20") != NULL);
	CHECK(strstr(buf, "ai g3 side 2 knights (5) turn 13 ATTACK") != NULL);
	CHECK(strstr(buf, "did: charge") != NULL);
}

TEST(ai_group_tick_without_the_log_only_runs_the_game)
{
	g_blog = NULL;
	o_group_tick = t_tick_charge;
	h_group_tick();
	CHECK_EQ(t_calls, 1);
	CHECK_EQ(GROUP_HUNTING(3), 1);
}
