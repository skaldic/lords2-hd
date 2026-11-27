/* diag.c - one line per second of network and pacing numbers in dplayx.log (v12). Included by dplayx.c. Our own code. ASCII only.
 *
 * Why: the heartbeat lines could only show THAT a game ran slowly, not why (is it the link, the hard stop or this PC?),
 * and a frozen peer writes up to 140 of them per second. These numbers replace them; log_heartbeats=1 in lords2hd.ini
 * (or env LOTR2NET_LOG_HEARTBEATS=1) brings the per-message heartbeat lines back. diag=0 / LOTR2NET_DIAG=0 turns this off.
 *
 *   DIAG sim=<SimTick> ticks=<advanced> ov=<overlay> pump n=<count> avg=<ms> max=<ms> lead=<low>..<high> held=<ms> by lord <l, 0 none>
 *        q n=<taken> avg=<ms> max=<ms> depth=<now> hb sent=<all> new=<with a new tick> slots=<lord:heartbeat,...>
 *   DIAG PEER <dpid> rtt=<ms> min=<ms> max=<ms> n=<replies> sent=<pings> rx=<messages> hb=<new ticks> gap=<ms> out=<bytes>
 *
 * pump: time between Receive calls that found the queue empty, so about one game loop as seen from here: a long max is
 *       the game thread busy or blocked; it keeps running while the sim is held.
 * lead: own heartbeat slot - the slowest other live lord's slot (FUN_0043e50d's filter, as exepatch.c's pace cave). The
 *       read is on the game thread, from the same table NetPacing reads. held: time at the mpfix hard stop (lead >= 30;
 *       stock 401), and the lord it waited on. The filter does not know who is in a battle, so with 3+ players the lord
 *       named may be a bystander: slots= is logged so that can be checked.
 * q:    time a message waited between our reader and the game taking it.
 * rtt:  T_PING / T_PONG, answered on the reader thread, never shown to the game, so game frames do not count. Timed over
 *       the same TCP link and writer queue as game messages; to another joiner it goes through the host and back.
 * gap:  longest time between new-tick heartbeats of that player arriving at our reader: link jitter plus its sender's own
 *       stalls, before any waiting for our game.
 * out:  bytes in the writer queue to that peer (joiner: to the host) now. It stays near 0 unless the link is backing up.
 *
 * Session clock (v13): every log line in the session folder, on every PC, is stamped [S <seconds>] with the HOST's time
 * since its Open(create). The host's own T_PONG answers carry that time (ss, ok=1; the host clears ok on every pong it
 * relays, so no joiner can set another's clock). A joiner takes offset = ss - the middle of the round trip, from the
 * fastest answer of each second (Cristian's method): the error is half the round trip's asymmetry, ~1 ms on a LAN,
 * typically 5-20 ms over the internet. Re-estimated each second (PC clocks drift 10-100 ppm); the clock never runs
 * backwards. Needs diag on (the pings); a joiner shows its own load time ([ 12.345]) until the first answer. */
#define T_PING 13   /* v12: [DgPing] from -> to, relayed by the host; only `from`'s clock is in it */
#define T_PONG 14   /* v12: the same DgPing back to `from`; v13: + the host's session clock when the host answers */
typedef struct { DPID from, to; DWORD seq, us, ss_lo, ss_hi, ok; } DgPing;   /* 28 bytes, no padding; ss: session us */
#define DG_PING_PER_S 32   /* T_PING/T_PONG frames taken from one conn per second (the DLL makes 2 per peer) */
#define DG_US 1000000ull

typedef struct {
    DPID id;
    DWORD sent, n, sum_us, min_us, max_us, rx, hb, gap_us, hb_us;
    int hb_seen;
    BYTE hb_tick;
} DgPeer;
/* the whole session, per player, for the summary in session.txt (sesslog.c) */
typedef struct {
    DPID id;
    char name[32];
    DWORD n, min_us, max_us, first_ms, last_ms;   /* first_ms: joined; last_ms: its last answer (session clock) */
    ULONGLONG sum_us;
} DgRoster;

static CRITICAL_SECTION dgcs;   /* a leaf lock: nothing else is taken under it. Order: cs -> dgcs */
static ULONGLONG dg_freq;
static volatile LONG g_diag;    /* the counters run: dg_start said so for this session */
static struct {
    DgPeer peer[MAXPL];
    DWORD pump_n, pump_sum_us, pump_max_us, pump_us, ticks, sim;
    long lead_lo, lead_hi, pv_lead;
    DWORD held_us, held_lord[7], pv_lord;
    DWORD q_n, q_sum_us, q_max_us, tx, tx_new;
    int have_lead, have_sim, have_tx, pv_ok;
    BYTE tx_tick;
    DWORD best_rtt;           /* joiner: the fastest host answer this second ... */
    LONGLONG best_off;        /* ... and the clock offset it gives */
    int have_best;
} D;
/* the whole session, for its summary (sesslog.c): reset by clk_open only (dg_start's reset of D comes after a joiner's
 * reader may already have added players). dgcs */
static struct {
    DgRoster ro[MAXPL];       /* every player of the session, this PC's own included */
    ULONGLONG held_all_us;
    DWORD pump_max_all_us;
} DS;

/* ---- session clock: session us = dg_us64() + g_clk_off once g_clk_ok */
static volatile LONGLONG g_clk_off, g_clk_last;
static volatile LONG g_clk_ok;

static int dg_cfg(const char *env, const char *key, int def)
{
    char v[16];
    char *e = getenv(env);
    if (e && *e) return cfg_on(e);
    if (ini_get(key, v, sizeof v)) return cfg_on(v);
    return def;
}

static void dg_init(void)   /* DllMain, after exedir */
{
    LARGE_INTEGER f;
    InitializeCriticalSection(&dgcs);
    QueryPerformanceFrequency(&f);
    dg_freq = (ULONGLONG)f.QuadPart;
    g_log_hb = dg_cfg("LOTR2NET_LOG_HEARTBEATS", "log_heartbeats", 0);
}

/* microseconds since boot. Split so c * 1e6 cannot overflow (a 10 MHz counter would after 21 days of uptime) */
static ULONGLONG dg_us64(void)
{
    LARGE_INTEGER c;
    ULONGLONG q;
    QueryPerformanceCounter(&c);
    q = (ULONGLONG)c.QuadPart;
    return q / dg_freq * DG_US + q % dg_freq * DG_US / dg_freq;
}

/* microseconds, wraps in 71 minutes: only differences are used. GetTickCount's 15.6 ms steps are too coarse for a rtt */
static DWORD dg_us(void) { return (DWORD)dg_us64(); }

static LONGLONG clk_read(volatile LONGLONG *v) { return InterlockedCompareExchange64((LONGLONG *)v, 0, 0); }

/* the session time now in us; 0 = no session clock yet. Any thread, no lock: logf_ calls it */
static int clk_now(ULONGLONG *out)
{
    LONGLONG t, last;
    if (!g_clk_ok || !dg_freq) return 0;
    t = (LONGLONG)dg_us64() + clk_read(&g_clk_off);
    last = clk_read(&g_clk_last);
    if (t < last) t = last;   /* a new estimate never turns the clock back */
    else InterlockedExchange64((LONGLONG *)&g_clk_last, t);
    if (t < 0) t = 0;
    *out = (ULONGLONG)t;
    return 1;
}
static int clk_now_ms(DWORD *ms)
{
    ULONGLONG t;
    if (!clk_now(&t)) return 0;
    *ms = (DWORD)(t / 1000u);
    return 1;
}

/* dp2_Open: a host starts the session clock at 0; a joiner has none until the host's first answer */
static void clk_open(int host)
{
    EnterCriticalSection(&dgcs);
    memset(&DS, 0, sizeof DS);
    LeaveCriticalSection(&dgcs);
    InterlockedExchange64((LONGLONG *)&g_clk_last, 0);
    if (host) InterlockedExchange64((LONGLONG *)&g_clk_off, -(LONGLONG)dg_us64());
    InterlockedExchange(&g_clk_ok, host);
}

/* Export for the HD ddraw (lords2hd.c logf_): the session time in ms; 0 = none (no session yet, or no answer from the
 * host yet). Its log lines then carry the same [S ...] stamp as ours */
__declspec(dllexport) int WINAPI LF_NetClock(DWORD *ms)
{
    return ms && clk_now_ms(ms);
}

static DgPeer *dg_peer(DPID id)   /* dgcs held; a new id takes a free row, none left = NULL */
{
    int i, fr = -1;
    for (i = 0; i < MAXPL; i++) {
        if (D.peer[i].id == id) return &D.peer[i];
        if (!D.peer[i].id && fr < 0) fr = i;
    }
    if (fr < 0) return NULL;
    D.peer[fr].id = id;
    return &D.peer[fr];
}

/* cs held (qpush): a game message reached our reader */
static void dg_on_rx(DPID from, const BYTE *d, DWORD len)
{
    DgPeer *p;
    if (!from || !g_diag) return;
    EnterCriticalSection(&dgcs);
    if ((p = dg_peer(from)) != NULL) {
        p->rx++;
        if (len == 8 && d[0] == 0x0a && (!p->hb_seen || p->hb_tick != d[2])) {   /* a frozen peer repeats its tick: not new */
            DWORD now = dg_us();
            if (p->hb_seen && now - p->hb_us > p->gap_us) p->gap_us = now - p->hb_us;
            p->hb_seen = 1; p->hb_tick = d[2]; p->hb_us = now; p->hb++;
        }
    }
    LeaveCriticalSection(&dgcs);
}

/* cs held (dp2_Receive): the game took a message that waited since m->t_us */
static void dg_on_take(DWORD t_us)
{
    DWORD w;
    if (!g_diag) return;
    w = dg_us() - t_us;
    EnterCriticalSection(&dgcs);
    D.q_n++; D.q_sum_us += w;
    if (w > D.q_max_us) D.q_max_us = w;
    LeaveCriticalSection(&dgcs);
}

/* game thread (dp2_Send), cs not held */
static void dg_on_send(const BYTE *buf, DWORD size)
{
    if (!g_diag || size != 8 || buf[0] != 0x0a) return;
    EnterCriticalSection(&dgcs);
    D.tx++;
    if (!D.have_tx || D.tx_tick != buf[2]) { D.tx_new++; D.tx_tick = buf[2]; D.have_tx = 1; }
    LeaveCriticalSection(&dgcs);
}

/* the slowest other live lord; 0 = none */
static DWORD dg_slowest(DWORD own, long *slot)
{
    DWORD i, who = 0;
    for (i = 1; i < 7; i++) {
        long s;
        if (i == own || !RD8(VA_NOBLE + i * 0x160 + 5) || !RD8(VA_NOBLE + i * 0x160 + 4) || RD8(0x553d76 + i * 0x2c)) continue;
        s = (long)RD32(VA_HBTAB + i * 4);
        if (!who || s < *slot) { who = i; *slot = s; }
    }
    return who;
}

/* game thread (dp2_Receive found no message), cs not held. Every read of the game's memory is here, between its own
 * writes, so the table is never read halfway through one */
static void dg_on_pump(void)
{
    DWORD now, sim = 0, own = 0, who = 0;
    long lead = 0, slot = 0;
    int ok = 0;
    if (!g_diag) return;
    now = dg_us();
    if (g_exe_ok) {
        sim = RD32(VA_SIMTICK);
        own = RD32(0x57c8cc);
        if (own >= 1 && own < 7 && (who = dg_slowest(own, &slot)) != 0) { lead = (long)RD32(VA_HBTAB + own * 4) - slot; ok = 1; }
    }
    EnterCriticalSection(&dgcs);
    if (D.pump_us) {
        DWORD dt = now - D.pump_us;
        D.pump_n++; D.pump_sum_us += dt;
        if (dt > D.pump_max_us) D.pump_max_us = dt;
        /* the state this interval began in, so the time goes to the stop that caused it */
        if (D.pv_ok && D.pv_lead >= (g_mpfix ? 30 : 401)) { D.held_us += dt; DS.held_all_us += dt; if (D.pv_lord < 7) D.held_lord[D.pv_lord] += dt; }
        if (dt > DS.pump_max_all_us) DS.pump_max_all_us = dt;
    }
    D.pump_us = now;
    if (g_exe_ok) {
        if (D.have_sim && sim > D.sim) D.ticks += sim - D.sim;   /* a phase change zeroes SimTick: not a tick */
        D.sim = sim; D.have_sim = 1;
    }
    D.pv_ok = ok; D.pv_lead = lead; D.pv_lord = who;
    if (ok) {
        if (!D.have_lead || lead < D.lead_lo) D.lead_lo = lead;
        if (!D.have_lead || lead > D.lead_hi) D.lead_hi = lead;
        D.have_lead = 1;
    }
    LeaveCriticalSection(&dgcs);
}

static DgRoster *dg_roster(DPID id)   /* dgcs held; a new id takes a free row, none left = NULL */
{
    int i, fr = -1;
    for (i = 0; i < MAXPL; i++) {
        if (DS.ro[i].id == id) return &DS.ro[i];
        if (!DS.ro[i].id && fr < 0) fr = i;
    }
    if (fr < 0) return NULL;
    memset(&DS.ro[fr], 0, sizeof DS.ro[fr]);
    DS.ro[fr].id = id;
    return &DS.ro[fr];
}

/* cs held (pl_add): every player of the session goes in the summary, with when it came */
static void dg_roster_add(DPID id, const char *name)
{
    DgRoster *r;
    DWORD ms;
    EnterCriticalSection(&dgcs);
    if ((r = dg_roster(id)) != NULL) {
        lstrcpynA(r->name, name ? name : "", sizeof r->name);
        if (!r->first_ms && clk_now_ms(&ms)) r->first_ms = ms ? ms : 1;
    }
    LeaveCriticalSection(&dgcs);
}

static void dg_rtt(const DgPing *p)   /* cs held */
{
    ULONGLONG now = dg_us64();
    DWORD us = (DWORD)now - p->us;
    DgPeer *r;
    DgRoster *ro;
    if (us > 120u * 1000000u) return;   /* not a stamp of ours this session */
    EnterCriticalSection(&dgcs);
    if ((r = dg_peer(p->to)) != NULL) {
        if (!r->n || us < r->min_us) r->min_us = us;
        if (us > r->max_us) r->max_us = us;
        r->n++; r->sum_us += us;
    }
    if ((ro = dg_roster(p->to)) != NULL) {
        DWORD ms;
        if (clk_now_ms(&ms)) ro->last_ms = ms;   /* its last answer */
        if (!ro->n || us < ro->min_us) ro->min_us = us;
        if (us > ro->max_us) ro->max_us = us;
        ro->n++; ro->sum_us += us;
    }
    if (!G.host && p->ok && (!D.have_best || us < D.best_rtt)) {   /* the host's clock: the middle of the round trip */
        ULONGLONG ss = (ULONGLONG)p->ss_hi << 32 | p->ss_lo;
        D.best_rtt = us;
        D.best_off = (LONGLONG)ss - (LONGLONG)(now - us / 2);
        D.have_best = 1;
    }
    LeaveCriticalSection(&dgcs);
}

/* cs held. Reader thread: ci = the conn it came from on the host, -1 on a joiner. A ping is answered whatever diag= says,
 * so one player's diag=0 does not leave the other with no rtt */
static void dg_on_frame(int ci, BYTE type, const BYTE *f, DWORD n)
{
    DgPing p;
    DPID dest;
    if (n != sizeof p) return;
    memcpy(&p, f, sizeof p);
    dest = type == T_PING ? p.to : p.from;
    if (ci >= 0) {
        Conn *c = &G.conns[ci];
        Player *src = pl_find(type == T_PING ? p.from : p.to);
        DWORD now = GetTickCount();
        if (now - c->dg_t >= 1000) { c->dg_t = now; c->dg_n = 0; }
        if (++c->dg_n > DG_PING_PER_S) return;
        if (!src || src->conn != ci) return;   /* a peer may only speak for players it created */
        if (dest != G.myid) {
            Player *d = pl_find(dest);
            p.ok = 0;   /* v13: only the host's own answers carry the session clock */
            if (d && d->conn >= 0) osend(G.conns[d->conn].o, type, &p, sizeof p);
            return;
        }
    } else if (dest != G.myid) return;
    if (type == T_PONG) { dg_rtt(&p); return; }
    {
        ULONGLONG ss;
        p.ok = G.host && clk_now(&ss);
        if (p.ok) { p.ss_lo = (DWORD)ss; p.ss_hi = (DWORD)(ss >> 32); } else p.ss_lo = p.ss_hi = 0;
    }
    if (ci >= 0) osend(G.conns[ci].o, T_PONG, &p, sizeof p);
    else jsend(T_PONG, &p, sizeof p);
}

/* two printf arguments, whole ms and tenths, for "%lu.%lu" */
#define DG_MS(us) (unsigned long)((us) / 1000u), (unsigned long)(((us) % 1000u) / 100u)

/* the diag thread, cs not held: ping everyone, write what the last second added up to */
static void dg_second(DWORD *seq)
{
    typedef struct { DPID id; DWORD out; } Row;
    Row row[MAXPL];
    DgPeer snap[MAXPL];
    char slots[96] = "";
    DWORD sim, ticks, pump_n, pump_sum, pump_max, held, lord[7], q_n, q_sum, q_max, tx, tx_new, ov = 0, qd;
    long lo, hi;
    int nr = 0, i, have, o = 0, top = 0, clk_set = 0, clk_had = 0;
    LONGLONG clk_was = 0, clk_new = 0;
    DWORD clk_rtt = 0;
    (*seq)++;
    if (!(*seq % 30)) lf_copy_all();   /* a crash writes no last copy */
    EnterCriticalSection(&cs);
    if (G.myid) for (i = 0; i < MAXPL; i++) if (G.pl[i].used && G.pl[i].id != G.myid) {
        DgPing p;
        Out *w = G.host ? (G.pl[i].conn >= 0 && G.conns[G.pl[i].conn].used ? G.conns[G.pl[i].conn].o : NULL) : G.jout;
        if (!w) continue;
        memset(&p, 0, sizeof p);
        p.from = G.myid; p.to = G.pl[i].id; p.seq = *seq; p.us = dg_us();
        row[nr].id = p.to;
        EnterCriticalSection(&w->oc); row[nr].out = w->bytes; LeaveCriticalSection(&w->oc);
        nr++;
        if (G.host) osend(w, T_PING, &p, sizeof p); else jsend(T_PING, &p, sizeof p);
        EnterCriticalSection(&dgcs);
        { DgPeer *r = dg_peer(p.to); if (r) r->sent++; }
        {
            DgRoster *ro = dg_roster(p.to);
            DWORD ms;
            if (ro) {
                lstrcpynA(ro->name, G.pl[i].name, sizeof ro->name);
                if (!ro->first_ms && clk_now_ms(&ms)) ro->first_ms = ms ? ms : 1;
            }
        }
        LeaveCriticalSection(&dgcs);
    }
    qd = (DWORD)G.qcount;
    LeaveCriticalSection(&cs);
    EnterCriticalSection(&dgcs);
    if (D.have_best) {   /* joiner: this second's fastest host answer sets the session clock (logged below, dgcs is a leaf) */
        clk_was = clk_read(&g_clk_off);
        clk_had = g_clk_ok;
        clk_new = D.best_off; clk_rtt = D.best_rtt; clk_set = 1;
        InterlockedExchange64((LONGLONG *)&g_clk_off, D.best_off);
        InterlockedExchange(&g_clk_ok, 1);
        D.have_best = 0;
    }
    memcpy(snap, D.peer, sizeof snap);
    for (i = 0; i < MAXPL; i++) {   /* the id and the last heartbeat carry over */
        DgPeer *r = &D.peer[i];
        r->sent = r->n = r->sum_us = r->min_us = r->max_us = r->rx = r->hb = r->gap_us = 0;
    }
    sim = D.sim; ticks = D.ticks; pump_n = D.pump_n; pump_sum = D.pump_sum_us; pump_max = D.pump_max_us;
    held = D.held_us; memcpy(lord, D.held_lord, sizeof lord); lo = D.lead_lo; hi = D.lead_hi; have = D.have_lead;
    q_n = D.q_n; q_sum = D.q_sum_us; q_max = D.q_max_us; tx = D.tx; tx_new = D.tx_new;
    D.ticks = D.pump_n = D.pump_sum_us = D.pump_max_us = D.held_us = D.q_n = D.q_sum_us = D.q_max_us = D.tx = D.tx_new = 0;
    memset(D.held_lord, 0, sizeof D.held_lord);
    D.have_lead = 0;
    LeaveCriticalSection(&dgcs);
    if (clk_set && !clk_had) logf_("session clock: set from the host's answer (round trip %lu.%lu ms)", DG_MS(clk_rtt));
    else if (clk_set && (clk_new - clk_was > 50000 || clk_was - clk_new > 50000))
        logf_("session clock: moved %ld ms (round trip %lu.%lu ms)", (long)((clk_new - clk_was) / 1000), DG_MS(clk_rtt));
    if (!nr) return;   /* nobody to measure yet: an empty lobby writes nothing */
    if (g_exe_ok) {
        ov = RD32(0x57c8fc);   /* one stale read here is harmless: a label on a line */
        for (i = 1; i < 7; i++) if (RD8(VA_NOBLE + i * 0x160 + 5) && RD8(VA_NOBLE + i * 0x160 + 4) && o < (int)sizeof slots - 16)
            o += sprintf(slots + o, "%s%d:%ld", o ? "," : "", i, (long)RD32(VA_HBTAB + i * 4));
    }
    for (i = 1; i < 7; i++) if (lord[i] > lord[top]) top = i;
    logf_("DIAG sim=%lu ticks=%lu ov=%lu pump n=%lu avg=%lu.%lu max=%lu.%lu lead=%ld..%ld held=%lu%s%d q n=%lu avg=%lu.%lu max=%lu.%lu depth=%lu hb sent=%lu new=%lu slots=%s",
          sim, ticks, ov, pump_n, DG_MS(pump_n ? pump_sum / pump_n : 0), DG_MS(pump_max), have ? lo : 0, have ? hi : 0, held / 1000u, " by lord ", top,
          q_n, DG_MS(q_n ? q_sum / q_n : 0), DG_MS(q_max), qd, tx, tx_new, slots);
    for (i = 0; i < nr; i++) {
        int k;
        for (k = 0; k < MAXPL; k++) if (snap[k].id == row[i].id) {
            DgPeer *r = &snap[k];
            logf_("DIAG PEER %08lx rtt=%lu.%lu min=%lu.%lu max=%lu.%lu n=%lu sent=%lu rx=%lu hb=%lu gap=%lu.%lu out=%lu",
                  (unsigned long)r->id, DG_MS(r->n ? r->sum_us / r->n : 0), DG_MS(r->min_us), DG_MS(r->max_us), r->n, r->sent, r->rx, r->hb, DG_MS(r->gap_us), row[i].out);
            break;
        }
    }
}

static DWORD WINAPI dg_thread(LPVOID a)
{
    int gen = (int)(INT_PTR)a;
    DWORD seq = 0, next = GetTickCount() + 1000;
    for (;;) {
        int alive;
        Sleep(50);   /* short: do_close waits for this thread */
        EnterCriticalSection(&cs);
        alive = G.gen == gen && G.open && !G.ghost;
        LeaveCriticalSection(&cs);
        if (!alive) break;
        if ((long)(GetTickCount() - next) < 0) continue;
        next = GetTickCount() + 1000;
        dg_second(&seq);
    }
    InterlockedDecrement(&g_live);
    return 0;
}

/* dp2_Open, session up, cs not held. Without the known exe (the selftest) only LOTR2NET_TEST_DIAG=1 starts it, so its
 * raw joiners that count frames see no pings */
static void dg_start(int gen)
{
    HANDLE th;
    char v[4] = "";
    InterlockedExchange(&g_diag, 0);
    if (!dg_cfg("LOTR2NET_DIAG", "diag", 1)) return;
    if (!g_exe_ok && !(GetEnvironmentVariableA("LOTR2NET_TEST_DIAG", v, sizeof v) && v[0] == '1')) return;
    EnterCriticalSection(&dgcs);
    memset(&D, 0, sizeof D);
    LeaveCriticalSection(&dgcs);
    InterlockedExchange(&g_diag, 1);
    InterlockedIncrement(&g_live);
    th = CreateThread(NULL, 0, dg_thread, (LPVOID)(INT_PTR)gen, 0, NULL);
    if (th) CloseHandle(th);
    else { InterlockedDecrement(&g_live); logf_("diag: CreateThread failed (%lu)", GetLastError()); }
}
