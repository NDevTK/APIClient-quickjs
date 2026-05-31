// C++ trampoline for the Z3 solve. Z3 can throw on internal errors —
// resource exhaustion, unsupported theory edge cases, version skews
// between the linked libz3 and the .smt2 the C-side built — even
// with Z3_set_error_handler installed (some paths throw via the C++
// runtime directly, bypassing the handler). On native gcc the throw
// unwinds out of libc and the process abort()s. On emcc with
// DISABLE_EXCEPTION_CATCHING=0 + -fexceptions, the throw becomes an
// UnhandledPromiseRejection on the Node host and tears down the
// worker. Either way the verdict for THIS @S is lost AND the worker
// loses every subsequent @S/@H record in the same run.
//
// We surface the failure (never silence it): the caller passes a
// diag callback that emits an @E record carrying the exception's
// what() text and the failing sink type; the verdict returned is a
// dedicated "Z3_ERROR" string so drive.mjs / ast-thread can flag the
// sink as "tainted, verdict unavailable (Z3 internal)" — distinct
// from TAINT_REACH (a real UNSAT-exploit verdict).
#include <exception>
#include <typeinfo>
#include <cstring>
#include <cstdio>
extern "C" {
typedef const char *(*qjs_z3_inner_fn)(void *, const char *, char **);
typedef void (*qjs_z3_diag_fn)(const char *sinkType, const char *what);
const char *qjs_z3_try(qjs_z3_inner_fn f,
                       qjs_z3_diag_fn diag,
                       void *psi, const char *sinkType, char **witness_out) noexcept {
    try {
        return f(psi, sinkType, witness_out);
    } catch (const std::exception &e) {
        if (witness_out) *witness_out = nullptr;
        // Z3's own exception type (z3::exception in z3++.h) carries the
        // message in msg() rather than what(); some Z3 internal throws
        // use that. Try to dig out a meaningful string by combining
        // typeid().name() with whatever the throwable exposes via what().
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s: %s", typeid(e).name(), e.what());
        if (diag) diag(sinkType, buf);
        return "Z3_ERROR";
    } catch (...) {
        if (witness_out) *witness_out = nullptr;
        if (diag) diag(sinkType, "non-std exception");
        return "Z3_ERROR";
    }
}
}
