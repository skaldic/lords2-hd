/* t.h - a small test framework for the patch's tests. Our own code.
 *
 *   TEST(name) { CHECK(cond); CHECK_EQ(a, b); REQUIRE(cond); }
 *
 * Tests register themselves at load (constructors) and run in file order. CHECK records a failure and goes on;
 * REQUIRE ends the test. A crash inside a test (access violation, divide by zero) is caught on the test's own
 * thread, reported as a failure, and the run goes on with the next test. */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void (*t_fn)(void);
typedef struct { const char *name, *file; t_fn fn; } t_case;
static t_case t_cases[1024];
static int t_ncases;
static int t_failed_now;           // failures in the running test
static const char *t_current;

#define TEST(tn)                                                                          \
	static void tn(void);                                                                 \
	__attribute__((constructor)) static void tn##_register(void)                          \
	{ if (t_ncases < 1024) { t_cases[t_ncases].name = #tn; t_cases[t_ncases].file = __FILE__; t_cases[t_ncases].fn = tn; t_ncases++; } } \
	static void tn(void)

static void t_fail(const char *file, int line, const char *what)
{
	const char *f = strrchr(file, '/'), *b = strrchr(file, '\\');
	f = f > b ? f : b;
	printf("    FAIL %s:%d: %s\n", f ? f + 1 : file, line, what);
	t_failed_now++;
}

#define CHECK(c) do { if (!(c)) t_fail(__FILE__, __LINE__, #c); } while (0)
#define REQUIRE(c) do { if (!(c)) { t_fail(__FILE__, __LINE__, "required: " #c); return; } } while (0)
#define CHECK_EQ(a, b) do {                                                               \
		long long t_a = (long long)(a), t_b = (long long)(b);                             \
		if (t_a != t_b) { char t_m[256]; snprintf(t_m, sizeof t_m, "%s == %s (%lld vs %lld)", #a, #b, t_a, t_b); t_fail(__FILE__, __LINE__, t_m); } \
	} while (0)
#define CHECK_NEAR(a, b, tol) do {                                                        \
		double t_a = (double)(a), t_b = (double)(b);                                      \
		if (!(t_a - t_b <= (tol) && t_b - t_a <= (tol))) { char t_m[256]; snprintf(t_m, sizeof t_m, "%s ~ %s (%g vs %g, tolerance %g)", #a, #b, t_a, t_b, (double)(tol)); t_fail(__FILE__, __LINE__, t_m); } \
	} while (0)

/* ---- crash catching: a fault on the test thread jumps back to the runner ---- */
static void *t_jmp[5];
static volatile LONG t_armed;
static volatile DWORD t_thread, t_crashCode;
static void *volatile t_crashAt;
static LONG CALLBACK t_crash_handler(EXCEPTION_POINTERS *ep)
{
	DWORD c = ep->ExceptionRecord->ExceptionCode;
	BOOL fault = c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
		c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_GUARD_PAGE;
	if (!fault || !t_armed || GetCurrentThreadId() != t_thread) return EXCEPTION_CONTINUE_SEARCH;
	t_armed = 0;
	t_crashCode = c;
	t_crashAt = ep->ExceptionRecord->ExceptionAddress;
	__builtin_longjmp(t_jmp, 1);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void t_reset(void);         // per test: defined by tests.c

/* Runs every test whose name contains filter (NULL: all). Returns the number of failed tests. */
static int t_run(const char *filter)
{
	static PVOID veh;
	if (!veh) veh = AddVectoredExceptionHandler(1, t_crash_handler);
	int ran = 0, failed = 0;
	for (int i = 0; i < t_ncases; i++) {
		if (filter && *filter && !strstr(t_cases[i].name, filter)) continue;
		t_current = t_cases[i].name;
		t_failed_now = 0;
		t_reset();
		t_thread = GetCurrentThreadId();
		if (__builtin_setjmp(t_jmp) == 0) {
			t_armed = 1;
			t_cases[i].fn();
			t_armed = 0;
		}
		else {
			char m[128];
			snprintf(m, sizeof m, "crashed: exception %08lx at %p", t_crashCode, t_crashAt);
			t_fail(t_cases[i].file, 0, m);
		}
		ran++;
		if (t_failed_now) { failed++; printf("FAIL %s\n", t_cases[i].name); }
		else printf("ok   %s\n", t_cases[i].name);
	}
	printf("\n%d tests, %d failed\n", ran, failed);
	return failed;
}
