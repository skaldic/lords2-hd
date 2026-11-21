/* tests.c - the test DLL: the patch's own source and the tests, in one file so the tests can call its static
 * functions. Our own code. Built and run by tests\run.ps1 (host.c loads it).
 *
 * The patch's DllMain (its start-up: settings, hooks into the game) is renamed away and never runs: a test sets
 * up only what it needs. The shipped DLL is built from hd\lords2hd.c alone and is not changed by any of this. */
#define DllMain l2hd_DllMain_off
/* The patch finds the game's exe (its PE headers, the import table) with GetModuleHandleA(NULL). In the tests that
 * is the host; while a game exe is copied into the fake memory (t_hooks.c) it answers that copy instead. */
#define COBJMACROS                  /* (as the patch's source sets it before its own windows.h) */
#include <windows.h>
static HMODULE t_gameModule;
static HMODULE WINAPI t_GetModuleHandleA(LPCSTR name) { return !name && t_gameModule ? t_gameModule : GetModuleHandleA(name); }
#define GetModuleHandleA t_GetModuleHandleA
#include "../hd/lords2hd.c"
#include "out/tramps.h"
#include "t.h"
#include "game_mem.h"
#include <shellapi.h>

static BYTE *t_gameLo, *t_gameHi;   // the game's address range the host provides (fake game memory)
static char t_root[MAX_PATH];       // this run's temp folder
static char t_exeDir[MAX_PATH];     // where the test DLL is (and l2pak.exe)

/* ---- helpers for tests ---- */
static void t_path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s\\%s", g_gameDir, rel); }
static void t_mkdirs(const char *path)   // every folder of path (up to its last backslash)
{
	char p[MAX_PATH];
	lstrcpynA(p, path, sizeof p);
	for (char *c = p + 3; *c; c++) if (*c == '\\') { *c = 0; CreateDirectoryA(p, NULL); *c = '\\'; }
}
static BOOL t_write(const char *rel, const void *data, size_t n)   // a file under the test's game folder
{
	char p[MAX_PATH];
	t_path(p, sizeof p, rel);
	t_mkdirs(p);
	FILE *f = fopen(p, "wb");
	if (!f) return FALSE;
	BOOL ok = fwrite(data, 1, n, f) == n;
	fclose(f);
	return ok;
}
static unsigned t_rand_state = 12345;
static unsigned t_rand(void) { t_rand_state = t_rand_state * 1103515245u + 12345u; return t_rand_state >> 8; }
static void t_rmtree(const char *dir)
{
	char from[MAX_PATH + 2] = { 0 };
	lstrcpynA(from, dir, MAX_PATH);
	SHFILEOPSTRUCTA op = { 0 };
	op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
	SHFileOperationA(&op);
}
/* Runs a program, returns its exit code (-1 if it couldn't start). */
static int t_exec(const char *cmd)
{
	char line[1024];
	lstrcpynA(line, cmd, sizeof line);
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
	WaitForSingleObject(pi.hProcess, 60000);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	return (int)code;
}

/* Before each test: fake game memory cleared, a fresh game folder, the patch's file state forgotten. */
static void t_reset(void)
{
	memset(t_gameLo, 0, t_gameHi - t_gameLo);
	snprintf(g_gameDir, sizeof g_gameDir, "%s\\%s", t_root, t_current);
	CreateDirectoryA(g_gameDir, NULL);
	snprintf(g_iniPath, sizeof g_iniPath, "%s\\lords2hd.ini", g_gameDir);
	free(g_pak); g_pak = NULL; g_pakN = 0; g_pakPath[0] = 0;
	hv_free_file(); g_hv.smk = NULL; g_hv.dest = NULL; InterlockedExchange(&g_hv.on, 0);
	g_hdVideos = TRUE;
	t_rand_state = 12345;
	t_battle_reset();
}

#include "t_core.c"
#include "t_battle.c"
#include "t_ai.c"
#include "t_campaign.c"
#include "t_settings.c"
#include "t_input.c"
#include "t_hooks.c"

__declspec(dllexport) int run_tests(const char *filter, void *gameLo, void *gameHi)
{
	t_gameLo = gameLo; t_gameHi = gameHi;
	HMODULE me = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)run_tests, &me);
	GetModuleFileNameA(me, t_exeDir, sizeof t_exeDir);
	char *slash = strrchr(t_exeDir, '\\');
	if (slash) *slash = 0;
	char tmp[MAX_PATH];
	GetTempPathA(sizeof tmp, tmp);
	snprintf(t_root, sizeof t_root, "%sl2hd-tests-%lu", tmp, GetCurrentProcessId());
	CreateDirectoryA(t_root, NULL);
	snprintf(g_logPath, sizeof g_logPath, "%s\\lords2hd.log", t_root);   // the patch's log lines, for a failing test
	InitializeCriticalSection(&g_lock);
	InitializeCriticalSection(&g_composeLock);
	InitializeCriticalSection(&g_recentLock);
	o_mmioOpenA = mmioOpenA;   // (the patch hooks it in the game; here the plain one)
	int failed = t_run(filter);
	if (failed) printf("the patch's log of this run: %s\n", g_logPath);
	else { Sleep(200); t_rmtree(t_root); }   // (let the video read-ahead threads close their files)
	return failed;
}
