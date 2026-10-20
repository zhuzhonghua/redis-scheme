# redis_scheme_module

A Redis module that embeds the
[dummyscheme](https://github.com/zhuzhonghua/dummyscheme) VM.
It exposes a script engine shaped like the `EVAL` family, so Scheme
scripts can run Redis commands the way Lua scripts do:

```
SCHEME.EVAL script numkeys key [key ...] arg [arg ...]
SCHEME.EVALSHA sha1 numkeys key [key ...] arg [arg ...]
SCHEME.SCRIPT LOAD body | EXISTS sha1 [sha1 ...] | FLUSH
```

The handler evaluates the script in a fresh scheme VM and replies with
the result converted to a typed Redis reply.  The key spec is the same
as `EVAL` (begin_search index pos=2, find_keys keynum firstkey=1
keystep=1), so key parsing and cluster routing follow the same rules.

## Build

Clone both repos side by side and build — run the whole block top to
bottom, no further setup needed:

```
git clone --depth=1 https://github.com/zhuzhonghua/dummyscheme
git clone --depth=1 https://github.com/zhuzhonghua/redis-scheme
cd redis-scheme
cmake -S . -B build -DSCHEME_SRC_DIR=../dummyscheme
cmake --build build -j8
```

`SCHEME_SRC_DIR` points at the host dummyscheme project (the scheme core
sources); `../dummyscheme` matches the two clones sitting next to each
other, and it is also the default when building in a checkout.

## How to use

Start Redis with the module loaded:

```
redis-server --loadmodule /path/to/scheme_module.so
```

Evaluate a scheme script:

```
redis-cli SCHEME.EVAL "(+ 40 2)" 0
```

Returns `42`.

Keys and args are bound to the `KEYS` / `ARGV` globals as 0-based
vectors (like `EVAL`, the `numkeys` keys follow the script, then the
args):

```
redis-cli SCHEME.EVAL "(vector-ref KEYS 0)" 1 mykey
redis-cli SCHEME.EVAL "(vector-ref ARGV 1)" 0 alpha beta
```

## Replies

The script's return value is converted to a typed Redis reply:

| Scheme value | Redis reply |
|---|---|
| integer | integer (e.g. `42`) |
| float | integer (truncated) |
| string | bulk string |
| `#t` | integer `1` |
| `#f` | null |
| proper list / vector | array, elements converted recursively |
| anything else (symbol, char, plain hashtable, dotted pair, ...) | bulk string of the write rendering |

Examples: `(list 1 2 3)` replies with a three-element array; `(list "a" 1
#t)` replies `["a", 1, 1]`; `'()` replies with an empty array; `(sqrt 2)`
replies `1` (floats truncate).

## Calling Redis commands

A script runs Redis commands through `redis-call` / `redis-pcall`.
The first argument is the command name, the rest are its arguments: strings pass through, integers
and floats are converted to their decimal text:

```
redis-cli SCHEME.EVAL '(redis-call "SET" "mykey" "hello")' 0
redis-cli SCHEME.EVAL '(redis-call "INCR" "counter")' 0
redis-cli SCHEME.EVAL '(begin (redis-call "SET" "f" 1.5) (redis-call "GET" "f"))' 0
```

The reply is converted back to a scheme value: bulk string -> string,
integer -> int, double -> float, bool -> `#t`/`#f`, null -> `#f` (falsy,
so `(if (redis-call "GET" k) ...)` works), array/set -> vector, map ->
hashtable, big number / verbatim -> string.

Write commands a script issues are propagated to AOF / replicas (the
`!` replicate flag on the module call), so replicas replay the writes,
not the script.

`redis-call` aborts the script when the command returns an error;
`redis-pcall` returns a `{"err": "..."}` table instead so the script can
continue:

```
redis-cli SCHEME.EVAL '(redis-pcall "NOSUCHCMD" "x")' 0   # error reply
redis-cli SCHEME.EVAL '(begin (redis-pcall "NOSUCHCMD" "x") "ok")' 0
```

A `{err=...}` value returned as the final result is emitted as an error
reply, the same tagged-value path described below.  Commands flagged
`deny-script` are rejected in script mode, so `redis-call "SCHEME.EVAL"
...` fails.

Error and status replies: a script can return an error / status reply
instead of a plain value.  Everyday use is two host functions, each
taking exactly one string argument:

```
redis-cli SCHEME.EVAL '(redis-error-reply "oops")' 0
redis-cli SCHEME.EVAL '(redis-status-reply "OK")' 0
```

`redis-error-reply` makes the client see an error reply (wire `-oops`;
a message that already starts with `ERR ` is passed through as-is).
`redis-status-reply` makes it see a status line (`OK`).  Under the hood
both return a tagged value: a hashtable with a single string field --
`{"err": "msg"}` for errors, `{"ok": "msg"}` for status.  The
reply path recognizes exactly this shape (a single string field whose
key is "err" or "ok", with a string value) and emits the matching Redis
reply; every other value keeps the plain string rendering.

Because the tagged value is an ordinary Scheme value, a script can also
build and return it directly instead of calling the host function:

```
redis-cli SCHEME.EVAL '((lambda (h) (hash-table-set! h "ok" "DONE") h)
                        (make-hash-table))' 0
```

This is the same value `(redis-status-reply "DONE")` produces.  Tables
with more than one field, or with a field other than "err"/"ok", are
not recognized as replies and render as `#<hash-table>`.

## Script cache

`SCHEME.EVAL` stores every script it runs in a server-side cache keyed by
its sha1 hex.  `SCHEME.SCRIPT` manages that cache and `SCHEME.EVALSHA`
runs a cached script by digest:

```
redis-cli SCHEME.SCRIPT LOAD '(+ 1 2)'            # -> the 40-char sha1 hex
redis-cli SCHEME.SCRIPT EXISTS <sha1>             # -> 1 / 0 per entry
redis-cli SCHEME.EVALSHA <sha1> 0                 # -> 3
redis-cli SCHEME.SCRIPT FLUSH                     # -> OK
```

- `SCHEME.EVALSHA` has the same argument shape as `SCHEME.EVAL` (digest
  first), and writes issued from the cached script propagate to replicas
  and AOF the same way.  A miss replies
  `NOSCRIPT no matching script; use SCHEME.EVAL`.
- The digest is the same 40-char lowercase hex `redis-sha1hex` computes;
  it is case-sensitive, and the cache lives in process memory only, so it
  is dropped on restart.
- The cache is node-local: `SCHEME.SCRIPT LOAD` and `FLUSH` are not
  propagated, so a replica's cache holds only what ran on that replica.
  After a failover a digest may miss — reply `NOSCRIPT`, then retry with
  `SCHEME.EVAL`, which re-caches the script on the new master.
- Entries cached by `SCHEME.EVAL` are evicted least-recently-used first
  once their count reaches 500; entries added by `SCHEME.SCRIPT LOAD`
  are never evicted.  Scripts cannot call `SCHEME.SCRIPT` /
  `SCHEME.EVALSHA` themselves.

## Hashing

`redis-sha1hex` returns the SHA-1 digest (RFC 3174) of one argument as
40 lowercase hex characters.  Strings hash their bytes; integers and
floats hash their decimal text form:

```
redis-cli SCHEME.EVAL '(redis-sha1hex "abc")' 0
```

Returns `a9993e364706816aba3e25717850c26c9cd0d89d`.  Passing any other
type is an error.

## Logging

`redis-log` writes a message to the Redis server log.  The first argument
is the log level (one of the constants below), the rest are message
fragments joined with spaces.  Only strings and numbers contribute;
other types are skipped.

```
redis-cli SCHEME.EVAL '(redis-log redis-log-warning "cache miss for" key)' 1 mykey
redis-cli SCHEME.EVAL '(redis-log redis-log-notice "script started")' 0
```

Constants: `redis-log-debug`=0, `redis-log-verbose`=1, `redis-log-notice`=2,
`redis-log-warning`=3.

## Propagation control

By default every write a script issues is propagated to AOF and replicas.
`redis-set-repl` turns propagation on or off for the rest of the script:

```
redis-cli SCHEME.EVAL '(begin
  (redis-call "SET" "visible" 1)          ; propagated
  (redis-set-repl redis-repl-none)
  (redis-call "SET" "temp-counter" 99)    ; NOT propagated
  (redis-set-repl redis-repl-all)
  (redis-call "SET" "visible-again" 1)    ; propagated again
)' 0
```

Constants: `redis-repl-none`=0, `redis-repl-aof`=1, `redis-repl-replica`=2,
`redis-repl-all`=3.  The flags reset to `all` at the start of every script
invocation, so settings do not leak between scripts.

Note: the module API controls AOF and replica propagation together; there
is no way to enable one without the other.

## Server version

Two globals expose the running server version:

```
redis-cli SCHEME.EVAL 'redis-version' 0        ; "8.10.2"
redis-cli SCHEME.EVAL 'redis-version-num' 0     ; 526850 (0x00080a02)
```
