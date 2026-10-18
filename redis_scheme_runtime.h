/*
 * redis_scheme_runtime.h -- minimal C bridge over the dummyscheme VM.
 *
 * create/destroy a VM, evaluate a script, reply with the result as a string.
 * Only this header (extern "C") is visible to the pure-C module.
 */
#ifndef REDIS_SCHEME_RUNTIME_H
#define REDIS_SCHEME_RUNTIME_H

#include <stddef.h>
#include "redismodule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to one scheme VM. */
typedef struct redis_scheme_vm_s redis_scheme_vm_t;

/*
 * Create a VM bound to the current command ctx.  The VM's memory goes
 * through the Redis allocator (zmalloc), like Lua's.  Returns NULL on
 * failure; the handle is non-NULL on success.
 */
redis_scheme_vm_t *redis_scheme_create(RedisModuleCtx *ctx);

/*
 * Destroy a VM created by redis_scheme_create.  Returns 0 on success, -1 on
 * failure (error in err).
 */
int redis_scheme_destroy(redis_scheme_vm_t *h, char *err, int err_cap);

/*
 * Evaluate the script in VM h.  On success the result is stored inside the
 * VM and later emitted by redis_scheme_reply; on failure returns -1 and the
 * message is written to err (err_cap bytes).
 */
int redis_scheme_eval(redis_scheme_vm_t *h, const char *script,
                      size_t script_len, char *err, int err_cap);

/*
 * Bind the KEYS and ARGV globals before evaluating the script.  argv follows
 * the EVAL shape: argv[3] .. argv[3+numkeys-1] are the keys, the remaining
 * argv[3+numkeys] .. argv[argc-1] are the args.  Both are bound as vectors
 * (0-based, usable with the host vector-ref/vector-length).  Returns 0 on
 * success, -1 on failure (error in err).
 */
int redis_scheme_bind_args(redis_scheme_vm_t *h, RedisModuleString **argv,
                           int argc, long long numkeys, char *err, int err_cap);

/* Reply to ctx with the value of the last eval.  Values are converted to
   typed Redis replies: int/float -> integer, string -> bulk, proper
   list/vector -> array (recursive), #t -> 1, #f -> null, {err}/{ok} tables
   -> error/status; any other value keeps the write-rendering (bulk string)
   path. */
void redis_scheme_reply(redis_scheme_vm_t *h, RedisModuleCtx *ctx);

/* Script cache: the server-side sha1 hex -> body store behind
   SCHEME.EVAL / SCHEME.EVALSHA / SCHEME.SCRIPT, in process memory only
   (dropped on restart, like the script cache). */

/* Store body in the cache under its sha1 hex and write that hex as 41
   bytes (40 chars + NUL) to out_sha1.  evictable != 0 marks an EVAL-sourced
   entry (LRU-evicted past 500 entries); LOAD-sourced entries pass 0 and
   are never evicted. */
void redis_scheme_cache_script(const char *body, size_t body_len,
                               char out_sha1[41], int evictable);

/* 1 when sha1hex is in the cache, else 0. */
int redis_scheme_cache_has(const char *sha1hex);

/* Drop every cached script. */
void redis_scheme_cache_flush(void);

/*
 * Run body end-to-end on a fresh VM: bind KEYS/ARGV from argv (the EVAL
 * shape: argv[3] .. argv[3+numkeys-1] keys, the rest args), evaluate,
 * reply to ctx, destroy the VM.  The script enters the cache under its
 * sha1 hex first.  Returns 0 on success, -1 on failure (the error reply
 * has already been written to ctx).
 */
int redis_scheme_run(RedisModuleCtx *ctx, const char *body, size_t body_len,
                     RedisModuleString **argv, int argc, long long numkeys);

/*
 * Like redis_scheme_run, but the body comes from the cache (sha1 hex
 * lookup).  On a miss writes a NOSCRIPT error reply to ctx.  Returns 0 on
 * success (hit), -1 on miss or failure.
 */
int redis_scheme_run_cached(RedisModuleCtx *ctx, const char *sha1hex,
                            RedisModuleString **argv, int argc,
                            long long numkeys);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_SCHEME_RUNTIME_H */
