/*
 * Forced-exec fork-local assert — MIRRORS engine/host/check.h (the quickjs submodule is a separate repo and
 * cannot include the host header, exactly as extension/check.js mirrors the same law on the JS side). Same
 * semantics: DFAIL = a DEV-ONLY should-never-happen (design invariant / not-yet-built capability) — emits
 * @WHY at the origin then aborts; CHECK/CHECK_FAIL = ALWAYS fatal (dev AND release) for a "must-not-proceed
 * even in production" invariant. DFAIL compiles out in release (APICLIENT_DEV=0); its condition must be
 * side-effect-free and never recoverable control flow.
 * All four names are the host header's and the JS mirror's. Only the EMIT is this file's (a plain @WHY/@E line
 * rather than the host's JSON), because the submodule cannot include the host header.
 * It lives in its OWN header because every translation unit of the fork asserts — libregexp's parser as much as
 * the interpreter — and a per-file copy of the definition is a place for two of them to drift apart.
 */
#ifndef QUICKJS_CHECK_H
#define QUICKJS_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#if defined(APICLIENT_DEV) && APICLIENT_DEV == 0
#define DFAIL(msg)         ((void)0)
#define DCHECK(cond, msg)  ((void)0)
#else
#define DFAIL(msg)         do { fprintf(stderr, "@WHY %s (%s:%d)\n", (msg), __FILE__, __LINE__); abort(); } while (0)
#define DCHECK(cond, msg)  do { if (!(cond)) DFAIL(msg); } while (0)
#endif
#define CHECK_FAIL(msg)    do { fprintf(stderr, "@E %s (%s:%d)\n", (msg), __FILE__, __LINE__); abort(); } while (0)
#define CHECK(cond, msg)   do { if (!(cond)) CHECK_FAIL(msg); } while (0)

#endif /* QUICKJS_CHECK_H */
