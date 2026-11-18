/* dplayx.c - our own drop-in DirectPlay (dplayx.dll) for the original Lords2.exe.
 * No Windows/Wine DirectPlay is used. Transport: TCP, star topology, host relays.
 * Written from public DirectPlay 2 documentation + our own static read of Lords2.exe
 * (tools/container-harness/mp-dplay-surface.md). C only, 32-bit, mingw-w64.
 *
 * Exports (by ordinal AND name): 1 DirectPlayCreate, 2 DirectPlayEnumerateA.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define DLL_VERSION 12  /* 12: diag.c, T_PING / T_PONG between the DLLs (a v11 peer would not answer them, so v11 is refused) and one DIAG line per second instead of per-message heartbeat lines; game traffic unchanged; 11: AI rally globals (0x56d89c, 0x522f70/74) zeroed at every battle setup on every peer + hashed as battle region ai-rally - battles differ from v10, so v10 is refused; 10: ghost hub (the host's game leaves a locked game -> this process keeps relaying for the joiners and T_HOST makes the lowest remaining player the game host via DPSYS_HOST); 9: peer trust (host validates every game message header and the sender rules, joiner takes host-only messages only from the host), resync authority = host by default, idle-socket caps, receive-queue backpressure; 8: victory fix (standings recomputed after a remote lord is removed), speed-10 and battle pace gated like SP, resync F1-F3 - game results differ from v7, so v7 is refused; 7: per-conn writer threads, sync v7 (epoch, stamped player, save-rest region), T_RESYNC (resync.c, RS_VER 1), pace in PROTO; 6 (+ Connect address box, wire unchanged): pace patch (peers must step alike); 5: T_SYNC frames (sync.c, per-turn sync check); 4: re-host bind fallback, mismatched peers see the game listed; the wire value is PROTO */
/* PROTO digits: thousands = DLL_VERSION, hundreds = pace patch applied (0/1), tens = 2*exe_ok + slowlink (0..3),
 * units = mpfix (0/1). g_exe_ok / g_pace_ok / g_mpfix are set in exepatch.c, g_slowlink once at load from
 * LOTR2NET_SLOWLINK. DLL_VERSION 7 carries resync wire version RS_VER 1 (a peer of another DLL version is refused). */
#define PROTO ((DWORD)(DLL_VERSION * 1000 + (g_pace_ok ? 100 : 0) + (g_exe_ok ? 20 : 0) + (g_slowlink ? 10 : 0) + (g_mpfix ? 1 : 0)))
#define PORT_DEFAULT 47700
#define MAXCONN 8
#define MAXPL 16
#define MAXRECV 260      /* the game's receive buffer is 0x104 bytes: never carry or deliver more */
#define QMAX 16384       /* receive queue cap, messages (v7: was 4096; review #3) */
#define QSYS 256         /* system messages may pass QMAX by this many, never more */
#define QSHED (QMAX / 2) /* from here on, the game's 2nd/3rd copies (slot byte 0x40/0x80) are not queued: see qpush */
#define OUT_CAP (4u * 1024u * 1024u)   /* per-conn outbound queue, bytes: over it the peer is stalled for good -> dropped */
#define MAXCREATE 4      /* players one connection may create over its life (destroy + create loops) */
#define MAXHS 16         /* in-flight (pre-hello) handshake threads */
#define LOG_CAP (8u * 1024u * 1024u)
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif
#define KEEPALIVE_VALS_IOCTL 0x98000004UL   /* SIO_KEEPALIVE_VALS = _WSAIOW(IOC_VENDOR, 4); mingw may lack mstcpip.h */
struct keepalive_vals_ { u_long onoff, keepalivetime, keepaliveinterval; };

typedef DWORD DPID;
typedef long DPHR;

/* ---- DirectPlay error codes (MAKE_DPHRESULT = 0x88770000 | code) ---- */
#define DP_OK 0
#define DPE(c) ((DPHR)(0x88770000u | (c)))
#define DPERR_ALREADYINITIALIZED DPE(5)
#define DPERR_ACTIVEPLAYERS DPE(20)
#define DPERR_BUFFERTOOSMALL DPE(30)
#define DPERR_CANTCREATEPLAYER DPE(60)
#define DPERR_CANTCREATESESSION DPE(70)
#define DPERR_INVALIDOBJECT DPE(130)
#define DPERR_INVALIDPLAYER DPE(150)
#define DPERR_NOCONNECTION DPE(170)
#define DPERR_NOMESSAGES DPE(190)
#define DPERR_NOSESSIONS DPE(220)
#define DPERR_SENDTOOBIG DPE(230)
#define DPERR_UNAVAILABLE DPE(250)
#define DPERR_UNSUPPORTED ((DPHR)0x80004001)
#define DPERR_GENERIC ((DPHR)0x80004005)
#define DPERR_INVALIDPARAMS ((DPHR)0x80070057)
#define DPOPEN_JOIN 1
#define DPOPEN_CREATE 2
#define DPSYS_CREATEPLAYERORGROUP 3
#define DPSYS_DESTROYPLAYERORGROUP 5
#define DPSYS_SESSIONLOST 0x31
#define DPSYS_HOST 0x101   /* DPMSG_HOST: dwType only. The game: FUN_004b8c3a -> FUN_0043e6a2(4) sets its host flag 0x553248 */
#define DPRECEIVE_PEEK 0x08   /* dplay.h (mingw-w64): ALL 1, TOPLAYER 2, FROMPLAYER 4, PEEK 8 (was 0x20; review #12) */

/* ---- our own struct definitions (public DirectPlay 2 layout) ---- */
typedef struct {
    DWORD dwSize, dwFlags;
    GUID guidInstance, guidApplication;
    DWORD dwMaxPlayers, dwCurrentPlayers;
    char *lpszSessionName, *lpszPassword; /* 32-bit: 4 bytes each; ANSI bytes in practice */
    DWORD dwReserved1, dwReserved2, dwUser1, dwUser2, dwUser3, dwUser4;
} DPSD2;
typedef struct { DWORD dwSize, dwFlags; char *lpszShortName, *lpszLongName; } DPNAME_;
typedef struct {
    DWORD dwSize, dwFlags, dwMaxBufferSize, dwMaxQueueSize, dwMaxPlayers, dwHundredBaud, dwLatency,
        dwMaxLocalPlayers, dwHeaderLength, dwTimeout;
} DPCAPS_;
typedef BOOL(WINAPI *ENUMSESS_CB)(const DPSD2 *, DWORD *, DWORD, void *);
typedef BOOL(WINAPI *ENUMSP_CB)(GUID *, char *, DWORD, DWORD, void *);

static const GUID IID_IUnknown_ = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID IID_DP2 = {0x2B74F7C0, 0x9154, 0x11CF, {0xA9, 0xCD, 0x00, 0xAA, 0x00, 0x68, 0x86, 0xE3}};
static const GUID IID_DP2A = {0x9D460580, 0xA822, 0x11CF, {0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82}};
static GUID SPGUID_TCPIP = {0x36E95EE0, 0x8577, 0x11CF, {0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82}};

/* ---- wire protocol: [u32 len][u8 type][payload]; len counts type+payload ---- */
enum { T_HELLO = 1, T_INFO, T_REJECT, T_CREATEPL, T_PLAYERID, T_SYS_CREATE, T_SYS_DESTROY, T_DATA, T_DESTROYPL }; /* 10 = T_SYNC (sync.c), 11 = T_RESYNC (resync.c) */
#define T_HOST 12   /* v10 hub -> joiners: [u32 DPID] plays the game host from now on (ghost hub) */
#define MODE_ENUM 1
#define MODE_JOIN 2
static const char MAGIC[8] = {'L', 'O', 'T', 'R', '2', 'N', 'E', 'T'};

typedef struct Msg { struct Msg *next; DPID from, to; DWORD len, t_us; BYTE data[4]; } Msg;   /* allocated to fit len; t_us: when queued (diag.c) */
typedef struct { int used; DPID id; char name[32]; int conn; DWORD lord, hello_t; int hello_n; BYTE cp_next; DWORD cp_n, cp_h;
    struct { DWORD tk[64], tt[64]; int z_tok; DWORD z_t; long hw_ahead, hw_hb; int hw_infl, hw_tick, b_tok, l_tok; DWORD b_t, l_t, old_r, nd, hb_last, hb_t, wd_n, hb_pre, hb_pre_t; int lead_add; BYTE seq, sq[10], sqi; } tr; } Player;   /* lord, hello_*, cp_*, tr: host only (trust.c) */
/* One outbound queue + writer thread per socket (review #2): a game thread or reader only appends under oc and never
 * blocks on a peer's TCP window. Frames leave in FIFO order, so per-sender order and "resync file before 0x16" hold.
 * refs: the reader and the writer each hold one; the last to let go closes the socket (never while one is in a call). */
typedef struct OFrame { struct OFrame *next; DWORD n; BYTE b[5]; } OFrame;
typedef struct Out {
    SOCKET s;
    CRITICAL_SECTION oc;
    HANDLE ev;
    OFrame *h, *t;
    DWORD bytes, peak;
    volatile LONG refs, stop, dead, wdone;
    char tag[16];
} Out;
typedef struct { int used; SOCKET s; Out *o; int haveplayer, ncreate, sync_n, rs_n, bad_lord, dt_tok, dt_over, dt_rate, dg_n; DWORD sync_t, rs_t, lord, dt_t, dg_t; u_long ip; } Conn;   /* ip: the peer's address (F3 per-address caps) */   /* lord: the one lord slot this conn may speak for (resync.c rs_conn_lord) */

static CRITICAL_SECTION cs;
static struct {
    int open, host, locked, gen, connected;
    SOCKET lsock, jsock;
    Out *jout;                  /* joiner: the writer for jsock */
    char sname[64];
    DWORD maxpl, flags;
    GUID inst;
    Conn conns[MAXCONN];
    Player pl[MAXPL];
    DPID myid, nextid;
    Msg *qh, *qt;
    int qcount, qfull_logged, qshed_logged;
    DWORD qshed, qpeak;
    HANDLE evPid;
    DPID pidResult;
    int pidWaiting;             /* a joiner's CreatePlayer is waiting for T_PLAYERID (F20) */
    int ghost;                  /* host: our game left a locked game, we only relay for the joiners (v10) */
    DPID gamehost;              /* hub in ghost: the player told T_HOST; joiner: the last T_HOST seen */
} G;

static HANDLE hLog = INVALID_HANDLE_VALUE;
static DWORD t0;
static char exedir[MAX_PATH];
static int wsa_ok;
static int g_slowlink;                 /* LOTR2NET_SLOWLINK on, evaluated once in DllMain */
static volatile LONG hs_count;         /* in-flight handshake threads */
#define JOIN_WAIT_MS 10000             /* a joined conn must create its player within this (the game does it in ~10 ms; was 30 s, review tb2 F3) */
#define MAXHS_IP 4                     /* in-flight handshakes from one address (F3: one machine cannot hold all MAXHS) */
#define MAXPEND_IP 2                   /* joined-but-playerless conns from one address (F3: two players behind one router still fit) */
#define SYNC_PER_S 32                  /* T_SYNC records taken from one conn per second (the game makes about 1) */
#define DATA_PER_S 200                 /* game messages taken from one conn per second, sustained (a 5-peer rig peaked at 61/s per player, 2026-10-07) */
#define DATA_BURST 3000                /* ... with this much saved up, so a long network stall's backlog still passes */
#define RESYNC_PER_S 256               /* T_RESYNC frames taken from one conn per second (a whole upload is at most 173; the budget cuts a BEGIN flood) */
static CRITICAL_SECTION logcs;
static DWORD log_bytes;
static int log_capped;
static int log_force;   /* sync.c sets it around SYNC/DESYNC lines (see logf_) */
static int g_log_hb;    /* log_heartbeats=1: per-message 0x0a lines too (diag.c) */
static volatile LONG g_msg_stopped;   /* per-message lines have stopped at the log cap; a new session folder resets it (sesslog.c) */
static const char *lf_dir(void);      /* sesslog.c: where a session's own files go */
static void lf_copy_all(void);

/* ------------------------------------------------------------------ logging */
/* Async log (review #14): logf_ only appends to a 512 KB buffer under logcs; a below-normal-priority thread writes it
 * every 200 ms under logwr. A full buffer is written by the caller, in order (the buffer swap and the write both run
 * under logwr). The 8 MB cap is counted when a line is accepted, so it means what it meant before. Lock order:
 * logwr -> logcs. A crash loses at most the last 200 ms of lines. */
#define LOGBUF (512u * 1024u)
static CRITICAL_SECTION logwr;
static char *lb_cur, *lb_spare;
static DWORD lb_n;
static volatile LONG lb_thread;

static void log_open(void)   /* logwr held */
{
    char p[MAX_PATH + 16], q[MAX_PATH + 16];
    static int tried;
    if (hLog != INVALID_HANDLE_VALUE || tried) return;
    tried = 1;
    lstrcpynA(p, exedir, MAX_PATH);
    lstrcatA(p, "dplayx.log");
    lstrcpynA(q, exedir, MAX_PATH);   /* once per run: the last run's log becomes dplayx.prev.log */
    lstrcatA(q, "dplayx.prev.log");
    MoveFileExA(p, q, MOVEFILE_REPLACE_EXISTING);
    hLog = CreateFileA(p, FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* logwr held: write what is buffered, in order */
static void log_drain(void)
{
    char *w;
    DWORD wn, x;
    log_open();
    EnterCriticalSection(&logcs);
    w = lb_cur; wn = lb_n;
    lb_cur = lb_spare; lb_spare = NULL; lb_n = 0;
    LeaveCriticalSection(&logcs);
    if (w && wn && hLog != INVALID_HANDLE_VALUE) WriteFile(hLog, w, wn, &x, NULL);
    EnterCriticalSection(&logcs);
    if (!lb_cur) lb_cur = w;          /* the spare was never allocated: keep using this one */
    else if (!lb_spare) lb_spare = w; else free(w);
    LeaveCriticalSection(&logcs);
}

static DWORD WINAPI log_thread(LPVOID a)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Sleep(200);
        if (!lb_n) continue;
        EnterCriticalSection(&logwr);
        log_drain();
        LeaveCriticalSection(&logwr);
    }
    return 0;
}

static void log_append(const char *b, DWORD n)
{
    static const char cap[] = "log cap reached (8 MB), no further lines written except SYNC/DESYNC/RESYNC\r\n";
    const char *s = b;
    DWORD sn = n;
    int full;
    EnterCriticalSection(&logcs);
    if (log_capped && !(log_force && log_bytes + n <= LOG_CAP + (1u << 20))) { LeaveCriticalSection(&logcs); return; }
    if (!log_capped && log_bytes + n > LOG_CAP) { s = cap; sn = (DWORD)sizeof cap - 1; log_capped = 1; }
    log_bytes += sn;
    if (!lb_cur) lb_cur = (char *)malloc(LOGBUF);
    if (!lb_spare) lb_spare = (char *)malloc(LOGBUF);
    full = !lb_cur || lb_n + sn > LOGBUF;
    if (!full) { memcpy(lb_cur + lb_n, s, sn); lb_n += sn; }
    LeaveCriticalSection(&logcs);
    if (!lb_thread && !InterlockedExchange(&lb_thread, 1)) {
        HANDLE th = CreateThread(NULL, 0, log_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    if (full) {   /* write the buffer, then this line, in order */
        DWORD x;
        EnterCriticalSection(&logwr);
        log_drain();
        if (hLog != INVALID_HANDLE_VALUE) WriteFile(hLog, s, sn, &x, NULL);
        LeaveCriticalSection(&logwr);
    }
}

/* DLL_PROCESS_DETACH: other threads are gone and may have died holding a lock, so only try */
static void log_flush_exit(void)
{
    if (TryEnterCriticalSection(&logwr)) {
        if (TryEnterCriticalSection(&logcs)) {
            DWORD x;
            LeaveCriticalSection(&logcs);
            log_open();
            if (lb_cur && lb_n && hLog != INVALID_HANDLE_VALUE) WriteFile(hLog, lb_cur, lb_n, &x, NULL);
            lb_n = 0;
        }
        LeaveCriticalSection(&logwr);
    }
}

static void logf_(const char *fmt, ...)
{
    char b[600];
    DWORD n, t = GetTickCount() - t0;
    int r;
    va_list ap;
    n = (DWORD)sprintf(b, "[%6lu.%03lu] %s ", t / 1000, t % 1000, G.host ? "H" : "J");
    va_start(ap, fmt);
    r = _vsnprintf(b + n, sizeof(b) - n - 2, fmt, ap);
    va_end(ap);
    {   /* review tb2 F11: peer-made text (player/session names) can never start a line of its own or fake one */
        DWORD k, s = n;
        if (r < 0 || (DWORD)r > sizeof(b) - n - 3) n = sizeof(b) - 3; else n += (DWORD)r;
        for (k = s; k < n; k++) if ((BYTE)b[k] < 0x20 || (BYTE)b[k] > 0x7e) b[k] = '?';
    }
    b[n++] = '\r';
    b[n++] = '\n';
    log_append(b, n);
}
static void logmsg(const char *dir, DPID from, DPID to, const BYTE *d, DWORD len)
{
    /* 5-peer rig 2026-10-07: per-message lines filled the 8 MB cap in ~9 minutes and the later conn-drop line was lost.
     * They stop at LOG_CAP - 2 MB; events (conns, players, TRUST, errors) keep the last 2 MB. */
    if (!g_log_hb && len == 8 && d[0] == 0x0a) return;   /* v12: a frozen peer repeats these up to 140 times a second; the DIAG lines count them */
    if (log_bytes > LOG_CAP - (2u << 20)) {
        if (!InterlockedExchange(&g_msg_stopped, 1)) logf_("per-message lines stop here (log at %u MB); events are still logged", (LOG_CAP >> 20) - 2);
        return;
    }
    logf_("MSG %s from=%08lx to=%08lx size=%lu b4=%02x %02x %02x %02x", dir, from, to, len,
          len > 0 ? d[0] : 0, len > 1 ? d[1] : 0, len > 2 ? d[2] : 0, len > 3 ? d[3] : 0);
}

/* ------------------------------------------------------------------ helpers */
static int ini_get(const char *key, char *out, int cap);
#include "exepatch.c"
#include "victory.c"

static void ensure_init(void)
{
    WSADATA w;
    if (wsa_ok) return;
    WSAStartup(MAKEWORD(2, 2), &w);
    wsa_ok = 1;
}

/* lotr2net.ini beside Lords2.exe: "key=value" lines; returns 1 and the trimmed value when key is present */
static int ini_get(const char *key, char *out, int cap)
{
    char p[MAX_PATH + 16], line[512];
    FILE *f;
    int kl = (int)strlen(key), found = 0, first = 1;
    lstrcpynA(p, exedir, MAX_PATH);
    lstrcatA(p, "lotr2net.ini");
    f = fopen(p, "r");
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        if (first) {   /* review tb2 F9: Notepad's "UTF-8" adds a BOM; "Unicode" (UTF-16) cannot be read as ASCII at all */
            static int warned;
            first = 0;
            if ((BYTE)s[0] == 0xFF || (BYTE)s[0] == 0xFE) {
                if (!warned++) logf_("lotr2net.ini is saved as Unicode (UTF-16) - it is IGNORED; save it as ANSI or UTF-8");
                break;
            }
            if ((BYTE)s[0] == 0xEF && (BYTE)s[1] == 0xBB && (BYTE)s[2] == 0xBF) s += 3;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (_strnicmp(s, key, kl)) continue;
        s += kl;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '=') continue;
        s++;
        while (*s == ' ' || *s == '\t') s++;
        lstrcpynA(out, s, cap);
        { char *c = strpbrk(out, ";#"); if (c) *c = 0; }   /* F9: "mpfix=off ; rig" is "off" (no value here holds ; or #) */
        int n = (int)strlen(out);
        while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
        found = 1;
    }
    fclose(f);
    return found;
}

static int parse_port(const char *t, const char *what)
{
    int v = atoi(t);
    if (v < 1024 || v > 65535) { logf_("%s port '%s' out of range - using %d", what, t, PORT_DEFAULT); return PORT_DEFAULT; }
    return v;
}
/* The address typed into our Connect box (ask_join), used for this run even when lotr2net.ini cannot be written. */
static char g_ask_host[128];
/* ddraw's in-game Multiplayer menu (ddraw/mpmenu.c, 2026-10-06): mode 1 = the next Connect joins g_menu_host with no
 * Win32 window at all (no address box; the session list stays invisible and its first session is taken, or it is
 * cancelled with g_menu_msg saying why), mode 2 = the next Create (nothing to do here). Game thread only. */
static int g_menu, g_menu_tries;
static DWORD g_menu_seq, g_menu_t0;   /* menu_enum: last enum round counted, when this menu join began (#15) */
static volatile LONG g_enum_gen;       /* bumped per menu join: an older round is never taken for a newer join */
static char g_menu_host[128], g_menu_msg[128];
static void beacon_start(int port);
static void grhb_start(int port);
static void beacon_stop(void);
static void get_target(char *host, int *port)
{
    char *e = getenv("LOTR2NET_HOST");
    host[0] = 0;
    *port = PORT_DEFAULT;
    if (g_menu == 1) lstrcpynA(host, g_menu_host, 120);
    else if (e && *e) lstrcpynA(host, e, 120);
    else if (g_ask_host[0]) lstrcpynA(host, g_ask_host, 120);
    else ini_get("host", host, 120);
    { /* trim both ends, optional :port */
        char *c, *l = host;
        int n;
        while (*l == ' ' || *l == '\t') l++;
        if (l != host) memmove(host, l, strlen(l) + 1);
        n = (int)strlen(host);
        while (n > 0 && (host[n - 1] == '\r' || host[n - 1] == '\n' || host[n - 1] == ' ' || host[n - 1] == '\t')) host[--n] = 0;
        c = strchr(host, ':');
        if (c && (strchr(c + 1, ':') || host[0] == '[')) {   /* F9: an IPv6 address must fail loudly, never fall back to loopback */
            static int warned;
            if (!warned++) logf_("host '%s' looks like an IPv6 address - only IPv4 addresses and computer names work", host);
            if (g_menu == 1) snprintf(g_menu_msg, sizeof g_menu_msg, "IPv6 is not supported: use an IPv4 address");
        } else if (c) { *c = 0; *port = parse_port(c + 1, "host"); }
    }
    if (!host[0]) lstrcpyA(host, "127.0.0.1");
}
static int listen_port(void)
{
    char *e = getenv("LOTR2NET_PORT");
    return (e && *e) ? parse_port(e, "listen") : PORT_DEFAULT;
}

/* End a connection whose reader thread may be inside recv on it: shutdown (never closesocket - the reader still owns
 * the handle), then cancel that pending recv, because on Windows shutdown alone does not wake a recv when the peer is
 * alive but stalled. The reader then fails and runs its normal drop / host-lost path. */
static void kill_sock(SOCKET s)
{
    static BOOL (WINAPI *cancel)(HANDLE, LPOVERLAPPED);
    static volatile LONG looked;
    shutdown(s, SD_BOTH);
    if (!looked) {
        cancel = (BOOL (WINAPI *)(HANDLE, LPOVERLAPPED))GetProcAddress(GetModuleHandleA("kernel32.dll"), "CancelIoEx");
        looked = 1;
    }
    if (cancel) cancel((HANDLE)s, NULL);
}

/* A failed or timed-out send (SO_SNDTIMEO) means a dead or stalled peer: kill_sock it. */
static int sendf(SOCKET s, BYTE type, const void *p, DWORD n)
{
    DWORD tot = 5 + n, off = 0;
    BYTE *b = (BYTE *)malloc(tot);
    if (!b) return -1;
    *(DWORD *)b = n + 1;
    b[4] = type;
    if (n) memcpy(b + 5, p, n);
    while (off < tot) {
        int r = send(s, (char *)b + off, (int)(tot - off), 0);
        if (r <= 0) {
            logf_("send failed/timed out (WSA %d) - shutting socket down", WSAGetLastError());
            kill_sock(s);
            free(b);
            return -1;
        }
        off += (DWORD)r;
    }
    free(b);
    return 0;
}
static void set_rcvto(SOCKET s, DWORD ms) { setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&ms, sizeof ms); }
/* deadline_ms != 0: absolute budget counted from t_start across all reads */
static int recv_all(SOCKET s, BYTE *b, DWORD n, DWORD t_start, DWORD deadline_ms)
{
    DWORD off = 0;
    while (off < n) {
        int r;
        if (deadline_ms) {
            DWORD el = GetTickCount() - t_start;
            if (el >= deadline_ms) return -1;
            set_rcvto(s, deadline_ms - el);
        }
        r = recv(s, (char *)b + off, (int)(n - off), 0);
        if (r <= 0) return -1;
        off += (DWORD)r;
    }
    return 0;
}
static BYTE *readf2(SOCKET s, BYTE *type, DWORD *n, DWORD maxlen, DWORD deadline_ms)
{
    DWORD len, t_start = GetTickCount();
    BYTE t, *b;
    if (recv_all(s, (BYTE *)&len, 4, t_start, deadline_ms) || len < 1 || len > maxlen) return NULL;
    if (recv_all(s, &t, 1, t_start, deadline_ms)) return NULL;
    b = (BYTE *)malloc(len + 1);
    if (!b) return NULL;
    if (len > 1 && recv_all(s, b, len - 1, t_start, deadline_ms)) { free(b); return NULL; }
    *type = t;
    *n = len - 1;
    return b;
}
static BYTE *readf(SOCKET s, BYTE *type, DWORD *n) { return readf2(s, type, n, 70000, 0); }

/* every socket: no Nagle, bounded blocking sends, fast dead-peer detection (10 s idle, probe every 2 s) */
static void tune_sock(SOCKET s)
{
    int one = 1;
    DWORD snd = 5000, got = 0;
    struct keepalive_vals_ ka;
    ka.onoff = 1; ka.keepalivetime = 10000; ka.keepaliveinterval = 2000;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&snd, sizeof snd);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof one);
    if (WSAIoctl(s, KEEPALIVE_VALS_IOCTL, &ka, sizeof ka, NULL, 0, &got, NULL, NULL))
        logf_("keepalive ioctl failed, WSA %d (default 2 h timers)", WSAGetLastError());
}

/* ------------------------------------------------- per-connection writer (review #2) */
static volatile LONG g_live;           /* accept / conn reader / join reader / writer / beacon threads: do_close waits */

static void out_release(Out *o)
{
    if (InterlockedDecrement(&o->refs)) return;
    closesocket(o->s);
    CloseHandle(o->ev);
    DeleteCriticalSection(&o->oc);
    free(o);
}

static DWORD WINAPI out_thread(LPVOID a)
{
    Out *o = (Out *)a;
    for (;;) {
        OFrame *f;
        EnterCriticalSection(&o->oc);
        f = o->h;
        if (f) { o->h = f->next; if (!o->h) o->t = NULL; o->bytes -= f->n; }
        LeaveCriticalSection(&o->oc);
        if (!f) {
            if (o->stop || o->dead) break;
            WaitForSingleObject(o->ev, 250);
            continue;
        }
        if (!o->dead) {
            DWORD off = 0;
            while (off < f->n) {
                int r = send(o->s, (char *)f->b + off, (int)(f->n - off), 0);
                if (r <= 0) {
                    logf_("%s: send failed/timed out (WSA %d) - shutting the connection down", o->tag, WSAGetLastError());
                    InterlockedExchange(&o->dead, 1);
                    kill_sock(o->s);
                    break;
                }
                off += (DWORD)r;
            }
        }
        free(f);
    }
    EnterCriticalSection(&o->oc);   /* dead: drop whatever is left */
    while (o->h) { OFrame *f = o->h; o->h = f->next; free(f); }
    o->t = NULL; o->bytes = 0;
    LeaveCriticalSection(&o->oc);
    InterlockedExchange(&o->wdone, 1);
    out_release(o);
    InterlockedDecrement(&g_live);
    return 0;
}

/* A writer for s; refs = 2 (the caller's reader + the writer). NULL: no memory/thread (the caller closes s). */
static Out *out_new(SOCKET s, const char *tag)
{
    Out *o = (Out *)calloc(1, sizeof *o);
    HANDLE th;
    if (!o) return NULL;
    o->s = s;
    o->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!o->ev) { free(o); return NULL; }
    InitializeCriticalSection(&o->oc);
    o->refs = 2;
    lstrcpynA(o->tag, tag, sizeof o->tag);
    InterlockedIncrement(&g_live);
    th = CreateThread(NULL, 0, out_thread, o, 0, NULL);
    if (!th) {
        InterlockedDecrement(&g_live);
        CloseHandle(o->ev);
        DeleteCriticalSection(&o->oc);
        free(o);
        return NULL;
    }
    CloseHandle(th);
    return o;
}

/* Append one frame; never blocks on the network. -1: the conn is dead or its queue passed OUT_CAP (then it is shut
 * down: a peer that has not taken 4 MB is stalled for good, and dropping frames silently would desync it). */
static int osend(Out *o, BYTE type, const void *p, DWORD n)
{
    OFrame *f;
    int over = 0;
    if (!o || o->dead || o->stop) return -1;
    f = (OFrame *)malloc(sizeof(OFrame) + n);
    if (!f) { logf_("%s: out of memory for a frame - shutting it down", o->tag); InterlockedExchange(&o->dead, 1); kill_sock(o->s); return -1; }
    f->next = NULL;
    f->n = 5 + n;
    *(DWORD *)f->b = n + 1;
    f->b[4] = type;
    if (n) memcpy(f->b + 5, p, n);
    EnterCriticalSection(&o->oc);
    if (o->bytes + f->n > OUT_CAP) over = 1;
    else {
        if (o->t) o->t->next = f; else o->h = f;
        o->t = f;
        o->bytes += f->n;
        if (o->bytes > o->peak) o->peak = o->bytes;
    }
    LeaveCriticalSection(&o->oc);
    if (over) {
        free(f);
        logf_("%s: outbound queue over %u bytes (peer not reading) - shutting the connection down", o->tag, OUT_CAP);
        InterlockedExchange(&o->dead, 1);
        kill_sock(o->s);
        return -1;
    }
    SetEvent(o->ev);
    return 0;
}

/* The writer sends what is queued, then exits (graceful); kill_sock after it to end the conn at once. */
static void out_stop(Out *o) { InterlockedExchange(&o->stop, 1); SetEvent(o->ev); }

/* Name lookup off the calling thread, bounded: the game polls EnumSessions from its UI loop, and a slow or dead DNS
 * server must not freeze it. The answer is cached (60 s, a failure 5 s); a lookup still running is waited on, never
 * restarted. state: 0 none, 1 running, 2 ok, 3 failed. Once a lookup has succeeded (good), its address is served at
 * once while later refreshes run in the background: a slow or failed refresh never empties the session list. */
static CRITICAL_SECTION dnscs;
static struct { char host[128]; int port, state, good; struct sockaddr_in addr; DWORD t; } DNS;

static DWORD WINAPI dns_thread(LPVOID a)
{
    char host[128], ps[16];
    int port, ok;
    struct addrinfo hints, *res = NULL;
    struct sockaddr_in sa;
    EnterCriticalSection(&dnscs);
    lstrcpynA(host, DNS.host, sizeof host);
    port = DNS.port;
    LeaveCriticalSection(&dnscs);
    sprintf(ps, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ok = !getaddrinfo(host, ps, &hints, &res) && res && res->ai_addrlen >= (int)sizeof sa;
    if (ok) memcpy(&sa, res->ai_addr, sizeof sa);
    if (res) freeaddrinfo(res);
    EnterCriticalSection(&dnscs);
    if (DNS.state == 1 && DNS.port == port && !strcmp(DNS.host, host)) {   /* still the target that was asked for */
        if (ok) { DNS.addr = sa; DNS.good = 1; }
        DNS.state = ok ? 2 : 3;
        DNS.t = GetTickCount();
    }
    LeaveCriticalSection(&dnscs);
    return 0;
}

static int resolve(const char *host, int port, struct sockaddr_in *out, DWORD wait_ms)
{
    DWORD start = GetTickCount(), age;
    unsigned long ip = inet_addr(host);
    int st;
    if (ip != INADDR_NONE) {   /* a numeric address needs no lookup */
        memset(out, 0, sizeof *out);
        out->sin_family = AF_INET;
        out->sin_addr.s_addr = ip;
        out->sin_port = htons((u_short)port);
        return 1;
    }
    EnterCriticalSection(&dnscs);
    age = GetTickCount() - DNS.t;
    if (DNS.port != port || strcmp(DNS.host, host)) DNS.good = 0;   /* a new target: the old address is not its */
    if (DNS.port != port || strcmp(DNS.host, host) || DNS.state == 0 ||
        (DNS.state == 2 && age > 60000) || (DNS.state == 3 && age > 5000)) {
        HANDLE th;
        lstrcpynA(DNS.host, host, sizeof DNS.host);
        DNS.port = port;
        DNS.state = 1;
        th = CreateThread(NULL, 0, dns_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
        else { DNS.state = 3; DNS.t = GetTickCount(); }
    }
    if (DNS.good) { *out = DNS.addr; LeaveCriticalSection(&dnscs); return 1; }
    while ((st = DNS.state) == 1) {
        LeaveCriticalSection(&dnscs);
        if (GetTickCount() - start >= wait_ms) { logf_("dial: lookup of '%s' still running after %lu ms", host, wait_ms); return 0; }
        Sleep(20);
        EnterCriticalSection(&dnscs);
    }
    if (st == 2) *out = DNS.addr;
    LeaveCriticalSection(&dnscs);
    return st == 2;
}

static SOCKET dial_to(const char *host, int port, DWORD ms)
{
    struct sockaddr_in sa;
    SOCKET s;
    u_long nb = 1;
    fd_set w, e;
    struct timeval tv;
    if (!resolve(host, port, &sa, ms)) {
        logf_("dial: cannot resolve '%s'", host);
        return INVALID_SOCKET;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { logf_("dial: socket() failed, WSA %d", WSAGetLastError()); return INVALID_SOCKET; }
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&sa, sizeof sa);
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
    if (select(0, NULL, &w, &e, &tv) <= 0 || !FD_ISSET(s, &w)) {
        logf_("dial: connect to %s:%d failed/timeout", host, port);
        if (inet_addr(host) == INADDR_NONE) {   /* may be stale: a refresh that is due or running is now waited on, not bypassed; a fresh (<60 s) one still serves */
            EnterCriticalSection(&dnscs);
            if (DNS.port == port && !strcmp(DNS.host, host)) DNS.good = 0;
            LeaveCriticalSection(&dnscs);
        }
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    tune_sock(s);
    logf_("dial: connected to %s:%d", host, port);
    return s;
}
static SOCKET dial(DWORD ms) { char host[128]; int port; get_target(host, &port); return dial_to(host, port, ms); }

/* ---- state helpers (cs held) ---- */
static int pl_count(void) { int i, n = 0; for (i = 0; i < MAXPL; i++) n += G.pl[i].used; return n; }
static Player *pl_find(DPID id) { int i; for (i = 0; i < MAXPL; i++) if (G.pl[i].used && G.pl[i].id == id) return &G.pl[i]; return NULL; }
static Player *pl_add(DPID id, const char *name, int conn)
{
    int i;
    for (i = 0; i < MAXPL; i++) if (!G.pl[i].used) {
        G.pl[i].used = 1; G.pl[i].id = id; G.pl[i].conn = conn; G.pl[i].lord = 0; G.pl[i].hello_t = 0; G.pl[i].hello_n = 0; G.pl[i].cp_next = 0; G.pl[i].cp_n = G.pl[i].cp_h = 0; memset(&G.pl[i].tr, 0, sizeof G.pl[i].tr);
        lstrcpynA(G.pl[i].name, name ? name : "", 32);
        return &G.pl[i];
    }
    return NULL;
}
/* returns 0 ok (or shed, see below), 1 queue full, 2 out of memory. force 1 (a player created): may pass QMAX by QSYS,
 * never more; callers check sys_room() BEFORE changing the player list, so a refused create changes nothing. force 2
 * (a player gone, session lost): always queued, or the game would keep a player we dropped; bounded, since each one
 * follows a create that was let in.
 * Shedding (review #3): net_emit_command 0x43eda0 sends most commands 3 times, copy 0 then the same bytes with slot
 * byte [3] | 0x40 and | 0x80. The receiver FUN_0043eac1 takes the first copy of a slot and rejects the others while
 * that slot entry is set (the "Resend Forced" counter 0x55ce68 is never set nonzero), and our transport keeps each
 * sender's order, so copy 0 always arrives first and is always taken. With QSHED messages already waiting, copies
 * 0x40/0x80 are not queued: the game would have dropped them anyway. Copy 0 and system messages are never shed. */
static DWORD g_qin, g_qout;   /* messages queued to / taken by our game so far (cs; trust.c review7 R7-4) */
static DWORD dg_us(void);
static void dg_on_rx(DPID from, const BYTE *d, DWORD len);
static int qpush(DPID from, DPID to, const BYTE *d, DWORD len, int force)
{
    Msg *m;
    if (!force && G.qcount >= QSHED && len >= 4 && (d[3] & 0xC0)) {
        G.qshed++;
        if (!G.qshed_logged) { G.qshed_logged = 1; logf_("receive queue at %d: the game's duplicate copies (0x40/0x80) are no longer queued until it drains", G.qcount); }
        return 0;
    }
    if (G.qshed_logged && G.qcount < QSHED / 2) { G.qshed_logged = 0; logf_("receive queue drained to %d (%lu duplicate copies not queued so far)", G.qcount, G.qshed); }
    if (force < 2 && G.qcount >= (force ? QMAX + QSYS : QMAX)) return 1;
    if (len > MAXRECV + 64) len = MAXRECV + 64;
    m = (Msg *)malloc(sizeof(Msg) + len);
    if (!m) { logf_("queue_push: out of memory - message dropped"); return 2; }
    m->next = NULL; m->from = from; m->to = to; m->len = len; m->t_us = dg_us();
    memcpy(m->data, d, len);
    dg_on_rx(from, d, len);
    if (G.qt) G.qt->next = m; else G.qh = m;
    G.qt = m;
    G.qcount++; g_qin++;
    if ((DWORD)G.qcount > G.qpeak) G.qpeak = (DWORD)G.qcount;
    return 0;
}
static int queue_push(DPID from, DPID to, const BYTE *d, DWORD len) { return qpush(from, to, d, len, 0); }
/* Review tb2 F4: a full queue means OUR game is not calling Receive (window dragged, a long load). Dropping the sender
 * punished a healthy peer; instead the reader stops reading that link until the game catches up (TCP pushes back on the
 * sender, whose writer queue holds 4 MB), and only after QWAIT the old end (host: drop that conn; joiner: host lost).
 * cs held; released while waiting. 0 queued, 1 still full after the wait, 2 no memory, 3 session or link ended meanwhile.
 * LOTR2NET_QWAIT_MS overrides the wait (selftest T10). */
static DWORD qwait_ms(void)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA("LOTR2NET_QWAIT_MS", v, sizeof v);
    return n && n < sizeof v ? (DWORD)atoi(v) : 120000u;
}
static int queue_push_wait(DPID from, DPID to, const BYTE *d, DWORD len, Out *o, int gen, const char *who)
{
    DWORD t0 = 0, lim = 0;
    int r;
    while ((r = queue_push(from, to, d, len)) == 1) {
        if (!t0) { t0 = GetTickCount() | 1; lim = qwait_ms(); logf_("receive queue full (%d): %s waits for the game to read (up to %lu ms)", QMAX, who, lim); }
        else if (GetTickCount() - t0 >= lim) return 1;
        LeaveCriticalSection(&cs);
        Sleep(20);
        EnterCriticalSection(&cs);
        if (G.gen != gen || o->dead || G.ghost) return 3;   /* v10 R12-3: never pushes into the queue ghost_go emptied */
    }
    if (t0 && !r) logf_("receive queue has room again after %lu ms - %s reads on", GetTickCount() - t0, who);
    return r;
}
static int sys_room(void) { return G.qcount < QMAX + QSYS; }
static void queue_clear(void) { while (G.qh) { Msg *m = G.qh; G.qh = m->next; free(m); } G.qt = NULL; G.qcount = 0; G.qfull_logged = 0; G.qshed_logged = 0; }
static int queue_sys(DWORD type, DPID id)
{
    int r;
    BYTE b[48];
    DWORD *d = (DWORD *)b;
    memset(b, 0, sizeof b);
    d[0] = type;
    if (type == DPSYS_CREATEPLAYERORGROUP) { d[1] = 1; d[2] = id; d[3] = (DWORD)pl_count(); d[6] = 16; r = qpush(0, G.myid, b, 48, 1); }
    /* DPMSG_DESTROYPLAYERORGROUP (dplay.h): dwType, dwPlayerType, dpId, lpLocalData, dwLocalDataSize, lpRemoteData +20,
     * dwRemoteDataSize +24, dpnName +28: dpnName.dwSize = 16, lpRemoteData NULL (was 16 at +20; review #11) */
    else if (type == DPSYS_DESTROYPLAYERORGROUP) { d[1] = 1; d[2] = id; d[5] = 0; d[7] = 16; r = qpush(0, G.myid, b, 44, 2); }
    else r = qpush(0, G.myid, b, 12, 2);
    if (r) logf_("SYS type=%lu id=%08lx NOT queued (%s)", type, id, r == 1 ? "queue full" : "no memory");
    else logf_("SYS queued type=%lu id=%08lx", type, id);
    return r;
}
static void bcast(int except, BYTE type, const void *p, DWORD n)
{
    int i;
    for (i = 0; i < MAXCONN; i++)   /* game traffic only to conns with a player: a joined-but-idle conn sees none */
        if (G.conns[i].used && i != except && (G.conns[i].haveplayer || (type != T_DATA && type != 10 /* T_SYNC */ && type != 11 /* T_RESYNC */)))
            osend(G.conns[i].o, type, p, n);
}
/* joiner: one frame to the host; a dead link ends the session's sends (cs held) */
static int jsend(BYTE type, const void *p, DWORD n)
{
    if (!G.connected) return -1;
    if (osend(G.jout, type, p, n)) { G.connected = 0; return -1; }
    return 0;
}

#include "sync.c"
#include "resync.c"
#include "trust.c"
#include "diag.c"
#include "sesslog.c"

/* ---- v10 ghost hub. The game asks for DPSESSION_MIGRATEHOST and handles DPSYS_HOST (FUN_004b8c3a ->
 * FUN_0043e6a2(4): that peer's game takes the host flag 0x553248 and the host duties). Our joiners reach only the hub
 * (star), so when the host's game leaves a locked game (its lord eliminated at turn 213 in Soak4, or it quit) this
 * process keeps relaying for the joiners instead of ending the game for all of them: no traffic to or from its own
 * game, no sync compare, resync or trust work (trust would drop the new game host's host-only messages), and the
 * lowest remaining player id is sent T_HOST so its DLL hands its game DPSYS_HOST. Ends when the last joiner leaves.
 * Off with ghost=0 in lotr2net.ini or LOTR2NET_GHOST=0. cs held unless said. */
static volatile LONG g_ghost_done;   /* the hub closed: a reader thread resets the window title outside cs */
static int ghost_enabled(void)   /* cs NOT held (ini read) */
{
    char v[16] = "";
    char *e = getenv("LOTR2NET_GHOST");
    if (e && *e) return cfg_on(e);
    if (ini_get("ghost", v, sizeof v)) return cfg_on(v);
    return 1;
}
static HWND g_ghost_w;            /* R14-1/2: the window the banner went on and its title before, for the restore */
static char g_ghost_old[160];
static volatile LONG g_tlock;      /* R15-1: banner and restore one at a time (no cs: the game thread may wait on cs) */
static void ghost_title(const char *t)   /* t = banner, NULL = restore. cs NOT held, any thread: SendMessageTimeout never
                                           * waits more than 1 s */
{
    DWORD pid = GetCurrentProcessId(), r = 0;
    HWND w = NULL;
    LRESULT ok = 0;
    while (InterlockedExchange(&g_tlock, 1)) Sleep(1);
    if (t && !G.ghost) {   /* R13-2/R15-1: the hub already closed (its restore ran or waits on this lock): no banner */
        InterlockedExchange(&g_tlock, 0);
        return;
    }
    if (t) {
        for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) {
            DWORD p = 0;
            GetWindowThreadProcessId(w, &p);
            if (p == pid && IsWindowVisible(w)) break;
        }
        g_ghost_old[0] = 0;
        if (w && !SendMessageTimeoutA(w, WM_GETTEXT, sizeof g_ghost_old, (LPARAM)g_ghost_old, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                      1000, (PDWORD_PTR)&r)) g_ghost_old[0] = 0;
        g_ghost_old[sizeof g_ghost_old - 1] = 0;
        if (strstr(g_ghost_old, "OUT OF SYNC")) g_ghost_old[0] = 0;   /* R15-2: restore a clean title, not a stale warning */
        g_ghost_w = w;
    } else {
        w = g_ghost_w && IsWindow(g_ghost_w) ? g_ghost_w : NULL;
        t = g_ghost_old[0] ? g_ghost_old : "Lords of the Realm II";
        g_ghost_w = NULL;
    }
    if (w) ok = SendMessageTimeoutA(w, WM_SETTEXT, 0, (LPARAM)t, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, (PDWORD_PTR)&r);
    InterlockedExchange(&g_tlock, 0);
    logf_("GHOST: window title %s: %s", !w ? "not set (no window)" : ok ? "set" : "not set (window busy 1 s)", t);   /* R15-3 */
}
static void ghost_elect(void)
{
    int k;
    DPID best = 0;
    for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn >= 0 && (!best || G.pl[k].id < best)) best = G.pl[k].id;
    if (!best || best == G.gamehost) return;
    G.gamehost = best;
    bcast(-1, T_HOST, &best, 4);
    logf_("GHOST: player %08lx is the game host now", (unsigned long)best);
}
static void ghost_check_end(void)
{
    int i;
    if (!G.ghost) return;
    for (i = 0; i < MAXCONN; i++) if (G.conns[i].used) return;
    G.ghost = 0; G.host = 0; G.locked = 0; G.gamehost = 0; G.gen++;
    memset(G.pl, 0, sizeof G.pl);
    queue_clear();   /* R12-3: nothing of the old game reaches the next one */
    InterlockedExchange(&g_ghost_done, 1);
    logf_("GHOST: the last player left - hub closed");
}
static int ghost_go(void)   /* do_close on the host of a locked game: 1 = became the hub */
{
    int k, n = 0;
    for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn >= 0) n++;
    if (!n) return 0;
    for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn < 0) {   /* our own player, if the game did not destroy it */
        DPID id = G.pl[k].id;
        G.pl[k].used = 0;
        bcast(-1, T_SYS_DESTROY, &id, 4);
    }
    if (G.lsock != INVALID_SOCKET) closesocket(G.lsock);   /* no new joiners; wakes the accept thread, which exits */
    G.lsock = INVALID_SOCKET;
    queue_clear();
    sy_reset();
    rs_reset();
    tr_reset();
    G.open = 0; G.ghost = 1; G.myid = 0; G.pidWaiting = 0; G.gamehost = 0;
    logf_("GHOST: our game left a locked game with %d player(s) still in it - relaying for them until the last one leaves", n);
    ghost_elect();
    return 1;
}

/* host: a connection ended or a player destroyed */
static void host_remove_player(Player *p, int except_conn)
{
    DPID id = p->id;
    p->used = 0;
    bcast(except_conn, T_SYS_DESTROY, &id, 4);
    if (G.ghost) { if (id == G.gamehost) ghost_elect(); return; }   /* no game of ours to tell */
    queue_sys(DPSYS_DESTROYPLAYERORGROUP, id);
}
static void host_drop_conn(int i)
{
    int k;
    if (!G.conns[i].used) return;
    kill_sock(G.conns[i].s);       /* reader and writer each let go of the Out; the last one closes the socket */
    out_stop(G.conns[i].o);
    G.conns[i].used = 0;
    for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn == i) host_remove_player(&G.pl[k], i);
    logf_("conn %d dropped", i);
    ghost_check_end();
}

static void fill_info(BYTE *b)
{
    *(DWORD *)(b + 0) = G.flags;
    *(DWORD *)(b + 4) = G.maxpl;
    *(DWORD *)(b + 8) = (DWORD)pl_count();
    memcpy(b + 12, &G.inst, 16);
    memset(b + 28, 0, 64);
    lstrcpynA((char *)b + 28, G.sname, 64);
}

/* ------------------------------------------------------- host: connections */
typedef struct { SOCKET s; int gen; Out *o; u_long ip; } CArg;

static void reject(SOCKET s, DWORD code) { sendf(s, T_REJECT, &code, 4); }
static int conns_pending(void)   /* accepted joiners that have not created their player yet (cs held) */
{
    int i, n = 0;
    for (i = 0; i < MAXCONN; i++) if (G.conns[i].used && !G.conns[i].haveplayer) n++;
    return n;
}
static int conns_pending_ip(u_long ip)   /* the same, from one address (cs held) */
{
    int i, n = 0;
    for (i = 0; i < MAXCONN; i++) if (G.conns[i].used && !G.conns[i].haveplayer && G.conns[i].ip == ip) n++;
    return n;
}
/* in-flight handshakes per address (F3), own lock: the accept thread and the conn threads */
static CRITICAL_SECTION hscs;
static struct { u_long ip; int n; } hs_ip[MAXHS];
static int hs_ip_take(u_long ip)   /* 1 = allowed and counted */
{
    int i, fr = -1, ok = 0;
    EnterCriticalSection(&hscs);
    for (i = 0; i < MAXHS; i++) if (hs_ip[i].n && hs_ip[i].ip == ip) break;
    if (i < MAXHS) { if (hs_ip[i].n < MAXHS_IP) { hs_ip[i].n++; ok = 1; } }
    else {
        for (i = 0; i < MAXHS; i++) if (!hs_ip[i].n) { fr = i; break; }
        if (fr >= 0) { hs_ip[fr].ip = ip; hs_ip[fr].n = 1; ok = 1; }
    }
    LeaveCriticalSection(&hscs);
    return ok;
}
static void hs_ip_done(u_long ip)
{
    int i;
    EnterCriticalSection(&hscs);
    for (i = 0; i < MAXHS; i++) if (hs_ip[i].n && hs_ip[i].ip == ip) { hs_ip[i].n--; break; }
    LeaveCriticalSection(&hscs);
}

static DWORD WINAPI conn_thread(LPVOID a)
{
    CArg *ca = (CArg *)a;
    SOCKET s = ca->s;
    int gen = ca->gen, ci = -1, i, have = 0, tmo = 0;
    DWORD tj;
    BYTE type, *f, info[92];
    DWORD n, ver, mode;
    Out *o;
    u_long ip = ca->ip;
    free(ca);
    tune_sock(s);
    f = readf2(s, &type, &n, 64, 5000);   /* hello only: <= 64 bytes, 5 s absolute */
    InterlockedDecrement(&hs_count);      /* the pre-auth blocking phase is over */
    hs_ip_done(ip);
    if (!f || type != T_HELLO || n < 16 || memcmp(f, MAGIC, 8)) {
        logf_("handshake: bad hello (n=%lu) - rejecting", f ? n : 0);
        if (f) { reject(s, DPERR_UNAVAILABLE); free(f); }
        closesocket(s);
        return 0;
    }
    ver = *(DWORD *)(f + 8);
    mode = *(DWORD *)(f + 12);
    free(f);
    if (ver != PROTO && mode == MODE_ENUM) {
        int ok;
        EnterCriticalSection(&cs);
        ok = G.open && G.host && G.gen == gen && !G.locked;
        if (ok) {
            fill_info(info);
            snprintf((char *)info + 28, 64, "%.24s [host v%lu, yours v%lu]", G.sname, PROTO, ver);
        }
        LeaveCriticalSection(&cs);
        logf_("enum from a different build (peer=%lu mine=%lu) - listed as incompatible", ver, PROTO);
        if (ok) sendf(s, T_INFO, info, 92); else reject(s, DPERR_NOSESSIONS);
        closesocket(s);
        return 0;
    }
    if (ver != PROTO) {
        logf_("handshake: version mismatch peer=%lu mine=%lu (PROTO digits: thousands = dll version, hundreds = pace patch applied, tens = 2*exe_ok + slowlink where exe_ok = running exe is the known 5 Jun 1997 build, units = mpfix 1/0; both players need the same build, the same exe, the same LOTR2NET_SLOWLINK and the same mpfix setting) - rejecting", ver, PROTO);
        reject(s, DPERR_UNAVAILABLE);
        closesocket(s);
        return 0;
    }
    if (mode != MODE_ENUM && mode != MODE_JOIN) {
        logf_("handshake: unknown mode %lu - rejecting", mode);
        reject(s, DPERR_UNAVAILABLE);
        closesocket(s);
        return 0;
    }
    set_rcvto(s, 0);
    EnterCriticalSection(&cs);
    if (!G.open || !G.host || G.gen != gen) {
        LeaveCriticalSection(&cs);
        reject(s, DPERR_NOSESSIONS);
        closesocket(s);
        return 0;
    }
    if (mode == MODE_ENUM) {
        int locked = G.locked;
        fill_info(info);
        LeaveCriticalSection(&cs);
        if (locked) reject(s, DPERR_NOSESSIONS); else sendf(s, T_INFO, info, 92);   /* direct: an enum conn has no writer */
        logf_("enum request answered (locked=%d)", locked);
        closesocket(s);
        return 0;
    }
    if (G.locked || (DWORD)(pl_count() + conns_pending()) >= G.maxpl || conns_pending_ip(ip) >= MAXPEND_IP) {
        LeaveCriticalSection(&cs);
        logf_("join refused: locked=%d players=%d pending from this address=%d", G.locked, pl_count(), conns_pending_ip(ip));
        reject(s, DPERR_ACTIVEPLAYERS);
        closesocket(s);
        return 0;
    }
    for (i = 0; i < MAXCONN; i++) if (!G.conns[i].used) break;
    if (i == MAXCONN || !(o = out_new(s, "conn"))) {
        LeaveCriticalSection(&cs);
        if (i < MAXCONN) logf_("join refused: no writer thread/memory");
        reject(s, DPERR_ACTIVEPLAYERS);
        closesocket(s);
        return 0;
    }
    ci = i;
    snprintf(o->tag, sizeof o->tag, "conn %d", ci);
    G.conns[ci].used = 1;
    G.conns[ci].s = s;
    G.conns[ci].o = o;
    G.conns[ci].haveplayer = 0;
    G.conns[ci].ncreate = 0;
    G.conns[ci].ip = ip;
    G.conns[ci].lord = 0; G.conns[ci].bad_lord = 0;   /* resync.c: bound at its first record */
    G.conns[ci].sync_n = 0; G.conns[ci].sync_t = 0;
    G.conns[ci].rs_n = 0; G.conns[ci].rs_t = 0;
    G.conns[ci].dg_n = 0; G.conns[ci].dg_t = 0;
    G.conns[ci].dt_tok = DATA_BURST; G.conns[ci].dt_over = 0; G.conns[ci].dt_t = GetTickCount(); G.conns[ci].dt_rate = data_rate();
    InterlockedIncrement(&g_live);
    tj = GetTickCount();
    fill_info(info);
    osend(o, T_INFO, info, 92);
    logf_("joiner connected as conn %d", ci);
    LeaveCriticalSection(&cs);

    for (;;) {
        /* after kill_sock: straight to the drop, no buffered frame after it. No player yet: JOIN_WAIT_MS in all,
         * so an idle conn cannot hold a lobby slot */
        if (o->dead) f = NULL;
        else if (!have) {
            DWORD el = GetTickCount() - tj;
            f = el >= JOIN_WAIT_MS ? NULL : readf2(s, &type, &n, 70000, JOIN_WAIT_MS - el);
            tmo = 1;
            if (!f && el >= JOIN_WAIT_MS - 50) logf_("conn %d made no player in %d ms - dropped", ci, JOIN_WAIT_MS);
        } else {
            if (tmo) { set_rcvto(s, 0); tmo = 0; }
            f = readf(s, &type, &n);
        }
        EnterCriticalSection(&cs);
        if (!f || G.gen != gen || !G.conns[ci].used || G.conns[ci].o != o) {
            if (G.gen == gen && G.conns[ci].used && G.conns[ci].o == o) host_drop_conn(ci);
            else { kill_sock(s); out_stop(o); }   /* a closed session (do_close) already let the writer go */
            LeaveCriticalSection(&cs);
            free(f);
            logf_("conn %d reader done (outbound peak %lu bytes)", ci, o->peak);
            if (g_ghost_done && InterlockedExchange(&g_ghost_done, 0)) ghost_title(NULL);
            out_release(o);   /* the socket is closed by whichever of reader/writer lets go last */
            InterlockedDecrement(&g_live);
            return 0;
        }
        if (type == T_CREATEPL && n >= 32) {
            DPID id = 0;
            Player *p = NULL;
            if (G.locked || G.conns[ci].haveplayer || G.conns[ci].ncreate >= MAXCREATE || !sys_room()) {
                logf_("CREATEPL refused on conn %d (locked=%d, already has player=%d, created=%d, queue=%d)", ci,
                      G.locked, G.conns[ci].haveplayer, G.conns[ci].ncreate, G.qcount);
                osend(o, T_PLAYERID, &id, 4);
            } else if ((DWORD)pl_count() < G.maxpl && (p = pl_add(G.nextid, (char *)f, ci)) != NULL &&
                       queue_sys(DPSYS_CREATEPLAYERORGROUP, G.nextid)) {
                p->used = 0;   /* no memory for the game's event: undo before anyone is told, refuse with id 0 */
                logf_("CREATEPL on conn %d: create event not queued - refused", ci);
                osend(o, T_PLAYERID, &id, 4);
            } else if (p) {
                int k;
                id = G.nextid++;
                G.conns[ci].haveplayer = 1;
                G.conns[ci].ncreate++;
                osend(o, T_PLAYERID, &id, 4);
                for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].id != id) {
                    BYTE m[36];
                    *(DWORD *)m = G.pl[k].id;
                    memcpy(m + 4, G.pl[k].name, 32);
                    osend(o, T_SYS_CREATE, m, 36);
                }
                { BYTE m[36]; *(DWORD *)m = id; memcpy(m + 4, p->name, 32); bcast(ci, T_SYS_CREATE, m, 36); }
                logf_("player %08lx '%s' created for conn %d", id, p->name, ci);
                lf_begin();
            } else osend(o, T_PLAYERID, &id, 4);
        } else if (type == T_SYNC && n >= SY_HDR && n <= SY_HDR + SYNC_MAXR * 4) {
            if (G.conns[ci].haveplayer) {   /* compare, relay to the others; a flood is cut at SYNC_PER_S */
                DWORD now = GetTickCount();
                if (now - G.conns[ci].sync_t >= 1000) { G.conns[ci].sync_t = now; G.conns[ci].sync_n = 0; }
                if (++G.conns[ci].sync_n <= SYNC_PER_S) {
                    /* review #10: the record's player word is stamped with this conn's DPID before compare and relay,
                     * so one peer cannot speak for another */
                    int k;
                    DPID cid = 0;
                    for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn == ci) { cid = G.pl[k].id; break; }
                    /* resync authority: the lord byte must be the one lord bound to this conn, or the record is dropped */
                    if (G.ghost) {   /* ghost hub: stamp and relay, the peers compare among themselves */
                        ((DWORD *)f)[5] = (cid << 8) | (((DWORD *)f)[5] & 0xff);
                        bcast(ci, T_SYNC, f, n);
                    } else if (rs_conn_lord(ci, cid, ((DWORD *)f)[5] & 0xff, "sync record")) {
                        ((DWORD *)f)[5] = (cid << 8) | (((DWORD *)f)[5] & 0xff);
                        if (sy_on_peer(f, n)) bcast(ci, T_SYNC, f, n);
                    }
                } else if (G.conns[ci].sync_n == SYNC_PER_S + 1) logf_("conn %d: over %d sync records in 1 s - extra dropped", ci, SYNC_PER_S);
            }
        } else if (type == T_RESYNC && n >= 8 && n <= 8 + 0x300) {   /* resync.c: a joiner's upload of the records it is the authority for */
            if (G.conns[ci].haveplayer && !G.ghost) {   /* ghost hub: no repair without a game here. Per-conn budget like T_SYNC: an upload burst is at most 173 frames, a flood is cut here */
                DWORD now = GetTickCount();
                if (now - G.conns[ci].rs_t >= 1000) { G.conns[ci].rs_t = now; G.conns[ci].rs_n = 0; }
                if (++G.conns[ci].rs_n <= RESYNC_PER_S) {
                    int k;
                    DPID cid = 0;
                    for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn == ci) { cid = G.pl[k].id; break; }
                    rs_on_up(ci, cid, f, n);
                } else if (G.conns[ci].rs_n == RESYNC_PER_S + 1) SLOG("conn %d: over %d resync frames in 1 s - extra dropped", ci, RESYNC_PER_S);
            }
        } else if ((type == T_PING || type == T_PONG) && G.conns[ci].haveplayer) {
            dg_on_frame(ci, type, f, n);   /* diag.c: answered or relayed here, never queued for the game */
        } else if (type == T_DESTROYPL && n >= 4) {
            Player *p = pl_find(*(DWORD *)f);
            if (p && p->conn == ci) { host_remove_player(p, ci); G.conns[ci].haveplayer = 0; }
        } else if (type == T_DATA && n >= 8) {
            DPID from = ((DWORD *)f)[0], to = ((DWORD *)f)[1];
            DWORD len = n - 8;
            Player *src = pl_find(from);
            if (!src || src->conn != ci) {   /* a peer may only speak for players it created */
                logf_("spoofed from=%08lx on conn %d - dropped", (unsigned long)from, ci);
                LeaveCriticalSection(&cs);
                free(f);
                continue;
            }
            if (len > MAXRECV) {   /* the game cannot receive it: drop, never queue or relay */
                logf_("oversize data %lu > %d from=%08lx on conn %d - dropped", len, MAXRECV, (unsigned long)from, ci);
                LeaveCriticalSection(&cs);
                free(f);
                continue;
            }
            logmsg("RELAY-IN", from, to, f + 8, len);
            {   /* token bucket: a flood is cut at DATA_PER_S per conn (review2-trust F7; the game's own peak is far lower) */
                Conn *c = &G.conns[ci];
                DWORD now = GetTickCount(), el = now - c->dt_t;
                if (c->dt_rate <= 0) c->dt_tok = 1;
                else if (el >= 5) {
                    DWORD add = el * (DWORD)c->dt_rate / 1000;
                    c->dt_tok = (int)((DWORD)c->dt_tok + add > DATA_BURST ? DATA_BURST : (DWORD)c->dt_tok + add);
                    c->dt_t += add * 1000 / (DWORD)c->dt_rate;
                }
                if (c->dt_tok <= 0) {
                    if (!c->dt_over++ || !(c->dt_over % 1000)) SLOG("TRUST DROPPED from %08lx on conn %d: over %d game messages per second (%d dropped)", (unsigned long)from, ci, c->dt_rate, c->dt_over);
                    LeaveCriticalSection(&cs);
                    free(f);
                    continue;
                }
                c->dt_tok--;
            }
            if (!tr_check(ci, from, to, f + 8, len)) {   /* v9 peer trust (trust.c): never relayed, never queued. In ghost
                                                           * (v10) only its static checks run; the T_HOST player may send host-only types */
                LeaveCriticalSection(&cs);
                free(f);
                continue;
            }
            if (G.ghost) {   /* ghost hub: relay only, nothing for this process's game */
                if (to == 0) bcast(ci, T_DATA, f, n);
                else { Player *p = pl_find(to); if (p && p->conn >= 0) osend(G.conns[p->conn].o, T_DATA, f, n); }
            } else if (to == 0) {
                int q = queue_push_wait(from, to, f + 8, len, o, gen, o->tag);
                if (q == 3 && G.ghost && G.conns[ci].used && G.conns[ci].o == o) bcast(ci, T_DATA, f, n);   /* R13-1: the ghost
                                                                   * started while it waited: the others still need it */
                else if (q == 3 || !G.conns[ci].used || G.conns[ci].o != o) {}   /* ended while waiting: the loop top drops it */
                else if (q) {   /* still full after QWAIT, or out of memory: never relay what we dropped */
                    logf_("receive queue full or no memory (%d) - shutting conn %d down", QMAX, ci);
                    InterlockedExchange(&o->dead, 1);
                    kill_sock(s);
                } else bcast(ci, T_DATA, f, n);
            } else {
                Player *p = pl_find(to);
                if (p && p->conn < 0) {
                    int q = queue_push_wait(from, to, f + 8, len, o, gen, o->tag);
                    if (q && q != 3 && G.conns[ci].used && G.conns[ci].o == o) {
                        logf_("receive queue full or no memory (%d) - shutting conn %d down", QMAX, ci);
                        InterlockedExchange(&o->dead, 1);
                        kill_sock(s);
                    }
                } else if (p && p->conn >= 0) osend(G.conns[p->conn].o, T_DATA, f, n);
            }
        }
        if (have && !G.conns[ci].haveplayer) tj = GetTickCount();   /* destroyed its player: the wait starts again */
        have = G.conns[ci].haveplayer;
        LeaveCriticalSection(&cs);
        free(f);
    }
}

static DWORD accept_body(LPVOID a)
{
    CArg *aa = (CArg *)a;
    SOCKET l = aa->s;
    int gen = aa->gen;
    free(aa);
    for (;;) {
        struct sockaddr_in pa;
        int pl = sizeof pa;
        SOCKET s = accept(l, (struct sockaddr *)&pa, &pl);
        CArg *ca;
        HANDLE th;
        int cur;
        if (s == INVALID_SOCKET) {
            int err = WSAGetLastError();
            EnterCriticalSection(&cs);
            cur = G.gen == gen && G.lsock == l;   /* R12-4: ghost_go closes the listener without a gen change */
            LeaveCriticalSection(&cs);
            if (!cur || err == WSAENOTSOCK || err == WSAEINTR || err == WSAEINVAL) return 0;
            logf_("accept error WSA %d - retrying", err);
            Sleep(50);
            continue;
        }
        EnterCriticalSection(&cs);
        cur = G.gen;
        LeaveCriticalSection(&cs);
        if (cur != gen) { closesocket(s); return 0; }
        if (pl < (int)sizeof pa || pa.sin_family != AF_INET) pa.sin_addr.s_addr = 0;
        if (InterlockedIncrement(&hs_count) > MAXHS) {
            InterlockedDecrement(&hs_count);
            logf_("too many handshakes in flight - connection closed");
            closesocket(s);
            continue;
        }
        if (!hs_ip_take(pa.sin_addr.s_addr)) {   /* F3: one address may not hold every handshake slot */
            static int nlog;
            InterlockedDecrement(&hs_count);
            if (++nlog <= 5 || !(nlog % 100)) logf_("over %d handshakes in flight from %s - connection closed (%d so far)", MAXHS_IP, inet_ntoa(pa.sin_addr), nlog);
            closesocket(s);
            continue;
        }
        ca = (CArg *)malloc(sizeof *ca);
        if (!ca) { InterlockedDecrement(&hs_count); hs_ip_done(pa.sin_addr.s_addr); closesocket(s); continue; }
        ca->s = s; ca->gen = gen; ca->ip = pa.sin_addr.s_addr;
        th = CreateThread(NULL, 0, conn_thread, ca, 0, NULL);
        if (!th) {
            logf_("CreateThread failed for a connection (%lu) - closed", GetLastError());
            InterlockedDecrement(&hs_count);
            hs_ip_done(pa.sin_addr.s_addr);
            free(ca);
            closesocket(s);
            continue;
        }
        CloseHandle(th);
    }
}

static DWORD WINAPI accept_thread(LPVOID a) { DWORD r = accept_body(a); InterlockedDecrement(&g_live); return r; }

/* ------------------------------------------------------ joiner: reader */
static DWORD join_body(LPVOID a)
{
    CArg *ja = (CArg *)a;
    SOCKET s = ja->s;
    int gen = ja->gen;
    Out *o = ja->o;
    free(ja);
    for (;;) {
        BYTE type, *f;
        DWORD n;
        int q;
        f = o->dead ? NULL : readf(s, &type, &n);   /* after kill_sock: straight to host lost */
        if (f && type == T_RESYNC) {   /* resync.c: file chunks, merged and written outside cs (this thread only) */
            rs_on_frame(f, n, gen);
            free(f);
            continue;
        }
        EnterCriticalSection(&cs);
        if (G.gen != gen) { LeaveCriticalSection(&cs); free(f); break; }
        if (!f) {
            int k;
            logf_("host connection lost");
            G.connected = 0;
            for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].id != G.myid) {
                DPID id = G.pl[k].id;
                G.pl[k].used = 0;
                queue_sys(DPSYS_DESTROYPLAYERORGROUP, id);
            }
            queue_sys(DPSYS_SESSIONLOST, 0);
            SetEvent(G.evPid);
            LeaveCriticalSection(&cs);
            break;
        }
        if (type == T_PLAYERID && n >= 4) {
            DPID id = *(DWORD *)f;
            if (G.pidWaiting) {
                G.pidResult = id;
                if (id) G.myid = id;   /* review #13: create events queued from now on carry to = our id, not 0 */
                SetEvent(G.evPid);
            } else if (id) {
                /* F20: CreatePlayer gave up waiting; the host made the player anyway. Destroy that ghost so the host
                 * and the other peers do not keep a player this game never knew it had. */
                logf_("late PLAYERID %08lx after CreatePlayer gave up - asking the host to destroy it", id);
                jsend(T_DESTROYPL, &id, 4);
            }
        } else if (type == T_SYS_CREATE && n >= 36) {
            DPID id = *(DWORD *)f;
            /* a player we already know is not news to the game: no second create event. No room: end the
             * session before the list changes, so the game and our list never disagree */
            if (!pl_find(id)) {
                if (!sys_room()) {
                    logf_("system queue full - treating it as host lost");
                    InterlockedExchange(&o->dead, 1); kill_sock(s);
                } else if (pl_add(id, (char *)f + 4, -1) && queue_sys(DPSYS_CREATEPLAYERORGROUP, id)) {
                    logf_("create not queued (no memory) - treating it as host lost");
                    InterlockedExchange(&o->dead, 1); kill_sock(s);
                }
            }
        } else if (type == T_SYS_DESTROY && n >= 4) {
            Player *p = pl_find(*(DWORD *)f);
            if (p) {
                p->used = 0;
                if (queue_sys(DPSYS_DESTROYPLAYERORGROUP, *(DWORD *)f)) {
                    logf_("destroy not queued (no memory) - treating it as host lost");
                    InterlockedExchange(&o->dead, 1); kill_sock(s);
                }
            }
        } else if (type == T_SYNC && n >= SY_HDR && n <= SY_HDR + SYNC_MAXR * 4) {
            sy_on_peer(f, n);
        } else if (type == T_PING || type == T_PONG) {
            dg_on_frame(-1, type, f, n);
        } else if (type == T_HOST && n >= 4) {   /* v10: the hub's game left; this player plays the game host now */
            DPID id = *(DWORD *)f;
            logf_("game host is now player %08lx%s", (unsigned long)id, id == G.myid ? " - this game (DPSYS_HOST)" : "");
            if (id == G.myid && G.gamehost != id && queue_sys(DPSYS_HOST, 0)) {
                logf_("host event not queued - treating it as host lost");
                InterlockedExchange(&o->dead, 1); kill_sock(s);
            }
            G.gamehost = id;
        } else if (type == T_DATA && n >= 8) {
            if (n - 8 > MAXRECV) logf_("oversize data %lu > %d from host - dropped", n - 8, MAXRECV);
            else if ((q = queue_push_wait(((DWORD *)f)[0], ((DWORD *)f)[1], f + 8, n - 8, o, gen, "host link")) != 0 && q != 3) {
                /* still full after QWAIT: dropping a game message is a certain desync, end the session cleanly instead */
                logf_("receive queue full or no memory (%d) - treating it as host lost", QMAX);
                InterlockedExchange(&o->dead, 1); kill_sock(s);
            }
        }
        LeaveCriticalSection(&cs);
        free(f);
    }
    kill_sock(s);
    out_stop(o);
    logf_("host link reader done (outbound peak %lu bytes)", o->peak);
    out_release(o);   /* the socket is closed by whichever of reader/writer lets go last */
    return 0;
}

static DWORD WINAPI join_thread(LPVOID a) { DWORD r = join_body(a); InterlockedDecrement(&g_live); return r; }

/* ------------------------------------------------------------ host: listener */
/* Exclusive use first: no other socket may share our LISTENING port. If that bind reports "in use", retry once with
 * no option (never SO_REUSEADDR: on Windows and Wine it lets a second listener share the port). Re-host within
 * seconds, with the last session's conns in TIME_WAIT on 47700, binds on the FIRST try on Windows 11 and Wine 11
 * (test/selftest.c T6, 2026-10-06); the retry is for stacks that refuse it with WSAEADDRINUSE. */
/* LOTR2NET_BIND=<ipv4> limits the listener to one local address (selftest uses 127.0.0.1 so Windows Firewall never
 * asks about a loopback-only test); unset or invalid = every address, as players need. */
static u_long listen_addr(void)
{
    char *e = getenv("LOTR2NET_BIND");
    u_long ip = (e && *e) ? inet_addr(e) : INADDR_NONE;
    return ip != INADDR_NONE ? ip : htonl(INADDR_ANY);
}
static SOCKET open_listener(int port)
{
    static const int mode[2] = {1, 0};   /* 1 exclusive, 0 none */
    int k;
    for (k = 0; k < 2; k++) {
        SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        int one = 1, err;
        if (l == INVALID_SOCKET) { logf_("Open(create): socket() failed, WSA %d", WSAGetLastError()); return INVALID_SOCKET; }
        if (mode[k] == 1) setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = listen_addr();
        a.sin_port = htons((u_short)port);
        if (!bind(l, (struct sockaddr *)&a, sizeof a) && !listen(l, 8)) {
            if (k) logf_("Open(create): port %d bound on retry (mode %d)", port, mode[k]);
            return l;
        }
        err = WSAGetLastError();
        logf_("Open(create): bind/listen port %d failed (mode %d), WSA %d", port, mode[k], err);
        closesocket(l);
        if (err != WSAEADDRINUSE) break;   /* never on WSAEACCES: that is another socket holding the port with REUSEADDR */
    }
    return INVALID_SOCKET;
}

/* ------------------------------------------------------------ IDirectPlay2 */
typedef struct { void **vt; } Obj;
static Obj dp2obj, dp1obj;
static LONG dp2ref, dp1ref;

/* Never closesocket a handle a reader may still be in recv on: a quick re-Open could get the same handle value and the
 * old reader would steal its bytes. Every conn/joiner socket belongs to its Out: the reader and the writer each hold a
 * ref and the last to let go closes it (review #5: nothing here closes a socket a thread may still use). The listener
 * is closed here (that is what wakes accept). What is already queued gets up to 1 s to leave (the game's last
 * messages), then every link is shut down and the threads are waited on, 3 s at most.
 * may_ghost (v10): only the game's Close / Release (and the test hook) may turn a locked host game into the ghost hub;
 * Open's own close of a still-open session ends it for everyone (R12-1: a ghost under a new session sent nothing). */
static void do_close(int may_ghost)
{
    int i, no = 0, ghost_ok = may_ghost && ghost_enabled();
    DWORD t0;
    Out *os[MAXCONN + 1];
    EnterCriticalSection(&cs);
    if (G.ghost) { LeaveCriticalSection(&cs); return; }   /* v10: hub for the last game; nothing of this game is left */
    if (G.open && G.host && G.locked && ghost_ok && ghost_go()) {
        for (i = 0; i < MAXCONN; i++) if (G.conns[i].used) {   /* R12-2: hold each link until DESTROY + T_HOST have left */
            InterlockedIncrement(&G.conns[i].o->refs);
            os[no++] = G.conns[i].o;
        }
        LeaveCriticalSection(&cs);
        beacon_stop();
        for (t0 = GetTickCount(); GetTickCount() - t0 < 1000;) {
            int busy = 0;
            for (i = 0; i < no; i++) {
                EnterCriticalSection(&os[i]->oc);
                if (os[i]->h && !os[i]->dead) busy = 1;
                LeaveCriticalSection(&os[i]->oc);
            }
            if (!busy) break;
            Sleep(5);
        }
        for (i = 0; i < no; i++) out_release(os[i]);
        ghost_title(   /* R13-2: skipped inside if the last player left during the wait */
            "Lords of the Realm II - still hosting the last game for the other players: keep this window open");
        return;
    }
    if (G.open) {
        G.gen++;
        if (G.host) {
            if (G.lsock != INVALID_SOCKET) closesocket(G.lsock);
            for (i = 0; i < MAXCONN; i++) if (G.conns[i].used) {
                Out *o = G.conns[i].o;
                InterlockedIncrement(&o->refs);   /* ours until the drain wait is over */
                out_stop(o);
                os[no++] = o;
                G.conns[i].used = 0;
            }
        } else if (G.jout) {
            out_stop(G.jout);
            os[no++] = G.jout;                     /* G's own ref moves to this list */
        }
        G.jout = NULL;
        G.lsock = G.jsock = INVALID_SOCKET;
        memset(G.pl, 0, sizeof G.pl);
        queue_clear();
        sy_reset();
        rs_reset();
        tr_reset();
        G.open = 0; G.host = 0; G.locked = 0; G.connected = 0; G.myid = 0; G.pidWaiting = 0; G.gamehost = 0;
    }
    LeaveCriticalSection(&cs);
    beacon_stop();
    for (t0 = GetTickCount(); GetTickCount() - t0 < 1000;) {   /* let queued frames leave */
        int busy = 0;
        for (i = 0; i < no; i++) if (!os[i]->wdone) busy = 1;
        if (!busy) break;
        Sleep(5);
    }
    for (i = 0; i < no; i++) { kill_sock(os[i]->s); out_release(os[i]); }
    for (t0 = GetTickCount(); g_live > 0 && GetTickCount() - t0 < 3000;) Sleep(5);
    if (g_live > 0) logf_("Close: %ld network thread(s) still running after 3 s (their sockets close when they exit)", g_live);
    lf_end();
}

static DPHR WINAPI dp2_QI(Obj *t, const GUID *iid, void **out)
{
    logf_("QueryInterface");
    if (!iid || !out) return DPERR_INVALIDPARAMS;
    if (IsEqualGUID(iid, &IID_DP2) || IsEqualGUID(iid, &IID_DP2A) || IsEqualGUID(iid, &IID_IUnknown_)) {
        InterlockedIncrement(&dp2ref);
        *out = &dp2obj;
        return DP_OK;
    }
    *out = NULL;
    return (DPHR)0x80004002; /* E_NOINTERFACE */
}
static ULONG WINAPI dp2_AddRef(Obj *t) { return (ULONG)InterlockedIncrement(&dp2ref); }
static ULONG WINAPI dp2_Release(Obj *t)
{
    LONG r = InterlockedDecrement(&dp2ref);
    logf_("Release -> %ld", r);
    if (r <= 0) { dp2ref = 0; do_close(1); }
    return (ULONG)(r < 0 ? 0 : r);
}
static DPHR WINAPI dp2_Close(Obj *t) { logf_("Close"); do_close(1); return DP_OK; }

/* Review #7: ddraw's [display] background=0 pauses this game while its window is unfocused, and in lockstep every
 * other player then waits. ddraw owns that setting (forcing it on in a session is a ddraw change, see dplayx/AGENTS.md);
 * here it is at least said, once per session, in the log. */
static void bg_warn(void)
{
    char p[MAX_PATH + 16], v[16];
    lstrcpynA(p, exedir, MAX_PATH);
    lstrcatA(p, "lotr2fix.ini");
    GetPrivateProfileStringA("display", "background", "1", v, sizeof v, p);
    if (!strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "no") || !_stricmp(v, "false"))
        logf_("WARN lotr2fix.ini [display] background=%s: this game pauses when its window loses focus, and every other "
              "player waits for it; set background=1 or auto for multiplayer", v);
}

static DPHR WINAPI dp2_Open(Obj *t, DPSD2 *d, DWORD flags)
{
    logf_("Open flags=%lu max=%lu name='%s'", flags, d ? d->dwMaxPlayers : 0,
          d && d->lpszSessionName ? d->lpszSessionName : "");
    if (!d) return DPERR_INVALIDPARAMS;
    ensure_init();
    if ((flags & DPOPEN_CREATE) && d->lpszPassword && d->lpszPassword[0]) {
        logf_("Open(create): session passwords are not supported - refused");
        return DPERR_UNSUPPORTED;
    }
    EnterCriticalSection(&cs);
    if (G.ghost) {
        LeaveCriticalSection(&cs);
        logf_("Open refused: still hosting the last game for its other players (it ends when they all leave)");
        if (g_menu) snprintf(g_menu_msg, sizeof g_menu_msg, "This PC still hosts the last game for its other players");
        return DPERR_UNAVAILABLE;
    }
    LeaveCriticalSection(&cs);
    if (G.open) do_close(0);
    bg_warn();
    if (flags & DPOPEN_CREATE) {
        int port = listen_port();
        SOCKET l;
        CArg *aa = (CArg *)malloc(sizeof *aa);
        int aagen;
        HANDLE th;
        if (!aa) return DPERR_GENERIC;
        l = open_listener(port);
        if (l == INVALID_SOCKET) { free(aa); return DPERR_CANTCREATESESSION; }
        EnterCriticalSection(&cs);
        memset(G.sname, 0, sizeof G.sname);
        if (d->lpszSessionName) lstrcpynA(G.sname, d->lpszSessionName, 64);
        G.maxpl = d->dwMaxPlayers ? d->dwMaxPlayers : 5;
        G.flags = d->dwFlags;
        G.inst = d->guidInstance;
        if (!G.inst.Data1) { G.inst = d->guidApplication; G.inst.Data1 ^= GetTickCount() | 1; }
        G.lsock = l; G.jsock = INVALID_SOCKET;
        G.host = 1; G.open = 1; G.locked = 0; G.nextid = 0x1000; G.myid = 0;
        G.gen++;
        aa->s = l; aa->gen = aagen = G.gen;
        LeaveCriticalSection(&cs);
        InterlockedIncrement(&g_live);   /* before the thread runs: a Close right now must still wait for it */
        th = CreateThread(NULL, 0, accept_thread, aa, 0, NULL);
        if (!th) {
            logf_("Open(create): CreateThread failed (%lu)", GetLastError());
            InterlockedDecrement(&g_live);
            free(aa);
            do_close(0);
            return DPERR_CANTCREATESESSION;
        }
        CloseHandle(th);
        logf_("Open(create) listening on %d name='%s' max=%lu", port, G.sname, G.maxpl);
        beacon_start(port);
        grhb_start(port);
        dg_start(aagen);
        return DP_OK;
    } else {
        SOCKET s;
        BYTE h[16], type, *f;
        DWORD n;
        CArg *ja;
        Out *jo;
        HANDLE th;
        int jagen;
        s = dial(2500);
        if (s == INVALID_SOCKET) {
            if (g_menu == 1) snprintf(g_menu_msg, sizeof g_menu_msg, "Could not reach the host");
            return DPERR_NOCONNECTION;
        }
        memcpy(h, MAGIC, 8);
        *(DWORD *)(h + 8) = PROTO;
        *(DWORD *)(h + 12) = MODE_JOIN;
        sendf(s, T_HELLO, h, 16);
        f = readf2(s, &type, &n, 256, 4000);   /* INFO or REJECT, 4 s in all (a trickling peer cannot hang the game) */
        set_rcvto(s, 0);
        if (!f || type == T_REJECT || type != T_INFO || n < 92) {
            DWORD code = (f && type == T_REJECT && n >= 4) ? *(DWORD *)f : (DWORD)DPERR_UNAVAILABLE;
            if (!(code & 0x80000000u)) code = (DWORD)DPERR_UNAVAILABLE;   /* never hand back a success-shaped code from the wire */
            logf_("Open(join) rejected/failed, code=%08lx", code);
            if (g_menu == 1) snprintf(g_menu_msg, sizeof g_menu_msg, "The host refused: game started or full");
            free(f);
            closesocket(s);
            return (DPHR)code;
        }
        ja = (CArg *)malloc(sizeof *ja);
        if (!ja) { free(f); closesocket(s); return DPERR_GENERIC; }
        if (!(jo = out_new(s, "host link"))) { free(ja); free(f); closesocket(s); return DPERR_GENERIC; }
        InterlockedIncrement(&jo->refs);   /* refs 3: reader, writer, G.jout (let go in do_close) */
        EnterCriticalSection(&cs);
        G.maxpl = *(DWORD *)(f + 4);
        G.flags = *(DWORD *)f;
        memcpy(&G.inst, f + 12, 16);
        memcpy(G.sname, f + 28, 64);
        G.sname[63] = 0;
        G.jsock = s; G.jout = jo; G.lsock = INVALID_SOCKET; G.host = 0; G.open = 1; G.connected = 1; G.myid = 0;
        G.gen++;
        ja->s = s; ja->gen = jagen = G.gen; ja->o = jo;
        if (!G.evPid) G.evPid = CreateEventA(NULL, FALSE, FALSE, NULL);
        LeaveCriticalSection(&cs);
        free(f);
        InterlockedIncrement(&g_live);
        th = CreateThread(NULL, 0, join_thread, ja, 0, NULL);
        if (!th) {
            logf_("Open(join): CreateThread failed (%lu)", GetLastError());
            InterlockedDecrement(&g_live);
            free(ja);
            out_release(jo);   /* the reader's ref: it never ran */
            do_close(0);
            return DPERR_NOCONNECTION;
        }
        CloseHandle(th);
        logf_("Open(join) ok, session '%s'", G.sname);
        lf_begin();
        dg_start(jagen);
        return DP_OK;
    }
}

static DPHR WINAPI dp2_CreatePlayer(Obj *t, DPID *out, DPNAME_ *nm, HANDLE ev, void *data, DWORD size, DWORD flags)
{
    char name[32] = {0};
    if (nm && nm->lpszShortName) lstrcpynA(name, nm->lpszShortName, 32);
    logf_("CreatePlayer name='%s' flags=%lu", name, flags);
    if (!out || !G.open) return G.open ? DPERR_INVALIDPARAMS : DPERR_INVALIDOBJECT;
    if (G.host) {
        Player *p;
        BYTE m[36];
        EnterCriticalSection(&cs);
        p = (DWORD)pl_count() < G.maxpl ? pl_add(G.nextid, name, -1) : NULL;
        if (!p) { LeaveCriticalSection(&cs); return DPERR_CANTCREATEPLAYER; }
        *out = G.myid = G.nextid++;
        *(DWORD *)m = *out;
        memcpy(m + 4, name, 32);
        bcast(-1, T_SYS_CREATE, m, 36);
        LeaveCriticalSection(&cs);
        logf_("CreatePlayer(host) id=%08lx", *out);
        return DP_OK;
    } else {
        BYTE m[32];
        DWORD w;
        memset(m, 0, 32);
        memcpy(m, name, 32);
        EnterCriticalSection(&cs);
        if (!G.connected) { LeaveCriticalSection(&cs); return DPERR_NOCONNECTION; }
        ResetEvent(G.evPid);
        G.pidResult = 0;
        G.pidWaiting = 1;
        if (jsend(T_CREATEPL, m, 32)) { G.pidWaiting = 0; LeaveCriticalSection(&cs); return DPERR_NOCONNECTION; }
        LeaveCriticalSection(&cs);
        w = WaitForSingleObject(G.evPid, 5000);
        EnterCriticalSection(&cs);
        G.pidWaiting = 0;   /* F20: a PLAYERID after this point is a ghost; join_body asks the host to destroy it */
        if (w != WAIT_OBJECT_0 || !G.pidResult) { LeaveCriticalSection(&cs); logf_("CreatePlayer(join) failed w=%lu", w); return DPERR_CANTCREATEPLAYER; }
        *out = G.myid = G.pidResult;
        pl_add(*out, name, -1);
        LeaveCriticalSection(&cs);
        logf_("CreatePlayer(join) id=%08lx", *out);
        return DP_OK;
    }
}

static DPHR WINAPI dp2_DestroyPlayer(Obj *t, DPID id)
{
    Player *p;
    logf_("DestroyPlayer %08lx", id);
    EnterCriticalSection(&cs);
    if (!G.open || !(p = pl_find(id))) { LeaveCriticalSection(&cs); return DPERR_INVALIDPLAYER; }
    if (G.host) {
        p->used = 0;
        bcast(-1, T_SYS_DESTROY, &id, 4);
    } else {
        p->used = 0;
        jsend(T_DESTROYPL, &id, 4);
    }
    LeaveCriticalSection(&cs);
    return DP_OK;
}

/* ---- Connect asks for the host's address (Matthew 2026-10-06: no Join.cmd) ----
 * The game's session list is a Win32 dialog (FUN_004b85b7): WM_INITDIALOG calls EnumSessions once with ctx = its
 * list box, then WM_TIMER every 3 s calls it again. The first EnumSessions after each DirectPlayCreate (one per
 * Connect press) shows this box on the game thread, before the list is visible. OK saves host=addr[:port] into
 * lotr2net.ini (other lines kept) and uses it at once; Cancel presses the session dialog's own Cancel (id 2, which
 * releases the DirectPlay object and goes back a screen). Skipped when LOTR2NET_HOST is set or the ini has ask=0
 * (rig and agent automation). */
#define ID_AHOST 101
#define ID_APORT 102
static volatile LONG g_ask_due;   /* set by DirectPlayCreate, taken by the first EnumSessions */
static volatile LONG g_in_ask;    /* re-entrancy guard: the box runs a modal loop on the game thread */

static int ask_enabled(void)
{
    char v[16], *e = getenv("LOTR2NET_HOST");
    if (e && *e) return 0;
    if (ini_get("ask", v, sizeof v) && (!strcmp(v, "0") || !_stricmp(v, "no") || !_stricmp(v, "off") || !_stricmp(v, "false"))) return 0;
    return 1;
}

/* Replace (or add) host= in lotr2net.ini, keeping every other line; drops the installer's "; host= unset" note. */
static int ini_set_host(const char *val)
{
    char p[MAX_PATH + 16], q[MAX_PATH + 16], line[512];
    FILE *f, *o;
    int done = 0;
    lstrcpynA(p, exedir, MAX_PATH);
    lstrcatA(p, "lotr2net.ini");
    lstrcpynA(q, exedir, MAX_PATH);
    lstrcatA(q, "lotr2net.ini.tmp");
    o = fopen(q, "w");
    if (!o) return 0;
    f = fopen(p, "r");
    if (f) {
        int first = 1;
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            if (first && (BYTE)s[0] == 0xEF && (BYTE)s[1] == 0xBB && (BYTE)s[2] == 0xBF) memmove(line, line + 3, strlen(line + 3) + 1);   /* F9: written back as plain ASCII */
            first = 0;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == ';') {
                char *c = s + 1;
                while (*c == ' ' || *c == '\t') c++;
                if (!_strnicmp(c, "host=", 5) && strstr(c, "unset")) continue;
            } else if (!_strnicmp(s, "host", 4)) {
                char *c = s + 4;
                while (*c == ' ' || *c == '\t') c++;
                if (*c == '=') { if (!done) fprintf(o, "host=%s\n", val); done = 1; continue; }
            }
            fputs(line, o);
            if (line[0] && line[strlen(line) - 1] != '\n') fputc('\n', o);
        }
        fclose(f);
    }
    if (!done) fprintf(o, "host=%s\n", val);
    if (fclose(o) || !MoveFileExA(q, p, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(q); return 0; }
    return 1;
}

static INT_PTR CALLBACK ask_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INITDIALOG) {
        char host[128], ps[16];
        int port;
        get_target(host, &port);
        if (!strcmp(host, "127.0.0.1")) host[0] = 0;   /* nothing set yet: an empty box, not our fallback */
        sprintf(ps, "%d", port);
        SetDlgItemTextA(d, ID_AHOST, host);
        SetDlgItemTextA(d, ID_APORT, ps);
        SendDlgItemMessageA(d, ID_AHOST, EM_LIMITTEXT, 120, 0);
        SendDlgItemMessageA(d, ID_APORT, EM_LIMITTEXT, 5, 0);
        SendDlgItemMessageA(d, ID_AHOST, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(d, ID_AHOST));
        return FALSE;   /* focus set by us */
    }
    if (m == WM_COMMAND && LOWORD(wp) == IDOK) {
        char host[128], ps[16], *a, *c, val[140];
        int n, port;
        GetDlgItemTextA(d, ID_AHOST, host, sizeof host);
        GetDlgItemTextA(d, ID_APORT, ps, sizeof ps);
        for (a = host; *a == ' ' || *a == '\t'; a++) {}
        n = (int)strlen(a);
        while (n > 0 && (a[n - 1] == ' ' || a[n - 1] == '\t')) a[--n] = 0;
        port = atoi(ps);
        if ((c = strchr(a, ':')) != NULL) { port = atoi(c + 1); *c = 0; }   /* "addr:port" typed in the address box */
        if (!a[0] || strchr(a, ' ')) { MessageBeep(MB_ICONWARNING); SetFocus(GetDlgItem(d, ID_AHOST)); return TRUE; }
        if (port < 1024 || port > 65535) { MessageBeep(MB_ICONWARNING); SetFocus(GetDlgItem(d, ID_APORT)); return TRUE; }
        if (port == PORT_DEFAULT) snprintf(val, sizeof val, "%s", a);
        else snprintf(val, sizeof val, "%s:%d", a, port);
        lstrcpynA(g_ask_host, val, sizeof g_ask_host);
        logf_("join address box: %s (%s)", val, ini_set_host(val) ? "saved to lotr2net.ini" : "lotr2net.ini NOT writable, this run only");
        EndDialog(d, IDOK);
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    return FALSE;
}

/* in-memory DLGTEMPLATE: header, then items, each DWORD aligned; strings are UTF-16 */
static WORD *dt_str(WORD *p, const char *s) { while (*s) *p++ = (WORD)(BYTE)*s++; *p++ = 0; return p; }
static WORD *dt_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const char *text)
{
    DLGITEMTEMPLATE *it;
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;   /* 0x80 button, 0x81 edit, 0x82 static */
    p = dt_str(p, text);
    *p++ = 0;                    /* no creation data */
    return p;
}

/* returns 1 = go on enumerating, 0 = the player cancelled (the session dialog has been told to close) */
static int ask_join(void *ctx)
{
    static DWORD tbuf[512];
    DLGTEMPLATE *t = (DLGTEMPLATE *)tbuf;
    WORD *p;
    HWND sess = (ctx && IsWindow((HWND)ctx)) ? GetParent((HWND)ctx) : NULL;
    HWND owner = sess ? GetWindow(sess, GW_OWNER) : NULL;
    INT_PTR r;
    char line[96];
    if (!owner) owner = sess;
    memset(tbuf, 0, sizeof tbuf);
    t->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 7;
    t->cx = 230; t->cy = 86;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;          /* no menu, default class */
    p = dt_str(p, "Join a game");
    *p++ = 8;
    p = dt_str(p, "MS Shell Dlg");
    p = dt_item(p, SS_LEFT, 7, 7, 160, 9, 0xFFFF, 0x82, "Host address (IP or computer name):");
    p = dt_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 18, 160, 13, ID_AHOST, 0x81, "");
    p = dt_item(p, SS_LEFT, 174, 7, 49, 9, 0xFFFF, 0x82, "Port:");
    p = dt_item(p, ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 174, 18, 49, 13, ID_APORT, 0x81, "");
    snprintf(line, sizeof line, "The host tells you their address. The port is %d unless the host changed it.", PORT_DEFAULT);
    p = dt_item(p, SS_LEFT, 7, 37, 216, 18, 0xFFFF, 0x82, line);
    p = dt_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 119, 64, 50, 15, IDOK, 0x80, "Connect");
    p = dt_item(p, BS_PUSHBUTTON | WS_TABSTOP, 173, 64, 50, 15, IDCANCEL, 0x80, "Cancel");
    logf_("join address box: shown (session dialog %p)", (void *)sess);
    r = DialogBoxIndirectParamA(GetModuleHandleA(NULL), t, owner, ask_proc, 0);
    if (r == IDOK) return 1;
    logf_("join address box: %s", r == IDCANCEL ? "cancelled" : "failed");
    if (r != IDCANCEL) return 1;   /* the box could not open: fall back to the saved address */
    if (sess) PostMessageA(sess, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
    return 0;
}

static int enum_once(DPSD2 *want, ENUMSESS_CB cb, void *ctx, char *name, int nn, DWORD *seq);
/* Menu join: the session dialog calls this from WM_INITDIALOG and then every 3 s (WM_TIMER). A session found -> select
 * it and send the dialog the double-click it takes for Join (its own Open path); a host of another build -> cancel and
 * say so; 2 finished rounds with no session, or 12 s in all -> cancel with "no game found". Cancel = the dialog's own
 * id 2. Review #15: the round trip runs on a worker (enum_once waits at most ENUM_WAIT_MS), so the invisible dialog
 * never freezes the title screen; a round still running is simply asked again on the next tick. */
static DPHR menu_enum(DPSD2 *want, ENUMSESS_CB cb, void *ctx, HWND lb, HWND dlg)
{
    char nm[64], host[128];
    int port;
    DWORD seq = 0;
    int r = enum_once(want, cb, ctx, nm, sizeof nm, &seq);
    get_target(host, &port);
    if (r == 1 && strstr(nm, "[host v")) {
        char *v = strstr(nm, "[host v");
        snprintf(g_menu_msg, sizeof g_menu_msg, "Different patch version %s", v);
        r = 0;
        g_menu_tries = 99;
    }
    if (r <= 0 && r != -1 && seq != g_menu_seq) { g_menu_seq = seq; g_menu_tries++; }   /* one more finished miss */
    if (r == -2 && !g_menu_msg[0]) snprintf(g_menu_msg, sizeof g_menu_msg, "The host refused: game started or full");
    if (r == 1 && lb && dlg) {
        PostMessageA(lb, LB_SETCURSEL, 0, 0);
        PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(lb), LBN_DBLCLK), (LPARAM)lb);
        logf_("menu join: '%s' at %s:%d, joining", nm, host, port);
    } else if (r != 1 && (g_menu_tries >= 2 || GetTickCount() - g_menu_t0 >= 12000 || !dlg)) {
        if (!g_menu_msg[0]) snprintf(g_menu_msg, sizeof g_menu_msg, "No game found at %s:%d", host, port);
        logf_("menu join: %s - back to the menu (%d finished rounds, %lu ms)", g_menu_msg, g_menu_tries, GetTickCount() - g_menu_t0);
        if (dlg) PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
    } else if (r == -1) logf_("menu join: still asking %s:%d (%lu ms)", host, port, GetTickCount() - g_menu_t0);
    return DP_OK;
}

__declspec(dllexport) void WINAPI LF_NetMenu(int mode, const char *host, int port)
{
    char val[160];
    g_menu = mode;
    g_menu_tries = 0;
    g_menu_seq = 0;
    g_menu_t0 = GetTickCount();
    InterlockedIncrement(&g_enum_gen);
    g_menu_msg[0] = 0;
    if (mode == 1 && host) {
        if (port < 1024 || port > 65535) { logf_("menu join: port %d out of range 1024-65535 - using %d", port, PORT_DEFAULT); port = PORT_DEFAULT; }   /* F9 */
        if (port == PORT_DEFAULT) snprintf(val, sizeof val, "%s", host);
        else snprintf(val, sizeof val, "%s:%d", host, port);
        lstrcpynA(g_menu_host, val, sizeof g_menu_host);
        lstrcpynA(g_ask_host, val, sizeof g_ask_host);
        logf_("menu join %s (%s)", val, ini_set_host(val) ? "saved to lotr2net.ini" : "lotr2net.ini NOT writable, this run only");
    } else logf_("menu mode %d", mode);
}
/* The address the menu shows first: what Connect would use now (env, last typed, ini); empty if none. */
__declspec(dllexport) void WINAPI LF_NetMenuGet(char *host, int n, int *port)
{
    char h[128], *e = getenv("LOTR2NET_HOST");
    int m = g_menu;
    g_menu = 0;
    h[0] = 0;
    if ((e && *e) || g_ask_host[0] || ini_get("host", h, 120)) get_target(h, port);
    else *port = PORT_DEFAULT;
    g_menu = m;
    lstrcpynA(host, h, n);
}
__declspec(dllexport) int WINAPI LF_NetMenuMsg(char *buf, int n)
{
    lstrcpynA(buf, g_menu_msg, n);
    g_menu_msg[0] = 0;
    return buf[0] != 0;
}

/* ---------------------------------------------------------------- local search (2026-10-06, Matthew: "Search local")
 * Host: while a session is open, a UDP socket on the game port answers "L2FIND1" with "L2HERE1" + port, PROTO, players,
 * max, locked, computer name. Searcher: sends "L2FIND1" to the directed broadcast of every up IPv4 adapter that can
 * broadcast (LAN, Wi-Fi, Hamachi, TAP VPNs), plus 255.255.255.255 and 127.0.0.1, three times over 1.5 s, and lists every
 * answer. A routed VPN with no broadcast (WireGuard, Tailscale) never answers: there, type the address. The game wire is
 * unchanged; this is a side channel. */
#define FIND_Q "L2FIND1"   /* v1-v6 query, 7 bytes: sent so an older host still shows (as another version); never answered */
#define FIND_Q2 "L2FIND2"  /* v7 query, padded to FIND_QLEN: the answer is no bigger than the question */
#define FIND_QLEN 72
#define FIND_A "L2HERE1"
#define FIND_MAX 8
typedef struct { char host[64], name[40]; int port, players, maxpl, locked; DWORD proto; } Found;
static CRITICAL_SECTION findcs;
static Found g_found[FIND_MAX];
static int g_nfound;
static volatile LONG g_finding;
static SOCKET g_usock = INVALID_SOCKET;

/* Review #9: is ip (network order) loopback, or inside the subnet of one of this machine's up IPv4 adapters? The
 * adapter list is read with SIO_GET_INTERFACE_LIST on u and kept for 10 s (one thread per cache: beacon, searcher). */
typedef struct { u_long net[16], mask[16]; int n; DWORD t; } LinkCache;
static int on_link(SOCKET u, LinkCache *c, u_long ip)
{
    int i;
    if ((ntohl(ip) >> 24) == 127) return 1;
    if (!c->t || GetTickCount() - c->t > 10000) {
        BYTE buf[2048];
        DWORD got = 0;
        c->n = 0;
        c->t = GetTickCount() | 1;
        if (!WSAIoctl(u, SIO_GET_INTERFACE_LIST, NULL, 0, buf, sizeof buf, &got, NULL, NULL)) {
            INTERFACE_INFO *ii = (INTERFACE_INFO *)buf;
            int k = (int)(got / sizeof *ii);
            for (i = 0; i < k && c->n < 16; i++) {
                u_long a = ii[i].iiAddress.AddressIn.sin_addr.s_addr, m = ii[i].iiNetmask.AddressIn.sin_addr.s_addr;
                if (!(ii[i].iiFlags & IFF_UP) || (ii[i].iiFlags & IFF_LOOPBACK)) continue;
                c->net[c->n] = a & m; c->mask[c->n] = m; c->n++;
            }
        }
    }
    for (i = 0; i < c->n; i++) if ((ip & c->mask[i]) == c->net[i]) return 1;
    return 0;
}

/* GameRanger (2026-10-28): its injected GameRanger.dll relays the room's traffic, so a joiner needs no typed address:
 * Connect runs the local search and joins the host that answers. Relayed packets may come from addresses outside the
 * local subnets, so the on-link rule (review #9) is not applied then (the answer is still no bigger than the query and
 * still rate-limited). LOTR2NET_GRJOIN=0 turns it off. */
static int gr_mode(void)
{
    char *e = getenv("LOTR2NET_GRJOIN");
    return GetModuleHandleA("GameRanger.dll") && !(e && !strcmp(e, "0"));
}
/* GameRanger's internal host-ready function, found by its unique "push 0x9100" with its prologue checked; NULL for a
 * GameRanger build we do not know. *n = matches seen. */
static const BYTE gr_pro[] = { 0x55, 0x8B, 0xEC, 0x56, 0x0F, 0xBE, 0x05 };   /* push ebp; mov ebp,esp; push esi; movsx eax,byte [..] */
static BYTE *gr_fn(BYTE **pbase, DWORD *pn)
{
    BYTE *base = (BYTE *)GetModuleHandleA("GameRanger.dll"), *t = NULL, *hit = NULL;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    DWORD i, tsz = 0, n = 0;
    *pbase = base; *pn = 0;
    if (!base) return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 6)) { t = base + sec[i].VirtualAddress; tsz = sec[i].Misc.VirtualSize; }
    if (!t || tsz < 0x40) return NULL;
    for (i = 0x32; i + 5 <= tsz; i++)
        if (t[i] == 0x68 && t[i + 1] == 0x00 && t[i + 2] == 0x91 && t[i + 3] == 0x00 && t[i + 4] == 0x00) { n++; hit = t + i - 0x32; }
    *pn = n;
    if (n != 1 || memcmp(hit, gr_pro, sizeof gr_pro) || hit[0x0B] != 0x85 || hit[0x0C] != 0xC0) return NULL;
    return hit;
}
/* The view GameRanger.exe shares with its dll (pointer at dll+0xCDB34): +0 hosting flag, +0x20 the room's IPv4 list,
 * count byte at +0x120. Only read for the GameRanger build this was taken from (host-ready at +0x1E04); else NULL. */
static BYTE *gr_view(void)
{
    BYTE *base, *p;
    DWORD n;
    MEMORY_BASIC_INFORMATION mi;
    if (gr_fn(&base, &n) != base + 0x1E04) return NULL;
    p = *(BYTE **)(base + 0xCDB34);
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE)) ||
        (BYTE *)mi.BaseAddress + mi.RegionSize < p + 0x124) return NULL;
    return p;
}
static int gr_peers(u_long *out, int max)
{
    BYTE *p = gr_view();
    int i, k = 0;
    if (!p) return 0;
    for (i = 0; i < p[0x120] && i < 64 && k < max; i++) {
        u_long ip = *(u_long *)(p + 0x20 + 4 * i);
        if (ip && ip != INADDR_BROADCAST) out[k++] = ip;
    }
    return k;
}

/* Review #9: only a padded L2FIND2 (>= 72 bytes, so the 72-byte answer amplifies nothing) from loopback or an on-link
 * source is answered, at most 32 answers a second in all; a port-forwarded host is no longer a UDP reflector. */
static DWORD WINAPI beacon_thread(LPVOID a)
{
    SOCKET u = (SOCKET)(UINT_PTR)a;
    char me[40];
    DWORD n = sizeof me, rt = 0, rn = 0, dropped = 0;
    LinkCache lc;
    memset(&lc, 0, sizeof lc);
    if (!GetComputerNameA(me, &n)) lstrcpyA(me, "host");
    for (;;) {
        char b[256];
        BYTE r[128];
        struct sockaddr_in from;
        int fl = sizeof from, k = recvfrom(u, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        DWORD now;
        if (k == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue; break; }
        if (k < FIND_QLEN || memcmp(b, FIND_Q2, 7)) continue;   /* L2FIND1 or junk: never answered */
        if (!on_link(u, &lc, from.sin_addr.s_addr) && !gr_mode()) {
            if (!dropped++) logf_("local search: query from %s is not on a local subnet - not answered (first of these)", inet_ntoa(from.sin_addr));
            continue;
        }
        now = GetTickCount();
        if (now - rt >= 1000) { rt = now; rn = 0; }
        if (++rn > 32) { if (rn == 33) logf_("local search: over 32 queries in 1 s - extra not answered"); continue; }
        memset(r, 0, sizeof r);
        memcpy(r, FIND_A, 7);
        EnterCriticalSection(&cs);
        if (!G.open || !G.host) { LeaveCriticalSection(&cs); continue; }
        *(WORD *)(r + 8) = (WORD)listen_port();
        *(DWORD *)(r + 12) = PROTO;
        *(DWORD *)(r + 16) = (DWORD)pl_count();
        *(DWORD *)(r + 20) = G.maxpl;
        r[24] = (BYTE)(G.locked != 0);
        LeaveCriticalSection(&cs);
        lstrcpynA((char *)r + 32, me, 40);
        sendto(u, (char *)r, 72, 0, (struct sockaddr *)&from, fl);
        logf_("local search answered for %s", inet_ntoa(from.sin_addr));
    }
    /* never closed here: beacon_stop owns the socket (closing it here too freed a handle that a new socket could
     * get before beacon_stop closed it again; review 2026-10-07) */
    InterlockedDecrement(&g_live);
    return 0;
}

static void beacon_start(int port)
{
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    int one = 1;
    HANDLE th;
    if (u == INVALID_SOCKET) return;
    setsockopt(u, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = listen_addr();
    a.sin_port = htons((u_short)port);
    if (bind(u, (struct sockaddr *)&a, sizeof a)) {
        logf_("local search: UDP %d not bound (WSA %d) - this game will not show in Search local", port, WSAGetLastError());
        closesocket(u);
        return;
    }
    InterlockedIncrement(&g_live);
    th = CreateThread(NULL, 0, beacon_thread, (LPVOID)(UINT_PTR)u, 0, NULL);
    if (!th) { InterlockedDecrement(&g_live); closesocket(u); return; }
    CloseHandle(th);
    EnterCriticalSection(&findcs);
    g_usock = u;
    LeaveCriticalSection(&findcs);
    logf_("local search: answering on UDP %d", port);
}

/* GameRanger: its injected GameRanger.dll launches the joiners only after the host's game reports "hosting" (one
 * PostMessage 0x9100 to GameRanger.exe from its internal function at +0x1E04). For Lords II nothing ever calls it: the
 * per-game config leaves both host ports 0, its DirectPlay Open hook exists only for IDirectPlay4 of another game, and
 * its GameSpy master emulator is not enabled for this game. So when hosting inside GameRanger, call that function
 * ourselves. It is found by its unique "push 0x9100" and its prologue is checked, so another GameRanger build is left
 * alone (logged). The function itself posts only once and only when GameRanger says this PC hosts.
 * LOTR2NET_GRHOST=0 turns it off. */
static void grhb_start(int port)
{
    char *e = getenv("LOTR2NET_GRHOST");
    BYTE *base, *hit;
    DWORD n;
    (void)port;
    if (!GetModuleHandleA("GameRanger.dll") || (e && !strcmp(e, "0"))) return;
    if (!(hit = gr_fn(&base, &n))) {
        logf_("GameRanger host-ready: function not recognised (%lu matches) - unknown GameRanger build, skipped", n);
        return;
    }
    logf_("GameRanger host-ready: calling GameRanger.dll+0x%lx", (unsigned long)(hit - base));
    ((void (__cdecl *)(void))hit)();
    logf_("GameRanger host-ready: done");
}

static void beacon_stop(void)   /* closing the socket wakes recvfrom; the thread then exits */
{
    SOCKET u;
    EnterCriticalSection(&findcs);
    u = g_usock; g_usock = INVALID_SOCKET;
    LeaveCriticalSection(&findcs);
    if (u != INVALID_SOCKET) closesocket(u);
}

static DWORD WINAPI find_thread(LPVOID a)
{
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
    u_long dst[24 + 16];
    int nd = 0, i, round, port = listen_port(), one = 1, gr = gr_mode(), gr0 = 0, done = 0;
    BYTE buf[2048];
    DWORD got = 0;
    char q[FIND_QLEN];
    LinkCache lc;
    memset(&lc, 0, sizeof lc);
    memset(q, 0, sizeof q);
    memcpy(q, FIND_Q2, 7);
    if (u == INVALID_SOCKET) { InterlockedExchange(&g_finding, 0); return 0; }
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof one);
    set_rcvto(u, 100);
    dst[nd++] = htonl(INADDR_BROADCAST);
    dst[nd++] = inet_addr("127.0.0.1");
    if (!WSAIoctl(u, SIO_GET_INTERFACE_LIST, NULL, 0, buf, sizeof buf, &got, NULL, NULL)) {
        INTERFACE_INFO *ii = (INTERFACE_INFO *)buf;
        int n = (int)(got / sizeof *ii);
        for (i = 0; i < n && nd < 24; i++) {
            u_long ip = ii[i].iiAddress.AddressIn.sin_addr.s_addr, m = ii[i].iiNetmask.AddressIn.sin_addr.s_addr;
            struct in_addr x;
            char sa[20];
            if (!(ii[i].iiFlags & IFF_UP) || (ii[i].iiFlags & IFF_LOOPBACK) || !(ii[i].iiFlags & IFF_BROADCAST)) continue;
            if (m == 0xffffffff) continue;
            dst[nd++] = ip | ~m;
            x.s_addr = ip; lstrcpynA(sa, inet_ntoa(x), sizeof sa);
            x.s_addr = dst[nd - 1];
            logf_("local search: adapter %s -> broadcast %s", sa, inet_ntoa(x));
        }
    }
    if (gr) {   /* GameRanger: also ask every address in its room list directly */
        int k = gr_peers(dst + nd, 16);
        gr0 = nd;
        for (i = 0; i < k; i++) { struct in_addr x; x.s_addr = dst[nd + i]; logf_("local search: GameRanger room address %s", inet_ntoa(x)); }
        nd += k;
        logf_("local search: inside GameRanger (%d room addresses)", k);
    }
    for (round = 0; round < 3 && !done; round++) {
        DWORD t = GetTickCount();
        for (i = 0; i < nd; i++) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET; to.sin_port = htons((u_short)port); to.sin_addr.s_addr = dst[i];
            sendto(u, q, FIND_QLEN, 0, (struct sockaddr *)&to, sizeof to);
            sendto(u, FIND_Q, 7, 0, (struct sockaddr *)&to, sizeof to);   /* an older (v1-v6) host lists as another version */
            if (port != PORT_DEFAULT) {             /* a host on the default port answers too (review 2026-10-07) */
                to.sin_port = htons((u_short)PORT_DEFAULT);
                sendto(u, q, FIND_QLEN, 0, (struct sockaddr *)&to, sizeof to);
                sendto(u, FIND_Q, 7, 0, (struct sockaddr *)&to, sizeof to);
            }
        }
        while (!done && GetTickCount() - t < 500) {
            BYTE r[128];
            struct sockaddr_in from;
            int fl = sizeof from, k = recvfrom(u, (char *)r, sizeof r, 0, (struct sockaddr *)&from, &fl), j, dup = 0;
            Found f;
            if (k < 72 || memcmp(r, FIND_A, 7)) continue;
            if (!gr && !on_link(u, &lc, from.sin_addr.s_addr)) continue;   /* review #9: no entries from off the local subnets */
            memset(&f, 0, sizeof f);
            lstrcpynA(f.host, inet_ntoa(from.sin_addr), sizeof f.host);
            f.port = *(WORD *)(r + 8);
            f.proto = *(DWORD *)(r + 12);
            f.players = (int)*(DWORD *)(r + 16);
            f.maxpl = (int)*(DWORD *)(r + 20);
            f.locked = r[24];
            r[71] = 0;
            lstrcpynA(f.name, (char *)r + 32, sizeof f.name);
            {   /* review tb2 F5: an answer with impossible fields is a fake or garbage: not listed; names printable only */
                char *c;
                if (f.port < 1024 || f.port > 65535 || f.maxpl < 1 || f.maxpl > MAXPL || f.players < 0 || f.players > f.maxpl || f.locked > 1) {
                    logf_("local search: answer from %s with bad fields (port %d, %d/%d, locked %d) - ignored", f.host, f.port, f.players, f.maxpl, f.locked);
                    continue;
                }
                for (c = f.name; *c; c++) if ((BYTE)*c < 0x20 || (BYTE)*c > 0x7e) *c = '?';
            }
            EnterCriticalSection(&findcs);
            for (j = 0; j < g_nfound; j++)   /* one host can answer on several adapters: keep the first address */
                if ((!strcmp(g_found[j].name, f.name) && g_found[j].port == f.port) ||
                    !strcmp(g_found[j].host, f.host)) dup = 1;   /* F5: one entry per source address, so one machine cannot fill the list */
            if (!dup && g_nfound < FIND_MAX) g_found[g_nfound++] = f;
            LeaveCriticalSection(&findcs);
            if (!dup) logf_("local search: '%s' at %s:%d (%d/%d%s)", f.name, f.host, f.port, f.players, f.maxpl, f.locked ? ", started" : "");
            if (gr && f.proto == PROTO && !f.locked)   /* GameRanger: a joinable host at a room address ends the search */
                for (j = gr0; j < nd; j++) if (from.sin_addr.s_addr == dst[j]) done = 1;
        }
    }
    closesocket(u);
    logf_("local search: %d found", g_nfound);
    InterlockedExchange(&g_finding, 0);
    return 0;
}

/* start != 0: begin a search (results cleared). Returns -1 while searching, else how many were found. */
__declspec(dllexport) int WINAPI LF_NetFind(int start)
{
    if (start && !InterlockedCompareExchange(&g_finding, 1, 0)) {
        HANDLE th;
        ensure_init();
        EnterCriticalSection(&findcs);
        g_nfound = 0;
        LeaveCriticalSection(&findcs);
        th = CreateThread(NULL, 0, find_thread, NULL, 0, NULL);
        if (th) CloseHandle(th); else InterlockedExchange(&g_finding, 0);
    }
    return g_finding ? -1 : g_nfound;
}

/* Result i: a label for the menu, the address and port to connect to. state: 0 joinable, 1 started, 2 other version. */
__declspec(dllexport) int WINAPI LF_NetFindGet(int i, char *label, int ln, char *host, int hn, int *port, int *state)
{
    int ok = 0;
    EnterCriticalSection(&findcs);
    if (i >= 0 && i < g_nfound) {
        Found *f = &g_found[i];
        snprintf(label, ln, "%d/%d  %s", f->players, f->maxpl, f->name);
        lstrcpynA(host, f->host, hn);
        *port = f->port;
        *state = f->proto != PROTO ? 2 : f->locked ? 1 : 0;
        ok = 1;
    }
    LeaveCriticalSection(&findcs);
    return ok;
}

/* Connect inside GameRanger: search (up to ~2 s, on the game thread like the address box it replaces) and take the
 * joinable host of this build, preferring one at a GameRanger room address. 0 = none found: the box is shown. */
static int gr_join(void)
{
    u_long peers[16];
    int np, n, i, j, best = -1;
    DWORD t0 = GetTickCount();
    if (!gr_mode()) return 0;
    np = gr_peers(peers, 16);
    LF_NetFind(1);
    while ((n = LF_NetFind(0)) < 0 && GetTickCount() - t0 < 3000) Sleep(50);
    if (n < 0) n = 0;
    EnterCriticalSection(&findcs);
    for (i = 0; i < g_nfound; i++) {
        Found *f = &g_found[i];
        if (f->proto != PROTO || f->locked) continue;
        for (j = 0; j < np; j++) if (inet_addr(f->host) == peers[j]) break;
        if (best < 0 || j < np) best = i;
        if (j < np) break;
    }
    if (best >= 0) {
        Found *f = &g_found[best];
        if (f->port == PORT_DEFAULT) snprintf(g_ask_host, sizeof g_ask_host, "%s", f->host);
        else snprintf(g_ask_host, sizeof g_ask_host, "%s:%d", f->host, f->port);
        logf_("GameRanger join: '%s' at %s (%d found, %lu ms) - no address box", f->name, g_ask_host, g_nfound, GetTickCount() - t0);
    } else logf_("GameRanger join: no joinable host found (%d answers, %lu ms) - asking for the address", g_nfound, GetTickCount() - t0);
    LeaveCriticalSection(&findcs);
    return best >= 0;
}

/* ---- GameRanger auto-setup (2026-10-28): inside GameRanger the menus always take the same answers, so the title's
 * Multiplayer path runs by itself, the way ddraw's mpmenu drives it: the title screen's own input code (gra_patch,
 * where the game starts Multiplayer by itself when launched from a DirectPlay lobby) calls the game's Multiplayer
 * handler FUN_00432b05 on the first title frame, with its dialogs answered in DialogBoxParamA: Connect or Create (0x6c) by GameRanger's hosting flag, the session name
 * (0x74) default. The connection method list picks GameRanger's line (dp EnumerateA); a joiner first finds the host
 * (gr_join) and the session list then runs invisible in menu-join mode. One run per game start; any step that does
 * not match (unknown exe or GameRanger build, title never reached, no host found) leaves the menus to the player.
 * LOTR2NET_GRAUTO=0 turns it off. */
#define A_UISTATE 0x4eac50
#define A_SUBSCR 0x5530f0
#define A_REDRAW 0x57d340
#define A_DBPSLOT 0x5cf52c   /* Lords2.exe's import slot for USER32!DialogBoxParamA */
static int g_gra;            /* 1 joining, 2 hosting: while the Multiplayer handler runs */
typedef INT_PTR (WINAPI *DBP)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static DBP g_dbp;

static INT_PTR WINAPI gra_dbp(HINSTANCE h, LPCSTR t, HWND o, DLGPROC f, LPARAM l)
{
    if (g_gra && IS_INTRESOURCE(t)) {
        WORD id = LOWORD((ULONG_PTR)t);
        if (id == 0x6c) { logf_("GameRanger auto: Connect or Create -> %s", g_gra == 1 ? "Connect" : "Create"); return g_gra == 1 ? 0x3ea : 0x3ec; }
        if (id == 0x74 && g_gra == 2) { logf_("GameRanger auto: session name -> default"); return 0; }
    }
    return g_dbp(h, t, o, f, l);
}

static int gra_hook_dbp(void)   /* chains to whatever the slot holds (another mod may have hooked it first) */
{
    DWORD old, *slot = (DWORD *)A_DBPSLOT;
    if (g_dbp) return 1;
    if (!*slot || *slot == (DWORD)gra_dbp || !VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return 0;
    g_dbp = (DBP)*slot;
    *slot = (DWORD)gra_dbp;
    VirtualProtect(slot, 4, old, &old);
    return 1;
}

static BOOL CALLBACK tl_cb(HWND w, LPARAM p)
{
    char c[16];
    HWND lb;
    if (GetClassNameA(w, c, sizeof c) && !strcmp(c, "#32770") && (lb = FindWindowExA(w, NULL, "ListBox", NULL)) != NULL) {
        *(HWND *)p = lb;
        return FALSE;
    }
    return TRUE;
}
/* The list box of a dialog of this thread (the top one): the game's connection method and session list dialogs pass
 * theirs as ctx, but not when GameRanger's hooks sit between. */
static HWND thread_listbox(void)
{
    HWND lb = NULL;
    EnumThreadWindows(GetCurrentThreadId(), tl_cb, (LPARAM)&lb);
    return lb;
}
static void hide_dlg(HWND dlg)
{
    SetWindowLongA(dlg, GWL_EXSTYLE, GetWindowLongA(dlg, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(dlg, 0, 0, LWA_ALPHA);
}

/* Connection method inside GameRanger: GameRanger adds its own line after ours returns, so the pick is posted; the
 * list handles it after the dialog's WM_INITDIALOG, when GameRanger's line is in: the first line, then GameRanger's
 * if there (LB_SELECTSTRING keeps the first when it finds none). Both end in our TCP/IP. */
static void gra_pick_method(HWND lb)
{
    static const char gr[] = "GameRanger";
    hide_dlg(GetParent(lb));
    PostMessageA(lb, LB_SETCURSEL, 0, 0);
    PostMessageA(lb, LB_SELECTSTRING, (WPARAM)-1, (LPARAM)gr);
    PostMessageA(GetParent(lb), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(lb), LBN_DBLCLK), (LPARAM)lb);
    logf_("connection method: GameRanger's line picked without showing the list");
}

/* Called by the title screen's own input code every title frame (patched in at 0x4bb04f, where the game asks whether
 * it was started by a DirectPlay lobby and then calls the Multiplayer handler itself, with no click). Returns 1 = the
 * lobby flag is set (the game's own path), 0 = the normal title, 2 = done here (the cave returns past the game's
 * call). Inside GameRanger the first title frame runs the Multiplayer path once. */
static int __cdecl gra_title(void)
{
    static int tried;
    BYTE *v;
    DWORD rd;
    int k;
    if (RD32(0x568464)) return 1;
    if (tried || !gr_mode()) return 0;
    tried = 1;
    if (!(v = gr_view())) { logf_("GameRanger auto: unknown GameRanger build - menus left to the player"); return 0; }
    if (!gra_hook_dbp()) { logf_("GameRanger auto: DialogBoxParamA slot not hooked - menus left to the player"); return 0; }
    g_gra = v[0] ? 2 : 1;
    logf_("GameRanger auto: %s (view %02x %02x %02x %02x)", g_gra == 2 ? "hosting -> Create" : "joining -> Connect", v[0], v[1], v[2], v[3]);
    if (g_gra == 1) {
        for (k = 0; k < 3 && !gr_join(); k++) Sleep(500);
        if (k == 3) { g_gra = 0; logf_("GameRanger auto: no host answered - menus left to the player"); return 0; }
        g_menu = 1;
        lstrcpynA(g_menu_host, g_ask_host, sizeof g_menu_host);
        g_menu_tries = 0;
        g_menu_seq = 0;
        g_menu_msg[0] = 0;
        InterlockedIncrement(&g_enum_gen);
        g_menu_t0 = GetTickCount();
    }
    rd = RD32(A_REDRAW);
    *(volatile DWORD *)0x59154c = 2; *(volatile DWORD *)0x591550 = 0; *(volatile DWORD *)0x591558 = 0;   /* as mpmenu run() */
    ((void (__cdecl *)(void))0x432b05)();
    g_gra = 0;
    g_menu = 0;
    if (RD8(A_UISTATE) == 0x1f && RD32(A_SUBSCR) == 1) {
        *(volatile DWORD *)A_REDRAW = rd;
        logf_("GameRanger auto: back on the title%s%s", g_menu_msg[0] ? ": " : "", g_menu_msg);
    } else logf_("GameRanger auto: done, now on screen %lu", (unsigned long)RD32(A_SUBSCR));
    return 2;
}

/* 0x4bb04f "cmp dword [0x568464],0" (7 bytes) -> "call cave; nop; nop". The cave keeps ecx/edx, sets ZF for the
 * game's following "je normal title" from gra_title's answer, and for 2 returns to 0x4bb06b (after the game's own
 * call of the handler: "mov eax,1; jmp" out of the title's input code). */
static void gra_patch(void)
{
    static const BYTE orig[] = { 0x83, 0x3D, 0x64, 0x84, 0x56, 0x00, 0x00, 0x0F, 0x84 };
    static const BYTE mp_pro[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0xC7, 0x05, 0x20, 0xD3, 0x57, 0x00 };
    char *e = getenv("LOTR2NET_GRAUTO");
    BYTE *c, j[7];
    DWORD old;
    if (e && !strcmp(e, "0")) return;
    if (!g_exe_ok) return;
    if (memcmp((void *)0x4bb04f, orig, sizeof orig) || memcmp((void *)0x432b05, mp_pro, sizeof mp_pro)) {
        logf_("WARN GameRanger auto patch: title bytes differ (another mod?) - skipped");
        return;
    }
    if (!(c = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return;
    c[0] = 0x51; c[1] = 0x52;                                                 /* push ecx; push edx */
    c[2] = 0xE8; *(DWORD *)(c + 3) = (DWORD)gra_title - (DWORD)(c + 7);       /* call gra_title */
    c[7] = 0x5A; c[8] = 0x59;                                                 /* pop edx; pop ecx */
    c[9] = 0x83; c[10] = 0xF8; c[11] = 0x02;                                  /* cmp eax,2 */
    c[12] = 0x74; c[13] = 0x03;                                               /* je done */
    c[14] = 0x85; c[15] = 0xC0; c[16] = 0xC3;                                 /* test eax,eax; ret */
    c[17] = 0xC7; c[18] = 0x04; c[19] = 0x24; *(DWORD *)(c + 20) = 0x4bb06b;  /* done: mov dword [esp],0x4bb06b */
    c[24] = 0xC3;                                                             /* ret */
    if (!VirtualProtect(c, 32, PAGE_EXECUTE_READ, &old)) return;
    j[0] = 0xE8; *(DWORD *)(j + 1) = (DWORD)c - (0x4bb04f + 5); j[5] = j[6] = 0x90;
    if (!VirtualProtect((void *)0x4bb04f, 7, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy((void *)0x4bb04f, j, 7);
    VirtualProtect((void *)0x4bb04f, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)0x4bb04f, 7);
    logf_("GameRanger auto patch: title input 4bb04f -> cave %p", (void *)c);
}

static DPHR WINAPI dp2_EnumSessions(Obj *t, DPSD2 *want, DWORD timeout, ENUMSESS_CB cb, void *ctx, DWORD flags)
{
    logf_("EnumSessions timeout=%lu flags=%lu", timeout, flags);
    ensure_init();
    if (!cb) return DPERR_INVALIDPARAMS;
    if (g_in_ask) return DP_OK;   /* a timer tick while the box is open: no list yet */
    if (g_menu == 1) {   /* the in-game menu already has the address: no box, and the list is never shown */
        HWND lb = (HWND)ctx, dlg = NULL;
        char cls[16];
        InterlockedExchange(&g_ask_due, 0);
        if (!(lb && IsWindow(lb) && GetClassNameA(lb, cls, sizeof cls) && !lstrcmpiA(cls, "ListBox")))
            lb = gr_mode() ? thread_listbox() : NULL;   /* GameRanger's hooks hide the list box from ctx */
        if (lb) {
            dlg = GetParent(lb);
            hide_dlg(dlg);
        }
        return menu_enum(want, cb, ctx, lb, dlg);
    }
    if (InterlockedExchange(&g_ask_due, 0) && ask_enabled()) {
        int go;
        g_in_ask = 1;
        go = gr_join() || ask_join(ctx);
        g_in_ask = 0;
        if (!go) return DP_OK;
    }
    enum_once(want, cb, ctx, NULL, 0, NULL);
    return DP_OK;
}

/* One ENUM round trip, off the game thread (review #15, F18). The game polls EnumSessions from its UI loop (the session
 * dialog's WM_TIMER, every 3 s), and the old inline dial (800 ms) + read (3 s) froze it for up to ~4 s per poll. Now a
 * worker does the round trip and keeps the last answer (one worker at a time); the caller waits at most ENUM_WAIT_MS,
 * so a LAN host is still listed on the first call, and a slow one on the next poll. An answer is used only if it is
 * for the same target (host:port) and the same menu join (gen), and is under ENUM_FRESH_MS old. The session name goes
 * through the caller's own buffer for the callback's duration (F18: no shared static name). */
#define ENUM_WAIT_MS 400
#define ENUM_FRESH_MS 10000
static CRITICAL_SECTION enumcs;
static struct { int running, have, result; LONG gen; DWORD t, seq; char target[140]; BYTE info[92]; } EN;
typedef struct { char host[128]; int port; LONG gen; } EArg;

static DWORD WINAPI enum_thread(LPVOID a)
{
    EArg *ea = (EArg *)a;
    SOCKET s;
    BYTE h[16], type, *f = NULL, info[92];
    DWORD n = 0;
    int res = 0;   /* 0 no answer, 1 session info, 2 refused */
    s = dial_to(ea->host, ea->port, 800);
    if (s != INVALID_SOCKET) {
        memcpy(h, MAGIC, 8);
        *(DWORD *)(h + 8) = PROTO;
        *(DWORD *)(h + 12) = MODE_ENUM;
        sendf(s, T_HELLO, h, 16);
        f = readf2(s, &type, &n, 256, 3000);
        closesocket(s);
        if (f && type == T_INFO && n >= 92) { memcpy(info, f, 92); res = 1; }
        else if (f) res = 2;
        free(f);
    }
    EnterCriticalSection(&enumcs);
    EN.running = 0;
    EN.have = 1;
    EN.result = res;
    EN.gen = ea->gen;
    snprintf(EN.target, sizeof EN.target, "%s:%d", ea->host, ea->port);
    if (res == 1) memcpy(EN.info, info, 92);
    EN.t = GetTickCount();
    EN.seq++;
    LeaveCriticalSection(&enumcs);
    free(ea);
    return 0;
}

/* Returns 1 a session was listed (its name in name), 0 the newest finished round found none, -2 it was refused,
 * -1 no finished round for this target yet. *seq = that round's number. */
static int enum_once(DPSD2 *want, ENUMSESS_CB cb, void *ctx, char *name, int nn, DWORD *seq)
{
    char host[128], target[140];
    int port, r = -1;
    LONG gen = g_enum_gen;
    DWORD t0 = GetTickCount(), myseq = 0;
    BYTE info[92];
    if (name && nn) name[0] = 0;
    get_target(host, &port);
    snprintf(target, sizeof target, "%s:%d", host, port);
    EnterCriticalSection(&enumcs);
    if (!EN.running) {   /* start a fresh round; the one this call can use is any round that finishes after now */
        EArg *ea = (EArg *)malloc(sizeof *ea);
        if (ea) {
            HANDLE th;
            lstrcpynA(ea->host, host, sizeof ea->host);
            ea->port = port;
            ea->gen = gen;
            EN.running = 1;
            th = CreateThread(NULL, 0, enum_thread, ea, 0, NULL);
            if (th) CloseHandle(th); else { EN.running = 0; free(ea); }
        }
    }
    for (;;) {
        if (EN.have && EN.gen == gen && !strcmp(EN.target, target) && GetTickCount() - EN.t < ENUM_FRESH_MS) {
            r = EN.result == 1 ? 1 : EN.result == 2 ? -2 : 0;
            myseq = EN.seq;
            if (r == 1) memcpy(info, EN.info, 92);
            if (!EN.running) break;   /* nothing newer is coming */
        }
        if (GetTickCount() - t0 >= ENUM_WAIT_MS) break;
        LeaveCriticalSection(&enumcs);
        Sleep(10);
        EnterCriticalSection(&enumcs);
    }
    LeaveCriticalSection(&enumcs);
    if (seq) *seq = myseq;
    if (r == 1) {
        DPSD2 d;
        char nm[64];
        DWORD to = 0;
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = *(DWORD *)info;
        d.dwMaxPlayers = *(DWORD *)(info + 4);
        d.dwCurrentPlayers = *(DWORD *)(info + 8);
        memcpy(&d.guidInstance, info + 12, 16);
        memcpy(&d.guidApplication, info + 12, 16); /* game compares nothing; real DP echoes the app guid */
        if (want) d.guidApplication = want->guidApplication;
        memcpy(nm, info + 28, 64);
        nm[63] = 0;
        d.lpszSessionName = nm;
        logf_("EnumSessions: callback '%s' %lu/%lu (round %lu)", nm, d.dwCurrentPlayers, d.dwMaxPlayers, myseq);
        cb(&d, &to, 0, ctx);
        if (name && nn) lstrcpynA(name, nm, nn);
    } else logf_("EnumSessions: no session (%s)", r == -1 ? "round still running" : r == -2 ? "reply: refused" : "reply: none");
    return r;
}

static DPHR WINAPI dp2_GetCaps(Obj *t, DPCAPS_ *c, DWORD flags)
{
    logf_("GetCaps flags=%lu", flags);
    if (!c || c->dwSize < sizeof *c) return DPERR_INVALIDPARAMS;
    memset((BYTE *)c + 4, 0, sizeof *c - 4);
    /* dplay.h: DPCAPS_ISHOST 0x2, GUARANTEEDOPTIMIZED 0x20, GUARANTEEDSUPPORTED 0x40 (was 0x10|0x8 = keepalive- and
     * group-optimized; F23). The game only logs baud, max players, latency and header (mp-dplay-surface.md). */
    c->dwFlags = (G.host ? 0x2 : 0) | 0x20 | 0x40;
    c->dwMaxBufferSize = MAXRECV;   /* the most Send accepts; the game only logs it */
    c->dwMaxPlayers = 65536;
    c->dwHundredBaud = 0;
    c->dwLatency = g_slowlink ? 250 : 50; /* 65536/250/8 would flip the game's modem flag */
    c->dwMaxLocalPlayers = 1;
    c->dwHeaderLength = 8;
    c->dwTimeout = 1000;
    return DP_OK;
}

static int hostquit_due(void)   /* LOTR2NET_TEST_HOSTQUIT=<turn> or ini test_hostquit=<turn>: the rig's stand-in for the
                                 * host's lord being eliminated (game thread) */
{
    static long at = -1;
    if (at < 0) {
        char v[16] = "", *e = getenv("LOTR2NET_TEST_HOSTQUIT");
        at = e && *e ? atol(e) : ini_get("test_hostquit", v, sizeof v) ? atol(v) : 0;
        if (at > 0) logf_("TEST HOSTQUIT armed at turn %ld", at);
    }
    return at > 0 && RD32(0x553240) >= (DWORD)at;
}
static DPHR WINAPI dp2_Send(Obj *t, DPID from, DPID to, DWORD flags, BYTE *buf, DWORD size)
{
    DPHR r = DP_OK;
    BYTE *m;
    if (!buf || !size) return DPERR_INVALIDPARAMS;
    /* DPERR_SENDTOOBIG = MAKE_DPHRESULT(230) = 0x887700E6 (dplay.h). The game's receive buffer is MAXRECV bytes. */
    if (size > MAXRECV) return DPERR_SENDTOOBIG;
    if (G.ghost) return DPERR_INVALIDOBJECT;   /* v10: this game left; the hub only relays for the others */
    if (G.host && G.locked && g_exe_ok && hostquit_due()) {   /* test only: the host's game leaves at turn N */
        DPID me = 0;
        int k;
        EnterCriticalSection(&cs);
        for (k = 0; k < MAXPL; k++) if (G.pl[k].used && G.pl[k].conn < 0) { me = G.pl[k].id; break; }
        LeaveCriticalSection(&cs);
        logf_("TEST HOSTQUIT at turn %lu: this game leaves (player %08lx)", (unsigned long)RD32(0x553240), (unsigned long)me);
        if (me) dp2_DestroyPlayer(t, me);
        do_close(1);
        return DPERR_INVALIDOBJECT;
    }
    sync_on_send(buf, size);   /* game thread: our sync check at msgs 0x08/0x0c/0x0a (sync.c) */
    dg_on_send(buf, size);
    if (rs_test_hold(from, (const BYTE *)buf, size)) return DP_OK;   /* test switch only (test_corrupt mode 8) */
    m = (BYTE *)malloc(8 + size);
    if (!m) { logf_("Send: out of memory"); return DPERR_GENERIC; }
    ((DWORD *)m)[0] = from; ((DWORD *)m)[1] = to; memcpy(m + 8, buf, size);
    rs_host_send(m + 8, size);   /* host, msg 0x16: may ship the resync file first and turn the verdict into a rollback */
    tr_on_host_send(to, buf, size);   /* host: our 0x02 tells which lord each joiner plays (trust.c) */
    tr_on_send_offer(buf, size);      /* host: our 0x49 offers may be answered with 0x4a / 0x4b */
    EnterCriticalSection(&cs);
    logmsg("SEND", from, to, buf, size);
    if (!G.open) r = DPERR_INVALIDOBJECT;
    else if (G.host) {
        if (to == 0) bcast(-1, T_DATA, m, 8 + size);   /* only queued: a stalled peer never blocks the game thread */
        else {
            Player *p = pl_find(to);
            if (!p) r = DPERR_INVALIDPLAYER;
            else if (p->conn >= 0) osend(G.conns[p->conn].o, T_DATA, m, 8 + size);
        }
    } else if (!G.connected) r = DPERR_NOCONNECTION;
    else if (jsend(T_DATA, m, 8 + size)) r = DPERR_NOCONNECTION;
    LeaveCriticalSection(&cs);
    free(m);
    return r;
}

static DPHR WINAPI dp2_Receive(Obj *t, DPID *from, DPID *to, DWORD flags, BYTE *buf, DWORD *size)
{
    Msg *m;
    DPHR r = DPERR_NOMESSAGES;
    if (!from || !to || !size) return DPERR_INVALIDPARAMS;
    sy_flush_title();   /* game thread, cs not held (sync.c) */
    EnterCriticalSection(&cs);
    if (!G.open) r = DPERR_INVALIDOBJECT;
    else if ((m = G.qh) != NULL) {
        if (!buf || *size < m->len) { *size = m->len; r = DPERR_BUFFERTOOSMALL; logf_("Receive: buffer too small, need %lu", m->len); }
        else {
            memcpy(buf, m->data, m->len);
            /* F21: a broadcast is delivered to this receiver's own id (real DirectPlay never hands out to = 0) */
            *from = m->from; *to = m->to ? m->to : G.myid; *size = m->len;
            logmsg("RECV", m->from, m->to, m->data, m->len);
            if (!(flags & DPRECEIVE_PEEK)) {
                if (m->from) rs_on_deliver(m->data, m->len);   /* joiner, msg 0x16: rollback bookkeeping (resync.c) */
                dg_on_take(m->t_us);
                G.qh = m->next; if (!G.qh) G.qt = NULL; G.qcount--; g_qout++; free(m);
            }
            r = DP_OK;
        }
    }
    LeaveCriticalSection(&cs);
    if (r == DPERR_NOMESSAGES) dg_on_pump();
    return r;
}

static DPHR WINAPI dp2_SetSessionDesc(Obj *t, DPSD2 *d, DWORD flags)
{
    logf_("SetSessionDesc flags=%lu -> session locked", flags);
    EnterCriticalSection(&cs);
    if (G.open && G.host) G.locked = 1;
    LeaveCriticalSection(&cs);
    return DP_OK;
}

/* stubs: right stdcall arity so a stray call cannot corrupt the stack */
#define STUB(nm, ...) static DPHR WINAPI dp2_##nm(Obj *t, __VA_ARGS__) { logf_("UNSUPPORTED %s", #nm); return DPERR_UNSUPPORTED; }
STUB(AddPlayerToGroup, DWORD a, DWORD b)
STUB(CreateGroup, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
STUB(DeletePlayerFromGroup, DWORD a, DWORD b)
STUB(DestroyGroup, DWORD a)
STUB(EnumGroupPlayers, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
STUB(EnumGroups, DWORD a, DWORD b, DWORD c, DWORD d)
STUB(EnumPlayers, DWORD a, DWORD b, DWORD c, DWORD d)
STUB(GetGroupData, DWORD a, DWORD b, DWORD c, DWORD d)
STUB(GetGroupName, DWORD a, DWORD b, DWORD c)
STUB(GetMessageCount, DWORD a, DWORD b)
STUB(GetPlayerAddress, DWORD a, DWORD b, DWORD c)
STUB(GetPlayerCaps, DWORD a, DWORD b, DWORD c)
STUB(GetPlayerData, DWORD a, DWORD b, DWORD c, DWORD d)
STUB(GetPlayerName, DWORD a, DWORD b, DWORD c)
STUB(GetSessionDesc, DWORD a, DWORD b)
STUB(SetGroupData, DWORD a, DWORD b, DWORD c, DWORD d)
STUB(SetGroupName, DWORD a, DWORD b, DWORD c)
STUB(SetPlayerData, DWORD a, DWORD b, DWORD c, DWORD d)
STUB(SetPlayerName, DWORD a, DWORD b, DWORD c)
static DPHR WINAPI dp2_Initialize(Obj *t, DWORD a) { logf_("Initialize"); return DPERR_ALREADYINITIALIZED; }

static void *dp2_vt[32] = {
    dp2_QI, dp2_AddRef, dp2_Release,                           /* 0x00 */
    dp2_AddPlayerToGroup, dp2_Close, dp2_CreateGroup,          /* 0x0C 0x10 0x14 */
    dp2_CreatePlayer, dp2_DeletePlayerFromGroup, dp2_DestroyGroup, /* 0x18 0x1C 0x20 */
    dp2_DestroyPlayer, dp2_EnumGroupPlayers, dp2_EnumGroups,   /* 0x24 0x28 0x2C */
    dp2_EnumPlayers, dp2_EnumSessions, dp2_GetCaps,            /* 0x30 0x34 0x38 */
    dp2_GetGroupData, dp2_GetGroupName, dp2_GetMessageCount,   /* 0x3C 0x40 0x44 */
    dp2_GetPlayerAddress, dp2_GetPlayerCaps, dp2_GetPlayerData, /* 0x48 0x4C 0x50 */
    dp2_GetPlayerName, dp2_GetSessionDesc, dp2_Initialize,     /* 0x54 0x58 0x5C */
    dp2_Open, dp2_Receive, dp2_Send,                           /* 0x60 0x64 0x68 */
    dp2_SetGroupData, dp2_SetGroupName, dp2_SetPlayerData,     /* 0x6C 0x70 0x74 */
    dp2_SetPlayerName, dp2_SetSessionDesc                      /* 0x78 0x7C */
};

/* ---- v1 IDirectPlay object: the game only does QueryInterface + Release on it ---- */
static DPHR WINAPI dp1_QI(Obj *t, const GUID *iid, void **out) { logf_("v1 QueryInterface"); return dp2_QI(t, iid, out); }
static ULONG WINAPI dp1_AddRef(Obj *t) { return (ULONG)InterlockedIncrement(&dp1ref); }
static ULONG WINAPI dp1_Release(Obj *t) { LONG r = InterlockedDecrement(&dp1ref); logf_("v1 Release -> %ld", r); return (ULONG)(r < 0 ? 0 : r); }
/* F19: every IDirectPlay (v1) method gets a stub with its real stdcall arity, so a stray v1 call returns
 * DPERR_UNSUPPORTED with the stack intact instead of unbalancing it (one generic 1-arg stub did that). Order and
 * argument counts are dplay.h's IDirectPlay. */
#define DP1S(nm, ...) static DPHR WINAPI dp1_##nm(Obj *t, ##__VA_ARGS__) { logf_("UNSUPPORTED v1 %s", #nm); return DPERR_UNSUPPORTED; }
DP1S(AddPlayerToGroup, DWORD a, DWORD b)
DP1S(Close)
DP1S(CreatePlayer, DWORD a, DWORD b, DWORD c, DWORD d)
DP1S(CreateGroup, DWORD a, DWORD b, DWORD c)
DP1S(DeletePlayerFromGroup, DWORD a, DWORD b)
DP1S(DestroyPlayer, DWORD a)
DP1S(DestroyGroup, DWORD a)
DP1S(EnableNewPlayers, DWORD a)
DP1S(EnumGroupPlayers, DWORD a, DWORD b, DWORD c, DWORD d)
DP1S(EnumGroups, DWORD a, DWORD b, DWORD c, DWORD d)
DP1S(EnumPlayers, DWORD a, DWORD b, DWORD c, DWORD d)
DP1S(EnumSessions, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
DP1S(GetCaps, DWORD a)
DP1S(GetMessageCount, DWORD a, DWORD b)
DP1S(GetPlayerCaps, DWORD a, DWORD b)
DP1S(GetPlayerName, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
DP1S(Initialize, DWORD a)
DP1S(Open, DWORD a)
DP1S(Receive, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
DP1S(SaveSession, DWORD a)
DP1S(Send, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
DP1S(SetPlayerName, DWORD a, DWORD b, DWORD c)
static void *dp1_vt[25] = {
    dp1_QI, dp1_AddRef, dp1_Release,
    dp1_AddPlayerToGroup, dp1_Close, dp1_CreatePlayer, dp1_CreateGroup, dp1_DeletePlayerFromGroup,
    dp1_DestroyPlayer, dp1_DestroyGroup, dp1_EnableNewPlayers, dp1_EnumGroupPlayers, dp1_EnumGroups,
    dp1_EnumPlayers, dp1_EnumSessions, dp1_GetCaps, dp1_GetMessageCount, dp1_GetPlayerCaps,
    dp1_GetPlayerName, dp1_Initialize, dp1_Open, dp1_Receive, dp1_SaveSession, dp1_Send, dp1_SetPlayerName
};

/* ------------------------------------------------------------------ exports */
__declspec(dllexport) DPHR WINAPI DirectPlayCreate(GUID *sp, void **out, void *outer)
{
    ensure_init();
    logf_("DirectPlayCreate sp=%08lx", sp ? sp->Data1 : 0);
    if (!out) return DPERR_INVALIDPARAMS;
    InterlockedIncrement(&dp1ref);
    InterlockedExchange(&g_ask_due, 1);   /* one per Connect press: the next EnumSessions asks for the address */
    *out = &dp1obj;
    return DP_OK;
}
__declspec(dllexport) DPHR WINAPI DirectPlayEnumerateA(ENUMSP_CB cb, void *ctx)
{
    logf_("DirectPlayEnumerateA");
    if (!cb) return DPERR_INVALIDPARAMS;
    { /* the ONLY connection this DLL offers; say exactly what it will do (the game lists this name) */
        static char nm[160];
        char host[128];
        int port, lp = listen_port();
        get_target(host, &port);
        if (ask_enabled())
            snprintf(nm, sizeof nm, "TCP/IP: Create hosts on port %d, Connect asks for the address", lp);
        else if (!strcmp(host, "127.0.0.1"))
            snprintf(nm, sizeof nm, "TCP/IP: host on port %d (no join address - run Join.cmd)", lp);
        else
            snprintf(nm, sizeof nm, "TCP/IP: host on port %d, or join %s:%d", lp, host, port);
        cb(&SPGUID_TCPIP, nm, 6, 0, ctx);
    }
    { /* Lords2's "Connection method" dialog passes its list box as ctx and then pre-selects "Sierra Internet Gaming
       * System", which never matches, so the player had to pick our one line by hand (Matthew 2026-10-06: "only one
       * option ... should take you right into Connect / Create / Cancel"). With one method there is nothing to
       * choose: the dialog is made invisible (no flash) and gets the double-click it would have had, its own path. */
        HWND lb = (HWND)ctx;
        char cls[16];
        if (lb && IsWindow(lb) && GetClassNameA(lb, cls, sizeof cls) && !lstrcmpiA(cls, "ListBox")) {
            HWND dlg = GetParent(lb);
            SetWindowLongA(dlg, GWL_EXSTYLE, GetWindowLongA(dlg, GWL_EXSTYLE) | WS_EX_LAYERED);
            SetLayeredWindowAttributes(dlg, 0, 0, LWA_ALPHA);
            PostMessageA(lb, LB_SETCURSEL, 0, 0);
            PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(lb), LBN_DBLCLK), (LPARAM)lb);
            logf_("connection method: one choice, picked without showing the list");
        } else if (gr_mode() && (lb = thread_listbox()) != NULL) gra_pick_method(lb);
    }
    return DP_OK;
}

/* Review #7: 1 while a session is open (hosting or joined). For ddraw: background=0 must not pause a peer that other
 * players are waiting on (ddraw change requested in dplayx/AGENTS.md; ddraw is not edited from here). */
__declspec(dllexport) int WINAPI LF_NetSessionOpen(void) { return G.open != 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
    if (why == DLL_PROCESS_ATTACH) {
        char *sl;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&cs);
        InitializeCriticalSection(&logcs);
        InitializeCriticalSection(&dnscs);
        InitializeCriticalSection(&findcs);
        InitializeCriticalSection(&hscs);
        InitializeCriticalSection(&enumcs);
        InitializeCriticalSection(&logwr);
        {   /* LOTR2NET_SLOWLINK=0/no/false/off (or empty) means off, anything else on */
            char *e = getenv("LOTR2NET_SLOWLINK");
            g_slowlink = e && *e && strcmp(e, "0") && _stricmp(e, "no") && _stricmp(e, "false") && _stricmp(e, "off");
        }
        t0 = GetTickCount();
        GetModuleFileNameA(NULL, exedir, MAX_PATH);
        sl = strrchr(exedir, '\\');
        if (!sl) sl = strrchr(exedir, '/');
        if (sl) sl[1] = 0; else exedir[0] = 0;
        G.lsock = G.jsock = INVALID_SOCKET;
        dg_init();
        lf_init();
        dp2obj.vt = dp2_vt;
        dp1obj.vt = dp1_vt;
        logf_("---- dplayx.dll v%d loaded (pid %lu)", DLL_VERSION, GetCurrentProcessId());
        exe_patch_all();
        victory_patch();
        gra_patch();
        logf_("PROTO %lu (dll v%d, pace %d, exe_ok %d, slowlink %d, mpfix %d)", PROTO, DLL_VERSION, g_pace_ok, g_exe_ok, g_slowlink, g_mpfix);
    } else if (why == DLL_PROCESS_DETACH) {
        log_flush_exit();   /* write what is buffered; at process exit other threads may have died holding a lock,
                             * so it only tries */
        lf_copy_all();      /* the game's last status.txt lines come after our Close */
    }
    return TRUE;
}
