/* sesslog.c - one folder per multiplayer session: mp_logs\<direct|gameranger>_<YYYY-MM-DD_HH-MM-SS>\ beside the exe.
 * Included by dplayx.c. Our own code. ASCII only.
 *
 * Starts when a second player is there (host: a joiner made its player; joiner: Open succeeded), so single player and an
 * empty lobby leave no folders. dplayx.log moves in and goes on there; desync.txt is written there (lf_dir). What others
 * write is copied in, only if this run touched it, so an old file never passes for this game's: status.txt (the game),
 * lotr2fix-ddraw.log and the Mod Menu's two logs for this pid (ddraw). Copied at Close, at process exit and every 30 s
 * while diag runs, so a crash still leaves recent ones. The LF_KEEP (10) newest folders are kept. The HD ddraw finds the
 * current one through the export LF_NetLogDir and copies dplayx.log and desync.txt into its own lords2hd-mp\ folder.
 * gameranger = GameRanger.dll is loaded in this process. mp_logs=0 in lords2hd.ini (or env LOTR2NET_MP_LOGS=0) keeps
 * everything beside the exe. Without the known exe (selftest) only LOTR2NET_TEST_MPLOGS=1 turns it on. If the folder cannot
 * be made, the log simply stays where it is. */
#define LF_PATH (MAX_PATH + 64)   /* exedir + mp_logs\<prefix>_<time>_<n>\, with room to spare: nothing here is cut short */
static char g_lf_dir[LF_PATH];    /* the last session's folder with its trailing \, "" = none yet */
static volatile LONG g_lf_open;   /* a session is writing to it */
static int g_lf_moved;            /* this run's dplayx.log has been moved out of the exe folder */
static FILETIME lf_start;

static void lf_init(void) { GetSystemTimeAsFileTime(&lf_start); }   /* DllMain */

static const char *lf_dir(void) { return g_lf_open ? g_lf_dir : exedir; }

static int lf_enabled(void)
{
    char v[4] = "";
    if (!dg_cfg("LOTR2NET_MP_LOGS", "mp_logs", 1)) return 0;
    return g_exe_ok || (GetEnvironmentVariableA("LOTR2NET_TEST_MPLOGS", v, sizeof v) && v[0] == '1');
}

/* mp_logs\ keeps the LF_KEEP newest session folders, the one about to be made included (as the HD ddraw's lords2hd-mp\).
 * Only folders of ours (direct_* / gameranger_*) and only the files in them go. Newest = the latest time in the name,
 * after the prefix, so a folder copied back in keeps its place */
#define LF_KEEP 10
static const char *lf_when(const char *name)
{
    if (!strncmp(name, "direct_", 7)) return name + 7;
    if (!strncmp(name, "gameranger_", 11)) return name + 11;
    return NULL;
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
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && lf_when(fd.cFileName) && lstrlenA(fd.cFileName) < (int)sizeof names[0] && n < 64)
            lstrcpyA(names[n++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    while (n > LF_KEEP - 1) {
        for (old = 0, i = 1; i < n; i++) if (strcmp(lf_when(names[i]), lf_when(names[old])) < 0) old = i;
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

/* the next free mp_logs\<prefix>_<time>[_n]\ made on disk; 0 = none, or the game's path leaves no room for it */
static int lf_make_dir(char *dir, int cap)
{
    SYSTEMTIME st;
    int k;
    const char *prefix = GetModuleHandleA("GameRanger.dll") ? "gameranger" : "direct";
    if (lstrlenA(exedir) > MAX_PATH - 64) return 0;
    snprintf(dir, cap, "%smp_logs", exedir);
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    lf_prune(dir);
    GetLocalTime(&st);
    for (k = 0; k < 100; k++) {
        char n[16] = "";
        if (k) snprintf(n, sizeof n, "_%d", k);
        snprintf(dir, cap, "%smp_logs\\%s_%04u-%02u-%02u_%02u-%02u-%02u%s", exedir, prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, n);
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
    if (!lf_make_dir(dir, sizeof dir)) { logf_("mp_logs: no session folder (%lu) - logs stay beside the exe", GetLastError()); return; }
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
        g_lf_dir[0] = 0;   /* an earlier session's folder no longer holds this log (LF_NetLogDir) */
    } else { lstrcpynA(g_lf_dir, dir, sizeof g_lf_dir); InterlockedExchange(&g_lf_open, 1); }
    /* GENERIC_WRITE beats FILE_APPEND_DATA: the handle starts at offset 0 and would overwrite the moved lines */
    if (hLog != INVALID_HANDLE_VALUE) SetFilePointer(hLog, 0, NULL, FILE_END);
    LeaveCriticalSection(&logwr);
    logf_("mp_logs: session folder %s", g_lf_open ? g_lf_dir : "not opened, logging beside the exe");
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
    char rel[96], name[64];
    int k;
    static const char *const mm[] = {"Lords2MultiplayerPeerVersion", "Lords2MultiplayerRuleset"};
    if (!g_lf_dir[0]) return;
    lf_copy1("status.txt", "status.txt");
    lf_copy1("lotr2fix-ddraw.log", "lotr2fix-ddraw.log");
    for (k = 0; k < 2; k++) {
        snprintf(name, sizeof name, "%s_%lu.txt", mm[k], GetCurrentProcessId());
        snprintf(rel, sizeof rel, "Lords2ModMenu\\logs\\%s", name);
        lf_copy1(rel, name);
    }
}

/* Export for the HD ddraw (lords2hd.c mp_copy_logs, which copies dplayx.log and desync.txt into its lords2hd-mp\ folder):
 * the folder dplayx.log is in, with its trailing \. After a Close it stays that session's folder, where the log goes on.
 * Returns the length; 0 = buf too small */
__declspec(dllexport) int WINAPI LF_NetLogDir(char *buf, int cap)
{
    const char *d;
    int n = 0;
    if (!buf || cap <= 0) return 0;
    EnterCriticalSection(&logwr);   /* lf_begin changes g_lf_dir under it */
    d = g_lf_dir[0] ? g_lf_dir : exedir;
    if (lstrlenA(d) < cap) { lstrcpyA(buf, d); n = lstrlenA(d); } else buf[0] = 0;
    LeaveCriticalSection(&logwr);
    return n;
}

/* do_close, its threads gone: the next session gets a folder of its own; lines until then stay in this one */
static void lf_end(void)
{
    if (!g_lf_open) return;
    lf_copy_all();
    InterlockedExchange(&g_lf_open, 0);
}
