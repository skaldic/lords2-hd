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

TEST(ai_second_writes_unit_values_once_and_the_cover)
{
	static char buf[1 << 16];
	static fig_snap n[81];
	t_human(t_unit(1, 1, 40, 40, 1)); t_men(1, 32);
	BYTE *k = t_unit(2, 2, 60, 40, 2); k[0x12] = 6; t_men(2, 25);   // computer knights
	FIG_S(2, 0x19c) = 20; FIG_S(2, 0x198) = 200; k[0x18b] = 16; k[0x18a] = 4; k[0x172] = 25;
	t_archer(3, 2, 44, 40, 3); t_men(3, 30);                        // isolated computer archers
	t_snap_all(n);
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	g_bshotsBy[2] = 7; g_bshotsBy[1] = 3;                           // shots in the last second by side
	battle_second(n);
	CHECK_EQ(g_bshotsBy[2], 0);                                     // counted afresh each second
	battle_second(n);                                               // the values only once a battle
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK(strstr(buf, "type 6 (melee): men 25, melee hit 20, first strike 200, toughness 16, turn class 4, armour 25") != NULL);
	const char *first = strstr(buf, "type 6 ");
	CHECK(first && !strstr(first + 1, "type 6 "));
	CHECK(strstr(buf, "cover side 2 (computer): archers 1, own melee within 5 cells of each: nearest avg 16 max 16 | isolated 1") != NULL);
	CHECK(strstr(buf, "men melee 25 ranged 30, enemy melee 32 ranged 0 | shots 7, enemy shots 3") != NULL);
	CHECK(strstr(buf, "| shots 0, enemy shots 0") != NULL);
	CHECK(!strstr(buf, "cover side 1"));                            // only the computer's side
}

TEST(ai_group_tick_without_the_log_only_runs_the_game)
{
	g_blog = NULL;
	o_group_tick = t_tick_charge;
	h_group_tick();
	CHECK_EQ(t_calls, 1);
	CHECK_EQ(GROUP_HUNTING(3), 1);
}

/* ================================ enemy tactics (BattleAI): the steady mode */

/* the game's weighing stand-in: the armies come out at t_raw, plus the random term t_roll (-10..21) */
static int t_raw, t_roll;
static void __cdecl t_weigh(void)
{
	t_calls++;
	*(int *)0x0058fd7c = t_roll + 10;                       // the game's random number: term = (n & 31) - 10
	AI_RATIO = t_raw + t_roll;
}
/* a new field battle in single player at `level`, with its hooks in */
static void t_ai_battle(int level)
{
	g_aiOk = TRUE; g_aiLevelSet = level; g_aiParts = AI_ALL;
	*(int *)0x0053e8fc = 0; *(int *)0x00553030 = 0;
	AI_THRESHOLD = 5;
	o_strength = t_weigh;
	battle_reset();
}
static int t_weigh_at(int raw, int roll) { t_raw = raw; t_roll = roll; h_strength(); return AI_RATIO; }

TEST(ai_hard_decides_once_and_keeps_to_it)
{
	static char buf[1 << 16];
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	t_ai_battle(1);
	CHECK_EQ(t_weigh_at(0, 12), 6);                         // even, rolled +12: attack, held just above the threshold
	CHECK_EQ(t_weigh_at(0, -10), 6);                        // the next roll no longer switches it
	CHECK_EQ(t_weigh_at(-14, 0), 6);                        // a little weaker: still attacking
	CHECK_EQ(t_weigh_at(-16, 21), -16);                     // 21 below the threshold: defence
	CHECK_EQ(t_weigh_at(20, 21), 5);                        // stronger again, but not by enough
	CHECK_EQ(t_weigh_at(26, -10), 26);                      // 21 above: attack
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK_EQ(t_calls, 6);                                   // the game's own weighing every time
	CHECK(strstr(buf, "ai Hard: decided ATTACK (strength +0, with the random term +12, threshold 5)") != NULL);
	CHECK(strstr(buf, "ai Hard: now DEFEND (strength -16") != NULL);
	CHECK(strstr(buf, "ai Hard: now ATTACK (strength +26") != NULL);
}

TEST(ai_hard_defender_attacks_after_a_long_wait_only_when_about_even)
{
	t_ai_battle(1);
	CHECK(t_weigh_at(-5, -10) <= 5);                        // defence
	for (int i = 1; i < AI_DEF_WAIT; i++) CHECK(t_weigh_at(-5, 21) <= 5);
	CHECK_EQ(t_weigh_at(-5, 0), 6);                         // about 70 s of waiting: it attacks
	t_ai_battle(1);
	CHECK(t_weigh_at(-11, 0) <= 5);                         // clearly weaker: it keeps waiting
	for (int i = 0; i < 3 * AI_DEF_WAIT; i++) CHECK(t_weigh_at(-11, 21) <= 5);
}

TEST(ai_legendary_weaker_defender_sallies_after_the_wait)
{
	t_ai_battle(2);
	CHECK(t_weigh_at(-15, -10) <= 5);                       // 20 below the threshold: defence
	for (int i = 1; i < AI_DEF_WAIT; i++) CHECK(t_weigh_at(-15, 21) <= 5);
	CHECK_EQ(t_weigh_at(-15, 0), 6);                        // about 70 s: it attacks (Hard keeps waiting)
	t_ai_battle(2);
	CHECK(t_weigh_at(-26, 0) <= 5);                         // 31 below: hopeless, it keeps waiting
	for (int i = 0; i < 3 * AI_DEF_WAIT; i++) CHECK(t_weigh_at(-26, 21) <= 5);
	t_ai_battle(2); g_aiParts = AI_ALL & ~AI_SALLY;         // switched off: as Hard
	CHECK(t_weigh_at(-15, -10) <= 5);
	for (int i = 0; i < 3 * AI_DEF_WAIT; i++) CHECK(t_weigh_at(-15, 21) <= 5);
	t_ai_battle(1);                                         // Hard
	CHECK(t_weigh_at(-15, -10) <= 5);
	for (int i = 0; i < 3 * AI_DEF_WAIT; i++) CHECK(t_weigh_at(-15, 21) <= 5);
}

TEST(ai_hard_each_battle_decides_afresh)
{
	t_ai_battle(1);
	CHECK_EQ(t_weigh_at(0, 21), 6);
	t_ai_battle(1);                                         // the next battle (battle_reset)
	CHECK_EQ(t_weigh_at(0, -10), 0);                        // its own first weighing decides: defence
	CHECK_EQ(t_weigh_at(0, 21), 0);
}

TEST(ai_normal_sieges_and_multiplayer_keep_the_games_weighing)
{
	t_ai_battle(0);                                         // Normal
	CHECK_EQ(t_weigh_at(0, 12), 12); CHECK_EQ(t_weigh_at(0, -10), -10);
	t_ai_battle(1); *(int *)0x0053e8fc = 1;                 // a siege
	CHECK_EQ(t_weigh_at(0, 12), 12); CHECK_EQ(t_weigh_at(0, -10), -10);
	t_ai_battle(1); *(int *)0x00553030 = 1;                 // multiplayer, MultiplayerFixes off
	g_mpFixes = FALSE;
	CHECK_EQ(t_weigh_at(0, 12), 12); CHECK_EQ(t_weigh_at(0, -10), -10);
	g_mpFixes = TRUE;                                       // on for every player: Hard (no random term there)
	CHECK_EQ(t_weigh_at(10, 0), 10); CHECK_EQ(t_weigh_at(-5, 0), 6);
	g_mpFixes = FALSE;
	t_ai_battle(1); g_aiOk = FALSE; battle_reset();         // its hooks never went in: Normal
	CHECK_EQ(t_weigh_at(0, 12), 12); CHECK_EQ(t_weigh_at(0, -10), -10);
	t_ai_battle(1); g_aiParts = AI_ARCHERS | AI_MELEE;      // (measuring the other rules alone)
	CHECK_EQ(t_weigh_at(0, 12), 12); CHECK_EQ(t_weigh_at(0, -10), -10);
}

/* ================================ enemy tactics: the archers */

static int t_ordG, t_ordX, t_ordY, t_fireAt;
static void __cdecl t_order6(int g, int x, int y, int a, int b, int c) { (void)a; (void)b; (void)c; t_calls++; t_ordG = g; t_ordX = x; t_ordY = y; }
static int __cdecl t_fire(int tg) { t_calls++; t_fireAt = tg; return 1; }
static void t_group(int g, int side, int kind, int count, int cx, int cy)
{
	GRP_B(g, 0x00) = (BYTE)side; GRP_B(g, 0x02) = (BYTE)count; GRP_B(g, 0x08) = (BYTE)kind;
	GRP_S(g, 0x1e) = (short)cx; GRP_S(g, 0x20) = (short)cy;
}
/* Hard, with the game's order and fire functions stood in for; the archers' group 2 thinks */
static void t_ai_think_archers(void)
{
	ai_game_order = t_order6; ai_game_fire = t_fire; o_ai_ranged = t_stub_void;
	t_ordG = t_fireAt = 0; t_calls = 0;
	*(int *)0x00568458 = 2;
	G_THINK(2) = 200;
	h_ai_ranged();
}
static BYTE *t_bow(int i, int side, int x, int y, int group, int range) { BYTE *f = t_archer(i, side, x, y, group); FIG_S(i, 0x170) = (short)(range * 8); return f; }

TEST(ai_hard_archers_step_back_from_melee_with_no_cover)
{
	t_ai_battle(1);
	t_bow(1, 2, 40, 40, 2, 15); t_bow(2, 2, 41, 40, 2, 15);   // computer archers, group 2, centre 40,40
	t_group(2, 2, 1, 2, 40, 40);
	t_human(t_unit(3, 1, 43, 40, 3)); t_group(3, 1, 3, 1, 43, 40);   // the player's melee 3 cells east
	t_unit(4, 2, 50, 40, 4); t_group(4, 2, 3, 1, 50, 40);           // its own melee far away
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 2);
	CHECK_EQ(t_ordX, 36); CHECK_EQ(t_ordY, 40);                     // 4 cells west, away from it
	CHECK_EQ(GROUP_DEST_X(2), 36);                                  // the destination written as the game's helpers do
	CHECK_EQ(t_fireAt, 0);
	CHECK_EQ(G_THINK(2), 0); CHECK_EQ(G_PHASE(2), 1);
	t_unit(4, 2, 42, 41, 4);                                        // its own melee beside them: they stay and shoot
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 0); CHECK_EQ(t_fireAt, 3);
}

TEST(ai_hard_archers_stand_behind_their_melee_until_in_range)
{
	t_ai_battle(1);
	t_bow(1, 2, 20, 40, 2, 8); t_group(2, 2, 1, 1, 20, 40);         // crossbowmen at 20,40 (range 8)
	t_unit(4, 2, 50, 40, 4); t_unit(5, 2, 50, 42, 4); t_group(4, 2, 3, 2, 50, 41);   // their melee round 50,41
	t_human(t_unit(6, 1, 70, 41, 6)); t_group(6, 1, 3, 1, 70, 41);  // the enemy far east
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 2);
	CHECK_EQ(t_ordX, 47); CHECK_EQ(t_ordY, 41);                     // 3 cells behind the melee, away from the enemy
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 0);                                            // already going there: no new order
	GROUP_HUNTING(4) = 1;                                           // the melee charges: walk up into range
	GRP_S(2, 0x1e) = 47; GRP_S(2, 0x20) = 41; t_unit_at(1, 47, 41);
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 2);
	CHECK_EQ(t_ordX, 64); CHECK_EQ(t_ordY, 41);                     // the enemy 2 cells inside range: 6 away
}

/* ================================ enemy tactics: the melee */

static int t_charged[8], t_nCharged;
static void __cdecl t_charge(int g) { t_calls++; if (t_nCharged < 8) t_charged[t_nCharged++] = g; }
/* melee group g (computer, Hard) thinks; the game's order and charge stood in for */
static void t_ai_think_melee(int g)
{
	ai_game_order = t_order6; ai_game_charge = t_charge; o_ai_melee34 = t_stub_void; o_ai_melee2 = t_stub_void;
	t_ordG = 0; t_nCharged = 0; t_calls = 0;
	*(int *)0x00568458 = g;
	G_THINK(g) = 200;
	if (GRP_B(g, 0x08) == 2) h_ai_melee2(); else h_ai_melee34();
}

TEST(ai_hard_melee_advances_no_further_than_a_step_ahead_of_the_rearmost)
{
	t_ai_battle(1); AI_RATIO = 30;                                  // attack
	t_unit(4, 2, 30, 40, 4); t_group(4, 2, 3, 1, 30, 40);          // a forward group, 20 cells from the enemy
	t_unit(5, 2, 10, 40, 5); t_group(5, 2, 4, 1, 10, 40);          // knights 40 cells away
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
	t_ai_think_melee(4);
	CHECK_EQ(t_ordG, 0); CHECK_EQ(t_nCharged, 0);                   // waits for the knights
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 5); CHECK_EQ(t_ordX, 16); CHECK_EQ(t_ordY, 40);   // they come up 6 cells
	CHECK_EQ(G_PHASE(5), 1);
}

TEST(ai_hard_melee_in_place_waits_for_the_volleys_then_charges_together)
{
	t_ai_battle(1); AI_RATIO = 30;
	t_unit(4, 2, 44, 40, 4); t_group(4, 2, 3, 1, 44, 40);          // 6 cells from the enemy
	t_unit(5, 2, 43, 41, 5); t_group(5, 2, 2, 1, 43, 41);          // pikemen 7 cells away: all in place
	t_bow(1, 2, 40, 40, 2, 15); t_group(2, 2, 1, 1, 40, 40);       // archers behind them
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
	g_btick = 1000;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 0);                   // in place: the archers shoot first
	g_btick = 1000 + AI_VOLLEY_STEPS - 1;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);
	G_THINK(5) = 20;
	g_btick = 1000 + AI_VOLLEY_STEPS;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_charged[0], 4);
	CHECK(g_aiCharge[2]);
	CHECK_EQ(G_THINK(5), 199);                                      // the pikemen think at their next step...
	t_ai_think_melee(5);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_charged[0], 5);             // ...and charge too
	CHECK_EQ(G_THINK(2), 0);                                        // (not the archers)
}

TEST(ai_hard_melee_turned_to_attack_late_still_advances_first)
{
	t_ai_battle(1); AI_RATIO = 0;                                   // defending for a long while
	t_unit(4, 2, 20, 40, 4); t_group(4, 2, 3, 1, 20, 40);
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
	g_btick = 6000;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);
	G_PHASE(4) = 30;                                                // (its 30th think)
	AI_RATIO = 30; g_btick = 6200;                                  // now it attacks
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 4);                   // it advances, no charge yet
	g_btick = 6200 + AI_CHARGE_STEPS - 1;
	GRP_S(4, 0x1e) = 30; t_unit_at(4, 30, 40);
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);
	g_btick = 6200 + AI_CHARGE_STEPS;                               // about 60 s of attacking: it charges anyway
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
}

TEST(ai_hard_melee_charges_at_once_when_hit_in_attack)
{
	t_ai_battle(1); AI_RATIO = 30;
	t_unit(4, 2, 20, 40, 4); t_group(4, 2, 3, 1, 20, 40);
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 5, 1, 50, 40);
	t_bow(1, 2, 18, 40, 2, 15); t_group(2, 2, 1, 1, 18, 40);
	GRP_B(2, 0x0c) = 50; GRP_S(2, 0x0a) = 6;                        // its archers are shot at by group 6
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK(g_aiCharge[2]);
}

TEST(ai_hard_defence_holds_until_the_enemy_is_close_or_it_bleeds)
{
	t_ai_battle(1); AI_RATIO = 0;                                   // defence
	for (int i = 1; i <= 3; i++) { t_unit(i, 2, 40, 39 + i, 4); t_men(i, 32); }
	t_group(4, 2, 3, 3, 40, 41);
	t_human(t_unit(6, 1, 46, 40, 6)); t_human(t_unit(7, 1, 46, 41, 6)); t_group(6, 1, 3, 2, 46, 40);
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 0);                   // two enemy units close: it holds
	t_human(t_unit(8, 1, 46, 42, 6));
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);                                        // three: the side charges
	t_ai_battle(1); AI_RATIO = 0;
	t_unit_at(6, 70, 40); t_unit_at(7, 70, 41); t_unit_at(8, 70, 42);   // far away now
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);                                        // (its men counted: 96)
	for (int i = 1; i <= 3; i++) t_men(i, 26);                      // shot down to 78: lost 19%
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
}

TEST(ai_hard_melee_normal_runs_the_games_handlers)
{
	t_ai_battle(0); AI_RATIO = 30;
	t_unit(4, 2, 20, 40, 4); t_group(4, 2, 2, 1, 20, 40);
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
	t_ai_think_melee(4);
	CHECK_EQ(t_calls, 1); CHECK_EQ(G_THINK(4), 200);                // the game's (stood-in) handler, nothing else
	t_ai_battle(1); g_aiParts = AI_STEADY | AI_ARCHERS;
	t_ai_think_melee(4);
	CHECK_EQ(t_calls, 1); CHECK_EQ(G_THINK(4), 200);
}

/* ================================ enemy tactics: Legendary's unit counters */

/* the game's melee hit stand-in: records the opponent's damage and the current unit's first strike it sees */
static int t_hitDmg, t_hitFirst;
static void __cdecl t_melee_hit(void) { t_calls++; t_hitDmg = FIG_S(*(int *)0x0052eff4, 0x19c); t_hitFirst = FIG_S(FIG_CUR, 0x198); }
/* unit `me` (type mt) is hit by `opp` (type ot): damage 20, first strike 200 each; returns what the game saw */
static void t_hit(int mt, int ot)
{
	t_unit(1, 2, 40, 40, 1); FIG(1)[0x12] = (BYTE)mt; FIG_S(1, 0x19c) = 20; FIG_S(1, 0x198) = 200;
	t_unit(2, 1, 41, 40, 2); FIG(2)[0x12] = (BYTE)ot; FIG_S(2, 0x19c) = 20; FIG_S(2, 0x198) = 200;
	FIG_CUR = 1; *(int *)0x0052eff4 = 2;
	o_melee_hit = t_melee_hit; t_calls = 0; t_hitDmg = t_hitFirst = -1;
	h_melee_hit();
}

TEST(ai_legendary_counters_scale_the_pair_and_put_it_back)
{
	t_ai_battle(2);
	t_hit(6, 4);                                                    // knights hit by pikemen
	CHECK_EQ(t_calls, 1); CHECK_EQ(t_hitDmg, 40); CHECK_EQ(t_hitFirst, 150);   // pikes x2 on them, their first strike x0.75
	CHECK_EQ(FIG_S(2, 0x19c), 20); CHECK_EQ(FIG_S(1, 0x198), 200);  // both put back
	t_hit(4, 6);                                                    // pikemen hit by knights
	CHECK_EQ(t_hitDmg, 15); CHECK_EQ(t_hitFirst, 400);
	t_hit(4, 3);                                                    // pikemen hit by swordsmen
	CHECK_EQ(t_hitDmg, 30); CHECK_EQ(t_hitFirst, 200);
	t_hit(6, 2);                                                    // knights hit by maces
	CHECK_EQ(t_hitDmg, 30); CHECK_EQ(t_hitFirst, 200);
	t_hit(3, 6);                                                    // no counter: as the game has it
	CHECK_EQ(t_hitDmg, 20); CHECK_EQ(t_hitFirst, 200);
	t_hit(5, 4); CHECK_EQ(t_hitDmg, 20);
}

TEST(ai_counters_only_on_legendary_in_field_battles)
{
	t_ai_battle(1); t_hit(6, 4);                                    // Hard: none
	CHECK_EQ(t_calls, 1); CHECK_EQ(t_hitDmg, 20); CHECK_EQ(t_hitFirst, 200);
	t_ai_battle(0); t_hit(6, 4); CHECK_EQ(t_hitDmg, 20);            // Normal
	t_ai_battle(2); *(int *)0x0053e8fc = 1; t_hit(6, 4);            // a siege
	CHECK_EQ(t_hitDmg, 20);
	t_ai_battle(2); *(int *)0x00553030 = 1; g_mpFixes = FALSE; t_hit(6, 4);   // multiplayer without MultiplayerFixes
	CHECK_EQ(t_hitDmg, 20);
	g_mpFixes = TRUE; t_hit(6, 4); CHECK_EQ(t_hitDmg, 40);          // with them (every player on Legendary)
	g_mpFixes = FALSE; *(int *)0x00553030 = 0;
	t_ai_battle(2); g_aiParts = AI_ALL & ~AI_COUNTERS; t_hit(6, 4);   // switched off for measuring
	CHECK_EQ(t_hitDmg, 20);
	t_ai_battle(2); t_hit(6, 4); FIG(2)[0x12] = 8;                  // a siege engine: left alone
	t_calls = 0; h_melee_hit(); CHECK_EQ(t_calls, 1); CHECK_EQ(t_hitDmg, 20);
}

/* ================================ enemy tactics: Legendary's tactics */

static BYTE *t_typed(BYTE *f, int type, int men) { f[0x12] = (BYTE)type; FIG_S((int)((f - FIG(0)) / 0x1b0), 0x1a0) = (short)men; return f; }

TEST(ai_legendary_knights_go_for_the_archers_and_pikemen_for_the_knights)
{
	t_ai_battle(2);
	t_typed(t_unit(4, 2, 40, 40, 4), 6, 30); t_group(4, 2, 4, 1, 40, 40);              // computer knights
	t_typed(t_unit(5, 2, 40, 50, 5), 4, 30); t_group(5, 2, 2, 1, 40, 50);              // computer pikemen
	t_typed(t_human(t_unit(6, 1, 50, 40, 6)), 3, 30); t_group(6, 1, 3, 1, 50, 40);     // your swords, 10 cells from the knights
	t_typed(t_human(t_archer(7, 1, 58, 44, 7)), 5, 30); t_group(7, 1, 1, 1, 58, 44);   // your archers, 18 cells away
	t_typed(t_human(t_unit(8, 1, 52, 50, 8)), 6, 30); t_group(8, 1, 4, 1, 52, 50);     // your knights, 12 from the pikemen
	CHECK_EQ(ai_pick_target(4), 7);                                 // the knights ride for the archers
	CHECK_EQ(ai_pick_target(5), 8);                                 // the pikemen for the knights (not the swords, 10 away)
	g_aiCharge[2] = TRUE; AI_RATIO = 30;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 4); CHECK_EQ(t_ordX, 58); CHECK_EQ(t_ordY, 44);   // walks to its target
	CHECK_EQ(g_aiTarget[4], 7);
	GRP_S(4, 0x1e) = 55; GRP_S(4, 0x20) = 43; t_unit_at(4, 55, 43);  // within reach
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK(g_aiCharged[4]);                 // the game's charge
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_ordG, 0);                   // and stays with it
}

TEST(ai_legendary_target_walk_charges_at_once_when_engaged_and_hard_has_none)
{
	t_ai_battle(2);
	t_typed(t_unit(4, 2, 40, 40, 4), 6, 30); t_group(4, 2, 4, 1, 40, 40);
	t_typed(t_human(t_archer(7, 1, 60, 40, 7)), 5, 30); t_group(7, 1, 1, 1, 60, 40);
	t_typed(t_human(t_unit(6, 1, 42, 40, 6)), 4, 30); t_group(6, 1, 2, 1, 42, 40);     // pikemen 2 cells away
	CHECK_EQ(ai_pick_target(4), 7);                                 // (pikes counter knights: it goes for the archers)
	g_aiCharge[2] = TRUE; AI_RATIO = 30;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_ordG, 0);                   // engaged on the way: charges where it is
	t_ai_battle(1);                                                 // Hard: the game's charge straight away
	t_unit_at(6, 70, 70); GRP_S(6, 0x1e) = 70; GRP_S(6, 0x20) = 70;
	g_aiCharge[2] = TRUE; AI_RATIO = 30;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_ordG, 0);
}

TEST(ai_legendary_line_keeps_closing_in_under_fire_from_afar)
{
	t_ai_battle(2); AI_RATIO = 30; g_aiParts = AI_ALL & ~AI_TARGETS;   // (the charge itself without target picks)
	t_typed(t_unit(4, 2, 20, 40, 4), 3, 30); t_group(4, 2, 3, 1, 20, 40);
	t_typed(t_human(t_unit(6, 1, 50, 40, 6)), 5, 30); t_group(6, 1, 1, 1, 50, 40);
	t_bow(1, 2, 18, 40, 2, 15); t_men(1, 15); t_group(2, 2, 1, 1, 18, 40);   // the side: 45 men
	GRP_B(2, 0x0c) = 50; GRP_S(2, 0x0a) = 6;                        // shot at by group 6, 30 cells away
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 4);                   // no charge: it advances
	CHECK(!g_aiCharge[2]);
	t_men(4, 26);                                                   // lost 4 of 45 men (9%): still advancing
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);
	t_men(4, 25);                                                   // 11%: it charges
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK(g_aiCharge[2]);
	t_ai_battle(2); AI_RATIO = 30; t_men(4, 30); g_aiParts = AI_ALL & ~AI_TARGETS;
	t_unit_at(4, 39, 40); GRP_S(4, 0x1e) = 39;                      // 11 cells away: hit, it charges
	GRP_B(2, 0x0c) = 50; GRP_S(2, 0x0a) = 6;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
}

TEST(ai_legendary_beaten_group_falls_back_and_rejoins_when_the_enemy_is_spent)
{
	t_ai_battle(2); AI_RATIO = 30; g_aiParts = AI_ALL & ~AI_TARGETS;
	t_typed(t_unit(4, 2, 40, 40, 4), 3, 40); t_group(4, 2, 3, 1, 40, 40);              // swords, 40 men
	t_typed(t_unit(5, 2, 40, 44, 5), 3, 60); t_group(5, 2, 3, 1, 40, 44);
	t_bow(1, 2, 30, 40, 2, 15); t_men(1, 30); t_group(2, 2, 1, 1, 30, 40);             // archers at 30,40
	t_typed(t_human(t_unit(6, 1, 46, 40, 6)), 3, 100); t_group(6, 1, 3, 1, 46, 40);
	g_aiCharge[2] = TRUE;
	t_ai_think_melee(4);                                            // (its men counted: 40)
	CHECK_EQ(t_nCharged, 1);
	t_men(4, 13);                                                   // down to 32%
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 4);
	CHECK_EQ(t_ordX, 26); CHECK_EQ(t_ordY, 40);                     // 4 cells behind its archers
	CHECK_EQ(g_aiBack[4], 1);
	t_men(6, 42);                                                   // the enemy (your men count) down to 42 of the side's 103
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);
	t_men(6, 41);                                                   // 41: under 40%
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(g_aiBack[4], 2);              // rejoins the fight
}

TEST(ai_legendary_archers_shoot_the_enemy_archers_first)
{
	t_ai_battle(2);
	t_bow(1, 2, 40, 40, 2, 15); t_group(2, 2, 1, 1, 40, 40);
	t_unit(5, 2, 44, 40, 5); t_group(5, 2, 3, 1, 44, 40);           // its own melee in front: covered
	t_human(t_unit(3, 1, 50, 40, 3)); t_group(3, 1, 3, 1, 50, 40);  // your swords 10 cells away
	t_human(t_archer(4, 1, 52, 46, 4)); t_group(4, 1, 1, 1, 52, 46);   // your archers 12 away
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 4);
	t_ai_battle(1);                                                 // Hard: the nearest
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 3);
}

TEST(ai_hard_archers_wait_while_a_member_fights_and_normal_runs_the_game)
{
	t_ai_battle(1);
	t_bow(1, 2, 40, 40, 2, 15); t_group(2, 2, 1, 1, 40, 40);
	t_human(t_unit(3, 1, 43, 40, 3)); t_group(3, 1, 3, 1, 43, 40);
	GRP_B(2, 0x0d) = 1;                                             // a member in melee: no think
	t_ai_think_archers();
	CHECK_EQ(t_calls, 0); CHECK_EQ(G_THINK(2), 201);
	GRP_B(2, 0x0d) = 0;
	t_ai_battle(0);                                                 // Normal: the game's own handler
	t_ai_think_archers();
	CHECK_EQ(t_calls, 1); CHECK_EQ(t_ordG, 0); CHECK_EQ(G_THINK(2), 200);
}
