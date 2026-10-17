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

/* Create a VM.  Returns NULL on failure. */
redis_scheme_vm_t *redis_scheme_create(void);

/* Destroy a VM.  Returns 0 on success, -1 on failure (error in err). */
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

/* Reply to ctx with the value of the last eval, rendered as a bulk string. */
void redis_scheme_reply(redis_scheme_vm_t *h, RedisModuleCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_SCHEME_RUNTIME_H */
