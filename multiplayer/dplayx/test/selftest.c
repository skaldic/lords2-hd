/* selftest.c - drive dplayx.dll the way Lords2.exe does, without the game. Our own code.
 *
 *   selftest.exe PROTO      (PROTO the host will expect: DLL_VERSION*1000 since v7, exe_ok 0 here -> 9000)
 *
 * Runs beside dplayx.dll (Windows or Wine), host side on 127.0.0.1:47700 (LOTR2NET_BIND: no firewall prompt):
 *   T1 Open(create)                                     -> DP_OK
 *   T2 raw ENUM, right PROTO                            -> INFO
 *   T3 raw ENUM, PROTO-1 (another build)                -> INFO, name marked (v4+) / REJECT (v3)
 *   T4 raw JOIN, PROTO-1                                -> REJECT
 *   T5 raw JOIN, right PROTO, stays connected           -> INFO
 *   T6 Close, then Open(create) again within 1 s        -> DP_OK (host side now holds a TIME_WAIT conn)
 *   T7 GetCaps dwMaxBufferSize                          -> printed
 *   T8 EnumSessions with LOTR2NET_HOST=localhost        -> callback (name lookup path), time printed
 *   T9 a joiner that stops reading while a second one reads: no host Send blocks over 500 ms (v7 writer queues), the
 *      staller is dropped (DESTROYPLAYER) once its queue passes OUT_CAP, and the live joiner keeps receiving after it
 *   T10 joiner side: a fake host floods QMAX+200 (16584) messages the game never reads -> SESSIONLOST, not silent drops
 *       (v9: only after the reader has waited LOTR2NET_QWAIT_MS for the game, set to 1500 here)
 *   T11 a joined conn that never creates a player is dropped after 10 s (v9; was 30 s); T12 Close returns within 3.5 s
 *   T13 local search (v7): a 7-byte L2FIND1 gets no answer; a 72-byte L2FIND2 from 127.0.0.1 gets a 72-byte L2HERE1
 *   T14 EnumSessions to a blackholed address (10.255.255.1) returns within 600 ms (the round trip runs off-thread)
 *   T15 (v7 review F1) a conn that claims a lord the game's lord table gives to another live player is refused, and is not
 *       bound by the refusal (the table is supplied by LOTR2NET_TEST_LORDTAB, honoured only without the known exe)
 *   T16 (review F2) a flood of T_RESYNC BEGIN frames from one conn is cut at the per-conn budget, the host stays up
 *   T17 (review F3) a joiner refuses a host's resync download it never asked for (no desync detected for that turn/epoch)
 *   T18 (v9 peer trust) a joiner bound to lord 2 by the host's 0x02 sends 7 forged/bad game messages: all dropped, the honest one after them arrives
 *   T19 (v9 F3) a 5th silent connection from one address is closed at once; the host still answers
 *   T20 (v9 F4) a joiner overfills the host queue while the game does not read: the conn waits, nothing is dropped
 *   T21 (review2-trust F1-F3) a joiner bound to lord 2 sends 8 diplomacy payloads the game never sends: each is dropped and logged, the honest 0x1e after them arrives
 *   T22 (review2-trust F5, review3 R3-1) the game's three copies of an answer to a real offer pass when they follow it back to back; the same copy sent later (a replay), a copy carrying another seq and an answer to an already answered offer are dropped
 *   T23 (review2-trust F4, review3 R3-2) 0x01 is refused for a player that already has a lord, and a second one within 4 s, and so are copies of a refused 0x01: 2 of 5 delivered for a fresh player, 0 for the bound one
 *   T24 (v9 F7) 5000 game messages from one conn in a single burst are cut at the per-conn budget, the host still answers
 *   T25 (review2-trust F6) a lord's 40-offer flood fills only its own quota: another lord's open offer survives and is still answered
 *   T26 (review3 R3-5) a game message whose length byte disagrees with its size is dropped; the honest one after it arrives
 *   T27 (review3 R3-3, RE tick findings) with the host's SimTick supplied (LOTR2NET_TEST_SIMTICK): a command scheduled past
 *       SimTick + lead + 600, a heartbeat 2000 ticks ahead, the 65th command not yet due, a 0x5a to every peer and the 5th
 *       directed 0x5a in a burst are dropped; 64 commands, an honest heartbeat and 4 0x5a pass
 *   T28 (soak 2026-10-07, review4 R4-1 R4-2 R4-4) SimTick resets: a heartbeat far behind the host's passes; after the
 *       host's reset a heartbeat / command on the old clock passes, a jump, a +300-per-message ramp and a command past the
 *       anchored window are dropped; 40 late commands hit the 32-token late cap, 24 of 27 commands at one tick pass
 *   T29 (review5 R5-3 R5-6, review6 R6-1 R6-2 R6-4) a heartbeat accepted in the reset window by a player who then goes
 *       silent: after the window the host rewrites that lord's slot (a synthetic 0x0a from that player reaches the host's
 *       game), so a command scheduled from the stale slot drops; four 0x5a to the honest player and the host's own 0x5a
 *       (plain, 0x40, 0x80: counted once) widen its bound by 10; 2 of 3 0x5b pass; a heartbeat 1000 ahead drops while
 *       the host's SimTick stands still; a seq past 40 drops (review7 R7-5)
 * Prints one line per test, PASS/FAIL; exit 0 all pass, 1 any fail. Read dplayx.log for the DLL's side.
 *   T30 (review8 R8-1 R8-3) the host gates nobody (LOTR2NET_TEST_NOGATE): a heartbeat may be at most 410 past the
 *       slowest OTHER player's; after the host's reset an old-clock heartbeat may be at most the pre-reset SimTick + 450
 *   T31 (review9 R9-1 R9-2 R9-5) the host gates nobody and one player resets first: the other, still on the old clock,
 *       is bounded by the first one's pre-reset heartbeat (passes within 410, drops past it); with LOTR2NET_TEST_MPFIX
 *       a heartbeat may be 100 ahead (101 drops) and one tick takes 6 commands from a player (the 7th drops)
 *   T32 (v10 ghost hub) the host's game destroys its player and closes a locked game with joiners A and B in it: both see
 *       the host player destroyed and T_HOST(A); A's host-only 0x16 to all and its message to B reach B (A plays the host);
 *       B's 0x16 is dropped (static trust checks stay on); the host's game cannot Open or Send meanwhile; A leaves -> B
 *       gets T_HOST(B); B leaves -> hub closed
 *   T34 (v10 R12-1) Open(create) on a locked host game WITHOUT a Close ends the old game (no ghost hub): the joiner's
 *       link closes and the new session's Send works
 *   T33 (v10 joiner side) a hub names another player, then this one twice: the game gets exactly one DPSYS_HOST (0x101)
 *   T35 (v12 diag, LOTR2NET_TEST_DIAG) the host pings a joiner and logs its answer as a rtt; the host answers a joiner's
 *       ping with its session clock (v13); a joiner's pong relayed to another loses any clock it claims; a ping to another joiner is relayed; one claiming another player's id is not answered; a host heartbeat
 *       writes no per-message line while another message still does
 *   T36 (v12 session folders, LOTR2NET_TEST_MPLOGS; last: it moves dplayx.log away from log_has) a host with no joiner
 *       makes no folder; the first joiner makes lords2hd-mp\<time>, the log moves there and goes on; the next session
 *       makes a second folder; LF_NetLogSession names it; with 12 older folders there the 10 newest are kept, ours
 *       included; a host alone gets a folder when LF_NetLogSession asks; after Close it names none; Close appends the
 *       session summary (v13) to session.txt
 *   T38 (v13 session clock, joiner side) a fake host answers the DLL's pings with its own clock: LF_NetClock has none
 *       before the first answer, then the host's time within 150 ms, and log lines are stamped [S ...]
 *   T39-T41 need a lords2-relay on 127.0.0.1:47710 (RELAY_ADDR=127.0.0.1:47710 LOBBY_ADDR=127.0.0.1:47711); without one
 *       they print SKIP
 *   T39 (v14 relay, host side; a local lords2-relay on 127.0.0.1:47710) a host with LOTR2NET_RELAY set lists its game
 *       (name, PROTO, 1 player); a raw joiner joins through the relay with the normal handshake; the list shows 2
 *       players; once the game locks the session the room leaves the list; Close removes the room
 *   T40 (v14 relay, joiner side) two raw hosts on the relay, one of an older PROTO: EnumSessions in list mode
 *       (LOTR2NET_TEST_RELAYLIST) reports both, the old one marked "[host v..., yours v...]", each with its room id in
 *       the guidInstance and its players; the first has a 53-byte name with Jen's Mod Menu marker, which arrives whole
 *   T41 (v14 relay, joiner side) Open(join) with a room taken from that list joins through the relay: a raw host on
 *       the relay pairs and does the handshake, the DLL gets its player; a missing room is DPERR_NOSESSIONS, a locked
 *       one DPERR_ACTIVEPLAYERS, an unreachable relay DPERR_NOCONNECTION
 *   T42 (v14, last) with a module named GameRanger.dll loaded (a copy of version.dll), a host with a relay set is not
 *       listed and EnumSessions shows no relay rooms: GameRanger keeps its own path
 *   T37 (v12 settings) a lotr2net.ini beside the DLL at load moves into lords2hd.ini [Multiplayer]: keys lords2hd.ini
 *       lacks are copied, its own (ghost=1) win, other sections stay, the old file becomes lotr2net.ini.old
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG HR;
typedef struct { void **vt; } Obj;
typedef struct {
    DWORD dwSize, dwFlags;
    GUID guidInstance, guidApplication;
    DWORD dwMaxPlayers, dwCurrentPlayers;
    char *lpszSessionName, *lpszPassword;
    DWORD r1, r2, u1, u2, u3, u4;
} DPSD2;
typedef struct { DWORD dwSize, dwFlags, dwMaxBufferSize, a, b, c, d, e, f, g; } CAPS;
typedef HR(WINAPI *CREATE)(GUID *, void **, void *);
typedef HR(WINAPI *QI)(Obj *, const GUID *, void **);
typedef HR(WINAPI *OPEN)(Obj *, DPSD2 *, DWORD);
typedef HR(WINAPI *CLOSE)(Obj *);
typedef HR(WINAPI *GETCAPS)(Obj *, CAPS *, DWORD);
typedef BOOL(WINAPI *ESCB)(const DPSD2 *, DWORD *, DWORD, void *);
typedef HR(WINAPI *ENUMS)(Obj *, DPSD2 *, DWORD, ESCB, void *, DWORD);
typedef struct { DWORD dwSize, dwFlags; char *s, *l; } NAME_;
typedef HR(WINAPI *CPL)(Obj *, DWORD *, NAME_ *, HANDLE, void *, DWORD, DWORD);
typedef HR(WINAPI *SEND)(Obj *, DWORD, DWORD, DWORD, BYTE *, DWORD);
typedef HR(WINAPI *RECV)(Obj *, DWORD *, DWORD *, DWORD, BYTE *, DWORD *);
typedef HR(WINAPI *DPL)(Obj *, DWORD);
typedef HR(WINAPI *SETSD)(Obj *, DPSD2 *, DWORD);

static const GUID IID_IUnknown_ = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static int fails;

static void result(const char *t, int ok, const char *detail)
{
    printf("%s %s %s\n", ok ? "PASS" : "FAIL", t, detail ? detail : "");
    fflush(stdout);
    if (!ok) fails++;
}

/* raw client: hello(proto, mode), returns frame type (0 none) and copies up to 92 payload bytes */
static int hello(DWORD proto, DWORD mode, BYTE *out, SOCKET *keep)
{
    struct sockaddr_in a;
    BYTE h[21], hdr[5];
    DWORD len, tmo = 3000;
    int r, got = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(47700);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return 0; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    *(DWORD *)h = 17; h[4] = 1;               /* T_HELLO */
    memcpy(h + 5, "LOTR2NET", 8);
    *(DWORD *)(h + 13) = proto;
    *(DWORD *)(h + 17) = mode;
    send(s, (char *)h, 21, 0);
    while (got < 5 && (r = recv(s, (char *)hdr + got, 5 - got, 0)) > 0) got += r;
    if (got < 5) { closesocket(s); return 0; }
    len = *(DWORD *)hdr - 1;
    got = 0;
    while ((DWORD)got < len && got < 92 && (r = recv(s, (char *)out + got, (int)(len < 92 ? len : 92) - got, 0)) > 0) got += r;
    while ((DWORD)got < len && got >= 92) {   /* the rest of the frame (v14 rules.c: INFO is 100 bytes): the conn stays in step */
        char tmp[64];
        if ((r = recv(s, tmp, (int)(len - got < sizeof tmp ? len - got : sizeof tmp), 0)) <= 0) break;
        got += r;
    }
    if (keep) *keep = s; else closesocket(s);
    return hdr[4];
}

/* one frame from a raw conn: returns its type (0 none), payload copied up to cap */
static int readframe(SOCKET s, BYTE *out, int cap)
{
    BYTE hdr[5];
    DWORD len;
    int r, got = 0;
    while (got < 5 && (r = recv(s, (char *)hdr + got, 5 - got, 0)) > 0) got += r;
    if (got < 5) return 0;
    len = *(DWORD *)hdr - 1;
    for (got = 0; (DWORD)got < len; ) {
        char tmp[512];
        r = recv(s, tmp, (int)(len - got < sizeof tmp ? len - got : sizeof tmp), 0);
        if (r <= 0) return 0;
        if (got < cap) memcpy(out + got, tmp, (int)(got + r <= cap ? r : cap - got));
        got += r;
    }
    return hdr[4];
}

/* ---- v14 relay tests: a raw client of the local lords2-relay (run_wine / bt.sh start one on 127.0.0.1:47710) */
#define RELAY_PORT 47710
static SOCKET rl_raw(BYTE role, const void *extra, int n)   /* connected, header and role part sent */
{
    struct sockaddr_in a;
    BYTE h[32];
    DWORD tmo = 4000;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(RELAY_PORT);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return INVALID_SOCKET; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    memcpy(h, "L2RL", 4); h[4] = 1; h[5] = role;
    if (n) memcpy(h + 6, extra, n);
    send(s, (char *)h, 6 + n, 0);
    return s;
}
static void rl_raw_frame(SOCKET s, BYTE type, const BYTE *p, int n)
{
    BYTE b[64];
    *(DWORD *)b = (DWORD)n + 1; b[4] = type;
    if (n) memcpy(b + 5, p, n);
    send(s, (char *)b, 5 + n, 0);
}
typedef struct { DWORD id, proto; int players, maxpl; char name[72]; } RawRoom;
static int rl_raw_list(RawRoom *r, int cap)   /* the relay's room list; -1 = no answer */
{
    BYTE b[2100];
    int n = -1, i, k = 1, ty;
    SOCKET s = rl_raw(4, NULL, 0);
    if (s == INVALID_SOCKET) return -1;
    rl_raw_frame(s, 8, NULL, 0);
    ty = readframe(s, b, sizeof b);
    if (ty == 9) {
        n = b[0];
        for (i = 0; i < n && i < cap; i++) {
            int ln;
            r[i].id = *(DWORD *)(b + k); r[i].proto = *(DWORD *)(b + k + 4);
            r[i].players = b[k + 8]; r[i].maxpl = b[k + 9];
            ln = b[k + 10];
            memcpy(r[i].name, b + k + 11, ln < 71 ? ln : 71); r[i].name[ln < 71 ? ln : 71] = 0;
            k += 11 + ln;
        }
        if (n > cap) n = cap;
    }
    closesocket(s);
    return n;
}
static int rl_raw_find(const char *name, RawRoom *out)   /* 1 = a room of that name is listed */
{
    RawRoom r[20];
    int n = rl_raw_list(r, 20), i;
    for (i = 0; i < n; i++) if (!strcmp(r[i].name, name)) { if (out) *out = r[i]; return 1; }
    return 0;
}
/* T_HELLO on an open conn s (a relayed one): the answer's type, its payload in out (up to 92 bytes) */
static int hello_on(SOCKET s, DWORD proto, DWORD mode, BYTE *out)
{
    BYTE h[21], hdr[5];
    DWORD len;
    int r, got = 0;
    *(DWORD *)h = 17; h[4] = 1;
    memcpy(h + 5, "LOTR2NET", 8);
    *(DWORD *)(h + 13) = proto;
    *(DWORD *)(h + 17) = mode;
    send(s, (char *)h, 21, 0);
    while (got < 5 && (r = recv(s, (char *)hdr + got, 5 - got, 0)) > 0) got += r;
    if (got < 5) return 0;
    len = *(DWORD *)hdr - 1;
    got = 0;
    while ((DWORD)got < len && got < 92 && (r = recv(s, (char *)out + got, (int)(len < 92 ? len : 92) - got, 0)) > 0) got += r;
    while ((DWORD)got < len && got >= 92) {   /* the rest of the frame (v14 rules.c: INFO is 100 bytes) */
        char tmp[64];
        if ((r = recv(s, tmp, (int)(len - got < sizeof tmp ? len - got : sizeof tmp), 0)) <= 0) break;
        got += r;
    }
    return hdr[4];
}

/* a raw host on the relay: control conn, HOST sent, HOSTED read; *room = its id. INVALID_SOCKET = failed */
static SOCKET rl_raw_host(DWORD proto, BYTE maxpl, const char *name, DWORD *room)
{
    BYTE b[96], r[64];
    int n = (int)strlen(name);
    SOCKET s = rl_raw(1, NULL, 0);
    if (s == INVALID_SOCKET) return s;
    *(DWORD *)b = proto; b[4] = maxpl; b[5] = (BYTE)n; memcpy(b + 6, name, n);
    rl_raw_frame(s, 1, b, 6 + n);
    if (readframe(s, r, sizeof r) != 2) { closesocket(s); return INVALID_SOCKET; }
    *room = *(DWORD *)r;
    return s;
}
/* T40: the sessions EnumSessions reported */
static int g40_n;
static char g40_name[8][96];
static GUID g40_guid[8];
static DWORD g40_cur[8], g40_max[8];
static BOOL WINAPI cb40(const DPSD2 *d, DWORD *to, DWORD f, void *ctx)
{
    if (g40_n < 8) {
        lstrcpynA(g40_name[g40_n], d->lpszSessionName ? d->lpszSessionName : "", 96);
        g40_guid[g40_n] = d->guidInstance; g40_cur[g40_n] = d->dwCurrentPlayers; g40_max[g40_n] = d->dwMaxPlayers;
        g40_n++;
    }
    return TRUE;
}

/* T41: a raw host on the relay that takes one joiner: JOIN -> its data conn -> the host's half of the handshake */
static volatile LONG g41_paired;
static int recv_n(SOCKET s, BYTE *b, int n) { int got = 0, r; while (got < n && (r = recv(s, (char *)b + got, n - got, 0)) > 0) got += r; return got == n; }
static DWORD WINAPI raw_host41(LPVOID p)
{
    SOCKET c = (SOCKET)p, d;
    BYTE b[128], st[5], in[21], f[97], dd[9];
    int ty = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 8000) {
        ty = readframe(c, b, sizeof b);
        if (ty == 4) break;
        if (ty == 5) rl_raw_frame(c, 6, NULL, 0);   /* PING */
        else if (!ty && WSAGetLastError() != WSAETIMEDOUT) return 0;
    }
    if (ty != 4) return 0;
    if ((d = rl_raw(2, b, 16)) == INVALID_SOCKET) return 0;
    if (!recv_n(d, st, 5) || st[0]) { closesocket(d); return 0; }
    InterlockedExchange(&g41_paired, 1);
    if (recv_n(d, in, 21)) {                                  /* T_HELLO */
        memset(f, 0, sizeof f);
        *(DWORD *)f = 93; f[4] = 2;                           /* T_INFO */
        *(DWORD *)(f + 9) = 4;
        strcpy((char *)f + 33, "raw41");
        send(d, (char *)f, 97, 0);
        if (recv_n(d, f, 37)) {                               /* T_CREATEPL */
            *(DWORD *)dd = 5; dd[4] = 5; *(DWORD *)(dd + 5) = 0x1002;   /* T_PLAYERID */
            send(d, (char *)dd, 9, 0);
        }
    }
    Sleep(2000);
    closesocket(d);
    return 0;
}

/* raw joiner: create a player named nm on the open conn s, return its DPID (0 = failed) */
static DWORD make_player(SOCKET s, const char *nm)
{
    BYTE fr[37], b[64];
    int k, ty;
    *(DWORD *)fr = 33; fr[4] = 4;                       /* T_CREATEPL, 32-byte name */
    memset(fr + 5, 0, 32);
    strcpy((char *)fr + 5, nm);
    send(s, (char *)fr, 37, 0);
    for (k = 0; k < 20; k++) {
        ty = readframe(s, b, sizeof b);
        if (!ty) return 0;
        if (ty == 5) return *(DWORD *)b;                /* T_PLAYERID */
    }
    return 0;
}

/* raw joiner: one T_DATA frame from player `from` to `to` (0 = all) carrying game bytes d[0..n) */
static void send_data(SOCKET s, DWORD from, DWORD to, const BYTE *d, int n)
{
    BYTE f[300];
    *(DWORD *)f = (DWORD)(1 + 8 + n); f[4] = 8;
    *(DWORD *)(f + 5) = from; *(DWORD *)(f + 9) = to;
    memcpy(f + 13, d, n);
    if (n >= 4 && !d[1]) f[14] = (BYTE)(n - 4);   /* the game's length byte (v9 checks n == 4 + d[1]); a test that sets it keeps it */
    send(s, (char *)f, 13 + n, 0);
}

/* dplayx.log holds needle (polled up to ms) */
static int log_has(const char *needle, DWORD ms)
{
    DWORD t0 = GetTickCount();
    do {
        char ln[600], lp[MAX_PATH], *sl;
        FILE *lf;
        int seen = 0;
        GetModuleFileNameA(NULL, lp, MAX_PATH);   /* the DLL logs beside the exe, not in the cwd: run from elsewhere = false FAIL */
        sl = strrchr(lp, '\\');
        lstrcpyA(sl ? sl + 1 : lp, "dplayx.log");
        lf = fopen(lp, "r");
        if (lf) { while (fgets(ln, sizeof ln, lf)) if (strstr(ln, needle)) { seen = 1; break; } fclose(lf); }
        if (seen) return 1;
        Sleep(100);
    } while (GetTickCount() - t0 < ms);
    return 0;
}

/* dplayx.log holds `needle` on this many lines (0 when the log is not there yet). The file is moved aside once per
 * run, so a count is per process: a test that must prove its own event did NOT happen compares before and after. */
static int log_count(const char *needle)
{
    char ln[600], lp[MAX_PATH], *sl;
    FILE *lf;
    int n = 0;
    GetModuleFileNameA(NULL, lp, MAX_PATH);
    sl = strrchr(lp, '\\');
    lstrcpyA(sl ? sl + 1 : lp, "dplayx.log");
    lf = fopen(lp, "r");
    if (!lf) return 0;
    while (fgets(ln, sizeof ln, lf)) if (strstr(ln, needle)) n++;
    fclose(lf);
    return n;
}

/* T21-T25: drain the host receive queue; froms[]/types[] take the sender DPID and the first game byte of up to `cap`
 * messages. Stops once the queue has stayed empty for `quiet` ms. Returns the number of messages taken (which may
 * exceed cap: the counters stay exact, only the stored detail is cut). */
static DWORD drain_host(Obj *dp, DWORD *froms, BYTE *types, DWORD cap, DWORD quiet)
{
    DWORD f2, t2, sz, n = 0, empty = 0;
    BYTE rb[300];
    while (empty < quiet) {
        sz = sizeof rb;
        if (((RECV)dp->vt[25])(dp, &f2, &t2, 0, rb, &sz)) { Sleep(20); empty += 20; continue; }
        empty = 0;
        if (n < cap && froms) froms[n] = f2;
        if (n < cap && types) types[n] = rb[0];
        n++;
    }
    return n;
}

static HANDLE g_flooded;
/* T10 fake host: answer the hello with INFO, then send QMAX+200 data frames */
static DWORD WINAPI fake_host(LPVOID p)
{
    SOCKET l = (SOCKET)p, s = accept(l, NULL, NULL);
    BYTE in[64], f[97], d[29];
    int i;
    if (s == INVALID_SOCKET) return 0;
    recv(s, (char *)in, 21, 0);
    memset(f, 0, sizeof f);
    *(DWORD *)f = 93; f[4] = 2;                       /* T_INFO, 92 bytes */
    *(DWORD *)(f + 9) = 4;                            /* max players */
    strcpy((char *)f + 33, "fake");
    send(s, (char *)f, 97, 0);
    for (i = 0; i < 16384 + 200; i++) {
        memset(d, 0, sizeof d);
        *(DWORD *)d = 25; d[4] = 8;                   /* T_DATA: from, to, 16 bytes */
        *(DWORD *)(d + 5) = 0x1000;
        *(DWORD *)(d + 13) = (DWORD)i;
        if (send(s, (char *)d, 29, 0) != 29) break;
    }
    SetEvent(g_flooded);
    Sleep(10000);                                     /* stay connected while the test looks: a close is host-lost anyway */
    closesocket(s);
    return 0;
}

/* T17 fake host: answer the hello with INFO, then push a resync download BEGIN (v1, full size) the joiner never asked for */
static DWORD WINAPI fake_host2(LPVOID p)
{
    SOCKET l = (SOCKET)p, s = accept(l, NULL, NULL);
    BYTE in[64], f[97], d[29];
    if (s == INVALID_SOCKET) return 0;
    recv(s, (char *)in, 21, 0);
    memset(f, 0, sizeof f);
    *(DWORD *)f = 93; f[4] = 2;                       /* T_INFO */
    *(DWORD *)(f + 9) = 4;
    strcpy((char *)f + 33, "fake");
    send(s, (char *)f, 97, 0);
    Sleep(500);
    memset(d, 0, sizeof d);
    *(DWORD *)d = 25; d[4] = 11;                      /* T_RESYNC, RS_DN_BEGIN: ver 1, size 471828, fnv, turn 3, epoch 0 */
    *(DWORD *)(d + 5) = 1; *(DWORD *)(d + 9) = 1; *(DWORD *)(d + 13) = 471828; *(DWORD *)(d + 17) = 0x1234; *(DWORD *)(d + 21) = 3;
    send(s, (char *)d, 29, 0);
    Sleep(3000);
    closesocket(s);
    return 0;
}

/* T33 fake hub: INFO, give the joiner player 0x1002, then T_HOST(0x1001), T_HOST(0x1002) twice */
static DWORD WINAPI fake_host3(LPVOID p)
{
    SOCKET l = (SOCKET)p, s = accept(l, NULL, NULL);
    BYTE in[64], f[97], d[9];
    int got = 0, r, k;
    DWORD ids[3] = {0x1001, 0x1002, 0x1002};
    if (s == INVALID_SOCKET) return 0;
    recv(s, (char *)in, 21, 0);
    memset(f, 0, sizeof f);
    *(DWORD *)f = 93; f[4] = 2;                       /* T_INFO */
    *(DWORD *)(f + 9) = 4;
    strcpy((char *)f + 33, "fake");
    send(s, (char *)f, 97, 0);
    while (got < 37 && (r = recv(s, (char *)f + got, 37 - got, 0)) > 0) got += r;   /* T_CREATEPL */
    *(DWORD *)d = 5; d[4] = 5; *(DWORD *)(d + 5) = 0x1002;                            /* T_PLAYERID */
    send(s, (char *)d, 9, 0);
    Sleep(300);
    for (k = 0; k < 3; k++) { *(DWORD *)d = 5; d[4] = 12; *(DWORD *)(d + 5) = ids[k]; send(s, (char *)d, 9, 0); Sleep(100); }   /* T_HOST */
    Sleep(3000);
    closesocket(s);
    return 0;
}

/* T43 (rules.c): a raw joiner's hello; the answer's type, *len = its payload length, up to cap bytes in out */
static int hello_len(DWORD proto, BYTE *out, int cap, int *len, SOCKET *keep)
{
    struct sockaddr_in a;
    BYTE h[21], hdr[5];
    DWORD tmo = 3000;
    int r, got = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(47700);
    *len = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return 0; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    *(DWORD *)h = 17; h[4] = 1;
    memcpy(h + 5, "LOTR2NET", 8);
    *(DWORD *)(h + 13) = proto;
    *(DWORD *)(h + 17) = 2;                           /* MODE_JOIN */
    send(s, (char *)h, 21, 0);
    while (got < 5 && (r = recv(s, (char *)hdr + got, 5 - got, 0)) > 0) got += r;
    if (got < 5) { closesocket(s); return 0; }
    *len = (int)(*(DWORD *)hdr - 1);
    for (got = 0; got < *len && (r = recv(s, (char *)out + got, (*len < cap ? *len : cap) - got, 0)) > 0; ) got += r;
    *keep = s;
    return hdr[4];
}
/* T43: T_CAPS from a raw joiner: id (24 bytes), can */
static void send_caps(SOCKET s, const char *id, DWORD can)
{
    BYTE f[33];
    *(DWORD *)f = 29; f[4] = 15;
    memset(f + 5, 0, 24);
    strcpy((char *)f + 5, id);
    *(DWORD *)(f + 29) = can;
    send(s, (char *)f, 33, 0);
}
/* T43: the next T_RULES on a raw joiner's conn (0xdead = none within its timeout); its locked word in g43_locked */
static DWORD g43_locked;
static DWORD next_rules(SOCKET s)
{
    BYTE b[300];
    int k, ty;
    for (k = 0; k < 30; k++) {
        ty = readframe(s, b, sizeof b);
        if (!ty) break;
        if (ty == 16) { g43_locked = *(DWORD *)(b + 4); return *(DWORD *)b; }
    }
    return 0xdead;
}
/* T43 joiner side: a host with the marker; it keeps the joiner's T_CAPS and answers with rules 0x05, then 0x07 for
 * the game (locked) and a late 0x01 the joiner must ignore */
static char g43_id[25];
static DWORD g43_can = 0xdead;
static DWORD WINAPI fake_host43(LPVOID p)
{
    SOCKET l = (SOCKET)p, s = accept(l, NULL, NULL);
    BYTE in[64], f[105], d[13];
    int ty, k;
    DWORD to = 3000;
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    recv(s, (char *)in, 21, 0);
    memset(f, 0, sizeof f);
    *(DWORD *)f = 101; f[4] = 2;                      /* T_INFO, 92 bytes + "L2RU" + RU_VER 1 */
    *(DWORD *)(f + 9) = 4;
    strcpy((char *)f + 33, "fake43");
    memcpy(f + 97, "L2RU", 4); *(DWORD *)(f + 101) = 1;
    send(s, (char *)f, 105, 0);
    for (k = 0; k < 5; k++) {
        BYTE b[64];
        ty = readframe(s, b, sizeof b);
        if (ty == 15) { memcpy(g43_id, b, 24); g43_id[24] = 0; g43_can = *(DWORD *)(b + 24); }
        if (ty == 4 || !ty) break;                     /* T_CREATEPL */
    }
    *(DWORD *)d = 5; d[4] = 5; *(DWORD *)(d + 5) = 0x1002;   /* T_PLAYERID */
    send(s, (char *)d, 9, 0);
    *(DWORD *)d = 9; d[4] = 16; *(DWORD *)(d + 5) = 0x05; *(DWORD *)(d + 9) = 0;   /* T_RULES */
    send(s, (char *)d, 13, 0);
    Sleep(2000);                                       /* the joiner reads 0x05 first */
    *(DWORD *)(d + 5) = 0x07; *(DWORD *)(d + 9) = 1;   /* for this game */
    send(s, (char *)d, 13, 0);
    *(DWORD *)(d + 5) = 0x01; *(DWORD *)(d + 9) = 0;   /* too late: kept out */
    send(s, (char *)d, 13, 0);
    Sleep(3000);
    closesocket(s);
    return 0;
}

/* T38: a host that announces its own player 0x1001 and answers the DLL's pings with a session clock it controls:
 * 5 s at g38_t0, running since */
static volatile LONG g38_answers;
static DWORD g38_t0;
static DWORD WINAPI fake_host_clk(LPVOID p)
{
    SOCKET l = (SOCKET)p, s = accept(l, NULL, NULL);
    BYTE in[64], f[97], d[41], rb[300], fr[33];
    int got = 0, r, ty;
    DWORD t0 = GetTickCount(), to = 500, q[7];
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    recv(s, (char *)in, 21, 0);
    memset(f, 0, sizeof f);
    *(DWORD *)f = 93; f[4] = 2;                       /* T_INFO */
    *(DWORD *)(f + 9) = 4;
    strcpy((char *)f + 33, "fake");
    send(s, (char *)f, 97, 0);
    while (got < 37 && (r = recv(s, (char *)f + got, 37 - got, 0)) > 0) got += r;   /* T_CREATEPL */
    *(DWORD *)d = 5; d[4] = 5; *(DWORD *)(d + 5) = 0x1002;                            /* T_PLAYERID */
    send(s, (char *)d, 9, 0);
    memset(d, 0, sizeof d);
    *(DWORD *)d = 37; d[4] = 6; *(DWORD *)(d + 5) = 0x1001; strcpy((char *)d + 9, "host38");   /* T_SYS_CREATE */
    send(s, (char *)d, 41, 0);
    while (GetTickCount() - t0 < 8000) {
        ty = readframe(s, rb, sizeof rb);
        if (ty != 13) { if (!ty && WSAGetLastError() != WSAETIMEDOUT) break; continue; }
        memcpy(q, rb, 28);
        if (q[1] != 0x1001) continue;
        {
            unsigned long long ss = 5000000ull + (unsigned long long)(GetTickCount() - g38_t0) * 1000ull;
            q[4] = (DWORD)ss; q[5] = (DWORD)(ss >> 32); q[6] = 1;
        }
        *(DWORD *)fr = 29; fr[4] = 14; memcpy(fr + 5, q, 28);
        send(s, (char *)fr, 33, 0);
        InterlockedIncrement(&g38_answers);
    }
    closesocket(s);
    return 0;
}

static volatile LONG g_live_bytes;
/* T9: a joiner that keeps reading (counts bytes until its socket closes) */
static DWORD WINAPI live_reader(LPVOID p)
{
    SOCKET s = (SOCKET)p;
    char b[4096];
    int r;
    while ((r = recv(s, b, sizeof b, 0)) > 0) InterlockedExchangeAdd(&g_live_bytes, r);
    return 0;
}

static int g_cb;
static char g_cbname[80];
static BOOL WINAPI cb(const DPSD2 *d, DWORD *to, DWORD f, void *ctx)
{
    g_cb++;
    lstrcpynA(g_cbname, d->lpszSessionName ? d->lpszSessionName : "", sizeof g_cbname);
    return FALSE;
}

int main(int argc, char **argv)
{
    DWORD proto = argc > 1 ? (DWORD)atoi(argv[1]) : 7000, t;
    HMODULE m;
    Obj *dp1, *dp;
    DPSD2 sd;
    CAPS caps;
    BYTE info[92];
    char buf[1024];   /* result lines: T29, T32 and T36 run past 300 characters */
    SOCKET keep = INVALID_SOCKET;
    DWORD me = 0;                 /* T18's host-side player, kept for T22/T25: the session allows no more than 4 players */
    WSADATA w;
    HR hr;
    int ty;
    WSAStartup(MAKEWORD(2, 2), &w);
    /* loopback only: a listener on every address makes Windows Firewall ask for permission (Matthew 2026-10-06) */
    SetEnvironmentVariableA("LOTR2NET_BIND", "127.0.0.1");
    _putenv("LOTR2NET_BIND=127.0.0.1");
    {   /* T37 setup: the files must be there before the DLL loads */
        FILE *f37 = fopen("lotr2net.ini", "w");
        if (f37) { fputs("; host= unset\nhost = 127.0.0.1\nghost=0 ; would end T32's ghost hub\ndiag=1\n", f37); fclose(f37); }
        f37 = fopen("lords2hd.ini", "w");
        if (f37) { fputs("[Display]\nEnabled=1\n[Multiplayer]\nghost=1\n", f37); fclose(f37); }
        DeleteFileA("lotr2net.ini.old");
    }
    m = LoadLibraryA(".\\dplayx.dll");
    if (!m) { printf("FAIL load dplayx.dll (%lu)\n", GetLastError()); return 1; }
    {   /* T37 (v12): lotr2net.ini moved into lords2hd.ini [Multiplayer] at load */
        char ini[MAX_PATH], h37[32], g37[8], d37[8], e37[8];
        int gone, old37;
        GetFullPathNameA("lords2hd.ini", MAX_PATH, ini, NULL);
        GetPrivateProfileStringA("Multiplayer", "host", "", h37, sizeof h37, ini);
        GetPrivateProfileStringA("Multiplayer", "ghost", "", g37, sizeof g37, ini);
        GetPrivateProfileStringA("Multiplayer", "diag", "", d37, sizeof d37, ini);
        GetPrivateProfileStringA("Display", "Enabled", "", e37, sizeof e37, ini);
        gone = GetFileAttributesA("lotr2net.ini") == INVALID_FILE_ATTRIBUTES;
        old37 = GetFileAttributesA("lotr2net.ini.old") != INVALID_FILE_ATTRIBUTES;
        sprintf(buf, "host '%s' ghost '%s' (want 1) diag '%s' [Display] Enabled '%s', lotr2net.ini gone %d, .old %d", h37, g37, d37, e37, gone, old37);
        result("T37 lotr2net.ini moved to lords2hd.ini", !strcmp(h37, "127.0.0.1") && !strcmp(g37, "1") && !strcmp(d37, "1") && !strcmp(e37, "1") && gone && old37, buf);
    }
    ((CREATE)GetProcAddress(m, "DirectPlayCreate"))(NULL, (void **)&dp1, NULL);
    ((QI)dp1->vt[0])(dp1, &IID_IUnknown_, (void **)&dp);

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwMaxPlayers = 4;
    sd.lpszSessionName = "selftest";
    sd.guidApplication.Data1 = 0x1234;
    hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
    sprintf(buf, "hr=%08lx", (unsigned long)hr); result("T1 open(create)", hr == 0, buf);

    ty = hello(proto, 1, info, NULL);
    sprintf(buf, "type=%d name='%.60s'", ty, ty == 2 ? (char *)info + 28 : ""); result("T2 enum same build", ty == 2, buf);

    memset(info, 0, sizeof info);
    ty = hello(proto - 1, 1, info, NULL);
    sprintf(buf, "type=%d name='%.60s'", ty, ty == 2 ? (char *)info + 28 : "");
    result("T3 enum other build listed+marked", ty == 2 && strstr((char *)info + 28, "yours v"), buf);

    ty = hello(proto - 1, 2, info, NULL);
    sprintf(buf, "type=%d", ty); result("T4 join other build refused", ty == 3, buf);

    ty = hello(proto, 2, info, &keep);
    sprintf(buf, "type=%d", ty); result("T5 join same build", ty == 2, buf);
    Sleep(200);

    ((CLOSE)dp->vt[4])(dp);                 /* host closes the joiner's conn first: TIME_WAIT stays on our side */
    Sleep(300);
    if (keep != INVALID_SOCKET) closesocket(keep);
    t = GetTickCount();
    hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
    sprintf(buf, "hr=%08lx after %lu ms", (unsigned long)hr, GetTickCount() - t); result("T6 re-host at once", hr == 0, buf);

    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    ((GETCAPS)dp->vt[14])(dp, &caps, 0);
    sprintf(buf, "dwMaxBufferSize=%lu", caps.dwMaxBufferSize); result("T7 getcaps", caps.dwMaxBufferSize == 260, buf);

    SetEnvironmentVariableA("LOTR2NET_HOST", "localhost");
    _putenv("LOTR2NET_HOST=localhost");
    t = GetTickCount();
    ((ENUMS)dp->vt[13])(dp, &sd, 1000, cb, NULL, 0);
    sprintf(buf, "callbacks=%d name='%s' %lu ms", g_cb, g_cbname, GetTickCount() - t);
    result("T8 enum by name", g_cb == 1, buf);

    {   /* T35 (v12 diag): pings between the DLLs, never shown to the game. Before the floods below: past 6 MB the log
         * holds no per-message lines at all, and the heartbeat check needs them. In a session of its own, then re-hosted */
        SOCKET ja = INVALID_SOCKET, jb = INVALID_SOCKET;
        NAME_ nm = {sizeof nm, 0, "host35", NULL};
        DWORD ida, idb, t35, f[7], me35 = 0;   /* DgPing: from, to, seq, us, ss_lo, ss_hi, ok (28 bytes, v13) */
        BYTE hb[8] = {0x0a, 4, 0x35, 1, 0, 0, 0, 0}, other[8] = {0x0b, 4, 0x35, 1, 0, 0, 0, 0}, rb[64], fr[33];
        int ty35, ping = 0, answered = 0, relayed = 0, spoof = 0, rtt, hbline, otherline, close_ok, clock = 0, relay_clear = 0;
        ((CLOSE)dp->vt[4])(dp);
        SetEnvironmentVariableA("LOTR2NET_TEST_DIAG", "1");
        hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
        ((CPL)dp->vt[6])(dp, &me35, &nm, NULL, NULL, 0, 0);
        hello(proto, 2, info, &ja);
        ida = make_player(ja, "a35");
        hello(proto, 2, info, &jb);
        idb = make_player(jb, "b35");
        *(DWORD *)fr = 29;
        /* the host's ping reaches a joiner within its second; the answer becomes that peer's rtt */
        for (t35 = GetTickCount(); !ping && GetTickCount() - t35 < 4000; ) {
            ty35 = readframe(ja, rb, sizeof rb);
            if (ty35 != 13) continue;
            memcpy(f, rb, 28);
            if (f[0] == me35 && f[1] == ida) ping = 1;
        }
        fr[4] = 14; memcpy(fr + 5, f, 28); send(ja, (char *)fr, 33, 0);
        rtt = log_has("n=1 sent=", 4000);
        /* the joiner pings the host; a stamp of ours comes back untouched */
        memset(f, 0, sizeof f);
        fr[4] = 13; f[0] = ida; f[1] = me35; f[2] = 7; f[3] = 123; memcpy(fr + 5, f, 28); send(ja, (char *)fr, 33, 0);
        /* the joiner pings the other joiner, through the host */
        f[1] = idb; f[2] = 9; memcpy(fr + 5, f, 28); send(ja, (char *)fr, 33, 0);
        /* the joiner claims to be the other joiner: dropped, so seq 11 never comes back */
        f[0] = idb; f[1] = me35; f[2] = 11; memcpy(fr + 5, f, 28); send(ja, (char *)fr, 33, 0);
        for (t35 = GetTickCount(); GetTickCount() - t35 < 2500; ) {
            ty35 = readframe(ja, rb, sizeof rb);
            if (ty35 != 14) continue;
            memcpy(f, rb, 28);
            if (f[2] == 7 && f[3] == 123) { answered = 1; clock = f[6] == 1 && (f[4] || f[5]); }   /* the host's clock in it */
            if (f[2] == 11) spoof = 1;
        }
        for (t35 = GetTickCount(); !relayed && GetTickCount() - t35 < 4000; ) {
            ty35 = readframe(jb, rb, sizeof rb);
            if (ty35 != 13) continue;
            memcpy(f, rb, 28);
            if (f[0] == ida && f[1] == idb && f[2] == 9) relayed = 1;
        }
        /* jb answers that ping claiming a session clock: the host relays the pong with the clock cleared */
        f[4] = 0x12345678; f[5] = 0; f[6] = 1;
        fr[4] = 14; memcpy(fr + 5, f, 28); send(jb, (char *)fr, 33, 0);
        for (t35 = GetTickCount(); !relay_clear && GetTickCount() - t35 < 3000; ) {
            ty35 = readframe(ja, rb, sizeof rb);
            if (ty35 != 14) continue;
            memcpy(f, rb, 28);
            if (f[2] == 9) relay_clear = f[6] == 0 && f[4] == 0x12345678 ? 1 : -1;
        }
        ((SEND)dp->vt[26])(dp, me35, 0, 0, hb, 8);
        ((SEND)dp->vt[26])(dp, me35, 0, 0, other, 8);
        otherline = log_has("b4=0b 04 35 01", 3000);
        hbline = log_has("b4=0a 04 35 01", 500);
        closesocket(ja); closesocket(jb);
        Sleep(200);
        t35 = GetTickCount();
        ((CLOSE)dp->vt[4])(dp);
        close_ok = GetTickCount() - t35 < 3500;
        SetEnvironmentVariableA("LOTR2NET_TEST_DIAG", NULL);
        sprintf(buf, "open hr=%08lx ping %d rtt line %d answered %d with clock %d relayed %d relayed pong clock cleared %d spoof answered %d, hb line %d other line %d, close ok %d",
                (unsigned long)hr, ping, rtt, answered, clock, relayed, relay_clear, spoof, hbline, otherline, close_ok);
        result("T35 diag pings and heartbeat lines", hr == 0 && ping && rtt && answered && clock && relayed && relay_clear == 1 && !spoof && !hbline && otherline && close_ok, buf);
        hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
    }

    {   /* T9 (v7): a stalled joiner never blocks the game thread and never starves a live joiner */
        DWORD me = 0, from, to, sz, t9, worst = 0, sends = 0, before, after;
        NAME_ nm = {sizeof nm, 0, "host", NULL};
        BYTE fr[37], msg[200], rb[300];
        int destroyed = 0, small = 2048, ty2;
        SOCKET j = INVALID_SOCKET, j2 = INVALID_SOCKET;
        HANDLE rt;
        ((CPL)dp->vt[6])(dp, &me, &nm, NULL, NULL, 0, 0);
        ty = hello(proto, 2, info, &j);
        setsockopt(j, SOL_SOCKET, SO_RCVBUF, (char *)&small, sizeof small);
        *(DWORD *)fr = 33; fr[4] = 4;                       /* T_CREATEPL, 32-byte name */
        memset(fr + 5, 0, 32);
        strcpy((char *)fr + 5, "staller");
        send(j, (char *)fr, 37, 0);
        ty2 = hello(proto, 2, info, &j2);
        strcpy((char *)fr + 5, "reader");
        send(j2, (char *)fr, 37, 0);
        rt = CreateThread(NULL, 0, live_reader, (LPVOID)j2, 0, NULL);
        Sleep(500);
        memset(msg, 0x55, sizeof msg);
        t = GetTickCount();
        while (GetTickCount() - t < 60000 && !destroyed) {   /* send until the staller is dropped */
            DWORD s0 = GetTickCount(), d;
            ((SEND)dp->vt[26])(dp, me, 0, 0, msg, sizeof msg);
            sends++;
            if ((d = GetTickCount() - s0) > worst) worst = d;
            for (;;) {
                sz = sizeof rb;
                if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) break;
                if (from == 0 && *(DWORD *)rb == 5) destroyed = 1;
            }
            if (!(sends & 255)) Sleep(1);
        }
        t9 = GetTickCount() - t;
        before = g_live_bytes;
        for (sz = 0; sz < 20; sz++) { ((SEND)dp->vt[26])(dp, me, 0, 0, msg, sizeof msg); Sleep(10); }
        Sleep(500);
        after = g_live_bytes;
        sprintf(buf, "%lu sends, slowest %lu ms, DESTROYPLAYER %s after %lu ms, live joiner %lu -> %lu bytes", sends, worst,
                destroyed ? "seen" : "NOT seen", t9, before, after);
        result("T9 stalled joiner dropped, never blocks", ty == 2 && ty2 == 2 && worst <= 500 && destroyed && after > before, buf);
        closesocket(j);
        closesocket(j2);
        WaitForSingleObject(rt, 3000);
    }

    {   /* T15 (review F1): the game's lord table gives lord 3 to player B; conn A claiming lord 3 is refused and stays unbound */
        SOCKET ja = INVALID_SOCKET, jb = INVALID_SOCKET;
        DWORD ida, idb;
        BYTE sy[5 + 32];
        char needle[200], tab[40];
        int dropped, bound4;
        hello(proto, 2, info, &ja);
        hello(proto, 2, info, &jb);
        ida = make_player(ja, "claimerA");
        idb = make_player(jb, "ownerB");
        sprintf(tab, "LOTR2NET_TEST_LORDTAB=%lx:3", idb);
        _putenv(tab);
        memset(sy, 0, sizeof sy);
        *(DWORD *)sy = 33; sy[4] = 10;                      /* T_SYNC, header only: kind 1, turn 5, player word = lord 3, 0 regions */
        ((DWORD *)(sy + 5))[0] = 1; ((DWORD *)(sy + 5))[1] = 5; ((DWORD *)(sy + 5))[5] = 3;
        send(ja, (char *)sy, sizeof sy, 0);
        sprintf(needle, "(player %08lx): lord byte 3 is given to another player", ida);
        dropped = log_has(needle, 3000);
        ((DWORD *)(sy + 5))[5] = 4;                         /* control: lord 4 is free in the table, A binds to it (the refusal bound nothing) */
        send(ja, (char *)sy, sizeof sy, 0);
        sprintf(needle, "(player %08lx) -> lord 4, learned from its first record", ida);
        bound4 = log_has(needle, 3000);
        _putenv("LOTR2NET_TEST_LORDTAB=");
        sprintf(buf, "A=%08lx B=%08lx, claim of B's lord %s, free lord %s", ida, idb, dropped ? "REFUSED" : "NOT refused", bound4 ? "bound" : "NOT bound");
        result("T15 lord owned by another player refused", ida && idb && dropped && bound4, buf);
        closesocket(ja);
        closesocket(jb);
    }

    {   /* T16 (review F2): 700 upload BEGIN frames in one burst are cut at the per-conn budget; the host still answers */
        SOCKET jc = INVALID_SOCKET;
        DWORD idc, k;
        BYTE *fl = (BYTE *)malloc(700 * 29);
        int cut, alive;
        hello(proto, 2, info, &jc);
        idc = make_player(jc, "flooder");
        memset(fl, 0, 700 * 29);
        for (k = 0; k < 700; k++) {
            BYTE *d = fl + k * 29;
            *(DWORD *)d = 25; d[4] = 11;                    /* T_RESYNC, RS_UP_BEGIN: ver 1, turn 3, epoch 0, lord 2 */
            *(DWORD *)(d + 5) = 4; *(DWORD *)(d + 9) = 1; *(DWORD *)(d + 13) = 3; *(DWORD *)(d + 21) = 2;
        }
        send(jc, (char *)fl, 700 * 29, 0);
        free(fl);
        cut = log_has("resync frames in 1 s - extra dropped", 3000);
        ty = hello(proto, 1, info, NULL);
        alive = ty == 2;
        sprintf(buf, "player %08lx, flood %s, host answers enum %s", idc, cut ? "cut" : "NOT cut", alive ? "yes" : "NO");
        result("T16 resync BEGIN flood rate-limited", idc && cut && alive, buf);
        closesocket(jc);
    }

    {   /* T18 (v9 peer trust): a joiner bound to lord 2 by the host's own 0x02 sends forged and bad game messages; the host
         * drops each one (log line + never delivered), and still delivers the honest message sent after them */
        SOCKET jd = INVALID_SOCKET;
        DWORD idd, from, to, sz, k;   /* me: the host player declared in main, T22/T25 reuse it */
        NAME_ nm = {sizeof nm, 0, "host2", NULL};
        BYTE m02[61], g[64], rb[300];
        int got16 = 0, got1e = 0, gotbad = 0, n_ok = 0;
        static const char *needles[] = {"type 0x16 only the host sends", "0x0f binds player", "acts for lord 3, the sender plays lord 2",
                                        "type 0x99 is not a game message", "answers no offer", "type 0x59 index 500", "is never sent by the game"};
        ((CPL)dp->vt[6])(dp, &me, &nm, NULL, NULL, 0, 0);
        hello(proto, 2, info, &jd);
        idd = make_player(jd, "cheater");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idd, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        memset(g, 0, sizeof g); g[0] = 0x16; g[1] = 1; send_data(jd, idd, 0, g, 5);                         /* rollback verdict */
        memset(g, 0, sizeof g); g[0] = 0x0f; g[1] = 8; *(DWORD *)(g + 4) = 2; *(DWORD *)(g + 8) = me; send_data(jd, idd, 0, g, 12);   /* bind the host's DPID */
        memset(g, 0, sizeof g); g[0] = 0x1e; g[1] = 5; g[7] = 3; send_data(jd, idd, 0, g, 9);               /* end lord 3's turn */
        memset(g, 0, sizeof g); g[0] = 0x99; send_data(jd, idd, 0, g, 8);                                   /* not a game type */
        memset(g, 0, sizeof g); g[0] = 0x4a; *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 1; *(DWORD *)(g + 16) = 500; send_data(jd, idd, 0, g, 24);   /* take lord 1's crowns */
        memset(g, 0, sizeof g); g[0] = 0x59; *(DWORD *)(g + 4) = 500; send_data(jd, idd, 0, g, 12);        /* chunk index past the buffer */
        memset(g, 0, sizeof g); g[0] = 0x50; send_data(jd, idd, 0, g, 8);                                   /* never sent by the game */
        memset(g, 0, sizeof g); g[0] = 0x1e; g[1] = 5; g[7] = 2; send_data(jd, idd, 0, g, 9);               /* honest: end OUR turn */
        for (k = 0; k < sizeof needles / sizeof needles[0]; k++) if (log_has(needles[k], 3000)) n_ok++; else printf("     missing log: %s\n", needles[k]);
        for (k = 0; k < 200; k++) {
            sz = sizeof rb;
            if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) { if (got1e) break; Sleep(20); continue; }
            if (from != idd) continue;
            if (rb[0] == 0x16) got16 = 1; else if (rb[0] == 0x1e && rb[7] == 2) got1e = 1; else gotbad = 1;
        }
        sprintf(buf, "player %08lx: %d/7 drops logged, forged delivered %s, honest 0x1e %s", idd, n_ok, got16 || gotbad ? "YES" : "no", got1e ? "delivered" : "NOT delivered");
        result("T18 forged game messages dropped by the host", idd && n_ok == 7 && !got16 && !gotbad && got1e, buf);
        closesocket(jd);
    }

    {   /* T19 (v9 F3): one address holds at most 4 handshakes; a 5th silent connection is closed at once, the host still answers */
        SOCKET hs[5];
        struct sockaddr_in a;
        int k, closed = 0, alive;
        char c;
        DWORD tmo = 1500;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_addr.s_addr = inet_addr("127.0.0.1"); a.sin_port = htons(47700);
        for (k = 0; k < 5; k++) { hs[k] = socket(AF_INET, SOCK_STREAM, 0); connect(hs[k], (struct sockaddr *)&a, sizeof a); Sleep(50); }
        setsockopt(hs[4], SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
        closed = recv(hs[4], &c, 1, 0) == 0 || log_has("handshakes in flight from 127.0.0.1", 1000);
        for (k = 0; k < 5; k++) closesocket(hs[k]);
        Sleep(200);
        ty = hello(proto, 1, info, NULL);
        alive = ty == 2;
        sprintf(buf, "5th silent connection %s, host answers enum %s", closed ? "closed" : "NOT closed", alive ? "yes" : "NO");
        result("T19 per-address handshake cap", closed && alive, buf);
    }

    {   /* T20 (v9 F4): a joiner fills the host's receive queue while the game is not reading; the conn WAITS (no drop),
         * and after the game reads, the joiner's later messages still arrive */
        SOCKET je = INVALID_SOCKET;
        DWORD ide, from, to, sz, n = 0, k, want = 16384 + 300;
        BYTE g[8], rb[300], *fl;
        int waited, dropped, resumed, last = 0;
        SetEnvironmentVariableA("LOTR2NET_QWAIT_MS", "20000");
        SetEnvironmentVariableA("LOTR2NET_TEST_DATA_RATE", "0");   /* no game-message cap here: the conn must fill up, not be cut */
        hello(proto, 2, info, &je);
        ide = make_player(je, "burst");
        for (;;) { sz = sizeof rb; if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) break; }   /* start empty */
        fl = (BYTE *)malloc(want * 21);
        for (k = 0; k < want; k++) {   /* 0x15 pause toggle: no lord field, 8 bytes */
            BYTE *f = fl + k * 21;
            *(DWORD *)f = 17; f[4] = 8; *(DWORD *)(f + 5) = ide; *(DWORD *)(f + 9) = 0;
            memset(f + 13, 0, 8); f[13] = 0x15; f[14] = 4;
        }
        send(je, (char *)fl, (int)(want * 21), 0);
        free(fl);
        waited = log_has("waits for the game to read (up to 20000 ms)", 15000);
        for (k = 0; k < 400; k++) {   /* the game reads everything */
            sz = sizeof rb;
            if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) { if (n >= want) break; Sleep(20); continue; }
            if (from == ide) n++;
            k = 0;
        }
        memset(g, 0, sizeof g); g[0] = 0x15; g[1] = 4; g[4] = 0x77;
        send_data(je, ide, 0, g, 8);
        for (k = 0; k < 100 && !last; k++) {
            sz = sizeof rb;
            if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) { Sleep(20); continue; }
            if (from == ide && rb[0] == 0x15 && rb[4] == 0x77) last = 1;
        }
        dropped = log_has("shutting conn", 10);
        resumed = log_has("has room again after", 10);
        sprintf(buf, "player %08lx: waited %s, %lu of %lu read, room again %s, conn dropped %s, later msg %s", ide, waited ? "yes" : "NO", n, want,
                resumed ? "yes" : "NO", dropped ? "YES" : "no", last ? "arrived" : "LOST");
        result("T20 full host queue waits for the game", ide && waited && resumed && !dropped && last && n >= want, buf);
        SetEnvironmentVariableA("LOTR2NET_QWAIT_MS", NULL);
        SetEnvironmentVariableA("LOTR2NET_TEST_DATA_RATE", NULL);
        closesocket(je);
    }

    {   /* T21 (review2-trust F1-F3): a joiner bound to lord 2 sends 8 diplomacy payloads the game never sends; the host
         * drops each one (its own log line, never delivered) and still delivers the honest 0x1e sent after them */
        SOCKET jf = INVALID_SOCKET;
        DWORD idf = 0, k, nd, n_ok = 0, got = 0, bad = 0, froms[64];
        BYTE m02[61], g[64], types[64];
        static const char *needles[] = {"0x46 gives a negative amount", "0x46 gives to lord 9", "0x48 kind 0 carries -5 crowns",
                                        "0x48 kind 3 carries 7 crowns", "0x49 offers a negative amount", "type 0x2f index 99, max 16",
                                        "type 0x2d index 999, max 150", "0x47 answers lord 7"};
        hello(proto, 2, info, &jf);
        idf = make_player(jf, "diplo");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idf, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        memset(g, 0, sizeof g); g[0] = 0x46; g[1] = 20; *(DWORD *)(g + 8) = 1; *(DWORD *)(g + 12) = 2; *(DWORD *)(g + 20) = 0xFFFFFF00u; send_data(jf, idf, 0, g, 24);   /* a negative gift */
        memset(g, 0, sizeof g); g[0] = 0x46; g[1] = 20; *(DWORD *)(g + 8) = 9; *(DWORD *)(g + 12) = 2; *(DWORD *)(g + 20) = 10; send_data(jf, idf, 0, g, 24);             /* gift to lord 9 */
        memset(g, 0, sizeof g); g[0] = 0x48; g[1] = 24; *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 1; *(DWORD *)(g + 20) = 0xFFFFFFFBu; send_data(jf, idf, 0, g, 28);  /* kind 0 carrying -5 crowns */
        memset(g, 0, sizeof g); g[0] = 0x48; g[1] = 24; *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 1; *(DWORD *)(g + 16) = 3; *(DWORD *)(g + 20) = 7; send_data(jf, idf, 0, g, 28);   /* kind 3 carrying crowns */
        memset(g, 0, sizeof g); g[0] = 0x49; g[1] = 20; *(DWORD *)(g + 4) = 2; *(DWORD *)(g + 8) = 1; *(DWORD *)(g + 16) = 0xFFFFFFFFu; send_data(jf, idf, 0, g, 24);   /* a negative offer */
        memset(g, 0, sizeof g); g[0] = 0x2f; g[1] = 12; *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 99; send_data(jf, idf, 0, g, 16);                                  /* county index past 16 */
        memset(g, 0, sizeof g); g[0] = 0x2d; g[1] = 12; *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 999; send_data(jf, idf, 0, g, 16);                                 /* record index past 150 */
        memset(g, 0, sizeof g); g[0] = 0x47; g[1] = 16; *(DWORD *)(g + 8) = 7; *(DWORD *)(g + 12) = 2; *(DWORD *)(g + 16) = 1; send_data(jf, idf, 0, g, 20);              /* alliance answer to lord 7 */
        memset(g, 0, sizeof g); g[0] = 0x1e; g[1] = 5; g[7] = 2; send_data(jf, idf, 0, g, 9);   /* honest: end OUR turn */
        for (k = 0; k < 8; k++) if (log_has(needles[k], 3000)) n_ok++; else printf("     missing log: %s\n", needles[k]);
        nd = drain_host(dp, froms, types, 64, 600);
        for (k = 0; k < nd && k < 64; k++) if (froms[k] == idf) { got++; if (types[k] != 0x1e) bad++; }
        sprintf(buf, "player %08lx: %lu/8 drops logged, %lu delivered, %lu of them bad", idf, n_ok, got, bad);
        result("T21 bad diplomacy values dropped", idf && n_ok == 8 && got == 1 && !bad, buf);
        closesocket(jf);
        Sleep(400);   /* let the host tear the conn down: the player table allows 4 (see T23) */
    }

    {   /* T22 (review2-trust F5, review3 R3-1): an answer to a real offer passes, and so do the game's two copies of it
         * sent right after it; the same copy sent again later (the game would execute it again once its seq slot ages
         * out), a copy carrying another seq and an answer to an already answered offer are dropped */
        SOCKET jg = INVALID_SOCKET;
        DWORD idg = 0, k, nd, ans = 0, got1e = 0, froms[64];
        BYTE m02[61], o[24], g[64], types[64];
        static const BYTE seqs[6] = {5, 5 | 0x40, 5 | 0x80, 5 | 0x40, 6 | 0x80, 7};
        hello(proto, 2, info, &jg);
        idg = make_player(jg, "answerer");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idg, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        memset(o, 0, sizeof o);   /* our own 0x49: lord 1 offers lord 2 500 crowns */
        o[0] = 0x49; o[1] = 20; *(DWORD *)(o + 4) = 1; *(DWORD *)(o + 8) = 2; *(DWORD *)(o + 16) = 500;
        ((SEND)dp->vt[26])(dp, me, 0, 0, o, 24);
        Sleep(200);
        for (k = 0; k < 6; k++) {   /* the joiner's 0x4a answers, with the seq and copy bits the game uses */
            memset(g, 0, sizeof g);
            g[0] = 0x4a; g[1] = 20; g[3] = seqs[k];
            *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 1; *(DWORD *)(g + 16) = 500;
            send_data(jg, idg, 0, g, 24);
        }
        memset(g, 0, sizeof g); g[0] = 0x1e; g[1] = 5; g[7] = 2; send_data(jg, idg, 0, g, 9);   /* honest: end OUR turn */
        nd = drain_host(dp, froms, types, 64, 800);
        for (k = 0; k < nd && k < 64; k++)
            if (froms[k] == idg) { if (types[k] == 0x4a) ans++; else if (types[k] == 0x1e) got1e = 1; }
        sprintf(buf, "player %08lx: %lu of 6 answers delivered (want 3), honest 0x1e %s", idg, ans, got1e ? "delivered" : "NOT delivered");
        result("T22 answer copies pass once each", idg && ans == 3 && got1e && log_has("answers no offer", 3000)
               && log_has("copy 0x40 of type 0x4a does not follow its original", 3000), buf);
        closesocket(jg);
        Sleep(400);
    }

    {   /* T23 (review2-trust F4): 0x01 is refused for a player that already has a lord, and a second one within 4 s; the
         * game's copy of a fresh player's first 0x01 passes. One player at a time: the table holds 4 (dplayx.c:928) */
        SOCKET jh = INVALID_SOCKET, ji = INVALID_SOCKET;
        DWORD idh = 0, idi = 0, k, nd, nh = 0, ni = 0, froms[64];
        BYTE m02[61], g[64], types[64];
        int lord3, again;
        hello(proto, 2, info, &jh);
        idh = make_player(jh, "lordX");
        hello(proto, 2, info, &ji);
        idi = make_player(ji, "lordY");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 3;
        ((SEND)dp->vt[26])(dp, me, idh, 0, m02, sizeof m02);   /* the host gives X lord 3 */
        Sleep(300);
        memset(g, 0, sizeof g); g[0] = 0x01; g[1] = 44; g[3] = 1; *(DWORD *)(g + 4) = idh; send_data(jh, idh, 0, g, 48);              /* X has a lord: refused */
        memset(g, 0, sizeof g); g[0] = 0x01; g[1] = 44; g[3] = 1; *(DWORD *)(g + 4) = idi; send_data(ji, idi, 0, g, 48);              /* Y: the first one */
        memset(g, 0, sizeof g); g[0] = 0x01; g[1] = 44; g[3] = 1 | 0x40; *(DWORD *)(g + 4) = idi; send_data(ji, idi, 0, g, 48);       /* Y: the game's copy */
        memset(g, 0, sizeof g); g[0] = 0x01; g[1] = 44; g[3] = 2; *(DWORD *)(g + 4) = idi; send_data(ji, idi, 0, g, 48);              /* Y: again at once */
        memset(g, 0, sizeof g); g[0] = 0x01; g[1] = 44; g[3] = 2 | 0x40; *(DWORD *)(g + 4) = idi; send_data(ji, idi, 0, g, 48);       /* copies of the refused one */
        memset(g, 0, sizeof g); g[0] = 0x01; g[1] = 44; g[3] = 1 | 0x80; *(DWORD *)(g + 4) = idi; send_data(ji, idi, 0, g, 48);       /* an old copy, replayed */
        lord3 = log_has("the player already has lord 3", 3000);
        again = log_has("0x01 again within 4 s", 3000) && log_has("copy 0x80 of type 0x01 does not follow its original", 3000);
        nd = drain_host(dp, froms, types, 64, 800);
        for (k = 0; k < nd && k < 64; k++) { if (froms[k] == idh) nh++; else if (froms[k] == idi) ni++; }
        sprintf(buf, "X=%08lx Y=%08lx: lord 3 %s, 4 s %s, X delivered %lu, Y delivered %lu", idh, idi,
                lord3 ? "refused" : "NOT refused", again ? "refused" : "NOT refused", nh, ni);
        result("T23 hello limited", idh && idi && lord3 && again && nh == 0 && ni == 2, buf);
        closesocket(jh);
        closesocket(ji);
        Sleep(400);
    }

    {   /* T24 (v9 F7): one conn sends 5000 game messages in a single burst; the host takes the saved-up budget of them,
         * drops the rest (logged) and still answers a fresh handshake */
        SOCKET jj = INVALID_SOCKET;
        DWORD idj = 0, k, nd, cnt = 0, want = 5000 * 21;
        BYTE m02[61], rb[300], *fl, *types;
        DWORD *froms;
        int cut, alive, calls = 0;
        hello(proto, 2, info, &jj);
        idj = make_player(jj, "flood2");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idj, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        for (;;) { DWORD f2, t2, sz = sizeof rb; if (((RECV)dp->vt[25])(dp, &f2, &t2, 0, rb, &sz)) break; }   /* start empty */
        fl = (BYTE *)malloc(want);
        for (k = 0; k < 5000; k++) {   /* 0x15 pause toggle: no lord field, 8 bytes (the buffer T20 builds) */
            BYTE *f = fl + k * 21;
            *(DWORD *)f = 17; f[4] = 8; *(DWORD *)(f + 5) = idj; *(DWORD *)(f + 9) = 0;
            memset(f + 13, 0, 8); f[13] = 0x15; f[14] = 4;
        }
        for (k = 0; k < want; ) {   /* one burst, no pacing: the socket only splits it when its buffer is short */
            int r = send(jj, (char *)fl + k, (int)(want - k), 0);
            if (r <= 0) break;
            k += (DWORD)r;
            calls++;
        }
        free(fl);
        Sleep(1000);
        froms = (DWORD *)malloc(4096 * sizeof(DWORD));
        types = (BYTE *)malloc(4096);
        nd = drain_host(dp, froms, types, 4096, 800);
        for (k = 0; k < nd && k < 4096; k++) if (froms[k] == idj) cnt++;
        free(froms);
        free(types);
        cut = log_has("over 200 game messages per second", 5000);
        ty = hello(proto, 2, info, NULL);
        alive = ty == 2;
        sprintf(buf, "player %08lx: %lu of 5000 delivered in %d write(s), flood %s, host answers join %s", idj, cnt, calls,
                cut ? "cut" : "NOT cut", alive ? "yes" : "NO");
        result("T24 game message flood capped", idj && cut && cnt >= 3000 && cnt < 5000 && alive, buf);
        closesocket(jj);
        Sleep(400);
    }

    {   /* T25 (review2-trust F6): 40 offers from lord 2 fill that lord's own quota of 6 and must never evict another
         * lord's offer: lord 2 can still answer our own offer from lord 1 */
        SOCKET jk = INVALID_SOCKET;
        DWORD idk = 0, k, nd, ans = 0, froms[64];
        BYTE m02[61], o[24], g[64], types[64];
        int before, after;
        hello(proto, 2, info, &jk);
        idk = make_player(jk, "quota");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idk, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        memset(o, 0, sizeof o);   /* our own 0x49: lord 1 offers lord 2 500 crowns */
        o[0] = 0x49; o[1] = 20; *(DWORD *)(o + 4) = 1; *(DWORD *)(o + 8) = 2; *(DWORD *)(o + 16) = 500;
        ((SEND)dp->vt[26])(dp, me, 0, 0, o, 24);
        Sleep(200);
        for (k = 0; k < 40; k++) {   /* lord 2 floods its own quota with 40 honest-looking offers to lord 1 */
            memset(g, 0, sizeof g);
            g[0] = 0x49; g[1] = 20; g[3] = (BYTE)(1 + k % 39);   /* honest seqs are 1..39 (R7-5, R8-6 drop 40 and up) */
            *(DWORD *)(g + 4) = 2; *(DWORD *)(g + 8) = 1; *(DWORD *)(g + 16) = k + 1;
            send_data(jk, idk, 0, g, 24);
        }
        /* T18 and T22 have already put this exact line in the log, so the check is scoped to this test's window */
        before = log_count("0x4a for lord 1 answers no offer from that lord to lord 2");
        memset(g, 0, sizeof g); g[0] = 0x4a; g[1] = 20; g[3] = 39;
        *(DWORD *)(g + 8) = 2; *(DWORD *)(g + 12) = 1; *(DWORD *)(g + 16) = 500;
        send_data(jk, idk, 0, g, 24);
        nd = drain_host(dp, froms, types, 64, 800);
        for (k = 0; k < nd && k < 64; k++) if (froms[k] == idk && types[k] == 0x4a) ans++;
        Sleep(300);
        after = log_count("0x4a for lord 1 answers no offer from that lord to lord 2");
        sprintf(buf, "player %08lx: the 0x4a %s, \"no offer\" lines %d -> %d", idk, ans ? "was delivered" : "was NOT delivered", before, after);
        result("T25 offer flood does not evict other lords", idk && ans && me && after == before, buf);
        closesocket(jk);
        Sleep(400);
    }

    {   /* T26 (review3 R3-5): the game's length byte d[1] is the payload size; a message that disagrees is dropped */
        SOCKET jl = INVALID_SOCKET;
        DWORD idl = 0, k, nd, n1e = 0, froms[64];
        BYTE m02[61], g[64], types[64];
        int bad;
        hello(proto, 2, info, &jl);
        idl = make_player(jl, "lenbyte");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idl, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        memset(g, 0, sizeof g); g[0] = 0x1e; g[1] = 40; g[7] = 2; send_data(jl, idl, 0, g, 9);    /* says 40, has 5 */
        memset(g, 0, sizeof g); g[0] = 0x1e; g[1] = 5; g[7] = 2; send_data(jl, idl, 0, g, 9);     /* honest */
        bad = log_has("type 0x1e length byte says 40 payload bytes, the message has 5", 3000);
        nd = drain_host(dp, froms, types, 64, 800);
        for (k = 0; k < nd && k < 64; k++) if (froms[k] == idl && types[k] == 0x1e) n1e++;
        sprintf(buf, "player %08lx: bad length %s, 0x1e delivered %lu (want 1)", idl, bad ? "dropped" : "NOT dropped", n1e);
        result("T26 length byte checked", idl && bad && n1e == 1, buf);
        closesocket(jl);
        Sleep(400);
    }

    {   /* T27 (review3 R3-3): the execution tick, heartbeats, commands in flight and 0x5a, against SimTick 100000 */
        SOCKET jm = INVALID_SOCKET;
        DWORD idm = 0, k, nd, n3b = 0, n0a = 0, n5a = 0, froms[256];
        BYTE m02[61], g[64], types[256];
        int tfar, hb, cap, z;
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "100000");
        hello(proto, 2, info, &jm);
        idm = make_player(jm, "ticks");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idm, 0, m02, sizeof m02);   /* the host gives the joiner lord 2 */
        Sleep(300);
        memset(g, 0, sizeof g); g[0] = 0x3b; *(DWORD *)(g + 4) = 100000 + 1600; send_data(jm, idm, 0, g, 8);   /* past lead 50 + 1500 */
        memset(g, 0, sizeof g); g[0] = 0x0a; *(DWORD *)(g + 4) = 102000; g[7] = 2; send_data(jm, idm, 0, g, 8);  /* heartbeat +2000 */
        memset(g, 0, sizeof g); g[0] = 0x0a; *(DWORD *)(g + 4) = 100005; g[7] = 2; send_data(jm, idm, 0, g, 8);  /* honest heartbeat */
        for (k = 0; k < 70; k++) {   /* 70 commands due at +100: 64 fit */
            memset(g, 0, sizeof g); g[0] = 0x3b; g[3] = (BYTE)(1 + k % 39); *(DWORD *)(g + 4) = 100100 + k; send_data(jm, idm, 0, g, 8);
        }
        memset(g, 0, sizeof g); g[0] = 0x5a; g[3] = 39; send_data(jm, idm, 0, g, 4);   /* to every peer: never honest */
        for (k = 0; k < 15; k++) { memset(g, 0, sizeof g); g[0] = 0x5a; g[3] = (BYTE)(1 + k % 39); send_data(jm, idm, me, g, 4); }
        tfar = log_has("type 0x3b runs at tick +1600 from the host's SimTick 100000", 3000);
        hb = log_has("heartbeat 0x0a tick +2000 from the host's SimTick 100000", 3000);
        cap = log_has("type 0x3b: 64 commands from this player not yet due (cap 64)", 3000);
        z = log_has("0x5a (raises every peer's command lead) sent to every peer", 3000) && log_has("0x5a (raises a peer's command lead) over 1 per 10 s", 3000);
        nd = drain_host(dp, froms, types, 256, 800);
        for (k = 0; k < nd && k < 256; k++)
            if (froms[k] == idm) { if (types[k] == 0x3b) n3b++; else if (types[k] == 0x0a) n0a++; else if (types[k] == 0x5a) n5a++; }
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", NULL);
        sprintf(buf, "player %08lx: far %s, heartbeat %s, cap %s, 0x5a %s; delivered 0x3b %lu (want 64), 0x0a %lu (want 1), 0x5a %lu (want 4)",
                idm, tfar ? "dropped" : "NOT dropped", hb ? "dropped" : "NOT dropped", cap ? "hit" : "NOT hit", z ? "cut" : "NOT cut", n3b, n0a, n5a);
        result("T27 tick window and in-flight cap", idm && tfar && hb && cap && z && n3b == 64 && n0a == 1 && n5a == 4, buf);
        closesocket(jm);
        Sleep(400);
    }

    {   /* T28: SimTick resets at phase changes (each peer at its own moment), late commands, commands at one tick */
        SOCKET jr = INVALID_SOCKET;
        DWORD idr = 0, k, nd, n3b = 0, n0a = 0, froms[256];
        BYTE m02[61], g[64], types[256];
        int rs, hj, cj, lt, pt, sq = 0;
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "100000");
        hello(proto, 2, info, &jr);
        idr = make_player(jr, "resets");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 3;
        ((SEND)dp->vt[26])(dp, me, idr, 0, m02, sizeof m02);   /* lord 3 */
        Sleep(300);
#define HB28(v) do { memset(g, 0, sizeof g); g[0] = 0x0a; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); g[7] = 3; send_data(jr, idr, 0, g, 8); } while (0)
#define CM28(v) do { memset(g, 0, sizeof g); g[0] = 0x3b; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); send_data(jr, idr, 0, g, 8); } while (0)
        HB28(7);          /* this player reset first: behind passes */
        HB28(100010);
        Sleep(300);
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "20");   /* the host resets; this player has not yet */
        HB28(100020);     /* old clock, inside the anchor: pass */
        HB28(160000);     /* jump: drop */
        CM28(100060);     /* old clock: pass */
        CM28(110000);     /* past the anchored window: drop */
        HB28(5);          /* reset: pass */
        for (k = 0; k < 40; k++) CM28(0);     /* R4-2, R5-4: late, all run at tick 21: 32 take a late token, 24 of them pass the per-tick cap */
        for (k = 0; k < 27; k++) CM28(100);   /* R4-4, R7-1: one tick, 24 pass */
        for (k = 1; k <= 50; k++) HB28(100020 + 300 * k);   /* R4-1 ramp, last (its drops pass the 50-line log cap): at most 1-2 pass */
#undef HB28
#undef CM28
        rs = log_has("TRUST host SimTick reset 100000 -> 20", 3000);
        hj = log_has("heartbeat 0x0a tick +159980 from the host's SimTick 20", 3000);
        cj = log_has("type 0x3b runs at tick +109980 from the host's SimTick 20", 3000);
        lt = log_has("over 32 late commands", 3000);
        pt = log_has("already has 24 from this player", 3000);
        nd = drain_host(dp, froms, types, 256, 800);
        for (k = 0; k < nd && k < 256; k++)
            if (froms[k] == idr) { if (types[k] == 0x3b) n3b++; else if (types[k] == 0x0a) n0a++; }
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", NULL);
        sprintf(buf, "player %08lx: reset %s, jump %s, far command %s, late cap %s, tick cap %s; delivered 0x0a %lu (want 5-6), 0x3b %lu (want 49)",
                idr, rs ? "seen" : "NOT seen", hj ? "dropped" : "NOT dropped", cj ? "dropped" : "NOT dropped", lt ? "hit" : "NOT hit",
                pt ? "hit" : "NOT hit", n0a, n3b);
        result("T28 SimTick resets, late and same-tick caps", idr && rs && hj && cj && lt && pt && n0a >= 5 && n0a <= 6 && n3b == 49, buf);
        closesocket(jr);
        Sleep(400);
    }

    {   /* T29 (review5, review6): a stale heartbeat slot rewritten, lead per player, 0x5b, host standing still */
        SOCKET jp = INVALID_SOCKET, jh = INVALID_SOCKET;
        DWORD idp = 0, idh = 0, k, nd, froms[256], hp0a = 0, h3b = 0, h0a = 0, p5b = 0;
        BYTE m02[61], g[64], types[256];
        char sv[16];
        int fix, stale, far1, once, b5, mv, sq = 0;
        BYTE m5a[4];
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "200000");
        hello(proto, 2, info, &jp);
        idp = make_player(jp, "stale");
        hello(proto, 2, info, &jh);
        idh = make_player(jh, "honest");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, idp, 0, m02, sizeof m02);   /* lord 2 */
        *(DWORD *)(m02 + 4 + 40) = 4;
        ((SEND)dp->vt[26])(dp, me, idh, 0, m02, sizeof m02);   /* lord 4 */
        Sleep(300);
#define HB29(s, id, l, v) do { memset(g, 0, sizeof g); g[0] = 0x0a; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); g[7] = (l); send_data(s, id, 0, g, 8); } while (0)
#define CM29(s, id, v) do { memset(g, 0, sizeof g); g[0] = 0x3b; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); send_data(s, id, 0, g, 8); } while (0)
        HB29(jp, idp, 2, 200010);
        Sleep(300);   /* the DLL reads the test SimTick on its own thread: let it process before the value changes */
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "30");   /* the host resets */
        HB29(jp, idp, 2, 200300);   /* old clock, in the window: passes, then this player goes silent */
        for (k = 0; k <= 10; k++) {   /* the host rewrites the silent slot 3 s in, inside the window (R8-1) */
            sprintf(sv, "%lu", 30 + 100 * k); SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", sv);
            HB29(jh, idh, 4, 30 + 100 * k);
            Sleep(1000);
        }
        CM29(jh, idh, 200300 + 50 + 100);         /* the stale slot is rewritten first (R6-1): drops */
        CM29(jh, idh, 1030 + 50 + 600 + 5);       /* past the highest heartbeat + lead + 600: drops */
        Sleep(300);   /* two sockets: let the host take these before the other player's 0x5a */
        for (k = 0; k < 4; k++) { memset(g, 0, sizeof g); g[0] = 0x5a; g[3] = (BYTE)(1 + sq++ % 39); send_data(jp, idp, idh, g, 4); }
        for (k = 0; k < 3; k++) {                 /* the host's own late report, as net_emit_command sends it (R6-4) */
            m5a[0] = 0x5a; m5a[1] = 0; m5a[2] = 0; m5a[3] = (BYTE)(7 | (k == 1 ? 0x40 : k == 2 ? 0x80 : 0));
            ((SEND)dp->vt[26])(dp, me, idh, 0, m5a, 4);
        }
        Sleep(300);
        CM29(jh, idh, 1030 + 50 + 600 + 9);       /* lead 50 + 8 + 2 now: passes */
        CM29(jh, idh, 1030 + 50 + 600 + 11);      /* over lead 60 (64 if the host's copies counted): drops */
        for (k = 0; k < 3; k++) { memset(g, 0, sizeof g); g[0] = 0x5b; g[3] = (BYTE)(1 + sq++ % 39); send_data(jp, idp, 0, g, 4); }
        Sleep(300);
        HB29(jh, idh, 4, 1030 + 1000);            /* the host's SimTick has stood still 2 s: still drops (R6-2) */
        memset(g, 0, sizeof g); g[0] = 0x3b; g[1] = 4; g[3] = 0x30; *(DWORD *)(g + 4) = 1030 + 60; send_data(jh, idh, 0, g, 8);   /* seq 48 (R7-5): drops */
#undef HB29
#undef CM29
        fix = log_has("lord 2's heartbeat slot held at 200300", 3000) && log_has("host SimTick 330: rewritten to 0 on every peer", 500);   /* inside the window (R9-4) */
        stale = log_has("type 0x3b runs at tick +199420 from the host's SimTick 1030 (highest heartbeat 1030), over lead 50 + 600", 3000);
        far1 = log_has("type 0x3b runs at tick +655 from the host's SimTick 1030 (highest heartbeat 1030), over lead 50 + 600", 3000);
        once = log_has("type 0x3b runs at tick +661 from the host's SimTick 1030 (highest heartbeat 1030), over lead 60 + 600", 3000);
        b5 = log_has("0x5b (clears every peer's commands) over 1 per 10 s", 3000);
        mv = log_has("heartbeat 0x0a tick +1000 from the host's SimTick 1030", 3000) && log_has("type 0x3b seq 48: the game sends 1..39", 3000);
        nd = drain_host(dp, froms, types, 256, 800);
        for (k = 0; k < nd && k < 256; k++) {
            if (froms[k] == idp) { if (types[k] == 0x0a) hp0a++; else if (types[k] == 0x5b) p5b++; }
            if (froms[k] == idh) { if (types[k] == 0x3b) h3b++; else if (types[k] == 0x0a) h0a++; }
        }
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", NULL);
        sprintf(buf, "players %08lx %08lx: slot fix %s, from-stale %s, past-bound %s, host 0x5a %s, 0x5b cap %s, still-host heartbeat %s; delivered stale 0x0a %lu (want 3, one ours), 0x5b %lu (want 2), honest 0x3b %lu (want 1), 0x0a %lu (want 11)",
                idp, idh, fix ? "seen" : "NOT seen", stale ? "dropped" : "NOT dropped", far1 ? "dropped" : "NOT dropped", once ? "counted once" : "NOT counted once",
                b5 ? "hit" : "NOT hit", mv ? "dropped" : "NOT dropped", hp0a, p5b, h3b, h0a);
        result("T29 stale slot rewritten, lead per player, 0x5b, host standing still", idp && idh && fix && stale && far1 && once && b5 && mv && hp0a == 3 && p5b == 2 && h3b == 1 && h0a == 11, buf);
        closesocket(jp);
        closesocket(jh);
        Sleep(400);
    }

    {   /* T11: a joiner that never creates a player is dropped after 10 s (it must not hold a lobby slot) */
        SOCKET j = INVALID_SOCKET;
        DWORD to = 45000, t11;
        char c;
        int r;
        ty = hello(proto, 2, info, &j);
        setsockopt(j, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
        t11 = GetTickCount();
        do r = recv(j, &c, 1, 0); while (r > 0);      /* no player: the host sends nothing, then closes */
        t11 = GetTickCount() - t11;
        sprintf(buf, "join type=%d, closed after %lu ms", ty, t11);
        result("T11 idle joiner dropped", ty == 2 && t11 > 8000 && t11 < 15000, buf);
        closesocket(j);
    }

    {   /* T30 (review8): the host gates nobody -> bound from the slowest other heartbeat; the reset window has no time term */
        SOCKET ja = INVALID_SOCKET, jb = INVALID_SOCKET;
        DWORD ida = 0, idb = 0, k, nd, na = 0, froms[256];
        BYTE m02[61], g[64], types[256];
        int rt, ng, ow, sq = 0;
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "5000");   /* above T29's last 1030: not a reset */
        hello(proto, 2, info, &ja);
        ida = make_player(ja, "gate-a");
        hello(proto, 2, info, &jb);
        idb = make_player(jb, "gate-b");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, ida, 0, m02, sizeof m02);   /* lord 2 */
        *(DWORD *)(m02 + 4 + 40) = 3;
        ((SEND)dp->vt[26])(dp, me, idb, 0, m02, sizeof m02);   /* lord 3 */
        Sleep(300);
#define HB30(s, id, l, v) do { memset(g, 0, sizeof g); g[0] = 0x0a; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); g[7] = (l); send_data(s, id, 0, g, 8); } while (0)
        SetEnvironmentVariableA("LOTR2NET_TEST_NOGATE", "1");
        HB30(ja, ida, 2, 5100);     /* pass */
        HB30(jb, idb, 3, 5400);     /* pass */
        Sleep(100);
        HB30(ja, ida, 2, 5700);     /* 700 past the host, 300 past B: pass */
        HB30(ja, ida, 2, 5900);     /* 500 past B, the slowest other: drop */
        Sleep(100);
        HB30(jb, idb, 3, 6000);     /* 300 past A: pass */
        Sleep(300);
        SetEnvironmentVariableA("LOTR2NET_TEST_NOGATE", NULL);
        closesocket(jb);
        Sleep(400);
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "100000");
        HB30(ja, ida, 2, 100010);   /* pass */
        Sleep(300);
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "20");   /* the host resets */
        HB30(ja, ida, 2, 100440);   /* old clock, within 100000 + 410 + 40: pass */
        HB30(ja, ida, 2, 100460);   /* past it (R8-1: no time term): drop */
#undef HB30
        ng = log_has("heartbeat 0x0a tick +500 from the slowest other heartbeat 5400 (the host gates nobody)", 3000);
        rt = log_has("TRUST host SimTick reset 100000 -> 20", 3000) && log_has("tick 100440, host 20)", 500);   /* T28 logs the same reset line (R9-5) */
        ow = log_has("heartbeat 0x0a tick +100440 from the host's SimTick 20", 3000);
        nd = drain_host(dp, froms, types, 256, 800);
        for (k = 0; k < nd && k < 256; k++) if (froms[k] == ida && types[k] == 0x0a) na++;
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", NULL);
        sprintf(buf, "players %08lx %08lx: slowest-other bound %s, reset %s, old-clock bound %s; A delivered 0x0a %lu (want 4)",
                ida, idb, ng ? "hit" : "NOT hit", rt ? "seen" : "NOT seen", ow ? "hit" : "NOT hit", na);
        result("T30 host gates nobody, reset window bound", ida && idb && ng && rt && ow && na == 4, buf);
        closesocket(ja);
        Sleep(400);
    }

    {   /* T31 (review9): one lord resets first while the host gates nobody; the mpfix bounds */
        SOCKET ja = INVALID_SOCKET, jb = INVALID_SOCKET;
        DWORD ida = 0, idb = 0, k, nd, na = 0, nc = 0, froms[256];
        BYTE m02[61], g[64], types[256];
        int pr, pd, mh, mp, sq = 0;
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", "200000");   /* above T30's last 20: not a reset; past T30's window anchor 100450,
                                                                         so the outcome is the same with T30's window open or closed (R10-1) */
        hello(proto, 2, info, &ja);
        ida = make_player(ja, "skew-a");
        hello(proto, 2, info, &jb);
        idb = make_player(jb, "skew-b");
        memset(m02, 0, sizeof m02);
        m02[0] = 0x02; m02[1] = 57; *(DWORD *)(m02 + 4) = me; *(DWORD *)(m02 + 4 + 36) = 1; *(DWORD *)(m02 + 4 + 40) = 2;
        ((SEND)dp->vt[26])(dp, me, ida, 0, m02, sizeof m02);   /* lord 2 */
        *(DWORD *)(m02 + 4 + 40) = 3;
        ((SEND)dp->vt[26])(dp, me, idb, 0, m02, sizeof m02);   /* lord 3 */
        Sleep(300);
#define HB31(s, id, l, v) do { memset(g, 0, sizeof g); g[0] = 0x0a; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); g[7] = (l); send_data(s, id, 0, g, 8); } while (0)
#define CM31(s, id, v) do { memset(g, 0, sizeof g); g[0] = 0x3b; g[3] = (BYTE)(1 + sq++ % 39); *(DWORD *)(g + 4) = (v); send_data(s, id, 0, g, 8); } while (0)
        SetEnvironmentVariableA("LOTR2NET_TEST_NOGATE", "1");
        HB31(ja, ida, 2, 200100);   /* pass */
        HB31(jb, idb, 3, 200200);     /* pass */
        Sleep(100);
        HB31(jb, idb, 3, 30);       /* B resets first: behind passes */
        Sleep(100);
        HB31(ja, ida, 2, 200500);   /* A on the old clock, 300 past B's pre-reset 200200: pass (R9-1) */
        HB31(ja, ida, 2, 200700); /* 500 past it: drop */
        Sleep(300);
        SetEnvironmentVariableA("LOTR2NET_TEST_NOGATE", NULL);
        SetEnvironmentVariableA("LOTR2NET_TEST_MPFIX", "1");
        HB31(ja, ida, 2, 200100);   /* +100: pass */
        HB31(ja, ida, 2, 200101);   /* +101: drop */
        for (k = 0; k < 7; k++) CM31(ja, ida, 200000 + 50 + 20);   /* 6 pass */
        Sleep(300);
        SetEnvironmentVariableA("LOTR2NET_TEST_MPFIX", NULL);
#undef HB31
#undef CM31
        pr = log_has("heartbeat 0x0a tick +500 from the slowest other heartbeat 200200 (the host gates nobody)", 3000);
        pd = !log_has("tick +200470 from the slowest other heartbeat 30", 300);
        mh = log_has("heartbeat 0x0a tick +101 from the host's SimTick 200000", 3000);
        mp = log_has("tick 200070 already has 6 from this player, 6 from all (caps 6, 20)", 3000);
        nd = drain_host(dp, froms, types, 256, 800);
        for (k = 0; k < nd && k < 256; k++) if (froms[k] == ida) { if (types[k] == 0x0a) na++; else if (types[k] == 0x3b) nc++; }
        SetEnvironmentVariableA("LOTR2NET_TEST_SIMTICK", NULL);
        sprintf(buf, "players %08lx %08lx: pre-reset bound %s, old-clock A %s, mpfix heartbeat %s, mpfix tick cap %s; A delivered 0x0a %lu (want 3), 0x3b %lu (want 6)",
                ida, idb, pr ? "hit" : "NOT hit", pd ? "passed" : "DROPPED", mh ? "dropped" : "NOT dropped", mp ? "hit" : "NOT hit", na, nc);
        result("T31 reset skew with the host gating nobody, mpfix bounds", ida && idb && pr && pd && mh && mp && na == 3 && nc == 6, buf);
        closesocket(ja); closesocket(jb);
        Sleep(400);
    }

    {   /* T13: the local-search beacon answers only a padded L2FIND2 */
        SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        DWORD tmo = 1500;
        char q[72], r[128];
        int k1, k2;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(47700);
        setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
        sendto(u, "L2FIND1", 7, 0, (struct sockaddr *)&a, sizeof a);
        k1 = recv(u, r, sizeof r, 0);
        memset(q, 0, sizeof q);
        memcpy(q, "L2FIND2", 7);
        sendto(u, q, 72, 0, (struct sockaddr *)&a, sizeof a);
        k2 = recv(u, r, sizeof r, 0);
        sprintf(buf, "L2FIND1 -> %d bytes, L2FIND2 -> %d bytes '%.7s'", k1 < 0 ? 0 : k1, k2 < 0 ? 0 : k2, k2 > 0 ? r : "");
        result("T13 beacon answers only L2FIND2", k1 <= 0 && k2 == 72 && !memcmp(r, "L2HERE1", 7), buf);
        closesocket(u);
    }

    t = GetTickCount();
    ((CLOSE)dp->vt[4])(dp);
    sprintf(buf, "%lu ms", GetTickCount() - t);
    result("T12 close waits for its threads, bounded", GetTickCount() - t < 3500, buf);

    {   /* T32 (v10 ghost hub): the host's game leaves a locked game; the hub keeps relaying and names a new game host */
        SOCKET ja = INVALID_SOCKET, jb = INVALID_SOCKET;
        DWORD hp = 0, ida, idb, k;
        NAME_ nm = {sizeof nm, 0, "host32", NULL};
        BYTE fr[300], g[8];
        int ty, a_dest = 0, b_dest = 0, b_data16 = 0, b_datato = 0, b_host = 0, ended, c0, b16drop = 0;
        DWORD a_host = 0, b_host1 = 0;
        HR ho, hs;
        hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
        ((CPL)dp->vt[6])(dp, &hp, &nm, NULL, NULL, 0, 0);
        hello(proto, 2, info, &ja); ida = make_player(ja, "ghostA");
        hello(proto, 2, info, &jb); idb = make_player(jb, "ghostB");
        ((SETSD)dp->vt[31])(dp, &sd, 0);                    /* the game locks the session at game start */
        Sleep(200);
        ((DPL)dp->vt[9])(dp, hp);                           /* the host's lord is gone: its game leaves */
        ((CLOSE)dp->vt[4])(dp);
        for (k = 0; k < 12 && !a_host; k++) {               /* A: T_SYS_DESTROY(host player), then T_HOST(A) */
            ty = readframe(ja, fr, sizeof fr);
            if (!ty) break;
            if (ty == 7 && *(DWORD *)fr == hp) a_dest = 1;
            if (ty == 12) a_host = *(DWORD *)fr;
        }
        for (k = 0; k < 12 && !b_host1; k++) {
            ty = readframe(jb, fr, sizeof fr);
            if (!ty) break;
            if (ty == 7 && *(DWORD *)fr == hp) b_dest = 1;
            if (ty == 12) b_host1 = *(DWORD *)fr;
        }
        memset(g, 0, sizeof g); g[0] = 0x16; send_data(ja, ida, 0, g, 5);      /* host-only type, now A's to send */
        memset(g, 0, sizeof g); g[0] = 0x1e; g[7] = 2; send_data(ja, ida, idb, g, 8);
        for (k = 0; k < 6 && !(b_data16 && b_datato); k++) {
            ty = readframe(jb, fr, sizeof fr);
            if (!ty) break;
            if (ty == 8 && *(DWORD *)fr == ida && *(DWORD *)(fr + 4) == 0 && fr[8] == 0x16) b_data16 = 1;
            if (ty == 8 && *(DWORD *)fr == ida && *(DWORD *)(fr + 4) == idb && fr[8] == 0x1e) b_datato = 1;
        }
        c0 = log_count("type 0x16 only the host sends");
        memset(g, 0, sizeof g); g[0] = 0x16; send_data(jb, idb, 0, g, 5);      /* B is not the game host */
        for (k = 0; k < 30 && !b16drop; k++) { Sleep(100); b16drop = log_count("type 0x16 only the host sends") > c0; }
        ho = ((OPEN)dp->vt[24])(dp, &sd, 2);
        hs = ((SEND)dp->vt[26])(dp, hp, 0, 0, g, 8);
        closesocket(ja);
        for (k = 0; k < 6 && !b_host; k++) {
            ty = readframe(jb, fr, sizeof fr);
            if (!ty) break;
            if (ty == 12 && *(DWORD *)fr == idb) b_host = 1;
        }
        closesocket(jb);
        ended = log_has("GHOST: the last player left - hub closed", 3000);
        sprintf(buf, "A %08lx B %08lx: host player destroyed A %d B %d, T_HOST A %08lx B %08lx, B got 0x16 %d / directed %d, "
                "B's 0x16 dropped %d, Open hr=%08lx Send hr=%08lx, after A left B hosts %d, hub closed %d",
                ida, idb, a_dest, b_dest, a_host, b_host1, b_data16, b_datato, b16drop, (unsigned long)ho, (unsigned long)hs, b_host, ended);
        result("T32 ghost hub keeps the game alive for the joiners", hr == 0 && ida && idb && a_dest && b_dest && a_host == ida
               && b_host1 == ida && b_data16 && b_datato && b16drop && ho != 0 && hs != 0 && b_host && ended, buf);
        Sleep(300);
    }

    {   /* T34 (v10 R12-1): Open on a locked host game without Close must not leave a ghost under the new session */
        SOCKET ja = INVALID_SOCKET;
        DWORD hp = 0, hp2 = 0, ida, k, t0;
        NAME_ nm = {sizeof nm, 0, "host34", NULL};
        BYTE fr[300], g[8];
        int ty, a_closed = 0, g0 = log_count("GHOST: our game left"), g1;
        HR h1, h2, hs;
        h1 = ((OPEN)dp->vt[24])(dp, &sd, 2);
        ((CPL)dp->vt[6])(dp, &hp, &nm, NULL, NULL, 0, 0);
        hello(proto, 2, info, &ja); ida = make_player(ja, "open34");
        ((SETSD)dp->vt[31])(dp, &sd, 0);
        Sleep(200);
        h2 = ((OPEN)dp->vt[24])(dp, &sd, 2);               /* no Close first */
        for (t0 = GetTickCount(), k = 0; k < 20 && GetTickCount() - t0 < 6000; k++) {
            ty = readframe(ja, fr, sizeof fr);
            if (!ty) { a_closed = recv(ja, (char *)fr, 1, 0) <= 0; break; }
        }
        closesocket(ja);
        g1 = log_count("GHOST: our game left");
        ((CPL)dp->vt[6])(dp, &hp2, &nm, NULL, NULL, 0, 0);
        memset(g, 0, sizeof g); g[0] = 0x0a;
        hs = ((SEND)dp->vt[26])(dp, hp2, 0, 0, g, 8);
        sprintf(buf, "open %08lx, A %08lx, re-Open %08lx, A's link closed %d, ghost starts %d -> %d, new player %08lx Send %08lx",
                (unsigned long)h1, ida, (unsigned long)h2, a_closed, g0, g1, hp2, (unsigned long)hs);
        result("T34 Open without Close ends the old game, no ghost", h1 == 0 && ida && h2 == 0 && a_closed && g1 == g0
               && hp2 && hs == 0, buf);
        ((CLOSE)dp->vt[4])(dp);
        Sleep(300);
    }

    {   /* T14: an unreachable host never freezes the caller (the game polls this from its UI loop) */
        DWORD worst = 0, k;
        _putenv("LOTR2NET_HOST=10.255.255.1");
        SetEnvironmentVariableA("LOTR2NET_HOST", "10.255.255.1");
        g_cb = 0;
        for (k = 0; k < 3; k++) {
            DWORD t0 = GetTickCount(), d;
            ((ENUMS)dp->vt[13])(dp, &sd, 1000, cb, NULL, 0);
            if ((d = GetTickCount() - t0) > worst) worst = d;
        }
        sprintf(buf, "3 calls, slowest %lu ms, callbacks %d", worst, g_cb);
        result("T14 enum to a dead address is bounded", worst < 600 && g_cb == 0, buf);
    }

    {   /* T10: joiner side, a fake host floods 4200 messages the game never receives -> SESSIONLOST, not silent drops */
        SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        HANDLE th;
        DWORD from, to, sz, n = 0;
        BYTE rb[300];
        int lost = 0;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(47701);
        bind(l, (struct sockaddr *)&a, sizeof a);
        listen(l, 1);
        g_flooded = CreateEventA(NULL, TRUE, FALSE, NULL);
        th = CreateThread(NULL, 0, fake_host, (LPVOID)l, 0, NULL);
        _putenv("LOTR2NET_HOST=127.0.0.1:47701");
        SetEnvironmentVariableA("LOTR2NET_QWAIT_MS", "1500");   /* v9: the reader waits this long for the game, then ends */
        hr = ((OPEN)dp->vt[24])(dp, &sd, 1);
        WaitForSingleObject(g_flooded, 20000);
        /* never read before the queue overflows (reading would make room): wait for the DLL's own "host lost" log line,
         * 20 s at most (a fresh Wine prefix queues slowly; a fixed 4 s sleep failed T10 about 1 run in 3), then drain */
        log_has("treating it as host lost", 20000);
        Sleep(500);
        for (;;) {
            sz = sizeof rb;
            if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) break;
            n++;
            if (from == 0 && *(DWORD *)rb == 0x31) lost = 1;
        }
        sprintf(buf, "open hr=%08lx, %lu delivered, SESSIONLOST %s", (unsigned long)hr, n, lost ? "seen" : "NOT seen");
        result("T10 joiner overflow = session lost", hr == 0 && lost, buf);
        ((CLOSE)dp->vt[4])(dp);
        WaitForSingleObject(th, 10000);
        closesocket(l);
    }
    {   /* T17 (review F3): a joiner takes a resync download only for a desync it detected itself; this one detected none */
        SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        HANDLE th;
        int refused;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(47702);
        bind(l, (struct sockaddr *)&a, sizeof a);
        listen(l, 1);
        th = CreateThread(NULL, 0, fake_host2, (LPVOID)l, 0, NULL);
        _putenv("LOTR2NET_HOST=127.0.0.1:47702");
        hr = ((OPEN)dp->vt[24])(dp, &sd, 1);
        refused = log_has("RESYNC refused unsolicited download", 5000);
        sprintf(buf, "open hr=%08lx, unsolicited download %s", (unsigned long)hr, refused ? "refused" : "NOT refused");
        result("T17 unsolicited resync download refused", hr == 0 && refused, buf);
        ((CLOSE)dp->vt[4])(dp);
        WaitForSingleObject(th, 10000);
        closesocket(l);
    }
    {   /* T33 (v10 joiner side): T_HOST naming this player hands the game one DPSYS_HOST */
        SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        HANDLE th;
        DWORD id = 0, from, to, sz, nhost = 0, k;
        NAME_ nm = {sizeof nm, 0, "j33", NULL};
        BYTE rb[300];
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(47703);
        bind(l, (struct sockaddr *)&a, sizeof a);
        listen(l, 1);
        th = CreateThread(NULL, 0, fake_host3, (LPVOID)l, 0, NULL);
        _putenv("LOTR2NET_HOST=127.0.0.1:47703");
        hr = ((OPEN)dp->vt[24])(dp, &sd, 1);
        ((CPL)dp->vt[6])(dp, &id, &nm, NULL, NULL, 0, 0);
        log_has("game host is now player 00001002", 3000);
        Sleep(300);
        for (k = 0; k < 50; k++) {
            sz = sizeof rb;
            if (((RECV)dp->vt[25])(dp, &from, &to, 0, rb, &sz)) break;
            if (from == 0 && *(DWORD *)rb == 0x101) nhost++;
        }
        sprintf(buf, "open hr=%08lx, player %08lx, DPSYS_HOST delivered %lu (want 1)", (unsigned long)hr, id, nhost);
        result("T33 joiner named game host gets one DPSYS_HOST", hr == 0 && id == 0x1002 && nhost == 1, buf);
        ((CLOSE)dp->vt[4])(dp);
        WaitForSingleObject(th, 10000);
        closesocket(l);
    }
    if (rl_raw_list(NULL, 0) < 0)   /* bt.sh / a dev box start one; mp_selftest.ps1 does not */
        printf("SKIP T39-T41: no lords2-relay on 127.0.0.1:%d\n", RELAY_PORT);
    else {
    #define J39 "relay39|L2M1:67d6eb0557e113b822e476e439cb0c28"   /* 45 bytes, a Mod Menu marker: listed whole */
    {   /* T39 (v14 relay, host side): the game is listed, a joiner comes in through the relay, the list follows */
            typedef HR (WINAPI *SETSD)(void *, DPSD2 *, DWORD);
            SOCKET js = INVALID_SOCKET;
            NAME_ nm = {sizeof nm, 0, "host39", NULL};
            DWORD me39 = 0, id39 = 0, k;
            BYTE st = 0xff, info[92];
            RawRoom rr;
            int listed = 0, one = 0, two = 0, ty = 0, gone = 0, paired, closed;
            DPSD2 s39 = sd;
            s39.lpszSessionName = J39;
            SetEnvironmentVariableA("LOTR2NET_RELAY", "127.0.0.1:47710");
            _putenv("LOTR2NET_RELAY=127.0.0.1:47710");   /* the DLL reads it with getenv */
            hr = ((OPEN)dp->vt[24])(dp, &s39, 2);
            ((CPL)dp->vt[6])(dp, &me39, &nm, NULL, NULL, 0, 0);
            for (k = 0; k < 30 && !(listed = rl_raw_find(J39, &rr)); k++) Sleep(100);
            for (k = 0; k < 20 && listed && !(one = rl_raw_find(J39, &rr) && rr.players == 1); k++) Sleep(100);
            if (listed) {   /* a joiner through the relay: status 0, then the normal handshake on that socket */
                js = rl_raw(3, &rr.id, 4);
                if (js != INVALID_SOCKET && recv(js, (char *)&st, 1, 0) == 1 && st == 0) {
                    ty = hello_on(js, proto, 2, info);
                    if (ty == 2) id39 = make_player(js, "r39");
                }
            }
            paired = log_has("relay: joiner relay#", 1000) && log_has("'r39' created", 1000);
            for (k = 0; k < 20 && listed && !(two = rl_raw_find(J39, &rr) && rr.players == 2); k++) Sleep(100);
            ((SETSD)dp->vt[31])(dp, &s39, 0);   /* the game locks the session when it starts: no longer listed */
            for (k = 0; k < 20 && !(gone = !rl_raw_find(J39, NULL)); k++) Sleep(100);
            if (js != INVALID_SOCKET) closesocket(js);
            ((CLOSE)dp->vt[4])(dp);
            closed = log_has("relay: session closed - room", 2000);
            SetEnvironmentVariableA("LOTR2NET_RELAY", NULL);
            _putenv("LOTR2NET_RELAY=");
            sprintf(buf, "open hr=%08lx listed %d (proto %lu, want %lu; %d/%d) 1 player %d, relay status %d, hello answer %d, player %08lx, paired+created %d, 2 players %d, locked gone %d, close logged %d",
                    (unsigned long)hr, listed, rr.proto, proto, rr.players, rr.maxpl, one, st, ty, id39, paired, two, gone, closed);
            result("T39 host lists its game on the relay, a relayed joiner plays", hr == 0 && listed && rr.proto == proto && one && st == 0 && ty == 2 && id39 && paired && two && gone && closed, buf);
        }
    #define J40 "Lords2 Session1|L2M1:67d6eb0557e113b822e476e439cb0c28"
    {   /* T40 (v14 relay, joiner side): EnumSessions in list mode shows the relay's rooms in the game's own list */
            DWORD r1 = 0, r2 = 0, k;
            SOCKET h1, h2;
            int i, got1 = -1, got2 = -1, guid_ok = 0;
            char want2[96];
            _putenv("LOTR2NET_RELAY=127.0.0.1:47710");
            SetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", "1");
            h1 = rl_raw_host(proto, 4, J40, &r1);   /* Jen's Mod Menu checks the whole name on the joiner */
            h2 = rl_raw_host(12000, 3, "old40", &r2);
            snprintf(want2, sizeof want2, "old40 [host v12000, yours v%lu]", proto);
            for (k = 0; k < 10 && (got1 < 0 || got2 < 0); k++) {
                g40_n = 0;
                ((ENUMS)dp->vt[13])(dp, &sd, 1000, cb40, NULL, 0);
                got1 = got2 = -1;
                for (i = 0; i < g40_n; i++) {
                    if (!strcmp(g40_name[i], J40)) got1 = i;
                    if (!strcmp(g40_name[i], want2)) got2 = i;
                }
                if (got1 < 0 || got2 < 0) Sleep(200);
            }
            if (got1 >= 0 && got2 >= 0)
                guid_ok = g40_guid[got1].Data1 == r1 && g40_guid[got2].Data1 == r2 && g40_cur[got1] == 1 && g40_max[got1] == 4 && g40_max[got2] == 3;
            SetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", NULL);
            if (h1 != INVALID_SOCKET) closesocket(h1);
            if (h2 != INVALID_SOCKET) closesocket(h2);
            sprintf(buf, "raw hosts %08lx %08lx, sessions %d: the 53-byte name at %d, '%s' at %d, guid/players ok %d",
                    r1, r2, g40_n, got1, want2, got2, guid_ok);
            result("T40 the relay's rooms in the session list", h1 != INVALID_SOCKET && h2 != INVALID_SOCKET && got1 >= 0 && got2 >= 0 && guid_ok, buf);
            _putenv("LOTR2NET_RELAY=");
        }
        {   /* T41 (v14 relay, joiner side): Open(join) with a room from the list joins through the relay; refusals map */
            DWORD r1 = 0, r2 = 0, k, id41 = 0;
            SOCKET h1, h2;
            HANDLE th = NULL;
            HR hr0 = -1, hr_none = -1, hr_locked = -1, hr_norelay = -1;
            NAME_ nm = {sizeof nm, 0, "j41", NULL};
            DPSD2 j = sd;
            BYTE up[2] = {1, 1};
            int found = 0, i, line;
            _putenv("LOTR2NET_RELAY=127.0.0.1:47710");
            SetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", "1");
            h1 = rl_raw_host(proto, 4, "raw41", &r1);
            if (h1 != INVALID_SOCKET) th = CreateThread(NULL, 0, raw_host41, (LPVOID)h1, 0, NULL);
            for (k = 0; k < 10 && !found; k++) {   /* the room as the game gets it: from the session list */
                g40_n = 0;
                ((ENUMS)dp->vt[13])(dp, &sd, 1000, cb40, NULL, 0);
                for (i = 0; i < g40_n; i++) if (!strcmp(g40_name[i], "raw41")) { j.guidInstance = g40_guid[i]; found = 1; }
                if (!found) Sleep(200);
            }
            SetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", NULL);
            if (found) {
                hr0 = ((OPEN)dp->vt[24])(dp, &j, 1);
                if (!hr0) ((CPL)dp->vt[6])(dp, &id41, &nm, NULL, NULL, 0, 0);
            }
            line = log_has("Open(join): paired with room", 1000);
            ((CLOSE)dp->vt[4])(dp);
            if (th) { WaitForSingleObject(th, 10000); CloseHandle(th); }
            if (h1 != INVALID_SOCKET) closesocket(h1);
            j.guidInstance.Data1 = 0x0badf00d;                    /* no such room */
            hr_none = ((OPEN)dp->vt[24])(dp, &j, 1);
            h2 = rl_raw_host(proto, 4, "lock41", &r2);            /* a locked room */
            if (h2 != INVALID_SOCKET) { rl_raw_frame(h2, 3, up, 2); Sleep(300); }
            j.guidInstance.Data1 = r2;
            hr_locked = ((OPEN)dp->vt[24])(dp, &j, 1);
            if (h2 != INVALID_SOCKET) closesocket(h2);
            _putenv("LOTR2NET_RELAY=127.0.0.1:47719");            /* nobody there */
            hr_norelay = ((OPEN)dp->vt[24])(dp, &j, 1);
            _putenv("LOTR2NET_RELAY=");
            sprintf(buf, "room %08lx listed %d, join hr=%08lx relay paired %ld, player %08lx, log line %d; no room %08lx locked %08lx no relay %08lx",
                    r1, found, (unsigned long)hr0, g41_paired, id41, line, (unsigned long)hr_none, (unsigned long)hr_locked, (unsigned long)hr_norelay);
            result("T41 Open(join) of a relay room", found && hr0 == 0 && g41_paired && id41 == 0x1002 && line
                   && hr_none == (HR)0x887700DC && hr_locked == (HR)0x88770014 && hr_norelay == (HR)0x887700AA, buf);
        }
    }
    {   /* T38 (v13 joiner side): the session clock follows the host's answers */
        SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        HANDLE th;
        DWORD id = 0, ms = 0, want = 0, k;
        long err = 99999;
        int before, have = 0, line;
        NAME_ nm = {sizeof nm, 0, "j38", NULL};
        typedef int (WINAPI *CLOCK)(DWORD *);
        CLOCK clk = (CLOCK)GetProcAddress(m, "LF_NetClock");
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(47704);
        bind(l, (struct sockaddr *)&a, sizeof a);
        listen(l, 1);
        SetEnvironmentVariableA("LOTR2NET_TEST_DIAG", "1");
        g38_t0 = GetTickCount();
        th = CreateThread(NULL, 0, fake_host_clk, (LPVOID)l, 0, NULL);
        _putenv("LOTR2NET_HOST=127.0.0.1:47704");
        hr = ((OPEN)dp->vt[24])(dp, &sd, 1);
        ((CPL)dp->vt[6])(dp, &id, &nm, NULL, NULL, 0, 0);
        before = clk && clk(&ms);   /* no clock until the host answers */
        for (k = 0; k < 50 && !have; k++) { Sleep(100); have = clk && clk(&ms); }
        if (have) {
            Sleep(300);
            clk(&ms);
            want = 5000 + (GetTickCount() - g38_t0);
            err = (long)ms - (long)want;
        }
        line = log_has("session clock: set from the host's answer", 1000) && log_has("[S ", 500);
        ((CLOSE)dp->vt[4])(dp);
        SetEnvironmentVariableA("LOTR2NET_TEST_DIAG", NULL);
        WaitForSingleObject(th, 10000);
        closesocket(l);
        sprintf(buf, "open hr=%08lx player %08lx, clock before an answer %d, host answers %ld, clock %d: %lu ms (want %lu, off %ld), log line %d",
                (unsigned long)hr, id, before, g38_answers, have, ms, want, err, line);
        result("T38 joiner takes the host's session clock", hr == 0 && !before && g38_answers > 0 && have && err > -150 && err < 150 && line, buf);
    }
    {   /* T36 (v12): session folders. Last: from here on dplayx.log is not beside the exe, so log_has finds nothing */
        SOCKET ja = INVALID_SOCKET;
        NAME_ nm = {sizeof nm, 0, "host36", NULL};
        DWORD me36 = 0;
        char ep[MAX_PATH], *sl, pat[MAX_PATH + 32], lg[MAX_PATH + 96], first[MAX_PATH] = "";
        int empty, one, moved, kept = 0, grew = 0, rootgone, two, k, oldleft, firstleft, dirok;
        typedef int (WINAPI *LOGDIR)(char *, int);
        LOGDIR logdir = (LOGDIR)GetProcAddress(m, "LF_NetLogSession");
        char ld[MAX_PATH + 96] = "", ld2[MAX_PATH + 96] = "";
        int solo, closed, summ = 0;
        WIN32_FIND_DATAA fd;
        HANDLE fh;
        GetModuleFileNameA(NULL, ep, MAX_PATH);
        sl = strrchr(ep, '\\');
        if (sl) sl[1] = 0; else ep[0] = 0;
        #define COUNT_DIRS(out) do { out = 0; fh = FindFirstFileA(pat, &fd); if (fh != INVALID_HANDLE_VALUE) { do { if (!first[0]) lstrcpyA(first, fd.cFileName); out++; } while (FindNextFileA(fh, &fd)); FindClose(fh); } } while (0)
        sprintf(pat, "%slords2hd-mp\\20*", ep);
        SetEnvironmentVariableA("LOTR2NET_TEST_MPLOGS", "1");
        hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
        ((CPL)dp->vt[6])(dp, &me36, &nm, NULL, NULL, 0, 0);
        Sleep(300);
        COUNT_DIRS(empty);
        hello(proto, 2, info, &ja);
        make_player(ja, "a36");
        Sleep(400);
        COUNT_DIRS(one);
        sprintf(lg, "%slords2hd-mp\\%s\\dplayx.log", ep, first);
        moved = GetFileAttributesA(lg) != INVALID_FILE_ATTRIBUTES;
        sprintf(pat, "%sdplayx.log", ep);
        rootgone = GetFileAttributesA(pat) == INVALID_FILE_ATTRIBUTES;
        closesocket(ja);
        Sleep(500);          /* its "conn dropped" line must land in the moved file */
        {
            FILE *lf = fopen(lg, "r");
            char ln[600];
            if (lf) {
                if (fgets(ln, sizeof ln, lf) && strstr(ln, " loaded (pid ")) kept = 1;   /* the moved lines are still its start */
                while (fgets(ln, sizeof ln, lf)) if (strstr(ln, "reader done")) grew = 1;
                fclose(lf);
            }
        }
        ((CLOSE)dp->vt[4])(dp);
        {   /* v13: Close appended the session summary to session.txt */
            FILE *sf;
            char ln[300];
            sprintf(lg, "%slords2hd-mp\\%s\\session.txt", ep, first);
            if ((sf = fopen(lg, "r")) != NULL) {
                while (fgets(ln, sizeof ln, sf)) {
                    if (strstr(ln, "session summary (this PC: host")) summ |= 1;
                    if (strstr(ln, "ended: S ") && strstr(ln, "the game closed the session")) summ |= 2;
                    if (strstr(ln, "'a36'")) summ |= 4;
                    if (strstr(ln, "sync checks: 0 ok, 0 desync(s), 0 resync(s)")) summ |= 8;
                }
                fclose(sf);
            }
        }
        Sleep(1100);         /* the folder name has one-second steps */
        for (k = 0; k < 12; k++) {   /* older sessions, one file each: pruned before the next folder is made */
            FILE *of;
            sprintf(lg, "%slords2hd-mp\\2000-01-01_00-00-%02d", ep, k);
            CreateDirectoryA(lg, NULL);
            lstrcatA(lg, "\\dplayx.log");
            if ((of = fopen(lg, "w")) != NULL) { fputs("old\n", of); fclose(of); }
        }
        hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
        ((CPL)dp->vt[6])(dp, &me36, &nm, NULL, NULL, 0, 0);
        hello(proto, 2, info, &ja);
        make_player(ja, "b36");
        Sleep(400);
        sprintf(pat, "%slords2hd-mp\\20*", ep);
        COUNT_DIRS(two);
        sprintf(pat, "%slords2hd-mp\\2000-*", ep);
        COUNT_DIRS(oldleft);
        sprintf(lg, "%slords2hd-mp\\%s", ep, first);
        firstleft = GetFileAttributesA(lg) != INVALID_FILE_ATTRIBUTES;
        if (logdir) logdir(ld, sizeof ld);
        sprintf(lg, "%sdplayx.log", ld);
        dirok = strstr(ld, "lords2hd-mp\\") && !strstr(ld, "\\2000-") && !strstr(ld, first) && GetFileAttributesA(lg) != INVALID_FILE_ATTRIBUTES;
        closesocket(ja);
        ((CLOSE)dp->vt[4])(dp);
        /* a host playing only computer lords: no joiner, the HD ddraw asks at the game's start */
        hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
        ((CPL)dp->vt[6])(dp, &me36, &nm, NULL, NULL, 0, 0);
        solo = logdir && logdir(ld2, sizeof ld2) > 0 && strstr(ld2, "lords2hd-mp\\") && strcmp(ld2, ld);
        ((CLOSE)dp->vt[4])(dp);
        closed = logdir && logdir(ld2, sizeof ld2) == 0;
        SetEnvironmentVariableA("LOTR2NET_TEST_MPLOGS", NULL);
        sprintf(buf, "no joiner %d folders, joined %d, '%.40s' moved %d start kept %d goes on %d, gone from exe folder %d, next session: %d folders (want 10), old left %d (want 8), first kept %d, LF_NetLogSession '%.60s' ok %d, host alone %d, after Close none %d, summary %d (want 15)",
                empty, one, first, moved, kept, grew, rootgone, two, oldleft, firstleft, ld, dirok, solo, closed, summ);
        result("T36 session folders", empty == 0 && one == 1 && moved && kept && grew && rootgone && two == 10 && oldleft == 8 && firstleft && dirok && solo && closed && summ == 15, buf);
    }
    {   /* T43 (rules.c): the host decides the game's fixes: its want & every player's can, 0 when anyone is not the same
         * patch; settled before the game starts and frozen at its start message. Then the joiner side against a host with
         * the marker */
        typedef void (WINAPI *RSET)(const char *, DWORD, DWORD);
        typedef int (WINAPI *RGET)(DWORD *);
        RSET rset = (RSET)GetProcAddress(m, "LF_NetRulesSet");
        RGET rget = (RGET)GetProcAddress(m, "LF_NetRulesGet");
        NAME_ nm = {sizeof nm, 0, "host43", NULL};
        DWORD me43 = 0, alone = 0, a1 = 0, b1 = 0, b2 = 0, c1 = 0, c2 = 0, w1 = 0, s1 = 0, s1lk = 0, d1 = 0, l1 = 0, l1lk = 0, lk = 0, after = 0, j1 = 0, j2 = 0, id43 = 0, hostv = 0;
        int have_alone = 0, len = 0, mark = 0, ty, none = 0, jhave = 0, sdata = 0, k43;
        SOCKET ja = INVALID_SOCKET, jb = INVALID_SOCKET, jc = INVALID_SOCKET;
        BYTE ib[128];
        char *oldhost = getenv("LOTR2NET_HOST"), keephost[160] = "";
        if (oldhost) snprintf(keephost, sizeof keephost, "LOTR2NET_HOST=%s", oldhost);
        if (!rset || !rget) result("T43 the host decides the game's fixes", 0, "LF_NetRulesSet / LF_NetRulesGet not exported");
        else {
            ((CLOSE)dp->vt[4])(dp);
            none = rget(&hostv) == 0;                                  /* no session: no rules */
            rset("L2HD test 1", 0x0f, 0x0b);                           /* can 1111, wants 1011 */
            hr = ((OPEN)dp->vt[24])(dp, &sd, 2);
            ((CPL)dp->vt[6])(dp, &me43, &nm, NULL, NULL, 0, 0);
            have_alone = rget(&alone);                                 /* the host alone: 0x0b */
            ty = hello_len(proto, ib, sizeof ib, &len, &ja);           /* A: this patch, can 0111 -> 0011 */
            mark = ty == 2 && len == 100 && !memcmp(ib + 92, "L2RU", 4) && *(DWORD *)(ib + 96) == 1;
            send_caps(ja, "L2HD test 1", 0x07);
            make_player(ja, "a43");
            a1 = next_rules(ja);
            hello_len(proto, ib, sizeof ib, &len, &jb);                /* B: an older dplayx, no T_CAPS -> 0 for everyone */
            make_player(jb, "b43");
            b1 = next_rules(ja);
            closesocket(jb);                                           /* B leaves before the start -> 0011 again */
            b2 = next_rules(ja);
            hello_len(proto, ib, sizeof ib, &len, &jc);                /* C: another build -> 0, and back once it leaves */
            send_caps(jc, "L2HD test 2", 0x0f);
            make_player(jc, "c43");
            c1 = next_rules(ja);
            closesocket(jc);
            c2 = next_rules(ja);
            rset("L2HD test 1", 0x0f, 0x09);                           /* the host's settings before the start: 1001 & 0111 */
            w1 = next_rules(ja);
            {   /* the game's start message (type 0x04, 12 bytes): frozen, sent once more with locked = 1 ahead of it */
                BYTE st[12] = {0x04, 0x08, 0x2f, 0x22}, fb[300];
                ((SEND)dp->vt[26])(dp, me43, 0, 0, st, sizeof st);
                g43_locked = 0;
                s1 = next_rules(ja);
                s1lk = g43_locked;
                sdata = readframe(ja, fb, sizeof fb) == 8 && fb[8] == 0x04;   /* T_DATA: from, to, the message */
                rset("L2HD test 1", 0x0f, 0x0f);                       /* settings after the start change nothing */
                ((SETSD)dp->vt[31])(dp, &sd, 0);
                rget(&lk);
                closesocket(ja);                                       /* A leaves after the start: nothing changes */
                Sleep(1000);                                           /* the host has seen it go (not from the log: see below) */
                rget(&after);
            }
            ((CLOSE)dp->vt[4])(dp);
            {   /* the backup: the session locks with no start message seen; frozen there, sent with locked = 1 */
                SOCKET jd = INVALID_SOCKET;
                DWORD me2 = 0;
                NAME_ n2 = {sizeof n2, 0, "host43b", NULL};
                rset("L2HD test 1", 0x0f, 0x0b);
                ((OPEN)dp->vt[24])(dp, &sd, 2);
                ((CPL)dp->vt[6])(dp, &me2, &n2, NULL, NULL, 0, 0);
                hello_len(proto, ib, sizeof ib, &len, &jd);
                send_caps(jd, "L2HD test 1", 0x0f);
                make_player(jd, "d43");
                d1 = next_rules(jd);
                g43_locked = 0;
                ((SETSD)dp->vt[31])(dp, &sd, 0);
                l1 = next_rules(jd);
                l1lk = g43_locked;
                closesocket(jd);
                ((CLOSE)dp->vt[4])(dp);
            }
            {   /* joiner side: our T_CAPS goes to a host with the marker, its T_RULES comes back */
                SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in a;
                HANDLE th;
                NAME_ nj = {sizeof nj, 0, "j43", NULL};
                memset(&a, 0, sizeof a);
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = inet_addr("127.0.0.1");
                a.sin_port = htons(47704);
                bind(l, (struct sockaddr *)&a, sizeof a);
                listen(l, 1);
                th = CreateThread(NULL, 0, fake_host43, (LPVOID)l, 0, NULL);
                _putenv("LOTR2NET_HOST=127.0.0.1:47704");
                rset("L2HD test 1", 0x06, 0x00);
                ((OPEN)dp->vt[24])(dp, &sd, 1);
                ((CPL)dp->vt[6])(dp, &id43, &nj, NULL, NULL, 0, 0);
                /* polled, not from the log: after T36 the log is in a session folder */
                for (k43 = 0; k43 < 30 && !(jhave = rget(&j1)); k43++) Sleep(50);
                for (k43 = 0; k43 < 80 && rget(&j2) && j2 != 0x07; k43++) Sleep(50);
                Sleep(500);                                            /* the late 0x01 is in by now */
                rget(&j2);
                ((CLOSE)dp->vt[4])(dp);
                WaitForSingleObject(th, 10000);
                closesocket(l);
                if (keephost[0]) _putenv(keephost); else _putenv("LOTR2NET_HOST=");
            }
            rset(NULL, 0, 0);
            sprintf(buf, "no session %d; host alone %d %02lx (want 0b); INFO marker %d; A %02lx (03), old joiner B %02lx (00), B gone %02lx (03), other build C %02lx (00), C gone %02lx (03), host settings %02lx (01); start: rules %02lx locked %lu then the message %d; locked %02lx and after A left %02lx (01); lock only: %02lx then %02lx locked %lu (0b, 0b 1); joiner: caps '%s' %02lx (06), rules %d %02lx (05), for the game %02lx (07)",
                    none, have_alone, alone, mark, a1, b1, b2, c1, c2, w1, s1, (unsigned long)s1lk, sdata, lk, after, d1, l1, (unsigned long)l1lk, g43_id, g43_can, jhave, j1, j2);
            result("T43 the host decides the game's fixes", none && hr == 0 && have_alone && alone == 0x0b && mark && a1 == 0x03 && b1 == 0 && b2 == 0x03 &&
                   c1 == 0 && c2 == 0x03 && w1 == 0x01 && s1 == 0x01 && s1lk == 1 && sdata && lk == 0x01 && after == 0x01 && d1 == 0x0b && l1 == 0x0b && l1lk == 1 &&
                   !strcmp(g43_id, "L2HD test 1") && g43_can == 0x06 && jhave && j1 == 0x05 && j2 == 0x07, buf);
        }
    }
    if (rl_raw_list(NULL, 0) < 0) printf("SKIP T42: no lords2-relay on 127.0.0.1:%d\n", RELAY_PORT);
    else {   /* T42 (v14): inside GameRanger the relay is never used. Last: GameRanger.dll stays loaded from here on */
        char sys[MAX_PATH], gr[MAX_PATH];
        HMODULE g;
        NAME_ nm = {sizeof nm, 0, "host42", NULL};
        DWORD me42 = 0, k;
        DPSD2 s42 = sd;
        int listed = 0, got = 0;
        GetSystemDirectoryA(sys, sizeof sys);
        lstrcatA(sys, "\\version.dll");
        GetFullPathNameA("GameRanger.dll", sizeof gr, gr, NULL);
        CopyFileA(sys, gr, FALSE);
        g = LoadLibraryA(gr);   /* any module of that name is what dplayx looks for */
        if (!g || !GetModuleHandleA("GameRanger.dll")) printf("SKIP T42: no stand-in GameRanger.dll (%lu)\n", GetLastError());
        else {
            _putenv("LOTR2NET_RELAY=127.0.0.1:47710");
            SetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", "1");
            s42.lpszSessionName = "gr42";
            hr = ((OPEN)dp->vt[24])(dp, &s42, 2);
            ((CPL)dp->vt[6])(dp, &me42, &nm, NULL, NULL, 0, 0);
            for (k = 0; k < 15 && !listed; k++) { Sleep(100); listed = rl_raw_find("gr42", NULL); }
            g40_n = 0;
            ((ENUMS)dp->vt[13])(dp, &sd, 1000, cb40, NULL, 0);   /* list mode is off inside GameRanger */
            for (k = 0; k < (DWORD)g40_n; k++) if (g40_guid[k].Data2 == 0x4c32) got++;
            ((CLOSE)dp->vt[4])(dp);
            SetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", NULL);
            _putenv("LOTR2NET_RELAY=");
            sprintf(buf, "open hr=%08lx, listed on the relay %d (want 0), relay rooms in the session list %d (want 0)", (unsigned long)hr, listed, got);
            result("T42 inside GameRanger the relay is never used", hr == 0 && !listed && !got, buf);
        }
    }
    printf("%s (%d failed)\n", fails ? "SOME FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
