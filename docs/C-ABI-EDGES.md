# C ABI edges — what extraction can see, and why the join is not there

A timeboxed spike asked whether Hyponoia can build a graph edge from a
`dlopen`/`dlsym` call site to the exported symbol it names in another repository
of the same workspace. The hypothesis it went to test:

> `dlopen("libfoo.so")` + `dlsym("bar")` has a string on the caller side and an
> exported symbol on the callee side and **should be** matchable. General
> link-time calls through shared headers **should not be**.

**Verdict: do not build it.** Both halves of that sentence turn out to be true
in isolation and useless together. The extractor really does reach the caller's
string — better than expected, including through macro expansion. What it cannot
reach is anything that says *which repository* the string refers to, and the
literal it *can* read is, in every real instance measured, either an operating
system library or a plugin entry-point convention shared by dozens of
definitions at once.

Read this before adding a dynamic-loading edge type, and before assuming the
preprocessed extraction pass sees more than it does.

---

## Summary of the measurements

| question | measured answer |
|---|---|
| Is a call argument's string literal reachable in C extraction? | **Yes** — `HYPCall.args[].value`, no dlopen-specific code needed |
| Does a `#define`d library name survive? | **Yes**, via the preprocessed pass, stamped `HYP_SOURCE_ORIGIN_PREPROCESSED` |
| Does a file-scope `static const char *` survive? | **No** — C's `init_declarator` is absent from constant propagation |
| Loader call sites whose library argument is a plain literal | **4 of 59 (6.8%)** across nine corpora |
| Loader call sites whose library argument is a literal in any spelling | **23 of 59 (39.0%)** |
| Of those 23 literals, how many name a non-OS library | **0** |
| Resolver call sites whose symbol argument is a plain literal | **62 of 92 (67.4%)** |
| Distinct literal symbols with exactly one definition in their own tree | **5 of 39** |
| Ambiguity of the two genuine plugin boundaries in the corpus | `RedisModule_OnLoad` **×59**, `_PG_init` **×44** |

---

## Part 1 · The caller's string is reachable — more than expected

Argument capture is not gated on language or on callee. `handle_calls` calls
`extract_call_args` for every language, and `extract_call_args` stores each
positional argument's raw text in `HYPCallArg.expr` and, when the node is
string-like, the unquoted content in `HYPCallArg.value`. tree-sitter's C and C++
grammars spell `"libfoo.so"` as `string_literal`, which `is_string_like`
accepts. So the data is already there, today, with no new extractor.

Six characterization tests in `tests/test_extraction.c` (suite `extraction`,
named `c_abi_*`) pin exactly where that stops being true. Each was planted with
a negative control and watched to fail before being trusted.

| fixture | result | mechanism |
|---|---|---|
| `dlopen("libplugin.so", 2)` and `dlsym(h, "plugin_entry")` | `args[0].value == "libplugin.so"` and `args[1].value == "plugin_entry"`, both origin RAW | generic argument capture |
| `#define PLUGIN_LIB "libplugin.so"` then `dlopen(PLUGIN_LIB, 2)` | `args[0].value == "libplugin.so"`, origin **PREPROCESSED** | the C-family second extraction pass over the expanded buffer; simplecpp round-trips string tokens verbatim |
| `static const char *kLib = "libplugin.so";` then `dlopen(kLib, 2)` | `args[0].expr == "kLib"`, **`value == NULL`** | constant propagation accepts `assignment`, `expression_statement`, `short_var_declaration`, `const_spec`, `variable_declarator` — C spells a file-scope initializer `init_declarator`, which is not in that set |
| wrapper: `load_lib("libplugin.so")` → `dlopen(path, 2)` | literal on the wrapper call, parameter name on the `dlopen` call, **no join** | argument capture is call-site-local; joining them is interprocedural constant propagation, a different capability |
| `#define app_dlsym(h,s) dlsym(h,s)` then `app_dlsym(h, "plugin_entry")` | zero `dlsym` occurrences on the RAW pass; one on the PREPROCESSED pass | the raw pass sees only the wrapper spelling |
| `dlopen("libplugin.so", 2)` | **no `HYPStringRef`** for the path | `hyp_classify_string` keeps URLs and config-file extensions; shared-object suffixes are in neither list, so the captured argument is the only place a library name survives extraction |

### The controls, and what each one proved

A test asserting that a literal is *unreachable* looks identical to a test that
cannot fail, so each fixture was mutated into the reachable form and watched to
fail. All six failed, each with a message naming its own mechanism:

| test | mutation | observed failure |
|---|---|---|
| literal args captured | look for the library path at argument index 1 | `open is NULL` |
| macro-defined library | assert the expanded occurrence is RAW | `source_origin == 1, expected 0` |
| file-scope const | replace the const with `#define kLib "libplugin.so"` | the literal **is** found — `find_call_with_arg_value(...) is not NULL` |
| wrapped loader | inline the literal at the `dlopen` site | the literal **is** found at the `dlopen` site |
| macro-renamed resolver | call `dlsym` directly instead of through the macro | `source_origin == 0, expected 1` |
| library path is not a string ref | change the literal to `"plugin.yaml"` | a `CONFIG` string ref **is** emitted |

The third and fourth are the ones that matter: a negative assertion is only
worth anything if the fixture can be shown to produce the positive.

### Three limits on the preprocessed rescue

The preprocessed pass is the strongest single result here, and it is narrower
than it looks:

1. **It is gated on the file containing a directive.** `has_preprocessor_work`
   in `internal/hyp/preprocessor.cpp` returns false unless the source contains
   `#define`, `#ifdef`, `#ifndef` or `#if ` — note the trailing space, so
   `#if(` misses. A file that gets its library name from an included header and
   has no local directive gets no second pass at all.
2. **No include paths and no extra defines are ever supplied.** Every
   production caller of `hyp_extract_file_ex` passes `NULL, NULL` for
   `extra_defines` and `include_paths` — `src/pipeline/pass_calls.c`,
   `src/pipeline/pass_definitions.c` (twice) and `src/pipeline/pass_parallel.c`.
   `src/pipeline/pass_compile_commands.c` does parse `-I` and `-D` into
   `hyp_compile_flags_t`, and nothing outside that file ever reads
   `include_paths`. So a macro defined in a header never expands.
3. **Adjacent literals do not concatenate.** The expanded text is a token
   stringification, so `"lib" "foo.so"` stays two tokens and parses as
   `concatenated_string`, a node kind `is_string_like` does not accept.

---

## Part 2 · How often the string is a literal, and what it names

Nine corpora, 17,930 C/C++/ObjC files, scanned for the dynamic-loading family
(`dlopen`, `dlmopen`, `LoadLibrary*`, `LoadPackagedLibrary`, `dlsym`, `dlvsym`,
`GetProcAddress*`). Classification is grep-shaped and therefore **a floor** —
see *What this could not see* below.

| corpus | files | loader sites | plain literal | wide literal | macro-wrapped | resolver sites | plain literal |
|---|---:|---:|---:|---:|---:|---:|---:|
| hyponoia first-party | 1,531 | 3 | 0 | 3 | 0 | 8 | 6 |
| hyponoia `vendored/` (not indexed) | 374 | 7 | 0 | 0 | 5 | 16 | 15 |
| LLVM `lld/ELF` | 68 | 0 | 0 | 0 | 0 | 0 | 0 |
| torch C/C++ headers | 10,027 | 4 | 0 | 0 | 0 | 4 | 1 |
| redis | 794 | 3 | 0 | 0 | 0 | 6 | 3 |
| postgres | 2,573 | 8 | 3 | 0 | 0 | 12 | 6 |
| nginx | 405 | 4 | 0 | 0 | 0 | 6 | 1 |
| cpython | 1,130 | 29 | 1 | 6 | 5 | 35 | 25 |
| curl | 1,028 | 1 | 0 | 0 | 0 | 5 | 5 |
| **total** | **17,930** | **59** | **4 (6.8%)** | **9** | **10** | **92** | **62 (67.4%)** |

The asymmetry is the whole finding, and it is consistent across every corpus:

- **The library name — the half that would say which repository — is a runtime
  variable.** 36 of 59 loader sites pass an identifier or an expression:
  `path`, `file`, `zFilename`, `module_name`, `file_scanner->filename`,
  `filePath.c_str()`. That is not an accident of sampling; it is what a plugin
  host *is*. A host loads whatever a config file or a directory scan names, and
  a compiled-in library path would defeat the point of loading it dynamically.
- **When the library name IS a literal, it never names workspace code.** Here
  are all 23 literal-bearing loader arguments in the corpus, enumerated rather
  than summarized:

  `L"advapi32.dll"` ×3 · `TEXT("kernelbase.dll")` ×2 · `TEXT("ntdll.dll")` ×2 ·
  `TEXT("kernel32.dll")` ×2 · `TEXT("psapi.dll")` ×2 · `TEXT("bcrypt.dll")` ×2 ·
  `"dbghelp.dll"` · `"netmsg.dll"` · `"ntdll.dll"` ·
  `L"api-ms-win-core-file-l2-1-4"` ×2 · `"/usr/lib/libSystem.B.dylib"` ·
  `L"SHELL32"` · `L"api-ms-win-core-path-l1-1-0.dll"` ×2 · `L"Kernel32.dll"`

  **Every one is an operating-system library.** Not one names a first-party
  library that could be a workspace member. A workspace edge needs a
  counterparty inside the workspace, and across 17,930 files the literal loader
  argument never once pointed at one.

### The symbol literal does not select a target either

For each literal symbol passed to `dlsym`/`GetProcAddress`, count the
definitions of that symbol in the same tree — a proxy for "would this resolve to
one thing inside a workspace". 39 distinct symbols across the five public
corpora:

| outcome | count | meaning |
|---|---:|---|
| no definition in-corpus | 31 (79.5%) | names an OS or third-party ABI; an edge would have no counterparty |
| exactly one definition | 5 (12.8%) | would resolve — but all five are wrappers over OS symbols, not plugin boundaries |
| multiple definitions | 3 (7.7%) | `RedisModule_OnLoad` **×59**, `RedisModule_OnUnload` **×13**, `_PG_init` **×44** |

The last row is the pattern the hypothesis was written about, and it is the row
that kills it. In a real plugin architecture the literal symbol is an
**entry-point convention**, deliberately identical across every plugin, so the
string that *is* reachable is precisely the string that cannot pick a target. A
44-way edge presented as a resolved cross-repository call is worse than no
edge: a false positive attaches reasoning to code that never had it, and a
reader has no way to tell which of the 44 was meant.

---

## Part 3 · The callee side has no library identity at all

Even given a literal library path, nothing in the index says which repository
produces `libfoo.so`.

- **No build-target extraction exists.** `CMakeLists.txt` is recognized as a
  *language* to parse, not as a build graph; `add_library`, `SONAME` and
  friends appear nowhere in `src/` or `internal/hyp/`. `pass_pkgmap.c` maps
  package manifests to module QNs for JavaScript, Go, Rust, Python, PHP, Dart,
  Java, Elixir, Ruby and Swift — C and C++ have no manifest, which is exactly
  why the mapping does not exist for them.
- **No link flags are read.** `-l` and `-L` are parsed nowhere, including in
  `pass_compile_commands.c`, which reads only `-I` and `-D`.
- **Built artifacts are excluded by design.** `.so`, `.dll`, `.a` and `.o` are
  in the discovery ignore list. The index tracks the working tree; it never
  looks at a linked binary, so the exported-symbol table does not exist to be
  consulted.
- **`is_exported` does not mean linkage.** `hyp_is_exported` in
  `internal/hyp/helpers.c` is a naming-convention heuristic — capital initial
  for Go, Java, C#, Kotlin; non-underscore for Python — and returns `true` by
  default for every other language, C included. Every C definition is
  "exported". It carries no information about visibility attributes, version
  scripts or `.def` files, and it never claimed to.

So the callee side of the proposed edge reduces to "a function with this name,
somewhere in the workspace". That is a name join, with no corroborating
evidence, over a key measured above to be either absent or shared by dozens of
definitions.

---

## Part 4 · Link-time calls through shared headers

The hypothesis says these are not matchable. That is **half right, and the
wording hides an important distinction**.

What extraction *does* have: the `#include` (an `IMPORTS` edge, with both
quoted and angle-bracket forms handled), the declaration in the header, the
definition in the other repository, and the callee name at the call site. A
direct source-level call from `repo1` into `repo2` is therefore matchable **by
qualified name** — that is the direct-call plugin case, and it needs nothing
from dynamic loading.

What extraction does *not* have is the **link-time fact**: whether that
declaration is satisfied by `repo2`'s definition, by libc, or by a third
repository that happens to define the same name. That answer lives in `-l`
flags, library search paths, link order, archive member selection, version
scripts and symbol visibility — none of which the pipeline reads. So the
accurate statement is not "link-time calls are not matchable"; it is:

> A call through a shared header is matchable by name, exactly as any other
> cross-repository call is. What is **not** available at extraction time is
> which library actually provides the symbol, so a name match cannot be
> upgraded to a link fact, and same-named definitions in two members cannot be
> told apart.

Which means the dynamic-loading case is not a *different* mechanism from the
static one. It is the same name join with **strictly less** evidence: the
static case at least has a shared header binding the two ends, and the dynamic
case has a string that names an OS library 100% of the time it is a literal.

---

## The half that already exists, and is already dead

`c_extract_dll_resolve_name` in `internal/hyp/lsp/c_lsp.c` already does the
caller-side extraction this spike was asked to evaluate. It unwraps casts,
takes a string-literal argument, and produces a resolved call to
`external.<symbol>` with strategy `lsp_dll_resolve` and confidence 0.80.

It has never produced an edge. Nothing anywhere mints an `external.*` node, so
`pass_calls.c` drops the resolved call when the target QN is not in the graph
buffer. `tests/repro/repro_lsp_c_cpp.c` already asserts that no resolvable edge
appears, with a comment explaining why.

Two things follow. First, the caller-side half of the proposal is not new work
— it is existing work whose missing counterpart is the node, and the node is
missing because the callee side has no identity (Part 3). Second, the validator
in that function rejects any literal containing `.`, `/`, `\` or a space, so it
excludes library paths by construction. The one existing implementation already
declined to model the library half.

---

## Cost, and what it would buy

**Cost.** Adding a seventh cross-repository edge type touches 13 mandatory code
sites across four files — five in the pass core (`pass_cross_repo.h`'s result
struct, the delete table, the orchestration, the total, the matcher), seven in
`src/mcp/mcp.c` (two prose descriptions, two duplicated `cross_types[]` tables,
the trace-mode table, the total, the six response keys), and one edge-colour
entry in the graph UI — plus mandatory doc prose and a new test. The graph
schema surface and the store need no change: `edges.type` is free-form text and
`get_graph_schema` derives its list from the data.

That is the cheap part. On top of it:

- an extractor and a synthesized-node path, both of which must be written twice
  because emission is duplicated across the sequential (`pass_calls.c`,
  `pass_definitions.c`) and parallel (`pass_parallel.c`) pipelines;
- `hyp_store_workspace_repos()` has **no production callers** — `pass_cross_repo`
  discovers projects by scanning the cache directory for `*.db` files and has no
  registry awareness at all, so a member-scoped edge would be its first consumer
  and would have to bring the "enumerate members for a pass" helper with it;
- the only end-to-end cross-repository matching test lives in the repro runner,
  which is deliberately off the merge gate, so new matching semantics arrive
  with no gating coverage unless that is fixed too.

**What it would buy, on the corpus that exists.** Zero edges in Hyponoia
itself: every first-party dynamic-loading site — the 11 the census counted and
the 36 hidden behind two local macros — is Windows-only resolution of
`advapi32.dll` and `kernel32.dll`, and the interesting sites in
`vendored/llama.cpp` and `vendored/sqlite3` are excluded from the index twice
over — by the hardcoded skip list and by this repository's own `.hypignore`.
Across five public C corpora the count of literal symbols that would resolve to
exactly one definition is 5 of 39, and none of the five is a plugin boundary.

---

## What this could not see

Every count above is a floor, and these are the specific reasons:

- **The scanner is regex-based, not a parse, and the undercount is large.** It
  keys on the callee spelling, so it misses every site that reaches the loader
  through an indirection the text does not show. Measured in this repository:
  the census found 8 resolver sites in the first-party tree, and two local
  macros — `RESOLVE_ADVAPI_MEMBER` in `src/daemon/ipc.c` and
  `PRIVATE_RESOLVE_ADVAPI` in `src/foundation/private_file_lock.c` — hide 36
  more (20 and 16 expansions respectively), each carrying a literal symbol name.
  **The real first-party resolver count is 44, five and a half times the 8 the
  census saw.** Both macros are in-file `#define`s, so
  the preprocessed extraction pass would recover exactly these, which is the
  cleanest demonstration in the corpus of why an origin-blind site count is a
  floor. Two other indirections are not recoverable that way: `sqlite3.c`
  routes every dynamic call through an overridable syscall table, so its call
  sites read `osLoadLibraryW` and `osGetProcAddressA`; and llama.cpp's backend
  loader wraps both operations in `dl_load_library` and `dl_get_sym`, ordinary
  C++ functions that no amount of preprocessing resolves. Six further counted
  sites are inside portability macro *definitions* rather than at their call
  sites, which inflates the site count and deflates the literal fraction at the
  same time.
- **Definition counting is a ctags-grade regex**, so the ambiguity figures are
  approximate in both directions: a definition split across lines in an unusual
  way is missed, and a sufficiently call-like line can be counted.
- **The corpora are what was reachable offline.** Five public C projects, this
  repository, its vendored trees, one sparse LLVM subtree and one set of
  installed C headers. The public five were picked as the canonical C plugin
  hosts, which if anything biases *toward* the hypothesis — those are the
  projects where a dynamic-loading edge should look best, and they are where the
  ambiguity numbers are worst.
- **Windows wide-string and `TEXT()` literals were not tested against the
  extractor.** They are counted separately in the census because `strip_quotes`
  keys on a leading quote character and a wide literal starts with `L`; whether
  the resulting `value` is normalized was not measured. It does not affect the
  verdict, because every literal in those two classes names an OS library and
  so produces no candidate edge either way.

---

## What to do instead

**Cut the dynamic-loading edge.** The information needed to make it a
*resolved* edge does not exist at extraction time, and the information that
does exist would make it a confident wrong answer at a rate the corpus lets us
name: 44 candidates for one `_PG_init`.

Three findings from this spike are worth keeping, all of them independent of
whether anyone ever revisits the edge:

1. **Constant propagation is blind to C and C++ file-scope initializers.**
   `handle_string_constants` in `internal/hyp/extract_unified.c` accepts five
   node kinds and `init_declarator` is not among them, so
   `static const char *URL = "https://…"; fetch(URL);` in C loses the string
   that the same shape in JavaScript or Go keeps. This suppresses
   `HYPCallArg.value` for *every* C call argument named by a file-scope
   constant, not only loader arguments. It is a general extraction gap with a
   general fix.
2. **The preprocessed pass never receives include paths or defines.**
   `pass_compile_commands.c` collects them and nothing consumes them. Any claim
   that the preprocessed pass "sees macro-defined values" is true only for
   macros defined in the same file. This bounds several other results, and the
   plumbing already half exists.
3. **`lsp_dll_resolve` is dead and its deadness is asserted.** Either wire it or
   retire it, but do not leave an extraction strategy that has never produced an
   edge sitting in the tree looking like coverage.

---

## Reproducing the measurements

The corpus scanners are scaffolding and were not committed; they are three
short Python filters over a directory tree, and the numbers above are stated
with enough structure to rebuild them:

- classify each `dlopen`/`dlsym`/`LoadLibrary*`/`GetProcAddress*` call site by
  the syntactic class of the argument that carries the interesting string
  (index 0 for loaders, index 1 for resolvers): plain literal, wide literal,
  macro-wrapped literal, identifier, expression;
- for each literal symbol, count definitions of that symbol in the same tree.

The extractor behaviour, which is the part that must not silently change, is
committed instead: the `c_abi_*` tests in `tests/test_extraction.c`, run with
`scripts/test.sh --suites extraction`.
