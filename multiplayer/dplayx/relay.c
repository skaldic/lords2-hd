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
    if (!rl_allowed() || !dg_cfg("LOTR2NET_RELAY_HOST", "relay_host", 1) || !rl_target(host, sizeof host, &port)) return;
    InterlockedIncrement(&g_live);   /* before the thread runs: a Close right now must still wait for it */
    if ((th = CreateThread(NULL, 0, rl_host_thread, (LPVOID)(INT_PTR)gen, 0, NULL)) != NULL) CloseHandle(th);
    else { InterlockedDecrement(&g_live); logf_("relay: CreateThread failed (%lu) - the game is not listed", GetLastError()); }
}
