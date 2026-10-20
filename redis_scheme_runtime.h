/*
 * redis_scheme_runtime.h -- C bridge over the dummyscheme VM.
 *
 * The VM is a process-lifetime singleton.  Each cached script compiles into a
 * closure bound to a fresh sandbox env (hashtable whose metatable __index
 * points at genv): top-level define/set! write into the sandbox, reads fall
 * through to genv, and the builtins plus the redis-* host functions are never
 * polluted.  KEYS/ARGV are bound on genv every request.
 */
#ifndef REDIS_SCHEME_RUNTIME_H
#define REDIS_SCHEME_RUNTIME_H

#include <stddef.h>
#include "redismodule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the long-lived VM and register the redis-* host functions.
   Called once from RedisModule_OnLoad.  Returns 0 on success, -1 on failure. */
int redis_scheme_runtime_init(void);

/* Destroy the long-lived VM.  Called from RedisModule_OnUnload and at exit. */
void redis_scheme_runtime_done(void);

/* Compile `body` into a closure in a fresh sandbox env, root it in the VM
   registry under "~script:<sha1>", and remember the sha1 in the LRU cache.
   Writes the 40-char lowercase hex (NUL-terminated, 41 bytes) to out_sha1.
   evictable != 0 marks an EVAL-sourced entry (LRU-evicted past the cap);
   LOAD-sourced entries pass 0 and are never evicted.  On compile failure
   writes an error reply to ctx and returns -1; on success returns 0. */
int redis_scheme_cache_script(RedisModuleCtx *ctx, const char *body,
                              size_t body_len, char out_sha1[41], int evictable);

/* 1 when sha1hex is in the cache, else 0. */
int redis_scheme_cache_has(const char *sha1hex);

/* Drop every cached script: unset each closure from the registry (the
   closures and their sandbox envs become garbage) and clear the LRU. */
void redis_scheme_cache_flush(void);

/*
 * Run body end-to-end: cache it (evictable), bind KEYS/ARGV from argv (the
 * EVAL shape: argv[3] .. argv[3+numkeys-1] keys, the rest args), apply the
 * cached closure, reply to ctx.  Returns 0 on success, -1 on failure (the
 * error reply has already been written to ctx).
 */
int redis_scheme_run(RedisModuleCtx *ctx, const char *body, size_t body_len,
                     RedisModuleString **argv, int argc, long long numkeys);

/*
 * Like redis_scheme_run, but the closure comes from the cache by sha1 hex
 * lookup.  On a miss writes a NOSCRIPT error reply to ctx.  Returns 0 on
 * success (hit), -1 on miss or failure.
 */
int redis_scheme_run_cached(RedisModuleCtx *ctx, const char *sha1hex,
                            RedisModuleString **argv, int argc,
                            long long numkeys);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_SCHEME_RUNTIME_H */
