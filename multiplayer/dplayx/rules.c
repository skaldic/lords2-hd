/* rules.c - the host decides which game fixes a multiplayer game runs (Lords2 HD, 2026-11-27). Included by dplayx.c.
 * Our own code. ASCII only.
 *
 * The HD ddraw.dll has game fixes that change what happens in the game (the food ration fix, the wheat fix, ...).
 * Every PC works out every county itself and the PCs must agree, so a fix may run in a multiplayer game only when
 * every player runs it. ddraw tells this DLL (LF_NetRulesSet) who it is (an id: its version and build) and, as a bit
 * mask, which fixes it can run (their code went in) and which it wants (its settings; only the host's count). The
 * host works out the game's rules and sends them to every joiner that said who it is; ddraw reads them back
 * (LF_NetRulesGet) and runs exactly those fixes.
 *
 *   rules = host's want & host's can & every joiner's can, when every player in the session has the host's id;
 *           0 (the original game) when any player has another id or never said: an older dplayx or Windows DirectPlay
 *           (it sends no T_CAPS), or another HD ddraw build (another id). Such a PC runs whatever its own ddraw does
 *           with no rules (an older HD ddraw: its fixes off in multiplayer; this one: LF_NetRulesGet says 0, so off),
 *           which is the original game, so the others must play it too.
 *
 * Wire, compatible both ways with a dplayx that has none of this (no PROTO change):
 *   T_INFO   host -> joiner at the join: 92 bytes as before, then "L2RU" + u32 RU_VER (8 bytes). An older joiner reads
 *            only the 92; a joiner that sees the marker knows the host listens for T_CAPS.
 *   T_CAPS   joiner -> host, right after that INFO (before its CREATEPL): id[RU_ID], u32 can. Only to a host that sent
 *            the marker, and only when ddraw said who it is.
 *   T_RULES  host -> each joiner that sent T_CAPS: u32 rules, u32 locked. Sent whenever the rules change before the
 *            game starts (a player made, a player gone, the host's settings) - each one before any game message that
 *            follows it on that conn, so a joiner always has the rules the host has when the game's start message
 *            comes. An older dplayx ignores the frame type.
 * The game sends its start message (type 0x04, 12 bytes) just BEFORE it locks the session (FUN_004329ec, then
 * FUN_004b7514 -> SetSessionDesc). The host freezes the rules as that message goes out and sends them once more with
 * locked = 1 ahead of it; a joiner keeps them from then on. After that they never change (a player leaving, a host
 * hand-over, a resync rollback): every PC keeps what it had. The lock freezes them too, should the start message not
 * have been seen. A new session (Open, Close) starts over with no rules. */
#define RU_VER 1
#define RU_INFO 100   /* T_INFO bytes with the marker */

static char ru_id[RU_ID];       /* ours, from ddraw (zero-padded) */
static DWORD ru_can, ru_want;
static int ru_set;              /* ddraw said who it is */
static volatile LONG ru_rules, ru_have;   /* this session's rules; 1 once they are known (host: open; joiner: a T_RULES) */
static int ru_host_aware;       /* joiner: the host sent the marker */
static int ru_frozen;           /* the session locked: the rules stay as they are */

static void ru_reset(void)   /* cs held: a new session */
{
    InterlockedExchange(&ru_rules, 0);
    InterlockedExchange(&ru_have, 0);
    ru_host_aware = 0;
    ru_frozen = 0;
}

/* host, cs held: the rules from everyone in the session now, sent to every joiner that said who it is */
static void ru_recompute(const char *why)
{
    DWORD v, frame[2];
    int i, who = 0, other = 0;
    if (!G.open || !G.host || G.ghost || ru_frozen || G.locked) return;
    v = ru_set ? ru_want & ru_can : 0;
    for (i = 0; i < MAXCONN; i++) if (G.conns[i].used && G.conns[i].haveplayer) {
        who++;
        if (!G.conns[i].ru_has || memcmp(G.conns[i].ru_id, ru_id, RU_ID)) other++;
        else v &= G.conns[i].ru_can;
    }
    if (other) v = 0;
    if ((DWORD)ru_rules != v || !ru_have)
        SLOG("rules: %08lx (%s; %d joiner%s, %d without this patch)", (unsigned long)v, why, who, who == 1 ? "" : "s", other);
    InterlockedExchange(&ru_rules, (LONG)v);
    InterlockedExchange(&ru_have, 1);
    frame[0] = v; frame[1] = 0;
    for (i = 0; i < MAXCONN; i++) if (G.conns[i].used && G.conns[i].ru_has) osend(G.conns[i].o, T_RULES, frame, 8);
}

/* host, cs held: the joiner's T_CAPS */
static void ru_on_caps(int ci, const BYTE *f, DWORD n)
{
    char id[RU_ID + 1];
    if (n < RU_ID + 4 || G.conns[ci].haveplayer || G.locked || ru_frozen) return;   /* only before its player: the rules are settled by the start */
    memcpy(G.conns[ci].ru_id, f, RU_ID);
    G.conns[ci].ru_can = *(const DWORD *)(f + RU_ID);
    G.conns[ci].ru_has = 1;
    memcpy(id, f, RU_ID); id[RU_ID] = 0;
    SLOG("rules: conn %d is '%s', can %08lx%s", ci, id, (unsigned long)G.conns[ci].ru_can,
         memcmp(f, ru_id, RU_ID) ? " - not this patch: the original rules" : "");
}

/* host, cs held: the game starts (its start message, or the lock if that never came): the rules stay as they are, and
 * every joiner that said who it is gets them once more with locked = 1, before the start message on its conn, so it
 * keeps them too (a host hand-over or a late frame can't change them afterwards). */
static void ru_lock(const char *why)
{
    DWORD frame[2];
    int i;
    if (!G.open || !G.host || G.ghost || ru_frozen) return;
    ru_frozen = 1;
    SLOG("rules: %08lx for this game (%s)", (unsigned long)ru_rules, why);
    frame[0] = (DWORD)ru_rules; frame[1] = 1;
    for (i = 0; i < MAXCONN; i++) if (G.conns[i].used && G.conns[i].ru_has) osend(G.conns[i].o, T_RULES, frame, 8);
}

/* host, cs held, before the game's message is queued: the game's start message freezes the rules. It is message type
 * 0x04, 12 bytes, to everyone: FUN_004329ec sends it (FUN_0043eda0(4, 0), the game's only sender of type 4) just
 * before it locks the session; the lobby's own messages are other types (0x09, 0x0e, 0x54). From the freeze on the
 * host takes no new player (conn_thread), so every player in the game has the rules. */
static void ru_on_send(DPID to, const BYTE *buf, DWORD size)
{
    if (to == 0 && size == 12 && buf[0] == 0x04 && !G.locked) ru_lock("the game starts");
}

/* host: T_INFO for a joiner, with the marker */
static void ru_info(BYTE *info)
{
    memcpy(info + 92, "L2RU", 4);
    *(DWORD *)(info + 96) = RU_VER;
}

/* joiner, cs held, the host's INFO in hand and G.jout set: tell a host that listens who we are */
static void ru_join(const BYTE *info, DWORD n)
{
    BYTE b[RU_ID + 4];
    ru_host_aware = n >= RU_INFO && !memcmp(info + 92, "L2RU", 4) && *(const DWORD *)(info + 96) == RU_VER;
    if (!ru_host_aware || !ru_set) {
        SLOG("rules: %s - the original rules", !ru_host_aware ? "the host does not decide them (an older dplayx)" : "no HD ddraw here");
        return;
    }
    memcpy(b, ru_id, RU_ID);
    *(DWORD *)(b + RU_ID) = ru_can;
    jsend(T_CAPS, b, sizeof b);
}

/* joiner, cs held: the host's T_RULES */
static void ru_on_rules(const BYTE *f, DWORD n)
{
    DWORD v, locked;
    if (n < 8 || !ru_host_aware || ru_frozen) return;
    v = *(const DWORD *)f; locked = *(const DWORD *)(f + 4);
    if ((DWORD)ru_rules != v || !ru_have || locked)
        SLOG("rules: %08lx from the host%s", (unsigned long)v, locked ? " for this game" : "");
    InterlockedExchange(&ru_rules, (LONG)v);
    InterlockedExchange(&ru_have, 1);
    if (locked) ru_frozen = 1;
}

/* ddraw: who we are, what we can run, what we want (the host's want decides). Before a session and whenever it changes;
 * the host works the rules out again until the game starts. */
__declspec(dllexport) void WINAPI LF_NetRulesSet(const char *id, DWORD can, DWORD want)
{
    EnterCriticalSection(&cs);
    memset(ru_id, 0, sizeof ru_id);
    if (id) lstrcpynA(ru_id, id, sizeof ru_id);
    ru_can = can; ru_want = want; ru_set = id != NULL;
    if (G.open && G.host) ru_recompute("the host's settings");
    LeaveCriticalSection(&cs);
}

/* ddraw: this session's rules. 1 = known (*rules is the mask), 0 = not (no session, or no rules from the host yet:
 * play the original rules). No lock: two words written in order. */
__declspec(dllexport) int WINAPI LF_NetRulesGet(DWORD *rules)
{
    if (!ru_have || !G.open) { if (rules) *rules = 0; return 0; }
    if (rules) *rules = (DWORD)ru_rules;
    return 1;
}
