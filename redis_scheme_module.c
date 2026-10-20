/*
 * redis_scheme_module.c -- Redis module for dummyscheme.
 *
 *
 * SCHEME.EVAL / SCHEME.EVALSHA run a scheme script on a fresh VM (the
 * execution flow lives in redis_scheme_runtime), SCHEME.SCRIPT manages the
 * server-side script cache.  The result is replied as a typed value.
 *
 */
#include <stdio.h>
#include <string.h>
#include "redismodule.h"
#include "redis_scheme_runtime.h"

/* ---------------------------------------------------------------------------
 * EVAL command handlers
 * ------------------------------------------------------------------------ */

/* Validate the argument shape shared by SCHEME.EVAL and SCHEME.EVALSHA:
 * script-or-sha1 numkeys key [key ...] arg [...].  numkeys must be a
 * non-negative integer and argv must hold at least 3 + numkeys entries,
 * because argv[3] .. argv[3+numkeys-1] are the keys bound to the KEYS
 * global before evaluation.  On a bad shape the error reply is written
 * and -1 is returned.  cmdname appears verbatim in the arity error. */
static int parse_eval_shape(RedisModuleCtx *ctx, const char *cmdname,
                            RedisModuleString **argv, int argc,
                            long long *numkeys)
{
  if (argc < 3) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "ERR wrong number of arguments for '%s' command", cmdname);
    RedisModule_ReplyWithError(ctx, msg);
    return -1;
  }

  if (RedisModule_StringToLongLong(argv[2], numkeys) != REDISMODULE_OK
      || *numkeys < 0) {
    RedisModule_ReplyWithError(ctx,
        "ERR value is not an integer or out of range");
    return -1;
  }
  if (argc < 3 + *numkeys) {
    RedisModule_ReplyWithError(ctx,
        "ERR Number of keys can't be greater than number of args");
    return -1;
  }
  return 0;
}

/* SCHEME.EVAL script numkeys key [key ...] arg [...] */
static int scheme_eval_command(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
  long long numkeys = 0;
  if (parse_eval_shape(ctx, "scheme.eval", argv, argc, &numkeys) != 0) {
    return REDISMODULE_ERR;
  }

  /* Script bytes: argv[1].  redis_scheme_run stores the script in the
     cache (sha1 hex -> body) before running it. */
  size_t script_len = 0;
  const char *script = RedisModule_StringPtrLen(argv[1], &script_len);
  redis_scheme_run(ctx, script, script_len, argv, argc, numkeys);
  return REDISMODULE_OK;
}

/* SCHEME.EVALSHA sha1 numkeys key [key ...] arg [...] */
static int scheme_evalsha_command(RedisModuleCtx *ctx,
                                  RedisModuleString **argv, int argc)
{
  long long numkeys = 0;
  if (parse_eval_shape(ctx, "scheme.evalsha", argv, argc, &numkeys) != 0) {
    return REDISMODULE_ERR;
  }

  /* sha1 hex: argv[1].  A miss replies NOSCRIPT, as with EVALSHA. */
  size_t sha1_len = 0;
  const char *sha1 = RedisModule_StringPtrLen(argv[1], &sha1_len);
  redis_scheme_run_cached(ctx, sha1, argv, argc, numkeys);
  return REDISMODULE_OK;
}

/* ---------------------------------------------------------------------------
 * SCHEME.SCRIPT subcommands
 * ------------------------------------------------------------------------ */

/* SCHEME.SCRIPT LOAD body -> bulk sha1 */
static int script_load_command(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
  if (argc != 3) {
    return RedisModule_ReplyWithError(ctx,
        "ERR wrong number of arguments for 'scheme.script|load' command");
  }
  size_t body_len = 0;
  const char *body = RedisModule_StringPtrLen(argv[2], &body_len);
  char sha1[41];
  /* evictable=0: LOAD-sourced entries are never evicted.  The cache is
     node-local, so LOAD is not propagated to replicas. */
  if (redis_scheme_cache_script(ctx, body, body_len, sha1, 0) != 0) {
    return REDISMODULE_ERR;
  }
  return RedisModule_ReplyWithStringBuffer(ctx, sha1, 40);
}

/* SCHEME.SCRIPT EXISTS sha1 [sha1 ...] -> array of 1/0 */
static int script_exists_command(RedisModuleCtx *ctx,
                                 RedisModuleString **argv, int argc)
{
  if (argc < 3) {
    return RedisModule_ReplyWithError(ctx,
        "ERR wrong number of arguments for 'scheme.script|exists' command");
  }
  RedisModule_ReplyWithArray(ctx, argc - 2);
  for (int i = 2; i < argc; i++) {
    size_t sha1_len = 0;
    const char *sha1 = RedisModule_StringPtrLen(argv[i], &sha1_len);
    RedisModule_ReplyWithLongLong(ctx, redis_scheme_cache_has(sha1));
  }
  return REDISMODULE_OK;
}

/* SCHEME.SCRIPT FLUSH -> OK */
static int script_flush_command(RedisModuleCtx *ctx,
                                RedisModuleString **argv, int argc)
{
  if (argc != 2) {
    return RedisModule_ReplyWithError(ctx,
        "ERR wrong number of arguments for 'scheme.script|flush' command");
  }
  redis_scheme_cache_flush();
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int scheme_script_command(RedisModuleCtx *ctx,
                                 RedisModuleString **argv, int argc)
{
  if (argc < 2) {
    return RedisModule_ReplyWithError(ctx,
        "ERR wrong number of arguments for 'scheme.script' command");
  }

  size_t sub_len = 0;
  const char *sub = RedisModule_StringPtrLen(argv[1], &sub_len);

  if (sub_len == 4 && strncasecmp(sub, "load", 4) == 0) {
    return script_load_command(ctx, argv, argc);
  }
  if (sub_len == 6 && strncasecmp(sub, "exists", 6) == 0) {
    return script_exists_command(ctx, argv, argc);
  }
  if (sub_len == 5 && strncasecmp(sub, "flush", 5) == 0) {
    return script_flush_command(ctx, argv, argc);
  }
  return RedisModule_ReplyWithError(ctx,
      "ERR unknown subcommand for 'scheme.script'");
}

/* ---------------------------------------------------------------------------
 * Command registration
 * ------------------------------------------------------------------------ */

/* Key spec for SCHEME.EVAL / SCHEME.EVALSHA, same shape as EVAL: script
   numkeys key [key ...] arg [...].  Search starts at argv[2] (numkeys),
   the key count is read there, and the keys begin at argv[3] with a step
   of 1.  Flags are the conservative RW/ACCESS/UPDATE, as in EVAL.  The
   array ends with a zeroed sentinel (begin_search_type = BS_INVALID). */
static RedisModuleCommandKeySpec scheme_eval_key_specs[] = {
    {
        .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
        .bs.index.pos = 2,
        .find_keys_type = REDISMODULE_KSPEC_FK_KEYNUM,
        .fk.keynum.keynumidx = 0,
        .fk.keynum.firstkey = 1,
        .fk.keynum.keystep = 1,
        .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_ACCESS
                 | REDISMODULE_CMD_KEY_UPDATE,
    },
    { 0 },
};

static int register_eval_command(RedisModuleCtx *ctx)
{
  if (RedisModule_CreateCommand(ctx, "scheme.eval", scheme_eval_command,
                                "write deny-oom deny-script", 0, 0, 0)
      == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "could not create command scheme.eval");
    return REDISMODULE_ERR;
  }

  RedisModuleCommandInfo info = {
      .version = REDISMODULE_COMMAND_INFO_VERSION,
      .summary = "Run a scheme script",
      .arity = -3,
      .key_specs = scheme_eval_key_specs,
  };
  RedisModuleCommand *cmd = RedisModule_GetCommand(ctx, "scheme.eval");
  if (cmd == NULL
      || RedisModule_SetCommandInfo(cmd, &info) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning",
        "could not set command info for scheme.eval");
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

static int register_evalsha_command(RedisModuleCtx *ctx)
{
  if (RedisModule_CreateCommand(ctx, "scheme.evalsha", scheme_evalsha_command,
                                "write deny-oom deny-script", 0, 0, 0)
      == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning",
        "could not create command scheme.evalsha");
    return REDISMODULE_ERR;
  }

  RedisModuleCommandInfo info = {
      .version = REDISMODULE_COMMAND_INFO_VERSION,
      .summary = "Run a cached scheme script by its sha1 hex",
      .arity = -3,
      .key_specs = scheme_eval_key_specs,
  };
  RedisModuleCommand *cmd = RedisModule_GetCommand(ctx, "scheme.evalsha");
  if (cmd == NULL
      || RedisModule_SetCommandInfo(cmd, &info) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning",
        "could not set command info for scheme.evalsha");
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

static int register_script_command(RedisModuleCtx *ctx)
{
  /* deny-script keeps scripts from calling scheme.script / scheme.evalsha
     again.  Not a keyspace write: the script cache is node-local and LOAD /
     FLUSH are not propagated, hence no write flag.  allow-stale permits
     running on a replica whose master link is down. */
  if (RedisModule_CreateCommand(ctx, "scheme.script", scheme_script_command,
                                "deny-script allow-stale", 0, 0, 0)
      == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning",
        "could not create command scheme.script");
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv,
                       int argc)
{
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);

  if (RedisModule_Init(ctx, "scheme", 1, REDISMODULE_APIVER_1)
      == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  /* Create the long-lived scheme VM and register the redis-* host functions. */
  if (redis_scheme_runtime_init() != 0) {
    RedisModule_Log(ctx, "warning", "could not initialize scheme runtime");
    return REDISMODULE_ERR;
  }

  if (register_eval_command(ctx) == REDISMODULE_ERR
      || register_evalsha_command(ctx) == REDISMODULE_ERR
      || register_script_command(ctx) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

/* Optional unload hook: resolved by dlsym in moduleUnload().  Without it,
   the VM heap (allocated through RedisModule_Realloc) would leak, because
   dlclose only unmaps the .so pages -- the pointer in g_vm lives in BSS,
   which gets unmapped, but the heap it points to stays in the Redis process
   with no referent left. */
int RedisModule_OnUnload(RedisModuleCtx *ctx)
{
  REDISMODULE_NOT_USED(ctx);
  redis_scheme_runtime_done();
  return REDISMODULE_OK;
}
