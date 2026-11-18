/* victory.c - in-memory fix to Lords2.exe (5 Jun 1997):
 * "the last player standing in a multiplayer game gets no victory screen".
 *
 * Include this from dplayx.c AFTER exepatch.c and call victory_patch() once from
 * exe_patch_all() (or straight after it). It needs write_code(), logf_() and g_exe_ok,
 * which exepatch.c defines. Our own code. ASCII only. Nothing on disk is changed.
 *
 * THE BUG
 *   recompute_lord_standings (0x49aa0e, void __cdecl(void)) is the only place a game result
 *   is decided and the only writer of the "living others" counter 0x56d5d8, which turns a
 *   0xe0 pop into the Victory screen. It runs from the turn's phase 7, from a LOCAL
 *   elimination (0x49b42b) and once at setup - but NOT from the two paths that remove a
 *   REMOTE lord:
 *     0x443587  network command 0x24, "a peer's lord was eliminated"
 *     0x49cf20  "remove this player", reached from the net-session lord sweep (0x49b2a3)
 *               and from the DirectPlay DESTROYPLAYER path (0x43e726)
 *   So on the survivor the standings stay stale, the 0xe0 pop plays with a foreign sender,
 *   0x56d5d8 is still > 0 and no victory is produced.
 *
 * THE FIX - wrap the CALLERS, never the function bodies.
 *   After the original handler has run, call recompute_lord_standings. Its body (0x49aa0e ..
 *   0x49b2a2) writes game globals and calls msg_post_to_player (0x472bc5) twice; that callee
 *   only appends to the local message queue. There is NO call to the RNG (0x404a46) and NO
 *   call to the net command sender (0x43eda0) anywhere in the body, so calling it is pure
 *   game state and happens identically on every receiving peer.
 *
 * SITES
 *   0x43e726  call 0x49cf20  bytes e8 f5 e7 05 00   (FUN_0043e6a2, DESTROYPLAYER)
 *   0x49b331  call 0x49cf20  bytes e8 ea 1b 00 00   (FUN_0049b2a3, net-session lord sweep)
 *   0x4d5a10  dword 0x443587 (dispatch table entry; see below)
 *
 * 0x49cf09 (call 0x49cf20 inside FUN_0049cebe) is deliberately NOT patched: that loop clears
 * lords 2..5 and the enclosing function already calls recompute_lord_standings itself two
 * instructions after the loop at 0x49cf16. Wrapping it would run the victory check on a
 * half-cleared lord table and could post a result that the post-loop state contradicts.
 *
 * 0x443587 has NO direct call site at all. Its only reach is the indirect dispatch
 *   `call dword ptr [ecx*4 + 0x4d5980]` at 0x43ed8e (ecx = the packet's command byte; index
 *   0x24 = 36 -> entry 0x4d5a10, which held 0x443587). A rel32 rewrite is impossible there,
 * so the TABLE ENTRY is repointed at the wrapper instead - same effect, one 4-byte write,
 * and it is refused if the entry does not already hold 0x443587. */

#ifndef VICTORY_C
#define VICTORY_C

#define VIC_LORD_ELIMINATED 0x00443587u   /* void __cdecl(void)   - command 0x24 receiver */
#define VIC_PLAYER_REMOVED  0x0049cf20u   /* void __cdecl(int lord) */
#define VIC_RECOMPUTE       0x0049aa0eu   /* void __cdecl(void)   - the only victory check */
#define VIC_CMD_TABLE       0x004d5980u   /* network command dispatch table, index = command byte */
#define VIC_CMD_ELIMINATED  0x24u         /* "a peer's lord was eliminated" */

/* One per wrapped handler. Same arguments as the original, cdecl (both originals end in a bare
 * `ret`, and their call sites do `push arg` / `call` / `add esp,4` - so no callee pops). */

static void __cdecl vic_after_lord_eliminated(void)
{
    ((void (__cdecl *)(void))VIC_LORD_ELIMINATED)();
    ((void (__cdecl *)(void))VIC_RECOMPUTE)();
}

static void __cdecl vic_after_player_removed(int lord)
{
    ((void (__cdecl *)(int))VIC_PLAYER_REMOVED)(lord);
    ((void (__cdecl *)(void))VIC_RECOMPUTE)();
}

struct vic_hook { DWORD site; DWORD target; void *wrapper; const char *why; };

/* direct E8 rel32 call sites */
static const struct vic_hook vic_hooks[] = {
    { 0x0043e726u, VIC_PLAYER_REMOVED, (void *)vic_after_player_removed,
      "DESTROYPLAYER: player removed (FUN_0043e6a2)" },
    { 0x0049b331u, VIC_PLAYER_REMOVED, (void *)vic_after_player_removed,
      "net-session lord sweep: player removed (FUN_0049b2a3)" },
};

struct vic_table { DWORD entry; DWORD expect; void *wrapper; const char *why; };

/* function-pointer table entries, for a handler with no direct caller */
static const struct vic_table vic_tables[] = {
    { VIC_CMD_TABLE + VIC_CMD_ELIMINATED * 4u, VIC_LORD_ELIMINATED, (void *)vic_after_lord_eliminated,
      "network command 0x24: peer lord eliminated" },
};

static int vic_is_call(DWORD site, DWORD target)
{
    const BYTE *p = (const BYTE *)site;
    int rel;
    if (p[0] != 0xe8) return 0;
    memcpy(&rel, p + 1, sizeof rel);
    return (DWORD)(site + 5 + (DWORD)rel) == target;
}

static void patch_victory(void)
{
    unsigned i, total = 0, done = 0, bad = 0;

    for (i = 0; i < (unsigned)(sizeof vic_hooks / sizeof vic_hooks[0]); i++) {
        const struct vic_hook *h = &vic_hooks[i];
        int rel;
        total++;
        if (!vic_is_call(h->site, h->target)) {
            logf_("WARN victory: call site %08x is not E8 -> %08x (%s) - refused",
                  (DWORD)h->site, (DWORD)h->target, h->why);
            bad++;
            continue;
        }
        rel = (int)(((DWORD)h->wrapper) - (h->site + 5));
        if (!write_code(h->site + 1, &rel, sizeof rel)) {
            logf_("WARN victory: cannot write call site %08x (%s) - refused", (DWORD)h->site, h->why);
            bad++;
            continue;
        }
        done++;
    }

    for (i = 0; i < (unsigned)(sizeof vic_tables / sizeof vic_tables[0]); i++) {
        const struct vic_table *t = &vic_tables[i];
        DWORD w;
        total++;
        if (*(const volatile DWORD *)t->entry != t->expect) {
            logf_("WARN victory: dispatch entry %08x holds %08x, not %08x (%s) - refused",
                  (DWORD)t->entry, *(const volatile DWORD *)t->entry, (DWORD)t->expect, t->why);
            bad++;
            continue;
        }
        w = (DWORD)t->wrapper;
        if (!write_code(t->entry, &w, sizeof w)) {
            logf_("WARN victory: cannot write dispatch entry %08x (%s) - refused", (DWORD)t->entry, t->why);
            bad++;
            continue;
        }
        done++;
    }

    if (bad || !done)
        logf_("WARN victory: %u of %u sites patched - the victory fix is INCOMPLETE", done, total);
    else
        logf_("victory: %u call sites patched", done);
}

/* Called once at load, after g_exe_ok is known. */
void victory_patch(void)
{
    if (!g_exe_ok) return;
    patch_victory();
}

#endif /* VICTORY_C */
