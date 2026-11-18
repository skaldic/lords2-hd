/* trust.c - v9 peer trust, #included by dplayx.c after resync.c. HOST ONLY: every game message a joiner sends passes
 * the host relay (star topology), so this one check covers the host's own game and every other joiner.
 *
 * Why (msg-trust survey 2026-10-07, decomp FUN_0043eac1:19-48): the game never takes the acting lord from the sender
 * DPID. Every handler reads the lord (and county / army / array indices) from the PAYLOAD, with no range check. So a
 * modified joiner could act for any lord, send host-only control messages (0x16 rollback, 0x14, 0x03 roster ...), or
 * write outside the 1..5 lord arrays on every peer. Stock DirectPlay trusted every player the same way.
 *
 * What is checked (payload offsets are AFTER the game's 4-byte header [type, len, sum, seq|copy]):
 *  - type 1..0x61; types the game never sends are dropped;
 *  - host-only types (proven host-gated in the sender, or host-only lobby UI) are dropped from joiners;
 *  - the sender's lord: learned ONLY from the host's own 0x02 to that joiner (u32 [40..43], the slot the host gave
 *    it), else the game's lord table 0x553d50 for that DPID. Every lord field must equal it;
 *  - 0x01 / 0x0f carry a DPID: it must be the sender's own;
 *  - county ids 0..16 and army ids 0..150 (resync.c VA_COUNTY 17 records, VA_ARMY 151), 0x59 chunk < 400;
 *  - the length byte: n == 4 + d[1] (honest traffic 85514 of 85514, review3 R3-5);
 *  - copies (d[3] 0x40 / 0x80, net_emit_command FUN_0043eda0 sends plain, 0x40, 0x80 back to back): a copy passes
 *    only as the very next game message from that player after a passed original with the same bytes (0x80 after
 *    its 0x40). The game executes a copy whose seq slot has aged out (10 later messages, FUN_0043eac1), so a copy
 *    sent later is a replay (review3 R3-1, R3-2); a copy of a dropped original drops too. Copies skip the checks
 *    below because their original passed them;
 *  - 0x4a / 0x4b move crowns / break an alliance for a lord who is NOT the sender: allowed only as the answer to an
 *    earlier 0x49 (kind 0 / 4) from that lord to the sender, once (review2-trust F6);
 *  - 0x47 accepting an alliance proposed by a HUMAN lord: only after that lord's 0x49 kind 3 to the sender (F3).
 *    A computer lord's proposal never crosses the wire (each game posts it itself), so that accept is not checked;
 *  - the other lord in 0x46 / 0x47 / 0x48 / 0x49 is 1..5 and crown amounts are never negative (F1, F2: a negative
 *    gift or request moved crowns FROM the other lord); 0x48 carries crowns only with kind 0, as the game sends it;
 *  - the execution tick (review3 R3-3, RE re-tick-alliance.md): 29 types carry the sim tick every peer runs them at
 *    (u32, or 24-bit with the lord byte in [3]); the game stores any tick in its 400-slot table 0x50d7c0 and frees a
 *    slot only once SimTick 0x57c998 reaches it, and over 100 due at once overruns its due list. So a tick may be at
 *    most TR_AHEAD past the host's SimTick + lead (0x544094), and a player may have at most TR_INFL commands not yet
 *    due on the host. Heartbeats 0x08 / 0x0a / 0x0c (the sender's SimTick; every peer schedules at max heartbeat +
 *    lead) may be at most tr_hb() ahead of the host's (410 stock, 100 with mpfix). Every SimTick resets at phase changes, each peer at its own
 *    moment, so behind is never a finding and just after the host's reset the sender's own clock counts too. 0x5a (every
 *    receiver adds 1-2 to its lead, FUN_00446935): directed only, 1 per 10 s, burst 4, per player. 0x5b (every peer
 *    clears its commands): 1 per 10 s, burst 2; strict mode also needs the host in a battle. A lord's heartbeat slot
 *    left past the honest bound by a silent player is rewritten by the host (tr_watch, review6 R6-1);
 *  - 0x47 accepting a COMPUTER lord's alliance (R3-4): that lord's AI target (+0x80) is the sender and it has no ally
 *    (+0x81), the state FUN_004a0c1d leaves on every peer when it proposes (op 0x29 itself checks nothing);
 *  - 0x01 hello: only before the host gave this player a lord, at most one new one per 4 s (F4: each hello takes a
 *    new lobby slot on every peer, past the 6-slot arrays);
 *  - offers kept per lord (6 each, a lord's flood evicts only its own), answered ones freed after 30 s (F7);
 *  - 0x17 / 0x18 (save / load on every peer, name from the payload): the name must be a plain file name.
 * Not enforced, logged only ("TRUST note"): a county or army order for an object the host's own copy says another
 * lord owns - the host may be a few ticks behind the sender, so a just-captured county or a just-raised army could
 * read wrong. lotr2net.ini trust=strict enforces it too; trust=0 logs every finding and drops nothing.
 * A dropped message is never relayed or queued; the sender's own game has acted on it, so a cheat desyncs only the
 * cheater (the sync check and resync then repair with the host's copy). */

static int tr_mode_v = -1;   /* 0 log only, 1 enforce (default), 2 strict (ownership too) */
static int tr_mode(void)
{
    if (tr_mode_v < 0) {
        char v[16];
        const char *e = getenv("LOTR2NET_TRUST");
        if (e && *e) lstrcpynA(v, e, sizeof v);
        else if (!ini_get("trust", v, sizeof v)) v[0] = 0;
        tr_mode_v = !v[0] ? 1 : !_stricmp(v, "strict") ? 2 : cfg_on(v) ? 1 : 0;
        SLOG("TRUST mode %s (lotr2net.ini trust= / LOTR2NET_TRUST)", tr_mode_v == 2 ? "strict" : tr_mode_v ? "on" : "LOG ONLY");
    }
    return tr_mode_v;
}

static const BYTE tr_never[] = {0x1b, 0x1f, 0x20, 0x41, 0x44, 0x4e, 0x4f, 0x50, 0x58, 0x5c, 0x61};
static const BYTE tr_hostonly[] = {0x02, 0x03, 0x04, 0x09, 0x0d, 0x0e, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1a, 0x1d, 0x4c, 0x4d, 0x51, 0x54};
/* lord fields: type, payload offset, width (1 or 4) */
static const BYTE tr_lordf[][3] = {
    {0x05, 0, 4}, {0x06, 0, 4}, {0x07, 0, 4}, {0x10, 0, 4}, {0x1c, 0, 4}, {0x24, 0, 4}, {0x2c, 0, 4}, {0x40, 0, 4},
    {0x49, 0, 4}, {0x52, 0, 4}, {0x53, 0, 4},
    {0x08, 3, 1}, {0x0a, 3, 1}, {0x0c, 3, 1}, {0x1e, 3, 1}, {0x42, 3, 1}, {0x43, 3, 1},
    {0x2a, 4, 4}, {0x2d, 4, 4}, {0x2f, 4, 4}, {0x3e, 4, 4}, {0x48, 4, 4}, {0x4a, 4, 4}, {0x4b, 4, 4},
    {0x46, 8, 4}, {0x47, 8, 4}, {0x29, 0, 1}, {0x28, 14, 1}, {0x0f, 0, 4}};
/* object fields: type, payload offset, width, kind (0 county <= 16, 1 army <= 150, 2 chunk < 400, 3 record <= 150,
 * 4 county / 5 army range only, no ownership note) */
static const BYTE tr_objf[][4] = {
    {0x21, 0, 1, 0}, {0x22, 0, 1, 0}, {0x23, 0, 1, 0}, {0x25, 0, 1, 0}, {0x26, 0, 1, 0}, {0x27, 0, 1, 0}, {0x31, 0, 1, 0},
    {0x3a, 0, 1, 0}, {0x28, 15, 1, 0}, {0x2b, 4, 4, 0}, {0x2a, 8, 4, 0},
    {0x29, 5, 1, 1}, {0x2c, 8, 4, 1}, {0x2e, 4, 4, 1}, {0x34, 4, 4, 1}, {0x35, 4, 4, 1}, {0x36, 4, 4, 1}, {0x38, 4, 4, 1},
    {0x39, 4, 4, 1}, {0x37, 0, 4, 1},
    {0x59, 0, 4, 2}, {0x52, 4, 4, 3}, {0x2f, 8, 4, 4}, {0x2d, 8, 4, 5}};

/* execution tick fields: type, payload offset, width (4, or 3 = 24-bit) - RE re-tick-alliance.md Q1 table */
static const BYTE tr_tickf[][3] = {
    {0x0b, 0, 4}, {0x17, 0, 4}, {0x18, 0, 4}, {0x2a, 0, 4}, {0x2b, 0, 4}, {0x2d, 0, 4}, {0x2e, 0, 4}, {0x2f, 0, 4},
    {0x32, 0, 4}, {0x34, 0, 4}, {0x35, 0, 4}, {0x36, 0, 4}, {0x38, 0, 4}, {0x39, 0, 4}, {0x3b, 0, 4}, {0x3e, 0, 4},
    {0x45, 0, 4}, {0x46, 0, 4}, {0x47, 0, 4}, {0x48, 0, 4}, {0x4a, 0, 4}, {0x4b, 0, 4},
    {0x29, 1, 4}, {0x2c, 4, 4}, {0x40, 4, 4}, {0x44, 4, 4}, {0x1e, 0, 3}, {0x42, 0, 3}, {0x43, 0, 3}};
#define VA_SIMTICK 0x57c998u
#define VA_LEAD 0x544094u
#define TR_AHEAD 600                  /* ticks past host SimTick + lead (honest: lead + TR_HB; rig max under 200) */
#define TR_HB 400                     /* heartbeat ahead of the host's SimTick: an honest peer stops 400 past the slowest slot it
                                         gates on (FUN_0043de70, duty 0 at 401), and the host's slot is one of them (rig max under 100) */
#define TR_RST_MS 10000u              /* after the host's SimTick reset, ticks on the old clock still pass this long (tr_tick) */
#define TR_INFL 64                    /* commands per player not yet due on the host (4 joiners x 64 < 400 slots) */
#define TR_TICK_PL 24                 /* stock: commands per player due at one tick; all joiners 10/3 of it = 80 < 100, the due list (tr_tpl) */
#define TR_LATE_MS 100u               /* late commands: 1 per 100 ms, burst TR_LATE_BURST, per player (review5 R5-4: a long link at dial 100) */
#define TR_LATE_BURST 32
#define TR_WD_MS 3000u                /* a lord's heartbeat slot left past the honest bound this long, silent: the host rewrites it */
#define TR_LEAD_MAX 1000              /* lead counted per player from 0x5a (the game adds 1-2 per message, unbounded) */

#define TR_NPEND 32
#define TR_PER_LORD 6                 /* open offers kept per proposing lord (5 lords x 6 < TR_NPEND) */
#define TR_OPEN_MS 1800000u           /* an unanswered offer is kept 30 min (the answer waits for the player to read it) */
/* 0x49 offers; state 1 open, 0 free (an answer frees its offer: the answer's copies pass by the copy chain) */
static struct { BYTE kind, from, to, state; DWORD amount, t; } tr_pend[TR_NPEND];
static DWORD tr_drops, tr_notes;

static int tr_in(const BYTE *set, int n, BYTE t) { int i; for (i = 0; i < n; i++) if (set[i] == t) return 1; return 0; }
static DWORD tr_field(const BYTE *p, int off, int w) { return w == 1 ? p[off] : *(const DWORD *)(p + off); }

/* the lord the sender plays (0 = unknown yet): the host's own 0x02 to it, else the game's lord table */
static DWORD tr_lord(DPID from)
{
    Player *p = pl_find(from);
    if (p && p->lord) return p->lord;
    return rs_game_lord_of(from);
}

static int tr_sim(DWORD *sim, DWORD *lead);
static void tr_watch(DWORD sim, DWORD now);
static void tr_sim_seen(DWORD sim, DWORD now);
/* host, game thread (dp2_Send), cs held or not: our own game's messages teach the bindings and the offers */
static void tr_on_host_send(DPID to, const BYTE *d, DWORD n)
{
    if (!G.host || n < 4) return;
    if (d[0] == 0x02 && n >= 4 + 44 && to) {   /* the slot the host gives this joiner: u32 [40..43] */
        DWORD l = *(const DWORD *)(d + 4 + 40);
        Player *p;
        EnterCriticalSection(&cs);
        p = pl_find(to);
        if (p && p->conn >= 0 && l >= 1 && l <= 5 && p->lord != l) {
            SLOG("TRUST bind: player %08lx plays lord %lu (the host's 0x02)%s", (unsigned long)to, l, p->lord ? " - rebound" : "");
            p->lord = l;
        }
        LeaveCriticalSection(&cs);
    }
    if (d[0] == 0x5a && to && !(d[3] & 0xC0)) {   /* the host's own late report: that joiner adds 1-2 to its lead (FUN_00446935);
                                                     the game sends it 3 times (copies 0x40 / 0x80), the joiner takes one (R6-4) */
        Player *p;
        EnterCriticalSection(&cs);
        p = pl_find(to);
        if (p && p->tr.lead_add < TR_LEAD_MAX) p->tr.lead_add += 2;
        LeaveCriticalSection(&cs);
    }
    if ((d[0] == 0x08 || d[0] == 0x0a || d[0] == 0x0c) && !(d[3] & 0xC0)) {   /* our own heartbeats: the watchdog's clock when every joiner is silent */
        DWORD sim, lead;
        EnterCriticalSection(&cs);
        if (tr_sim(&sim, &lead)) { DWORD now = GetTickCount(); tr_sim_seen(sim, now); tr_watch(sim, now); }
        LeaveCriticalSection(&cs);
    }
}

/* cs held: free offers left unanswered too long */
static void tr_expire(DWORD now)
{
    int i;
    for (i = 0; i < TR_NPEND; i++)
        if (tr_pend[i].state == 1 && now - tr_pend[i].t >= TR_OPEN_MS)
            tr_pend[i].state = 0;
}

/* cs held: remember an offer (0x49 copy 0, kind 0 crowns / 3 alliance / 4 break) from lord `from` */
static void tr_note_offer(const BYTE *d, DWORD n)
{
    DWORD from, to, kind, amt, now = GetTickCount(), old = 0;
    int i, oi = -1, mine = 0, mi = -1;
    if (n < 4 + 16 || (d[3] & 0xC0)) return;
    from = *(const DWORD *)(d + 4); to = *(const DWORD *)(d + 8); kind = *(const DWORD *)(d + 12); amt = *(const DWORD *)(d + 16);
    if ((kind != 0 && kind != 3 && kind != 4) || from < 1 || from > 5 || to < 1 || to > 5) return;
    tr_expire(now);
    for (i = 0; i < TR_NPEND; i++) {
        if (!tr_pend[i].state) { if (oi < 0) oi = i; continue; }
        if (tr_pend[i].from != from) continue;
        if (tr_pend[i].kind == kind && tr_pend[i].to == to && tr_pend[i].amount == amt) { tr_pend[i].t = now; return; }   /* the same offer again */
        mine++;
        if (now - tr_pend[i].t >= old) { old = now - tr_pend[i].t; mi = i; }
    }
    if (mine >= TR_PER_LORD || oi < 0) oi = mi;   /* this lord's quota is full: replace its own oldest, never another lord's */
    if (oi < 0) return;
    tr_pend[oi].state = 1; tr_pend[oi].kind = (BYTE)kind; tr_pend[oi].from = (BYTE)from; tr_pend[oi].to = (BYTE)to;
    tr_pend[oi].amount = amt; tr_pend[oi].t = now;
}

/* cs held: lord L answers (kind 0 crowns 0x4a, 4 break 0x4b, 3 alliance 0x47) an offer by lord x; the answer takes
 * (frees) that offer. Only originals get here: copies pass by the copy chain in tr_check. 1 = a real offer. */
static int tr_take(DWORD kind, DWORD x, DWORD L, DWORD amt)
{
    DWORD now = GetTickCount();
    int i;
    tr_expire(now);
    for (i = 0; i < TR_NPEND; i++) {
        if (!tr_pend[i].state || tr_pend[i].kind != kind || tr_pend[i].from != x || tr_pend[i].to != L) continue;
        if (kind == 0 && tr_pend[i].amount != amt) continue;
        tr_pend[i].state = 0;
        return 1;
    }
    return 0;
}

static int tr_answer(const BYTE *d, DWORD n, DWORD L)
{
    return tr_take(d[0] == 0x4a ? 0 : 4, *(const DWORD *)(d + 12), L, n >= 4 + 16 ? *(const DWORD *)(d + 16) : 0);
}

static int tr_is_lord(DWORD v) { return v >= 1 && v <= 5; }

/* the host's SimTick and lead; 0 = unknown (no known exe: the selftest supplies LOTR2NET_TEST_SIMTICK) */
static int tr_sim(DWORD *sim, DWORD *lead)
{
    char v[16];
    DWORD n;
    if (g_exe_ok) { *sim = RD32(VA_SIMTICK); *lead = RD32(VA_LEAD); return 1; }
    n = GetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", v, sizeof v);
    if (!n || n >= sizeof v) return 0;
    *sim = (DWORD)strtoul(v, NULL, 0); *lead = 50;
    return 1;
}


/* cs held: the tick checks (R3-3, review4 R4-1..R4-5, R4-7). 0 = drop, reason in why.
 * Every peer zeroes SimTick and the heartbeat table at a phase change and at a rollback (FUN_0049a010, FUN_0049b973,
 * FUN_0043f01d, FUN_0043f304), each at its own moment (soak 2026-10-07: joiners reset ~120 ticks before the host and
 * 50 honest heartbeats were dropped). Only AHEAD is a finding: a heartbeat behind slows the lockstep exactly as a
 * slow or silent peer does, which no host check can prevent. For TR_RST_MS after the host's own reset, a tick may
 * still be on the OLD clock: up to the host's pre-reset SimTick + tr_hb() + 40 (an anchor that no message moves, R4-1,
 * R8-1). Every command not yet due, and every late one (run at the next pass), is
 * counted per tick: the executor's due list holds 100 (FUN_004472ce) (R4-2, R4-4).
 * review5: every peer schedules at the max of its heartbeat table + its own lead (FUN_00446e21), and a slot holds the
 * LAST heartbeat relayed from that lord until that peer resets (FUN_00442361 overwrites, no age). So the command bound
 * starts from the highest last heartbeat of any player, not the host's SimTick (R5-1). The lead is the host's, or the
 * sender's own: 50 + 2 per 0x5a relayed TO it (R5-6), at most TR_LEAD_MAX.
 * review6: the heartbeat bound holds while the host's SimTick stands still too: every honest peer gates on the host's
 * slot, so it stops 400 past it (R6-2; the R5-5 pause let a joiner that froze the host poison every slot). The host's
 * slot gates nobody when its lord is out (dead, or not in the battle, FUN_00446e21): then heartbeats are not bounded.
 * A slot left far ahead by a player who then goes silent makes every peer schedule every command at that one tick
 * (R6-1): tr_watch rewrites it. The game's table is cleared only at game / battle init and by 0x5b (FUN_00446f06), not
 * at SimTick resets, so a command parked past the phase stays in it (R6-3): an entry counts while its tick is ahead
 * and the host's own table (0x50d7c0, 400 x 0x38, used flag +0x36) still holds that tick, or it was passed under 2 s ago.
 * review7: the game's slot table is zeroed at every reset, so tr_watch reads the host game's own slot and rewrites a
 * held one to 0, the reset state (rig r6ab A). With mpfix an honest peer stops 30 past the slowest slot, so the bound
 * is 100 (R7-2). When the host gates nobody, a heartbeat may be at most TR_HB past the highest OTHER player's (R7-3).
 * A ring entry counts until our game has taken that message from the queue, then while its table holds the tick
 * (R7-4: a host stalled in a dialog no longer frees the caps). A seq past 40 marks the next sender's duplicate slots in
 * FUN_0043eac1 (stride 0x29, index seq & 0x3f), so it drops (R7-5). Per-tick caps 24 / 80 (R7-1). Residual by design: a
 * heartbeat far BEHIND halts every peer, as a silent player does (lockstep); the 10 check bytes of 0x08 / 0x0c are
 * the sender's own state and are not checked (a liar there is a desynced peer, R7-10).
 * review8: the old-clock window bound is tr_pre + tr_hb() + 40 with no time term, and tr_watch runs inside it (R8-1);
 * tr_watch takes the higher of the host's slot and our last relayed value (a joiner that reset first, R8-2); with the
 * host gating nobody the bound is tr_hb() past the SLOWEST other gating lord (R8-3); the synthetic heartbeat's seq is
 * one the player's last 10 relayed messages do not hold (R8-4); seq 40 drops (R8-6); caps 6 / 20 with mpfix (R8-7).
 * review9: each player's heartbeat from before its own reset counts for TR_RST_MS in the slowest-other bound (R9-1: a
 * peer that reset first left 20 in its slot, and an honest one still on the old clock was dropped), and that bound now
 * applies in the host's reset window too (R9-2: a tick past the window anchor passed unbounded). The seq ring index
 * wraps at 10, not at 256 (R9-6). Selftest: LOTR2NET_TEST_MPFIX runs the mpfix bounds without the exe.
 * review10: hb_pre is armed only outside an open pre window (R10-4); env buffers initialised (R10-2).
 * Residual: with the host gating nobody tr_watch is off, and two players can still ratchet each other when they are
 * the only gating lords; a joiner can reopen the reset window with 0x5b (1 per 10 s), now bounded as above.
 * Not checked, residual: a command a few ticks ahead of the host but behind a peer runs late there (R5-2); a cheat that
 * keeps sending heartbeats can hold its slot up to tr_hb() ahead, and every command then waits for it (the per-tick
 * caps keep the due list safe). */
static DWORD tr_sim_last, tr_reset_t, tr_pre;
#define VA_CMDTAB 0x50d7c0u
#define VA_HBTAB 0x53e8d0u   /* the game's heartbeat slot per lord (FUN_00442361 writes, FUN_0043e330 reads) */
/* 1 = the host's own lord gates every honest peer (alive and human, and in the battle if one runs; FUN_00446e21) */
static int tr_gates(DWORD me)
{
    if (!g_exe_ok) return 1;
    if (me < 1 || me > 5 || !RD8(VA_NOBLE + me * 0x160 + 5) || !RD8(VA_NOBLE + me * 0x160 + 4) || RD8(0x553d76 + me * 0x2c)) return 0;
    return RD32(0x57c8fc) != 2 || RD32(0x5681ec) == me || RD32(0x5681d8) == me;
}
static int tr_host_gates(void)
{
    char v[4] = "";
    if (!g_exe_ok) return !(GetEnvironmentVariableA("LOTR2NET_TEST_NOGATE", v, sizeof v) && v[0] == '1');   /* selftest only */
    return tr_gates(rs_lord());
}
/* the heartbeat ahead bound: mpfix P1a stops an honest peer 30 past the slowest slot (0x43e1de 0x190 -> 0x1d,
 * mpfix_patches.h), stock 400 + 10 (review7 R7-2, R7-6; soak3 112 turns: no honest heartbeat 100 ahead) */
static int tr_mpfix(void) { char v[4] = ""; return g_exe_ok ? g_mpfix : GetEnvironmentVariableA("LOTR2NET_TEST_MPFIX", v, sizeof v) && v[0] == '1'; }   /* selftest only without the exe */
static long tr_hb(void) { return tr_mpfix() ? 100 : TR_HB + 10; }   /* stock: a halted peer's keepalive is min + 401 (R7-6) */
/* commands per player due at one tick; all joiners together: 10/3 of it. mpfix H2 gives each sender's commands rising
 * ticks, so honest is 1-2 (soak3, 112 turns: no 3 at one tick); stock: a held max slot puts them all on one (R7-1, R8-7) */
static int tr_tpl(void) { return tr_mpfix() ? 6 : TR_TICK_PL; }
static int tr_cmp(const void *a, const void *b) { DWORD x = *(const DWORD *)a, y = *(const DWORD *)b; return x < y ? -1 : x > y; }
/* the ticks the host's own command table holds now, sorted; -1 = no known exe */
static int tr_table(DWORD *v)
{
    int i, n = 0;
    if (!g_exe_ok) return -1;
    for (i = 0; i < 400; i++) if (RD8(VA_CMDTAB + i * 0x38 + 0x36) == 1) v[n++] = RD32(VA_CMDTAB + i * 0x38);
    qsort(v, n, sizeof *v, tr_cmp);
    return n;
}
/* a player's heartbeat as every peer's slot now holds it; one that went back is that player's own reset, and the value
 * before it stays the bound for others for TR_RST_MS (R9-1: honest peers still on the old clock gate on it). Honest
 * reset skew is about 120 ticks (soak 2026-10-07, joiners before the host), about 0.3 s at the ~440 ticks/s soak3's
 * resets show. Armed only when no such window is open, so stepping back inside a window does not extend it; it re-arms
 * TR_RST_MS later, so a stepping player still slows the two-player ratchet residual rather than ending it (R10-4, R11-2) */
static void tr_hb_set(Player *pp, DWORD tk, DWORD now)
{
    if ((long)(tk + 50 - pp->tr.hb_last) < 0 && (!pp->tr.hb_pre_t || now - pp->tr.hb_pre_t > TR_RST_MS)) { pp->tr.hb_pre = pp->tr.hb_last; pp->tr.hb_pre_t = now ? now : 1; }
    pp->tr.hb_last = tk; pp->tr.hb_t = now;
}
/* the highest tick on the old clock still allowed now (0 = no reset window open) */
/* review8 R8-1: an honest peer on the old clock stops tr_hb() past its slowest slot, and the host's slot there is at most
 * tr_pre, so no time term (it let one old-clock heartbeat park every slot thousands of ticks ahead) */
static DWORD tr_old_max(DWORD now) { return tr_reset_t && now - tr_reset_t <= TR_RST_MS ? tr_pre + (DWORD)tr_hb() + 40 : 0; }
/* cs held, host: a lord's slot left past the honest bound (accepted on the old clock, then the player went silent) is
 * rewritten on every peer with a heartbeat at tick 0 from that player, as if it had sent one (R6-1). The held value is
 * read from the host game's own table (0x53e8d0), not from what we relayed: every SimTick reset zeroes the table
 * (FUN_0043f304:8, FUN_0049a010:16, FUN_0049b973:14), so an old-clock heartbeat the game took BEFORE its reset is gone
 * (rig r6ab A: a "would fix" for a slot the game had already cleared). Tick 0 is what a reset leaves: every peer then
 * waits for that player until it speaks again, as stock does; a higher value would let the others run ahead of a
 * halted player and take its commands late. The game takes the slot index from the lord byte [3] (FUN_00442361:
 * 0x56d5cc) and drops a seq it saw in that sender's last 10 messages (FUN_0043eac1), so the seq is the first from 20
 * past the player's last of 1..39 that none of its last 10 relayed holds (R8-4). */
/* cs held: every reading of the host's SimTick goes through here, so a reset opens the old-clock window before
 * anything acts on the new clock (review7 R7-7: the host-heartbeat path called tr_watch without it) */
static void tr_sim_seen(DWORD sim, DWORD now)
{
    int k;
    if (sim + 50 < tr_sim_last) {   /* the host's SimTick went back: a reset. Old in-flight ticks mean nothing now */
        tr_reset_t = now ? now : 1;
        tr_pre = tr_sim_last;
        if (!g_exe_ok) for (k = 0; k < MAXPL; k++) memset(G.pl[k].tr.tk, 0, sizeof G.pl[k].tr.tk);   /* no table to read: the selftest's model */
        SLOG("TRUST host SimTick reset %lu -> %lu: old-clock window %u ms", tr_sim_last, sim, TR_RST_MS);
    }
    tr_sim_last = sim;
}
static void tr_watch(DWORD sim, DWORD now)
{
    int j;
    if (!G.host || !tr_host_gates()) return;   /* in the reset window too (R8-1): an honest old-clock peer is not silent */
    for (j = 0; j < MAXPL; j++) {
        Player *q = &G.pl[j];
        BYTE f[16], *m = f + 8;
        DWORD L, held, sil, s;
        int i;
        if (!q->used || q->conn < 0 || now - q->tr.hb_t < TR_WD_MS) continue;
        L = tr_lord(q->id);
        if (L < 1 || L > 5) continue;
        held = g_exe_ok ? RD32(VA_HBTAB + L * 4) : 0;   /* no exe: the selftest's model is hb_last alone */
        if (!g_exe_ok || (long)(q->tr.hb_last - held) > 0) held = q->tr.hb_last;   /* a joiner that reset first may still hold it (R8-2) */
        if ((long)(held - sim) <= tr_hb()) continue;
        sil = now - q->tr.hb_t;
        if (!tr_mode()) {   /* trust=0: log only, once per hold */
            if (q->tr.wd_n != held) { q->tr.wd_n = held; SLOG("TRUST would fix (trust=0) player %08lx: lord %lu's heartbeat slot held at %lu, %lu ms silent, host SimTick %lu", (unsigned long)q->id, L, held, sil, sim); }
            continue;
        }
        *(DWORD *)f = q->id; *(DWORD *)(f + 4) = 0;
        for (s = (q->tr.seq + 19) % 39 + 1; ; s = s % 39 + 1) {   /* a seq none of its last 10 relayed holds: the game drops those (R8-4) */
            for (i = 0; i < 10 && q->tr.sq[i] != s; i++) ;
            if (i == 10) break;
        }
        m[0] = 0x0a; m[1] = 4; m[3] = (BYTE)s;
        m[4] = m[5] = m[6] = 0; m[7] = (BYTE)L;
        m[2] = (BYTE)L;
        bcast(q->conn, T_DATA, f, sizeof f);
        qpush(q->id, 0, m, 8, 0);
        q->tr.seq = m[3]; q->tr.sq[q->tr.sqi] = m[3]; q->tr.sqi = (BYTE)((q->tr.sqi + 1) % 10); q->tr.hb_last = 0; q->tr.hb_t = now;
        if (++q->tr.wd_n <= 20 || !(q->tr.wd_n % 200))
            SLOG("TRUST fix player %08lx: lord %lu's heartbeat slot held at %lu, %lu ms silent, host SimTick %lu: rewritten to 0 on every peer (%lu so far)",
                 (unsigned long)q->id, L, held, sil, sim, q->tr.wd_n);
    }
}
static int tr_bucket(int *tok, DWORD *t0, DWORD now, DWORD per_ms, int burst)
{
    if (!*t0) { *tok = burst; *t0 = now; }
    else if (now - *t0 >= per_ms) {
        DWORD add = (now - *t0) / per_ms;
        *tok = add >= (DWORD)burst ? burst : (*tok + (int)add > burst ? burst : *tok + (int)add);
        *t0 += add * per_ms;
    }
    if (*tok <= 0) return 0;
    (*tok)--;
    return 1;
}
static int tr_tick(Player *pp, BYTE t, DPID to, const BYTE *p, DWORD pl, char *why, int nw)
{
    DWORD sim, lead, i, now, tk, at, om, base, tab[400];
    long dl;
    int k, j, nt, live = 0, mine = 0, all = 0, fr = -1;
    if (!pp) return 1;
    now = GetTickCount();
    if (t == 0x5a) {   /* every receiver adds 1-2 to its lead for good (FUN_00446935); the game sends it only to the sender of a late command */
        if (!to) { snprintf(why, nw, "0x5a (raises every peer's command lead) sent to every peer"); return 0; }
        if (!tr_bucket(&pp->tr.z_tok, &pp->tr.z_t, now, 10000, 4)) { snprintf(why, nw, "0x5a (raises a peer's command lead) over 1 per 10 s"); return 0; }
        {   Player *q = pl_find(to); if (q && q->tr.lead_add < TR_LEAD_MAX) q->tr.lead_add += 2; }
        return 1;
    }
    if (t == 0x5b) {   /* every peer clears its command table and zeroes SimTick (FUN_00446983); honest: the battle desync path only,
                          repeated every 0x82 ticks while that peer stays out of sync (FUN_00440666, FUN_004b99c0) */
        if (tr_mode() == 2 && g_exe_ok && RD32(0x57c8fc) != 2) { snprintf(why, nw, "0x5b (clears every peer's commands) while the host is not in a battle"); return 0; }
        if (!tr_bucket(&pp->tr.b_tok, &pp->tr.b_t, now, 10000, 2)) { snprintf(why, nw, "0x5b (clears every peer's commands) over 1 per 10 s"); return 0; }
        for (k = 0; k < MAXPL; k++) memset(G.pl[k].tr.tk, 0, sizeof G.pl[k].tr.tk);   /* every peer's table is empty now */
        return 1;
    }
    if (!tr_sim(&sim, &lead)) return 1;
    tr_sim_seen(sim, now);
    om = tr_old_max(now);
    tr_watch(sim, now);
    if (t == 0x08 || t == 0x0a || t == 0x0c) {   /* the game stores the 24-bit tick zero-extended, overwriting the slot (FUN_00442361) */
        if (pl < 4) { snprintf(why, nw, "heartbeat 0x%02x too short (%lu)", t, pl); return 0; }
        tk = p[0] | p[1] << 8 | p[2] << 16;
        dl = (long)tk - (long)sim;
        if (dl > tr_hb() && om && tk <= om) {
            if (pp->tr.old_r != tr_reset_t) { pp->tr.old_r = tr_reset_t; SLOG("TRUST stat player %08lx: heartbeat on its old clock %lu ms after the host's reset (tick %lu, host %lu)", (unsigned long)pp->id, now - tr_reset_t, tk, sim); }
            tr_hb_set(pp, tk, now);
            return 1;
        }
        if (dl > pp->tr.hw_hb) { pp->tr.hw_hb = dl; if (dl >= 100) SLOG("TRUST stat player %08lx: heartbeat 0x%02x %+ld ticks ahead of the host's", (unsigned long)pp->id, t, dl); }
        if (dl > tr_hb() && tr_host_gates()) { snprintf(why, nw, "heartbeat 0x%02x tick %+ld from the host's SimTick %lu", t, dl, sim); return 0; }
        if (dl > tr_hb()) {   /* the host gates nobody: an honest peer stops near the slowest OTHER lord that gates it.
                                 The min, not the max: two players cannot ratchet each other (R7-3, R8-3). No other such lord:
                                 a sole human free-runs, no bound. A lord that reset in the last TR_RST_MS counts at its
                                 pre-reset heartbeat when that is higher (R9-1), so the bound holds in the window too (R9-2) */
            DWORD Lq, v;
            int got = 0;
            for (base = 0, j = 0; j < MAXPL; j++) {
                if (&G.pl[j] == pp || !G.pl[j].used || G.pl[j].conn < 0) continue;   /* our own lord gates nobody here */
                Lq = tr_lord(G.pl[j].id);
                if (Lq < 1 || Lq > 5 || !tr_gates(Lq)) continue;
                v = G.pl[j].tr.hb_last;
                if (G.pl[j].tr.hb_pre_t && now - G.pl[j].tr.hb_pre_t <= TR_RST_MS && (long)(G.pl[j].tr.hb_pre - v) > 0) v = G.pl[j].tr.hb_pre;
                if (!got || (long)(v - base) < 0) base = v;
                got = 1;
            }
            if (got && (long)(tk - base) > tr_hb()) { snprintf(why, nw, "heartbeat 0x%02x tick %+ld from the slowest other heartbeat %lu (the host gates nobody)", t, (long)(tk - base), base); return 0; }
        }
        tr_hb_set(pp, tk, now);   /* what every peer's slot for this lord now holds */
        return 1;
    }
    for (i = 0; i < sizeof tr_tickf / sizeof tr_tickf[0]; i++) if (tr_tickf[i][0] == t) break;
    if (i == sizeof tr_tickf / sizeof tr_tickf[0]) return 1;
    if (pl < (DWORD)tr_tickf[i][1] + tr_tickf[i][2]) { snprintf(why, nw, "type 0x%02x too short (%lu) for its tick", t, pl); return 0; }
    /* the scheduler and executor compare signed ints (FUN_00447136, FUN_004472ce): a negative tick runs at once */
    tk = tr_tickf[i][2] == 3 ? (DWORD)(p[0] | p[1] << 8 | p[2] << 16) : *(const DWORD *)(p + tr_tickf[i][1]);
    dl = (long)(int)tk - (long)sim;
    base = sim;
    for (j = 0; j < MAXPL; j++) if (G.pl[j].used && (long)(G.pl[j].tr.hb_last - base) > 0) base = G.pl[j].tr.hb_last;
    if (lead < 50 + (DWORD)pp->tr.lead_add) lead = 50 + (DWORD)pp->tr.lead_add;
    if ((long)(int)tk - (long)base > (long)(lead + TR_AHEAD) && !(om && (long)(int)tk <= (long)(om + lead + TR_AHEAD))) {
        snprintf(why, nw, "type 0x%02x runs at tick %+ld from the host's SimTick %lu (highest heartbeat %lu), over lead %lu + %d", t, dl, sim, base, lead, TR_AHEAD); return 0;
    }
    if (dl > pp->tr.hw_ahead) { pp->tr.hw_ahead = dl; if (dl >= 200) SLOG("TRUST stat player %08lx: type 0x%02x runs %ld ticks after the host's SimTick (lead %lu)", (unsigned long)pp->id, t, dl, lead); }
    if (dl <= 0) {   /* late: the game runs it at the next pass (and answers 0x5a); the rig saw none */
        if (!tr_bucket(&pp->tr.l_tok, &pp->tr.l_t, now, TR_LATE_MS, TR_LATE_BURST)) { snprintf(why, nw, "type 0x%02x late by %ld ticks: over %d late commands (1 per %u ms)", t, -dl, TR_LATE_BURST, TR_LATE_MS); return 0; }
        SLOG("TRUST stat player %08lx: type 0x%02x late by %ld ticks", (unsigned long)pp->id, t, -dl);
        dl = 1;
    }
    at = sim + (DWORD)dl;
    nt = tr_table(tab);
    for (j = 0; j < MAXPL; j++) {
        if (!G.pl[j].used) continue;
        for (k = 0; k < 64; k++) {
            DWORD e = G.pl[j].tr.tk[k];
            int lv = e && (long)(e - sim) > 0 && (nt < 0 || (long)(g_qout - G.pl[j].tr.tt[k]) < 0 || bsearch(&e, tab, nt, sizeof *tab, tr_cmp));
            if (e && !lv) G.pl[j].tr.tk[k] = 0;
            if (lv && e == at) { all++; if (&G.pl[j] == pp) mine++; }
            if (&G.pl[j] == pp) { if (lv) live++; else if (fr < 0) fr = k; }
        }
    }
    if (live > pp->tr.hw_infl) { pp->tr.hw_infl = live; if (live >= 16) SLOG("TRUST stat player %08lx: %d commands not yet due on the host", (unsigned long)pp->id, live); }
    if (mine > pp->tr.hw_tick) { pp->tr.hw_tick = mine; if (mine >= 2) SLOG("TRUST stat player %08lx: %d commands due at one tick", (unsigned long)pp->id, mine + 1); }
    if (live >= TR_INFL || fr < 0) { snprintf(why, nw, "type 0x%02x: %d commands from this player not yet due (cap %d)", t, live, TR_INFL); return 0; }
    if (mine >= tr_tpl() || all >= tr_tpl() * 10 / 3) {
        snprintf(why, nw, "type 0x%02x: tick %lu already has %d from this player, %d from all (caps %d, %d)", t, at, mine, all, tr_tpl(), tr_tpl() * 10 / 3); return 0;
    }
    pp->tr.tk[fr] = at; pp->tr.tt[fr] = g_qin + 1;   /* not yet taken by our game: counts until it is, however long that takes (R7-4) */
    return 1;
}

/* 1 = lord x is played by a human (noble record +0x05, the flag the game itself tests before it sends 0x48 / 0x49) */
static int tr_human(DWORD x) { return g_exe_ok && tr_is_lord(x) && RD8(VA_NOBLE + x * 0x160 + 5) != 0; }

/* cs held: the diplomacy payload values (review2-trust F1-F3). 0 = drop, reason in why. */
static int tr_diplo(BYTE t, const BYTE *d, DWORD pl, DWORD L, char *why, int nw)
{
    const BYTE *p = d + 4;
#define U(o) (*(const DWORD *)(p + (o)))
    if (t == 0x46) {   /* crown gift: [4] recipient, [8] payer = sender, [16] amount */
        if (pl < 20) { snprintf(why, nw, "0x46 too short (%lu)", pl); return 0; }
        if (!tr_is_lord(U(4))) { snprintf(why, nw, "0x46 gives to lord %lu", U(4)); return 0; }
        if (U(16) >= 0x80000000u) { snprintf(why, nw, "0x46 gives a negative amount (%ld) to lord %lu", (long)U(16), U(4)); return 0; }
    } else if (t == 0x47) {   /* alliance answer: [4] proposer, [8] responder = sender, [12] 1 accept */
        if (pl < 16) { snprintf(why, nw, "0x47 too short (%lu)", pl); return 0; }
        if (!tr_is_lord(U(4)) || U(4) == L) { snprintf(why, nw, "0x47 answers lord %lu", U(4)); return 0; }
        if (tr_human(U(4)) && !tr_take(3, U(4), L, 0) && U(12) == 1) {   /* a refusal consumes the proposal too */
            snprintf(why, nw, "0x47 accepts an alliance lord %lu never proposed to lord %lu", U(4), L); return 0;
        }
        if (U(12) == 1 && g_exe_ok && !tr_human(U(4)) && (RD8(VA_NOBLE + U(4) * 0x160 + 0x80) != L || RD8(VA_NOBLE + U(4) * 0x160 + 0x81)
                                                          || !RD8(VA_NOBLE + U(4) * 0x160 + 0x04))) {
            snprintf(why, nw, "0x47 accepts an alliance computer lord %lu does not want with lord %lu (its target %u, ally %u, alive %u)", U(4), L,
                     RD8(VA_NOBLE + U(4) * 0x160 + 0x80), RD8(VA_NOBLE + U(4) * 0x160 + 0x81), RD8(VA_NOBLE + U(4) * 0x160 + 0x04)); return 0;
        }
    } else if (t == 0x48) {   /* request to a computer lord: [4] sender, [8] target, [12] kind 0..6, [16] crowns, [20] county */
        if (pl < 24) { snprintf(why, nw, "0x48 too short (%lu)", pl); return 0; }
        if (!tr_is_lord(U(8)) || U(8) == L) { snprintf(why, nw, "0x48 targets lord %lu", U(8)); return 0; }
        if (U(12) > 6) { snprintf(why, nw, "0x48 kind %lu", U(12)); return 0; }
        if (U(12) ? U(16) != 0 : U(16) >= 0x80000000u) { snprintf(why, nw, "0x48 kind %lu carries %ld crowns", U(12), (long)U(16)); return 0; }
        if (U(20) > 16) { snprintf(why, nw, "0x48 county %lu", U(20)); return 0; }
    } else if (t == 0x49) {   /* offer to a human lord: [0] sender, [4] target, [8] kind 0..12, [12] crowns */
        if (pl < 16) { snprintf(why, nw, "0x49 too short (%lu)", pl); return 0; }
        if (!tr_is_lord(U(4)) || U(4) == L) { snprintf(why, nw, "0x49 targets lord %lu", U(4)); return 0; }
        if (U(8) > 12) { snprintf(why, nw, "0x49 kind %lu", U(8)); return 0; }
        if (!U(8) && U(12) >= 0x80000000u) { snprintf(why, nw, "0x49 offers a negative amount (%ld) to lord %lu", (long)U(12), U(4)); return 0; }
    }
#undef U
    return 1;
}

/* cs held: 0x01 hello from player pp (review2-trust F4). 0 = drop. */
static int tr_hello(Player *pp, const BYTE *d, char *why, int nw)
{
    DWORD now = GetTickCount();
    (void)d;
    if (!pp) { snprintf(why, nw, "0x01 from an unknown player"); return 0; }
    if (G.locked) { snprintf(why, nw, "0x01 after the game started"); return 0; }
    if (pp->lord) { snprintf(why, nw, "0x01 again, the player already has lord %lu", pp->lord); return 0; }
    if (pp->hello_n && now - pp->hello_t < 4000) { snprintf(why, nw, "0x01 again within 4 s"); return 0; }
    pp->hello_t = now; pp->hello_n++;
    return 1;
}

static int tr_plain_name(const BYTE *s, int cap)
{
    int i;
    for (i = 0; i < cap && s[i]; i++) {
        BYTE c = s[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == ' ')) return 0;
        if (c == '.' && i + 1 < cap && s[i + 1] == '.') return 0;
    }
    return i > 0 && i < cap;
}

/* Host, cs held: a game message from a joiner's player `from` (already proven to be that conn's player).
 * Returns 1 = relay / queue it, 0 = drop it (logged).
 * v10 ghost hub (G.ghost): this process's game has left, so only the static checks run (header, seq, copy chain, type
 * range, 0x01/0x0f self-binding, lord fields, index ranges); the tick, diplomacy, offer and ownership checks read the
 * departed game's memory and are skipped. The player named by T_HOST (G.gamehost) plays the host: its messages pass
 * after the header checks, host-only types included, as the real host's own messages always did. */
static int tr_check(int ci, DPID from, DPID to, const BYTE *d, DWORD n)
{
    char why[160];
    const BYTE *p = d + 4;
    DWORD pl = n >= 4 ? n - 4 : 0, L, i, h;
    BYTE t, cp;
    int mode = tr_mode(), note = 0;
    Player *pp = pl_find(from);
    why[0] = 0;
    if (pp && !(n >= 4 && (d[3] & 0xC0))) pp->cp_next = 0;   /* a non-copy ends the chain; it reopens only if it passes */
    if (n < 4) { snprintf(why, sizeof why, "%lu bytes, shorter than the game header", n); goto drop; }
    t = d[0];
    if (n != 4 + (DWORD)d[1]) { snprintf(why, sizeof why, "type 0x%02x length byte says %u payload bytes, the message has %lu", t, d[1], pl); goto drop; }
    if ((d[3] & 0x3f) > 0x27) { snprintf(why, sizeof why, "type 0x%02x seq %u: the game sends 1..39 and decays 0..39 (FUN_0043eac1); 40 never decays, more marks the next sender's", t, d[3] & 0x3f); goto drop; }
    cp = d[3] & 0xC0;
    for (h = 2166136261u, i = 0; i < n; i++) h = (h ^ (i == 3 ? (BYTE)(d[3] & 0x3f) : d[i])) * 16777619u;   /* FNV-1a, copy bits masked */
    if (cp) {
        int ok = pp && pp->cp_next == cp && pp->cp_n == n && pp->cp_h == h;
        if (pp) pp->cp_next = ok && cp == 0x40 ? 0x80 : 0;
        if (ok) return 1;
        snprintf(why, sizeof why, "copy 0x%02x of type 0x%02x does not follow its original", cp, t);
        goto drop;
    }
    if (!t || t > 0x61) { snprintf(why, sizeof why, "type 0x%02x is not a game message", t); goto drop; }
    if (tr_in(tr_never, sizeof tr_never, t)) { snprintf(why, sizeof why, "type 0x%02x is never sent by the game", t); goto drop; }
    if (G.ghost && from == G.gamehost) goto pass;
    if (tr_in(tr_hostonly, sizeof tr_hostonly, t)) { snprintf(why, sizeof why, "type 0x%02x only the host sends", t); goto drop; }
    L = tr_lord(from);
    if (t == 0x01 && (pl < 4 || *(const DWORD *)p != from)) { snprintf(why, sizeof why, "0x01 names player %08lx, not itself", pl >= 4 ? *(const DWORD *)p : 0); goto drop; }
    if (t == 0x01 && !tr_hello(pp, d, why, sizeof why)) goto drop;
    if (t == 0x0f && (pl < 8 || *(const DWORD *)(p + 4) != from)) { snprintf(why, sizeof why, "0x0f binds player %08lx, not itself", pl >= 8 ? *(const DWORD *)(p + 4) : 0); goto drop; }
    if ((t == 0x17 || t == 0x18) && (pl < 20 || !tr_plain_name(p + 4, 16))) { snprintf(why, sizeof why, "0x%02x save/load name is not a plain file name", t); goto drop; }
    for (i = 0; i < sizeof tr_lordf / sizeof tr_lordf[0]; i++) {
        DWORD v;
        if (tr_lordf[i][0] != t) continue;
        if (pl < (DWORD)tr_lordf[i][1] + tr_lordf[i][2]) { snprintf(why, sizeof why, "type 0x%02x too short (%lu) for its lord field", t, pl); goto drop; }
        v = tr_field(p, tr_lordf[i][1], tr_lordf[i][2]);
        if (!L) { snprintf(why, sizeof why, "type 0x%02x names lord %lu before the host gave this player a lord", t, v); goto drop; }
        if (v != L) { snprintf(why, sizeof why, "type 0x%02x acts for lord %lu, the sender plays lord %lu", t, v, L); goto drop; }
    }
    for (i = 0; i < sizeof tr_objf / sizeof tr_objf[0]; i++) {
        DWORD v, k = tr_objf[i][3], max = k == 0 || k == 4 ? 16 : k == 2 ? 399 : 150;
        BYTE own;
        if (tr_objf[i][0] != t) continue;
        if (pl < (DWORD)tr_objf[i][1] + tr_objf[i][2]) { snprintf(why, sizeof why, "type 0x%02x too short (%lu) for its index", t, pl); goto drop; }
        v = tr_field(p, tr_objf[i][1], tr_objf[i][2]);
        if (v > max) { snprintf(why, sizeof why, "type 0x%02x index %lu, max %lu", t, v, max); goto drop; }
        if (k > 1 || !v || !g_exe_ok || !L || G.ghost) continue;
        own = k == 0 ? RD8(VA_COUNTY + v * 0x300 + 5) : RD8(VA_ARMY + v * 0x1a4);
        if (own != L && !note) {
            note = 1;
            snprintf(why, sizeof why, "type 0x%02x orders %s %lu, which lord %u owns on the host (sender plays lord %lu)",
                     t, k ? "army" : "county", v, own, L);
        }
    }
    if (G.ghost) goto pass;
    if (!tr_diplo(t, d, pl, L, why, sizeof why)) goto drop;
    if (!tr_tick(pp, t, to, p, pl, why, sizeof why)) goto drop;
    if ((t == 0x4a || t == 0x4b) && (pl < 12 || !tr_answer(d, n, L))) {
        snprintf(why, sizeof why, "0x%02x for lord %lu answers no offer from that lord to lord %lu", t, pl >= 12 ? *(const DWORD *)(p + 8) : 0, L);
        goto drop;
    }
    if (t == 0x49) tr_note_offer(d, n);
    if (note) {
        tr_notes++;
        if (tr_notes <= 20 || !(tr_notes % 200)) SLOG("TRUST note (%s) from %08lx on conn %d: %s", mode == 2 ? "dropped" : "relayed", (unsigned long)from, ci, why);
        if (mode == 2) return 0;
    }
pass:
    if (pp) { pp->cp_next = 0x40; pp->cp_n = n; pp->cp_h = h; pp->tr.seq = d[3] & 0x3f; pp->tr.sq[pp->tr.sqi] = pp->tr.seq; pp->tr.sqi = (BYTE)((pp->tr.sqi + 1) % 10); }
    return 1;
drop:
    tr_drops++;
    if (pp) pp->tr.nd++;   /* the first 50 drops per player are logged (one flooder never hides another's), then every 500th */
    if ((pp ? pp->tr.nd : tr_drops) <= 50 || !((pp ? pp->tr.nd : tr_drops) % 500))
        SLOG("TRUST %s from %08lx on conn %d: %s (%lu so far)", mode ? "DROPPED" : "would drop (trust=0)", (unsigned long)from, ci, why, tr_drops);
    return mode == 0;
}

/* host, game thread: our own outgoing 0x49 is an offer the joiners may answer */
static void tr_on_send_offer(const BYTE *d, DWORD n)
{
    if (!G.host || n < 4 || d[0] != 0x49) return;
    EnterCriticalSection(&cs);
    tr_note_offer(d, n);
    LeaveCriticalSection(&cs);
}

/* cs held: a session starts clean */
static void tr_reset(void) { memset(tr_pend, 0, sizeof tr_pend); tr_drops = tr_notes = 0; tr_sim_last = tr_reset_t = tr_pre = 0; }

/* the per-conn cap, read when the conn is accepted: DATA_PER_S; selftest only (no known exe) LOTR2NET_TEST_DATA_RATE, 0 = none */
static int data_rate(void)
{
    char v[16];
    DWORD n = g_exe_ok ? 0 : GetEnvironmentVariableA("LOTR2NET_TEST_DATA_RATE", v, sizeof v);
    return n && n < sizeof v ? atoi(v) : DATA_PER_S;
}
