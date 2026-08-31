/*
 * Forced-exec fork-local assert — MIRRORS engine/host/check.h (the quickjs submodule is a separate repo and
 * cannot include the host header, exactly as extension/check.js mirrors the same law on the JS side). Same
 * semantics: DFAIL = a DEV-ONLY should-never-happen (design invariant / not-yet-built capability) — emits
 * @WHY at the origin then aborts; CHECK/CHECK_FAIL = ALWAYS fatal (dev AND release) for a "must-not-proceed
 * even in production" invariant. DFAIL compiles out in release (APICLIENT_DEV=0); its condition must be
 * side-effect-free and never recoverable control flow.
 * Every name here is the host header's and the JS mirror's. Only the EMIT is this file's (a plain @WHY/@E line
 * rather than the host's JSON), because the submodule cannot include the host header.
 * It lives in its OWN header because every translation unit of the fork asserts — libregexp's parser as much as
 * the interpreter — and a per-file copy of the definition is a place for two of them to drift apart.
 *
 * AND A MIRROR THAT CARRIES FEWER NAMES THAN WHAT IT MIRRORS IS NOT A SMALLER MIRROR, IT IS A CAPABILITY THE
 * FORK DOES NOT HAVE. This header said "all four names" while the host header had grown formatted forms, and
 * the drift is invisible in exactly the way a mirror's is: nothing includes both, so no compiler can compare
 * them, and the absence shows up only as asserts inside the fork that cannot say WHICH value broke them. That
 * is not cosmetic here — the fork is where the interpreter, the trampoline and every step machine live, so a
 * should-never-happen with hundreds of call sites converging on it reports one line and no operand. Measured:
 * a step machine leaked a code into a stage that routed five others, and establishing WHICH code took a
 * reading of the machine rather than of the crash, because the crash had no way to hold an integer.
 * SO THE RULE IS: a name the host header has and this one lacks is a fork-side assert that cannot state its
 * evidence, and the fix is to add it HERE first. The always-fatal formatted pair (CHECKF / CHECK_FAILF) is
 * deliberately absent because nothing in the fork composes one yet; the day something does, it is added here
 * before it is used, and its absence would show as a CHECK in this submodule spelling out a runtime value it
 * cannot interpolate.
 */
#ifndef QUICKJS_CHECK_H
#define QUICKJS_CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* The reason buffer for a composed message. Sized here rather than at the call sites for the reason the host
   header composes into its own buffer: a per-site `char msg[400]` is a cap nobody reviewed, and a truncated
   reason is prose a reader cannot tell was cut. */
#define APICLIENT_QJS_REASON_CAP 512

#if defined(__GNUC__) || defined(__clang__)
#define APICLIENT_QJS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define APICLIENT_QJS_PRINTF(f, a)
#endif

/* THE RELEASE FORM OF A COMPILED-OUT FORMATTED ASSERT, and it is not decoration. `sizeof` does not evaluate its
   operand, so no argument of a dropped message is computed — but the call is still PARSED, which is what keeps
   -Wformat checking the arguments in the build that never runs them, and keeps a variable used ONLY in an
   assert message from reading as unused. Dropping the arguments entirely instead would mean release is the
   build where a wrong format specifier is legal, which is the one place nobody would look for it. */
APICLIENT_QJS_PRINTF(1, 2)
static inline int apiclient_qjs_fmt_check(const char *fmt, ...) { (void) fmt; return 0; }
#define APICLIENT_QJS_FMT_UNUSED(...) ((void) sizeof apiclient_qjs_fmt_check(__VA_ARGS__))

#if defined(APICLIENT_DEV) && APICLIENT_DEV == 0
#define DFAIL(msg)         ((void)0)
#define DCHECK(cond, msg)  ((void)0)
#define DCHECKF(cond, ...) do { (void)sizeof(cond); APICLIENT_QJS_FMT_UNUSED(__VA_ARGS__); } while (0)
#else
#define DFAIL(msg)         do { fprintf(stderr, "@WHY %s (%s:%d)\n", (msg), __FILE__, __LINE__); abort(); } while (0)
#define DCHECK(cond, msg)  do { if (!(cond)) DFAIL(msg); } while (0)
/* Composed into ONE buffer and emitted as ONE line, never as three fprintf calls: the harness reads
   `@WHY <reason> (<file>:<line>)` as a record, and a reason split across writes is a record another thread's
   output can land inside of. The format arguments are as side-effect-free as a DCHECK condition must be —
   they vanish with the assert in release. */
#define DCHECKF(cond, ...) do { if (!(cond)) { \
        char apiclient_qjs_r_[APICLIENT_QJS_REASON_CAP]; \
        snprintf(apiclient_qjs_r_, sizeof(apiclient_qjs_r_), __VA_ARGS__); \
        DFAIL(apiclient_qjs_r_); \
    } } while (0)
#endif
#define CHECK_FAIL(msg)    do { fprintf(stderr, "@E %s (%s:%d)\n", (msg), __FILE__, __LINE__); abort(); } while (0)
#define CHECK(cond, msg)   do { if (!(cond)) CHECK_FAIL(msg); } while (0)

#endif /* QUICKJS_CHECK_H */
