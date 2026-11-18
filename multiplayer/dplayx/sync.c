/* sync.c - our own per-turn sync check (detection only), #included by dplayx.c after the state helpers.
 *
 * The game's own end-turn check is an 8-bit byte-sum on the HOST only (mp-static-audit.md Q6). This adds a 32-bit
 * FNV-1a per region on EVERY peer, exchanged over our TCP layer (frame T_SYNC, never handed to the game):
 *   kind 1 TURN  : when the game sends msg 0x08. That message is packed by FUN_00441f76, called from the scheduled
 *                  command handler FUN_00447b8e, i.e. at the same sim point on every peer (inside NetExecuteCommands).
 *   kind 2 BATTLE: when the game sends msg 0x0c (every human, SimTick % 0x82 == 0 in battle, before that tick's
 *                  NetExecuteCommands; FrameAndSimStep L104206-104210).
 *   kind 3 TICK  : when the game sends msg 0x0a in the campaign with SimTick % 100 == 0 (same place, L104221-104225):
 *                  a mid-turn probe that pins the FIRST tick two peers differ.
 * We are inside dp2_Send on the game's own thread, so the state cannot tear. Ranges are the game's own checksum ranges
 * (NetStateChecksum 0x440231): counties 1..16 x 0x300 skip 5, nobles 1..5 x 0x160 skip 6, armies 1..150 x 0x1a4,
 * plus the RNG pair 0x58fe28/0x58fe08; battle figures 1..80 x 0x1b0 skip 0x12 and records 0x57a100 1..100 x 0x4c
 * skip 4 (the game's groups slot is forced to 1, so groups are left out here too).
 * v11: battle region 18 = ai-rally (0x56d89c, 0x522f70, 0x522f74), only when the rally-clear hook is in (g_rally_ok).
 * Key = (kind, turn 0x553240, tick, battle armies 0x568210/0x568218 for kind 2). A record is compared when both sides
 * of a key exist. Kind 1/2 mismatch = "DESYNC" (logged, written to desync.txt, shown in the game window title);
 * kind 3 mismatch = "DIFF" (log only: a late command can differ for a few ticks and converge).
 * Only active on the known 5 Jun 1997 exe (g_exe_ok). Our own code. ASCII only.
 * v7 (2026-10-07): the header is 32 bytes (d[7] = EPOCH: how many rollbacks this session has run, so a replayed turn
 * never matches the first run's records); the player word is (DPID << 8) | lord slot (the host re-stamps the DPID of
 * the conn it came from); TURN records carry region 37 = REST, an FNV of every other shared byte of the save image
 * (resync.c rs_snapshot; detection only). A TURN desync in regions 0..36 or a turn-end skew starts the AUTOMATIC
 * RESYNC (resync.c). */

#define T_SYNC 10
#define SYNC_MAXR 40
#define SYNC_RING 96
#define SY_TURN 1
#define SY_BATTLE 2
#define SY_TICK 3
#define SY_HDR 32
#define SY_REST 37   /* region index of the save-image rest hash (TURN records) */
static DWORD sy_epoch;   /* rollbacks run this session (both sides bump it at the same verdict; resync.c) */
static DWORD sy_end_turn = 0xffffffffu;   /* turn number of our last turn-end (0x08) record: the turn a 0x16 verdict is about */
/* resync.c (included right after this file) */
static DWORD rs_snapshot(DWORD turn, DWORD epoch);
static void rs_note_peer(DWORD player);
static void rs_note_battle(DWORD turn, DWORD epoch, DWORD ba, DWORD bb);
static int rs_peer_battle_ok(DWORD turn, DWORD epoch, DWORD ba, DWORD bb, DWORD player);
static void rs_on_turn_desync(DWORD turn, DWORD epoch, DWORD peer_player);
static void rs_check_verify(DWORD turn, DWORD epoch, DWORD peer, int nd, const char *list);
static void rs_test_corrupt(const BYTE *buf, DWORD ov, DWORD tick);

typedef struct {
    int used;
    DWORD kind, turn, tick, ba, bb, player, nreg, epoch;
    DWORD h[SYNC_MAXR];
    DWORD seen[8];   /* own records: the peer players already compared against this one (a replay is ignored) */
    int nseen;
} SyncRec;

static SyncRec sy_own[SYNC_RING], sy_peer[SYNC_RING];
static int sy_own_i, sy_peer_i;
static DWORD sy_last_kind, sy_last_turn, sy_last_tick, sy_last_epoch;
static int sy_desyncs, sy_oks, sy_diffs, sy_titled, sy_lines;
static DWORD sy_diff_turn = 0xffffffffu, sy_bt = 0xffffffffu, sy_bba, sy_bbb;
static int sy_diff_in_turn, sy_in_b;
/* battle figure bytes per battle check (figures 0x554480 + 81 x 0x1b0, records 0x57a100 + 101 x 0x4c), written to
 * figdump-<turn>-<tick>.bin at the first DESYNC of a battle so two PCs' dumps can be diffed field by field (2026-10-07,
 * battle drift in the 0.02 two-PC game). Log-only: nothing on the wire changes. */
#define FD_FIG (81 * 0x1b0)
#define FD_REC (101 * 0x4c)
#define FD_N 6
typedef struct { DWORD turn, tick, ba, bb, epoch, used; BYTE b[FD_FIG + FD_REC]; } FigDump;
static FigDump fd_ring[FD_N];
static int fd_i;
static DWORD sy_skew_turn = 0xffffffffu;   /* last turn reported as ended at a different tick */
#define SY_MAXLINES 200   /* desync.txt lines per game: a split game stays split, a replaying peer must not fill the disk */

/* compare an own record with this peer player at most once; 8 players at most (the player field is the sender's word) */
static int sy_mark(SyncRec *own, DWORD player)
{
    int i;
    for (i = 0; i < own->nseen; i++) if (own->seen[i] == player) return 0;
    if (own->nseen >= 8) return 0;
    own->seen[own->nseen++] = player;
    return 1;
}

#define SLOG(...) do { log_force = 1; logf_(__VA_ARGS__); log_force = 0; } while (0)
#define RD32(va) (*(volatile DWORD *)(va))

static DWORD fnv(const BYTE *p, DWORD n, DWORD h)
{
    while (n--) { h ^= *p++; h *= 0x01000193u; }
    return h;
}

/* region names for the log: index -> text */
static void sy_region_name(DWORD kind, DWORD i, char *out)
{
    if (kind == SY_BATTLE) {
        if (i < 8) sprintf(out, "figures %lu-%lu", i * 10 + 1, i * 10 + 10);
        else if (i == 18) sprintf(out, "ai-rally");
        else sprintf(out, "battle-records %lu-%lu", (i - 8) * 10 + 1, (i - 8) * 10 + 10);
        return;
    }
    if (i < 16) sprintf(out, "county %lu", i + 1);
    else if (i < 21) sprintf(out, "noble %lu", i - 15);
    else if (i < 36) sprintf(out, "armies %lu-%lu", (i - 21) * 10 + 1, (i - 21) * 10 + 10);
    else if (i == 36) sprintf(out, "rng");
    else sprintf(out, "rest");
}

/* State dump (diagnostic, 2026-10-08): ini "statedump=1" or env LOTR2NET_STATEDUMP=1 -> write every save-table region
 * (table 0x4de960: 225 x {ptr,size}, the same bytes and order as a .sav's table part) to <exedir>state-<turn>-<tag>.bin,
 * once at battle setup (before the rally clear) and once at the first battle SYNC record, so two PCs' state going into a
 * battle can be diffed region by region (tools: .workers save_desc.json offsets). Off by default: ~267 KB per file. */
static int sd_on = -1;
static void sd_write(DWORD turn, const char *tag)
{
    char fp[MAX_PATH + 64], v[16];
    FILE *ff; int i;
    if (sd_on < 0) {
        char *e = getenv("LOTR2NET_STATEDUMP");
        sd_on = (e && *e) ? cfg_on(e) : (ini_get("statedump", v, sizeof v) ? cfg_on(v) : 0);
    }
    if (!sd_on) return;
    lstrcpynA(fp, exedir, MAX_PATH);
    sprintf(fp + strlen(fp), "state-%lu-%s.bin", turn, tag);
    if (!(ff = fopen(fp, "wb"))) return;
    for (i = 0; i < 225; i++) {
        DWORD ptr = RD32(0x4de960 + i * 8), sz = RD32(0x4de960 + i * 8 + 4);
        if (ptr >= 0x400000 && ptr < 0x5d0000 && sz < 0x20000) fwrite((void *)ptr, 1, sz, ff);
    }
    fclose(ff); SLOG("state dump written: %s", fp);
}
static DWORD sd_bt = 0xffffffffu, sd_ba;

static void sy_fill(SyncRec *r, DWORD kind)
{
    DWORD i, k;
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->kind = kind;
    r->turn = RD32(0x553240);
    r->tick = RD32(0x57c998);
    r->epoch = sy_epoch;
    r->player = (G.myid << 8) | (RD32(0x57c8cc) & 0xff);
    if (kind == SY_BATTLE) {
        r->ba = RD32(0x568210);
        r->bb = RD32(0x568218);
        for (i = 0; i < 8; i++) {
            DWORD h = 0x811c9dc5u;
            for (k = 1; k <= 10; k++) h = fnv((BYTE *)(0x554480 + (i * 10 + k) * 0x1b0) + 0x12, 0x1b0 - 0x12, h);
            r->h[i] = h;
        }
        for (i = 0; i < 10; i++) {
            DWORD h = 0x811c9dc5u;
            for (k = 1; k <= 10; k++) h = fnv((BYTE *)(0x57a100 + (i * 10 + k) * 0x4c) + 4, 0x4c - 4, h);
            r->h[8 + i] = h;
        }
        r->nreg = 18;
        if (g_rally_ok) {   /* region 18 = ai-rally: flag 0x56d89c, point 0x522f70/0x522f74 (v11; cleared at battle setup) */
            r->h[18] = fnv((BYTE *)0x522f74, 4, fnv((BYTE *)0x522f70, 4, fnv((BYTE *)0x56d89c, 4, 0x811c9dc5u)));
            r->nreg = 19;
        }
        if (r->turn != sd_bt || r->ba != sd_ba) { char t[16]; sd_bt = r->turn; sd_ba = r->ba; sprintf(t, "t%lu", r->tick); sd_write(r->turn, t); }
        if (sd_on == 1)     /* statedump probe: AI rally inputs not in the save table (net flag, strength ratio, its bar) */
            SLOG("battle probe turn=%lu tick=%lu net=%lu ratio=%ld bar=%ld rally=%lu,%ld,%ld alarm=%ld hits=%ld",
                 r->turn, r->tick, RD32(0x553030), (long)RD32(0x4eea98), (long)RD32(0x57c8b4), RD32(0x56d89c),
                 (long)RD32(0x522f70), (long)RD32(0x522f74), (long)RD32(0x553070), (long)RD32(0x553f28));
        {
            FigDump *d = &fd_ring[fd_i];
            fd_i = (fd_i + 1) % FD_N;
            d->turn = r->turn; d->tick = r->tick; d->ba = r->ba; d->bb = r->bb; d->epoch = r->epoch; d->used = 1;
            memcpy(d->b, (BYTE *)0x554480, FD_FIG);
            memcpy(d->b + FD_FIG, (BYTE *)0x57a100, FD_REC);
        }
        return;
    }
    for (i = 0; i < 16; i++) r->h[i] = fnv((BYTE *)(0x53f9b0 + (i + 1) * 0x300) + 5, 0x300 - 5, 0x811c9dc5u);
    for (i = 0; i < 5; i++) r->h[16 + i] = fnv((BYTE *)(0x57bf00 + (i + 1) * 0x160) + 6, 0x160 - 6, 0x811c9dc5u);
    for (i = 0; i < 15; i++) r->h[21 + i] = fnv((BYTE *)(0x52f0b0 + (i * 10 + 1) * 0x1a4), 10 * 0x1a4, 0x811c9dc5u);
    r->h[36] = fnv((BYTE *)0x58fe08, 4, fnv((BYTE *)0x58fe28, 4, 0x811c9dc5u));
    r->nreg = 37;
    if (kind == SY_TURN) {   /* 5-peer rig 2026-10-07: the mid-turn probe took this copy too, overwriting the turn-end copy
                              * a repair is built from, and compared REST mid-turn ("SYNC DIFF ... first: rest" every turn) */
        DWORD rest = rs_snapshot(r->turn, r->epoch);   /* own turn-end copy for resync + the REST hash */
        if (rest) { r->h[SY_REST] = rest; r->nreg = 38; }
    }
}

static int sy_same_key(const SyncRec *a, const SyncRec *b)
{
    return a->kind == b->kind && a->turn == b->turn && a->tick == b->tick && a->ba == b->ba && a->bb == b->bb && a->epoch == b->epoch;
}

static char sy_title[160];
static volatile LONG sy_title_pending;

/* The game has no chat: the window title is the only text we can show without touching its UI state.
 * GAME THREAD ONLY and cs NOT held: SetWindowText from a network thread waits for the game thread, which may be waiting
 * on cs inside Send/Receive -> deadlock (2026-10-06 rig: both peers froze at the first battle DESYNC). */
static void sy_flush_title(void)
{
    DWORD pid = GetCurrentProcessId();
    HWND w;
    if (!sy_title_pending || !InterlockedExchange(&sy_title_pending, 0)) return;
    for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) {
        DWORD p = 0;
        GetWindowThreadProcessId(w, &p);
        if (p == pid && IsWindowVisible(w)) break;
    }
    if (!w) { SLOG("DESYNC: no game window found for the title"); return; }
    SLOG("DESYNC shown in window title (hwnd %p): %s", (void *)w, SetWindowTextA(w, sy_title) ? sy_title : "SetWindowText FAILED");
}

static void sy_tell_player(const char *what)   /* any thread: only queues the text */
{
    snprintf(sy_title, sizeof sy_title, "Lords of the Realm II - OUT OF SYNC: %s (see dplayx.log)", what);
    InterlockedExchange(&sy_title_pending, 1);
}

/* cs held */
static void sy_compare(const SyncRec *mine, const SyncRec *peer)
{
    DWORD i, n = mine->nreg < peer->nreg ? mine->nreg : peer->nreg;
    char list[300], nm[48];
    int nd = 0;
    list[0] = 0;
    if (mine->kind == SY_TURN && n > SY_REST) {   /* REST: detection only, never a desync (it may hold bytes we do not know are local) */
        if (mine->h[SY_REST] != peer->h[SY_REST])
            SLOG("SYNC REST-DIFF turn=%lu epoch=%lu peer-player=%lu: shared bytes outside the 37 checked regions differ (detection only, not repaired by itself)",
                 mine->turn, mine->epoch, peer->player & 0xff);
        n = SY_REST;
    }
    for (i = 0; i < n; i++)
        if (mine->h[i] != peer->h[i]) {
            if (nd < 6) {
                sy_region_name(mine->kind, i, nm);
                if (strlen(list) + strlen(nm) + 3 < sizeof list) { if (nd) strcat(list, ", "); strcat(list, nm); }
            }
            nd++;
        }
    if (mine->kind == SY_TURN) rs_check_verify(mine->turn, mine->epoch, peer->player, nd, list);
    if (!nd) {
        sy_oks++;
        if (mine->kind != SY_TICK)
            SLOG("SYNC ok kind=%lu turn=%lu tick=%lu epoch=%lu peer-player=%lu (ok %d, desync %d)", mine->kind, mine->turn,
                  mine->tick, mine->epoch, peer->player & 0xff, sy_oks, sy_desyncs);
        return;
    }
    if (mine->kind == SY_TICK) {
        sy_diffs++;
        if (mine->turn != sy_diff_turn) { sy_diff_turn = mine->turn; sy_diff_in_turn = 0; }
        if (++sy_diff_in_turn > 3) return;   /* the first 3 per turn pin where it starts; the turn-end line has the rest */
        SLOG("SYNC DIFF (mid-turn) turn=%lu tick=%lu peer-player=%lu regions=%d first: %s", mine->turn, mine->tick,
              peer->player & 0xff, nd, list);
        return;
    }
    sy_desyncs++;
    if (mine->kind == SY_TURN) rs_on_turn_desync(mine->turn, mine->epoch, peer->player);
    if (mine->kind == SY_BATTLE) {   /* a split battle stays split: log the first 3 of each battle, count the rest */
        if (mine->turn != sy_bt || mine->ba != sy_bba || mine->bb != sy_bbb) { sy_bt = mine->turn; sy_bba = mine->ba; sy_bbb = mine->bb; sy_in_b = 0; }
        if (++sy_in_b > 3) return;
        if (sy_in_b == 1) {
            int j;
            for (j = 0; j < FD_N; j++) {
                FigDump *d = &fd_ring[j];
                if (d->used && d->turn == mine->turn && d->tick == mine->tick && d->ba == mine->ba && d->bb == mine->bb && d->epoch == mine->epoch) {
                    char fp[MAX_PATH + 48];
                    FILE *ff;
                    lstrcpynA(fp, exedir, MAX_PATH);
                    sprintf(fp + strlen(fp), "figdump-%lu-%lu.bin", mine->turn, mine->tick);
                    if ((ff = fopen(fp, "wb")) != NULL) { fwrite(d->b, 1, sizeof d->b, ff); fclose(ff); SLOG("figure dump written: %s", fp); }
                    break;
                }
            }
        }
    }
    SLOG("DESYNC kind=%s turn=%lu tick=%lu epoch=%lu me=player %lu peer=player %lu regions=%d first: %s (mid-turn diffs so far %d)",
          mine->kind == SY_TURN ? "turn-end" : "battle", mine->turn, mine->tick, mine->epoch, mine->player & 0xff,
          peer->player & 0xff, nd, list, sy_diffs);
    {
        char p[MAX_PATH + 16], line[512];
        FILE *f;
        lstrcpynA(p, lf_dir(), MAX_PATH);
        lstrcatA(p, "desync.txt");
        if (sy_lines <= SY_MAXLINES && (f = fopen(p, "a")) != NULL) {
            snprintf(line, sizeof line, "%s desync, turn %lu, tick %lu, with player %lu: %s%s\n",
                     mine->kind == SY_TURN ? "End-of-turn" : "Battle", mine->turn, mine->tick, peer->player & 0xff, list,
                     nd > 6 ? ", ..." : "");
            if (sy_lines++ == SY_MAXLINES) snprintf(line, sizeof line, "More desyncs this game: see dplayx.log\n");
            fputs(line, f);
            fclose(f);
        }
    }
    if (!sy_titled) {
        char w[96];
        snprintf(w, sizeof w, "turn %lu, %s", mine->turn, list);
        sy_tell_player(w);
        sy_titled = 1;
    }
}

/* cs held: the same turn ended at different ticks here and at a peer. Records match on (turn, tick), so no region
 * compare would ever run and the split was silent (review 2026-10-07): report it once per turn like a desync. */
static void sy_skew(const SyncRec *mine, const SyncRec *peer)
{
    char w[96];
    if (mine->turn == sy_skew_turn) return;
    sy_skew_turn = mine->turn;
    sy_desyncs++;
    SLOG("DESYNC kind=turn-end turn=%lu epoch=%lu: ended at tick %lu here, tick %lu at player %lu", mine->turn, mine->epoch,
          mine->tick, peer->tick, peer->player & 0xff);
    rs_on_turn_desync(mine->turn, mine->epoch, peer->player);
    if (!sy_titled) {
        snprintf(w, sizeof w, "turn %lu ended at a different time", mine->turn);
        sy_tell_player(w);
        sy_titled = 1;
    }
}
static int sy_is_skew(const SyncRec *a, const SyncRec *b)
{
    return a->kind == SY_TURN && b->kind == SY_TURN && a->turn == b->turn && a->epoch == b->epoch && a->tick != b->tick;
}

/* cs held: a record from a peer */
static int sy_on_peer(const BYTE *p, DWORD n)   /* 1 = kept (the host relays it), 0 = dropped */
{
    SyncRec r;
    int i;
    if (n < SY_HDR) return 0;
    memset(&r, 0, sizeof r);
    r.used = 1;
    r.kind = ((DWORD *)p)[0]; r.turn = ((DWORD *)p)[1]; r.tick = ((DWORD *)p)[2];
    r.ba = ((DWORD *)p)[3]; r.bb = ((DWORD *)p)[4]; r.player = ((DWORD *)p)[5]; r.nreg = ((DWORD *)p)[6];
    r.epoch = ((DWORD *)p)[7];
    if (r.nreg > SYNC_MAXR || n < SY_HDR + r.nreg * 4) return 0;
    memcpy(r.h, p + SY_HDR, r.nreg * 4);
    if (r.kind == SY_BATTLE && !rs_peer_battle_ok(r.turn, r.epoch, r.ba, r.bb, r.player)) return 0;   /* host: only battles it ran */
    rs_note_peer(r.player);
    for (i = 0; i < SYNC_RING; i++)
        if (sy_own[i].used && sy_same_key(&sy_own[i], &r)) { if (sy_mark(&sy_own[i], r.player)) sy_compare(&sy_own[i], &r); return 1; }
    for (i = 0; i < SYNC_RING; i++)
        if (sy_own[i].used && sy_is_skew(&sy_own[i], &r)) { sy_skew(&sy_own[i], &r); break; }
    for (i = 0; i < SYNC_RING; i++)   /* a re-run of the same key (rollback reload) replaces the old one */
        if (sy_peer[i].used && sy_peer[i].player == r.player && sy_same_key(&sy_peer[i], &r)) sy_peer[i].used = 0;
    sy_peer[sy_peer_i] = r;
    sy_peer_i = (sy_peer_i + 1) % SYNC_RING;
    return 1;
}

static DWORD sy_pack(const SyncRec *r, BYTE *b)
{
    DWORD *d = (DWORD *)b;
    d[0] = r->kind; d[1] = r->turn; d[2] = r->tick; d[3] = r->ba; d[4] = r->bb; d[5] = r->player; d[6] = r->nreg; d[7] = r->epoch;
    memcpy(b + SY_HDR, r->h, r->nreg * 4);
    return SY_HDR + r->nreg * 4;
}

/* game thread, from dp2_Send, cs NOT held: make our record at a sync point, compare with what peers sent, send ours */
static void sync_on_send(const BYTE *buf, DWORD size)
{
    DWORD kind = 0, tick, turn, n, ov;
    SyncRec r;
    BYTE out[SY_HDR + SYNC_MAXR * 4];
    int i;
    if (!g_exe_ok || size < 1) return;
    sy_flush_title();
    ov = RD32(0x57c8fc);
    tick = RD32(0x57c998);
    rs_test_corrupt(buf, ov, tick);   /* test switch only (LOTR2NET_TEST_CORRUPT); off by default */
    if (buf[0] == 0x08 && ov == 0) kind = SY_TURN;
    else if (buf[0] == 0x0c && ov == 2) kind = SY_BATTLE;
    else if (buf[0] == 0x0a && ov == 0 && tick % 100 == 0) kind = SY_TICK;
    if (!kind) return;
    turn = RD32(0x553240);
    if (kind == sy_last_kind && turn == sy_last_turn && tick == sy_last_tick && sy_epoch == sy_last_epoch) return;   /* the 2nd/3rd copy */
    sy_last_kind = kind; sy_last_turn = turn; sy_last_tick = tick; sy_last_epoch = sy_epoch;
    if (kind == SY_TURN) sy_end_turn = turn;
    sy_fill(&r, kind);
    n = sy_pack(&r, out);
    EnterCriticalSection(&cs);
    if (G.open) {
        SyncRec *own = &sy_own[sy_own_i];
        for (i = 0; i < SYNC_RING; i++) if (sy_own[i].used && sy_same_key(&sy_own[i], &r)) sy_own[i].used = 0;
        *own = r;
        sy_own_i = (sy_own_i + 1) % SYNC_RING;
        if (kind == SY_BATTLE) rs_note_battle(r.turn, r.epoch, r.ba, r.bb);
        for (i = 0; i < SYNC_RING; i++)
            if (sy_peer[i].used && sy_same_key(&sy_peer[i], own)) {
                if (sy_mark(own, sy_peer[i].player)) sy_compare(own, &sy_peer[i]);
                sy_peer[i].used = 0;
            } else if (sy_peer[i].used && sy_is_skew(own, &sy_peer[i])) sy_skew(own, &sy_peer[i]);
        if (G.host) bcast(-1, T_SYNC, out, n);
        else jsend(T_SYNC, out, n);   /* queued on the writer: never blocks the game thread */
    }
    LeaveCriticalSection(&cs);
}

/* Called from the cave at the top of setup_battle (exepatch.c patch_rally), on the game thread, every peer, before the
 * first battle tick. Zeroes the AI rally flag/point so no battle inherits the last one's (per peer) values. */
static void rally_clear(void)
{
    DWORD turn = RD32(0x553240), f, x, y;
    if (!G.open) return;
    sd_write(turn, "setup");
    f = RD32(0x56d89c); x = RD32(0x522f70); y = RD32(0x522f74);
    RD32(0x56d89c) = 0; RD32(0x522f70) = 0; RD32(0x522f74) = 0;
    logf_("rally cleared turn=%lu (was flag=%lu x=%lu y=%lu)", turn, f, x, y);
}

/* cs held: a new session starts clean (game 2 in one process must not match game 1's records or keep its title) */
static void sy_reset(void)
{
    memset(sy_own, 0, sizeof sy_own); memset(sy_peer, 0, sizeof sy_peer);
    sy_own_i = sy_peer_i = 0;
    sy_last_kind = sy_last_turn = sy_last_tick = sy_last_epoch = 0;
    sy_epoch = 0; sy_end_turn = 0xffffffffu;
    sy_desyncs = sy_oks = sy_diffs = sy_titled = sy_lines = 0;
    sy_diff_turn = sy_bt = sy_skew_turn = 0xffffffffu; sy_bba = sy_bbb = 0; sy_diff_in_turn = sy_in_b = 0;
}
