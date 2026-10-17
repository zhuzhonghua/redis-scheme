# redis_scheme_module

A Redis module that embeds the
[dummyscheme](https://github.com/zhuzhonghua/dummyscheme) VM.
It exposes a single command, `SCHEME.EVAL`, shaped like `EVAL`:

```
SCHEME.EVAL script numkeys key [key ...] arg [arg ...]
```

The handler evaluates the script in a fresh scheme VM and replies with
the result rendered as a string.  The key spec is the same as `EVAL`
(begin_search index pos=2, find_keys keynum firstkey=1 keystep=1), so
key parsing and cluster routing behave like Lua scripts.

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
