/* resync.c - automatic repair of a split game (dplayx v7), #included by dplayx.c right after sync.c. Our own code.
 *
 * WHAT: when our turn-end check (sync.c, kind 1) finds that a peer's state differs from the host's, the game is put
 * back in step at that turn's end, with no player action:
 *   1. every peer keeps a copy of its own save image (the 225 entries of the save table at 0x4de960, 267028 bytes)
 *      taken at its turn-end sync point (msg 0x08, the same sim point on every peer);
 *   2. a joiner that sees a turn-end DESYNC uploads the records it is the AUTHORITY for (below) from that copy;
 *   3. the host, when its game sends the end-of-turn verdict (msg 0x16, copy 0), builds the AUTHORITATIVE turn-end
 *      state: its own copy, with every object whose authority is another (flagged) peer replaced by that peer's
 *      upload, plus its own castles.dat; writes it as its lastturn.sva, ships it (bounded, chunked, hashed) to every
 *      joiner ahead of the 0x16 on the same FIFO, and turns the verdict into the game's own rollback;
 *   4. every peer's game then loads that file through its OWN load path (FUN_0049b973 -> load_state_from_file, with
 *      every post-load fixup), each joiner having merged in its own local-only bytes first. The safe point is the
 *      game's own rollback; no game memory is written by us except the verdict byte and the rollback counter.
 *
 * AUTHORITY (Matthew 2026-10-07), decided from the HOST's turn-end copy so every peer gets the same answer:
 *   - battle owner first: for a battle fought this turn, the peer playing the battle owner lord (the lord of army A
 *     if A is human, else of army B if B is human - the game's own rule, FUN_004a6a30:21-32; that peer is the one
 *     with 0x5533d0 = 1 that sends the 0x33 result) is the authority for both armies' records, the counties each
 *     army stood in, and both lords' noble records;
 *   - otherwise the owner: county c -> the peer playing county+0x05; army i -> the peer playing army+0x00; noble l ->
 *     the peer playing lord l;
 *   - the host for everything else: AI and neutral objects, RNG, season, turn counter, every other shared byte, and
 *     castles.dat. No averaging, no voting: the authoritative copy overwrites.
 * A peer that is in step with the host needs no upload (its records hash-equal the host's).
 * v9 (peer trust): the "otherwise the owner" rule above is OFF by default: the host's copy wins for every object that
 * was not in a battle the host saw a peer own (rs_owner_mode, resync_authority=owner on the host turns it back on).
 *
 * BOUNDS: at most RS_TRIES repairs per turn number; uploads waited for at most RS_WAIT_MS; the file is RS_FILE bytes
 * per joiner in RS_CHUNK pieces. VERSION: RS_VER in every BEGIN frame, plus PROTO (dll v7) at connect.
 * REFUSED (stock behaviour kept, logged): resync=0 (lords2hd.ini) or LOTR2NET_RESYNC=0 on the HOST; exe not the
 * 5 Jun 1997 build or a save table that does not walk to exactly 225 entries / 267028 bytes with the expected
 * tables; no turn-end copy for this turn; castles.dat not 204800 bytes; a file write failing.
 * NOT REPAIRED: local-only bytes (camera, selection, fog bits, advisor slots, battle leftovers: kept per peer by
 * design); a split inside a battle until that turn ends; a split where only bytes outside our 37 checked regions
 * differ (REST, detection only - see sync.c). ASCII only. */

#define T_RESYNC 11
#define RS_VER 1
#define RS_NENT 225
#define RS_TABLE 0x4de960u
#define RS_IMG 267028u
#define RS_CAST 204800u        /* 16 x 0x3200: castles.dat, appended by save_state_to_file */
#define RS_FILE (RS_IMG + RS_CAST)
#define RS_CHUNK 32768u
#define RS_TRIES 3
#define RS_WAIT_MS (g_slowlink ? 8000u : 4000u)
#define RS_NPEER 8
#define RS_NBAT 16
#define RS_BEGIN_MS 500u       /* host: least gap between two upload BEGINs from one peer (a real peer uploads once per turn and epoch) */
#define VA_COUNTY 0x53f9b0u    /* 17 x 0x300, record 0 unused; +0x05 owner lord */
#define VA_ARMY 0x52f0b0u      /* 151 x 0x1a4, record 0 unused; +0x00 owner lord, +0x01 is-human, +0x10 county */
#define VA_NOBLE 0x57bf00u     /* 6 x 0x160, record 0 unused; record l = lord l */
#define RD8(va) (*(volatile BYTE *)(va))
enum { RS_DN_BEGIN = 1, RS_DN_DATA, RS_DN_END, RS_UP_BEGIN, RS_UP_DATA, RS_UP_END };

static int rs_ok = -1;                 /* -1 not checked yet, 0 refused (rs_why), 1 ready */
static char rs_why[128];
static DWORD rs_eptr[RS_NENT], rs_esz[RS_NENT], rs_eoff[RS_NENT];
static BYTE *rs_mask;                  /* per image byte: 0 shared, 1 local (own kept), 2 grid flags (own bits 0x61 kept), 3 grid anim (own kept) */
static DWORD rs_county, rs_army, rs_noble;   /* image offsets of record 0 */
static BYTE *rs_snap;                  /* own turn-end image (game thread writes, under cs) */
static DWORD rs_snap_turn, rs_snap_epoch;
static int rs_snap_ok;
static DWORD rs_try_turn = 0xffffffffu;
static int rs_tries, rs_patch = -1;
static DWORD rs_verify_turn, rs_verify_epoch;
static int rs_verify;
/* host */
static struct { DPID dpid; DWORD lord; } rs_peer[RS_NPEER];
static DPID rs_flag[RS_NPEER];
static int rs_nflag;
static DWORD rs_flag_turn, rs_flag_epoch;
static struct { DPID dpid; DWORD turn, epoch, ranges, bytes, t_begin; int begun, done, bad, dirty; BYTE *img, *have; } rs_up[RS_NPEER];
static struct { DPID dpid; DWORD epoch; } rs_giveup[RS_NPEER];   /* peers whose upload timed out: not waited on again in that epoch */
/* both */
static struct { DWORD turn, epoch, ba, bb, owner, la, lb, ca, cb; } rs_bat[RS_NBAT];
static int rs_nbat;
static DWORD rs_up_turn = 0xffffffffu, rs_up_epoch;   /* joiner: the upload already sent */
static DWORD rs_exp_turn = 0xffffffffu, rs_exp_epoch;  /* joiner: (turn, epoch) of the desync WE detected = the only download we take */
/* joiner download */
static struct { BYTE *buf; DWORD total, got, fnv, turn, epoch; int on; } rs_dn;
static volatile LONG rs_ready;
static DWORD rs_ready_turn, rs_ready_epoch;

static DWORD rs_lord(void) { return g_exe_ok ? RD32(0x57c8cc) & 0xff : 0; }   /* no game memory without the known exe (selftest) */
static int rs_check_img(const BYTE *img, char *why, int cap);

static int rs_enabled(void)
{
    char v[16], *e = getenv("LOTR2NET_RESYNC");
    if (e && *e) return cfg_on(e);
    if (ini_get("resync", v, sizeof v)) return cfg_on(v);
    return 1;
}

/* image offset of [va, va+len) if it lies inside one save-table entry, else -1 */
static long rs_off(DWORD va, DWORD len)
{
    int i;
    for (i = 0; i < RS_NENT; i++)
        if (va >= rs_eptr[i] && va + len <= rs_eptr[i] + rs_esz[i]) return (long)(rs_eoff[i] + (va - rs_eptr[i]));
    return -1;
}

static void rs_mark(DWORD va, DWORD len, BYTE v)
{
    long o = rs_off(va, len);
    if (o >= 0) memset(rs_mask + o, v, len);
}

/* Walk the save table exactly as save/load do (`< 0xe1` entries), check it is the layout these rules were written
 * for, and build the local-byte mask (the same classes as tools/container-harness/mp_lastturn_cmp.py). */
static int rs_validate(void)
{
    static const DWORD local_va[] = {0x5651b4, 0x5651b8, 0x57c93c, 0x57c8cc, 0x56d5cc, 0x553fbc, 0x553c64, 0x53e9bc,
                                     /* rig REST-DIFF (rs1/restdiff/REPORT.md): per-peer scheduler/UI scalars the peers hold 1 apart at
                                      * any turn end, so they stay out of the REST hash and the host's file never overwrites them:
                                      * within-phase tick, AI round-robin cursor, UI popup flag */
                                     0x53f658, 0x57c944, 0x57a0c4,
                                     /* REST-DIFF 2026-10-08 (.workers/out/mp-fps/restdiff): merchant goods picked in the local UI;
                                      * the wire trade handler FUN_004438c3 takes the item from the message, only UI paths read it */
                                     0x56d8ac,
                                     /* REST-DIFF 2026-10-08 (.workers/out/mp-only/mixed1, every turn from turn 1): the single-player
                                      * campaign index (specs/new-game-and-victory.md B10), written only by the Campaign button
                                      * 0x433461 and a won game 0x49788c, read only by the scenario loader 0x499e5d and the post-win
                                      * click 0x431883; each copy keeps the value of the last campaign it played (3 vs 2) */
                                     0x53f258};
    static const DWORD whole[] = {0x568480 /* advisor slots */, 0x554480, 0x57a100, 0x566520, 0x553400, 0x55ce70,
                                  0x5678a0, 0x5533d0, 0x568470, 0x53e984, 0x565404, 0x553c58, 0x56d6ac, 0x553f74,
                                  0x53f084, 0x53f028 /* battle tables left from the last battle */,
                                  /* battle AI rally flag/point + DEFEND hit counter: read only in battle, reset at every battle
                                   * setup (0x553f28 stock, the other three by v11 patch_rally); a loaded save carries each
                                   * peer's own values (REST-DIFF every turn after an MP load until the first battle).
                                   * With mpfix=0 (no rally clear) a split here is real - REST is detection only anyway. */
                                  0x56d89c, 0x522f70, 0x522f74, 0x553f28};
    IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nh;
    DWORD i, k, off = 0, lo, hi;
    if (rs_ok >= 0) return rs_ok;
    rs_ok = 0;
    if (!g_exe_ok) { lstrcpyA(rs_why, "the exe is not the 5 Jun 1997 build"); return 0; }
    nh = (IMAGE_NT_HEADERS *)((BYTE *)dh + dh->e_lfanew);
    lo = (DWORD)dh; hi = lo + nh->OptionalHeader.SizeOfImage;
    for (i = 0; i < RS_NENT; i++) {
        DWORD p = ((DWORD *)RS_TABLE)[i * 2], sz = ((DWORD *)RS_TABLE)[i * 2 + 1];
        if (!p || !sz || p < lo || sz > hi - lo || p + sz > hi) {
            snprintf(rs_why, sizeof rs_why, "save table entry %lu (%08lx, %lu bytes) is not inside the exe image", i, p, sz);
            return 0;
        }
        rs_eptr[i] = p; rs_esz[i] = sz; rs_eoff[i] = off; off += sz;
    }
    if (off != RS_IMG || rs_eptr[0] != 0x522f90 || rs_esz[0] != 0x8000) {
        snprintf(rs_why, sizeof rs_why, "save table is %lu bytes, first entry %08lx/%lx (expected %u, 522f90/8000)", off, rs_eptr[0], rs_esz[0], RS_IMG);
        return 0;
    }
    {
        long c = rs_off(VA_COUNTY, 17 * 0x300), a = rs_off(VA_ARMY, 151 * 0x1a4), nb = rs_off(VA_NOBLE, 6 * 0x160);
        if (c < 0 || a < 0 || nb < 0) {
            snprintf(rs_why, sizeof rs_why, "county/army/noble tables not found whole in the save table (%ld %ld %ld)", c, a, nb);
            return 0;
        }
        rs_county = (DWORD)c; rs_army = (DWORD)a; rs_noble = (DWORD)nb;
    }
    rs_mask = (BYTE *)calloc(RS_IMG, 1);
    rs_snap = (BYTE *)malloc(RS_IMG);
    if (!rs_mask || !rs_snap) { lstrcpyA(rs_why, "no memory"); free(rs_mask); free(rs_snap); rs_mask = rs_snap = NULL; return 0; }
    for (k = 0; k < 0x8000; k++) rs_mask[k] = (k % 8 == 2) ? 2 : (k % 8 == 3) ? 3 : 0;   /* map grid cells */
    memset(rs_mask + 0x8000, 1, 0x14800 - 0x8000);                                     /* view / battle-grid buffer */
    for (k = 0; k < sizeof local_va / sizeof *local_va; k++) rs_mark(local_va[k], 4, 1);
    for (k = 0; k <= 5; k++) {   /* lobby slot records 0x553d64 + k*0x2c (5-peer rig n5run2, 2026-10-07): a later joiner never learns an
                                  * earlier joiner's +0x08..+0x0f text and +0x13 human flag. Read only by the single-player start
                                  * (FUN_0049792e) and the defeat-movie pick (FUN_00475b41); noble +5 holds the game's human flag */
        rs_mark(0x553d6c + k * 0x2c, 8, 1);
        rs_mark(0x553d77 + k * 0x2c, 1, 1);
    }
    for (k = 0; k <= 16; k++) memset(rs_mask + rs_county + k * 0x300, 1, 5);             /* county +0..+4 */
    for (k = 0; k < sizeof whole / sizeof *whole; k++)
        for (i = 0; i < RS_NENT; i++) if (rs_eptr[i] == whole[k]) memset(rs_mask + rs_eoff[i], 1, rs_esz[i]);
    rs_ok = 1;
    return 1;
}

static DWORD rs_fnv(const BYTE *p, DWORD n, DWORD h) { while (n--) { h ^= *p++; h *= 0x01000193u; } return h; }

/* game thread, cs NOT held: copy the save image at our turn-end sync point; returns the REST hash (all shared bytes,
 * sync.c region 37) or 0 when resync is unavailable */
static int rs_write(const char *name, const BYTE *b, DWORD n);
static int rs_dump = -1;   /* test aid, off by default: lords2hd.ini resync_dump=1 writes each turn-end copy (rsdump_t<turn>_e<epoch>.bin)
                            * and the mask (rsmask.bin) beside the saves, so two peers' REST-DIFF can be diffed byte by byte */
static DWORD rs_snapshot(DWORD turn, DWORD epoch)
{
    DWORD i, h = 0x811c9dc5u;
    if (!rs_validate()) return 0;
    EnterCriticalSection(&cs);
    for (i = 0; i < RS_NENT; i++) memcpy(rs_snap + rs_eoff[i], (void *)rs_eptr[i], rs_esz[i]);
    rs_snap_turn = turn; rs_snap_epoch = epoch; rs_snap_ok = 1;
    LeaveCriticalSection(&cs);
    {   /* our own live state must pass the bounds the received images are held to (a false refusal shows up here) */
        char why[96];
        static int warned;
        if (!rs_check_img(rs_snap, why, sizeof why) && warned++ < 3) SLOG("RESYNC selfcheck turn=%lu: own state fails the bounds check: %s", turn, why);
    }
    if (rs_dump < 0) { char v[16]; rs_dump = ini_get("resync_dump", v, sizeof v) && cfg_on(v); if (rs_dump) rs_write("rsmask.bin", rs_mask, RS_IMG); }
    if (rs_dump) { char nm[48]; snprintf(nm, sizeof nm, "rsdump_t%lu_e%lu.bin", turn, epoch); rs_write(nm, rs_snap, RS_IMG); }
    for (i = 0; i < RS_IMG; i++)   /* only this thread writes rs_snap: safe to read outside cs */
        if (rs_mask[i] == 0) { h ^= rs_snap[i]; h *= 0x01000193u; }
        else if (rs_mask[i] == 2) { h ^= (BYTE)(rs_snap[i] & ~0x61); h *= 0x01000193u; }
    return h ? h : 1;
}

/* ---- record bounds (review 2026-10-07: a received image or record must never carry an index the game would use
 * out of range when it loads it). Every bound is from the save-layout specs; one bad field refuses the WHOLE
 * image or upload, never part of it.
 *   army (151 x 0x1a4, 0 unused), specs/army-records.md:
 *     +0x00 owner 0..6 (:181 lord slot 1-5, 6 sentinel, 0 free)
 *     +0x01 is-human 0..2 (:182 says 0/1; 2 = the REMOTE human in a network game, rig lastturn.sva 2026-10-07;
 *           the 236 single-player corpus saves stay 0..1)
 *     +0x08 kind 0..4 (:188)    +0x0a/+0x0b grid cell x/y 0..63 (:189-190; 64x64 grid,
 *           cell = 0x522f90 + ((y<<6)+x)*8, owner6-and-kind2-contacts.md:39)
 *     +0x10 / +0x11 / +0x151 / +0x198 / +0x199 county 0..16 (:191-192, :203, :215-216; 17 county records, slot 0 unused)
 *     +0x19a army slot 0..150 (:217)     +0x1c waypoint count 0..150 (:198-199, u8[300] = 150 pairs)
 *     +0x14c route status 0..2 (:200)    +0x195 troop type 0..6 (:212)     +0x197 company 0..12 (:214)
 *   county (17 x 0x300, 0 unused): +0x05 owner 0..6 (owner6-and-kind2-contacts.md:41, :235 "county owner != 6");
 *     +0x1bc garrison army slot 0..150 (army-records.md:215, kind2-revolt-garrison-handoff.md:30)
 *   noble/lord (6 x 0x160, 0 unused): +0x05 is-human 0..2 (record-model.md:171; 2 = remote human, as army +0x01); +0xe5 muster county 0..16
 *     (army-records.md:192); +0xe8 objective county 0..16 (army-records.md:218)
 *   grid (4096 cells x 8 at image offset 0): f7 = county id (ai-farm-plan-sow-and-pasture.md:260). NOT a 0..16 bound:
 *     the rig's first v7 run (2026-10-07) found 84 cells with 0x20 on a live map, which the game tolerates (it tests
 *     f7 < 0x11 itself, path_build_terrain_cost_grid 0x46ff43, FUN_00467770 0x467770). f7 is static map data: its
 *     only writer is the map loader FUN_00467770 (decomp grep of 0x522f97: no other store). So the check is EQUALITY
 *     with this peer's own live grid: an image whose f7 differs anywhere is refused.
 * Returns 1 ok; 0 with why filled. kind 0 county, 1 army, 2 noble. */
static int rs_check_rec(int kind, DWORD id, const BYTE *r, char *why, int cap)
{
    static const struct { BYTE kind; WORD off; BYTE max; const char *nm; } F[] = {
        {1, 0x00, 6, "owner"}, {1, 0x01, 2, "is-human"}, {1, 0x08, 4, "kind"}, {1, 0x0a, 63, "cell x"}, {1, 0x0b, 63, "cell y"},
        {1, 0x10, 16, "county"}, {1, 0x11, 16, "home county"}, {1, 0x151, 16, "target county"}, {1, 0x198, 16, "garrison county"},
        {1, 0x199, 16, "besieged county"}, {1, 0x19a, 150, "besieger slot"}, {1, 0x1c, 150, "waypoint count"},
        {1, 0x14c, 2, "route status"}, {1, 0x195, 6, "merc type"}, {1, 0x197, 12, "merc company"},
        {0, 0x05, 6, "owner"}, {0, 0x1bc, 150, "garrison slot"},
        {2, 0x05, 2, "is-human"}, {2, 0xe5, 16, "muster county"}, {2, 0xe8, 16, "objective county"}};
    unsigned k;
    for (k = 0; k < sizeof F / sizeof *F; k++)
        if (F[k].kind == kind && r[F[k].off] > F[k].max) {
            snprintf(why, cap, "%s %lu +0x%x %s = %u, max %u", kind == 0 ? "county" : kind == 1 ? "army" : "noble", id,
                     F[k].off, F[k].nm, r[F[k].off], F[k].max);
            return 0;
        }
    return 1;
}

/* the whole image (host before it writes/ships, joiner before it writes/loads) */
static int rs_check_img(const BYTE *img, char *why, int cap)
{
    DWORD i;
    for (i = 0; i < 4096; i++)
        if (img[i * 8 + 7] != RD8(0x522f97 + i * 8)) {
            snprintf(why, cap, "grid cell %lu county id %u, this peer's map has %u (static map data)", i, img[i * 8 + 7], RD8(0x522f97 + i * 8));
            return 0;
        }
    for (i = 1; i <= 16; i++) if (!rs_check_rec(0, i, img + rs_county + i * 0x300, why, cap)) return 0;
    for (i = 1; i <= 150; i++) if (!rs_check_rec(1, i, img + rs_army + i * 0x1a4, why, cap)) return 0;
    for (i = 1; i <= 5; i++) if (!rs_check_rec(2, i, img + rs_noble + i * 0x160, why, cap)) return 0;
    return 1;
}

/* the DPID the game's lord table gives lord l (0 = none). Without the known exe there is no table: only the selftest
 * supplies one, LOTR2NET_TEST_LORDTAB="dpid-hex:lord,...", read per call, never consulted when the exe is known. */
static DPID rs_tab(DWORD l)
{
    const char *e;
    unsigned long d, k;
    if (g_exe_ok) return RD32(0x553d50 + l * 0x2c);
    for (e = getenv("LOTR2NET_TEST_LORDTAB"); e && *e; ) {
        if (sscanf(e, "%lx:%lu", &d, &k) == 2 && k == l) return (DPID)d;
        if ((e = strchr(e, ',')) != NULL) e++;
    }
    return 0;
}

/* the lord slot the GAME has bound to DPID d: its own table sv_14800 = 0x553d50 (6 x 0x2c, +0 = DPID), filled by msg
 * 0x0f from each peer (FUN_0043e9e2, FUN_0043e98b; dplay-surface.md:125-127; ai-turn-structure.md:1285). 0 = none. */
static DWORD rs_game_lord_of(DPID d)
{
    DWORD l;
    if (!d) return 0;
    for (l = 1; l <= 5; l++) if (rs_tab(l) == d) return l;
    return 0;
}

/* Host, cs held, for every sync record / upload from conn ci (player cid): the conn may speak only for ONE lord.
 * The binding comes from the game's own lord table for that DPID when it is filled; else the first valid record
 * binds it and it is locked. A lord already bound to another live conn, or the host's own lord, is never bound to
 * this one. Returns 1 when lord byte rl is the bound lord, 0 = drop (logged). */
static int rs_conn_lord(int ci, DPID cid, DWORD rl, const char *what)
{
    Conn *c = &G.conns[ci];
    DWORD gl = rs_game_lord_of(cid), me = rs_lord();
    int i;
    if (!cid) return 0;
    if (gl && gl != me && c->lord != gl) {
        SLOG("RESYNC bind: conn %d (player %08lx) -> lord %lu, from the game's own lord table (0x553d50)%s", ci, cid, gl,
             c->lord ? " - replaces the lord learned from its first record" : "");
        c->lord = gl;
        for (i = 0; i < MAXCONN; i++)   /* the table wins: nobody else keeps this lord (review F1: two conns held one lord) */
            if (i != ci && G.conns[i].used && G.conns[i].lord == gl) {
                SLOG("RESYNC unbind: conn %d no longer holds lord %lu, the game's table gives it to player %08lx", i, gl, cid);
                G.conns[i].lord = 0;
            }
    }
    if (!c->lord) {
        DPID tab = rl >= 1 && rl <= 5 ? rs_tab(rl) : 0;
        int taken = rl == me, tabbed = tab && tab != cid && pl_find(tab);   /* the game's table gives rl to another LIVE player */
        for (i = 0; i < MAXCONN; i++) if (i != ci && G.conns[i].used && G.conns[i].lord == rl) taken = 1;
        if (rl < 1 || rl > 5 || taken || tabbed) {
            c->bad_lord++;
            if (c->bad_lord <= 5 || !(c->bad_lord % 100))
                SLOG("RESYNC DROPPED %s from conn %d (player %08lx): lord byte %lu is %s - not bound", what, ci, cid, rl,
                     rl < 1 || rl > 5 ? "out of range" : rl == me ? "the host's own lord" : tabbed ? "given to another player by the game's lord table" : "bound to another connection");
            return 0;
        }
        c->lord = rl;
        SLOG("RESYNC bind: conn %d (player %08lx) -> lord %lu, learned from its first record and locked (the game's table has no entry yet)", ci, cid, rl);
    }
    if (rl != c->lord) {
        if (++c->bad_lord <= 5 || !(c->bad_lord % 100))
            SLOG("RESYNC DROPPED %s from conn %d (player %08lx): lord byte %lu, the conn is bound to lord %lu (%d dropped)", what, ci, cid, rl, c->lord, c->bad_lord);
        return 0;
    }
    return 1;
}

/* cs held: who plays which lord, from sync records (player = dpid << 8 | lord). On the host the records were
 * already checked by rs_conn_lord. Never remaps a lord bound to another live DPID, never binds our own lord. */
static void rs_note_peer(DWORD player)
{
    DPID d = player >> 8;
    DWORD l = player & 0xff;
    int i, fr = -1;
    if (!d || d == G.myid || l < 1 || l > 5 || l == rs_lord()) return;
    for (i = 0; i < RS_NPEER; i++)
        if (rs_peer[i].dpid && rs_peer[i].dpid != d && rs_peer[i].lord == l && pl_find(rs_peer[i].dpid)) {
            SLOG("RESYNC: player %08lx claims lord %lu, already bound to live player %08lx - ignored", d, l, rs_peer[i].dpid);
            return;
        }
    for (i = 0; i < RS_NPEER; i++) {
        if (rs_peer[i].dpid == d) { if (rs_peer[i].lord != l) SLOG("RESYNC: player %08lx stays lord %lu (record said %lu)", d, rs_peer[i].lord, l); return; }
        if ((!rs_peer[i].dpid || !pl_find(rs_peer[i].dpid)) && fr < 0) fr = i;
    }
    if (fr >= 0) { rs_peer[fr].dpid = d; rs_peer[fr].lord = l; }
}

/* cs held: the dpid allowed to speak for lord l. Host: the game's lord table, else the conn bound to l (rs_conn_lord),
 * never anything a record claims. 0 = nobody connected (the host decides for it). */
static DPID rs_dpid_of(DWORD l)
{
    int i, k;
    if (l < 1 || l > 5) return 0;
    if (l == rs_lord()) return G.myid;
    if (G.host) {
        DPID t = rs_tab(l);   /* the game's own table first; the conn binding only where the table has no live entry */
        if (t && pl_find(t)) return t;
        for (i = 0; i < MAXCONN; i++)
            if (G.conns[i].used && G.conns[i].haveplayer && G.conns[i].lord == l)
                for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn == i) return G.pl[k].id;
        return 0;
    }
    for (i = 0; i < RS_NPEER; i++) if (rs_peer[i].dpid && rs_peer[i].lord == l && pl_find(rs_peer[i].dpid)) return rs_peer[i].dpid;
    return 0;
}

/* cs held: a battle THIS peer ran (its own battle sync point, sync_on_send only - never a peer's record: review
 * 2026-10-07, a peer could invent one). The owner is decided once, from this peer's memory at the first sighting, by
 * the game's own rule (FUN_004a6a30: army A's lord if A is human, else B's if B is human). The battle covers: the two
 * armies, the county it was fought in (army B's county - the defender's; A's is logged), and the two lords. */
static void rs_note_battle(DWORD turn, DWORD epoch, DWORD ba, DWORD bb)
{
    int i;
    DWORD a, b, la, lb, ha, hb;
    if (ba < 1 || ba > 150 || bb < 1 || bb > 150) return;
    for (i = 0; i < rs_nbat; i++)
        if (rs_bat[i].turn == turn && rs_bat[i].epoch == epoch && rs_bat[i].ba == ba && rs_bat[i].bb == bb) return;
    if (rs_nbat && (rs_bat[0].turn != turn || rs_bat[0].epoch != epoch)) rs_nbat = 0;   /* a new turn: forget the old */
    if (rs_nbat >= RS_NBAT) return;
    a = VA_ARMY + ba * 0x1a4; b = VA_ARMY + bb * 0x1a4;
    la = RD8(a); lb = RD8(b); ha = RD8(a + 1); hb = RD8(b + 1);
    i = rs_nbat++;
    rs_bat[i].turn = turn; rs_bat[i].epoch = epoch; rs_bat[i].ba = ba; rs_bat[i].bb = bb;
    rs_bat[i].la = la; rs_bat[i].lb = lb;
    rs_bat[i].ca = RD8(b + 0x10); rs_bat[i].cb = rs_bat[i].ca;
    rs_bat[i].owner = (ha && la >= 1 && la <= 5) ? la : (hb && lb >= 1 && lb <= 5) ? lb : 0;
    SLOG("RESYNC battle noted (own lockstep) turn=%lu armies %lu (lord %lu%s, county %u) vs %lu (lord %lu%s) in county %lu: owner lord %lu (own 0x5533d0=%lu)",
         turn, ba, la, ha ? ", human" : "", RD8(a + 0x10), bb, lb, hb ? ", human" : "", rs_bat[i].ca, rs_bat[i].owner,
         RD32(0x5533d0));
}

/* Host, cs held: a peer's BATTLE record is kept (for the compare) only if the host ran that battle too: an own record
 * with the same key, or the host is in that battle now (its own record is still to come). Else dropped, logged. */
static int rs_peer_battle_ok(DWORD turn, DWORD epoch, DWORD ba, DWORD bb, DWORD player)
{
    int i;
    static int dropped;
    if (!G.host) return 1;
    for (i = 0; i < rs_nbat; i++)
        if (rs_bat[i].turn == turn && rs_bat[i].epoch == epoch && rs_bat[i].ba == ba && rs_bat[i].bb == bb) return 1;
    if (RD32(0x57c8fc) == 2 && RD32(0x568210) == ba && RD32(0x568218) == bb && RD32(0x553240) == turn && sy_epoch == epoch) return 1;
    if (++dropped <= 5 || !(dropped % 100))
        SLOG("RESYNC DROPPED battle record from player %lu (%08lx): turn %lu epoch %lu armies %lu vs %lu - the host ran no such battle; no authority granted (%d dropped)",
             player & 0xff, player >> 8, turn, epoch, ba, bb, dropped);
    return 0;
}

/* cs held: the battle owner lord for an object this turn, 0 = none. kind 0 county, 1 army, 2 noble. Last battle wins. */
static DWORD rs_battle_auth(int kind, DWORD id, DWORD turn, DWORD epoch)
{
    int i;
    DWORD o = 0;
    for (i = 0; i < rs_nbat; i++) {
        if (rs_bat[i].turn != turn || rs_bat[i].epoch != epoch || !rs_bat[i].owner) continue;
        if ((kind == 0 && rs_bat[i].ca == id) || (kind == 1 && (rs_bat[i].ba == id || rs_bat[i].bb == id)) ||
            (kind == 2 && (rs_bat[i].la == id || rs_bat[i].lb == id)))
            o = rs_bat[i].owner;
    }
    return o;
}

/* AUTHORITY POLICY (v9, peer trust, review tb2 dplayx F1): outside a battle every peer runs the same lockstep commands,
 * so a peer's copy of its own counties, armies and noble holds nothing the host lacks, and taking it let a joiner write
 * any gold, troops or waypoints into everyone's game. So the host's copy now wins for everything EXCEPT the objects of a
 * battle the host itself saw that peer own this turn (rs_note_battle: the owner is the only one who played it out).
 * lords2hd.ini on the HOST: resync_authority=owner restores the v8 rule (every peer is the authority for its own
 * objects). Read once. */
static int rs_owner_auth = -1;
static int rs_owner_mode(void)
{
    if (rs_owner_auth < 0) {
        char v[16];
        rs_owner_auth = ini_get("resync_authority", v, sizeof v) && !_stricmp(v, "owner");
        SLOG("RESYNC authority policy: %s", rs_owner_auth ? "owner (lords2hd.ini resync_authority=owner): each peer for its own objects, the battle owner for its battles"
                                                         : "host: the host's copy for everything except battles the host saw a peer own");
    }
    return rs_owner_auth;
}
/* host, cs held: does flagged peer d have to upload for (turn, epoch)? Under the host policy only a battle owner does. */
static int rs_needs_up(DPID d, DWORD turn, DWORD epoch)
{
    int i;
    if (rs_owner_mode()) return 1;
    for (i = 0; i < rs_nbat; i++)
        if (rs_bat[i].turn == turn && rs_bat[i].epoch == epoch && rs_bat[i].owner && rs_dpid_of(rs_bat[i].owner) == d) return 1;
    return 0;
}

static int rs_tc_poison = 0;   /* test: the next upload carries one army record with an out-of-range owner (rs_test_corrupt mode 5) */
static void rs_up_range(DWORD off, DWORD len, DWORD *nr, DWORD *nb)
{
    BYTE fr[8 + 0x300];
    if (len > 0x300) return;
    ((DWORD *)fr)[0] = RS_UP_DATA; ((DWORD *)fr)[1] = off;
    memcpy(fr + 8, rs_snap + off, len);
    if (rs_tc_poison == 1 && len == 0x1a4) {
        rs_tc_poison = 2;
        fr[8] = 0xee;
        SLOG("TEST CORRUPT: upload record at image offset %lu (army %lu) sent with owner byte 0xee", off, (off - rs_army) / 0x1a4);
    }
    jsend(T_RESYNC, fr, 8 + len);
    (*nr)++; *nb += len;
}

/* joiner, cs held: upload, from our turn-end copy, every record this peer is the authority for in its own view
 * (its lord's counties, armies and noble; for battles it owned this turn, both armies, the battle county and both
 * nobles). The host picks what it needs by ITS view and its own bindings. Once per (turn, epoch). */
static void rs_upload(DWORD turn, DWORD epoch)
{
    DWORD me = rs_lord(), i, nr = 0, nb = 0, h[5];
    int b;
    if (G.host || !rs_snap_ok || rs_snap_turn != turn || rs_snap_epoch != epoch) return;
    if (rs_up_turn == turn && rs_up_epoch == epoch) return;
    rs_up_turn = turn; rs_up_epoch = epoch;
    h[0] = RS_UP_BEGIN; h[1] = RS_VER; h[2] = turn; h[3] = epoch; h[4] = me;
    jsend(T_RESYNC, h, 20);
    for (i = 1; i <= 16; i++) if (rs_snap[rs_county + i * 0x300 + 5] == me || rs_battle_auth(0, i, turn, epoch) == me) rs_up_range(rs_county + i * 0x300, 0x300, &nr, &nb);
    for (i = 1; i <= 150; i++) if (rs_snap[rs_army + i * 0x1a4] == me || rs_battle_auth(1, i, turn, epoch) == me) rs_up_range(rs_army + i * 0x1a4, 0x1a4, &nr, &nb);
    for (i = 1; i <= 5; i++) if (i == me || rs_battle_auth(2, i, turn, epoch) == me) rs_up_range(rs_noble + i * 0x160, 0x160, &nr, &nb);
    h[0] = RS_UP_END; h[1] = turn; h[2] = epoch; h[3] = nr;
    jsend(T_RESYNC, h, 16);
    for (b = 0; b < rs_nbat; b++) if (rs_bat[b].owner == me && rs_bat[b].turn == turn && rs_bat[b].epoch == epoch) break;
    SLOG("RESYNC upload turn=%lu epoch=%lu: %lu records, %lu bytes (lord %lu%s)", turn, epoch, nr, nb, me,
         b < rs_nbat ? ", includes battles this peer owned" : "");
}

/* cs held, from sync.c: a turn-end compare differed in regions 0..36 (or the turn ended at another tick) */
static void rs_on_turn_desync(DWORD turn, DWORD epoch, DWORD peer_player)
{
    int i;
    DPID d = peer_player >> 8;
    if (!G.host) { rs_exp_turn = turn; rs_exp_epoch = epoch; rs_upload(turn, epoch); return; }   /* the only download we will take */
    if (rs_flag_turn != turn || rs_flag_epoch != epoch) { rs_nflag = 0; rs_flag_turn = turn; rs_flag_epoch = epoch; }
    for (i = 0; i < rs_nflag; i++) if (rs_flag[i] == d) return;
    if (d && rs_nflag < RS_NPEER) rs_flag[rs_nflag++] = d;
}

/* image offset -> (kind, id) when [off, off+len) is exactly one county/army/noble record (1..max), else -1 */
static int rs_rec_at(DWORD off, DWORD len, DWORD *id)
{
    if (len == 0x300 && off >= rs_county + 0x300 && off < rs_county + 17 * 0x300 && !((off - rs_county) % 0x300)) { *id = (off - rs_county) / 0x300; return 0; }
    if (len == 0x1a4 && off >= rs_army + 0x1a4 && off < rs_army + 151 * 0x1a4 && !((off - rs_army) % 0x1a4)) { *id = (off - rs_army) / 0x1a4; return 1; }
    if (len == 0x160 && off >= rs_noble + 0x160 && off < rs_noble + 6 * 0x160 && !((off - rs_noble) % 0x160)) { *id = (off - rs_noble) / 0x160; return 2; }
    return -1;
}

/* host, cs held: a T_RESYNC frame from conn ci (player cid): its upload. The upload must name the lord this conn is
 * bound to; every record must be exactly one county/army/noble record and pass rs_check_rec, or the WHOLE upload is
 * marked bad and never used. */
static void rs_on_up(int ci, DPID cid, const BYTE *f, DWORD n)
{
    DWORD sub = n >= 4 ? ((DWORD *)f)[0] : 0;
    int i, s = -1;
    if (!cid || n < 8 || !rs_validate()) return;
    for (i = 0; i < RS_NPEER; i++) if (rs_up[i].dpid == cid) { s = i; break; }
    if (s < 0) for (i = 0; i < RS_NPEER; i++) if (!rs_up[i].dpid || !pl_find(rs_up[i].dpid)) { s = i; break; }
    if (s < 0) return;
    if (sub == RS_UP_BEGIN && n >= 20) {
        if (((DWORD *)f)[1] != RS_VER) { SLOG("RESYNC upload from %08lx: version %lu, mine %d - ignored", cid, ((DWORD *)f)[1], RS_VER); return; }
        if (rs_up[s].dpid == cid && rs_up[s].begun && GetTickCount() - rs_up[s].t_begin < RS_BEGIN_MS) {   /* review F2: BEGIN flood */
            static int limited;
            if (++limited <= 5 || !(limited % 100)) SLOG("RESYNC upload BEGIN from %08lx ignored: under %u ms since its last one (%d ignored)", cid, RS_BEGIN_MS, limited);
            return;
        }
        if (!rs_up[s].img) rs_up[s].img = (BYTE *)malloc(RS_IMG);
        if (!rs_up[s].have) rs_up[s].have = (BYTE *)calloc(RS_IMG, 1);
        if (!rs_up[s].img || !rs_up[s].have) return;
        rs_up[s].dpid = cid; rs_up[s].turn = ((DWORD *)f)[2]; rs_up[s].epoch = ((DWORD *)f)[3];
        rs_up[s].ranges = rs_up[s].bytes = 0; rs_up[s].begun = 1; rs_up[s].done = 0; rs_up[s].bad = 0;
        rs_up[s].t_begin = GetTickCount();
        if (rs_up[s].dirty) { memset(rs_up[s].have, 0, RS_IMG); rs_up[s].dirty = 0; }   /* only if a record was stored since: a bare BEGIN costs nothing */
        if (!rs_conn_lord(ci, cid, ((DWORD *)f)[4], "resync upload")) { rs_up[s].bad = 1; return; }
    } else if (sub == RS_UP_DATA && rs_up[s].dpid == cid && rs_up[s].begun && !rs_up[s].done && !rs_up[s].bad) {
        DWORD off = ((DWORD *)f)[1], len = n - 8, id;
        char why[96];
        int kind = rs_rec_at(off, len, &id);
        if (kind < 0) { rs_up[s].bad = 1; SLOG("RESYNC upload from %08lx REFUSED: %lu bytes at image offset %lu is not one county/army/noble record", cid, len, off); return; }
        if (!rs_check_rec(kind, id, f + 8, why, sizeof why)) { rs_up[s].bad = 1; SLOG("RESYNC upload from %08lx REFUSED (whole upload): %s", cid, why); return; }
        memcpy(rs_up[s].img + off, f + 8, len);
        memset(rs_up[s].have + off, 1, len);
        rs_up[s].dirty = 1;
        rs_up[s].ranges++; rs_up[s].bytes += len;
    } else if (sub == RS_UP_END && n >= 16 && rs_up[s].dpid == cid && rs_up[s].begun) {
        if (!rs_up[s].bad && ((DWORD *)f)[1] == rs_up[s].turn && ((DWORD *)f)[2] == rs_up[s].epoch && ((DWORD *)f)[3] == rs_up[s].ranges) rs_up[s].done = 1;
        SLOG("RESYNC upload from %08lx turn=%lu epoch=%lu: %lu records, %lu bytes%s", cid, rs_up[s].turn, rs_up[s].epoch,
             rs_up[s].ranges, rs_up[s].bytes, rs_up[s].done ? "" : rs_up[s].bad ? " - REFUSED, not used" : " - INCOMPLETE, not used");
    }
}

static int rs_up_ready(DPID d, DWORD turn, DWORD epoch)
{
    int i;
    for (i = 0; i < RS_NPEER; i++)
        if (rs_up[i].dpid == d && rs_up[i].done && rs_up[i].turn == turn && rs_up[i].epoch == epoch) return i;
    return -1;
}

/* an upload from d for (turn, epoch) that was refused can never become ready, so nobody waits for it */
static int rs_up_refused(DPID d, DWORD turn, DWORD epoch)
{
    int i;
    for (i = 0; i < RS_NPEER; i++)
        if (rs_up[i].dpid == d && rs_up[i].bad && rs_up[i].turn == turn && rs_up[i].epoch == epoch) return 1;
    return 0;
}

static int rs_gave_up(DPID d, DWORD epoch)
{
    int i;
    for (i = 0; i < RS_NPEER; i++) if (rs_giveup[i].dpid == d && rs_giveup[i].epoch == epoch) return 1;
    return 0;
}

static void rs_path(char *out, const char *name)
{
    const char *gd = (const char *)0x58fe30;   /* the game's own directory string (FUN_004af502 chdirs there before a load) */
    int n = 0;
    if (g_exe_ok) while (n < 200 && gd[n]) n++;
    if (n > 0 && n < 200) {
        memcpy(out, gd, n);
        if (out[n - 1] != '\\' && out[n - 1] != '/') out[n++] = '\\';
        out[n] = 0;
    } else lstrcpynA(out, exedir, MAX_PATH);
    lstrcatA(out, name);
}

static int rs_write(const char *name, const BYTE *b, DWORD n)
{
    char p[MAX_PATH + 32], q[MAX_PATH + 40];
    FILE *f;
    int ok;
    rs_path(p, name);
    lstrcpyA(q, p); lstrcatA(q, ".rs.tmp");
    f = fopen(q, "wb");
    if (!f) return 0;
    ok = fwrite(b, 1, n, f) == n;
    if (fclose(f)) ok = 0;
    if (ok) ok = MoveFileExA(q, p, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileA(q);
    return ok;
}

static void rs_epoch_bump(void) { EnterCriticalSection(&cs); sy_epoch++; LeaveCriticalSection(&cs); }

/* cs held: take record [off, off+len) from upload slot s into the file, shared bytes only */
static void rs_take(BYTE *file, int s, DWORD off, DWORD len)
{
    DWORD k;
    for (k = off; k < off + len; k++) if (rs_up[s].have[k] && rs_mask[k] == 0) file[k] = rs_up[s].img[k];
}

static void rs_add(char *list, int cap, const char *t)
{
    int n = (int)strlen(list);
    if (n + (int)strlen(t) + 3 < cap) { if (n) lstrcatA(list, ", "); lstrcatA(list, t); }
    else if (n + 6 < cap && !strstr(list, "...")) lstrcatA(list, ", ...");
}

/* Host, game thread, cs NOT held, from dp2_Send before the message is queued: msg 0x16 (end-of-turn verdict,
 * size 5, buf[4] = 1 ok / 0 rollback, buf[2] = its checksum = buf[4]). Copy 0 decides; the 0x40/0x80 copies get the
 * same byte (the game sends its own buffer 3 times; we patch our copy of each). */
static void rs_host_send(BYTE *buf, DWORD size)
{
    DWORD turn, ep, t0 = GetTickCount(), k, fnvh, verdict;
    int bad, i, nj = 0, missing = 0;
    BYTE *file = NULL;
    char from_peers[400], miss[160], why[160];
    if (!G.host || size != 5 || buf[0] != 0x16 || !g_exe_ok) return;
    if (buf[3] & 0xC0) { if (rs_patch >= 0) { buf[4] = (BYTE)rs_patch; buf[2] = (BYTE)rs_patch; } return; }
    rs_patch = -1;
    turn = sy_end_turn;   /* the turn the verdict is about = our last turn-end record (game turn now: logged below) */
    EnterCriticalSection(&cs);
    ep = sy_epoch;
    bad = rs_nflag > 0 && rs_flag_turn == turn && rs_flag_epoch == ep;
    LeaveCriticalSection(&cs);
    if (!bad) {
        if (buf[4] == 0) { SLOG("RESYNC not needed: our turn-end check is clean; the game's own check asked for a rollback (stock, turn %lu epoch %lu)", turn, ep); rs_epoch_bump(); }
        return;
    }
    why[0] = 0;
    if (rs_try_turn != turn) { rs_try_turn = turn; rs_tries = 0; }
    if (!rs_enabled()) lstrcpyA(why, "resync=0 on this host");
    else if (!rs_validate()) snprintf(why, sizeof why, "regions unknown for this exe: %s", rs_why);
    else if (rs_tries >= RS_TRIES) snprintf(why, sizeof why, "already %d repairs at turn %lu", RS_TRIES, turn);
    else if (!rs_snap_ok || rs_snap_turn != turn || rs_snap_epoch != ep) snprintf(why, sizeof why, "no turn-end copy for turn %lu epoch %lu", turn, ep);
    else if (!(file = (BYTE *)malloc(RS_FILE))) lstrcpyA(why, "no memory");
    if (!why[0]) {   /* castles.dat: the host's, whole */
        char p[MAX_PATH + 32];
        FILE *f;
        rs_path(p, "castles.dat");
        f = fopen(p, "rb");
        if (!f || fread(file + RS_IMG, 1, RS_CAST, f) != RS_CAST || fgetc(f) != EOF) snprintf(why, sizeof why, "castles.dat is not %u bytes", RS_CAST);
        if (f) fclose(f);
    }
    if (why[0]) {
        SLOG("RESYNC refused turn=%lu epoch=%lu: %s - %s", turn, ep, why,
             buf[4] ? "verdict stays ok, the turn stays SPLIT (the next turn end checks again)" : "the game's own rollback goes ahead (each peer loads its own file)");
        free(file);
        if (buf[4] == 0) rs_epoch_bump();
        return;
    }
    /* wait for the flagged peers' uploads (they are sent the moment each peer sees the DESYNC). Review F2: a peer that
     * timed out once in this epoch is never waited on again in it; a refused upload is not waited on at all */
    for (t0 = GetTickCount();;) {
        int all = 1;
        EnterCriticalSection(&cs);
        for (i = 0; i < rs_nflag; i++)
            if (pl_find(rs_flag[i]) && rs_needs_up(rs_flag[i], turn, ep) && !rs_gave_up(rs_flag[i], ep) && !rs_up_refused(rs_flag[i], turn, ep) && rs_up_ready(rs_flag[i], turn, ep) < 0) all = 0;
        LeaveCriticalSection(&cs);
        if (all || GetTickCount() - t0 >= RS_WAIT_MS) break;
        Sleep(20);
    }
    EnterCriticalSection(&cs);
    for (i = 0; i < rs_nflag; i++)
        if (pl_find(rs_flag[i]) && rs_needs_up(rs_flag[i], turn, ep) && !rs_gave_up(rs_flag[i], ep) && !rs_up_refused(rs_flag[i], turn, ep) && rs_up_ready(rs_flag[i], turn, ep) < 0) {
            int g;
            for (g = 0; g < RS_NPEER - 1 && rs_giveup[g].dpid && rs_giveup[g].epoch == ep; g++) ;   /* first free or old-epoch slot, else the last */
            rs_giveup[g].dpid = rs_flag[i]; rs_giveup[g].epoch = ep;
            SLOG("RESYNC: player %08lx sent no usable upload for turn %lu within %lu ms - not waited on again in epoch %lu", rs_flag[i], turn, GetTickCount() - t0, ep);
        }
    LeaveCriticalSection(&cs);
    from_peers[0] = miss[0] = 0;
    EnterCriticalSection(&cs);
    memcpy(file, rs_snap, RS_IMG);
    for (i = 0; i < rs_nflag; i++) if (pl_find(rs_flag[i]) && rs_needs_up(rs_flag[i], turn, ep) && rs_up_ready(rs_flag[i], turn, ep) < 0) {
        char t[40];
        int k2, bad = 0;
        for (k2 = 0; k2 < RS_NPEER; k2++) if (rs_up[k2].dpid == rs_flag[i] && rs_up[k2].bad) bad = 1;
        snprintf(t, sizeof t, "%08lx (%s)", rs_flag[i], bad ? "upload refused" : "no upload in time");
        rs_add(miss, sizeof miss, t);
        missing++;
    }
    {   /* every object: battle owner, else owner, else host. Only flagged peers' copies can differ from ours. */
        int kind;
        for (kind = 0; kind < 3; kind++) {
            DWORD n = kind == 0 ? 16 : kind == 1 ? 150 : 5, id;
            for (id = 1; id <= n; id++) {
                DWORD off = kind == 0 ? rs_county + id * 0x300 : kind == 1 ? rs_army + id * 0x1a4 : rs_noble + id * 0x160;
                DWORD len = kind == 0 ? 0x300 : kind == 1 ? 0x1a4 : 0x160;
                DWORD bl = rs_battle_auth(kind, id, turn, ep);
                DWORD lord = bl ? bl : !rs_owner_mode() ? 0 : kind == 0 ? rs_snap[off + 5] : kind == 1 ? rs_snap[off] : id;   /* 0: the host's copy */
                DPID d = rs_dpid_of(lord);
                int s, f;
                char t[64];
                if (!d || d == G.myid) continue;
                for (f = 0; f < rs_nflag; f++) if (rs_flag[f] == d) break;
                if (f == rs_nflag) continue;                       /* in step with us: our copy is theirs */
                if ((s = rs_up_ready(d, turn, ep)) < 0) continue;  /* no upload: our copy stays (listed in miss) */
                if (memcmp(rs_up[s].have + off, "\1\1\1\1", 4) || !rs_up[s].have[off + len - 1]) {
                    snprintf(t, sizeof t, "%s %lu: lord %lu sent no copy", kind == 0 ? "county" : kind == 1 ? "army" : "noble", id, lord);
                    rs_add(miss, sizeof miss, t);
                    continue;
                }
                if (memcmp(file + off, rs_up[s].img + off, len)) {
                    rs_take(file, s, off, len);
                    snprintf(t, sizeof t, "%s %lu<-lord %lu%s", kind == 0 ? "county" : kind == 1 ? "army" : "noble", id, lord, bl ? " (battle owner)" : "");
                    rs_add(from_peers, sizeof from_peers, t);
                }
            }
        }
    }
    LeaveCriticalSection(&cs);
    if (missing || miss[0] || !rs_check_img(file, why, sizeof why)) {   /* never a partial repair */
        if (!missing && !miss[0]) { char t[200]; snprintf(t, sizeof t, "the composed image fails its bounds check: %s", why); lstrcpynA(miss, t, sizeof miss); }
        SLOG("RESYNC refused turn=%lu epoch=%lu: an authority copy is missing or bad: %s - nothing applied; %s", turn, ep, miss,
             buf[4] ? "verdict stays ok, the desync stays reported (the next turn end checks again)" : "the game's own rollback goes ahead (each peer loads its own file)");
        free(file);
        if (buf[4] == 0) rs_epoch_bump();
        return;
    }
    fnvh = rs_fnv(file, RS_FILE, 0x811c9dc5u);
    if (!rs_write("lastturn.sva", file, RS_FILE)) {
        SLOG("RESYNC refused turn=%lu epoch=%lu: lastturn.sva could not be written - %s", turn, ep,
             buf[4] ? "verdict stays ok, the turn stays SPLIT" : "the game's own rollback goes ahead");
        free(file);
        if (buf[4] == 0) rs_epoch_bump();
        return;
    }
    EnterCriticalSection(&cs);
    for (i = 0; i < MAXCONN; i++) {
        DWORD h[6];
        Out *o = G.conns[i].o;
        if (!G.conns[i].used || !G.conns[i].haveplayer) continue;
        h[0] = RS_DN_BEGIN; h[1] = RS_VER; h[2] = RS_FILE; h[3] = fnvh; h[4] = turn; h[5] = ep;
        osend(o, T_RESYNC, h, 24);
        for (k = 0; k < RS_FILE; k += RS_CHUNK) {
            DWORD len = RS_FILE - k < RS_CHUNK ? RS_FILE - k : RS_CHUNK;
            BYTE *fr = (BYTE *)malloc(8 + len);
            if (!fr) break;
            ((DWORD *)fr)[0] = RS_DN_DATA; ((DWORD *)fr)[1] = k;
            memcpy(fr + 8, file + k, len);
            osend(o, T_RESYNC, fr, 8 + len);
            free(fr);
        }
        h[0] = RS_DN_END; h[1] = turn; h[2] = ep;
        osend(o, T_RESYNC, h, 12);
        nj++;
    }
    rs_tries++;
    sy_epoch++;
    rs_verify = 1; rs_verify_turn = turn; rs_verify_epoch = sy_epoch;
    rs_nflag = 0;
    LeaveCriticalSection(&cs);
    free(file);
    /* the game's own rollback, from the lastturn file (counter 0 = lastturn, not old_turn) */
    verdict = buf[4];
    *(volatile BYTE *)0x59151c = 0;
    *(volatile DWORD *)0x553f7c = 0;
    buf[4] = 0; buf[2] = 0;
    rs_patch = 0;
    SLOG("RESYNC start turn=%lu (game turn now %lu) epoch=%lu try %d/%d: game verdict was %s, sent as rollback; %lu bytes (fnv %08lx) to %d joiner(s), waited %lu ms",
         turn, RD32(0x553240), ep, rs_tries, RS_TRIES, verdict ? "ok" : "rollback", RS_FILE, fnvh, nj, GetTickCount() - t0);
    SLOG("RESYNC authority: taken from peers: %s; everything else from the host (its own objects, AI/neutral objects, RNG, season, turn, all other shared bytes, castles.dat)",
         from_peers[0] ? from_peers : "nothing (the peers' own records already matched)");
    SLOG("RESYNC repairs: every shared byte of the 225 save-table entries and castles.dat, loaded by the game's own rollback on every peer. "
         "NOT repaired: local-only bytes (camera, selection, fog bits, advisor slots, battle leftovers) - kept per peer by design");
}

/* Joiner reader thread, cs NOT held: the host's repaired file, in order. On END: hash check, local bytes merged from
 * our own turn-end copy, written as lastturn.svb BEFORE this thread queues the 0x16 that follows on the same link. */
static void rs_on_frame(const BYTE *f, DWORD n, int gen)
{
    DWORD sub = n >= 4 ? ((DWORD *)f)[0] : 0;
    if (G.gen != gen || G.host) return;
    if (sub == RS_DN_BEGIN && n >= 24) {
        rs_dn.on = 0;
        if (((DWORD *)f)[1] != RS_VER || ((DWORD *)f)[2] != RS_FILE) {
            SLOG("RESYNC refused: host sent version %lu size %lu, mine %d/%u - this peer keeps its own file", ((DWORD *)f)[1], ((DWORD *)f)[2], RS_VER, RS_FILE);
            return;
        }
        {   /* review F3: only a file for a turn end this peer has: the desync WE detected, or (review tb2 F6, 3+ players)
             * our own last turn-end copy, so a peer that did not see the split still loads the host's file with the
             * others instead of its own old state. The file comes only from the host link: the host is the authority. */
            DWORD et, ee, st, se;
            int snap;
            EnterCriticalSection(&cs);
            et = rs_exp_turn; ee = rs_exp_epoch; st = rs_snap_turn; se = rs_snap_epoch; snap = rs_snap_ok;
            LeaveCriticalSection(&cs);
            if ((((DWORD *)f)[4] != et || ((DWORD *)f)[5] != ee) && !(snap && ((DWORD *)f)[4] == st && ((DWORD *)f)[5] == se)) {
                SLOG("RESYNC refused unsolicited download: host sent turn %lu epoch %lu, this peer detected a desync for turn %lu epoch %lu and its last turn end is turn %lu epoch %lu - dropped",
                     ((DWORD *)f)[4], ((DWORD *)f)[5], et, ee, st, se);
                return;
            }
            if (((DWORD *)f)[4] != et || ((DWORD *)f)[5] != ee)
                SLOG("RESYNC: this peer saw no split at turn %lu epoch %lu, taking the host's repair with the others", ((DWORD *)f)[4], ((DWORD *)f)[5]);
        }
        if (!rs_dn.buf && !(rs_dn.buf = (BYTE *)malloc(RS_FILE))) { SLOG("RESYNC refused: no memory for the file"); return; }
        rs_dn.total = RS_FILE; rs_dn.got = 0; rs_dn.fnv = ((DWORD *)f)[3]; rs_dn.turn = ((DWORD *)f)[4]; rs_dn.epoch = ((DWORD *)f)[5];
        rs_dn.on = 1;
        InterlockedExchange(&rs_ready, 0);
    } else if (sub == RS_DN_DATA && rs_dn.on && n >= 8) {
        DWORD off = ((DWORD *)f)[1], len = n - 8;
        if (off != rs_dn.got || len > rs_dn.total - off) { SLOG("RESYNC: chunk at %lu (have %lu, len %lu) out of order - dropped", off, rs_dn.got, len); rs_dn.on = 0; return; }
        memcpy(rs_dn.buf + off, f + 8, len);
        rs_dn.got += len;
    } else if (sub == RS_DN_END && rs_dn.on && n >= 12) {
        DWORD h, k, own = 0;
        int ok;
        rs_dn.on = 0;
        if (rs_dn.got != rs_dn.total || (h = rs_fnv(rs_dn.buf, rs_dn.total, 0x811c9dc5u)) != rs_dn.fnv) {
            SLOG("RESYNC refused: file incomplete or hash differs (%lu/%lu bytes) - this peer keeps its own file", rs_dn.got, rs_dn.total);
            return;
        }
        EnterCriticalSection(&cs);
        ok = rs_validate() && rs_snap_ok && rs_snap_turn == rs_dn.turn && rs_snap_epoch == rs_dn.epoch;
        if (ok) for (k = 0; k < RS_IMG; k++) {
            BYTE m = rs_mask[k];
            if (m == 1 || m == 3) { if (rs_dn.buf[k] != rs_snap[k]) own++; rs_dn.buf[k] = rs_snap[k]; }
            else if (m == 2) rs_dn.buf[k] = (BYTE)((rs_dn.buf[k] & ~0x61) | (rs_snap[k] & 0x61));
        }
        LeaveCriticalSection(&cs);
        if (!ok) { SLOG("RESYNC refused: no own turn-end copy for turn %lu epoch %lu - this peer keeps its own file", rs_dn.turn, rs_dn.epoch); return; }
        {
            char why[96];
            if (!rs_check_img(rs_dn.buf, why, sizeof why)) { SLOG("RESYNC refused: the host's file fails the bounds check (%s) - not written, this peer keeps its own file", why); return; }
        }
        if (!rs_write("lastturn.svb", rs_dn.buf, RS_FILE)) { SLOG("RESYNC refused: lastturn.svb could not be written - this peer keeps its own file"); return; }
        EnterCriticalSection(&cs);
        rs_ready_turn = rs_dn.turn; rs_ready_epoch = rs_dn.epoch;
        InterlockedExchange(&rs_ready, 1);
        LeaveCriticalSection(&cs);
        SLOG("RESYNC received turn=%lu epoch=%lu: %lu bytes, hash ok, %lu local bytes kept as ours, written to lastturn.svb", rs_dn.turn, rs_dn.epoch, rs_dn.total, own);
    }
}

/* Joiner, game thread, cs held, from dp2_Receive: the game is handed msg 0x16 copy 0 */
static void rs_on_deliver(const BYTE *d, DWORD len)
{
    DWORD turn;
    if (G.host || len != 5 || d[0] != 0x16 || (d[3] & 0xC0) || !g_exe_ok) return;
    if (d[4] != 0) return;   /* verdict ok: nothing rolls back */
    turn = sy_end_turn;
    if (rs_ready && rs_ready_turn == turn && rs_ready_epoch == sy_epoch) {
        *(volatile DWORD *)0x553f7c = 0;   /* load lastturn.svb (the repaired file), not old_turn */
        InterlockedExchange(&rs_ready, 0);
        rs_verify = 1; rs_verify_turn = turn; rs_verify_epoch = sy_epoch + 1;
        SLOG("RESYNC applied turn=%lu epoch=%lu: the game now loads the host-built lastturn.svb through its own rollback", turn, sy_epoch);
    } else SLOG("RESYNC: rollback at turn %lu with no repaired file (stock: this peer loads its own %s)", turn,
                RD32(0x553f7c) ? "old_turn.svb" : "lastturn.svb");
    sy_epoch++;
}

/* cs held, from sync.c: the first turn-end compare after a repair */
static void rs_check_verify(DWORD turn, DWORD epoch, DWORD peer, int nd, const char *list)
{
    if (!rs_verify || epoch != rs_verify_epoch) return;
    rs_verify = 0;
    if (!nd) SLOG("RESYNC verified turn=%lu epoch=%lu with player %lu: all checked regions equal", turn, epoch, peer & 0xff);
    else SLOG("RESYNC FAILED turn=%lu epoch=%lu with player %lu: %d regions still differ: %s", turn, epoch, peer & 0xff, nd, list);
}

/* cs held: a session starts clean */
static void rs_reset(void)
{
    int i;
    rs_snap_ok = 0; rs_nflag = 0; rs_nbat = 0; rs_tries = 0; rs_try_turn = 0xffffffffu; rs_patch = -1; rs_verify = 0;
    rs_up_turn = 0xffffffffu; rs_exp_turn = 0xffffffffu; rs_exp_epoch = 0;
    memset(rs_giveup, 0, sizeof rs_giveup);
    memset(rs_peer, 0, sizeof rs_peer);
    for (i = 0; i < RS_NPEER; i++) { rs_up[i].dpid = 0; rs_up[i].begun = rs_up[i].done = 0; }
    rs_dn.on = 0;
    InterlockedExchange(&rs_ready, 0);
}

/* ---- test switch: LOTR2NET_TEST_CORRUPT="turn,va,mode" (or test_corrupt= in lords2hd.ini). Game thread, from
 * sync_on_send. Applied once, at turn >= N in the campaign (tick >= 50):
 *   mode 1: xor 0x01 into the byte at va;
 *   mode 2: +1 at va and -1 at va+1 (an 8-bit-sum-neutral change the game's own check cannot see);
 *   mode 4: (va ignored) after the first battle at turn >= N that THIS peer does not own, add 7 to the troop total
 *           (+0x168) of that battle's army B (A if B's slot is free) - a wrong battle result on the loser's copy. */
static int rs_tc_mode = -1, rs_tc_done;
static DWORD rs_tc_turn, rs_tc_va, rs_tc_ba, rs_tc_bb, rs_tc_owner;
/* Joiner test modes (va ignored), game thread, cs not held. Each first makes a REAL split on this peer (so a resync is
 * needed): modes 5/6 +7 bushels in the grain store (+0x224, county-food-economy.md:1068) of a county this lord owns
 * (a store carries over, so the split survives to the turn end; the first rig try used army troops +0x168 and a
 * battle that turn rewrote them, so no desync happened); mode 7 the same grain split (so a repair runs even when a
 * real battle later rewrites the next field) AND +7 troops (+0x168) on the foreign army. Then:
 *   5: the next upload carries one army record with owner byte 0xee (out of range) -> the host must refuse the whole
 *      upload and apply nothing;
 *   6: a forged sync record and a forged upload that claim another lord -> the host must drop both;
 *   7: a forged BATTLE record (an army of another lord vs one of ours, "owned" by us) plus that foreign army in our
 *      upload -> the host must grant no authority: the foreign army comes from its owner (the host's copy).
 *   8: (no split) a held heartbeat slot, review6 R6-1: see rs_test_hold. */
static void rs_test_forge(DWORD turn, DWORD tick)
{
    DWORD me = rs_lord(), i, own = 0, foreign = 0, cty = 0, other = me % 5 + 1, va, old;
    for (i = 1; i <= 150; i++) {
        DWORD o = RD8(VA_ARMY + i * 0x1a4);
        if (RD8(VA_ARMY + i * 0x1a4 + 8) != 1) continue;
        if (o == me && !own) own = i;
        if (o >= 1 && o <= 5 && o != me) foreign = i;   /* the LAST one: the first is the likeliest to be marching */
    }
    for (i = 1; i <= 16 && !cty; i++) if (RD8(VA_COUNTY + i * 0x300 + 5) == me) cty = i;
    if (G.host || !own || !cty) { SLOG("TEST CORRUPT mode %d: needs a joiner with an army and a county (host %d, own army %lu, county %lu) - skipped", rs_tc_mode, G.host, own, cty); return; }
    if (rs_tc_mode == 7 && !foreign) { SLOG("TEST CORRUPT mode 7: no army of another lord - skipped"); return; }
    va = VA_COUNTY + cty * 0x300 + 0x224;
    old = RD32(va);
    *(volatile DWORD *)va = old + 7;
    SLOG("TEST CORRUPT mode %d turn=%lu tick=%lu: county %lu (lord %lu) +0x224 grain %lu -> %lu on this peer", rs_tc_mode, turn, tick, cty, me, old, old + 7);
    if (rs_tc_mode == 7) {
        va = VA_ARMY + foreign * 0x1a4 + 0x168;
        old = RD32(va);
        *(volatile DWORD *)va = old + 7;
        SLOG("TEST CORRUPT mode 7 turn=%lu tick=%lu: army %lu (lord %u) +0x168 troops %lu -> %lu on this peer", turn, tick, foreign,
             RD8(VA_ARMY + foreign * 0x1a4), old, old + 7);
    }
    EnterCriticalSection(&cs);
    if (rs_tc_mode == 5) rs_tc_poison = 1;
    else if (rs_tc_mode == 6) {
        BYTE b[SY_HDR + 38 * 4];
        DWORD *d = (DWORD *)b, h[5];
        BYTE fr[8 + 0x1a4];
        memset(b, 0, sizeof b);
        d[0] = SY_TURN; d[1] = turn; d[2] = 1; d[5] = other; d[6] = 37; d[7] = sy_epoch;
        jsend(T_SYNC, b, SY_HDR + 37 * 4);
        h[0] = RS_UP_BEGIN; h[1] = RS_VER; h[2] = turn; h[3] = sy_epoch; h[4] = other;
        jsend(T_RESYNC, h, 20);
        ((DWORD *)fr)[0] = RS_UP_DATA; ((DWORD *)fr)[1] = rs_army + own * 0x1a4;
        memcpy(fr + 8, (void *)(VA_ARMY + own * 0x1a4), 0x1a4);
        jsend(T_RESYNC, fr, sizeof fr);
        h[0] = RS_UP_END; h[1] = turn; h[2] = sy_epoch; h[3] = 1;
        jsend(T_RESYNC, h, 16);
        SLOG("TEST CORRUPT mode 6: sent a sync record and an upload claiming lord %lu (I am lord %lu)", other, me);
    } else {
        BYTE b[SY_HDR + 18 * 4];
        DWORD *d = (DWORD *)b;
        int k;
        memset(b, 0, sizeof b);
        d[0] = SY_BATTLE; d[1] = turn; d[2] = tick; d[3] = foreign; d[4] = own; d[5] = me; d[6] = 18; d[7] = sy_epoch;
        jsend(T_SYNC, b, SY_HDR + 18 * 4);
        if (rs_nbat && (rs_bat[0].turn != turn || rs_bat[0].epoch != sy_epoch)) rs_nbat = 0;
        if (rs_nbat < RS_NBAT) {   /* our own (forged) belief: we own that battle, so our upload carries the foreign army */
            k = rs_nbat++;
            memset(&rs_bat[k], 0, sizeof rs_bat[k]);
            rs_bat[k].turn = turn; rs_bat[k].epoch = sy_epoch; rs_bat[k].ba = foreign; rs_bat[k].bb = own; rs_bat[k].owner = me;
            rs_bat[k].la = RD8(VA_ARMY + foreign * 0x1a4); rs_bat[k].lb = me;
        }
        SLOG("TEST CORRUPT mode 7: sent a forged battle record, armies %lu (lord %u) vs %lu (mine), claiming lord %lu owns it", foreign,
             RD8(VA_ARMY + foreign * 0x1a4), own, me);
    }
    LeaveCriticalSection(&cs);
}

static void rs_test_corrupt(const BYTE *buf, DWORD ov, DWORD tick)
{
    DWORD turn;
    if (rs_tc_mode < 0) {
        char v[64], *e = getenv("LOTR2NET_TEST_CORRUPT");
        rs_tc_mode = 0;
        if ((e && *e && lstrcpynA(v, e, sizeof v)) || ini_get("test_corrupt", v, sizeof v)) {
            unsigned long t = 0, va = 0, m = 0;
            if (sscanf(v, "%lu,%lx,%lu", &t, &va, &m) == 3 && (m == 1 || m == 2 || (m >= 4 && m <= 8))) {
                rs_tc_turn = t; rs_tc_va = va; rs_tc_mode = (int)m;
                SLOG("TEST CORRUPT armed: turn>=%lu va=%08lx mode %lu", t, va, m);
            } else SLOG("TEST CORRUPT: '%s' not understood (turn,va-hex,mode 1/2/4/5/6/7/8) - off", v);
        }
    }
    if (!rs_tc_mode || rs_tc_mode == 8 || rs_tc_done || !g_exe_ok) return;
    turn = RD32(0x553240);
    if (turn < rs_tc_turn) return;
    if (rs_tc_mode == 4) {
        if (ov == 2 && buf[0] == 0x0c && !rs_tc_ba) {
            DWORD a = RD32(0x568210), b = RD32(0x568218), la, lb;
            if (a < 1 || a > 150 || b < 1 || b > 150) return;
            la = RD8(VA_ARMY + a * 0x1a4); lb = RD8(VA_ARMY + b * 0x1a4);
            rs_tc_owner = RD8(VA_ARMY + a * 0x1a4 + 1) ? la : RD8(VA_ARMY + b * 0x1a4 + 1) ? lb : 0;
            if (!rs_tc_owner || rs_tc_owner == rs_lord()) return;   /* AI-only, or we own it: wait for one a peer owns */
            rs_tc_ba = a; rs_tc_bb = b;
            SLOG("TEST CORRUPT: battle %lu vs %lu owned by lord %lu (me lord %lu, 0x5533d0=%lu) - corrupting after it ends", a, b, rs_tc_owner, rs_lord(), RD32(0x5533d0));
        } else if (ov == 0 && rs_tc_ba && tick >= 20) {
            DWORD slot = RD8(VA_ARMY + rs_tc_bb * 0x1a4) ? rs_tc_bb : rs_tc_ba, va = VA_ARMY + slot * 0x1a4 + 0x168, old = RD32(va);
            *(volatile DWORD *)va = old + 7;
            rs_tc_done = 1;
            SLOG("TEST CORRUPT applied turn=%lu tick=%lu: army %lu +0x168 %lu -> %lu (loser's copy; battle owner lord %lu)", turn, tick, slot, old, old + 7, rs_tc_owner);
        }
        return;
    }
    if (ov != 0 || tick < 50) return;
    if (rs_tc_mode >= 5) { rs_test_forge(turn, tick); rs_tc_done = 1; return; }
    if (rs_tc_mode == 1) *(volatile BYTE *)rs_tc_va ^= 0x01;
    else { (*(volatile BYTE *)rs_tc_va)++; (*(volatile BYTE *)(rs_tc_va + 1))--; }
    rs_tc_done = 1;
    SLOG("TEST CORRUPT applied turn=%lu tick=%lu va=%08lx mode %d", turn, tick, rs_tc_va, rs_tc_mode);
}

/* mode 8 (joiner, review6 R6-1), game thread from dp2_Send: 2 s after this peer's first SimTick reset at turn >= N
 * (the host has reset by then too: joiners reset first, rig soak1), one 0x0a for our lord on the OLD clock (last + 200,
 * inside what the host accepts around its own reset), then none of our own 0x0a for 16 s. The host's slot for our lord stays far past its SimTick after its old-clock window; its watchdog must
 * rewrite it (trust=0: it only logs). The seq is 20 past the game's next (FUN_0043eda0 1..39), so none of ours is lost
 * as a duplicate. 1 = do not send this message. */
static DWORD rs_th_last, rs_th_until, rs_th_at, rs_th_v;
static int rs_test_hold(DPID from, const BYTE *buf, DWORD size)
{
    DWORD tick, now = GetTickCount();
    if (rs_tc_mode != 8 || G.host || !g_exe_ok || size < 8) return 0;
    if (rs_th_until) {
        if ((long)(now - rs_th_until) < 0) {
            if (buf[0] == 0x0a) return 1;
            if (buf[0] == 0x08 || buf[0] == 0x0c) SLOG("TEST HOLD: our 0x%02x went out while holding (it rewrites our slot)", buf[0]);
            return 0;
        }
        SLOG("TEST HOLD: 16 s over, our heartbeats go out again");
        rs_th_until = 0; rs_tc_done = 1;
        return 0;
    }
    if (rs_tc_done || buf[0] != 0x0a || (buf[3] & 0xC0)) return 0;
    tick = buf[4] | buf[5] << 8 | buf[6] << 16;
    if (!rs_th_at && RD32(0x553240) >= rs_tc_turn && rs_th_last && tick + 50 < rs_th_last) {
        rs_th_at = (now + 2000) | 1; rs_th_v = rs_th_last + 200;
        SLOG("TEST HOLD mode 8: our SimTick reset %lu -> %lu; the old-clock heartbeat goes out in 2 s", rs_th_last, tick);
    }
    if (rs_th_at && (long)(now - rs_th_at) >= 0) {
        BYTE f[16], *m = f + 8;
        DWORD v = rs_th_v, me = rs_lord();
        *(DWORD *)f = from; *(DWORD *)(f + 4) = 0;
        m[0] = 0x0a; m[1] = 4; m[3] = (BYTE)((RD32(0x512f4c) + 19) % 39 + 1);
        m[4] = (BYTE)v; m[5] = (BYTE)(v >> 8); m[6] = (BYTE)(v >> 16); m[7] = (BYTE)me;
        m[2] = (BYTE)(m[4] + m[5] + m[6] + m[7]);
        EnterCriticalSection(&cs);
        jsend(T_DATA, f, sizeof f);
        LeaveCriticalSection(&cs);
        rs_th_until = (now + 16000) | 1;
        SLOG("TEST HOLD mode 8 turn=%lu: SimTick %lu; sent heartbeat %lu for lord %lu on the old clock, holding our 0x0a 16 s",
             RD32(0x553240), tick, v, me);
        return 1;
    }
    rs_th_last = tick;
    return 0;
}
