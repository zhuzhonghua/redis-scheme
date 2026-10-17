/*
 * redis_scheme_module.c -- Redis module for dummyscheme.
 *
 *
 * The handler creates a fresh scheme VM, evaluates the script, replies with
 * the result as a string, then destroys the VM.
 *
 */
#include <stdio.h>
#include <string.h>
#include "redismodule.h"
#include "redis_scheme_runtime.h"

/* ---------------------------------------------------------------------------
 * SCHEME.EVAL handler
 * ------------------------------------------------------------------------ */

static int scheme_eval_command(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
  if (argc < 3) {
    return RedisModule_ReplyWithError(ctx,
        "ERR wrong number of arguments for 'scheme.eval' command");
  }

  /* numkeys must be a non-negative integer; keys (argv[3] .. argv[3+nk-1])
     and args (argv[3+nk] ..) are bound to the KEYS/ARGV globals before
     evaluation, so at least numkeys arguments must follow the script. */
  long long numkeys = 0;
  if (RedisModule_StringToLongLong(argv[2], &numkeys) != REDISMODULE_OK
      || numkeys < 0) {
    return RedisModule_ReplyWithError(ctx,
        "ERR value is not an integer or out of range");
  }
  if (argc < 3 + numkeys) {
    return RedisModule_ReplyWithError(ctx,
        "ERR Number of keys can't be greater than number of args");
  }

  /* Script bytes: argv[1] */
  size_t script_len = 0;
  const char *script = RedisModule_StringPtrLen(argv[1], &script_len);

  /* Create a fresh scheme VM for this command. */
  redis_scheme_vm_t *h = redis_scheme_create();
  if (h == NULL) {
    return RedisModule_ReplyWithError(ctx, "ERR scheme: cannot create VM");
  }

  char errbuf[512];
  if (redis_scheme_bind_args(h, argv, argc, numkeys,
                             errbuf, sizeof(errbuf)) != 0) {
    redis_scheme_destroy(h, NULL, 0);
    return RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", errbuf);
  }

  if (redis_scheme_eval(h, script, script_len, errbuf, sizeof(errbuf)) != 0) {
    redis_scheme_destroy(h, NULL, 0);
    return RedisModule_ReplyWithErrorFormat(ctx, "ERR scheme: %s", errbuf);
  }

  redis_scheme_reply(h, ctx);
  redis_scheme_destroy(h, NULL, 0);
  return REDISMODULE_OK;
}

/* ---------------------------------------------------------------------------
 * Module entry point
 * ------------------------------------------------------------------------ */

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv,
                       int argc)
{
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);

  if (RedisModule_Init(ctx, "scheme", 1, REDISMODULE_APIVER_1)
      == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  /* Key spec, same shape as EVAL: script numkeys key [key ...] arg [...]
     Search starts at argv[2] (numkeys), the key count is read there, and
     the keys begin at argv[3] with a step of 1.  Flags are the conservative
     RW/ACCESS/UPDATE, as in EVAL.  The array ends with a zeroed sentinel
     (begin_search_type = BS_INVALID). */
  RedisModuleCommandKeySpec kspecs[] = {
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
  RedisModuleCommandInfo info = {
      .version = REDISMODULE_COMMAND_INFO_VERSION,
      .summary = "Run a scheme script",
      .arity = -3,
      .key_specs = kspecs,
  };

  /* No fixed-position keys: firstkey=0 lastkey=0 keystep=0. */
  if (RedisModule_CreateCommand(ctx, "scheme.eval", scheme_eval_command,
                                "write deny-oom", 0, 0, 0)
      == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "could not create command scheme.eval");
    return REDISMODULE_ERR;
  }

  RedisModuleCommand *cmd = RedisModule_GetCommand(ctx, "scheme.eval");
  if (cmd == NULL
      || RedisModule_SetCommandInfo(cmd, &info) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning",
        "could not set command info for scheme.eval");
    return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}
