/* t_battle.c - tier 2: the battle fixes on a made-up battlefield in fake game memory. Our own code. Included by
 * tests.c. Each test starts with the battlefield empty and flat, no figures, every battle fix on, single player. */

/* ================================ walking distances (dm_*: by the game's movement rules) */

static void t_clear_flags(void) { for (int c = 0; c < 6400; c++) ((BYTE *)0x005440e1)[c * 8] = 0; }

TEST(distance_open_field)
{
	t_unit(1, 1, 10, 10, 1);
	CHECK_EQ(dm_from(1, 10 * 80 + 20), 10);    // straight
	CHECK_EQ(dm_from(1, 15 * 80 + 15), 5);     // diagonal steps count one
	dm_from(1, -1);                            // the whole map
	CHECK_EQ(g_dscratch[79 * 80 + 79], 69);
	CHECK_EQ(g_dscratch[10 * 80 + 10], 0);
}

TEST(distance_round_walls_and_through_nothing_else)
{
	t_unit(1, 1, 10, 10, 1);
	t_flags(15, 0, 15, 79, FL_BLOCKED);
	BCELL(15, 40)[1] = 0;                      // one gap
	CHECK_EQ(dm_from(1, 10 * 80 + 20), 60);    // down to the gap and back up
	static const BYTE stops[] = { FL_BLOCKED, FL_WATER, FL_GATE, FL_SIEGE };
	for (int i = 0; i < 4; i++) {
		t_clear_flags();
		t_flags(15, 0, 15, 79, stops[i]);
		CHECK_EQ(dm_from(1, 10 * 80 + 20), DM_FAR);
	}
	t_clear_flags();
	t_flags(15, 0, 15, 79, FL_MOAT);           // moats are crossed (the game fills them)
	CHECK_EQ(dm_from(1, 10 * 80 + 20), 10);
}

TEST(distance_height_steps)
{
	int a = 10 * 80 + 10, b = 10 * 80 + 11;
	t_height(10, 10, 10, 10, 0);
	t_height(11, 10, 11, 10, 1); CHECK(dm_height_ok(a, b));    // one level up
	t_height(11, 10, 11, 10, 2); CHECK(!dm_height_ok(a, b));   // two: a cliff
	CHECK(!dm_height_ok(b, a));                                 // and down it
	t_height(11, 10, 11, 10, 5); CHECK(dm_height_ok(a, b));    // level 5 is entered from any level
	CHECK(!dm_height_ok(b, a));                                 // but not left two levels down
	t_height(10, 10, 10, 10, 4); CHECK(dm_height_ok(a, b) && dm_height_ok(b, a));
	// a plateau two levels up, reached only by a ramp of level 1 on its far side
	t_unit(1, 1, 10, 40, 1);
	t_height(20, 30, 40, 50, 2);
	t_height(41, 39, 41, 41, 1);
	int d = dm_from(1, 40 * 80 + 30);
	CHECK(d != DM_FAR && d > 30);               // round to the ramp, not straight up
}

TEST(distance_to_a_target_counts_its_own_cell)
{
	t_unit(1, 1, 10, 10, 1);
	t_unit(2, 2, 20, 10, 2);
	BCELL(20, 10)[1] = FL_GATE;                 // the enemy stands on a cell a walk can't enter (a gate)
	CHECK_EQ(dm_dist(2, 1), 10);                // reached from the cell beside it
	CHECK_EQ(dm_dist(0, 1), DM_FAR);
	CHECK_EQ(dm_dist(0x51, 1), DM_FAR);
	FIG(2)[0x2c] = 0;                           // an empty slot
	CHECK_EQ(dm_dist(2, 1), DM_FAR);
}

TEST(distance_depth_cap)
{
	t_unit(1, 1, 10, 10, 1);
	g_dmMaxDepth = 3;
	CHECK_EQ(dm_from(1, 10 * 80 + 13), 3);
	CHECK_EQ(dm_from(1, 10 * 80 + 15), DM_FAR);
	g_dmMaxDepth = 0x7fff;
}

TEST(target_pick_by_walking_distance)
{
	t_unit(1, 1, 10, 40, 1);
	t_unit(2, 2, 14, 40, 2);                    // 4 cells away, behind a wall
	t_unit(3, 2, 10, 48, 3);                    // 8 cells away, open ground
	t_flags(12, 20, 12, 60, FL_BLOCKED);
	CHECK_EQ(dm_pick(1), 3);
	t_clear_flags();
	CHECK_EQ(dm_pick(1), 2);
}

TEST(target_pick_rules)
{
	t_unit(1, 1, 20, 20, 1);
	t_unit(2, 2, 25, 20, 2);                    // foot soldier, 5 steps
	t_archer(3, 2, 28, 20, 3);                  // archer, 8 steps: counts as 4
	CHECK_EQ(dm_pick(1), 3);
	FIG(3)[0x31] = ST_DYING;                    // dying ones are skipped
	CHECK_EQ(dm_pick(1), 2);
	FIG(3)[0x31] = ST_IDLE; FIG(3)[0x12] = 7;   // so are siege engines
	CHECK_EQ(dm_pick(1), 2);
	FIG(3)[0x12] = 1; FIG(3)[0x2c] = 1;         // and friends
	CHECK_EQ(dm_pick(1), 2);
	g_cAvoid[1] = 2; g_cAvoidUntil[1] = 100; g_btick = 50;   // just given up on: ignored for a while
	CHECK_EQ(dm_pick(1), 0);
	g_btick = 100;
	CHECK_EQ(dm_pick(1), 2);
	t_unit(4, 2, 26, 20, 4);                    // 6 steps but already crowded (+2 per chaser)
	FIG(2)[0x175] = 1;
	CHECK_EQ(dm_pick(1), 2);                    // 5 + 1 = 6: a tie keeps the first
	FIG(2)[0x175] = 2;
	CHECK_EQ(dm_pick(1), 4);                    // 5 + 2 = 7 > 6
}

/* ================================ Mop-Up */

TEST(mopup_target_hook)
{
	t_unit(1, 1, 20, 20, 1);
	t_unit(2, 2, 25, 20, 2);
	FIG_CUR = 1;
	t_stub_ret = 77;
	CHECK_EQ(h_target(), 2);
	CHECK_EQ(*(int *)0x0056d5d0, 2);
	CHECK_EQ(FIG(2)[0x175], 2);                 // one more chaser on it
	CHECK_EQ(t_calls, 0);
	g_fixMopUp = FALSE;                         // off: the game's own pick
	CHECK_EQ(h_target(), 77);
	g_fixMopUp = TRUE;
	MP_SESSION = 1;                             // multiplayer without MultiplayerFixes: the game's own
	CHECK_EQ(h_target(), 77);
	g_mpFixes = TRUE;
	CHECK_EQ(h_target(), 2);
	MP_SESSION = 0; g_mpFixes = FALSE;
	g_dmFullThisStep = DM_FULL_PER_STEP;       // over this step's search cap: the game's quick pick
	CHECK_EQ(h_target(), 77);
	g_dmFullThisStep = 0;
	FIG(1)[0x16a] = 1;                          // archers keep the game's pick
	CHECK_EQ(h_target(), 77);
}

TEST(mopup_lets_go_of_a_fallen_target)
{
	t_unit(1, 1, 20, 20, 3);
	t_unit(2, 2, 21, 20, 4);
	GROUP_HUNTING(3) = 1;
	FIG(1)[0x31] = ST_CHASE; FIG(1)[0x174] = 2;
	FIG(2)[0x31] = ST_DYING;
	FIG_CUR = 1;
	h_chase();
	CHECK_EQ(FIG(1)[0x174], 0);                 // let go, so the game's own search picks the next one
	CHECK_EQ(t_calls, 1);
	FIG(1)[0x174] = 2; GROUP_HUNTING(3) = 0;    // not hunting: as the game
	h_chase();
	CHECK_EQ(FIG(1)[0x174], 2);
	GROUP_HUNTING(3) = 1; FIG(2)[0x31] = ST_MELEE;   // a live target is kept
	h_chase();
	CHECK_EQ(FIG(1)[0x174], 2);
}

TEST(mopup_gives_up_on_an_unreachable_target)
{
	t_unit(16, 1, 20, 20, 3);
	t_unit(2, 2, 40, 40, 4);
	t_flags(38, 38, 42, 42, FL_WATER);
	BCELL(40, 40)[1] = 0;                       // on an island
	FIG(16)[0x31] = ST_CHASE; FIG(16)[0x174] = 2;
	g_btick = 0; chase_progress(16);
	g_btick = 160; chase_progress(16);
	CHECK_EQ(FIG(16)[0x174], 2);                // not yet two seconds
	g_btick = 176; chase_progress(16);
	CHECK_EQ(FIG(16)[0x174], 0);
	CHECK_EQ(g_cAvoid[16], 2);
	CHECK_EQ(g_cAvoidUntil[16], 176 + 328);
}

TEST(mopup_keeps_chasing_a_far_target)
{
	t_unit(16, 1, 10, 20, 3);
	t_unit(2, 2, 40, 20, 4);                    // 30 steps, keeping its distance
	FIG(16)[0x31] = ST_CHASE; FIG(16)[0x174] = 2;
	for (g_btick = 0; g_btick <= 800; g_btick += 16) chase_progress(16);
	CHECK_EQ(FIG(16)[0x174], 2);
}

TEST(mopup_wakes_idle_hunters)
{
	g_fixEngage = FALSE;
	t_unit(16, 1, 20, 20, 3);
	t_unit(2, 2, 30, 20, 4);
	GROUP_HUNTING(3) = 1;
	FIG_CUR = 16; g_btick = 0;
	h_idle();
	CHECK_EQ(t_calls, 1);                       // the game's idle step ran first
	CHECK_EQ(FIG(16)[0x31], ST_CHASE);
	t_unit(16, 1, 20, 20, 5);                   // a group not on Mop-Up stays idle
	h_idle();
	CHECK_EQ(FIG(16)[0x31], ST_IDLE);
}

static void __cdecl t_select_regroups(unsigned player)   // the game making a new group 9 of figures 1 and 2
{
	(void)player; t_calls++;
	FIG_S(1, 0x178) = 9; FIG_S(2, 0x178) = 9;
}
TEST(mopup_survives_a_new_selection)
{
	t_unit(1, 1, 20, 20, 3);
	t_unit(2, 1, 21, 20, 3);
	GROUP_HUNTING(3) = 1;
	o_select = t_select_regroups;
	h_select(0);
	CHECK_EQ(GROUP_HUNTING(9), 1);
	FIG_S(1, 0x178) = 3; FIG_S(2, 0x178) = 3; GROUP_HUNTING(9) = 0;
	g_fixMopUp = FALSE;
	h_select(0);
	CHECK_EQ(GROUP_HUNTING(9), 0);
}

/* ================================ idle units engage (EngageFix) */

TEST(engage_a_close_enemy)
{
	t_unit(8, 1, 20, 20, 2);
	t_unit(9, 2, 22, 20, 3);
	g_btick = 0;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
	CHECK_EQ(FIG(8)[0x174], 9);
	CHECK_EQ(g_eTarget[8], 9);
}

TEST(engage_range_by_kind)
{
	t_unit(8, 1, 20, 20, 2);
	t_unit(9, 2, 24, 20, 3);                    // 4 cells: too far for a foot soldier
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	FIG(8)[0x12] = 6;                           // a knight reaches 4
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
	t_unit(8, 1, 20, 20, 2);
	t_archer(9, 2, 25, 20, 3);                  // enemy archers shooting at it: 5
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
}

TEST(engage_never_past_the_leash)
{
	t_unit(8, 1, 20, 20, 2);
	g_btick = 0; engage_idle(8);                // its place: 20,20
	t_unit_at(8, 24, 20);                       // pushed 4 cells on, idle every step since
	for (g_btick = 1; g_btick < 8; g_btick++) engage_idle(8);
	t_unit(9, 2, 27, 20, 3);                    // 3 from it, 7 from its place
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	t_unit_at(9, 25, 22);                       // 5 from its place: fair game
	g_btick = 16;
	for (unsigned t = 9; t <= 16; t++) { g_btick = t; engage_idle(8); }
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
}

TEST(engage_at_most_two_on_one_enemy)
{
	t_unit(8, 1, 20, 20, 2);
	t_unit(10, 1, 20, 22, 2); t_unit(11, 1, 20, 18, 2);
	t_unit(9, 2, 22, 20, 3);
	g_eTarget[10] = 9; g_eTarget[11] = 9;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);            // two of ours are on it already
	FIG(9)[0x31] = ST_MELEE;                    // locked in a fight: no bait in that
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
	t_unit(8, 1, 20, 20, 2); g_eTarget[10] = 9; g_eTarget[11] = 9;
	FIG(9)[0x31] = ST_IDLE; t_unit_at(9, 21, 20);   // right beside it
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
}

TEST(engage_only_with_the_odds)
{
	t_unit(8, 1, 20, 20, 2); FIG_S(8, 0x1a0) = 30;                 // our pikemen, 30 men
	t_unit(9, 2, 22, 20, 3); FIG_S(9, 0x1a0) = 30;                 // two enemy units, 60 men
	t_unit(10, 2, 23, 21, 3); FIG_S(10, 0x1a0) = 30;
	g_btick = 0;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);                                // outnumbered: holds its ground
	t_unit(11, 1, 20, 22, 2); FIG_S(11, 0x1a0) = 30;               // a friend 2 cells from the enemy: 60 to 60
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
	t_unit(8, 1, 20, 20, 2); FIG_S(8, 0x1a0) = 30; FIG_S(11, 0x1a0) = 0;
	t_archer(10, 2, 23, 21, 3); FIG_S(10, 0x1a0) = 30;             // archers count half: 30 to 45
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	FIG_S(10, 0x1a0) = 0;                                           // 30 to 30: it goes
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
	t_unit(8, 1, 20, 20, 2); FIG_S(8, 0x1a0) = 30; FIG_S(10, 0x1a0) = 90;
	t_unit_at(9, 21, 20);                                           // right beside it: fought whatever the odds
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
}

TEST(engage_holds_the_line_next_to_an_obstacle)
{
	t_unit(8, 1, 20, 20, 2);                                        // pikemen in a gap, stakes beside them
	t_flags(19, 21, 19, 21, FL_BLOCKED);
	t_unit(9, 2, 23, 20, 3);                                        // a lone knight 3 cells off
	g_btick = 0;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);                                // they stay in the gap
	t_unit_at(9, 21, 20);                                           // right beside them: fought
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
	BCELL(19, 21)[1] = 0;                                           // in the open: they go for one 3 cells off
	t_unit(8, 1, 20, 20, 2); t_unit_at(9, 23, 20);
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
}

TEST(engage_the_whole_formation_holds_by_an_obstacle)
{
	t_unit(10, 1, 20, 20, 2);                                       // a pikeman beside the stakes...
	t_flags(19, 21, 19, 21, FL_BLOCKED);
	t_unit(8, 1, 21, 20, 2);                                        // ...and its neighbour in the open
	t_unit(9, 2, 24, 20, 3);                                        // an enemy 3 cells from the neighbour
	g_btick = 0;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);                                // the formation holds
	FIG_S(10, 0x178) = 4; FIG_S(8, 0x178) = 2;                      // the one at the stakes in another group
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);                               // nothing holds this one
	BCELL(19, 21)[1] = 0;
}

TEST(archers_back_up_a_fight_of_their_side)
{
	t_archer(8, 1, 20, 20, 2);
	t_unit(9, 2, 23, 20, 3);                                        // the nearest enemy, standing
	t_unit(10, 2, 26, 20, 4); FIG(10)[0x31] = ST_MELEE;             // fighting our pikeman 11
	t_unit(11, 1, 27, 20, 5); FIG(11)[0x31] = ST_MELEE;
	FIG(10)[0x17e] = 11; FIG(11)[0x17e] = 10;
	CHECK_EQ(support_target(8, 8), 10);                             // it shoots the one fighting ours
	CHECK_EQ(support_target(8, 5), 0);                              // out of range: the game's pick
	FIG(10)[0x17e] = 9;                                             // in a fight, but not with one of ours
	CHECK_EQ(support_target(8, 8), 0);
}

TEST(archers_shoot_instead_of_chasing_after_a_fight)
{
	g_fixEngage = TRUE;
	t_archer(8, 1, 20, 20, 2);
	t_unit(9, 2, 24, 20, 3);                                        // an enemy 4 cells off after the fight
	t_unit(12, 1, 30, 30, 6);                                       // a pikeman of ours
	CHECK_EQ(pick_filter(PICK_AFTER_FIGHT, 5, 8, 0, 0x1201), 0);     // the archer: no chase, it stands
	CHECK_EQ(pick_filter(PICK_AFTER_FIGHT, 5, 12, 0, 0x1201), 0x1201);   // a soldier: the game's answer
	CHECK_EQ(pick_filter((void *)0x0048c9e7, 40, 8, 0, 0x1201), 0x1201); // the AI's fire order: untouched
	t_unit(10, 2, 26, 20, 4); FIG(10)[0x31] = ST_MELEE;             // the shot: an enemy fighting ours
	t_unit(11, 1, 27, 20, 5); FIG(11)[0x31] = ST_MELEE; FIG(10)[0x17e] = 11;
	CHECK_EQ(pick_filter(PICK_SHOT, 8, 8, 0, 0x1201), 1);
	CHECK_EQ(*(int *)0x0052eff4, 10);
	g_fixEngage = FALSE;
	CHECK_EQ(pick_filter(PICK_AFTER_FIGHT, 5, 8, 0, 0x1201), 0x1201);   // fix off: the game as it was
}

TEST(engage_counts_the_column_behind_the_first_enemy)
{
	t_unit(8, 1, 20, 20, 2); FIG_S(8, 0x1a0) = 30;                 // two of our pikemen at a gap, 60 men
	t_unit(11, 1, 20, 21, 2); FIG_S(11, 0x1a0) = 30;
	t_unit(9, 2, 23, 20, 3); FIG_S(9, 0x1a0) = 30;                 // the head of a column 3 cells off
	t_unit(10, 2, 27, 20, 3); FIG_S(10, 0x1a0) = 30;               // the next 4 cells behind it
	t_unit(12, 2, 28, 21, 3); FIG_S(12, 0x1a0) = 30;               // and 5 behind: 60 to 90
	g_btick = 0;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);                                // they hold the gap
	t_unit_at(10, 40, 20); t_unit_at(12, 40, 22);                  // the head alone: 60 to 30
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_CHASE);
}

TEST(engage_skips_walls_the_dying_and_siege)
{
	t_unit(8, 1, 20, 20, 2);
	t_unit(9, 2, 22, 20, 3);
	BCELL(20, 20)[4] = 1;                       // on a wall
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	BCELL(20, 20)[4] = 0;
	FIG(9)[0x31] = ST_DYING;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	FIG(9)[0x31] = ST_IDLE; FIG(9)[0x12] = 7;
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	FIG(9)[0x12] = 1;
	t_flags(21, 0, 21, 79, FL_WATER);           // across a river
	engage_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
}

static void t_engaged(int me, int ax, int ay, int target)   // me engaged target from its place ax,ay
{
	g_eTarget[me] = target; FIG(me)[0x174] = (BYTE)target; FIG(me)[0x31] = ST_CHASE;
	g_eAnchorX[me] = (short)ax; g_eAnchorY[me] = (short)ay; g_eAnchorOk[me] = 1;
	g_eBestD[me] = 99; g_eSince[me] = g_btick;
}
TEST(engage_chase_leash)
{
	t_unit(8, 1, 25, 20, 2);
	t_unit(9, 2, 27, 20, 3);
	g_btick = 1000;
	t_engaged(8, 20, 20, 9);                    // 5 cells from its place
	CHECK(engage_chase(8));
	CHECK_EQ(FIG(8)[0x31], ST_MARCH);
	CHECK_EQ(FIG_S(8, 0x24), 20); CHECK_EQ(FIG_S(8, 0x26), 20);   // walks back
	CHECK_EQ(g_eIgnore[8], 9);
	CHECK_EQ(g_eIgnoreUntil[8], 1000 + 246);
}

TEST(engage_chase_lets_a_runner_go)
{
	t_unit(8, 1, 21, 20, 2);
	t_unit(9, 2, 24, 20, 3);
	g_btick = 0;
	t_engaged(8, 20, 20, 9);
	CHECK(!engage_chase(8));                    // 3 away: the best so far
	g_btick = 100; CHECK(!engage_chase(8));
	g_btick = 164; CHECK(engage_chase(8));      // no closer for 2 s
	CHECK_EQ(g_eIgnore[8], 9);
	t_unit(9, 2, 24, 20, 3); FIG(9)[0x16a] = 1; // an archer stepping back inside the leash is still chased
	g_btick = 200; t_engaged(8, 20, 20, 9); engage_chase(8);
	g_btick = 400;
	CHECK(!engage_chase(8));
	FIG(9)[0x31] = ST_DYING;                    // target down: back, nothing to ignore
	g_eIgnore[8] = 0;
	CHECK(engage_chase(8));
	CHECK_EQ(g_eIgnore[8], 0);
}

TEST(archer_steps_back_from_a_charge)
{
	t_archer(8, 1, 20, 20, 2);
	t_unit(9, 2, 22, 20, 3);
	FIG(9)[0x31] = ST_MARCH;
	g_btick = 0;
	archer_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_MARCH);
	CHECK_EQ(FIG_S(8, 0x24), 17); CHECK_EQ(FIG_S(8, 0x26), 20);   // 3 cells straight away
	CHECK_EQ(g_aNext[8], 246);
}

TEST(archer_step_back_rules)
{
	t_archer(8, 1, 20, 20, 2);
	t_unit(9, 2, 22, 20, 3);
	FIG(9)[0x31] = ST_IDLE;                     // an enemy standing still is no threat
	archer_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	FIG(9)[0x31] = ST_MARCH;
	t_unit(10, 1, 17, 20, 2);                   // a friend stands where it would go: beside that instead
	archer_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_MARCH);
	CHECK(!(FIG_S(8, 0x24) == 17 && FIG_S(8, 0x26) == 20));
	CHECK(BCELL(FIG_S(8, 0x24), FIG_S(8, 0x26))[5] == 0);
	CHECK(cheb(20, 20, FIG_S(8, 0x24), FIG_S(8, 0x26)) >= 2);
}

TEST(archer_cornered_stands_and_fights)
{
	t_archer(8, 1, 20, 20, 2);
	t_unit(9, 2, 22, 20, 3);
	FIG(9)[0x31] = ST_MARCH;
	t_flags(16, 15, 18, 25, FL_WATER);          // water behind it
	g_btick = 0;
	archer_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);
	CHECK_EQ(g_aNext[8], 82);                   // looks again in a second
}

TEST(archer_behind_stakes_stays_put)
{
	t_archer(8, 1, 20, 53, 2);
	t_unit(9, 2, 20, 51, 3); FIG(9)[0x31] = ST_MARCH;           // 2 cells off, coming at it
	t_flags(0, 52, 79, 52, FL_BLOCKED);                          // a row of stakes between them
	g_btick = 0;
	archer_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_IDLE);                             // it can't walk up: the archer stays and shoots
	CHECK(can_reach_soon(9, 8) == FALSE);
	BCELL(21, 52)[1] = 0;                                        // a gap right beside it: now it can
	archer_idle(8);
	CHECK_EQ(FIG(8)[0x31], ST_MARCH);
	for (int x = 0; x < 80; x++) BCELL(x, 52)[1] = 0;
}

TEST(archer_leaves_melee_once_covered)
{
	t_archer(8, 1, 20, 20, 2);
	t_unit(9, 2, 21, 20, 3);
	FIG(8)[0x31] = ST_MELEE; FIG(8)[0x17e] = 9; FIG(9)[0x31] = ST_MELEE;
	FIG_CUR = 8; g_btick = 0;
	h_melee();                                  // alone: keeps fighting
	CHECK_EQ(FIG(8)[0x31], ST_MELEE);
	CHECK_EQ(t_calls, 1);
	t_unit(10, 1, 22, 21, 2); FIG(10)[0x31] = ST_MELEE;   // a friend beside its enemy
	h_melee();
	CHECK_EQ(FIG(8)[0x31], ST_MARCH);
	CHECK_EQ(t_calls, 1);                       // stepped out instead of the game's melee step
}

/* ================================ group routes (GroupMoveFix) */

TEST(group_line_clear)
{
	CHECK(gr_line_clear(10, 40, 30, 40));
	CHECK(gr_line_clear(10, 40, 30, 55));
	t_flags(18, 30, 22, 50, FL_BLOCKED);        // a forest
	CHECK(!gr_line_clear(10, 40, 30, 40));
	CHECK(gr_line_clear(10, 60, 30, 60));       // passing below it
}

TEST(group_route_round_a_forest)
{
	t_unit(1, 1, 10, 40, 4);
	t_flags(18, 30, 22, 50, FL_BLOCKED);
	g_grSolo = TRUE;
	int n = gr_plan(1, 30, 40);
	REQUIRE(n > 0);
	CHECK_EQ(FIG_S(1, 0x36), n);
	CHECK_EQ(FIG(1)[0x164], 1);
	CHECK_EQ(t_route_legal(1), n);              // every step a legal move, none into the forest
	CHECK_EQ(FIG(1)[0x38], 30); CHECK_EQ(FIG(1)[0x39], 40);   // entry 0 is the goal
	CHECK(n >= 26 && n <= 32);                  // the short way round (26 steps)
}

TEST(group_routes_keep_lanes_together)
{
	t_unit(1, 1, 10, 40, 4);
	t_unit(2, 1, 10, 42, 4);
	t_flags(18, 30, 22, 50, FL_BLOCKED);
	int n1 = gr_plan(1, 30, 40);                // the first sets the group's corridor
	int lead[200], ln = FIG_S(1, 0x36);
	for (int k = 0; k < ln; k++) lead[k] = FIG(1)[0x38 + 2 * k] + 80 * FIG(1)[0x39 + 2 * k];
	int n2 = gr_plan(2, 30, 42);
	REQUIRE(n1 > 0 && n2 > 0);
	CHECK_EQ(t_route_legal(2), n2);
	int outside = 0, shared = 0;
	for (int k = 0; k < FIG_S(2, 0x36); k++) {
		int x = FIG(2)[0x38 + 2 * k], y = FIG(2)[0x39 + 2 * k], closest = 99;
		for (int j = 0; j < ln; j++) { int d = cheb(x, y, lead[j] % 80, lead[j] / 80); if (d < closest) closest = d; }
		if (closest > 3) outside++;
		if (closest == 0) shared++;
	}
	CHECK_EQ(outside, 0);                       // same side of the forest, beside the first
	CHECK(shared * 2 < n2);                     // in its own lane, mostly
}

TEST(group_detour_cells_only)
{
	t_unit(1, 1, 10, 40, 4);
	int out[24];
	g_grSolo = TRUE; g_grAvoid = 40 * 80 + 11; g_grOut = out; g_grOutMax = 14;
	int n = gr_plan(1, 14, 40);
	REQUIRE(n >= 4);
	CHECK_EQ(out[0], 40 * 80 + 14);             // goal first
	int avoided = 1;
	for (int k = 0; k < n; k++) if (out[k] == g_grAvoid) avoided = 0;
	CHECK(avoided);
	CHECK_EQ(FIG_S(1, 0x36), 0);                // the unit's own waypoints untouched
	CHECK_EQ(FIG(1)[0x164], 0);
	g_grOutMax = 2;
	CHECK_EQ(gr_plan(1, 14, 40), 0);            // longer than wanted
	g_grOut = NULL; g_grAvoid = -1; g_grSolo = FALSE;
}

TEST(group_route_when_there_is_none)
{
	t_unit(1, 1, 10, 40, 4);
	t_flags(28, 38, 32, 42, FL_WATER);
	BCELL(30, 40)[1] = 0;
	CHECK_EQ(gr_plan(1, 30, 40), 0);            // an island
	CHECK_EQ(gr_plan(1, 10, 40), 0);            // already there
	CHECK_EQ(gr_plan(1, 80, 40), 0);            // off the field
	CHECK_EQ(gr_plan(1, 5, -1), 0);
}

TEST(group_followers_take_the_leaders_way)
{
	t_unit(1, 1, 10, 40, 4);
	t_unit(2, 1, 10, 42, 4);
	t_flags(18, 30, 22, 50, FL_BLOCKED);
	for (int i = 1; i <= 2; i++) { FIG(i)[0x31] = ST_MARCH; FIG_S(i, 0x24) = 30; FIG_S(i, 0x26) = (short)(38 + 2 * i); }
	gr_route(1);
	gr_route(2);
	REQUIRE(FIG_S(1, 0x36) > 0 && FIG_S(2, 0x36) > 0);
	CHECK(t_route_legal(1) > 0);
	CHECK(t_route_legal(2) > 0);
	// both go round the same end of the forest
	int y1 = 40, y2 = 40;
	for (int k = 0; k < FIG_S(1, 0x36); k++) if (FIG(1)[0x38 + 2 * k] == 20) y1 = FIG(1)[0x39 + 2 * k];
	for (int k = 0; k < FIG_S(2, 0x36); k++) if (FIG(2)[0x38 + 2 * k] == 20) y2 = FIG(2)[0x39 + 2 * k];
	CHECK((y1 < 30) == (y2 < 30));
}

static void __cdecl t_slot_game(int x, int y, int fig, int group)   // the game handing out a formation place
{
	(void)group; t_calls++;
	FIG_S(fig, 0x24) = (short)x; FIG_S(fig, 0x26) = (short)y;
	FIG(fig)[0x31] = ST_MARCH; FIG(fig)[0x164] = 0;
}
TEST(group_slots_queue_only_blocked_walks)
{
	o_slot = t_slot_game;
	t_flags(18, 30, 22, 50, FL_BLOCKED);
	for (int i = 1; i <= 6; i++) t_unit(i, 1, 10, 33 + 2 * i, 4);
	h_slot(30, 40, 1, 4);                       // through the forest: queued for a planned route
	CHECK_EQ(g_grQLen, 1);
	h_slot(30, 60, 6, 4);                       // a clear line (below the forest): the game's own walk
	CHECK_EQ(g_grQLen, 1);
	h_slot(12, 37, 2, 4);                       // 2 cells: the game's own walk
	CHECK_EQ(g_grQLen, 1);
	for (int i = 2; i <= 5; i++) h_slot(30, 33 + 2 * i, i, 4);
	h_slot(30, 40, 1, 4);                       // already queued: once
	CHECK_EQ(g_grQLen, 5);
	gr_step();
	CHECK_EQ(g_grQLen, 2);                      // at most 3 routes a logic step
	CHECK(FIG_S(1, 0x36) > 0);
}

/* ================================ jitter fixes */

TEST(swaps_are_not_repeated)
{
	t_unit(1, 1, 20, 20, 2); t_unit(2, 1, 21, 20, 2);
	*(int *)0x0052eff4 = 2;                     // the unit the game would swap with
	FIG_CUR = 1; t_stub_ret = 2;                // the game's answer: swapped
	g_btick = 100;
	CHECK_EQ(h_swapq(), 2);
	CHECK_EQ(g_jSwapWith[1], 2);
	g_btick = 110;
	CHECK_EQ(h_swapq(), 0);                     // too soon: waits instead
	g_btick = 300;
	CHECK_EQ(h_swapq(), 0);                     // the same two: not within 5 s
	CHECK_EQ(t_calls, 1);
	g_btick = 600;
	CHECK_EQ(h_swapq(), 2);
	g_fixJitter = FALSE; g_btick = 601;
	CHECK_EQ(h_swapq(), 2);                     // off: the game's own
	CHECK_EQ(t_calls, 3);
}

TEST(adjacent_enemy_skips_the_dying)
{
	int *off = (int *)0x004d8588;               // the game's table of the 8 neighbours' byte offsets
	static const int ndx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, ndy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	for (int k = 0; k < 8; k++) off[k] = (ndy[k] * 80 + ndx[k]) * 8;
	t_unit(1, 1, 20, 20, 2);
	t_unit(2, 2, 21, 20, 3); FIG(2)[0x31] = ST_DYING;   // east: dying
	t_unit(3, 2, 20, 21, 3);                             // south: alive
	FIG_CUR = 1;
	int cell = (20 * 80 + 20) * 8;
	CHECK_EQ(h_adj_enemy(20, 20, cell), 4);
	BCELL(20, 21)[4] = 1;                       // a level up: not beside it for a fight
	CHECK_EQ(h_adj_enemy(20, 20, cell), 8);
	CHECK_EQ(h_adj_enemy(0, 20, cell), 8);      // at the edge
}

static void t_marching(int me, int x, int y, int nx, int ny, int lx, int ly)   // at a cell centre, next waypoint nx,ny
{
	BYTE *f = FIG(me);
	f[0x31] = ST_MARCH; f[0x34] = 1; f[0x164] = 1;
	FIG_S(me, 0x36) = 2;
	f[0x38] = (BYTE)lx; f[0x39] = (BYTE)ly;     // entry 0: the last
	f[0x3a] = (BYTE)nx; f[0x3b] = (BYTE)ny;     // entry 1: the next
	FIG_S(me, 0x24) = (short)lx; FIG_S(me, 0x26) = (short)ly;
	(void)x; (void)y;
}
TEST(mover_waits_for_a_walking_friend)
{
	g_fixGroupMove = FALSE;
	t_unit(1, 1, 20, 20, 2);
	t_unit(2, 1, 21, 20, 3); FIG(2)[0x31] = ST_MARCH;
	FIG_CUR = 1; g_btick = 1000;
	int waited = 0;
	for (int i = 0; i < JIT_MAX_WAITS + 2; i++) {
		t_marching(1, 20, 20, 21, 20, 22, 20);
		int before = t_calls;
		h_mover(0);
		if (t_calls == before && FIG(1)[0x31] == ST_WAIT) waited++;
	}
	CHECK_EQ(waited, JIT_MAX_WAITS);            // then the game's own handling takes over
	CHECK_EQ(FIG(1)[0x2f], ST_MARCH);
	g_jWaits[1] = 0;
	t_unit(3, 2, 19, 19, 5);                    // an enemy beside it: no waiting
	t_marching(1, 20, 20, 21, 20, 22, 20);
	h_mover(0);
	CHECK_EQ(FIG(1)[0x31], ST_MARCH);
}

TEST(mover_stops_beside_a_taken_destination)
{
	t_unit(1, 1, 20, 20, 2);
	t_unit(2, 1, 21, 20, 2);                    // a friend standing on its destination
	FIG(1)[0x31] = ST_MARCH; FIG_S(1, 0x24) = 21; FIG_S(1, 0x26) = 20;
	FIG_CUR = 1;
	h_mover(0);
	CHECK_EQ(FIG_S(1, 0x24), 20);               // stops where it is instead of sidestepping round it
	CHECK_EQ(FIG_S(1, 0x26), 20);
	CHECK_EQ(t_calls, 1);
}

/* ================================ the battle minimap's outlines (MinimapPop; the 1.5.0 crash) */

static Surface t_mmBack, t_mmPrim;
static Palette t_mmPal;
static BYTE *t_mmCells, t_mmSpr[4200];
static void t_minimap_setup(void)
{
	if (!g_sprLayer) {
		g_sprLayer = malloc((size_t)(2 * GAME_H) * (2 * MAX_W) * 4);
		g_sprShadow = malloc(MAX_W * GAME_H); g_sprValid = calloc(1, MAX_W * GAME_H);
		t_mmBack.bits = malloc(640 * 480);
		t_mmCells = malloc(80 * 80 * 8);
	}
	g_ringN = 0; g_miniW = 0;
	t_mmBack.w = 640; t_mmBack.h = 480; t_mmBack.pitch = 640;
	memset(t_mmBack.bits, 0x11, 640 * 480);
	t_mmPrim.pal = &t_mmPal;
	for (int i = 0; i < 256; i++) { t_mmPal.e[i].peRed = (BYTE)i; t_mmPal.e[i].peGreen = (BYTE)i; t_mmPal.e[i].peBlue = (BYTE)i; }
	g_back = &t_mmBack; g_primary = &t_mmPrim;
	g_miniPop = TRUE; g_battleZoom = FALSE;
	*(int *)0x0057c8fc = 2;                     // in a battle
	*(int *)0x004e5d58 = 2;                     // the minimap shown
	*(BYTE **)0x004ea1a8 = t_mmBack.bits;      // the game's frame
	*(int *)0x004eb274 = 640;                   // its pitch
	memset(t_mmCells, 0, 80 * 80 * 8);
	*(BYTE **)0x004e6574 = t_mmCells;          // the cell table, 80x80 records of 8 bytes
	*(int *)0x004e5d50 = 8; *(int *)0x004e6578 = 80; *(int *)0x004e6570 = 80;
	*(int *)0x004e5d68 = 10; *(int *)0x004e5d60 = 300;   // where the minimap is drawn
	memset(t_mmSpr, 0, sizeof t_mmSpr);
	for (int f = 1; f <= 6; f++) { *(int *)(t_mmSpr + f * 16 + 12) = 200 + f; t_mmSpr[200 + f] = (BYTE)(0x40 + f); }
	*(BYTE **)0x004e5d40 = t_mmSpr;            // the dots' sprite file (t2_spri.pl8)
	t_unit(5, 1, 10, 10, 2);
	t_mmCells[(10 * 80 + 10) * 8 + 5] = 5;      // figure 5 (side 1) on the minimap's cell 10,10
	*(BYTE *)(0x0057bf0a + 1 * 0x160) = 3;      // side 1 draws with frame 3
}

TEST(minimap_outlines_drawn)
{
	t_minimap_setup();
	minimap_dots_paint();
	CHECK_EQ(g_miniW, 160);
	CHECK(g_ringN > 0 && g_ringN <= 12);        // the ring round one dot
	for (int i = 0; i < g_ringN; i++) {          // none of it on the dot itself (minimap pixels 20..21, 20..21)
		int x = g_ringFx[i] - 10, y = g_ringFy[i] - 300;
		CHECK(!(x >= 20 && x <= 21 && y >= 20 && y <= 21));
		CHECK(x >= 19 && x <= 22 && y >= 19 && y <= 22);
	}
	minimap_dots_restore();
	g_miniPop = FALSE;                          // off: nothing
	minimap_dots_paint();
	CHECK_EQ(g_ringN, 0);
}

TEST(minimap_freed_cell_table)
{
	t_minimap_setup();
	BYTE *gone = VirtualAlloc(NULL, 80 * 80 * 8, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	VirtualFree(gone, 0, MEM_RELEASE);
	*(BYTE **)0x004e6574 = gone;
	minimap_dots_paint();                       // must not read it
	CHECK_EQ(g_ringN, 0); CHECK_EQ(g_miniW, 0);
	memset(t_mmCells, 0xDD, 80 * 80 * 8);       // freed by the game's heap: filled with 0xDD
	*(BYTE **)0x004e6574 = t_mmCells;
	minimap_dots_paint();
	CHECK_EQ(g_ringN, 0);
}

TEST(minimap_bad_sprite_offsets)
{
	static const int bad[] = { 0, -5, 4096, (int)0xDDDDDDDD };
	for (int i = 0; i < 4; i++) {
		t_minimap_setup();
		*(int *)(t_mmSpr + 3 * 16 + 12) = bad[i];
		minimap_dots_paint();
		CHECK_EQ(g_ringN, 0); CHECK_EQ(g_miniW, 0);
	}
	// an offset that is in range but lands on memory that isn't there
	BYTE *pg = VirtualAlloc(NULL, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	REQUIRE(pg);
	DWORD old; VirtualProtect(pg + 4096, 4096, PAGE_NOACCESS, &old);
	BYTE *spr = pg + 4096 - 7 * 16;             // its 7 records fill the end of the readable page
	for (int f = 1; f <= 6; f++) *(int *)(spr + f * 16 + 12) = 6;
	*(int *)(spr + 3 * 16 + 12) = 200;          // past the page
	t_minimap_setup();
	*(BYTE **)0x004e5d40 = spr;
	minimap_dots_paint();
	CHECK_EQ(g_ringN, 0);
	VirtualFree(pg, 0, MEM_RELEASE);
}

TEST(minimap_odd_values)
{
	t_minimap_setup(); *(int *)0x004e5d50 = 9;  // a stride the game never sets
	minimap_dots_paint(); CHECK_EQ(g_miniW, 0);
	t_minimap_setup(); *(int *)0x004e5d50 = 0x40000001;   // (cw * ch * stride wraps)
	minimap_dots_paint(); CHECK_EQ(g_miniW, 0);
	t_minimap_setup(); *(int *)0x004e6578 = 97;
	minimap_dots_paint(); CHECK_EQ(g_miniW, 0);
	t_minimap_setup(); *(int *)0x004e5d60 = 400;          // would run off the bottom of the frame
	minimap_dots_paint(); CHECK_EQ(g_miniW, 0);
	t_minimap_setup(); FIG(5)[0x2c] = 7;                  // a side no battle has
	minimap_dots_paint(); CHECK_EQ(g_ringN, 0);
	t_minimap_setup(); t_mmCells[(10 * 80 + 10) * 8 + 5] = 0x51;   // a figure number past 80
	minimap_dots_paint(); CHECK_EQ(g_ringN, 0);
	t_minimap_setup(); *(int *)0x0057c8fc = 0;            // not in a battle
	minimap_dots_paint(); CHECK_EQ(g_miniW, 0);
}

TEST(minimap_restore_puts_the_black_dots_back)
{
	t_minimap_setup();
	t_mmSpr[203] = 0x31;                        // side 1's frame is the black player's colour: dots drawn white-ringed black
	BYTE *before = malloc(640 * 480);
	memcpy(before, t_mmBack.bits, 640 * 480);
	minimap_dots_paint();
	CHECK(memcmp(before, t_mmBack.bits, 640 * 480) != 0);   // the dot was drawn into the frame
	minimap_dots_restore();
	CHECK(memcmp(before, t_mmBack.bits, 640 * 480) == 0);
	free(before);
}

/* ================================ the F10 Battles switches */

TEST(battle_switches_take_effect_at_a_battle_start)
{
	g_bfSet[BF_PATH] = TRUE; g_bfSet[BF_GROUP] = FALSE; g_bfSet[BF_ENGAGE] = TRUE; g_bfSet[BF_MOPUP] = FALSE;
	for (int f = 0; f < BF_COUNT; f++) g_bfOk[f] = TRUE;
	battle_reset();
	CHECK(g_fixJitter); CHECK(!g_fixGroupMove); CHECK(g_fixEngage); CHECK(!g_fixMopUp);
	g_bfOk[BF_ENGAGE] = FALSE;                  // switched on, but its hooks are not in: needs a restart
	battle_reset();
	CHECK(!g_fixEngage);
	g_bfSet[BF_MOPUP] = TRUE;                   // a change in the middle of a battle waits for the next
	CHECK(!g_fixMopUp);
	battle_reset();
	CHECK(g_fixMopUp);
}

TEST(fixes_stay_off_in_multiplayer)
{
	CHECK(fix_on(TRUE));
	CHECK(!fix_on(FALSE));
	MP_SESSION = 1;
	CHECK(!fix_on(TRUE));
	g_mpFixes = TRUE;                           // every player set MultiplayerFixes=1
	CHECK(fix_on(TRUE));
	CHECK(!fix_on(FALSE));
}

/* The fixes' hook sites with the code the patch expects there (the start of each game function). */
static const struct { DWORD at; BYTE code[6]; } kFixSites[] = {
	{ 0x004a79a3, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C } }, { 0x004508de, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C } },
	{ 0x0049f12f, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C } }, { 0x004a07f2, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 } },
	{ 0x004a6270, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 } }, { 0x0044d281, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 } },
	{ 0x0049edc7, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 } }, { 0x00436d02, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C } },
	{ 0x00483e55, { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 } }, { 0x004822ed, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 } },
	{ 0x0048f1dd, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C } }, { 0x0049005f, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 } },
	{ 0x004972f9, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 } }, { 0x00489b8d, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 } },
	{ 0x004954dd, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C } }, { 0x004832ea, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 } },
	{ 0x00479e90, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 } }, { 0x00478987, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 } },
	{ 0x004831d8, { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 } }, { 0x0047fc01, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 } },
	{ 0x0048a9d2, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 } }, { 0x0048acd2, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 } },
	{ 0x0048b02b, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 } }, { 0x00494908, { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 } },
	{ 0x004956cc, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 } },
};
/* Runs the patch's start-up of the game fixes on fake code: every site but `missing` (0 = none) as expected. */
static void t_install_fixes(DWORD missing, const char *ini)
{
	memset((void *)0x00436000, 0, 0x004b0000 - 0x00436000);
	for (int i = 0; i < (int)(sizeof kFixSites / sizeof *kFixSites); i++)
		if (kFixSites[i].at != missing) memcpy((void *)kFixSites[i].at, kFixSites[i].code, 6);
	char text[256];
	snprintf(text, sizeof text, "[Debug]\r\nMpLogs=0\r\n[Game]\r\n%s\r\n", ini ? ini : "");
	t_write("lords2hd.ini", text, strlen(text));
	g_chaseHookFailed = FALSE; g_dmapOn = FALSE;
	install_game_fixes();
}
TEST(fix_switches_know_when_a_restart_is_needed)
{
	t_install_fixes(0, NULL);
	for (int f = 0; f < BF_COUNT; f++) { CHECK(g_bfSet[f]); CHECK(g_bfOk[f]); }
	t_install_fixes(0x004831d8, NULL);          // the archers' melee hook didn't go in
	CHECK(g_bfOk[BF_PATH]); CHECK(g_bfOk[BF_GROUP]); CHECK(g_bfOk[BF_MOPUP]);
	CHECK(g_bfSet[BF_ENGAGE]); CHECK(!g_bfOk[BF_ENGAGE]);
	t_install_fixes(0x00483e55, NULL);          // nor the chase handler: no Mop-Up fix, no engaging
	CHECK(!g_fixMopUp); CHECK(g_chaseHookFailed);
	CHECK(!g_bfSet[BF_MOPUP]); CHECK(!g_bfOk[BF_MOPUP]); CHECK(!g_bfSet[BF_ENGAGE]);
	t_install_fixes(0x004822ed, NULL);          // nor the battle loop: none can run
	for (int f = 0; f < BF_COUNT; f++) CHECK(!g_bfOk[f]);
	// Mop-Up and engaging both off at start-up: their shared hooks were never put in, so switching either
	// on in F10 needs a restart; the other two can be switched at the next battle
	t_install_fixes(0, "MopUpFix=0\r\nEngageFix=0\r\nJitterFix=0");
	CHECK(!g_bfSet[BF_MOPUP]); CHECK(!g_bfOk[BF_MOPUP]);
	CHECK(!g_bfSet[BF_ENGAGE]); CHECK(!g_bfOk[BF_ENGAGE]);
	CHECK(!g_bfSet[BF_PATH]); CHECK(!g_bfOk[BF_PATH]);
	CHECK(g_bfOk[BF_GROUP]);
}
