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
	*(int *)0x00568210 = 0; *(int *)0x0057a0f0 = 0;         // no campaign mover, not a custom battle
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
	CHECK_EQ(t_weigh_at(-15, 0), 6);                        // (20 below the threshold: still attacking)
	CHECK(t_weigh_at(-16, 0) <= 5);                         // 21 below: back to defence, as before
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
	CHECK_EQ(t_weigh_at(-16, 0), 6);                        // a dip does not call it back
	CHECK_EQ(t_weigh_at(-35, 0), 6);                        // (20 below where it began: still attacking)
	CHECK(t_weigh_at(-36, 0) <= 5);                         // beaten: back to defence
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

static void t_group(int g, int side, int kind, int count, int cx, int cy);
/* lord 5's army 7 marched on yours on the campaign map (the battle's first army); its pikemen on the field */
static void t_invasion(int level, int owner)
{
	t_ai_battle(level);
	((signed char *)0x0052f0b0)[7 * 0x1a4] = (signed char)owner; *(int *)0x00568210 = 7;
	t_group(4, 5, 2, 10, 33, 9);
}
TEST(ai_invader_attacks_unless_far_weaker)
{
	static char buf[1 << 16];
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	t_invasion(2, 5);
	CHECK_EQ(t_weigh_at(-13, -1), 6);                       // the user's save 7 (-13): it attacks
	CHECK_EQ(t_weigh_at(-45, 0), 6);                        // losing, 50 below the threshold: still attacking
	CHECK(t_weigh_at(-46, 0) <= 5);                         // beaten: defence
	CHECK_EQ(t_weigh_at(-25, 0), 6);                        // back within 30 of the threshold: attacks again
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK(strstr(buf, "ai Legendary: decided ATTACK (strength -13, with the random term -14, threshold 5; it marched on you on the campaign map)") != NULL);
	t_invasion(1, 5);                                       // Hard the same
	CHECK_EQ(t_weigh_at(-25, 0), 6);
	t_invasion(2, 5);
	CHECK(t_weigh_at(-26, 0) <= 5);                         // more than 30 below from the start: it defends
	t_invasion(2, 1);                                       // your army marched on it: as before
	CHECK(t_weigh_at(-13, -1) <= 5);
	t_invasion(2, 5); *(int *)0x0057a0f0 = 1;               // a custom battle: as before
	CHECK(t_weigh_at(-13, -1) <= 5);
	t_invasion(0, 5);                                       // Normal: the game's weighing
	CHECK_EQ(t_weigh_at(-13, -1), -14);
	*(int *)0x00568210 = 0; *(int *)0x0057a0f0 = 0;
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
	t_ai_battle(1); *(int *)0x00553030 = 1;                 // multiplayer: Enemy tactics are single player only
	CHECK_EQ(t_weigh_at(0, 12), 12); CHECK_EQ(t_weigh_at(0, -10), -10);
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

TEST(ai_hard_archers_behind_stakes_shoot_instead_of_stepping_back)
{
	t_ai_battle(1);
	t_bow(1, 2, 40, 40, 2, 15); t_bow(2, 2, 41, 40, 2, 15); t_group(2, 2, 1, 2, 40, 40);
	t_human(t_unit(3, 1, 43, 40, 3)); t_group(3, 1, 3, 1, 43, 40);   // the player's melee 3 cells east
	t_unit(4, 2, 50, 40, 4); t_group(4, 2, 3, 1, 50, 40);
	t_flags(42, 0, 42, 79, FL_BLOCKED);                             // stakes between them, no way round
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 0); CHECK_EQ(t_fireAt, 3);                     // they stay and shoot
	BCELL(42, 40)[1] = 0;                                           // a gap: now they step back
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 2); CHECK_EQ(t_ordX, 36);
	for (int y = 0; y < 80; y++) BCELL(42, y)[1] = 0;
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

TEST(ai_hard_advance_keeps_walking_and_waits_for_a_real_step)
{
	t_ai_battle(1); AI_RATIO = 30;
	t_unit(4, 2, 30, 40, 4); t_group(4, 2, 3, 1, 30, 40); GRP_S(4, 0x22) = 30; GRP_S(4, 0x24) = 40;   // standing at 30,40
	t_unit(5, 2, 10, 40, 5); t_group(5, 2, 4, 1, 10, 40);
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 5); CHECK_EQ(t_ordX, 16);                      // the knights: 6 cells
	t_unit_at(5, 12, 40); GRP_S(5, 0x1e) = 12; FIG(5)[0x31] = ST_MARCH;   // 4 cells still to go, walking
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 0);                                            // they keep walking (not re-formed)
	FIG(5)[0x31] = ST_IDLE;                                         // stopped there (its places taken): arrived
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 5); CHECK_EQ(t_ordX, 18);                      // the next step from where it stands
	GRP_S(5, 0x22) = 16;                                            // (back to the first point)
	t_unit_at(5, 15, 40); GRP_S(5, 0x1e) = 15;                      // nearly there: the next step
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 5); CHECK_EQ(t_ordX, 21);
	t_unit_at(5, 26, 40); GRP_S(5, 0x1e) = 26;                      // the knights 24 from the enemy: the line 18
	t_ai_think_melee(4);
	CHECK_EQ(t_ordG, 0);                                            // a 2-cell hop: the forward group waits
	t_unit_at(5, 28, 40); GRP_S(5, 0x1e) = 28;                      // the line 16: a 4-cell step
	t_ai_think_melee(4);
	CHECK_EQ(t_ordG, 4); CHECK_EQ(t_ordX, 34);
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
	CHECK(g_aiCharge[2]); CHECK(g_aiChargeAtt[2]);                  // (a planned charge: Legendary's targets would run)
	CHECK_EQ(G_THINK(5), 199);                                      // the pikemen think at their next step...
	t_ai_think_melee(5);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_charged[0], 5);             // ...and charge too
	CHECK_EQ(G_THINK(2), 0);                                        // (not the archers)
}

/* A campaign battle's sides are the lords' numbers: against lord 5 its melee stood still for the whole battle
 * (the user's Legendary battle in save 7: the melee AI took sides 1 and 2 only and the game's own was replaced) */
TEST(ai_melee_of_any_lord_advances_and_charges)
{
	for (int side = 2; side <= 5; side++) {
		t_ai_battle(2); AI_RATIO = 30;
		t_unit(4, side, 30, 40, 4); t_group(4, side, 3, 1, 30, 40);   // 20 cells from the enemy
		t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
		t_ai_think_melee(4);
		CHECK_EQ(t_ordG, 4); CHECK_EQ(t_ordX, 36);                      // it advances 6 cells
		t_ai_battle(2); AI_RATIO = 30;
		t_unit(4, side, 44, 40, 4); t_group(4, side, 3, 1, 44, 40);   // in place, no archers
		t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
		t_ai_think_melee(4);
		CHECK(g_aiCharge[side]);                                        // and charges: Legendary walks to its target
		CHECK_EQ(t_ordG, 4); CHECK_EQ(t_ordX, 50); CHECK_EQ(t_ordY, 40);
		CHECK(side == 2 || !g_aiCharge[2]);
	}
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
	CHECK(!g_aiChargeAtt[2]);                                       // (made to charge: no target walks)
	t_ai_battle(1); AI_RATIO = 30; GRP_B(2, 0x0c) = 0;
	FIG(4)[0x31] = 4;                                               // one of its units fighting: the same
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK(!g_aiChargeAtt[2]);
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
	CHECK(!g_aiChargeAtt[2]);                                       // (from defence)
	t_ai_battle(1); AI_RATIO = 0;
	t_unit_at(6, 70, 40); t_unit_at(7, 70, 41); t_unit_at(8, 70, 42);   // far away now
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);                                        // (its men counted: 96)
	for (int i = 1; i <= 3; i++) t_men(i, 26);                      // shot down to 78: lost 19%
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
}

TEST(ai_hard_nearest_free_melee_goes_to_help_its_archers)
{
	t_ai_battle(1); AI_RATIO = 0;                                   // defence
	t_bow(1, 2, 40, 40, 2, 15); t_group(2, 2, 1, 1, 40, 40);       // its archers
	t_unit(4, 2, 50, 40, 4); t_group(4, 2, 3, 1, 50, 40);          // swords 10 cells off
	t_unit(5, 2, 60, 40, 5); t_group(5, 2, 2, 1, 60, 40);          // pikemen 20 off
	t_human(t_unit(6, 1, 42, 41, 6)); t_group(6, 1, 3, 1, 42, 41); // your swords at the archers
	G_THINK(4) = 20;
	ai_rescue_wake(2);
	CHECK_EQ(G_THINK(4), 199);                                      // the nearest is woken at once
	t_ai_think_melee(4);
	CHECK_EQ(t_ordG, 4); CHECK_EQ(t_ordX, 42); CHECK_EQ(t_ordY, 41);   // and walks to the attacker
	CHECK_EQ(g_aiRescue[4], 2);
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 0); CHECK_EQ(t_nCharged, 0);                   // one is enough for one enemy unit
	GRP_B(4, 0x0d) = 1;                                             // fighting now: still the helper, no second
	t_ai_think_melee(5);
	CHECK_EQ(t_ordG, 0);
	GRP_B(4, 0x0d) = 0;
	t_unit_at(4, 45, 41); GRP_S(4, 0x1e) = 45; GRP_S(4, 0x20) = 41;   // 3 cells from it: the game's charge
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
	t_unit_at(6, 70, 70); GRP_S(6, 0x1e) = 70; GRP_S(6, 0x20) = 70;   // the archers are clear: let go
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(g_aiRescue[4], 0);
	t_ai_battle(1); AI_RATIO = 0;                                   // too far (over 20 cells): nobody goes
	t_unit_at(6, 42, 41); GRP_S(6, 0x1e) = 42; GRP_S(6, 0x20) = 41;
	t_unit_at(4, 70, 40); GRP_S(4, 0x1e) = 70; t_unit_at(5, 75, 40); GRP_S(5, 0x1e) = 75;
	t_ai_think_melee(4);
	CHECK_EQ(t_ordG, 0); CHECK_EQ(g_aiRescue[4], 0);
}

TEST(ai_hard_no_help_for_archers_behind_stakes)
{
	t_ai_battle(1); AI_RATIO = 0;
	t_bow(1, 2, 40, 40, 2, 15); t_group(2, 2, 1, 1, 40, 40);
	t_unit(4, 2, 50, 40, 4); t_group(4, 2, 3, 1, 50, 40);
	t_human(t_unit(6, 1, 42, 41, 6)); t_group(6, 1, 3, 1, 42, 41); // your swords 2 cells off...
	t_flags(41, 0, 41, 79, FL_BLOCKED);                            // ...across a line of stakes
	int ex = 0, ey = 0;
	CHECK_EQ(ai_archers_attackers(2, &ex, &ey), 0);
	G_THINK(4) = 20;
	ai_rescue_wake(2);
	CHECK_EQ(G_THINK(4), 20);                                       // nobody woken, nobody sent
	t_ai_think_melee(4);
	CHECK_EQ(g_aiRescue[4], 0);
	for (int y = 0; y < 80; y++) BCELL(41, y)[1] = 0;
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
	CHECK_EQ(t_hitDmg, 30); CHECK_EQ(t_hitFirst, 300);              // (and knights x1.5 on maces)
	t_hit(3, 6);                                                    // no counter: as the game has it
	CHECK_EQ(t_hitDmg, 20); CHECK_EQ(t_hitFirst, 200);
	t_hit(5, 4); CHECK_EQ(t_hitDmg, 20);
}

TEST(ai_legendary_knights_stomp_light_units)
{
	t_ai_battle(2);
	t_hit(0, 6); CHECK_EQ(t_hitDmg, 30);                            // peasants hit by knights: x1.5
	t_hit(5, 6); CHECK_EQ(t_hitDmg, 30);                            // archers
	t_hit(2, 6); CHECK_EQ(t_hitDmg, 30); CHECK_EQ(t_hitFirst, 300); // maces: hit hard both ways
	t_hit(1, 6); CHECK_EQ(t_hitDmg, 20);                            // crossbowmen are armoured: none
	t_hit(6, 0); CHECK_EQ(t_hitFirst, 300);                         // the knights' own first strike on peasants
}

TEST(ai_legendary_swordsmen_cut_down_light_units)
{
	t_ai_battle(2);
	t_hit(0, 3); CHECK_EQ(t_hitDmg, 30);                            // peasants hit by swordsmen: x1.5
	t_hit(5, 3); CHECK_EQ(t_hitDmg, 30);                            // archers
	t_hit(2, 3); CHECK_EQ(t_hitDmg, 30);                            // maces
	t_hit(1, 3); CHECK_EQ(t_hitDmg, 20);                            // crossbowmen are armoured: none
	t_hit(6, 3); CHECK_EQ(t_hitDmg, 20);                            // knights: none
	t_ai_battle(1); t_hit(0, 3); CHECK_EQ(t_hitDmg, 20);            // Hard: none
}

TEST(ai_counters_only_on_legendary_in_field_battles)
{
	t_ai_battle(1); t_hit(6, 4);                                    // Hard: none
	CHECK_EQ(t_calls, 1); CHECK_EQ(t_hitDmg, 20); CHECK_EQ(t_hitFirst, 200);
	t_ai_battle(0); t_hit(6, 4); CHECK_EQ(t_hitDmg, 20);            // Normal
	t_ai_battle(2); *(int *)0x0053e8fc = 1; t_hit(6, 4);            // a siege
	CHECK_EQ(t_hitDmg, 20);
	t_ai_battle(2); *(int *)0x00553030 = 1; t_hit(6, 4);            // multiplayer: the game's own
	CHECK_EQ(t_hitDmg, 20);
	*(int *)0x00553030 = 0;
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
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_ordG, 0);                   // a defending side's charge: no target walk
	g_aiCharged[4] = 0; g_aiChargeAtt[2] = TRUE;                    // charged from attack
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 4); CHECK_EQ(t_ordX, 58); CHECK_EQ(t_ordY, 44);   // walks to its target
	CHECK_EQ(g_aiTarget[4], 7);
	GRP_S(4, 0x1e) = 55; GRP_S(4, 0x20) = 43; t_unit_at(4, 55, 43);  // within reach
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK(g_aiCharged[4]);                 // the game's charge
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(t_ordG, 0);                   // and stays with it
}

TEST(ai_charge_is_not_repeated_while_the_group_is_on_it)
{
	t_battle_reset(); t_ai_battle(1); AI_RATIO = 30;
	t_typed(t_unit(4, 2, 40, 40, 4), 3, 30); t_group(4, 2, 3, 1, 40, 40);
	t_typed(t_human(t_unit(6, 1, 43, 40, 6)), 3, 30); t_group(6, 1, 3, 1, 43, 40);
	g_aiCharge[2] = TRUE;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
	GROUP_HUNTING(4) = 1;                                           // (what the game's charge sets)
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0);                                        // still on it: its chasers keep their routes
	GROUP_HUNTING(4) = 0;                                           // off it (an order)
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
	GROUP_HUNTING(4) = 1; g_bfSet[BF_MOPUP] = FALSE; battle_reset(); g_aiCharge[2] = TRUE; g_aiCharged[4] = 1;
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);                                        // without MopUpFix nothing wakes idle members: repeated
}

/* ================================ enemy tactics: the crossing assault */

/* a river at rows 39-41 with a bridge 2 cells wide at x 20-21 (the only way over); your pikemen hold it from
 * the north (60 men, 6 on the bridge, 7 behind it); the computer's swordsmen charge from the south (group 4: 4
 * and 5 chasing, 8 in melee at the bridge mouth, 70 men in all), attacking; no archers */
static void t_bridge_battle(int level)
{
	t_battle_reset(); t_ai_battle(level); AI_RATIO = 30;
	t_flags(0, 39, 79, 41, FL_WATER);
	for (int y = 39; y <= 41; y++) { BCELL(20, y)[1] = 0; BCELL(21, y)[1] = 0; }
	t_typed(t_unit(4, 2, 20, 52, 4), 3, 25); t_typed(t_unit(5, 2, 22, 52, 4), 3, 25); t_typed(t_unit(8, 2, 21, 42, 4), 3, 20);
	t_group(4, 2, 3, 3, 21, 52);
	FIG(4)[0x31] = ST_CHASE; FIG(4)[0x174] = 6; FIG(4)[0x164] = 1;
	FIG(5)[0x31] = ST_WAIT; FIG(5)[0x2f] = ST_CHASE; FIG(5)[0x174] = 6;
	FIG(8)[0x31] = ST_MELEE; FIG(8)[0x174] = 6;
	GROUP_HUNTING(4) = 1; g_aiCharged[4] = 1; g_aiCharge[2] = TRUE;
	t_typed(t_human(t_unit(6, 1, 20, 40, 6)), 4, 30); t_typed(t_human(t_unit(7, 1, 21, 37, 6)), 4, 30);
	t_group(6, 1, 2, 2, 20, 38); GRP_B(6, 0x01) = 1;
	map_read();
	ai_game_order = t_order6; ai_game_charge = t_charge; t_ordG = 0; t_nCharged = 0; t_calls = 0;
}
static int t_south(void) { return g_crossBank[0][52 * 80 + 21]; }   // the computer's bank of the bridge
static void t_assault_at(unsigned tick) { g_btick = tick; t_ordG = 0; ai_assault_step(); }

TEST(ai_hard_parks_before_a_held_bridge_and_its_fighters_fight_on)
{
	static char buf[1 << 16];
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	t_bridge_battle(1);
	REQUIRE(g_ncross == 1);
	CHECK(t_south() != 0); CHECK_EQ(g_crossBank[0][30 * 80 + 21], 3 - t_south());
	t_assault_at(25);
	CHECK_EQ(g_asPhase[2], 0);                                      // looked at every 50 steps
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 1); CHECK_EQ(g_asCross[2], 1); CHECK(as_parked(2)); CHECK(!as_parked(1));
	CHECK_EQ(t_ordG, 4);                                            // sent back to its place
	CHECK_EQ(g_crossBank[0][t_ordY * 80 + t_ordX], t_south());
	CHECK(t_ordY >= 46 && t_ordY <= 50); CHECK(t_ordX >= 15 && t_ordX <= 26);   // 6 cells out from the mouth
	CHECK(!as_near(0, t_ordX, t_ordY, 2));
	CHECK_EQ(FIG(4)[0x31], ST_IDLE); CHECK_EQ(FIG(4)[0x174], 0); CHECK_EQ(FIG(4)[0x164], 0);   // the chasers stop
	CHECK_EQ(FIG(5)[0x31], ST_IDLE);
	CHECK_EQ(FIG(8)[0x31], ST_MELEE); CHECK_EQ(FIG(8)[0x174], 6);   // the one fighting at the mouth fights on
	CHECK_EQ(GROUP_HUNTING(4), 0); CHECK_EQ(g_aiCharged[4], 0);
	CHECK_EQ(FIG(6)[0x31], ST_IDLE);                                // yours untouched
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 0);                   // parked: no charge over the bridge
	t_assault_at(100);
	CHECK_EQ(t_ordG, 0);                                            // already going there: no new order
	CHECK(g_asIn[2] != 0);                                          // nobody walking: in place
	t_unit_at(6, 20, 43);                                           // your pikeman steps off the bridge onto its side
	t_assault_at(150);
	CHECK_EQ(g_asPhase[2], 1); CHECK_EQ(g_asIn[2], 100);            // still a defender of it: the assault holds
	t_unit_at(6, 25, 50);                                           // well over on its side
	t_assault_at(200);
	CHECK_EQ(g_asPhase[2], 0); CHECK_EQ(G_THINK(4), 199);           // fought as usual
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK(strstr(buf, "ai Hard: side 2 parks before crossing 1 at") != NULL);
	CHECK(strstr(buf, "(2 enemy units, 60 men there; its melee 70 men)") != NULL);
	CHECK(strstr(buf, "ai Hard: side 2 in place before crossing 1") != NULL);
	CHECK(strstr(buf, "ai Hard: side 2 ends its assault on crossing 1 (an enemy came over to its side)") != NULL);
}

TEST(ai_hard_no_storm_from_across_the_field_or_while_walking)
{
	t_bridge_battle(1);
	t_men(6, 5); t_men(7, 0); FIG(7)[0x31] = ST_DYING;             // 5 of yours at the bridge
	t_unit_at(4, 20, 75); t_unit_at(5, 22, 75); t_unit_at(8, 21, 74); FIG(8)[0x31] = ST_CHASE;   // its army far south
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 0); CHECK_EQ(t_ordG, 0);                 // too far: the usual advance
	t_assault_at(850);
	CHECK_EQ(g_asPhase[2], 0);
	t_unit_at(4, 20, 52); t_unit_at(5, 22, 52); t_unit_at(8, 21, 52);   // come up
	t_assault_at(900);
	CHECK_EQ(g_asPhase[2], 1); CHECK_EQ(t_ordG, 4);
	FIG(4)[0x31] = ST_MARCH;                                        // walking to its place
	t_assault_at(950);
	CHECK_EQ(g_asPhase[2], 1); CHECK_EQ(g_asIn[2], 0);              // no storm while walking
	FIG(4)[0x31] = ST_IDLE;
	t_assault_at(1000);
	CHECK_EQ(g_asPhase[2], 2);                                      // in place, 5 men there: storm
}

TEST(ai_a_unit_stuck_short_of_its_place_does_not_hold_the_storm)
{
	t_bridge_battle(1);
	t_men(6, 5); t_men(7, 0); FIG(7)[0x31] = ST_DYING;             // 5 of yours at the bridge
	t_unit_at(4, 20, 52); t_unit_at(5, 22, 52); t_unit_at(8, 21, 52);
	t_assault_at(50);
	REQUIRE(g_asPhase[2] == 1);
	FIG(4)[0x31] = ST_MARCH; FIG(5)[0x31] = ST_IDLE; FIG(8)[0x31] = ST_IDLE;
	t_assault_at(100);
	CHECK_EQ(g_asIn[2], 0);                                         // fig 4 walking (it hasn't been seen stuck yet)
	for (unsigned t = 150; t < 200; t += 50) { FIG(4)[0x31] = t % 100 ? ST_WAIT : ST_MARCH; t_assault_at(t); }
	CHECK_EQ(g_asIn[2], 0);
	FIG(4)[0x31] = ST_MARCH;                                        // still flipping on the same cell...
	t_assault_at(200);
	CHECK_EQ(g_asPhase[2], 2);                                      // ...stuck AS_STUCK steps: in place, it storms
}

TEST(ai_hard_storms_the_bridge_when_its_defenders_are_down_to_half)
{
	static char buf[1 << 16];
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	t_bridge_battle(1);
	t_assault_at(50);
	REQUIRE(g_asPhase[2] == 1);
	t_men(7, 5);                                                    // 35 men left there: half of its 70
	t_assault_at(100);
	CHECK_EQ(g_asPhase[2], 2); CHECK(g_aiCharge[2]); CHECK(!g_aiChargeAtt[2]);
	CHECK_EQ(G_THINK(4), 199);                                      // every melee group thinks at its next step
	CHECK(!as_parked(2));
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);                                        // and charges
	t_men(7, 30);
	t_assault_at(150);
	CHECK_EQ(g_asPhase[2], 2);                                      // a storm runs to the end
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK(strstr(buf, "ai Hard: side 2 charges (storms crossing 1: 35 enemy men there, its melee 70)") != NULL);
}

TEST(ai_assault_works_for_any_lord)
{
	t_bridge_battle(1);
	FIG(4)[0x2c] = FIG(5)[0x2c] = FIG(8)[0x2c] = 5; GRP_B(4, 0x00) = 5;   // lord 5's army, not side 2
	g_aiCharge[2] = FALSE; g_aiCharge[5] = TRUE;
	map_read();
	CHECK_EQ(g_mapSide[0], 1); CHECK_EQ(g_mapSide[1], 5);
	t_assault_at(50);
	CHECK_EQ(g_asPhase[5], 1); CHECK(as_parked(5)); CHECK(!as_parked(2)); CHECK_EQ(t_ordG, 4);
	t_men(7, 5);
	t_assault_at(100);
	CHECK_EQ(g_asPhase[5], 2); CHECK(!as_parked(5));                 // storms
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1);
}

TEST(ai_hard_archers_soften_the_bridge_before_the_storm)
{
	t_bridge_battle(1);
	t_typed(t_bow(1, 2, 20, 62, 2, 15), 5, 25); t_typed(t_bow(2, 2, 21, 62, 2, 15), 5, 25);   // 50 archers, range 15
	t_group(2, 2, 1, 2, 20, 62);
	t_men(7, 5);                                                    // already down to half
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 1);                                      // with archers: they shoot first
	int x = 0, y = 0;
	REQUIRE(as_soften_place(2, 2, 15, 20, 62, &x, &y));
	CHECK_EQ(g_crossBank[0][y * 80 + x], t_south());
	map_cross *m = &g_cross[0];
	int fx = t_south() == 1 ? m->bx : m->ax, fy = t_south() == 1 ? m->by : m->ay, ex, ey, dx, dy;
	CHECK(cheb(x, y, fx, fy) <= 15 - AS_REACH);                     // the far end well inside range
	CHECK_EQ(cheb(x, y, fx, fy), 15 - AS_REACH);                    // as far back as that allows
	as_lane(0, t_south(), &ex, &ey, &dx, &dy);
	int cr = (x - m->x) * dy - (y - m->y) * dx;
	CHECK(cr * cr >= AS_LANE * AS_LANE * (dx * dx + dy * dy));      // beside the lane, not in it
	t_ai_think_archers();
	CHECK_EQ(t_ordG, 2); CHECK_EQ(t_ordX, x); CHECK_EQ(t_ordY, y);
	t_assault_at(100);                                              // in place
	t_assault_at(100 + AS_SOFTEN - 50);
	CHECK_EQ(g_asPhase[2], 1);
	t_assault_at(100 + AS_SOFTEN);
	CHECK_EQ(g_asPhase[2], 2);                                      // after about 10 s of shooting: storm
	t_bridge_battle(1);                                             // not parked: no such place
	CHECK(!as_soften_place(2, 2, 15, 20, 62, &x, &y));
}

TEST(ai_hard_storms_after_the_long_wait_only_when_stronger)
{
	t_bridge_battle(1);
	t_typed(t_bow(1, 2, 20, 62, 2, 15), 5, 25); t_typed(t_bow(2, 2, 21, 62, 2, 15), 5, 25);
	t_group(2, 2, 1, 2, 20, 62);                                    // 120 men to your 60, but 60 at the bridge
	t_assault_at(50); t_assault_at(100);
	t_assault_at(100 + AS_WAIT - 50);
	CHECK_EQ(g_asPhase[2], 1);
	t_assault_at(100 + AS_WAIT);
	CHECK_EQ(g_asPhase[2], 2);
	t_bridge_battle(1);
	t_typed(t_bow(1, 2, 20, 62, 2, 15), 5, 5); t_group(2, 2, 1, 1, 20, 62);   // 75 men to your 60...
	memset(FIG(2), 0, 0x1b0);
	t_typed(t_human(t_unit(9, 1, 30, 20, 9)), 4, 30); t_group(9, 1, 2, 1, 30, 20);   // ...and 90 of yours
	t_assault_at(50); t_assault_at(100);
	t_assault_at(100 + 3 * AS_WAIT);
	CHECK_EQ(g_asPhase[2], 1);                                      // weaker: it keeps waiting
}

TEST(ai_hard_storms_a_bridge_that_comes_free_and_ignores_one_you_come_over)
{
	static char buf[1 << 16];
	InitializeCriticalSection(&g_blogLock);
	g_blogBuf = buf; g_blogLen = 0; g_blog = (FILE *)1;
	t_bridge_battle(1);
	t_assault_at(50);
	REQUIRE(g_asPhase[2] == 1);
	t_unit_at(6, 20, 30); t_unit_at(7, 21, 30);                     // yours step back from it
	t_assault_at(100);
	CHECK_EQ(g_asPhase[2], 2); CHECK(g_aiCharge[2]);                // over it
	buf[g_blogLen] = 0;
	g_blog = NULL; g_blogBuf = NULL; g_blogLen = 0;
	DeleteCriticalSection(&g_blogLock);
	CHECK(strstr(buf, "ai Hard: side 2 charges (storms crossing 1: it is free)") != NULL);
	t_bridge_battle(1);
	t_typed(t_human(t_unit(9, 1, 30, 50, 9)), 4, 30); t_group(9, 1, 2, 1, 30, 50);   // yours on its side
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 0); CHECK_EQ(t_ordG, 0);                 // fought as usual
	CHECK_EQ(FIG(4)[0x31], ST_CHASE);
}

TEST(ai_hard_defending_side_has_no_assault)
{
	t_bridge_battle(1); AI_RATIO = 0; g_aiCharge[2] = FALSE;        // defending, no charge
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 0); CHECK_EQ(t_ordG, 0);                 // its defence rules as before
	CHECK_EQ(FIG(4)[0x31], ST_CHASE);
	CHECK(!as_parked(2));
	t_bridge_battle(1);
	t_assault_at(50);
	REQUIRE(g_asPhase[2] == 1);
	AI_RATIO = 0; g_aiCharge[2] = FALSE;                            // turned to defence while parked
	t_assault_at(100);
	CHECK_EQ(g_asPhase[2], 0);
}

TEST(ai_normal_sieges_and_your_side_have_no_assault)
{
	t_bridge_battle(0);                                             // Normal
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 0); CHECK_EQ(t_ordG, 0); CHECK_EQ(FIG(4)[0x31], ST_CHASE);
	t_bridge_battle(1); *(int *)0x0053e8fc = 1;                     // a siege
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 0); CHECK_EQ(FIG(4)[0x31], ST_CHASE);
	*(int *)0x0053e8fc = 0;
	t_bridge_battle(1);                                             // the bridge held by the computer, you attacking
	FIG(6)[0x31] = ST_CHASE; FIG(6)[0x174] = 8;
	t_assault_at(50);
	CHECK_EQ(g_asPhase[1], 0); CHECK_EQ(FIG(6)[0x31], ST_CHASE);    // your units never park
}

TEST(ai_hard_archers_fire_only_inside_reach_and_stand_down_from_a_far_target)
{
	t_ai_battle(1);
	t_bow(1, 2, 20, 60, 2, 15); t_bow(2, 2, 21, 60, 2, 15); t_group(2, 2, 1, 2, 20, 60);
	t_unit(4, 2, 20, 52, 4); t_group(4, 2, 3, 1, 20, 52);           // its melee in front
	t_human(t_unit(6, 1, 20, 46, 6)); t_group(6, 1, 3, 1, 20, 46);  // yours 14 cells off: in range 15, not 2 inside
	FIG(1)[0x31] = 17; FIG(1)[0x174] = 6;                           // holding a fire order on it (shoots nothing)
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 0);                                          // no new order the game won't shoot
	CHECK_EQ(FIG(1)[0x31], ST_IDLE); CHECK_EQ(FIG(1)[0x174], 0);    // idle: the game shoots what is in range
	t_unit_at(6, 20, 47); GRP_S(6, 0x20) = 47;                      // 13 cells
	FIG(1)[0x31] = 17; FIG(1)[0x174] = 6;
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 6);
	CHECK_EQ(FIG(1)[0x31], 17);                                     // a target in reach is kept
	CHECK_EQ(ai_fire_ready(2, 6), 2);
	t_unit_at(2, 21, 61);                                           // one archer 14 off: half the group still fires
	CHECK_EQ(ai_fire_ready(2, 6), 1);
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 6);
}

TEST(ai_hard_a_flank_far_along_its_bank_doesnt_call_off_the_assault)
{
	t_bridge_battle(1);
	t_typed(t_human(t_unit(9, 1, 70, 60, 9)), 4, 30); t_group(9, 1, 2, 1, 70, 60);   // over the river far east
	t_assault_at(50);
	CHECK_EQ(g_asPhase[2], 1);
	t_unit_at(9, 31, 52);                                           // 9 cells from its swordsmen
	t_assault_at(100);
	CHECK_EQ(g_asPhase[2], 0);
}

TEST(ai_hard_jam_scan_catches_a_route_planned_while_walking)
{
	t_bridge_battle(1);
	FIG(4)[0x31] = ST_CHASE; FIG(4)[0x174] = 6; FIG(4)[0x164] = 1; FIG_S(4, 0x36) = 110;
	g_btick = AS_JAM_EVERY + 1; jam_scan();
	CHECK_EQ(FIG(4)[0x31], ST_CHASE);                               // looked at every AS_JAM_EVERY steps
	g_btick = 2 * AS_JAM_EVERY; jam_scan();
	CHECK_EQ(FIG(4)[0x31], ST_WAIT); CHECK_EQ(FIG(4)[0x2f], ST_CHASE);
	FIG(4)[0x31] = ST_CHASE; FIG(4)[0x164] = 1; g_ncross = 0;       // no crossings on the field: not looked at
	jam_scan();
	CHECK_EQ(FIG(4)[0x31], ST_CHASE);
}

TEST(ai_hard_chaser_waits_instead_of_a_long_way_round_its_own_men)
{
	t_bridge_battle(1);
	FIG(4)[0x31] = ST_CHASE; FIG(4)[0x174] = 6; FIG(4)[0x164] = 1; FIG_S(4, 0x36) = 110;   // 110 steps round
	jam_check(4);
	CHECK_EQ(FIG(4)[0x31], ST_WAIT); CHECK_EQ(FIG(4)[0x2f], ST_CHASE); CHECK_EQ(FIG(4)[0x164], 0);   // waits its turn
	CHECK(FIG(4)[0x30] > 0);
	FIG(4)[0x31] = ST_CHASE; FIG(4)[0x164] = 1; FIG_S(4, 0x36) = 14;   // a short route: kept
	jam_check(4);
	CHECK_EQ(FIG(4)[0x31], ST_CHASE); CHECK_EQ(FIG(4)[0x164], 1);
	FIG_S(4, 0x36) = 110;
	t_human(FIG(4));                                                // yours: kept
	jam_check(4);
	CHECK_EQ(FIG(4)[0x31], ST_CHASE);
	FIG(4)[0x13] = 0;
	for (int y = 39; y <= 41; y++) { BCELL(20, y)[1] = FL_WATER; BCELL(21, y)[1] = FL_WATER; }   // no bridge: far on an empty field too
	jam_check(4);
	CHECK_EQ(FIG(4)[0x31], ST_CHASE); CHECK_EQ(FIG(4)[0x164], 1);
	for (int y = 39; y <= 41; y++) { BCELL(20, y)[1] = 0; BCELL(21, y)[1] = 0; }
	g_aiLevel = 0;                                                  // Normal: kept
	jam_check(4);
	CHECK_EQ(FIG(4)[0x31], ST_CHASE);
}

TEST(ai_legendary_targets_only_within_reach_of_the_nearest_enemies)
{
	t_ai_battle(2);
	t_typed(t_unit(4, 2, 40, 40, 4), 6, 30); t_group(4, 2, 4, 1, 40, 40);              // computer knights
	t_typed(t_human(t_unit(6, 1, 45, 40, 6)), 3, 30); t_group(6, 1, 3, 1, 45, 40);     // your swords, 5 cells away
	t_typed(t_human(t_archer(7, 1, 56, 40, 7)), 5, 30); t_group(7, 1, 1, 1, 56, 40);   // your archers, 16 away
	CHECK_EQ(ai_pick_target(4), 6);                                 // past the swords + 10: not across the field
	t_unit_at(7, 55, 40); GRP_S(7, 0x1e) = 55;                      // 15 away: within reach
	CHECK_EQ(ai_pick_target(4), 7);
}

TEST(ai_legendary_targets_spread_at_most_two_groups_on_one)
{
	t_ai_battle(2);
	t_typed(t_unit(4, 2, 40, 40, 4), 4, 30); t_group(4, 2, 2, 1, 40, 40);              // three computer pikemen
	t_typed(t_unit(5, 2, 40, 42, 5), 4, 30); t_group(5, 2, 2, 1, 40, 42);
	t_typed(t_unit(9, 2, 40, 44, 9), 4, 30); t_group(9, 2, 2, 1, 40, 44);
	t_typed(t_human(t_unit(6, 1, 46, 42, 6)), 6, 30); t_group(6, 1, 4, 1, 46, 42);     // your knights, 6 cells away
	t_typed(t_human(t_unit(7, 1, 50, 42, 7)), 3, 30); t_group(7, 1, 3, 1, 50, 42);     // your swords, 10 (they counter pikes)
	CHECK_EQ(g_aiTarget[4] = (BYTE)ai_pick_target(4), 6);           // the knights
	CHECK_EQ(g_aiTarget[5] = (BYTE)ai_pick_target(5), 6);           // a second on them
	CHECK_EQ(ai_pick_target(9), 7);                                 // the third takes the swords
	t_unit_at(7, 70, 42); GRP_S(7, 0x1e) = 70;                      // the swords out of reach: nothing else left
	CHECK_EQ(ai_pick_target(9), 6);
}

TEST(ai_legendary_group_with_no_men_left_does_nothing)
{
	t_ai_battle(2); AI_RATIO = 30;
	t_typed(t_unit(4, 2, 40, 40, 4), 3, 40); t_group(4, 2, 3, 1, 40, 40);
	t_typed(t_human(t_unit(6, 1, 43, 40, 6)), 3, 100); t_group(6, 1, 3, 1, 43, 40);
	g_aiCharge[2] = TRUE;
	t_ai_think_melee(4);                                            // (its men counted: 40)
	CHECK_EQ(t_nCharged, 1);
	t_men(4, 0);
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK_EQ(t_ordG, 0); CHECK_EQ(g_aiBack[4], 0);   // no fall back, no order
}

TEST(ai_legendary_target_walk_charges_at_once_when_engaged_and_hard_has_none)
{
	t_ai_battle(2);
	t_typed(t_unit(4, 2, 40, 40, 4), 6, 30); t_group(4, 2, 4, 1, 40, 40);
	t_typed(t_human(t_archer(7, 1, 52, 40, 7)), 5, 30); t_group(7, 1, 1, 1, 52, 40);
	t_typed(t_human(t_unit(6, 1, 42, 40, 6)), 4, 30); t_group(6, 1, 2, 1, 42, 40);     // pikemen 2 cells away
	CHECK_EQ(ai_pick_target(4), 7);                                 // (pikes counter knights: it goes for the archers)
	g_aiCharge[2] = g_aiChargeAtt[2] = TRUE; AI_RATIO = 30;
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
	CHECK_EQ(t_nCharged, 1); CHECK(g_aiCharge[2]); CHECK(!g_aiChargeAtt[2]);  // (made to charge by the shots)
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
	t_men(4, 13);                                                   // down to 32%, but the side has 103 men to your 100
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 1); CHECK_EQ(g_aiBack[4], 0);              // winning: it fights on
	t_men(6, 103);                                                  // as many as the side: it falls back
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
	t_unit_at(3, 47, 40); GRP_S(3, 0x1e) = 47;                      // your swords 7 cells away: they come first
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 3);
	t_unit_at(3, 50, 40); GRP_S(3, 0x1e) = 50;
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

/* ================================ burning oil (Hard, sieges too) */

/* oil poured at x,y: that cell and the four beside it burn (the game's cell type 10) */
static void t_oil(int x, int y, BYTE t)
{
	BCELL(x, y)[7] = t; BCELL(x - 1, y)[7] = t; BCELL(x + 1, y)[7] = t; BCELL(x, y - 1)[7] = t; BCELL(x, y + 1)[7] = t;
}

TEST(ai_hard_soldiers_step_out_of_burning_oil)
{
	t_ai_battle(1); *(int *)0x0053e8fc = 1;                         // a siege
	t_human(t_unit(60, 1, 40, 49, 9));                              // your men on the wall
	t_unit(5, 2, 40, 52, 3); FIG(5)[0x31] = ST_CHASE; FIG(5)[0x174] = 60;   // the computer's, chasing them
	t_oil(40, 52, 10);
	fire_step();
	CHECK_EQ(FIG(5)[0x31], ST_MARCH);
	int x = FIG_S(5, 0x24), y = FIG_S(5, 0x26);
	CHECK(!fire_at(x, y));                                          // out of the fire
	CHECK(!fire_near(x, y, 1));                                     // and not beside it (2 steps away)
	CHECK_EQ(cheb(40, 52, x, y), 2);
	CHECK_EQ(FIG(5)[0x174], 0);
	CHECK_EQ(g_bfire, 1);
	fire_step();                                                    // on its way out: not sent again
	CHECK_EQ(FIG_S(5, 0x24), x); CHECK_EQ(FIG_S(5, 0x26), y); CHECK_EQ(g_bfire, 1);
	t_unit(6, 2, 30, 30, 3); FIG(6)[0x31] = ST_MELEE;               // spreading fire (0x11), in a fight: steps out too
	t_oil(30, 30, 0x11);
	fire_step();
	CHECK_EQ(FIG(6)[0x31], ST_MARCH); CHECK(!fire_at(FIG_S(6, 0x24), FIG_S(6, 0x26)));
}

TEST(ai_fire_leaves_your_units_engines_and_normal_alone)
{
	t_ai_battle(1); *(int *)0x0053e8fc = 1;
	t_human(t_unit(5, 1, 40, 52, 3));                               // yours: left to you
	t_unit(7, 2, 20, 52, 4); FIG(7)[0x12] = 8;                      // a siege engine
	t_oil(40, 52, 10); t_oil(20, 52, 10);
	fire_step();
	CHECK_EQ(FIG(5)[0x31], ST_IDLE); CHECK_EQ(FIG(7)[0x31], ST_IDLE);
	t_ai_battle(0);                                                 // Normal: the game's own
	t_unit(8, 2, 60, 52, 5); t_oil(60, 52, 10);
	fire_step();
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	t_ai_battle(1); *(int *)0x00553030 = 1;                         // multiplayer: single player only
	fire_step();
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	*(int *)0x00553030 = 0;
}

TEST(ai_fire_boxed_in_unit_stays_and_a_unit_walking_through_goes_on)
{
	t_ai_battle(1);
	t_unit(5, 2, 40, 52, 3); t_oil(40, 52, 10);
	for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) BCELL(40 + dx, 52 + dy)[1] |= FL_BLOCKED;
	fire_step();                                                    // walled in: nowhere to go
	CHECK_EQ(FIG(5)[0x31], ST_IDLE); CHECK_EQ(g_bfire, 0);
	t_unit(6, 2, 20, 52, 4); t_oil(20, 52, 10);
	FIG(6)[0x31] = ST_MARCH; FIG_S(6, 0x24) = 20; FIG_S(6, 0x26) = 60;   // walking on, through the fire
	g_btick = 100;
	fire_step();
	CHECK_EQ(FIG_S(6, 0x26), 60);                                   // just stepped in: goes on
	g_btick = 100 + FIRE_PASSING;                                   // stuck there: steps out
	fire_step();
	CHECK(FIG_S(6, 0x26) != 60); CHECK(!fire_at(FIG_S(6, 0x24), FIG_S(6, 0x26)));
}

TEST(ai_fire_soldiers_dont_walk_back_in_while_it_burns)
{
	t_ai_battle(1); *(int *)0x0053e8fc = 1;
	t_human(t_unit(60, 1, 40, 47, 9));
	t_unit(5, 2, 40, 52, 3); t_oil(40, 52, 10);
	fire_step();
	int hx = FIG_S(5, 0x24), hy = FIG_S(5, 0x26);
	t_unit_at(5, hx, hy); FIG(5)[0x31] = ST_IDLE;                   // got there
	FIG(5)[0x31] = ST_MARCH; FIG_S(5, 0x24) = 40; FIG_S(5, 0x26) = 52;   // sent back into the fire
	fire_step();
	CHECK_EQ(FIG_S(5, 0x24), hx); CHECK_EQ(FIG_S(5, 0x26), hy);    // turned back to where it went
	FIG(5)[0x31] = ST_CHASE; FIG(5)[0x174] = 60;                    // chasing your men on the wall, across the fire
	t_unit_at(5, 40, 54);
	fire_step();
	CHECK_EQ(FIG(5)[0x31], ST_MARCH); CHECK_EQ(FIG_S(5, 0x24), 40); CHECK_EQ(FIG_S(5, 0x26), 54);   // stops beside it
	t_oil(40, 52, 0);                                               // burnt out
	FIG(5)[0x31] = ST_MARCH; FIG_S(5, 0x24) = 40; FIG_S(5, 0x26) = 52;
	fire_step();
	CHECK_EQ(FIG_S(5, 0x26), 52);                                   // free to go again
	CHECK_EQ(g_fHold[5], 0);
}
TEST(ai_fire_siege_marchers_at_the_wall_step_out_too)
{
	t_ai_battle(1); *(int *)0x0053e8fc = 1;
	t_unit(5, 2, 40, 58, 3); t_oil(40, 58, 10);
	FIG(5)[0x31] = 9; FIG_S(5, 0x24) = 40; FIG_S(5, 0x26) = 56;    // a siege's march to its place at the wall foot
	FIG(5)[0x18f] = 1; *(int *)(FIG(5) + 0x190) = (56 * 80 + 40) * 8;   // working on a moat cell
	g_btick = 1000;
	fire_step();                                                    // just stepped in, its place out of the fire: goes on
	CHECK_EQ(FIG(5)[0x31], 9);
	t_unit_at(5, 41, 58); g_btick = 1020;                           // shuffles to the next burning cell: still in it
	fire_step();
	CHECK_EQ(FIG(5)[0x31], 9);
	g_btick = 1000 + FIRE_PASSING;                                  // 40 steps in the fire: out
	fire_step();
	CHECK_EQ(FIG(5)[0x31], ST_MARCH); CHECK(!fire_at(FIG_S(5, 0x24), FIG_S(5, 0x26)));
	CHECK_EQ(FIG(5)[0x18f], 0); CHECK_EQ(*(int *)(FIG(5) + 0x190), 0);   // the moat work stops
	int hx = FIG_S(5, 0x24), hy = FIG_S(5, 0x26);
	t_unit_at(5, hx, hy);
	FIG(5)[0x31] = 9; FIG_S(5, 0x24) = 40; FIG_S(5, 0x26) = 58;     // marched back into the fire
	fire_step();
	CHECK_EQ(FIG(5)[0x31], ST_MARCH); CHECK_EQ(FIG_S(5, 0x24), hx); CHECK_EQ(FIG_S(5, 0x26), hy);
}
TEST(ai_hard_archers_out_of_reach_walk_up_to_their_soldiers_fight)
{
	t_ai_battle(2);                                                 // Legendary (Hard's rule)
	t_bow(1, 2, 20, 60, 2, 15); t_bow(2, 2, 21, 60, 2, 15); t_bow(3, 2, 22, 60, 2, 15);
	t_group(2, 2, 1, 3, 21, 60);                                    // its archers, left of the field
	t_unit(4, 2, 40, 50, 4); t_group(4, 2, 3, 1, 40, 50);           // its pikemen at the bridge...
	t_human(t_unit(5, 1, 40, 49, 5)); t_group(5, 1, 3, 1, 33, 52);  // ...fighting your swords (your group's
	FIG(4)[0x31] = ST_MELEE; FIG(5)[0x31] = ST_MELEE;               // centre is 12 cells from the archers')
	FIG(4)[0x17e] = 5; FIG(5)[0x17e] = 4;
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 0);                                          // no fire order none of them can carry out
	CHECK_EQ(t_ordG, 2);                                            // they walk up to the fight
	CHECK(cheb(t_ordX, t_ordY, 40, 49) <= 13); CHECK(cheb(t_ordX, t_ordY, 40, 49) >= 12);
	t_unit_at(1, 30, 52); t_unit_at(2, 31, 52); t_unit_at(3, 32, 52); GRP_S(2, 0x1e) = 31; GRP_S(2, 0x20) = 52;
	t_ai_think_archers();
	CHECK_EQ(t_fireAt, 5);                                          // in range now: they shoot
}
TEST(ai_hard_timed_charge_waits_until_the_line_is_close)
{
	t_ai_battle(2); AI_RATIO = 30;                                  // Legendary, attacking
	t_unit(4, 2, 20, 40, 4); t_group(4, 2, 3, 1, 20, 40);           // its swords 30 cells from you
	t_typed(t_unit(5, 2, 24, 44, 5), 0, 32); t_group(5, 2, 2, 1, 24, 44);   // its peasants a little ahead
	t_human(t_unit(6, 1, 50, 40, 6)); t_group(6, 1, 3, 1, 50, 40);
	g_btick = 100;
	t_ai_think_melee(4);                                            // (attacking from here)
	g_btick = 100 + AI_CHARGE_STEPS;                                // 60 s on, the line still 30 cells off
	t_ai_think_melee(4);
	CHECK_EQ(t_nCharged, 0); CHECK(!g_aiCharge[2]);                 // no charge from there: it advances
	CHECK_EQ(GRP_S(4, 0x22), 26);                                   // (6 cells on, as ordered at the first think)
	t_unit_at(4, 31, 40); GRP_S(4, 0x1e) = 31; GRP_S(4, 0x22) = 31; // the line 19 cells off: the timed charge
	t_unit_at(5, 32, 41); GRP_S(5, 0x1e) = 32; GRP_S(5, 0x20) = 41;
	t_ai_think_melee(4);
	CHECK(g_aiCharge[2]);
}