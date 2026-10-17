# redis_scheme_module

A Redis module that embeds the
[dummyscheme](https://github.com/zhuzhonghua/dummyscheme) VM.
It exposes a single command, `SCHEME.EVAL`, shaped like `EVAL`, so
Scheme scripts can run Redis commands the way Lua scripts do:

```
SCHEME.EVAL script numkeys key [key ...] arg [arg ...]
```

The handler evaluates the script in a fresh scheme VM and replies with
the result converted to a typed Redis reply.  The key spec is the same
as `EVAL` (begin_search index pos=2, find_keys keynum firstkey=1
keystep=1), so key parsing and cluster routing follow the same rules.

## Build

`SCHEME_SRC_DIR` must point at the host dummyscheme project (the scheme
core sources).

```
cmake -S . -B build -DSCHEME_SRC_DIR=/path/to/dummy
cmake --build build -j8
```

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
