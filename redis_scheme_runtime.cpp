/*
 * redis_scheme_runtime.cpp -- C bridge over the dummyscheme VM.
 *
 * create/destroy a VM, evaluate a script, reply with the result rendered as a string.
 *
 */
#include "vm.h"
#include "redis_scheme_runtime.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>

using namespace Scheme;

/* Registry keys */
#define REDIS_SCHEME_CTX_KEY "~redis-scheme-ctx"
#define REDIS_SCHEME_LAST_KEY "~redis-scheme-last"

struct redis_scheme_vm_s {
  VM *vm;               /* interpreter (created by scm_create_vm) */
  RedisModuleCtx *ctx;  /* ctx of the current command (set at eval, used at reply) */
  char *errbuf;         /* most recent error message (owned) */
  int   errcap;
};

/* ---------------------------------------------------------------------------
 * Host functions -- Add entries here as features land, e.g.
 * RegCProc("redis-call", ...).  Keep the (NULL, -1) sentinel last.
 * ------------------------------------------------------------------------ */

static const RegCProc redis_scheme_lib[] = {
  RegCProc(NULL, -1)               /* end sentinel */
};

/* ---------------------------------------------------------------------------
 * C API implementation
 * ------------------------------------------------------------------------ */

/* Root v on the value stack, then store it in the registry: the collector
   only marks stack slots and the registry, so a bare C-local ValueT would
   not survive a collection inside scm_registry_set. */
static void redis_registry_set(VM* vm, const char* name, ValueT v)
{
  Sgcvar1(vm, slot);
  *slot = v;
  scm_registry_set(vm, name, *slot);
}

redis_scheme_vm_t *
redis_scheme_create(void)
{
  redis_scheme_vm_t *h = (redis_scheme_vm_t *) malloc(sizeof(*h));
  if (h == NULL) {
    return NULL;
  }

  h->vm = NULL;
  h->ctx = NULL;
  h->errbuf = NULL;
  h->errcap = 0;

  try {
    h->vm = scm_create_vm();
    if (h->vm == NULL) {
      free(h);
      return NULL;
    }
    /* Store the handle in the registry so host functions (added later) can
       find their context. */
    scm_registry_set(h->vm, REDIS_SCHEME_CTX_KEY,
        scm_make_lightuserdata(h->vm, h));
    scm_register_lib(h->vm, redis_scheme_lib);
  }
  catch (...) {
    if (h->vm != NULL) {
      scm_destroy_vm(h->vm);
    }
    free(h);
    return NULL;
  }

  return h;
}

int
redis_scheme_destroy(redis_scheme_vm_t *h, char *err, int err_cap)
{
  if (err_cap > 0) {
    err[0] = '\0';
  }
  if (h == NULL) {
    return 0;
  }

  if (h->vm != NULL) {
    try {
      scm_destroy_vm(h->vm);
    }
    catch (const char *e) {
      if (err_cap > 0) {
        snprintf(err, err_cap, "scheme teardown error: %s",
            e != NULL ? e : "unknown");
      }
      free(h->errbuf);
      free(h);
      return -1;
    }
    catch (...) {
      if (err_cap > 0) {
        snprintf(err, err_cap, "scheme teardown error: unknown exception");
      }
      free(h->errbuf);
      free(h);
      return -1;
    }
  }

  free(h->errbuf);
  free(h);
  return 0;
}

int
redis_scheme_eval(redis_scheme_vm_t *h, const char *script, size_t script_len,
                  char *err, int err_cap)
{
  if (h == NULL || h->vm == NULL) {
    return -1;
  }
  if (err_cap > 0) {
    err[0] = '\0';
  }
  if (h->errbuf != NULL) {
    h->errbuf[0] = '\0';
  }

  /* Full source = user script + trailing newline (the host lexer rejects
     #t/#f exactly at EOF). */
  std::string src(script, script_len);
  src += '\n';

  try {
    ValueT res = scm_eval_string(h->vm, src.c_str(), (int) src.size());
    redis_registry_set(h->vm, REDIS_SCHEME_LAST_KEY, res);
  }
  catch (const char *e) {
    if (err_cap > 0) {
      const char *msg = (h->errbuf != NULL && h->errbuf[0] != '\0')
                          ? h->errbuf : (e != NULL ? e : "unknown scheme error");
      snprintf(err, err_cap, "%s", msg);
    }
    return -1;
  }
  catch (...) {
    if (err_cap > 0) {
      const char *msg = (h->errbuf != NULL && h->errbuf[0] != '\0')
                          ? h->errbuf : "unknown scheme error";
      snprintf(err, err_cap, "%s", msg);
    }
    return -1;
  }

  return 0;
}

void
redis_scheme_reply(redis_scheme_vm_t *h, RedisModuleCtx *ctx)
{
  if (h == NULL || h->vm == NULL || ctx == NULL) {
    return;  /* the caller already replied with an error; nothing to do */
  }

  ValueT last = scm_registry_get(h->vm, REDIS_SCHEME_LAST_KEY);

  /* Render the value with write semantics (strings come out quoted; proper
     type conversion replaces this later).  Copy immediately: the pointer is
     only valid until the next scm_tostring call. */
  int n = 0;
  const char *s = NULL;
  try {
    s = scm_tostring(h->vm, &last, &n);
  }
  catch (...) {
    const char *msg = (h->errbuf != NULL && h->errbuf[0] != '\0')
                        ? h->errbuf : "cannot render scheme value";
    RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", msg);
    return;
  }
  if (s == NULL) {
    RedisModule_ReplyWithNull(ctx);
    return;
  }
  RedisModule_ReplyWithStringBuffer(ctx, s, (size_t) n);
}
