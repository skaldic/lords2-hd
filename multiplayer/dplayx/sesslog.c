/* sesslog.c - one folder per multiplayer game: lords2hd-mp\<YYYY-MM-DD_HH-MM-SS>\ beside the exe, shared with the HD
 * ddraw. Included by dplayx.c. Our own code. ASCII only.
 *
 * Starts when a second player is there (host: a joiner made its player; joiner: Open succeeded), or when the HD ddraw
 * asks for it at the start of a multiplayer game (LF_NetLogSession: a host playing only computer lords), so single
 * player and an empty lobby leave no folders. dplayx.log moves in and goes on there. The HD ddraw writes session.txt
 * into it at each game's start; at Close this file appends the session's summary to it (lf_summary: how it ended,
 * the players and their round trips, sync checks, desyncs and resyncs). Copied in, only if this run touched them, so an old
 * file never passes for this game's: status.txt (the game) and lords2hd.log (HD). Copied at Close, at process exit and
 * every 30 s while diag runs, so a crash still leaves recent ones. The LF_KEEP (10) newest folders are kept.
 * [Multiplayer] MpLogs=0 in lords2hd.ini (or env LOTR2NET_MP_LOGS=0) keeps dplayx.log beside the exe and makes no
 * folders. Without the known exe (selftest) only LOTR2NET_TEST_MPLOGS=1 turns it on. If the folder cannot be made, the
 * log simply stays where it is. */
#define LF_ROOT "lords2hd-mp"
#define LF_PATH (MAX_PATH + 64)   /* exedir + lords2hd-mp\<time>_<n>\, with room to spare: nothing here is cut short */
static char g_lf_dir[LF_PATH];    /* the last session's folder with its trailing \, "" = none yet */
static volatile LONG g_lf_open;   /* a session is writing to it */
static int g_lf_moved;            /* this run's dplayx.log has been moved out of the exe folder */
static FILETIME lf_start;
static CRITICAL_SECTION lfcs;    /* lf_begin, which the reader (cs held) and the HD ddraw's thread may both call. Order: cs -> lfcs -> logwr */

static void lf_init(void) { GetSystemTimeAsFileTime(&lf_start); InitializeCriticalSection(&lfcs); }   /* DllMain */

static int lf_enabled(void)
{
    char v[4] = "";
    if (!dg_cfg("LOTR2NET_MP_LOGS", "MpLogs", 1)) return 0;
    return g_exe_ok || (GetEnvironmentVariableA("LOTR2NET_TEST_MPLOGS", v, sizeof v) && v[0] == '1');
}

/* lords2hd-mp\ keeps the LF_KEEP newest session folders, the one about to be made included. Only folders named by a time
 * (YYYY-MM-DD_...; the HD ddraw's own before v12 too) and only the files in them go. The names sort by time */
#define LF_KEEP 10
static int lf_is_session(const char *name)
{
    int i;
    for (i = 0; i < 10; i++) if (i == 4 || i == 7 ? name[i] != '-' : name[i] < '0' || name[i] > '9') return 0;
    return name[10] == '_';
}

static void lf_prune(const char *root)
{
    char pat[LF_PATH + 8], names[64][48], file[LF_PATH + 128];
    int n = 0, i, old;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", root);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && lf_is_session(fd.cFileName) && lstrlenA(fd.cFileName) < (int)sizeof names[0] && n < 64)
            lstrcpyA(names[n++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    while (n > LF_KEEP - 1) {
        for (old = 0, i = 1; i < n; i++) if (strcmp(names[i], names[old]) < 0) old = i;
        snprintf(pat, sizeof pat, "%s\\%s\\*", root, names[old]);
        if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
            do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(file, sizeof file, "%s\\%s\\%s", root, names[old], fd.cFileName);
                DeleteFileA(file);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        snprintf(file, sizeof file, "%s\\%s", root, names[old]);
        RemoveDirectoryA(file);
        lstrcpyA(names[old], names[--n]);
    }
}

/* the next free lords2hd-mp\<time>[_n]\ made on disk; 0 = none, or the game's path leaves no room for it */
static int lf_make_dir(char *dir, int cap)
{
    SYSTEMTIME st;
    int k;
    if (lstrlenA(exedir) > MAX_PATH - 64) return 0;
    snprintf(dir, cap, "%s" LF_ROOT, exedir);
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    lf_prune(dir);
    GetLocalTime(&st);
    for (k = 0; k < 100; k++) {
        char n[16] = "";
        if (k) snprintf(n, sizeof n, "_%d", k);
        snprintf(dir, cap, "%s" LF_ROOT "\\%04u-%02u-%02u_%02u-%02u-%02u%s", exedir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, n);
        if (CreateDirectoryA(dir, NULL)) { lstrcatA(dir, "\\"); return 1; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    return 0;
}

/* Any thread, cs may be held (logwr is taken under cs on a full log buffer too). No-op while a session has its folder */
static void lf_begin(void)
{
    char dir[LF_PATH], from[MAX_PATH + 16], to[LF_PATH + 16];
    if (g_lf_open || !lf_enabled()) return;
    EnterCriticalSection(&lfcs);
    if (g_lf_open) { LeaveCriticalSection(&lfcs); return; }   /* another thread made it meanwhile */
    if (!lf_make_dir(dir, sizeof dir)) {
        LeaveCriticalSection(&lfcs);
        logf_("session logs: no folder (%lu) - logs stay beside the exe", GetLastError());
        return;
    }
    snprintf(from, sizeof from, "%sdplayx.log", exedir);
    snprintf(to, sizeof to, "%sdplayx.log", dir);
    EnterCriticalSection(&logwr);
    log_drain();
    if (hLog != INVALID_HANDLE_VALUE) { CloseHandle(hLog); hLog = INVALID_HANDLE_VALUE; }
    if (!g_lf_moved) {
        if (MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED)) g_lf_moved = 1;
        else if (CopyFileA(from, to, FALSE)) { DeleteFileA(from); g_lf_moved = 1; }   /* something still has it open */
    } else {   /* another session of this run: its own file, its own 8 MB */
        EnterCriticalSection(&logcs);
        log_bytes = 0; log_capped = 0; g_msg_stopped = 0;
        LeaveCriticalSection(&logcs);
    }
    hLog = CreateFileA(to, FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog == INVALID_HANDLE_VALUE) {   /* back beside the exe rather than no log at all */
        hLog = CreateFileA(from, FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        g_lf_dir[0] = 0;   /* an earlier session's folder no longer holds this log */
    } else { lstrcpynA(g_lf_dir, dir, sizeof g_lf_dir); InterlockedExchange(&g_lf_open, 1); }
    /* GENERIC_WRITE beats FILE_APPEND_DATA: the handle starts at offset 0 and would overwrite the moved lines */
    if (hLog != INVALID_HANDLE_VALUE) SetFilePointer(hLog, 0, NULL, FILE_END);
    LeaveCriticalSection(&logwr);
    LeaveCriticalSection(&lfcs);
    if (g_lf_open) logf_("session logs: folder %s (%s)", g_lf_dir, GetModuleHandleA("GameRanger.dll") ? "GameRanger" : "direct connection");
    else logf_("session logs: folder not opened, logging beside the exe");
}

static void lf_copy1(const char *rel, const char *name)
{
    char src[MAX_PATH + 96], dst[LF_PATH + 96];
    WIN32_FILE_ATTRIBUTE_DATA a;
    snprintf(src, sizeof src, "%s%s", exedir, rel);
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &a) || CompareFileTime(&a.ftLastWriteTime, &lf_start) < 0) return;
    snprintf(dst, sizeof dst, "%s%s", g_lf_dir, name);
    CopyFileA(src, dst, FALSE);
}

/* Takes no lock, so DLL_PROCESS_DETACH may call it. g_lf_dir only changes in lf_begin, which the callers never race:
 * the diag thread stops before do_close returns, and a new session starts after that */
static void lf_copy_all(void)
{
    if (!g_lf_dir[0]) return;
    lf_copy1("status.txt", "status.txt");
    lf_copy1("lords2hd.log", "lords2hd.log");
}

/* Export for the HD ddraw (lords2hd.c mp_session_start, at the start of a multiplayer game): this session's folder with
 * its trailing \, made now if the session has none yet (a host with no joiner). Returns the length; 0 = no folder
 * (MpLogs=0, no session open, the folder could not be made, or buf too small) */
__declspec(dllexport) int WINAPI LF_NetLogSession(char *buf, int cap)
{
    int n = 0;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    EnterCriticalSection(&cs);
    if (G.open) lf_begin();
    LeaveCriticalSection(&cs);
    EnterCriticalSection(&logwr);   /* lf_begin changes g_lf_dir under it */
    if (g_lf_open && lstrlenA(g_lf_dir) < cap) { lstrcpyA(buf, g_lf_dir); n = lstrlenA(buf); }
    LeaveCriticalSection(&logwr);
    return n;
}

/* ---- v13 session summary, appended to session.txt. lf_snapshot copies the numbers under cs in do_close (sy_reset clears
 * them right after); lf_summary writes them with no lock held */
typedef struct {
    int have, host, clk;
    DPID me;
    DWORD end_ms, oks, desyncs, resyncs, pump_max_us;
    ULONGLONG held_us;
    char first[200], why[120];
    DgRoster ro[MAXPL];
} LfSum;
static LfSum g_lf_sum;

static void lf_snapshot(const char *why)   /* cs held */
{
    LfSum *m = &g_lf_sum;
    int i, k;
    if (!g_lf_open) return;
    memset(m, 0, sizeof *m);
    m->have = 1;
    m->host = G.host;
    m->me = G.myid;
    m->clk = clk_now_ms(&m->end_ms);
    m->oks = (DWORD)sy_oks; m->desyncs = (DWORD)sy_desyncs; m->resyncs = sy_epoch;
    lstrcpynA(m->first, sy_first, sizeof m->first);
    lstrcpynA(m->why, why, sizeof m->why);
    EnterCriticalSection(&dgcs);
    memcpy(m->ro, DS.ro, sizeof m->ro);
    m->held_us = DS.held_all_us; m->pump_max_us = DS.pump_max_all_us;
    LeaveCriticalSection(&dgcs);
    for (i = 0; i < MAXPL; i++) if (G.pl[i].used && G.pl[i].id != G.myid) {   /* still here but never measured (diag off) */
        k = 0;
        while (k < MAXPL && m->ro[k].id != G.pl[i].id) k++;
        if (k < MAXPL) continue;
        k = 0;
        while (k < MAXPL && m->ro[k].id) k++;
        if (k < MAXPL) { m->ro[k].id = G.pl[i].id; lstrcpynA(m->ro[k].name, G.pl[i].name, sizeof m->ro[k].name); }
    }
}

static void lf_summary(void)   /* no lock held */
{
    LfSum *m = &g_lf_sum;
    char p[LF_PATH + 16];
    FILE *f;
    int i;
    if (!m->have || !g_lf_dir[0]) return;
    m->have = 0;
    snprintf(p, sizeof p, "%ssession.txt", g_lf_dir);
    if (!(f = fopen(p, "a"))) return;
    fprintf(f, "---- dplayx.dll v%d session summary (this PC: %s, %s)\n", DLL_VERSION, m->host ? "host" : "joiner",
            GetModuleHandleA("GameRanger.dll") ? "GameRanger" : "direct connection");
    if (m->clk) fprintf(f, "ended: S %lu.%03lu (%lu min %lu s into the session): %s\n", m->end_ms / 1000, m->end_ms % 1000,
                        m->end_ms / 60000, m->end_ms / 1000 % 60, m->why);
    else fprintf(f, "ended (no session clock: the host never answered): %s\n", m->why);
    fprintf(f, "players (round trip min / avg / max over the session):\n");
    for (i = 0; i < MAXPL; i++) if (m->ro[i].id) {
        DgRoster *r = &m->ro[i];
        fprintf(f, "  %08lx '%s'%s", (unsigned long)r->id, r->name, r->id == m->me ? " (this PC)" : "");
        if (r->n) {
            DWORD avg = (DWORD)(r->sum_us / r->n);
            fprintf(f, "  %lu.%lu / %lu.%lu / %lu.%lu ms, %lu answers", DG_MS(r->min_us), DG_MS(avg), DG_MS(r->max_us), r->n);
        } else if (r->id != m->me) fprintf(f, "  no round trips measured");
        if (r->first_ms && r->id != m->me) fprintf(f, ", joined S %lu.%03lu", r->first_ms / 1000, r->first_ms % 1000);
        if (r->n && r->last_ms) fprintf(f, ", last answer S %lu.%03lu", r->last_ms / 1000, r->last_ms % 1000);
        fputc('\n', f);
    }
    fprintf(f, "sync checks: %lu ok, %lu desync(s), %lu resync(s)\n", m->oks, m->desyncs, m->resyncs);
    if (m->first[0]) fprintf(f, "first desync: %s (the DESYNC lines in dplayx.log have every one)\n", m->first);
    if (m->pump_max_us)   /* measured by diag (dg_on_pump); without it there is nothing to say */
        fprintf(f, "held at the pace limit: %lu.%lu s in all; longest game-loop gap %lu.%lu ms\n",
                (unsigned long)(m->held_us / 1000000u), (unsigned long)(m->held_us / 100000u % 10u), DG_MS(m->pump_max_us));
    fputc('\n', f);
    fclose(f);
}

/* do_close, its threads gone: the next session gets a folder of its own; lines until then stay in this one */
static void lf_end(void)
{
    if (!g_lf_open) return;
    lf_summary();
    lf_copy_all();
    InterlockedExchange(&g_lf_open, 0);
}
