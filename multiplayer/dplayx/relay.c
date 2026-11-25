/* relay.c - the lords2-relay internet lobby (v14). Included by dplayx.c. Our own code. ASCII only.
 *
 * Hosts list their game on a relay server and joiners pick it from the game's own session list, with no typed address
 * and no port forward: every connection goes from the player to the relay, which pairs a joiner with the host and then
 * copies bytes both ways. Once paired, a relayed connection is a plain TCP socket carrying the normal T_HELLO / T_INFO /
 * T_DATA stream: on the host it takes the place of a socket from accept() (with the relay's joiner_id in place of the
 * peer address, so the per-address limits still work), on the joiner the place of the socket from dial(). Wire format:
 * lords2-relay docs/protocol.md (version 1); integration notes: docs/dplayx-integration.md.
 *
 * Privacy is the point of the relay: players never learn each other's addresses. Nothing here puts an address into a
 * frame; a relayed joiner is logged as relay#<joiner_id>, never as an address.
 *
 * What stays as it was: the LAN listener, the address box and a typed address (direct), the local search, GameRanger
 * (a GameRanger game is never listed, and its joins never use the relay) and the menu join. A relay that cannot be
 * reached only leaves the game unlisted; hosting never fails because of it.
 *
 * Settings ([Multiplayer] in lords2hd.ini, env overrides):
 *   relay=      LOTR2NET_RELAY       relay host[:port]; 0 / off / empty = off. Unset: the built-in server, but only
 *                                     with the known Lords2.exe, so the selftest never reaches the real server
 *   relay_host= LOTR2NET_RELAY_HOST  1 (default) = a host also lists its game on the relay */
#define RL_HOST 1     /* host -> relay: proto u32, max_players u8, name str */
#define RL_HOSTED 2   /* relay -> host: room_id u32 */
#define RL_UPDATE 3   /* host -> relay: players u8, locked u8 */
#define RL_JOIN 4     /* relay -> host: pair_code (16 bytes) */
#define RL_PING 5
#define RL_PONG 6
#define RL_ERROR 7    /* relay -> host: code u8, then the relay closes */
#define RL_LIST 8     /* client -> relay */
#define RL_ROOMS 9    /* relay -> client: count u8, then per room: room_id u32, proto u32, players u8, max u8, name str */
#define RL_MAXF 1024  /* the relay's frame limit */
#define RL_MAXROOMS 20
#define RL_NAME 32    /* name bytes (UTF-8) the relay keeps */
#define RELAY_DEFAULT "139.59.200.92:47700"

/* The relay's error codes (ERROR frames and the status byte of a data connection) */
static const char *rl_err(int code)
{
    switch (code) {
    case 1: return "unsupported relay protocol version - update the mod";
    case 2: return "bad request (a dplayx.dll bug)";
    case 3: return "no such room (it closed meanwhile)";
    case 4: return "the game is locked or full";
    case 5: return "the host did not answer";
    case 6: return "the relay is full, or too many connections from your address";
    }
    return "unknown relay error";
}

/* the relay's host and port; 0 = the relay is off */
static int rl_target(char *host, int cap, int *port)
{
    char v[128] = "", *e = getenv("LOTR2NET_RELAY");
    if (e && *e) lstrcpynA(v, e, sizeof v);
    else if (!ini_get("relay", v, sizeof v)) {
        if (!g_exe_ok) return 0;   /* not the known exe (the selftest): only an explicit setting reaches a relay */
        lstrcpyA(v, RELAY_DEFAULT);
    }
    *port = PORT_DEFAULT;
    if (parse_addr(v, port, "relay")) return 0;
    if (!v[0] || !strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "no") || !_stricmp(v, "false")) return 0;
    lstrcpynA(host, v, cap);
    return 1;
}

/* GameRanger relays its rooms itself: a game inside it never uses ours */
static int rl_allowed(void) { return !GetModuleHandleA("GameRanger.dll"); }

/* The Multiplayer choice dialog (dplayx.c mp_choice, in place of the game's Connect-or-Create dialog 0x6c): what the
 * player picked for this visit. 0 = the stock dialog ran (relay_host and the address box decide, as before) */
#define PICK_PUBLIC 1    /* host: listed on the relay */
#define PICK_PRIVATE 2   /* host: not listed */
#define PICK_INTERNET 1  /* join: straight to the relay's room list, no address box */
#define PICK_ADDRESS 2   /* join: the address box, without its Internet games button */
static volatile LONG g_pick_host, g_pick_join;

static SOCKET rl_dial(DWORD ms)
{
    char host[128];
    int port;
    if (!rl_target(host, sizeof host, &port)) return INVALID_SOCKET;
    return dial_to(host, port, ms);   /* tune_sock included */
}

/* the 6-byte header and the role's part in one send: the relay wants both within 10 s */
static int rl_header(SOCKET s, BYTE role, const void *extra, int n)
{
    BYTE b[32];
    memcpy(b, "L2RL", 4);
    b[4] = 1;
    b[5] = role;
    if (n) memcpy(b + 6, extra, n);
    return send(s, (char *)b, 6 + n, 0) == 6 + n ? 0 : -1;
}

/* the game's names are ANSI (CP_ACP), the relay's UTF-8. At most cap bytes, never cut inside a character */
static int rl_to_utf8(const char *ansi, char *out, int cap)
{
    WCHAR w[80];
    char u[256];
    int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, 80);
    if (n <= 0) return 0;
    n = WideCharToMultiByte(CP_UTF8, 0, w, n - 1, u, sizeof u, NULL, NULL);
    if (n <= 0) return 0;
    if (n > cap) { n = cap; while (n > 0 && ((BYTE)u[n] & 0xC0) == 0x80) n--; }
    memcpy(out, u, n);
    return n;
}
static void rl_from_utf8(const BYTE *u, int n, char *out, int cap)
{
    WCHAR w[80];
    int k = n ? MultiByteToWideChar(CP_UTF8, 0, (const char *)u, n, w, 80) : 0;
    k = k > 0 ? WideCharToMultiByte(CP_ACP, 0, w, k, out, cap - 1, "?", NULL) : 0;
    out[k > 0 ? k : 0] = 0;
}

/* ---- host: pairing one joiner. The relay sent JOIN(pair_code); our data connection must reach it within 10 s, so
 * this runs on its own thread, never waiting for the game or holding cs */
typedef struct { BYTE code[16]; int gen; } RlPair;
static DWORD WINAPI rl_pair_thread(LPVOID a)
{
    RlPair *p = (RlPair *)a;
    SOCKET s = rl_dial(4000);
    BYTE st[5];
    DWORD t0 = GetTickCount();
    u_long id;
    CArg *ca;
    HANDLE th;
    if (s == INVALID_SOCKET) { logf_("relay: a joiner is waiting, but the relay was not reached for it"); goto out; }
    if (rl_header(s, 2, p->code, 16) || recv_all(s, st, 1, t0, 8000)) { logf_("relay: pairing a joiner failed (no answer)"); closesocket(s); goto out; }
    if (st[0]) { logf_("relay: pairing a joiner refused: %s (%u)", rl_err(st[0]), st[0]); closesocket(s); goto out; }
    if (recv_all(s, st + 1, 4, t0, 8000)) { logf_("relay: pairing a joiner failed (no joiner id)"); closesocket(s); goto out; }
    id = *(u_long *)(st + 1);
    set_rcvto(s, 0);
    /* from here on, as accept_body does for a socket from accept(): one handshake slot, one per-address slot */
    if (InterlockedIncrement(&hs_count) > MAXHS) {
        InterlockedDecrement(&hs_count);
        logf_("relay: too many handshakes in flight - relay#%08lx closed", (unsigned long)id);
        closesocket(s);
        goto out;
    }
    if (!hs_ip_take(id)) {
        InterlockedDecrement(&hs_count);
        logf_("relay: over %d handshakes in flight from relay#%08lx - closed", MAXHS_IP, (unsigned long)id);
        closesocket(s);
        goto out;
    }
    if (!(ca = (CArg *)malloc(sizeof *ca))) { InterlockedDecrement(&hs_count); hs_ip_done(id); closesocket(s); goto out; }
    ca->s = s; ca->gen = p->gen; ca->o = NULL; ca->ip = id;
    logf_("relay: joiner relay#%08lx paired - handshake", (unsigned long)id);
    if (!(th = CreateThread(NULL, 0, conn_thread, ca, 0, NULL))) {
        logf_("relay: CreateThread failed for relay#%08lx (%lu) - closed", (unsigned long)id, GetLastError());
        InterlockedDecrement(&hs_count);
        hs_ip_done(id);
        free(ca);
        closesocket(s);
        goto out;
    }
    CloseHandle(th);
out:
    free(p);
    InterlockedDecrement(&g_live);
    return 0;
}

/* ---- host: the room. One control connection for as long as the session is open: HOST -> HOSTED, then PING -> PONG,
 * JOIN -> a pairing thread, and UPDATE when the player count or the locked state changes. The control thread checks
 * those every 0.5 s itself (cs, briefly), so nothing else ever writes to its socket and no other code needs a hook. It
 * ends when the session closes or turns into a ghost hub (G.gen / G.open / G.ghost), and closing the connection
 * removes the room; joiners already paired keep playing. */
static DWORD WINAPI rl_host_thread(LPVOID a)
{
    int gen = (int)(INT_PTR)a, players = -1, locked = -1, k;
    char name[64], u8[RL_NAME], host[128];
    BYTE b[8 + RL_NAME], type, *f;
    DWORD n, room = 0, maxpl;
    int port;
    SOCKET s;
    EnterCriticalSection(&cs);
    lstrcpynA(name, G.sname, sizeof name);
    maxpl = G.maxpl;
    LeaveCriticalSection(&cs);
    if (!rl_target(host, sizeof host, &port)) goto out;
    if ((s = dial_to(host, port, 2500)) == INVALID_SOCKET) {
        logf_("relay: %s:%d not reached - the game is not listed on the internet (LAN and direct joins still work)", host, port);
        goto out;
    }
    if (maxpl < 2) maxpl = 2;
    if (maxpl > 5) maxpl = 5;   /* the relay's limits: in range, so it never has to correct us */
    k = rl_to_utf8(name, u8, RL_NAME);
    *(DWORD *)b = PROTO;
    b[4] = (BYTE)maxpl;
    b[5] = (BYTE)k;
    memcpy(b + 6, u8, k);
    if (rl_header(s, 1, NULL, 0) || sendf(s, RL_HOST, b, 6 + k)) { logf_("relay: listing the game failed (send)"); goto done; }
    f = readf2(s, &type, &n, RL_MAXF, 5000);
    if (!f || type != RL_HOSTED || n < 4) {
        if (f && type == RL_ERROR && n >= 1) logf_("relay: the game was not listed: %s (%u)", rl_err(f[0]), f[0]);
        else logf_("relay: the game was not listed (no answer from the relay)");
        free(f);
        goto done;
    }
    room = *(DWORD *)f;
    free(f);
    logf_("relay: game listed as room %08lx on %s:%d", (unsigned long)room, host, port);
    set_rcvto(s, 0);
    for (;;) {
        fd_set r;
        struct timeval tv = {0, 500000};
        int sel, cur_p, cur_l, alive;
        EnterCriticalSection(&cs);
        alive = G.gen == gen && G.open && !G.ghost;
        cur_p = pl_count();
        cur_l = G.locked != 0;
        LeaveCriticalSection(&cs);
        if (!alive) { logf_("relay: session closed - room %08lx removed", (unsigned long)room); break; }
        if (cur_p < 1) cur_p = 1;   /* 0 before the host's own player exists */
        if ((DWORD)cur_p > maxpl) cur_p = (int)maxpl;
        if (cur_p != players || cur_l != locked) {
            BYTE u[2];
            u[0] = (BYTE)cur_p;
            u[1] = (BYTE)cur_l;
            if (sendf(s, RL_UPDATE, u, 2)) break;
            players = cur_p;
            locked = cur_l;
            logf_("relay: room %08lx now %d/%lu player(s)%s", (unsigned long)room, players, maxpl, locked ? ", locked (no longer listed)" : "");
        }
        FD_ZERO(&r);
        FD_SET(s, &r);
        sel = select(0, &r, NULL, NULL, &tv);
        if (sel < 0) break;
        if (!sel) continue;
        if (!(f = readf2(s, &type, &n, RL_MAXF, 5000))) {
            logf_("relay: control connection lost - room %08lx is gone (LAN and direct joins still work)", (unsigned long)room);
            break;
        }
        if (type == RL_PING) {
            if (sendf(s, RL_PONG, NULL, 0)) { free(f); break; }
        } else if (type == RL_JOIN && n == 16) {
            RlPair *p = (RlPair *)malloc(sizeof *p);
            HANDLE th;
            if (p) {
                memcpy(p->code, f, 16);
                p->gen = gen;
                InterlockedIncrement(&g_live);
                if ((th = CreateThread(NULL, 0, rl_pair_thread, p, 0, NULL)) != NULL) CloseHandle(th);
                else { InterlockedDecrement(&g_live); free(p); logf_("relay: CreateThread failed for a joiner (%lu)", GetLastError()); }
            }
        } else if (type == RL_ERROR) {
            logf_("relay: room %08lx closed by the relay: %s (%u)", (unsigned long)room, rl_err(n ? f[0] : 0), n ? f[0] : 0);
            free(f);
            break;
        } else {
            logf_("relay: unexpected frame type %u on the control connection - closing it", type);
            free(f);
            break;
        }
        free(f);
    }
done:
    closesocket(s);
out:
    InterlockedDecrement(&g_live);
    return 0;
}

/* dp2_Open(create), the session up, cs not held */
static void rl_host_start(int gen)
{
    char host[128];
    int port;
    HANDLE th;
    if (!rl_allowed() || !rl_target(host, sizeof host, &port)) return;
    if (g_pick_host == PICK_PRIVATE) { logf_("relay: a private game - not listed"); return; }
    if (g_pick_host != PICK_PUBLIC && !dg_cfg("LOTR2NET_RELAY_HOST", "relay_host", 1)) return;
    InterlockedIncrement(&g_live);   /* before the thread runs: a Close right now must still wait for it */
    if ((th = CreateThread(NULL, 0, rl_host_thread, (LPVOID)(INT_PTR)gen, 0, NULL)) != NULL) CloseHandle(th);
    else { InterlockedDecrement(&g_live); logf_("relay: CreateThread failed (%lu) - the game is not listed", GetLastError()); }
}

/* ---- joiner: the room list. The address box's "Internet games" button (or LOTR2NET_TEST_RELAYLIST=1) makes the
 * game's own session list show the relay's rooms for this Connect visit (g_rl_list; DirectPlayCreate clears it). The
 * dialog polls EnumSessions every 3 s from its UI loop, so the round trip runs on a worker, as enum_thread does, and
 * the caller waits at most RL_WAIT_MS for it. Each room goes to the callback with a guidInstance that marks it as a
 * relay room (RELAY_GUID, Data1 = room_id), which is how Open(join) knows to join through the relay. */
#define RL_WAIT_MS 400
#define RL_FRESH_MS 10000
static const GUID RELAY_GUID = {0, 0x4c32, 0x4c52, {'L', '2', 'R', 'L', 'R', 'O', 'O', 'M'}};
static volatile LONG g_rl_list;
static CRITICAL_SECTION rlcs;
typedef struct { DWORD id, proto; int players, maxpl; char name[48]; } RlRoom;
static struct { int running, have, ok, n; DWORD t, seq; RlRoom r[RL_MAXROOMS]; } RL;

static void rl_init(void) { InitializeCriticalSection(&rlcs); }   /* DllMain */

static int rl_list_wanted(void)
{
    char v[4] = "";
    if (!rl_allowed()) return 0;
    return g_rl_list || g_pick_join == PICK_INTERNET || (GetEnvironmentVariableA("LOTR2NET_TEST_RELAYLIST", v, sizeof v) && v[0] == '1');
}

static DWORD WINAPI rl_list_thread(LPVOID a)
{
    SOCKET s = rl_dial(800);
    BYTE type, *f = NULL;
    DWORD n = 0, k = 1;
    RlRoom r[RL_MAXROOMS];
    int cnt = 0, ok = 0, i;
    (void)a;
    if (s != INVALID_SOCKET) {
        if (!rl_header(s, 4, NULL, 0) && !sendf(s, RL_LIST, NULL, 0)) f = readf2(s, &type, &n, RL_MAXF, 3000);
        closesocket(s);
    }
    if (f && type == RL_ROOMS && n >= 1) {
        ok = 1;
        for (i = 0; i < f[0] && cnt < RL_MAXROOMS; i++) {   /* room_id, proto, players, max, name str: checked against n */
            int ln;
            if (k + 11 > n || k + 11 + (ln = f[k + 10]) > n) { ok = 0; break; }
            r[cnt].id = *(DWORD *)(f + k);
            r[cnt].proto = *(DWORD *)(f + k + 4);
            r[cnt].players = f[k + 8];
            r[cnt].maxpl = f[k + 9];
            rl_from_utf8(f + k + 11, ln, r[cnt].name, sizeof r[cnt].name);
            if (r[cnt].id) cnt++;
            k += 11 + ln;
        }
    }
    free(f);
    EnterCriticalSection(&rlcs);
    RL.running = 0;
    RL.have = 1;
    RL.ok = ok;
    RL.n = ok ? cnt : 0;
    memcpy(RL.r, r, sizeof r);
    RL.t = GetTickCount();
    RL.seq++;
    LeaveCriticalSection(&rlcs);
    return 0;
}

/* dp2_EnumSessions in list mode: one callback per room of the newest answer */
static void rl_enum(DPSD2 *want, ENUMSESS_CB cb, void *ctx)
{
    RlRoom r[RL_MAXROOMS];
    int n = 0, i, have = 0, ok = 0;
    DWORD t0 = GetTickCount(), seq = 0;
    EnterCriticalSection(&rlcs);
    if (!RL.running) {   /* a fresh round; this call takes any answer under RL_FRESH_MS old */
        HANDLE th;
        RL.running = 1;
        if ((th = CreateThread(NULL, 0, rl_list_thread, NULL, 0, NULL)) != NULL) CloseHandle(th); else RL.running = 0;
    }
    for (;;) {
        if (RL.have && GetTickCount() - RL.t < RL_FRESH_MS) {
            have = 1; ok = RL.ok; n = RL.n; seq = RL.seq;
            memcpy(r, RL.r, sizeof r);
            if (!RL.running) break;
        }
        if (GetTickCount() - t0 >= RL_WAIT_MS) break;
        LeaveCriticalSection(&rlcs);
        Sleep(10);
        EnterCriticalSection(&rlcs);
    }
    LeaveCriticalSection(&rlcs);
    if (!have || !ok) { logf_("EnumSessions: relay list - %s", !have ? "round still running" : "no answer from the relay"); return; }
    for (i = 0; i < n; i++) {
        DPSD2 d;
        DWORD to = 0;
        char nm[96];   /* the callback's own buffer (F18) */
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwMaxPlayers = (DWORD)r[i].maxpl;
        d.dwCurrentPlayers = (DWORD)r[i].players;
        d.guidInstance = RELAY_GUID;
        d.guidInstance.Data1 = r[i].id;
        if (want) d.guidApplication = want->guidApplication;
        if (r[i].proto == PROTO) lstrcpynA(nm, r[i].name, sizeof nm);
        else snprintf(nm, sizeof nm, "%.24s [host v%lu, yours v%lu]", r[i].name, (unsigned long)r[i].proto, (unsigned long)PROTO);
        d.lpszSessionName = nm;
        if (!cb(&d, &to, 0, ctx)) break;
    }
    logf_("EnumSessions: relay list, %d room(s) (round %lu)", n, (unsigned long)seq);
}
/* 1 and the room id when g is one of ours */
static int rl_room_of(const GUID *g, DWORD *id)
{
    if (!g->Data1 || g->Data2 != RELAY_GUID.Data2 || g->Data3 != RELAY_GUID.Data3 || memcmp(g->Data4, RELAY_GUID.Data4, 8)) return 0;
    *id = g->Data1;
    return 1;
}


/* ---- joiner: joining a room. Open(join) with a relay room's guidInstance: the relay pairs us with the host, then the
 * socket carries the normal handshake (dp2_Open goes on from T_HELLO as for a socket from dial). *hr = why not */
static SOCKET rl_join(DWORD room, DPHR *hr)
{
    BYTE st;
    DWORD t0;
    SOCKET s = rl_dial(2500);
    if (s == INVALID_SOCKET) { logf_("Open(join): the relay was not reached"); *hr = DPERR_NOCONNECTION; return s; }
    t0 = GetTickCount();
    /* 12 s: longer than the relay's own 10 s pairing timeout, so its answer (5, host did not answer) is what arrives */
    if (rl_header(s, 3, &room, 4) || recv_all(s, &st, 1, t0, 12000)) {
        logf_("Open(join): no answer from the relay for room %08lx", (unsigned long)room);
        closesocket(s);
        *hr = DPERR_NOCONNECTION;
        return INVALID_SOCKET;
    }
    if (st) {
        logf_("Open(join): the relay refused room %08lx: %s (%u)", (unsigned long)room, rl_err(st), st);
        closesocket(s);
        *hr = st == 3 ? DPERR_NOSESSIONS : st == 4 ? DPERR_ACTIVEPLAYERS : st == 5 ? DPERR_NOCONNECTION : st == 2 ? DPERR_GENERIC : DPERR_UNAVAILABLE;
        return INVALID_SOCKET;
    }
    set_rcvto(s, 0);
    logf_("Open(join): paired with room %08lx through the relay", (unsigned long)room);
    return s;
}
