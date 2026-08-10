# Local patches to the vendored llama.cpp

Upstream: <https://github.com/ggml-org/llama.cpp> at
`74ce15741b420b8d6f12e720398458b576c51c2c` (2026-08-09).

The vendored tree is byte-identical to that commit except for the two patches
below. Re-derive and diff it with:

```sh
scripts/vendor-llama-cpp.sh <path-to-a-llama.cpp-checkout-at-the-pin>
```

The script reads every file with `git show <pin>:<path>`, so a dirty checkout
cannot smuggle an unrecorded edit in. Both patches are applied by that script
and both refuse loudly if upstream's code no longer matches what they expect —
a silently-skipped hardening patch is worse than a failed vendoring run.

Twelve lines in total, which is smaller than the patch this project already
carries in `vendored/mimalloc/src/options.c`. The precedent and the review
habit exist.

## 1 — `ggml/src/ggml-backend-dl.cpp`: no `dlopen` / `dlsym`

`ggml_backend_load_all()` scans for `libggml-*.so` at runtime and `dlopen`s
them. Hyponoia registers every backend statically at link time, so that path is
dead code by construction — but the compiler cannot know it, so `dlopen` and
`dlsym` land in the binary's undefined dynamic symbols anyway.

Three things break if they do:

- **A property is lost, not merely unimproved.** The shipped binary has *zero*
  undefined `dlopen` today: `nm -D --undefined-only build/c/hyponoia | grep -c
  dlopen` is `0`. Gaining one would be a regression in the release artifact.
- **`-static` stops meaning static.** `ld` warns that "using `dlopen` in
  statically linked applications requires at runtime the shared libraries from
  the glibc version used for linking", which is exactly the property
  `STATIC=1` exists to provide.
- **`scripts/security-vendored.sh` blocks it** — dynamic loading outside
  sqlite3 and mimalloc is a structural failure.

The POSIX branch's three `dl_*` functions return `nullptr` / a fixed string.
The Windows branch is untouched.

## 2 — `ggml/src/ggml.c`: no debugger attach on abort

`ggml_print_backtrace()` `fork()`s and `execlp()`s `gdb` — then `lldb` — against
its own pid to dump a backtrace when ggml aborts. A shipped binary that spawns
a debugger on itself reads exactly like malware to a generic classifier. It is
the same concern that made `TEST_SEAMS` opt-in rather than opt-out
(`Makefile.hyp:61-72`).

The body is replaced with a call to `ggml_print_backtrace_symbols()`, which
uses `backtrace()` / `backtrace_symbols_fd()` and spawns nothing. Behaviour on
abort is a symbol backtrace instead of a debugger session; nothing else changes.

**The dead branch is deleted, not `#if 0`-ed.** Track 2's prototype wrapped it
in `#if 0` and that would not have passed here:
`scripts/security-vendored.sh:303` greps the *text* of every vendored `.c` and
`.h`, so a `fork()` inside a disabled preprocessor block still blocks the gate.
This is one of the differences between a prototype that links and a product
that ships.

## What is deliberately NOT patched

Nothing else. In particular the `.cpp` sources are not rewritten for warnings:
llama.cpp's C++ does not survive `-Wall -Wextra -Werror`, and the vendored
bucket in `Makefile.hyp` relaxes the warning posture the way `vendored/sqlite3`
and the tree-sitter grammars already do. Editing third-party code to satisfy
our warning flags would make every future upstream sync a merge conflict.

## Verification

`scripts/security-vendored.sh` covers the source side. The binary side is
checked directly, and is the claim that actually matters:

```
$ nm -D --undefined-only build/c/hyponoia | grep -cE 'dlopen|dlsym|execl|execv|fork|popen|system'
0
```
