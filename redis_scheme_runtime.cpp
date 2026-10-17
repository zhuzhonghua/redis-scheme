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
#include <stdarg.h>
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
 * Host functions -- the functions callable from scheme scripts.  The
 * helpers below (get_ctx / throw_err / reply_tagged) support them; register
 * new functions in redis_scheme_lib, keeping the (NULL, -1) sentinel last.
 * ------------------------------------------------------------------------ */

/* Resolve the command handle stored in the VM registry. */
static redis_scheme_vm_t *
redis_scheme_get_ctx(VM *vm)
{
  ValueT ctx = scm_registry_get(vm, REDIS_SCHEME_CTX_KEY);
  if (!scm_is_lightuserdata(vm, &ctx)) {
    throw "no scheme context";
  }
  return (redis_scheme_vm_t *) scm_to_lightuserdata(vm, &ctx);
}

/* Fill the handle's error buffer and throw it.  The eval wrapper prefers
   h->errbuf over the thrown string, so the message reaches the client
   as-is. */
static void
redis_scheme_throw_err(redis_scheme_vm_t *h, const char *fmt, ...)
{
  if (h->errbuf == NULL) {
    h->errcap = 256;
    h->errbuf = (char *) malloc((size_t) h->errcap);
    if (h->errbuf == NULL) {
      throw "ScmError";
    }
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(h->errbuf, (size_t) h->errcap, fmt, ap);
  va_end(ap);
  throw h->errbuf;
}

/* Build the tagged reply value: a hashtable with a single string field
   "err"/"ok" holding msg -- the Scheme form of an error / status reply
   object.  msg must be a string (caller stack slot, rooted); the key is
   built fresh here and rooted before the table set (which may collect). */
static ValueT
redis_reply_tagged(VM* vm, ValueT* msg, const char* tag, int tag_len)
{
  Sgcvar3(vm, tbl_slot, key_slot, val_slot);
  *tbl_slot = scm_make_table(vm);
  *key_slot = scm_make_string(vm, tag, tag_len);        /* "err" / "ok" */
  *val_slot = *msg;
  scm_table_set(vm, tbl_slot, key_slot, *val_slot);
  return *tbl_slot;
}

/* redis-error-reply / redis-status-reply: both build a tagged reply value
   -- a hashtable with a single string field "err"/"ok" holding the message,
   the Scheme form of an error / status reply object.  redis_scheme_reply
   recognizes the shape and emits a Redis error / status reply; any other
   value keeps the write-rendering path. */
static ValueT
redis_scheme_stub_error_reply(VM* vm, ValueT* msg)
{
  if (!scm_is_string(vm, msg)) {
    redis_scheme_throw_err(redis_scheme_get_ctx(vm),
        "redis-error-reply: expected string, got %s", scm_typename(vm, msg));
  }
  return redis_reply_tagged(vm, msg, "err", 3);
}

static ValueT
redis_scheme_stub_status_reply(VM* vm, ValueT* msg)
{
  if (!scm_is_string(vm, msg)) {
    redis_scheme_throw_err(redis_scheme_get_ctx(vm),
        "redis-status-reply: expected string, got %s", scm_typename(vm, msg));
  }
  return redis_reply_tagged(vm, msg, "ok", 2);
}

/* Registration table; keep the (NULL, -1) sentinel last. */
static const RegCProc redis_scheme_lib[] = {
  RegCProc("redis-error-reply", redis_scheme_stub_error_reply),
  RegCProc("redis-status-reply", redis_scheme_stub_status_reply),
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

/* Translate an exception into the module error contract: fill err with the
   message, preferring h->errbuf (set by redis_scheme_throw_err) over the
   thrown string.  The caller returns -1 after calling this. */
static void
redis_scheme_fill_err(redis_scheme_vm_t *h, const char *e, char *err, int err_cap)
{
  if (err_cap > 0) {
    const char *msg = (h->errbuf != NULL && h->errbuf[0] != '\0')
                        ? h->errbuf : (e != NULL ? e : "unknown scheme error");
    snprintf(err, err_cap, "%s", msg);
  }
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
  /* h is always a non-NULL handle from redis_scheme_create. */
  if (err_cap > 0) {
    err[0] = '\0';
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
    redis_scheme_fill_err(h, e, err, err_cap);
    return -1;
  }
  catch (...) {
    redis_scheme_fill_err(h, NULL, err, err_cap);
    return -1;
  }
  return 0;
}

int
redis_scheme_bind_args(redis_scheme_vm_t *h, RedisModuleString **argv,
                       int argc, long long numkeys, char *err, int err_cap)
{
  if (err_cap > 0) {
    err[0] = '\0';
  }

  /* The caller (module.c) already validated argc >= 3 + numkeys and
     numkeys >= 0; only bind the vectors here. */
  VM *vm = h->vm;
  try {
    int nkeys = (int) numkeys;
    int nargs = argc - 3 - nkeys;

    /* KEYS: vector of the declared keys.  The slot roots the vector across
       any collection inside scm_make_string/scm_vector_set/scm_set_global. */
    Sgcvar1(vm, keys_slot);
    *keys_slot = scm_make_vector(vm, nkeys);
    for (int i = 0; i < nkeys; i++) {
      size_t len = 0;
      const char *p = RedisModule_StringPtrLen(argv[3 + i], &len);
      scm_vector_set(vm, keys_slot, i, scm_make_string(vm, p, (int) len));
    }
    scm_set_global(vm, "KEYS", *keys_slot);

    /* ARGV: vector of the remaining arguments. */
    Sgcvar1(vm, args_slot);
    *args_slot = scm_make_vector(vm, nargs);
    for (int i = 0; i < nargs; i++) {
      size_t len = 0;
      const char *p = RedisModule_StringPtrLen(argv[3 + nkeys + i], &len);
      scm_vector_set(vm, args_slot, i, scm_make_string(vm, p, (int) len));
    }
    scm_set_global(vm, "ARGV", *args_slot);
  }
  catch (const char *e) {
    redis_scheme_fill_err(h, e, err, err_cap);
    return -1;
  }
  catch (...) {
    redis_scheme_fill_err(h, NULL, err, err_cap);
    return -1;
  }
  return 0;
}

/* Detect a tagged reply value: a hashtable whose "err" field is a string
   (error reply) or whose "ok" field is a string (status reply); "err" is
   checked first.  Returns 1 for an error reply, 2 for a status reply, 0
   when the value is anything else (which keeps the write-rendering path).
   Extra fields do not matter.  last is registry-rooted, so the hashtable
   survives any allocation. */
static int
redis_tagged_reply(VM* vm, ValueT* last)
{
  if (!scm_is_table(vm, last)) {
    return 0;
  }
  Sgcvar2(vm, key_slot, val_slot);
  *key_slot = scm_make_string(vm, "err", 3);
  *val_slot = scm_table_get(vm, last, key_slot);
  if (scm_is_string(vm, val_slot)) {
    return 1;
  }
  *key_slot = scm_make_string(vm, "ok", 2);
  *val_slot = scm_table_get(vm, last, key_slot);
  if (scm_is_string(vm, val_slot)) {
    return 2;
  }
  return 0;
}

void
redis_scheme_reply(redis_scheme_vm_t *h, RedisModuleCtx *ctx)
{
  ValueT last = scm_registry_get(h->vm, REDIS_SCHEME_LAST_KEY);

  /* Tagged replies first: a hashtable field "err" -> error, "ok" -> status.
     RedisModule_ReplyWithError / ReplyWithSimpleString need a NUL-terminated
     buffer, so copy the payload out of the scheme string first. */
  int tag = redis_tagged_reply(h->vm, &last);
  if (tag != 0) {
    /* fetch the payload -- the single "err"/"ok" field (recognized above) */
    Sgcvar2(h->vm, key_slot, payload_slot);
    const char *key_name = (tag == 1) ? "err" : "ok";
    int key_len = (tag == 1) ? 3 : 2;
    *key_slot = scm_make_string(h->vm, key_name, key_len);
    *payload_slot = scm_table_ref(h->vm, &last, key_slot);
    int n = 0;
    const char *s = scm_get_string(h->vm, payload_slot, &n);
    std::string body(s != NULL ? s : "", s != NULL ? (size_t) n : 0);
    if (tag == 1) {
      RedisModule_ReplyWithError(ctx, body.c_str());
    } else {
      RedisModule_ReplyWithSimpleString(ctx, body.c_str());
    }
    return;
  }

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
