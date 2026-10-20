/*
 * redis_scheme_runtime.cpp -- C bridge over the dummyscheme VM.
 *
 * The VM is a process-lifetime singleton.  Each cached script compiles into a
 * closure bound to a fresh sandbox env (hashtable with metatable {__index =
 * genv}), and lives in the VM registry.  Per request we only bind KEYS/ARGV on
 * genv and apply the cached closure.
 */
#include "vm.h"
#include "redis_scheme_runtime.h"

extern "C" {
#include "sha1.h"  /* vendored from redis: SHA-1 digest (RFC 3174) */
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <list>
#include <unordered_map>
#include <vector>
#include <charconv>
#include <climits>

using namespace Scheme;

/* Registry keys */
#define REDIS_SCHEME_CTX_KEY "~redis-scheme-ctx"
#define REDIS_SCHEME_LAST_KEY "~redis-scheme-last"
/* Registry slot prefix for compiled script closures; the full key is
 * "~script:<sha1hex>" (40 hex chars). */
#define REDIS_SCHEME_SCRIPT_KEY_PREFIX "~script:"

/* Log levels for redis-log, matching Redis server.h's LL_DEBUG..LL_WARNING
 * (0..3).  These are the values exposed to scripts as redis-log-* constants
 * and used for input validation. */
enum {
  REDIS_SCHEME_LOG_DEBUG = 0,
  REDIS_SCHEME_LOG_VERBOSE = 1,
  REDIS_SCHEME_LOG_NOTICE = 2,
  REDIS_SCHEME_LOG_WARNING = 3
};

/* Replication propagation flags, matching Redis server.h's PROPAGATE_*:
 * NONE=0, AOF=1, REPL=2.  The "!" flag in RedisModule_Call enables both
 * AOF and REPL together; there is no per-channel control in the module API. */
enum {
  REDIS_SCHEME_PROPAGATE_NONE = 0,
  REDIS_SCHEME_PROPAGATE_AOF = 1,
  REDIS_SCHEME_PROPAGATE_REPL = 2
};

struct redis_scheme_vm_s {
  VM *vm;               /* interpreter (created once by runtime_init) */
  RedisModuleCtx *ctx;  /* ctx of the current command (set before apply) */
  char *errbuf;         /* most recent error message (owned) */
  int   errcap;
  int   repl_flags;     /* propagation flags for redis-call (reset per request) */
};

typedef struct redis_scheme_vm_s redis_scheme_vm_t;

/* The long-lived VM handle.  Created in redis_scheme_runtime_init, never
 * recreated per request. */
static redis_scheme_vm_t *g_vm = NULL;

/* ---------------------------------------------------------------------------
 * Error plumbing shared by host functions and the run path.
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

/* Fill the handle's error buffer and throw it.  The apply wrapper prefers
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
    const char *s = RedisModule_CallReplyVerbatim(reply, &len, NULL);
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
    /* null replies map to #f: scripts test with (if ...), and returning
       it produces a null reply. */
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
   map -> hashtable, big number / verbatim -> string.  redis-call aborts
   the script on an error reply; redis-pcall returns a {err=...} table
   instead.  raise selects that error behavior. */
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

  /* "vSE" always; "!" enables replication of write commands to AOF/replicas.
     redis-set-repl can turn it off per-script.  S = script mode (reject
     deny-script commands), E = dispatch failures come back as error replies. */
  const char *fmt = (h->repl_flags != REDIS_SCHEME_PROPAGATE_NONE)
                      ? "v!SE" : "vSE";
  RedisModuleCallReply *reply = RedisModule_Call(h->ctx, cmdname.c_str(),
                                                 fmt, args.data(),
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

/* SHA-1 of a buffer as 40 lowercase hex chars + NUL, shared by the
   redis-sha1hex stub and the script cache. */
static void
redis_scheme_sha1hex_buf(const char *data, size_t len, char out[41])
{
  SHA1_CTX ctx;
  unsigned char digest[20];
  SHA1Init(&ctx);
  SHA1Update(&ctx, (const unsigned char *) data, (uint32_t) len);
  SHA1Final(digest, &ctx);

  static const char hexdig[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    out[2 * i] = hexdig[digest[i] >> 4];
    out[2 * i + 1] = hexdig[digest[i] & 0x0f];
  }
  out[40] = '\0';
}

/* redis-sha1hex: the SHA-1 digest of one argument as 40 lowercase hex
   characters.  Strings hash their bytes; ints and floats hash their
   decimal text form; any other type is an error. */
static ValueT
redis_scheme_stub_sha1hex(VM* vm, ValueT* arg)
{
  const char *data = NULL;
  int len = 0;
  std::string numtext;
  if (scm_is_string(vm, arg)) {
    data = scm_get_string(vm, arg, &len);
  } else if (scm_is_int(vm, arg)) {
    char buf[32];
    len = snprintf(buf, sizeof(buf), "%lld",
                   (long long) scm_get_int(vm, arg));
    numtext.assign(buf, (size_t) len);
    data = numtext.data();
  } else if (scm_is_float(vm, arg)) {
    numtext = redis_scheme_ftoa(scm_get_float(vm, arg));
    data = numtext.data();
    len = (int) numtext.size();
  } else {
    redis_scheme_throw_err(redis_scheme_get_ctx(vm),
        "redis-sha1hex: expected string or number, got %s",
        scm_typename(vm, arg));
    return scm_make_bool(vm, false); /* unreachable */
  }

  char hex[41];
  redis_scheme_sha1hex_buf(data != NULL ? data : "", (size_t) len, hex);
  Sgcvar1(vm, slot);
  *slot = scm_make_string(vm, hex, 40);
  return *slot;
}

/* redis-log: write a message to the Redis log.  Usage:
 *   (redis-log LEVEL FRAG ...)
 * LEVEL is one of the redis-log-debug/verbose/notice/warning constants.
 * Remaining fragments are joined with spaces; only strings and numbers
 * contribute, other types are skipped. */
static ValueT
redis_scheme_stub_log(VM* vm, ValueT* rest)
{
  redis_scheme_vm_t *h = redis_scheme_get_ctx(vm);
  if (scm_is_null(vm, rest)) {
    redis_scheme_throw_err(h,
        "redis-log: expected level followed by message fragments");
  }
  ValueT *level_slot = scm_car(vm, rest);
  if (!scm_is_int(vm, level_slot)) {
    redis_scheme_throw_err(h,
        "redis-log: level must be an integer, got %s",
        scm_typename(vm, level_slot));
  }
  int lvl = (int) scm_get_int(vm, level_slot);
  if (lvl < REDIS_SCHEME_LOG_DEBUG || lvl > REDIS_SCHEME_LOG_WARNING) {
    redis_scheme_throw_err(h,
        "redis-log: invalid level %d (expected %d-%d: debug/verbose/notice/warning)",
        lvl, REDIS_SCHEME_LOG_DEBUG, REDIS_SCHEME_LOG_WARNING);
  }

  /* Render each fragment and join with spaces.  Only strings and numbers
     contribute; other types are skipped. */
  std::string msg;
  bool first = true;
  for (ValueT *p = scm_cdr(vm, rest); !scm_is_null(vm, p); p = scm_cdr(vm, p)) {
    ValueT *item = scm_car(vm, p);
    std::string frag;
    if (scm_is_string(vm, item)) {
      int n = 0;
      const char *s = scm_get_string(vm, item, &n);
      frag.assign(s != NULL ? s : "", (size_t) n);
    } else if (scm_is_int(vm, item)) {
      char tmp[32];
      int n = snprintf(tmp, sizeof(tmp), "%lld", (long long) scm_get_int(vm, item));
      frag.assign(tmp, (size_t) n);
    } else if (scm_is_float(vm, item)) {
      frag = redis_scheme_ftoa(scm_get_float(vm, item));
    } else {
      continue; /* skip non-scalar fragments */
    }
    if (!first) msg += ' ';
    msg += frag;
    first = false;
  }

  static const char *level_names[] = {
    "debug", "verbose", "notice", "warning"
  };
  RedisModule_Log(h->ctx, level_names[lvl], "%s", msg.c_str());
  return_void();
}

/* redis-set-repl: set propagation flags for subsequent redis-call in this
   script invocation.  Argument is a bitmask of redis-repl-none/aof/replica/all. */
static ValueT
redis_scheme_stub_set_repl(VM* vm, ValueT* flags)
{
  redis_scheme_vm_t *h = redis_scheme_get_ctx(vm);
  if (!scm_is_int(vm, flags)) {
    redis_scheme_throw_err(h,
        "redis-set-repl: flags must be an integer, got %s",
        scm_typename(vm, flags));
  }
  int f = (int) scm_get_int(vm, flags);
  if (f & ~(REDIS_SCHEME_PROPAGATE_AOF | REDIS_SCHEME_PROPAGATE_REPL)) {
    redis_scheme_throw_err(h,
        "redis-set-repl: invalid flags %d (use redis-repl-none/aof/replica/all)", f);
  }
  h->repl_flags = f;
  return_void();
}

/* ---------------------------------------------------------------------------
 * Script cache: sha1 hex -> LRU metadata.  The compiled closure itself
 * lives in the VM registry under "~script:<sha1>", so GC roots it (and
 * transitively its sandbox env, lambda, constants).  The C++ map only
 * tracks presence and eviction order.  Single-threaded command execution,
 * no locking.
 * ------------------------------------------------------------------------ */
#define REDIS_SCHEME_SCRIPT_LRU_MAX 500

struct redis_scheme_cache_entry {
  int evictable;                         /* EVAL-sourced entries only */
  std::list<std::string>::iterator lru;  /* valid only when evictable */
};

typedef std::unordered_map<std::string, redis_scheme_cache_entry>
    redis_scheme_script_cache_t;

static redis_scheme_script_cache_t redis_scheme_script_cache;
static std::list<std::string> redis_scheme_script_lru; /* front = least used */

/* Move an evictable entry to the most-recently-used end. */
static void
redis_scheme_cache_touch(redis_scheme_script_cache_t::iterator it)
{
  if (!it->second.evictable) return;
  redis_scheme_script_lru.erase(it->second.lru);
  redis_scheme_script_lru.push_back(it->first);
  it->second.lru = --redis_scheme_script_lru.end();
}

/* Build the registry slot name "~script:<sha1>". */
static std::string
redis_scheme_script_regkey(const char *sha1)
{
  return std::string(REDIS_SCHEME_SCRIPT_KEY_PREFIX) + sha1;
}

/* ---------------------------------------------------------------------------
 * VM lifecycle
 * ------------------------------------------------------------------------ */

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

/* Registration table; keep the (NULL, -1) sentinel last. */
static const RegCProc redis_scheme_lib[] = {
  RegCProc("redis-error-reply", redis_scheme_stub_error_reply),
  RegCProc("redis-status-reply", redis_scheme_stub_status_reply),
  RegCProc("redis-call", redis_scheme_stub_call, true),
  RegCProc("redis-pcall", redis_scheme_stub_pcall, true),
  RegCProc("redis-sha1hex", redis_scheme_stub_sha1hex),
  RegCProc("redis-log", redis_scheme_stub_log, true),
  RegCProc("redis-set-repl", redis_scheme_stub_set_repl),
  RegCProc(NULL, -1)               /* end sentinel */
};

/* Strip file/IO builtins from genv so scripts cannot read or write the
   server filesystem.  Each name is overwritten with #f; calling it from a
   script raises "not a procedure". */
static void
redis_scheme_sandbox_strip(VM *vm)
{
  static const char *unsafe_names[] = {
    /* file ports */
    "open-input-file", "open-output-file",
    "close-input-port", "close-output-port",
    /* IO operations */
    "read", "read-char", "peek-char",
    "write", "write-char", "display", "newline",
    /* port management */
    "flush-output", "flush-output-port",
    "current-input-port", "current-output-port",
    /* file loading */
    "load",
    /* port type predicates */
    "input-port?", "output-port?", "port?",
    "eof-object?", "char-ready?",
    NULL
  };
  for (int i = 0; unsafe_names[i] != NULL; i++) {
    scm_set_global(vm, unsafe_names[i], scm_make_bool(vm, false));
  }
}

/* Release g_vm and its VM.  Used on init failure and on runtime_done. */
static void
redis_scheme_free_gvm(void)
{
  if (g_vm == NULL) return;
  if (g_vm->vm != NULL) {
    scm_destroy_vm(g_vm->vm);
  }
  free(g_vm->errbuf);
  free(g_vm);
  g_vm = NULL;
}

int
redis_scheme_runtime_init(void)
{
  if (g_vm != NULL) {
    return 0;
  }
  g_vm = (redis_scheme_vm_t *) calloc(1, sizeof(*g_vm));
  if (g_vm == NULL) {
    return -1;
  }
  g_vm->ctx = NULL;
  g_vm->errbuf = NULL;
  g_vm->errcap = 0;
  g_vm->repl_flags = REDIS_SCHEME_PROPAGATE_AOF | REDIS_SCHEME_PROPAGATE_REPL;

  try {
    g_vm->vm = scm_create_vm(redis_scheme_alloc, NULL);
    if (g_vm->vm == NULL) {
      redis_scheme_free_gvm();
      return -1;
    }
    /* Store the handle in the registry so host functions can find their
       context.  The lightuserdata outlives every command (g_vm is a
       singleton); host functions read g_vm->ctx for the current command. */
    scm_registry_set(g_vm->vm, REDIS_SCHEME_CTX_KEY,
        scm_make_lightuserdata(g_vm->vm, g_vm));
    scm_register_lib(g_vm->vm, redis_scheme_lib);

    /* Log-level constants for redis-log, matching Redis's LL_* values. */
    scm_set_global(g_vm->vm, "redis-log-debug",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_LOG_DEBUG));
    scm_set_global(g_vm->vm, "redis-log-verbose",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_LOG_VERBOSE));
    scm_set_global(g_vm->vm, "redis-log-notice",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_LOG_NOTICE));
    scm_set_global(g_vm->vm, "redis-log-warning",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_LOG_WARNING));

    /* Server version constants.  GetServerVersion returns the packed
     * hex-format number (major<<16 | minor<<8 | patch), same as
     * REDIS_VERSION_NUM. */
    int ver = RedisModule_GetServerVersion();
    scm_set_global(g_vm->vm, "redis-version-num", scm_make_int(g_vm->vm, ver));
    char ver_str[32] = {0};
    int ver_len = snprintf(ver_str, sizeof(ver_str), "%d.%d.%d",
                           (ver >> 16) & 0xff, (ver >> 8) & 0xff, ver & 0xff);
    scm_set_global(g_vm->vm, "redis-version",
                   scm_make_string(g_vm->vm, ver_str, ver_len));

    /* Replication propagation constants for redis-set-repl. */
    scm_set_global(g_vm->vm, "redis-repl-none",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_PROPAGATE_NONE));
    scm_set_global(g_vm->vm, "redis-repl-aof",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_PROPAGATE_AOF));
    scm_set_global(g_vm->vm, "redis-repl-replica",
                   scm_make_int(g_vm->vm, REDIS_SCHEME_PROPAGATE_REPL));
    scm_set_global(g_vm->vm, "redis-repl-all",
                   scm_make_int(g_vm->vm,
                       REDIS_SCHEME_PROPAGATE_AOF | REDIS_SCHEME_PROPAGATE_REPL));

    /* Strip file/IO builtins from genv. */
    redis_scheme_sandbox_strip(g_vm->vm);
  }
  catch (...) {
    redis_scheme_free_gvm();
    return -1;
  }
  return 0;
}

void
redis_scheme_runtime_done(void)
{
  redis_scheme_script_cache.clear();
  redis_scheme_script_lru.clear();
  redis_scheme_free_gvm();
}

/* ---------------------------------------------------------------------------
 * Script cache API
 * ------------------------------------------------------------------------ */

int
redis_scheme_cache_script(RedisModuleCtx *ctx, const char *body,
                          size_t body_len, char out_sha1[41], int evictable)
{
  redis_scheme_sha1hex_buf(body, body_len, out_sha1);

  redis_scheme_script_cache_t::iterator it =
      redis_scheme_script_cache.find(out_sha1);
  if (it != redis_scheme_script_cache.end()) {
    redis_scheme_cache_touch(it);
    return 0;
  }

  VM *vm = g_vm->vm;
  try {
    /* Build the sandbox env: a fresh hashtable.  scm_compile installs
       {__index = genv} on it and compiles the script as a closure whose
       outers[0] is this env.  Top-level define/set! write into the sandbox;
       reads miss the sandbox and fall through to genv (builtins, redis-*,
       KEYS/ARGV). */
    Sgcvar1(vm, env_slot);
    *env_slot = scm_make_table(vm);

    Sgcvar1(vm, cl_slot);
    *cl_slot = scm_compile(vm, env_slot, body, (int) body_len);

    /* Root the closure in the registry under "~script:<sha1>". */
    std::string regkey = redis_scheme_script_regkey(out_sha1);
    scm_registry_set(vm, regkey.c_str(), *cl_slot);

    /* LRU eviction of EVAL-sourced entries. */
    if (evictable) {
      while (redis_scheme_script_lru.size() >= REDIS_SCHEME_SCRIPT_LRU_MAX) {
        const std::string &victim = redis_scheme_script_lru.front();
        std::string vkey = redis_scheme_script_regkey(victim.c_str());
        scm_registry_unset(vm, vkey.c_str());
        redis_scheme_script_cache.erase(victim);
        redis_scheme_script_lru.pop_front();
      }
    }

    redis_scheme_cache_entry &e = redis_scheme_script_cache[out_sha1];
    e.evictable = evictable;
    if (evictable) {
      redis_scheme_script_lru.push_back(out_sha1);
      e.lru = --redis_scheme_script_lru.end();
    }
  }
  catch (const char *e) {
    const char *msg = (g_vm->errbuf != NULL && g_vm->errbuf[0] != '\0')
                        ? g_vm->errbuf : (e != NULL ? e : "compile error");
    RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", msg);
    return -1;
  }
  catch (...) {
    RedisModule_ReplyWithError(ctx, "ERR scheme: compile error");
    return -1;
  }
  return 0;
}

int
redis_scheme_cache_has(const char *sha1hex)
{
  return redis_scheme_script_cache.count(sha1hex) != 0;
}

void
redis_scheme_cache_flush(void)
{
  /* Unroot every closure from the registry so the collector can reclaim
     the closure, its lambda, its constants, and the sandbox env. */
  for (auto &kv : redis_scheme_script_cache) {
    std::string regkey = redis_scheme_script_regkey(kv.first.c_str());
    scm_registry_unset(g_vm->vm, regkey.c_str());
  }
  redis_scheme_script_cache.clear();
  redis_scheme_script_lru.clear();
}

/* ---------------------------------------------------------------------------
 * Reply rendering (used by the run path; reads g_vm directly)
 * ------------------------------------------------------------------------ */

/* Detect a tagged reply value: a hashtable whose "err" field is a string
   (error reply) or whose "ok" field is a string (status reply); "err" is
   checked first.  Returns 1 for an error reply, 2 for a status reply, 0
   when the value is anything else (which keeps the write-rendering path). */
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
   Anything else (plain hashtables, symbols, chars, dotted pairs, ...)
   keeps the write-rendering path. */
static void
redis_scheme_reply_value(VM *vm, ValueT *slot, RedisModuleCtx *ctx)
{
  if (scm_is_int(vm, slot)) {
    RedisModule_ReplyWithLongLong(ctx, (long long) scm_get_int(vm, slot));
    return;
  }
  if (scm_is_float(vm, slot)) {
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
    int len = scm_proper_list_len(vm, slot);
    if (len >= 0) {
      RedisModule_ReplyWithArray(ctx, len);
      for (int i = 0; i < len; i++) {
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

  /* Fallback: write-rendering (plain hashtables, symbols, chars, ...). */
  int n = 0;
  const char *s = scm_tostring(vm, slot, &n);
  if (s == NULL) {
    RedisModule_ReplyWithNull(ctx);
    return;
  }
  RedisModule_ReplyWithStringBuffer(ctx, s, (size_t) n);
}

/* ---------------------------------------------------------------------------
 * Run path
 * ------------------------------------------------------------------------ */

/* Bind KEYS/ARGV on genv.  The cached closure's sandbox env has
   {__index = genv}, so the script sees KEYS/ARGV through the metatable.
   argv follows the EVAL shape. */
static int
redis_scheme_bind_args(RedisModuleCtx *ctx, RedisModuleString **argv,
                       int argc, long long numkeys)
{
  (void) ctx;
  VM *vm = g_vm->vm;
  int nkeys = (int) numkeys;
  int nargs = argc - 3 - nkeys;

  Sgcvar1(vm, keys_slot);
  *keys_slot = scm_make_vector(vm, nkeys);
  for (int i = 0; i < nkeys; i++) {
    size_t len = 0;
    const char *p = RedisModule_StringPtrLen(argv[3 + i], &len);
    scm_vector_set(vm, keys_slot, i, scm_make_string(vm, p, (int) len));
  }
  scm_set_global(vm, "KEYS", *keys_slot);

  Sgcvar1(vm, args_slot);
  *args_slot = scm_make_vector(vm, nargs);
  for (int i = 0; i < nargs; i++) {
    size_t len = 0;
    const char *p = RedisModule_StringPtrLen(argv[3 + nkeys + i], &len);
    scm_vector_set(vm, args_slot, i, scm_make_string(vm, p, (int) len));
  }
  scm_set_global(vm, "ARGV", *args_slot);
  return 0;
}

int
redis_scheme_run(RedisModuleCtx *ctx, const char *body, size_t body_len,
                 RedisModuleString **argv, int argc, long long numkeys)
{
  char sha1[41];
  if (redis_scheme_cache_script(ctx, body, body_len, sha1, 1) != 0) {
    return -1;
  }
  return redis_scheme_run_cached(ctx, sha1, argv, argc, numkeys);
}

int
redis_scheme_run_cached(RedisModuleCtx *ctx, const char *sha1hex,
                        RedisModuleString **argv, int argc, long long numkeys)
{
  redis_scheme_script_cache_t::iterator it =
      redis_scheme_script_cache.find(sha1hex);
  if (it == redis_scheme_script_cache.end()) {
    RedisModule_ReplyWithError(ctx,
        "NOSCRIPT no matching script; use SCHEME.EVAL");
    return -1;
  }
  redis_scheme_cache_touch(it);

  VM *vm = g_vm->vm;
  g_vm->ctx = ctx;
  if (g_vm->errbuf != NULL) {
    g_vm->errbuf[0] = '\0';
  }
  g_vm->repl_flags = REDIS_SCHEME_PROPAGATE_AOF | REDIS_SCHEME_PROPAGATE_REPL;

  /* Bind KEYS/ARGV on genv for this request. */
  try {
    redis_scheme_bind_args(ctx, argv, argc, numkeys);
  }
  catch (const char *e) {
    const char *m = (g_vm->errbuf != NULL && g_vm->errbuf[0] != '\0')
                      ? g_vm->errbuf : (e != NULL ? e : "bind args error");
    RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", m);
    return -1;
  }
  catch (...) {
    RedisModule_ReplyWithError(ctx, "ERR scheme: bind args error");
    return -1;
  }

  /* Seed the RNG from the script sha1 so replicas replay the same random
     sequence as the master.  The first 16 hex chars give a 64-bit seed. */
  scm_uint seed = 0;
  for (int i = 0; i < 16; i++) {
    char c = sha1hex[i];
    int v = (c >= '0' && c <= '9') ? (c - '0') : (c - 'a' + 10);
    seed = (seed << 4) | (scm_uint) v;
  }
  G(vm)->randstate = seed;

  /* Pull the compiled closure out of the registry and apply it.  The
     registry itself is GC-rooted, so the closure needs no extra slot;
     out_slot must be rooted because it holds scm_apply's return value
     across the subsequent reply rendering. */
  std::string regkey = redis_scheme_script_regkey(sha1hex);
  ValueT closure = scm_registry_get(vm, regkey.c_str());
  Sgcvar1(vm, out_slot);
  bool ok = scm_apply(vm, &closure, NULL, 0, out_slot);
  if (!ok) {
    /* out_slot holds the error string (pcall CATCH path interns it). */
    const char *m = "script error";
    int mlen = 0;
    if (scm_is_string(vm, out_slot)) {
      m = scm_get_string(vm, out_slot, &mlen);
    } else if (g_vm->errbuf != NULL && g_vm->errbuf[0] != '\0') {
      m = g_vm->errbuf;
      mlen = (int) strlen(m);
    }
    std::string msg(m != NULL ? m : "", (size_t) (mlen > 0 ? mlen : strlen(m != NULL ? m : "")));
    RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", msg.c_str());
    return -1;
  }

  /* Success: root the result as the last value, then reply. */
  scm_registry_set(vm, REDIS_SCHEME_LAST_KEY, *out_slot);

  int tag = redis_tagged_reply(vm, out_slot);
  if (tag != 0) {
    Sgcvar1(vm, key_slot);
    const char *key_name = (tag == 1) ? "err" : "ok";
    int key_len = (tag == 1) ? 3 : 2;
    *key_slot = scm_make_string(vm, key_name, key_len);
    ValueT payload = scm_table_ref(vm, out_slot, key_slot);
    int n = 0;
    const char *s = scm_get_string(vm, &payload, &n);
    std::string body(s != NULL ? s : "", s != NULL ? (size_t) n : 0);
    if (tag == 1) {
      RedisModule_ReplyWithError(ctx, body.c_str());
    } else {
      RedisModule_ReplyWithSimpleString(ctx, body.c_str());
    }
    return 0;
  }

  try {
    redis_scheme_reply_value(vm, out_slot, ctx);
  }
  catch (...) {
    const char *msg = (g_vm->errbuf != NULL && g_vm->errbuf[0] != '\0')
                        ? g_vm->errbuf : "cannot render scheme value";
    RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", msg);
  }
  return 0;
}
