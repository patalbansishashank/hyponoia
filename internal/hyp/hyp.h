#ifndef HYP_H
#define HYP_H

#include <stdint.h>
#include <stdbool.h>
#include "arena.h"
#include "tree_sitter/api.h"

// Language enum mirrors lang.Language in Go.
// Order must match lang_specs.c tables.
typedef enum {
    HYP_LANG_GO = 0,
    HYP_LANG_PYTHON,
    HYP_LANG_JAVASCRIPT,
    HYP_LANG_TYPESCRIPT,
    HYP_LANG_TSX,
    HYP_LANG_RUST,
    HYP_LANG_JAVA,
    HYP_LANG_CPP,
    HYP_LANG_CSHARP,
    HYP_LANG_PHP,
    HYP_LANG_LUA,
    HYP_LANG_SCALA,
    HYP_LANG_KOTLIN,
    HYP_LANG_RUBY,
    HYP_LANG_C,
    HYP_LANG_BASH,
    HYP_LANG_ZIG,
    HYP_LANG_ELIXIR,
    HYP_LANG_HASKELL,
    HYP_LANG_OCAML,
    HYP_LANG_OBJC,
    HYP_LANG_SWIFT,
    HYP_LANG_DART,
    HYP_LANG_PERL,
    HYP_LANG_GROOVY,
    HYP_LANG_ERLANG,
    HYP_LANG_R,
    HYP_LANG_HTML,
    HYP_LANG_CSS,
    HYP_LANG_SCSS,
    HYP_LANG_YAML,
    HYP_LANG_TOML,
    HYP_LANG_HCL,
    HYP_LANG_SQL,
    HYP_LANG_DOCKERFILE,
    // New languages (v0.5 expansion)
    HYP_LANG_CLOJURE,
    HYP_LANG_FSHARP,
    HYP_LANG_JULIA,
    HYP_LANG_VIMSCRIPT,
    HYP_LANG_NIX,
    HYP_LANG_COMMONLISP,
    HYP_LANG_ELM,
    HYP_LANG_FORTRAN,
    HYP_LANG_CUDA,
    HYP_LANG_COBOL,
    HYP_LANG_VERILOG,
    HYP_LANG_EMACSLISP,
    HYP_LANG_JSON,
    HYP_LANG_XML,
    HYP_LANG_MARKDOWN,
    HYP_LANG_MAKEFILE,
    HYP_LANG_CMAKE,
    HYP_LANG_PROTOBUF,
    HYP_LANG_GRAPHQL,
    HYP_LANG_VUE,
    HYP_LANG_SVELTE,
    HYP_LANG_MESON,
    HYP_LANG_GLSL,
    HYP_LANG_INI,
    // Scientific/math languages
    HYP_LANG_MATLAB,
    HYP_LANG_LEAN,
    HYP_LANG_FORM,
    HYP_LANG_MAGMA,
    HYP_LANG_WOLFRAM,
    HYP_LANG_SOLIDITY,
    HYP_LANG_TYPST,
    HYP_LANG_GDSCRIPT,
    HYP_LANG_GLEAM,
    HYP_LANG_POWERSHELL,
    HYP_LANG_PASCAL,
    HYP_LANG_DLANG,
    HYP_LANG_NIM,
    HYP_LANG_SCHEME,
    HYP_LANG_FENNEL,
    HYP_LANG_FISH,
    HYP_LANG_AWK,
    HYP_LANG_ZSH,
    HYP_LANG_TCL,
    HYP_LANG_ADA,
    HYP_LANG_AGDA,
    HYP_LANG_RACKET,
    HYP_LANG_ODIN,
    HYP_LANG_RESCRIPT,
    HYP_LANG_PURESCRIPT,
    HYP_LANG_NICKEL,
    HYP_LANG_CRYSTAL,
    HYP_LANG_TEAL,
    HYP_LANG_HARE,
    HYP_LANG_PONY,
    HYP_LANG_LUAU,
    HYP_LANG_JANET,
    HYP_LANG_SWAY,
    HYP_LANG_NASM,
    HYP_LANG_ASSEMBLY,
    HYP_LANG_ASTRO,
    HYP_LANG_BLADE,
    HYP_LANG_JUST,
    HYP_LANG_GOTEMPLATE,
    HYP_LANG_TEMPL,
    HYP_LANG_LIQUID,
    HYP_LANG_JINJA2,
    HYP_LANG_PRISMA,
    HYP_LANG_HYPRLANG,
    HYP_LANG_DOTENV,
    HYP_LANG_DIFF,
    HYP_LANG_WGSL,
    HYP_LANG_KDL,
    HYP_LANG_JSON5,
    HYP_LANG_JSONNET,
    HYP_LANG_RON,
    HYP_LANG_THRIFT,
    HYP_LANG_CAPNP,
    HYP_LANG_PROPERTIES,
    HYP_LANG_SSHCONFIG,
    HYP_LANG_BIBTEX,
    HYP_LANG_STARLARK,
    HYP_LANG_BICEP,
    HYP_LANG_CSV,
    HYP_LANG_REQUIREMENTS,
    HYP_LANG_HLSL,
    HYP_LANG_VHDL,
    HYP_LANG_SYSTEMVERILOG,
    HYP_LANG_DEVICETREE,
    HYP_LANG_LINKERSCRIPT,
    HYP_LANG_GN,
    HYP_LANG_KCONFIG,
    HYP_LANG_BITBAKE,
    HYP_LANG_SMALI,
    HYP_LANG_TABLEGEN,
    HYP_LANG_ISPC,
    HYP_LANG_CAIRO,
    HYP_LANG_MOVE,
    HYP_LANG_SQUIRREL,
    HYP_LANG_FUNC,
    HYP_LANG_REGEX,
    HYP_LANG_JSDOC,
    HYP_LANG_RST,
    HYP_LANG_BEANCOUNT,
    HYP_LANG_MERMAID,
    HYP_LANG_PUPPET,
    HYP_LANG_PO,
    HYP_LANG_GITATTRIBUTES,
    HYP_LANG_GITIGNORE,
    HYP_LANG_SLANG,
    HYP_LANG_LLVM_IR,
    HYP_LANG_SMITHY,
    HYP_LANG_WIT,
    HYP_LANG_TLAPLUS,
    HYP_LANG_PKL,
    HYP_LANG_GOMOD,
    HYP_LANG_APEX,
    HYP_LANG_SOQL,
    HYP_LANG_SOSL,
    HYP_LANG_KUSTOMIZE,            // kustomization.yaml — Kubernetes overlay tool
    HYP_LANG_K8S,                  // Generic Kubernetes manifest (apiVersion: detected)
    HYP_LANG_PINE,                 // Pine Script (TradingView indicator / strategy language)
    HYP_LANG_QML,                  // Qt QML (Qt Modeling Language — declarative UI + embedded JS)
    HYP_LANG_CFSCRIPT,             // CFML script dialect (.cfc components — Lucee/ColdFusion)
    HYP_LANG_CFML,                 // CFML tag dialect (.cfm templates — Lucee/ColdFusion)
    HYP_LANG_MOJO,                 // Mojo
    HYP_LANG_OBJECTSCRIPT_UDL,     // InterSystems ObjectScript UDL (.cls class files)
    HYP_LANG_OBJECTSCRIPT_ROUTINE, // InterSystems ObjectScript routine (.mac/.int/.rtn/.inc)
    HYP_LANG_OBJECTSCRIPT_EXPORT,  // InterSystems Studio Export XML (<Export generator="Cache">)
    HYP_LANG_COUNT
} HYPLanguage;

// --- Extraction result structs ---

typedef struct {
    const char *name;           // short name
    const char *qualified_name; // project.path.name
    const char *label;          // "Function", "Method", "Class", "Variable", "Module"
    const char *file_path;      // relative path
    uint32_t start_line;
    uint32_t end_line;
    const char *signature;              // parameter text (NULL if none)
    const char *return_type;            // return type text (NULL if none)
    const char *receiver;               // Go method receiver (NULL if none)
    const char *docstring;              // leading doc comment (NULL if none)
    const char *parent_class;           // enclosing class QN for methods (NULL if none)
    const char **decorators;            // NULL-terminated array (NULL if none)
    const char **base_classes;          // NULL-terminated array (NULL if none)
    const char **param_names;           // NULL-terminated array (NULL if none)
    const char **param_types;           // NULL-terminated array (NULL if none)
    const char **signature_param_types; // ordered internal signature types; "?" means unknown
    int signature_param_count;          // number of entries in signature_param_types
    const char **return_types;          // NULL-terminated array (NULL if none)
    const char *route_path;   // HTTP route path from decorator (e.g., "/api/users") or NULL
    const char *route_method; // HTTP method from decorator (e.g., "POST") or NULL
    int complexity;           // cyclomatic complexity
    int cognitive;            // cognitive complexity (nesting-weighted)
    int loop_count;           // number of loop constructs in the body
    int loop_depth;           // max nested-loop depth (bottleneck proxy)
    bool is_recursive;        // body contains a direct self-call (seed for "recursive")
    int param_count;          // number of parameters (large = complexity smell)
    int max_access_depth;     // deepest chained member/subscript access (a.b.c.d)
    int linear_scan_in_loop;  // count of linear-scan calls (find/contains/indexOf) inside loops
    int alloc_in_loop;        // count of allocation/append calls inside loops
    bool recursion_in_loop;   // a self-call occurs inside a loop body
    bool unguarded_recursion; // recursive with no self-call guarded by a conditional
    int lines;                // body line count
    uint32_t *fingerprint;    // MinHash fingerprint (arena-allocated, K values) or NULL
    int fingerprint_k;        // number of hash values (HYP_MINHASH_K or 0)
    bool is_exported;
    bool is_abstract;
    bool is_test;
    bool is_entry_point;
    const char *structural_profile; // AST structural profile (arena-allocated) or NULL
    const char *body_tokens; // space-separated raw identifier tokens from body (arena) or NULL
    /* Rust only: raw trait path from the exact `impl Trait for Type` block
     * that declared this method.  Kept at the tail so zero-initialised
     * callers in every other language remain ABI/source compatible. */
    const char *impl_trait;
} HYPDefinition;

/* Argument captured from a call expression */
typedef struct {
    const char *expr;    // raw expression text ("payload.info", "MY_URL", "'hello'")
    const char *value;   // resolved string value or NULL (constant propagation)
    const char *keyword; // keyword name if keyword arg ("url", "topic_id"), NULL if positional
    int index;           // positional index (0-based)
} HYPCallArg;

#define HYP_MAX_CALL_ARGS 8

/* Byte offsets are meaningful only within the source buffer that produced
 * them. C/C++/CUDA run both raw and preprocessed extraction passes, and those
 * buffers can contain unrelated occurrences at the same numeric span. */
typedef enum {
    HYP_SOURCE_ORIGIN_RAW = 0,
    HYP_SOURCE_ORIGIN_PREPROCESSED,
} HYPSourceOrigin;

typedef struct {
    const char *callee_name;            // raw callee text ("pkg.Func", "foo")
    const char *enclosing_func_qn;      // QN of enclosing function (or module QN)
    const char *first_string_arg;       // first string literal argument (URL, topic, key) or NULL
    const char *second_arg_name;        // second argument identifier (handler ref) or NULL
    HYPCallArg args[HYP_MAX_CALL_ARGS]; // first N arguments with expressions
    int arg_count;                      // number of captured arguments
    int loop_depth;                     // enclosing loop nesting at the call site
    int branch_depth;                   // enclosing branch nesting at the call site
    int start_line;                     // 1-based source line of the call (for def range-match)
    uint32_t site_start_byte;           // exact AST occurrence span; end > start when present
    uint32_t site_end_byte;             // exclusive byte offset in the source file
    HYPSourceOrigin source_origin;      // raw source or C-family preprocessed buffer
    bool is_method;                     // method/member call with a non-self receiver. Perl:
                                        // arrow/method call ($obj->m). TS/JS/TSX: member call
                                        // x.foo() whose receiver is not this/super. Default false.
    bool requires_lsp_resolution;       // synthetic semantic candidate (for example an implicit
                                        // C++ operator). Never fall back to textual resolution.
} HYPCall;

typedef struct {
    const char *local_name;  // local alias or name
    const char *module_path; // resolved module path / QN
} HYPImport;

typedef enum {
    HYP_USAGE_VALUE = 0,
    HYP_USAGE_CALL_REFERENCE,
} HYPUsageKind;

typedef struct {
    const char *ref_name;            // referenced identifier
    const char *enclosing_func_qn;   // QN of enclosing function (or module QN)
    HYPUsageKind kind;               // ordinary USAGE or explicit callable reference
    bool may_be_call_reference;      // syntactic candidate; exact LSP proof may upgrade its edge
    bool semantic_reference_blocked; // lexical evidence blocks only unproven textual fallback
    bool semantic_reference_local_shadow; // blocker belongs to a non-module lexical scope
    uint32_t lexical_scope_id;            // extraction-local scope instance; never graph identity
    uint32_t site_start_byte;             // exact reference-token span; end > start when present
    uint32_t site_end_byte;               // exclusive byte offset in the source file
    HYPSourceOrigin source_origin;        // raw source or C-family preprocessed buffer
} HYPUsage;

typedef struct {
    const char *exception_name;    // exception class/type name
    const char *enclosing_func_qn; // QN of enclosing function
} HYPThrow;

typedef struct {
    const char *var_name;          // variable name
    const char *enclosing_func_qn; // QN of enclosing function
    bool is_write;                 // true = write, false = read
} HYPReadWrite;

typedef struct {
    const char *type_name;         // referenced type/class name
    const char *enclosing_func_qn; // QN of enclosing function
} HYPTypeRef;

typedef struct {
    const char *env_key;           // environment variable key
    const char *enclosing_func_qn; // QN of enclosing function
} HYPEnvAccess;

typedef struct {
    const char *var_name;          // variable being assigned
    const char *type_name;         // class/type name of RHS constructor
    const char *enclosing_func_qn; // QN of enclosing function
} HYPTypeAssign;

// String reference: URL, config key, or async target found in source.
// Extracted from string literals during AST walk.
typedef enum {
    HYP_STRREF_URL = 0,    // REST path or full URL
    HYP_STRREF_CONFIG = 1, // config file path or env var key
} HYPStringRefKind;

typedef struct {
    const char *value;             // the string literal content
    const char *enclosing_func_qn; // QN of enclosing function
    const char *key_path;          // dotted key path from YAML/JSON nesting (NULL if flat)
    HYPStringRefKind kind;         // URL, CONFIG
} HYPStringRef;

/* Infrastructure binding: topic/queue → endpoint URL.
 * Extracted from YAML/HCL/JSON subscription/scheduler configs.
 * Used by pass_route_nodes to connect async Route nodes to handler services. */
typedef struct {
    const char *source_name; // topic, queue, or schedule name
    const char *target_url;  // push_endpoint, uri, or http_target URL
    const char *broker;      // "pubsub", "cloud_tasks", "cloud_scheduler", "sqs", "kafka"
} HYPInfraBinding;

/* Pub/sub channel participation.  One record per emit() or on()/addListener()
 * call detected in source — the receiver (e.g. Socket.IO client, EventEmitter
 * instance) is intentionally NOT identified; matching is by channel_name
 * across files, which captures the common pattern of one logical bus per
 * service.  Transport disambiguates Socket.IO vs EventEmitter vs future
 * detectors (Kafka, Cloud Pub/Sub, etc.). */
typedef enum {
    HYP_CHANNEL_EMIT = 0,
    HYP_CHANNEL_LISTEN = 1,
} HYPChannelDirection;

typedef struct {
    const char *channel_name;      // literal channel name (e.g. "user.created")
    const char *transport;         // "socketio", "event_emitter", ...
    const char *enclosing_func_qn; // QN of the function containing the emit/on call
    HYPChannelDirection direction;
} HYPChannel;

// Rust: impl Trait for Struct
typedef struct {
    const char *trait_name;  // trait name (raw text)
    const char *struct_name; // struct/type name (raw text)
    /* Exact extracted QN of the implementing type.  Unlike struct_name this
     * does not need a later leaf-name guess, and the relation exists even for
     * an empty `impl Trait for Type {}` block. */
    const char *struct_qn;
} HYPImplTrait;

typedef enum {
    HYP_RESOLVED_INVOCATION = 0,
    HYP_RESOLVED_CALL_REFERENCE,
} HYPResolvedKind;

// LSP-resolved invocation/reference: high-confidence type-aware resolution.
typedef struct {
    const char *caller_qn;         // enclosing function QN
    const char *callee_qn;         // resolved target QN (fully qualified)
    const char *strategy;          // "lsp_type_dispatch", "lsp_direct", etc.
    float confidence;              // 0.90-0.95
    const char *reason;            // diagnostic label for unresolved calls (NULL if resolved)
    HYPResolvedKind kind;          // invocation (CALLS) or explicit callable reference
    uint32_t site_start_byte;      // exact source occurrence; end > start when present
    uint32_t site_end_byte;        // exclusive byte offset in the source file
    HYPSourceOrigin source_origin; // raw source or C-family preprocessed buffer
} HYPResolvedCall;

typedef struct {
    HYPResolvedCall *items;
    int count;
    int cap;
} HYPResolvedCallArray;

// Growable arrays used during extraction.
typedef struct {
    HYPDefinition *items;
    int count;
    int cap;
} HYPDefArray;

typedef struct {
    HYPCall *items;
    int count;
    int cap;
} HYPCallArray;

typedef struct {
    HYPImport *items;
    int count;
    int cap;
} HYPImportArray;

typedef struct {
    HYPUsage *items;
    int count;
    int cap;
} HYPUsageArray;

typedef struct {
    HYPThrow *items;
    int count;
    int cap;
} HYPThrowArray;

typedef struct {
    HYPReadWrite *items;
    int count;
    int cap;
} HYPRWArray;

typedef struct {
    HYPTypeRef *items;
    int count;
    int cap;
} HYPTypeRefArray;

typedef struct {
    HYPEnvAccess *items;
    int count;
    int cap;
} HYPEnvAccessArray;

typedef struct {
    HYPTypeAssign *items;
    int count;
    int cap;
} HYPTypeAssignArray;

typedef struct {
    HYPStringRef *items;
    int count;
    int cap;
} HYPStringRefArray;

typedef struct {
    HYPInfraBinding *items;
    int count;
    int cap;
} HYPInfraBindingArray;

typedef struct {
    HYPImplTrait *items;
    int count;
    int cap;
} HYPImplTraitArray;

typedef struct {
    HYPChannel *items;
    int count;
    int cap;
} HYPChannelArray;

// Full extraction result for one file.
typedef struct HYPFileResult {
    HYPArena arena; // owns local memory; composites may also retain child arenas below

    HYPDefArray defs;
    HYPCallArray calls;
    HYPImportArray imports;
    HYPUsageArray usages;
    HYPThrowArray throws;
    HYPRWArray rw;
    HYPTypeRefArray type_refs;
    HYPEnvAccessArray env_accesses;
    HYPTypeAssignArray type_assigns;
    HYPImplTraitArray impl_traits;       // Rust: impl Trait for Struct pairs
    HYPResolvedCallArray resolved_calls; // LSP-resolved invocations/references (high confidence)
    HYPStringRefArray string_refs;       // URL/config string literals from AST
    HYPInfraBindingArray infra_bindings; // topic→URL pairs from IaC configs
    HYPChannelArray channels;            // Socket.IO / EventEmitter pub/sub participation

    const char *module_qn;      // module qualified name
    const char *namespace_name; // declared namespace/package (Java/Kotlin/C#/PHP), NULL if none
    const char **exports;       // NULL-terminated (NULL if none)
    const char **constants;     // NULL-terminated (NULL if none)
    const char **global_vars;   // NULL-terminated (NULL if none)
    const char **macros;        // NULL-terminated, C/C++ only (NULL if none)

    bool has_error;
    const char *error_msg;
    /* Best-effort parse-coverage signal (experimental). parse_incomplete is true
     * when the parse tree contains tree-sitter ERROR/MISSING nodes — constructs
     * in those regions are silently absent from the graph. error_ranges is a
     * compact "start-end,start-end" list of 1-based line ranges (arena-owned) or
     * NULL. This only marks what we can DETECT: the absence of a flag is NOT a
     * completeness guarantee. Callers should treat a flagged file as "prefer
     * grep here", never treat an unflagged file as provably complete. */
    bool parse_incomplete;
    const char *error_ranges;
    int error_region_count;
    bool is_test_file;
    int imports_count;
    TSTree *cached_tree;     // retained parse tree (caller frees via hyp_free_tree)
    HYPLanguage cached_lang; // language of cached tree (for parser selection)

    // Retained source bytes — copied into `arena` by the parallel
    // extract pass so the fused cross-file LSP step in resolve_worker
    // can run without re-reading the file from disk. NULL when the
    // file exceeded the per-file (100 MB) or total (2 GB) retention
    // cap; in that case the cross-file LSP step is skipped for this
    // file (defs/calls already extracted are unaffected).
    const char *source;
    int source_len;

    // Composite extraction results (currently ObjectScript Studio Export)
    // retain their per-unit results so shallow-copied carrier strings remain
    // valid for the composite's full lifetime. Owned and recursively released
    // by hyp_free_result(); ordinary single-file results leave these zeroed.
    struct HYPFileResult **owned_results;
    int owned_result_count;
} HYPFileResult;

// --- Enclosing function cache ---
// Avoids repeated parent-chain walks for nodes within the same function body.
// Each entry records a function's byte range and its precomputed QN.
#define EFC_SIZE 64 // power of 2 for fast modulo

typedef struct {
    uint32_t start_byte;
    uint32_t end_byte;
    const char *qn;
} EFCEntry;

typedef struct {
    EFCEntry entries[EFC_SIZE];
    int count;
} EFCache;

// --- Extraction context passed to sub-extractors ---

// Module-level string constant map (for constant propagation)
#define HYP_MAX_STRING_CONSTANTS 256
typedef struct {
    const char *names[HYP_MAX_STRING_CONSTANTS];
    const char *values[HYP_MAX_STRING_CONSTANTS];
    int count;
} HYPStringConstantMap;

// Forward declaration: ObjectScript macro table (defined in macro_table.h).
typedef struct HYPMacroTable HYPMacroTable;

// Method-return-type table for ObjectScript variable type inference. Populated
// from definition nodes (method QN -> declared return type) so a later
// `Set x = obj.Method()` can resolve x's class.
#define HYP_RETURN_TYPE_TABLE_CAP 2048

typedef struct {
    const char *method_qn;
    const char *return_type;
} HYPReturnTypeEntry;

typedef struct {
    HYPReturnTypeEntry entries[HYP_RETURN_TYPE_TABLE_CAP];
    int count;
} HYPReturnTypeTable;

typedef struct {
    HYPArena *arena;
    HYPFileResult *result;
    const char *source;
    int source_len;
    HYPLanguage language;
    const char *project;
    const char *rel_path;
    const char *module_qn;
    TSNode root;
    EFCache ef_cache;                            // enclosing function cache
    const char *enclosing_class_qn;              // for nested class QN computation
    HYPStringConstantMap string_constants;       // module-level NAME = "value" pairs
    const HYPMacroTable *macro_table;            // ObjectScript $$$macro table (NULL if none)
    const HYPReturnTypeTable *return_type_table; // ObjectScript method return types (NULL if none)
    /* Set by extract_class_variables around its extract_var_names calls, so a
     * class-body variable def records which class declares it (parent_class)
     * without changing its module-level qualified name. NULL elsewhere. */
    const char *var_parent_class;
} HYPExtractCtx;

// --- Public API ---

// Bind third-party allocators (tree-sitter, sqlite3) to mimalloc as
// defense-in-depth, so they never depend on the fragile MI_OVERRIDE symbol
// override (#424). MUST be called as the very first statement of main(), before
// any sqlite3_open*/sqlite3_initialize (SQLITE_CONFIG_MALLOC returns
// SQLITE_MISUSE once sqlite has initialized).
// Idempotent (static guard); intended for single-threaded startup. hyp_init()
// also calls it so non-main entry points (pipeline passes) still get the binds.
// In the test build (no HYP_BIND_TS_ALLOCATOR) this is a no-op.
void hyp_alloc_init(void);

// Initialize the library. Call once at startup. Returns 0 on success.
int hyp_init(void);

// True when rel_path is in the crash-quarantine set — the newline-delimited list
// of files (HYP_INDEX_QUARANTINE_FILE) the crash supervisor pinned as crashers
// during its single-threaded recovery re-run. Loaded once, lazily; read-only
// after load. hyp_extract_file short-circuits such files to an empty result so no
// pass can crash on them; the pipeline extract loops call this to also REPORT the
// skip as phase="crash". Always false (cheap no-op) when the env var is unset.
bool hyp_index_is_quarantined(const char *rel_path);

// Phase a quarantined file was pinned under: "crash" (a fault signal) or "hang"
// (killed for making no progress). Returns NULL when rel_path is not quarantined.
// Drives the same lazy once-load as hyp_index_is_quarantined. Used by the pipeline
// extract loops to report the skip's phase in skipped[] (falls back to "crash").
const char *hyp_index_quarantine_phase(const char *rel_path);

// Crash-supervisor marker journal (parallel-safe): appends "S <rel_path>" /
// "D <rel_path>" to HYP_INDEX_MARKER_FILE. Files with an S but no D form the
// parent's crash/hang suspect set. No-ops when the env var is unset.
// hyp_extract_file journals its own start/done; long-running per-file phases
// (cross-LSP resolve) call these around their per-file work so a hang there
// is attributed to the RIGHT file instead of a stale extraction marker.
void hyp_index_mark_start(const char *rel_path);
void hyp_index_mark_done(const char *rel_path);

// Extract all data from one file. Caller must call hyp_free_result().
// source must remain valid for the duration of the call.
// timeout_micros: per-file parse timeout in microseconds (0 = no timeout).
HYPFileResult *hyp_extract_file(const char *source, int source_len, HYPLanguage language,
                                const char *project, const char *rel_path, int64_t timeout_micros,
                                const char **extra_defines, // NULL-terminated, or NULL
                                const char **include_paths  // NULL-terminated, or NULL
);

// Pipeline-internal variant of hyp_extract_file() carrying ObjectScript
// per-project tables (macro table + method-return-type table). The public
// hyp_extract_file() is a thin wrapper that passes NULL, NULL for both.
HYPFileResult *hyp_extract_file_ex(
    const char *source, int source_len, HYPLanguage language, const char *project,
    const char *rel_path, int64_t timeout_micros,
    const char **extra_defines,                 // NULL-terminated, or NULL
    const char **include_paths,                 // NULL-terminated, or NULL
    const HYPMacroTable *macro_table,           // ObjectScript macros, or NULL
    const HYPReturnTypeTable *return_type_table // OS return types, or NULL
);

// Free all memory associated with a result.
void hyp_free_result(HYPFileResult *result);

// Free only the cached tree from a result (caller retained it for reuse).
void hyp_free_tree(HYPFileResult *result);

// Free a standalone TSTree pointer (for Go layer cleanup).
void hyp_free_tree_ptr(TSTree *tree);

// Reset the thread-local parser's internal state, releasing slab-allocated
// subtrees. Must be called BEFORE hyp_slab_reset_thread() so the slab rebuild
// doesn't corrupt live parser state.
void hyp_reset_thread_parser(void);

// Destroy the thread-local parser. Call on worker thread exit.
void hyp_destroy_thread_parser(void);

// Shutdown the library. Call once at exit.
void hyp_shutdown(void);

// Profiling: get accumulated parse/extraction times and file count.
typedef struct {
    uint64_t *parse_ns;
    uint64_t *extract_ns;
    uint64_t *files;
} hyp_profile_out_t;
void hyp_get_profile(hyp_profile_out_t out);
uint64_t hyp_get_lsp_ns(void);
uint64_t hyp_get_preprocess_ns(void);
uint64_t hyp_get_files_preprocessed(void);
void hyp_reset_profile(void);

#if defined(HYP_KOTLIN_DEDUP_TEST_API) && HYP_KOTLIN_DEDUP_TEST_API
// Test-build-only operation counter for Kotlin operator-carrier deduplication.
// Production builds do not expose or retain this instrumentation.
void hyp_kotlin_operator_dedup_test_reset(void);
uint64_t hyp_kotlin_operator_dedup_test_comparisons(void);
#endif

#if defined(HYP_CALL_REFERENCE_LOOKUP_TEST_API) && HYP_CALL_REFERENCE_LOOKUP_TEST_API
// Test-build-only work counter for resolving a node's field role while
// classifying value references. Production builds retain no instrumentation.
void hyp_usage_field_lookup_test_reset(void);
uint64_t hyp_usage_field_lookup_test_work(void);
uint64_t hyp_usage_slow_parent_fallback_test_count(void);
#endif

// Toggle C/C++ preprocessor Macro-node extraction (#375). The pipeline enables
// it only for full/advanced index modes (it dominates extraction on macro-dense
// codebases). Default ON. Set before extraction; read-only during.
void hyp_set_macro_extraction(int enabled);
int hyp_macro_extraction_enabled(void);

// --- Internal helpers used by extractors ---

// Growable array push functions (arena-allocated, no individual free needed).
void hyp_defs_push(HYPDefArray *arr, HYPArena *a, HYPDefinition def);
void hyp_calls_push(HYPCallArray *arr, HYPArena *a, HYPCall call);
void hyp_imports_push(HYPImportArray *arr, HYPArena *a, HYPImport imp);
void hyp_usages_push(HYPUsageArray *arr, HYPArena *a, HYPUsage usage);
void hyp_throws_push(HYPThrowArray *arr, HYPArena *a, HYPThrow thr);
void hyp_rw_push(HYPRWArray *arr, HYPArena *a, HYPReadWrite rw);
void hyp_typerefs_push(HYPTypeRefArray *arr, HYPArena *a, HYPTypeRef tr);
void hyp_envaccess_push(HYPEnvAccessArray *arr, HYPArena *a, HYPEnvAccess ea);
void hyp_typeassign_push(HYPTypeAssignArray *arr, HYPArena *a, HYPTypeAssign ta);
void hyp_stringref_push(HYPStringRefArray *arr, HYPArena *a, HYPStringRef sr);
void hyp_infrabinding_push(HYPInfraBindingArray *arr, HYPArena *a, HYPInfraBinding ib);
void hyp_impltrait_push(HYPImplTraitArray *arr, HYPArena *a, HYPImplTrait it);
void hyp_resolvedcall_push(HYPResolvedCallArray *arr, HYPArena *a, HYPResolvedCall rc);
void hyp_channels_push(HYPChannelArray *arr, HYPArena *a, HYPChannel ch);

// --- Sub-extractor entry points ---

void hyp_extract_definitions(HYPExtractCtx *ctx);
void hyp_extract_imports(HYPExtractCtx *ctx);
void hyp_extract_usages(HYPExtractCtx *ctx);
void hyp_extract_semantic(HYPExtractCtx *ctx);
void hyp_extract_type_refs(HYPExtractCtx *ctx);
void hyp_extract_env_accesses(HYPExtractCtx *ctx);
void hyp_extract_type_assigns(HYPExtractCtx *ctx);
void hyp_extract_channels(HYPExtractCtx *ctx);

// Single-pass unified extraction (replaces the 7 calls above except defs+imports).
void hyp_extract_unified(HYPExtractCtx *ctx);

// K8s / Kustomize semantic extractor (called when language is HYP_LANG_K8S or HYP_LANG_KUSTOMIZE).
void hyp_extract_k8s(HYPExtractCtx *ctx);

// --- Label predicates ---

// True when `label` names a TYPE-LIKE container definition — a node that can own
// methods/fields, be a base/embedded type, satisfy/declare an interface, and be a
// target of name→type resolution. The canonical set is:
//   Class, Struct, Interface, Enum, Type, Trait.
// Single source of truth for every type-resolution / registry-seeding /
// INHERITS·IMPLEMENTS / LSP-type-registrar consumer, so adding a new type-like
// label (e.g. "Struct" for Rust/Go/Swift/D structs) updates them all at once
// instead of scattering `|| strcmp(label,"Struct")==0` across the tree.
// `label` may be NULL (returns false). Defined in helpers.c.
bool hyp_label_is_type_like(const char *label);

#endif // HYP_H
