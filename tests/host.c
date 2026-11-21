/* host.c - runs the patch's tests. Our own code.
 *
 * The patch reads the game's data at fixed addresses (the battle's unit table at 0x00554480, the battlefield at
 * 0x005440e0, ...). This exe is linked at the game's own image base and its zero-filled array below covers those
 * addresses, so the test DLL (the patch's source with the tests, tests.c) can build a made-up game state there
 * and call the patch's functions on it. A test exe that only moved its image elsewhere can't: Windows puts heaps
 * and stacks in that range before main runs.
 *
 *   l2hd_tests.exe [name filter]      exit code: the number of failed tests (0 = all passed) */
#include <windows.h>
#include <stdio.h>

#define GAME_LO 0x00410000   // what the tests may touch: the game's code addresses (hook sites) and data
#define GAME_HI 0x00600000
__attribute__((used)) volatile BYTE g_gameSpace[0x00200000];  // .bss right after this exe's few pages of code

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if ((DWORD)g_gameSpace > GAME_LO || (DWORD)(g_gameSpace + sizeof g_gameSpace) < GAME_HI) {
		printf("host: the game's address range is not covered (array at %p)\n", (void *)g_gameSpace);
		return 100;
	}
	char dll[MAX_PATH];
	GetModuleFileNameA(NULL, dll, sizeof dll);
	char *slash = strrchr(dll, '\\');
	lstrcpyA(slash ? slash + 1 : dll, "l2hd_tests.dll");
	HMODULE m = LoadLibraryA(dll);
	if (!m) { printf("host: can't load %s (error %lu)\n", dll, GetLastError()); return 101; }
	int (*run)(const char *, void *, void *) = (void *)GetProcAddress(m, "run_tests");
	if (!run) { printf("host: no run_tests in %s\n", dll); return 102; }
	return run(argc > 1 ? argv[1] : NULL, (void *)GAME_LO, (void *)GAME_HI);
}
