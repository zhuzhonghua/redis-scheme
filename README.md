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
