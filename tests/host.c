/* host.c - runs the patch's tests. Our own code.
 *
 * The patch reads the game's data at fixed addresses (the battle's unit table at 0x00554480, the battlefield at
 * 0x005440e0, ...) and patches its code at fixed addresses. This exe is linked just below the game's image base
 * (0x00300000) and its zero-filled array covers the game's whole image, 0x00400000-0x005e9000, so the test DLL (the
 * patch's source with the tests, tests.c) can build a made-up game state there, or copy a real Lords2.exe's
 * sections in (t_hooks.c), and call the patch's functions on it. A test exe that only moved its image elsewhere
 * can't: Windows puts heaps and stacks in that range before main runs.
 *
 *   l2hd_tests.exe [name filter]      exit code: the number of failed tests (0 = all passed) */
#include <windows.h>
#include <stdio.h>

#define GAME_LO 0x00410000   // what each test starts with cleared: the game's code addresses (hook sites) and data
#define GAME_HI 0x00600000
#define IMAGE_LO 0x00400000  // the game's whole image (a real exe copied in)
#define IMAGE_HI 0x005e9000
__attribute__((used)) volatile BYTE g_gameSpace[0x00320000];  // .bss right after this exe's few pages of code

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if ((DWORD)g_gameSpace > IMAGE_LO || (DWORD)(g_gameSpace + sizeof g_gameSpace) < GAME_HI) {
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
