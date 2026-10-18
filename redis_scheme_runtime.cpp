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
#include <vector>
#include <charconv>
#include <climits>

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

/* Build a {err=...} table from a C string, the shared error shape of
   redis-error-reply and redis-pcall's error return. */
static ValueT
redis_scheme_err_table(VM* vm, const char* msg, int len)
{
  Sgcvar1(vm, slot);
  *slot = scm_make_string(vm, msg != NULL ? msg : "", len);
  return redis_reply_tagged(vm, slot, "err", 3);
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
  int len = 0;
  const char *s = scm_get_string(vm, msg, &len);
  return redis_scheme_err_table(vm, s, len);
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

/* Shortest round-trip text of a double, matching the number-to-string
   conversion used for script arguments: an integer-valued double within
   long-long range prints as an integer (so 1e15 -> "1000000000000000",
   never "1e+15"), everything else uses the shortest decimal that parses
   back to the same double (so 0.1 -> "0.1", not "0.10000000000000001"). */
static std::string
redis_scheme_ftoa(double d)
{
  if (d >= (double)(-LLONG_MAX / 2) && d <= (double)(LLONG_MAX / 2)) {
    long long ll = (long long)d;
    if (ll == d) {
      char buf[32];
      int n = snprintf(buf, sizeof(buf), "%lld", ll);
      return std::string(buf, (size_t)n);
    }
  }
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), d);
  return std::string(buf, res.ptr);
}

/* Convert a scheme value into a RedisModuleString command argument.
   Strings pass through; ints and floats render as decimal text.  Returns a
   string the caller must free, or throws via the error channel. */
static RedisModuleString *
redis_scheme_arg_to_string(redis_scheme_vm_t *h, ValueT *slot)
{
  RedisModuleCtx *ctx = h->ctx;
  if (scm_is_string(h->vm, slot)) {
    int n = 0;
    const char *s = scm_get_string(h->vm, slot, &n);
    return RedisModule_CreateString(ctx, s != NULL ? s : "", (size_t) n);
  }
  if (scm_is_int(h->vm, slot)) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld",
                     (long long) scm_get_int(h->vm, slot));
    return RedisModule_CreateString(ctx, buf, (size_t) n);
  }
  if (scm_is_float(h->vm, slot)) {
    std::string s = redis_scheme_ftoa(scm_get_float(h->vm, slot));
    return RedisModule_CreateString(ctx, s.data(), s.size());
  }
  redis_scheme_throw_err(h, "redis-call: expected string or number, got %s",
                         scm_typename(h->vm, slot));
  return NULL; /* unreachable */
}

/* Convert a RedisModuleCallReply into a scheme value.  Error replies throw
   (the message is filled into h->errbuf) so redis-call aborts; redis-pcall
   catches and wraps them.  The caller stores the returned value into a
   rooted slot before the next allocation. */
static ValueT
redis_scheme_parse_reply(redis_scheme_vm_t *h, RedisModuleCallReply *reply)
{
  VM *vm = h->vm;
  switch (RedisModule_CallReplyType(reply)) {
  case REDISMODULE_REPLY_STRING: {
    size_t len = 0;
    const char *s = RedisModule_CallReplyStringPtr(reply, &len);
    Sgcvar1(vm, str);
    *str = scm_make_string(vm, s != NULL ? s : "", (int) len);
    return *str;
  }
  case REDISMODULE_REPLY_BIG_NUMBER: {
    size_t len = 0;
    const char *s = RedisModule_CallReplyBigNumber(reply, &len);
    Sgcvar1(vm, str);
    *str = scm_make_string(vm, s != NULL ? s : "", (int) len);
    return *str;
  }
  case REDISMODULE_REPLY_VERBATIM_STRING: {
    size_t len = 0;
    const char *format = NULL;
    const char *s = RedisModule_CallReplyVerbatim(reply, &len, &format);
    Sgcvar1(vm, str);
    *str = scm_make_string(vm, s != NULL ? s : "", (int) len);
    return *str;
  }
  case REDISMODULE_REPLY_INTEGER: {
    Sgcvar1(vm, i);
    *i = scm_make_int(vm, (scm_int) RedisModule_CallReplyInteger(reply));
    return *i;
  }
  case REDISMODULE_REPLY_DOUBLE: {
    Sgcvar1(vm, f);
    *f = scm_make_float(vm, RedisModule_CallReplyDouble(reply));
    return *f;
  }
  case REDISMODULE_REPLY_BOOL: {
    Sgcvar1(vm, b);
    *b = scm_make_bool(vm, RedisModule_CallReplyBool(reply) != 0);
    return *b;
  }
  case REDISMODULE_REPLY_NULL:
    /* null replies map to #f: falsy like Lua's nil (scripts test with
       (if ...)), and returning it produces a null reply -- the same round
       trip as Lua's nil. */
    {
      Sgcvar1(vm, b);
      *b = scm_make_bool(vm, false);
      return *b;
    }
  case REDISMODULE_REPLY_ERROR: {
    size_t len = 0;
    const char *s = RedisModule_CallReplyStringPtr(reply, &len);
    redis_scheme_throw_err(h, "%.*s", (int) len, s != NULL ? s : "command error");
    return scm_make_bool(vm, false); /* unreachable */
  }
  case REDISMODULE_REPLY_ARRAY:
  case REDISMODULE_REPLY_SET: {
    size_t n = RedisModule_CallReplyLength(reply);
    Sgcvar1(vm, vec);
    *vec = scm_make_vector(vm, (int) n);
    Sgcvar1(vm, elem);
    for (size_t i = 0; i < n; i++) {
      RedisModuleCallReply *sub = RedisModule_CallReplyArrayElement(reply, i);
      *elem = redis_scheme_parse_reply(h, sub);
      scm_vector_set(vm, vec, (int) i, *elem);
    }
    return *vec;
  }
  case REDISMODULE_REPLY_MAP: {
    size_t n = RedisModule_CallReplyLength(reply);
    Sgcvar1(vm, tbl);
    *tbl = scm_make_table(vm);
    Sgcvar2(vm, key, val);
    for (size_t i = 0; i < n; i++) {
      RedisModuleCallReply *rk = NULL, *rv = NULL;
      RedisModule_CallReplyMapElement(reply, i, &rk, &rv);
      *key = redis_scheme_parse_reply(h, rk);
      *val = redis_scheme_parse_reply(h, rv);
      scm_table_set(vm, tbl, key, *val);
    }
    return *tbl;
  }
  default:
    redis_scheme_throw_err(h, "redis-call: unsupported reply type %d",
                           (int) RedisModule_CallReplyType(reply));
    return scm_make_bool(vm, false); /* unreachable */
  }
}

/* redis-call / redis-pcall: run a Redis command from inside the script.
   The first argument is the command name (a string); the rest are its
   arguments (strings, ints and floats, converted to bulk strings).  The
   reply comes back as a scheme value: bulk string -> string, integer ->
   int, double -> float, bool -> #t/#f, null -> #f, array/set -> vector,
   map -> hashtable, big number / verbatim -> string.  redis-call aborts the
   script on an error reply; redis-pcall returns a {err=...} table instead.
   raise selects that error behavior. */
static ValueT
redis_scheme_do_call(VM *vm, ValueT *rest, bool raise)
{
  redis_scheme_vm_t *h = redis_scheme_get_ctx(vm);

  if (scm_is_null(vm, rest)) {
    redis_scheme_throw_err(h,
        "redis-call: please specify at least one argument");
  }
  ValueT *cmd_slot = scm_car(vm, rest);
  if (!scm_is_string(vm, cmd_slot)) {
    redis_scheme_throw_err(h,
        "redis-call: command name must be a string, got %s",
        scm_typename(vm, cmd_slot));
  }
  int cmd_len = 0;
  const char *cmd = scm_get_string(vm, cmd_slot, &cmd_len);
  std::string cmdname(cmd != NULL ? cmd : "", cmd != NULL ? (size_t) cmd_len : 0);

  /* The remaining arguments, converted to bulk strings. */
  std::vector<RedisModuleString *> args;
  ValueT *p = scm_cdr(vm, rest);
  for (;;) {
    if (scm_is_null(vm, p)) {
      break;
    }
    args.push_back(redis_scheme_arg_to_string(h, scm_car(vm, p)));
    p = scm_cdr(vm, p);
  }

  /* Free the argument strings once the call has consumed them ("v" only
     borrows a reference), also on the throw path. */
  struct ArgsGuard {
    RedisModuleCtx *ctx;
    std::vector<RedisModuleString *> *args;
    ~ArgsGuard() {
      for (size_t i = 0; i < args->size(); i++) {
        RedisModule_FreeString(ctx, (*args)[i]);
      }
    }
  } args_guard = { h->ctx, &args };

  /* "v!SE": v = the args vector, ! = replicate (propagate write commands
     to AOF / replicas, like EVAL's effect replication), S = script mode
     (reject deny-script commands), E = dispatch failures come back as
     error replies instead of NULL + errno, so both kinds of failure look
     alike. */
  RedisModuleCallReply *reply = RedisModule_Call(h->ctx, cmdname.c_str(),
                                                 "v!SE", args.data(),
                                                 args.size());
  if (reply == NULL) {
    redis_scheme_throw_err(h, "redis-call: command '%s' failed",
                           cmdname.c_str());
  }

  struct ReplyGuard {
    RedisModuleCallReply *reply;
    ~ReplyGuard() {
      if (reply != NULL) {
        RedisModule_FreeCallReply(reply);
      }
    }
  } reply_guard = { reply };

  Sgcvar1(vm, out);
  try {
    *out = redis_scheme_parse_reply(h, reply);
  }
  catch (...) {
    if (!raise) {
      /* redis-pcall: turn the error message into a {err=...} table, the
         same shape redis-error-reply builds. */
      const char *m = (h->errbuf != NULL && h->errbuf[0] != '\0')
                        ? h->errbuf : "command error";
      *out = redis_scheme_err_table(vm, m, (int) strlen(m));
    } else {
      throw; /* redis-call: abort the script with the command error */
    }
  }
  return *out;
}

static ValueT
redis_scheme_stub_call(VM* vm, ValueT* rest)
{
  return redis_scheme_do_call(vm, rest, true);
}

static ValueT
redis_scheme_stub_pcall(VM* vm, ValueT* rest)
{
  return redis_scheme_do_call(vm, rest, false);
}

/* Registration table; keep the (NULL, -1) sentinel last. */
static const RegCProc redis_scheme_lib[] = {
  RegCProc("redis-error-reply", redis_scheme_stub_error_reply),
  RegCProc("redis-status-reply", redis_scheme_stub_status_reply),
  RegCProc("redis-call", redis_scheme_stub_call, true),
  RegCProc("redis-pcall", redis_scheme_stub_pcall, true),
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

/* Allocator handed to the host VM, so every scheme object lives in Redis
   memory (zmalloc, counted by INFO memory and OOM-checked), the same
   arrangement as Lua.  ScmAlloc follows lua_Alloc's shape: nsize==0 frees,
   otherwise (re)allocates. */
static void *
redis_scheme_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)ud;
  (void)osize;
  if (nsize == 0) {
    RedisModule_Free(ptr);
    return NULL;
  }
  return RedisModule_Realloc(ptr, nsize);
}

redis_scheme_vm_t *
redis_scheme_create(RedisModuleCtx *ctx)
{
  redis_scheme_vm_t *h = (redis_scheme_vm_t *) malloc(sizeof(*h));
  if (h == NULL) {
    return NULL;
  }

  h->vm = NULL;
  h->ctx = ctx;
  h->errbuf = NULL;
  h->errcap = 0;

  try {
    h->vm = scm_create_vm(redis_scheme_alloc, NULL);
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

/* Convert one scheme value into a Redis reply: numbers (int and float)
   reply as integers, strings as bulk, #t as integer 1, #f as null, proper
   lists and vectors as arrays with the elements converted recursively.
   Anything else (plain hashtables, symbols, chars, dotted pairs, ...) keeps
   the write-rendering path.  slot must be rooted for the duration of the
   call (the recursion allocates, which may collect). */
static void
redis_scheme_reply_value(VM *vm, ValueT *slot, RedisModuleCtx *ctx)
{
  if (scm_is_int(vm, slot)) {
    RedisModule_ReplyWithLongLong(ctx, (long long) scm_get_int(vm, slot));
    return;
  }
  if (scm_is_float(vm, slot)) {
    /* floats reply as long long, truncated */
    RedisModule_ReplyWithLongLong(ctx, (long long) scm_get_float(vm, slot));
    return;
  }
  if (scm_is_string(vm, slot)) {
    int n = 0;
    const char *s = scm_get_string(vm, slot, &n);
    RedisModule_ReplyWithStringBuffer(ctx, s, (size_t) n);
    return;
  }
  if (scm_is_true(vm, slot)) {
    RedisModule_ReplyWithLongLong(ctx, 1);
    return;
  }
  if (scm_is_false(vm, slot)) {
    RedisModule_ReplyWithNull(ctx);
    return;
  }
  if (scm_is_list(vm, slot)) {
    /* scm_is_list counts any pair, so the length check doubles as the
       proper-list test: scm_proper_list_len returns -1 for a dotted pair,
       which keeps the write-rendering path below (scm_list_len would
       throw). */
    int len = scm_proper_list_len(vm, slot);
    if (len >= 0) {
      RedisModule_ReplyWithArray(ctx, len);
      for (int i = 0; i < len; i++) {
        /* element slot lives inside the rooted list and stays valid across
           the recursive conversion's allocations */
        redis_scheme_reply_value(vm, scm_list_ref(vm, slot, i), ctx);
      }
      return;
    }
  } else if (scm_is_vector(vm, slot)) {
    int len = scm_vector_len(vm, slot);
    RedisModule_ReplyWithArray(ctx, len);
    Sgcvar1(vm, elem);
    for (int i = 0; i < len; i++) {
      *elem = scm_vector_ref(vm, slot, i);
      redis_scheme_reply_value(vm, elem, ctx);
    }
    return;
  }

  /* Fallback: write-rendering (plain hashtables, symbols, chars, ...).  The
     pointer is only valid until the next scm_tostring call; consume it
     synchronously. */
  int n = 0;
  const char *s = scm_tostring(vm, slot, &n);
  if (s == NULL) {
    RedisModule_ReplyWithNull(ctx);
    return;
  }
  RedisModule_ReplyWithStringBuffer(ctx, s, (size_t) n);
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

  /* Typed conversion (numbers/strings/bools/lists/vectors); anything else is
     write-rendered inside.  last is registry-rooted, so the value survives
     the allocations of the conversion. */
  try {
    redis_scheme_reply_value(h->vm, &last, ctx);
  }
  catch (...) {
    const char *msg = (h->errbuf != NULL && h->errbuf[0] != '\0')
                        ? h->errbuf : "cannot render scheme value";
    RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", msg);
  }
}
