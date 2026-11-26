/* exepatch.c - in-memory fixes to Lords2.exe (5 Jun 1997), applied from DllMain before the game runs.
 * Refused unless the running exe looks like the known build: PE TimeDateStamp, SizeOfImage and image base are
 * checked, then the ORIGINAL BYTES at every patch site (each patch is skipped/refused if any site differs).
 * This is NOT a full-file hash: the SHA-256 gate of the known Lords2.exe lives in the installer.
 * Nothing on disk is changed: quit the game and it is stock again.
 *   ui    : drop the dead "Sierra Internet Gaming System" entry the game adds to "Connection method"
 *           and pre-selects (OK on it went to a service shut down decades ago). Always on.
 *   lobby : a joiner leaving a 2-player lobby no longer closes the host's session. Always on; local to
 *           the host, so it is not part of the handshake version.
 *   continue: the host's Continue works at once instead of 6 s after Create. Always on, host-local.
 *   pace  : multiplayer steps at the speed dial instead of a permanent 2x catch-up, dial 100 keeps its 2 ms gate, level battle peers step once. Always on.
 *   mpfix : the multiplayer desync/late-command patches from ../mp-fix-manifest.md, all-or-nothing.
 *           ON by default since v5 (2026-10-06 rig A/B, ../mp-fix-manifest.md "Rig results"); lords2hd.ini "mpfix=0" or
 *           env LOTR2NET_MPFIX=0 turns it off (env wins). Includes the v11 rally-clear hook on setup_battle (patch_rally). Peers must match; the handshake
 *           version carries it, so a mismatched pair is refused with a clear log line instead of desyncing.
 * Our own code. ASCII only. */
#include "mpfix_patches.h"

#define EXE_TIMESTAMP 0x3396efaaUL
#define EXE_SIZEOFIMAGE 0x1e9000UL
#ifndef MPFIX_DEFAULT
#define MPFIX_DEFAULT 1   /* ON: human-vs-human battles completed under it, late commands 0..12 vs 42..203 OFF, no split (MULTIPLAYER.md mpv5 rows) */
#endif

int g_exe_ok, g_mpfix;
int g_rally_ok;   /* rally-clear hook on setup_battle is in (needs mpfix); sync.c then hashes the three rally dwords */
int g_pace_ok;   /* review #7: the pace patch went in; part of PROTO, so a peer at 2x catch-up cannot join a 1x peer */

static int exe_is_known(void)
{
    BYTE *b = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)b;
    if (!b || d->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(b + d->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == EXE_TIMESTAMP &&
           nt->OptionalHeader.SizeOfImage == EXE_SIZEOFIMAGE && (DWORD_PTR)b == 0x400000;
}

static int write_code(DWORD va, const void *p, DWORD n)
{
    DWORD old;
    if (!VirtualProtect((void *)va, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)va, p, n);
    VirtualProtect((void *)va, n, old, &old);
    return 1;
}

/* 0x4b7fd3: push "Sierra Internet Gaming System"; push 0; push LB_ADDSTRING; mov eax,[list]; push eax;
 * call [SendMessageA]; mov [sel],eax  ->  xor eax,eax; mov [sel],eax; jmp 0x4b8016 (skips its SETITEMDATA).
 * The selection becomes item 0 = the first connection DirectPlayEnumerateA reported = ours. */
static void patch_ui(void)
{
    static const BYTE orig[] = {0x68, 0x84, 0x2c, 0x4e, 0x00, 0x6a, 0x00, 0x68, 0x80, 0x01, 0x00, 0x00};
    static const BYTE repl[] = {0x31, 0xc0, 0xa3, 0x10, 0x5a, 0x4e, 0x00, 0xeb, 0x3a};
    if (memcmp((void *)0x4b7fd3, orig, sizeof orig) || *(BYTE *)0x4b8016 != 0xa1) { logf_("WARN ui patch: bytes differ - skipped"); return; }
    logf_("ui patch (no SIGS entry): %s", write_code(0x4b7fd3, repl, sizeof repl) ? "applied" : "FAILED");
}

/* Lobby leave (FUN_0043e6a2, DESTROYPLAYER before the game starts): stock closes the session when fewer than 2
 * players remain, so in a 2-player lobby one joiner pressing Cancel threw the HOST back to the main menu too
 * (two-PC test 2026-10-06). Hook the `jg 0x43e802` after `cmp [0x553f98],1` (players left) and send the host
 * down the "players remain" path (re-broadcast the player list, msg 3) whatever the count; a joiner keeps stock.
 * The host's Start already refuses with fewer than 2 players (FUN_004329ec), so it just waits for a new joiner.
 * Also clears the departed slot's ready flag (0x5530e0[slot]), which stock leaves set, so Start cannot count it.
 *   cave: jg clr | cmp [0x553248],0 | je 0x43e7b7 (stock shutdown) | clr: mov eax,[ebp-4]
 *         mov byte [eax+0x5530e0],0 | jmp 0x43e802 */
static void patch_lobby(void)
{
    static const BYTE orig[] = {0x83, 0x3d, 0x98, 0x3f, 0x55, 0x00, 0x01, 0x0f, 0x8f, 0x4b, 0x00, 0x00, 0x00};
    static const BYTE tail[] = {0x83, 0x3d, 0x48, 0x32, 0x55, 0x00, 0x00, 0x0f, 0x84};   /* 0x43e802 */
    static const BYTE code[] = {0x7f, 0x0d, 0x83, 0x3d, 0x48, 0x32, 0x55, 0x00, 0x00, 0x0f, 0x84, 0, 0, 0, 0,
                                0x8b, 0x45, 0xfc, 0xc6, 0x80, 0xe0, 0x30, 0x55, 0x00, 0x00, 0xe9, 0, 0, 0, 0};
    BYTE hook[6] = {0xe9, 0, 0, 0, 0, 0x90}, *cave;
    if (memcmp((void *)0x43e7aa, orig, sizeof orig) || memcmp((void *)0x43e802, tail, sizeof tail)) {
        logf_("WARN lobby patch: bytes differ - skipped"); return;
    }
    if (!(cave = (BYTE *)VirtualAlloc(NULL, sizeof code, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
        logf_("WARN lobby patch: no cave memory - skipped"); return;
    }
    memcpy(cave, code, sizeof code);
    *(int *)(cave + 11) = (int)(0x43e7b7 - ((DWORD)cave + 15));
    *(int *)(cave + 26) = (int)(0x43e802 - ((DWORD)cave + 30));
    *(int *)(hook + 1) = (int)((DWORD)cave - (0x43e7b1 + 5));
    logf_("lobby patch (host keeps the lobby when a joiner leaves): %s", write_code(0x43e7b1, hook, sizeof hook) ? "applied" : "FAILED");
}

/* Host Continue wait (FUN_004bbe51): stock enables the shield screen's Continue (0x57c940, tested by FUN_00433155)
 * only 6000 ms after Create (timer 0x55ce74 set in FUN_00497a34 / FUN_00432b05), so players clicked it 5+ times
 * (two-PC test 2026-10-06). Joiners may still join after the host moves on, so the wait buys nothing on our
 * transport: cmp eax,6000 -> cmp eax,0. */
static void patch_continue(void)
{
    static const BYTE orig[] = {0x2b, 0x05, 0x74, 0xce, 0x55, 0x00, 0x3d, 0x70, 0x17, 0x00, 0x00, 0x0f, 0x8d};
    static const BYTE repl[] = {0x3d, 0x00, 0x00, 0x00, 0x00};
    if (memcmp((void *)0x4bbe78, orig, sizeof orig)) { logf_("WARN continue patch: bytes differ - skipped"); return; }
    logf_("continue patch (host Continue without the 6 s wait): %s", write_code(0x4bbe7e, repl, sizeof repl) ? "applied" : "FAILED");
}

/* Multiplayer pace (NetPacing FUN_0043de70, main branch): a peer that is not ahead and less than 20 ticks
 * behind the leader got -2 = "catch up, 2 steps per frame". Our transport keeps peers within a few ticks, so
 * both sat in catch-up for good: 162 ticks/s at speed 9 (two-PC game 2026-10-06), twice single player's
 * 83/s, and armies marched far too fast. -2 -> 1 at 0x43dff7: level peers step once per frame under the
 * speed dial; 20+ behind still catches up at 2; ahead still throttles through the 0x4e3260 rows.
 * Dial 100 (fix34): the step pacer FUN_004bb978 skips its wall-clock gate when q == 0 AND net_session_active
 * ([0x553030]) - 0x4bbb8e: cmp [ebp-8],0 / jne gate / cmp [0x553030],0 / je gate / mov eax,[ebp-0xc] / jmp ret.
 * So MP at dial 100 stepped every frame (~2220/s) while SP waits the 2 ms gate (~500/s). jne 0x4bbbad at
 * 0x4bbb92 -> jmp 0x4bbbad: every mode takes the gate (10*q+2 ms); dials 0..90 never reached that branch.
 * Battle (fix34b): the free-run branch (FUN_0043e433 true = fewer than 2 live human combatants, e.g. human vs
 * AI) computes own - FUN_0043e50d(), the min tick over ALL live players INCLUDING self, so it is never < 0; at
 * 0 (we are the slowest or tied) it returns -1 = 2 steps per gate (caller 0x4bba2b), with no depth tiers. Peers
 * sit tied, so MP battles ran 2x the dial (166.5/s at dial 90). 0x43df47 `mov eax,-1; jmp ret` -> jmp to a cave
 * that takes the same min over the OTHER live players (FUN_0043e50d's filter minus self) and returns 1 when
 * min_others - own < 20 (level: one step, as SP and the 0x43dff7 fix), else -1 (genuinely behind: catch up at 2,
 * as stock). Every site is checked first and all are written or none, so g_pace_ok (PROTO) means "all". */
static const BYTE pace_cave[] = {
    0x8b, 0x3d, 0xcc, 0xc8, 0x57, 0x00,             /* mov edi,[0x57c8cc]           own slot */
    0xba, 0x80, 0x96, 0x98, 0x00,                   /* mov edx,10000000             min over the others */
    0xbe, 0x01, 0x00, 0x00, 0x00,                   /* mov esi,1 */
    0x39, 0xfe, 0x74, 0x31,                         /* loop: cmp esi,edi ; je next */
    0x69, 0xc6, 0x60, 0x01, 0x00, 0x00,             /* imul eax,esi,0x160 */
    0x80, 0xb8, 0x05, 0xbf, 0x57, 0x00, 0x00, 0x74, 0x22,   /* cmp byte [eax+0x57bf05],0 ; je next */
    0x80, 0xb8, 0x04, 0xbf, 0x57, 0x00, 0x00, 0x74, 0x19,   /* cmp byte [eax+0x57bf04],0 ; je next */
    0x6b, 0xc6, 0x2c,                               /* imul eax,esi,0x2c */
    0x80, 0xb8, 0x76, 0x3d, 0x55, 0x00, 0x00, 0x75, 0x0d,   /* cmp byte [eax+0x553d76],0 ; jne next */
    0x8b, 0x04, 0xb5, 0xd0, 0xe8, 0x53, 0x00,       /* mov eax,[esi*4+0x53e8d0] */
    0x39, 0xd0, 0x7d, 0x02, 0x89, 0xc2,             /* cmp eax,edx ; jge next ; mov edx,eax */
    0x46, 0x83, 0xfe, 0x07, 0x7c, 0xc5,             /* next: inc esi ; cmp esi,7 ; jl loop */
    0x2b, 0x14, 0xbd, 0xd0, 0xe8, 0x53, 0x00,       /* sub edx,[edi*4+0x53e8d0]     min_others - own */
    0x83, 0xfa, 0x14, 0x7d, 0x0a,                   /* cmp edx,20 ; jge behind */
    0xb8, 0x01, 0x00, 0x00, 0x00, 0xe9, 0, 0, 0, 0, /* mov eax,1 ; jmp 0x43e32b (rel32 at 93) */
    0xb8, 0xff, 0xff, 0xff, 0xff, 0xe9, 0, 0, 0, 0, /* behind: mov eax,-1 ; jmp 0x43e32b (rel32 at 103) */
};

typedef char pace_cave_is_107_bytes[(sizeof pace_cave == 107) ? 1 : -1];   /* rel32 fixups at 93 and 103 */

static void patch_pace(void)
{
    static const BYTE orig[] = {0x83, 0x7d, 0xf4, 0x14, 0x0f, 0x8d, 0x0a, 0x00, 0x00, 0x00, 0xb8, 0xfe, 0xff, 0xff, 0xff};
    static const BYTE repl[] = {0xb8, 0x01, 0x00, 0x00, 0x00};
    static const BYTE orig100[] = {0x83, 0x7d, 0xf8, 0x00, 0x0f, 0x85, 0x15, 0x00, 0x00, 0x00,   /* cmp [ebp-8],0; jne 0x4bbbad */
                                   0x83, 0x3d, 0x30, 0x30, 0x55, 0x00, 0x00,                     /* cmp [0x553030],0 */
                                   0x0f, 0x84, 0x08, 0x00, 0x00, 0x00};                          /* je 0x4bbbad */
    static const BYTE repl100[] = {0xe9, 0x16, 0x00, 0x00, 0x00, 0x90};                          /* jmp 0x4bbbad; nop */
    static const BYTE origbt[] = {0x83, 0x7d, 0xf4, 0x00, 0x0f, 0x8f, 0x0f, 0x00, 0x00, 0x00,    /* cmp [ebp-0xc],0; jg 0x43df56 */
                                  0xb8, 0xff, 0xff, 0xff, 0xff, 0xe9, 0xda, 0x03, 0x00, 0x00,    /* mov eax,-1; jmp 0x43e32b */
                                  0xe9, 0x07, 0x00, 0x00, 0x00};                                 /* (dead) jmp 0x43df5d */
    BYTE replbt[10] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};                          /* 0x43df47: jmp cave; nop x5 */
    DWORD old;
    g_pace_ok = 0;
    if (memcmp((void *)0x43dfed, orig, sizeof orig)) { logf_("WARN pace patch: bytes differ at 43dfed - skipped"); return; }
    if (memcmp((void *)0x4bbb8e, orig100, sizeof orig100)) { logf_("WARN pace patch: bytes differ at 4bbb8e - skipped"); return; }
    if (memcmp((void *)0x43df3d, origbt, sizeof origbt)) { logf_("WARN pace patch: bytes differ at 43df3d - skipped"); return; }
    BYTE *cave = (BYTE *)VirtualAlloc(NULL, sizeof pace_cave, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!cave) { logf_("WARN pace patch: no cave memory - skipped"); return; }
    memcpy(cave, pace_cave, sizeof pace_cave);
    *(int *)(cave + 93) = (int)(0x43e32b - ((DWORD)cave + 93 + 4));
    *(int *)(cave + 103) = (int)(0x43e32b - ((DWORD)cave + 103 + 4));
    if (!VirtualProtect(cave, sizeof pace_cave, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(cave, 0, MEM_RELEASE); logf_("WARN pace patch: cave not executable - skipped"); return;
    }
    *(int *)(replbt + 1) = (int)((DWORD)cave - (0x43df47 + 5));
    if (write_code(0x43dff7, repl, sizeof repl)) {
        if (write_code(0x4bbb92, repl100, sizeof repl100)) {
            if (write_code(0x43df47, replbt, sizeof replbt)) g_pace_ok = 1;
            else write_code(0x4bbb92, orig100 + 4, sizeof repl100);   /* undo: all or none */
        }
        if (!g_pace_ok) write_code(0x43dff7, orig + 10, sizeof repl);
    }
    if (!g_pace_ok) VirtualFree(cave, 0, MEM_RELEASE);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    logf_("pace patch (multiplayer runs at the speed dial, not 2x; dial 100 gated at 2 ms like SP; level battle peers step once, cave %p): %s",
          (void *)cave, g_pace_ok ? "applied" : "FAILED");
}

/* AI rally globals (v11, 2026-10-08). The rally flag 0x56d89c and the rally point 0x522f70 (x) / 0x522f74 (y) are written
 * by FUN_0048acd2 / FUN_0048b02b, cleared by FUN_0048b7fc and read by the AI class-1 missile DEFEND (FUN_0048a9d2).
 * setup_battle (0x4778a0) resets 0x553070 and 0x553f28 but NOT these three, so they carry over from the last battle on
 * each peer separately and split the next battle (proven: planting them on one peer desynced a battle in 54 s).
 * Hook: the `call 0x499d97` at 0x4778a9 (e8 e9 24 02 00, first call of setup_battle, no arguments, every peer passes it
 * before the first battle tick) -> `call cave`; cave: mov eax,rally_clear | call eax | jmp 0x499d97 (so 0x499d97 returns
 * to 0x4778ae as before). rally_clear (sync.c) zeroes the three dwords only while a session is open, logs one line.
 * Part of mpfix: applied only when mpfix is ON (g_mpfix), so mpfix=0 leaves the exe stock. Prologue bytes are checked
 * first; on a mismatch: WARN and skip. */
static void rally_clear(void);
static void patch_rally(void)
{
    static const BYTE orig[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x53, 0x56, 0x57, 0xe8, 0xe9, 0x24, 0x02, 0x00, 0xe8, 0x7c, 0x02, 0x00, 0x00};
    BYTE hook[5] = {0xe8, 0, 0, 0, 0}, *cave;
    g_rally_ok = 0;
    if (memcmp((void *)0x4778a0, orig, sizeof orig)) { logf_("WARN rally patch: setup_battle bytes differ at 4778a0 - skipped"); return; }
    if (!(cave = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
        logf_("WARN rally patch: no cave memory - skipped"); return;
    }
    cave[0] = 0xb8; *(DWORD *)(cave + 1) = (DWORD)(DWORD_PTR)&rally_clear;   /* mov eax,rally_clear */
    cave[5] = 0xff; cave[6] = 0xd0;                                           /* call eax */
    cave[7] = 0xe9; *(int *)(cave + 8) = (int)(0x499d97 - ((DWORD)(DWORD_PTR)cave + 12));   /* jmp 0x499d97 */
    *(int *)(hook + 1) = (int)((DWORD)(DWORD_PTR)cave - (0x4778a9 + 5));
    g_rally_ok = write_code(0x4778a9, hook, sizeof hook);
    if (!g_rally_ok) VirtualFree(cave, 0, MEM_RELEASE);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    logf_("rally patch (AI rally globals zeroed at battle setup, cave %p): %s", (void *)cave, g_rally_ok ? "applied" : "FAILED");
}

static int patch_mpfix(void)
{
    unsigned i;
    for (i = 0; i < MPFIX_PATCH_COUNT; i++)
        if (memcmp((void *)mpfix_patches[i].va, mpfix_patches[i].orig, mpfix_patches[i].len)) {
            logf_("WARN mpfix: bytes differ at %08x (%s) - NOTHING applied", mpfix_patches[i].va, mpfix_patches[i].name);
            return 0;
        }
    BYTE *cave = (BYTE *)VirtualAlloc(NULL, MPFIX_CAVE_LEN, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) { logf_("WARN mpfix: no cave memory - NOTHING applied"); return 0; }
    memcpy(cave, mpfix_cave, MPFIX_CAVE_LEN);
    for (i = 0; i < MPFIX_CAVE_FIXUP_COUNT; i++) {
        const struct mpfix_cave_fixup *f = &mpfix_cave_fixups[i];
        if (f->kind == MPFIX_REL32) *(int *)(cave + f->cave_offset) = (int)(f->target - ((DWORD)cave + f->cave_offset + 4));
        else *(DWORD *)(cave + f->cave_offset) = (DWORD)cave + f->target;
    }
    BYTE buf[MPFIX_PATCH_COUNT][16];
    for (i = 0; i < MPFIX_PATCH_COUNT; i++) memcpy(buf[i], mpfix_patches[i].repl, mpfix_patches[i].len);
    for (i = 0; i < MPFIX_HOOK_FIXUP_COUNT; i++) {
        const struct mpfix_hook_fixup *h = &mpfix_hook_fixups[i];
        DWORD at = mpfix_patches[h->patch_index].va + h->repl_offset;
        *(int *)(buf[h->patch_index] + h->repl_offset) = (int)(((DWORD)cave + h->cave_offset) - (at + 4));
    }
    /* all-or-nothing: make EVERY target page writable first; any failure restores and applies nothing */
    DWORD oldp[MPFIX_PATCH_COUNT];
    for (i = 0; i < MPFIX_PATCH_COUNT; i++)
        if (!VirtualProtect((void *)mpfix_patches[i].va, mpfix_patches[i].len, PAGE_EXECUTE_READWRITE, &oldp[i])) {
            unsigned k;
            logf_("ERR mpfix: cannot unprotect %08x (%s) - NOTHING applied", mpfix_patches[i].va, mpfix_patches[i].name);
            for (k = i; k > 0; k--) { DWORD tmp; VirtualProtect((void *)mpfix_patches[k - 1].va, mpfix_patches[k - 1].len, oldp[k - 1], &tmp); }
            return 0;
        }
    for (i = 0; i < MPFIX_PATCH_COUNT; i++) memcpy((void *)mpfix_patches[i].va, buf[i], mpfix_patches[i].len);
    for (i = MPFIX_PATCH_COUNT; i > 0; i--) { DWORD tmp; VirtualProtect((void *)mpfix_patches[i - 1].va, mpfix_patches[i - 1].len, oldp[i - 1], &tmp); }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    for (i = 0; i < MPFIX_PATCH_COUNT; i++) logf_("mpfix applied: %s", mpfix_patches[i].name);
    return 1;
}

/* "0", "off", "no", "false" = off; anything else = on (atoi read "on" as 0 and turned mpfix OFF; review 2026-10-07) */
static int cfg_on(const char *v)
{
    return !(v[0] == '0' || !lstrcmpiA(v, "off") || !lstrcmpiA(v, "no") || !lstrcmpiA(v, "false"));
}

static void exe_patch_all(void)
{
    char v[16];
    g_exe_ok = exe_is_known();
    if (!g_exe_ok) { logf_("exe is not the 5 Jun 1997 Lords2.exe - no exe fixes (transport only)"); return; }
    patch_ui();
    patch_lobby();
    patch_continue();
    patch_pace();
    int want = MPFIX_DEFAULT;
    if (ini_get("mpfix", v, sizeof v)) want = cfg_on(v);
    { char *e = getenv("LOTR2NET_MPFIX"); if (e && *e) want = cfg_on(e); }
    g_mpfix = want ? patch_mpfix() : 0;
    g_rally_ok = 0;
    if (g_mpfix) patch_rally();
    logf_("mpfix %s", g_mpfix ? "ON" : want ? "REQUESTED but not applied" : "off (config)");
}
