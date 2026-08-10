/*
 * language.c — Language detection from filename and extension.
 *
 * Maps file extensions and special filenames to HYPLanguage enum values.
 * Handles .m disambiguation (Objective-C vs Magma vs MATLAB).
 * Consults the process-global user config (set via hyp_set_user_lang_config)
 * before the built-in lookup table.
 */
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "hyp.h" // HYPLanguage, HYP_LANG_*

#include "foundation/constants.h"
#include "foundation/compat_fs.h"

enum { LANG_SCAN_PASSES = 2 };
#define SLEN(s) (sizeof(s) - 1)
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Extension → Language lookup table ───────────────────────────── */

typedef struct {
    const char *ext; /* including dot, e.g. ".go" */
    HYPLanguage language;
} ext_entry_t;

/* Sorted by extension for binary search (but linear scan is fine for ~120 entries) */
static const ext_entry_t EXT_TABLE[] = {
    /* Bash */
    {".bash", HYP_LANG_BASH},
    {".sh", HYP_LANG_BASH},

    /* C */
    {".c", HYP_LANG_C},

    /* C++ */
    {".cc", HYP_LANG_CPP},
    {".ccm", HYP_LANG_CPP},
    {".cpp", HYP_LANG_CPP},
    {".cppm", HYP_LANG_CPP},
    {".cxx", HYP_LANG_CPP},
    {".h", HYP_LANG_CPP},
    {".hh", HYP_LANG_CPP},
    {".hpp", HYP_LANG_CPP},
    {".hxx", HYP_LANG_CPP},
    {".ixx", HYP_LANG_CPP},

    /* C# */
    {".cs", HYP_LANG_CSHARP},

    /* Clojure */
    {".clj", HYP_LANG_CLOJURE},
    {".cljc", HYP_LANG_CLOJURE},
    {".cljs", HYP_LANG_CLOJURE},

    /* CMake */
    {".cmake", HYP_LANG_CMAKE},

    /* COBOL */
    {".cbl", HYP_LANG_COBOL},
    {".cob", HYP_LANG_COBOL},

    /* Common Lisp */
    {".cl", HYP_LANG_COMMONLISP},
    {".lisp", HYP_LANG_COMMONLISP},
    {".lsp", HYP_LANG_COMMONLISP},

    /* CSS */
    {".css", HYP_LANG_CSS},

    /* CUDA */
    {".cu", HYP_LANG_CUDA},
    {".cuh", HYP_LANG_CUDA},

    /* Dart */
    {".dart", HYP_LANG_DART},

    /* Dockerfile */
    {".dockerfile", HYP_LANG_DOCKERFILE},

    /* Elixir */
    {".ex", HYP_LANG_ELIXIR},
    {".exs", HYP_LANG_ELIXIR},

    /* DotEnv */
    {".env", HYP_LANG_DOTENV},

    /* Elm */
    {".elm", HYP_LANG_ELM},

    /* Emacs Lisp */
    {".el", HYP_LANG_EMACSLISP},

    /* Erlang */
    {".erl", HYP_LANG_ERLANG},

    /* F# */
    {".fs", HYP_LANG_FSHARP},
    {".fsi", HYP_LANG_FSHARP},
    {".fsx", HYP_LANG_FSHARP},

    /* FORM */
    {".frm", HYP_LANG_FORM},
    {".prc", HYP_LANG_FORM},

    /* Fortran */
    {".f03", HYP_LANG_FORTRAN},
    {".f08", HYP_LANG_FORTRAN},
    {".f90", HYP_LANG_FORTRAN},
    {".f95", HYP_LANG_FORTRAN},

    /* GLSL */
    {".frag", HYP_LANG_GLSL},
    {".glsl", HYP_LANG_GLSL},
    {".vert", HYP_LANG_GLSL},

    /* Go */
    {".go", HYP_LANG_GO},

    /* GraphQL */
    {".gql", HYP_LANG_GRAPHQL},
    {".graphql", HYP_LANG_GRAPHQL},

    /* Groovy */
    {".gradle", HYP_LANG_GROOVY},
    {".groovy", HYP_LANG_GROOVY},

    /* Haskell */
    {".hs", HYP_LANG_HASKELL},

    /* HCL / Terraform */
    {".hcl", HYP_LANG_HCL},
    {".tf", HYP_LANG_HCL},

    /* HTML */
    {".htm", HYP_LANG_HTML},
    {".html", HYP_LANG_HTML},

    /* INI */
    {".cfg", HYP_LANG_INI},
    {".conf", HYP_LANG_INI},
    {".ini", HYP_LANG_INI},

    /* Java */
    {".java", HYP_LANG_JAVA},

    /* JavaScript */
    {".js", HYP_LANG_JAVASCRIPT},
    {".jsx", HYP_LANG_JAVASCRIPT},
    {".mjs", HYP_LANG_JAVASCRIPT}, /* ES modules (#197) */
    {".cjs", HYP_LANG_JAVASCRIPT}, /* CommonJS modules */

    /* JSON */
    {".json", HYP_LANG_JSON},

    /* Julia */
    {".jl", HYP_LANG_JULIA},

    /* Kotlin */
    {".kt", HYP_LANG_KOTLIN},
    {".kts", HYP_LANG_KOTLIN},

    /* Lean */
    {".lean", HYP_LANG_LEAN},

    /* Lua */
    {".lua", HYP_LANG_LUA},

    /* Magma */
    {".mag", HYP_LANG_MAGMA},
    {".magma", HYP_LANG_MAGMA},

    /* Makefile */
    {".mk", HYP_LANG_MAKEFILE},

    /* Markdown */
    {".md", HYP_LANG_MARKDOWN},
    {".mdx", HYP_LANG_MARKDOWN},

    /* MATLAB */
    {".m", HYP_LANG_MATLAB},
    {".matlab", HYP_LANG_MATLAB},
    {".mlx", HYP_LANG_MATLAB},

    /* Meson */
    {".meson", HYP_LANG_MESON},

    /* Mojo */
    {".mojo", HYP_LANG_MOJO},

    /* Nix */
    {".nix", HYP_LANG_NIX},

    /* OCaml */
    {".ml", HYP_LANG_OCAML},
    {".mli", HYP_LANG_OCAML},

    /* Perl */
    {".pl", HYP_LANG_PERL},
    {".pm", HYP_LANG_PERL},

    /* PHP */
    {".php", HYP_LANG_PHP},

    /* Protobuf */
    {".proto", HYP_LANG_PROTOBUF},

    /* Python */
    {".py", HYP_LANG_PYTHON},

    /* R — case insensitive handled separately */
    {".R", HYP_LANG_R},
    {".r", HYP_LANG_R},

    /* Ruby */
    {".gemspec", HYP_LANG_RUBY},
    {".rake", HYP_LANG_RUBY},
    {".rb", HYP_LANG_RUBY},

    /* Rust */
    {".rs", HYP_LANG_RUST},

    /* Scala */
    {".sc", HYP_LANG_SCALA},
    {".scala", HYP_LANG_SCALA},

    /* SCSS */
    {".scss", HYP_LANG_SCSS},

    /* SQL */
    {".sql", HYP_LANG_SQL},

    /* Svelte */
    {".svelte", HYP_LANG_SVELTE},

    /* Swift */
    {".swift", HYP_LANG_SWIFT},

    /* SystemVerilog + Verilog */
    {".sv", HYP_LANG_VERILOG},
    {".v", HYP_LANG_VERILOG},

    /* TOML */
    {".toml", HYP_LANG_TOML},

    /* TSX */
    {".tsx", HYP_LANG_TSX},

    /* TypeScript */
    {".ts", HYP_LANG_TYPESCRIPT},
    {".mts", HYP_LANG_TYPESCRIPT}, /* TS ES modules */
    {".cts", HYP_LANG_TYPESCRIPT}, /* TS CommonJS modules */

    /* VimScript */
    {".vim", HYP_LANG_VIMSCRIPT},
    {".vimrc", HYP_LANG_VIMSCRIPT},
    {"justfile", HYP_LANG_JUST},
    {"Justfile", HYP_LANG_JUST},
    {".justfile", HYP_LANG_JUST},
    {".just", HYP_LANG_JUST}, /* `import 'common.just'` target files */
    {"hyprland.conf", HYP_LANG_HYPRLANG},
    {"ssh_config", HYP_LANG_SSHCONFIG},
    {"sshd_config", HYP_LANG_SSHCONFIG},
    {"BUILD", HYP_LANG_STARLARK},
    {"BUILD.bazel", HYP_LANG_STARLARK},
    {"WORKSPACE", HYP_LANG_STARLARK},
    {"WORKSPACE.bazel", HYP_LANG_STARLARK},

    /* BitBake include fragments — `require/include foo.inc` target files.
     * NOTE: .inc is also used by ObjectScript include (macro) files; the
     * ambiguity is resolved by content in hyp_disambiguate_inc(). */
    {".inc", HYP_LANG_BITBAKE},

    /* InterSystems ObjectScript routines (.mac/.int/.rtn unambiguous; .cls is
     * shared with Apex and resolved by content in hyp_disambiguate_cls()). */
    {".mac", HYP_LANG_OBJECTSCRIPT_ROUTINE},
    {".int", HYP_LANG_OBJECTSCRIPT_ROUTINE},
    {".rtn", HYP_LANG_OBJECTSCRIPT_ROUTINE},

    /* Vue */
    {".vue", HYP_LANG_VUE},

    /* Wolfram */
    {".wl", HYP_LANG_WOLFRAM},
    {".wls", HYP_LANG_WOLFRAM},

    /* XML */
    {".xml", HYP_LANG_XML},
    {".xsd", HYP_LANG_XML},
    {".xsl", HYP_LANG_XML},
    {".svg", HYP_LANG_XML},

    /* YAML */
    {".yaml", HYP_LANG_YAML},
    {".yml", HYP_LANG_YAML},

    /* Ada */
    {".adb", HYP_LANG_ADA},

    /* Ada */
    {".ads", HYP_LANG_ADA},

    /* Agda */
    {".agda", HYP_LANG_AGDA},

    /* Astro */
    {".astro", HYP_LANG_ASTRO},

    /* AWK */
    {".awk", HYP_LANG_AWK},

    /* BitBake */
    {".bb", HYP_LANG_BITBAKE},

    /* BitBake */
    {".bbappend", HYP_LANG_BITBAKE},

    /* BitBake */
    {".bbclass", HYP_LANG_BITBAKE},

    /* Beancount */
    {".beancount", HYP_LANG_BEANCOUNT},

    /* BibTeX */
    {".bib", HYP_LANG_BIBTEX},

    /* Bicep */
    {".bicep", HYP_LANG_BICEP},

    /* Blade */
    /* .blade.php handled by userconfig compound extensions, not EXT_TABLE */

    /* Starlark */
    {".bzl", HYP_LANG_STARLARK},

    /* Cairo */
    {".cairo", HYP_LANG_CAIRO},

    /* Cap'n Proto */
    {".capnp", HYP_LANG_CAPNP},

    /* Apex */
    {".cls", HYP_LANG_APEX},

    /* Crystal */
    {".cr", HYP_LANG_CRYSTAL},

    /* CSV */
    {".csv", HYP_LANG_CSV},

    /* D */
    {".d", HYP_LANG_DLANG},

    /* Diff */
    {".diff", HYP_LANG_DIFF},

    /* Pascal */
    {".dpr", HYP_LANG_PASCAL},

    /* DeviceTree */
    {".dts", HYP_LANG_DEVICETREE},

    /* DeviceTree */
    {".dtsi", HYP_LANG_DEVICETREE},

    /* FunC */
    {".fc", HYP_LANG_FUNC},

    /* Fish */
    {".fish", HYP_LANG_FISH},

    /* Fennel */
    {".fnl", HYP_LANG_FENNEL},

    /* HLSL */
    {".fx", HYP_LANG_HLSL},

    /* GDScript */
    {".gd", HYP_LANG_GDSCRIPT},

    /* Gleam */
    {".gleam", HYP_LANG_GLEAM},

    /* GN */
    {".gn", HYP_LANG_GN},

    /* GN */
    {".gni", HYP_LANG_GN},

    /* Go Template */
    {".gotmpl", HYP_LANG_GOTEMPLATE},
    {".tpl", HYP_LANG_GOTEMPLATE}, /* Helm _helpers.tpl named-template definitions */

    /* Hare */
    {".ha", HYP_LANG_HARE},

    /* Hyprlang */
    {".hl", HYP_LANG_HYPRLANG},

    /* HLSL */
    {".hlsl", HYP_LANG_HLSL},

    /* HLSL */
    {".hlsli", HYP_LANG_HLSL},

    /* ISPC */
    {".ispc", HYP_LANG_ISPC},

    /* Jinja2 */
    {".j2", HYP_LANG_JINJA2},

    /* Janet */
    {".janet", HYP_LANG_JANET},

    /* Jinja2 */
    {".jinja", HYP_LANG_JINJA2},

    /* Jinja2 */
    {".jinja2", HYP_LANG_JINJA2},

    /* JSON5 */
    {".json5", HYP_LANG_JSON5},

    /* Jsonnet */
    {".jsonnet", HYP_LANG_JSONNET},

    /* KDL */
    {".kdl", HYP_LANG_KDL},

    /* Linker Script */
    {".ld", HYP_LANG_LINKERSCRIPT},

    /* Linker Script */
    {".lds", HYP_LANG_LINKERSCRIPT},

    /* Jsonnet */
    {".libsonnet", HYP_LANG_JSONNET},

    /* Liquid */
    {".liquid", HYP_LANG_LIQUID},

    /* LLVM IR */
    {".ll", HYP_LANG_LLVM_IR},

    /* Pascal */
    {".lpr", HYP_LANG_PASCAL},

    /* Luau */
    {".luau", HYP_LANG_LUAU},

    /* Qt QML */
    {".qml", HYP_LANG_QML},

    /* CFML / ColdFusion — .cfc components are script-dialect; .cfm are tag templates */
    {".cfc", HYP_LANG_CFSCRIPT},
    {".cfm", HYP_LANG_CFML},

    /* Mermaid */
    {".mermaid", HYP_LANG_MERMAID},

    /* Mermaid */
    {".mmd", HYP_LANG_MERMAID},

    /* Move */
    {".move", HYP_LANG_MOVE},

    /* NASM */
    {".nasm", HYP_LANG_NASM},

    /* Nickel */
    {".ncl", HYP_LANG_NICKEL},

    /* Nim */

    /* Nim */

    /* Squirrel */
    {".nut", HYP_LANG_SQUIRREL},

    /* Odin */
    {".odin", HYP_LANG_ODIN},

    /* DeviceTree */
    {".overlay", HYP_LANG_DEVICETREE},

    /* Pascal */
    {".pas", HYP_LANG_PASCAL},

    /* Diff */
    {".patch", HYP_LANG_DIFF},

    /* Pine Script */
    {".pine", HYP_LANG_PINE},

    /* Pkl */
    {".pkl", HYP_LANG_PKL},

    /* PO */
    {".po", HYP_LANG_PO},

    /* Pony */
    {".pony", HYP_LANG_PONY},

    /* PO */
    {".pot", HYP_LANG_PO},

    /* Puppet */
    {".pp", HYP_LANG_PUPPET},

    /* Prisma */
    {".prisma", HYP_LANG_PRISMA},

    /* Properties */
    {".properties", HYP_LANG_PROPERTIES},

    /* PowerShell */
    {".ps1", HYP_LANG_POWERSHELL},

    /* PowerShell */
    {".psd1", HYP_LANG_POWERSHELL},

    /* PowerShell */
    {".psm1", HYP_LANG_POWERSHELL},

    /* PureScript */
    {".purs", HYP_LANG_PURESCRIPT},

    /* ReScript */
    {".res", HYP_LANG_RESCRIPT},

    /* ReScript */
    {".resi", HYP_LANG_RESCRIPT},

    /* Regex */
    {".re", HYP_LANG_REGEX},

    /* Racket */
    {".rkt", HYP_LANG_RACKET},

    /* RON */
    {".ron", HYP_LANG_RON},

    /* reStructuredText */
    {".rst", HYP_LANG_RST},

    /* Assembly */
    {".s", HYP_LANG_ASSEMBLY},

    /* Assembly */
    {".S", HYP_LANG_ASSEMBLY},

    /* Scheme */
    {".scm", HYP_LANG_SCHEME},

    /* Slang */
    {".slang", HYP_LANG_SLANG},

    /* Smali */
    {".smali", HYP_LANG_SMALI},

    /* Smithy */
    {".smithy", HYP_LANG_SMITHY},

    /* Solidity */
    {".sol", HYP_LANG_SOLIDITY},

    /* SOQL */
    {".soql", HYP_LANG_SOQL},

    /* SOSL */
    {".sosl", HYP_LANG_SOSL},

    /* Scheme */
    {".ss", HYP_LANG_SCHEME},

    /* Starlark */
    {".star", HYP_LANG_STARLARK},

    /* SystemVerilog */

    /* SystemVerilog */

    /* Sway */
    {".sw", HYP_LANG_SWAY},

    /* Tcl */
    {".tcl", HYP_LANG_TCL},

    /* TableGen */
    {".td", HYP_LANG_TABLEGEN},

    /* Templ */
    {".templ", HYP_LANG_TEMPL},

    /* Thrift */
    {".thrift", HYP_LANG_THRIFT},

    /* Teal */
    {".tl", HYP_LANG_TEAL},

    /* TLA+ */
    {".tla", HYP_LANG_TLAPLUS},

    /* Go Template */
    {".tmpl", HYP_LANG_GOTEMPLATE},

    /* Apex */
    {".trigger", HYP_LANG_APEX},

    /* Typst */
    {".typ", HYP_LANG_TYPST},

    /* VHDL */
    {".vhd", HYP_LANG_VHDL},

    /* VHDL */
    {".vhdl", HYP_LANG_VHDL},

    /* WGSL */
    {".wgsl", HYP_LANG_WGSL},

    /* WIT */
    {".wit", HYP_LANG_WIT},

    /* Zsh */
    {".zsh", HYP_LANG_ZSH},

    /* Zig */
    {".zig", HYP_LANG_ZIG},
};

#define EXT_TABLE_SIZE (sizeof(EXT_TABLE) / sizeof(EXT_TABLE[0]))

/* ── Special filename → Language lookup ──────────────────────────── */

typedef struct {
    const char *filename;
    HYPLanguage language;
} filename_entry_t;

static const filename_entry_t FILENAME_TABLE[] = {
    {"CMakeLists.txt", HYP_LANG_CMAKE},
    {"Dockerfile", HYP_LANG_DOCKERFILE},
    {"GNUmakefile", HYP_LANG_MAKEFILE},
    {"Makefile", HYP_LANG_MAKEFILE},
    {"makefile", HYP_LANG_MAKEFILE},
    {"meson.build", HYP_LANG_MESON},
    {"meson.options", HYP_LANG_MESON},
    {"meson_options.txt", HYP_LANG_MESON},
    {"kustomization.yaml", HYP_LANG_KUSTOMIZE},
    {"kustomization.yml", HYP_LANG_KUSTOMIZE},
    /* Note: FILENAME_TABLE uses case-sensitive strcmp, so mixed-case variants
     * (e.g. "Kustomization.yaml") are not matched here.  They fall through to
     * HYP_LANG_YAML and are re-classified by hyp_is_kustomize_file() in
     * pass_k8s.c, which performs a case-insensitive comparison.  This is the
     * intended behaviour — no additional entries are needed. */
    {".vimrc", HYP_LANG_VIMSCRIPT},
    {".zshrc", HYP_LANG_ZSH},
    {".zshenv", HYP_LANG_ZSH},
    {".zprofile", HYP_LANG_ZSH},
    {"justfile", HYP_LANG_JUST},
    {"Justfile", HYP_LANG_JUST},
    {".justfile", HYP_LANG_JUST},
    {"hyprland.conf", HYP_LANG_HYPRLANG},
    {"ssh_config", HYP_LANG_SSHCONFIG},
    {"sshd_config", HYP_LANG_SSHCONFIG},
    {".ssh/config", HYP_LANG_SSHCONFIG},
    {"BUILD", HYP_LANG_STARLARK},
    {"BUILD.bazel", HYP_LANG_STARLARK},
    {"WORKSPACE", HYP_LANG_STARLARK},
    {"WORKSPACE.bazel", HYP_LANG_STARLARK},
    {"requirements.txt", HYP_LANG_REQUIREMENTS},
    {"requirements-dev.txt", HYP_LANG_REQUIREMENTS},
    {"requirements-test.txt", HYP_LANG_REQUIREMENTS},
    {"Kconfig", HYP_LANG_KCONFIG},
    {"go.mod", HYP_LANG_GOMOD},
    {".env", HYP_LANG_DOTENV},
    {".env.local", HYP_LANG_DOTENV},
    {".gitattributes", HYP_LANG_GITATTRIBUTES},

};

#define FILENAME_TABLE_SIZE (sizeof(FILENAME_TABLE) / sizeof(FILENAME_TABLE[0]))

/* ── Language names ──────────────────────────────────────────────── */

static const char *LANG_NAMES[HYP_LANG_COUNT] = {
    [HYP_LANG_GO] = "Go",
    [HYP_LANG_PYTHON] = "Python",
    [HYP_LANG_JAVASCRIPT] = "JavaScript",
    [HYP_LANG_TYPESCRIPT] = "TypeScript",
    [HYP_LANG_TSX] = "TSX",
    [HYP_LANG_RUST] = "Rust",
    [HYP_LANG_JAVA] = "Java",
    [HYP_LANG_CPP] = "C++",
    [HYP_LANG_CSHARP] = "C#",
    [HYP_LANG_PHP] = "PHP",
    [HYP_LANG_LUA] = "Lua",
    [HYP_LANG_SCALA] = "Scala",
    [HYP_LANG_KOTLIN] = "Kotlin",
    [HYP_LANG_RUBY] = "Ruby",
    [HYP_LANG_C] = "C",
    [HYP_LANG_BASH] = "Bash",
    [HYP_LANG_ZIG] = "Zig",
    [HYP_LANG_ELIXIR] = "Elixir",
    [HYP_LANG_HASKELL] = "Haskell",
    [HYP_LANG_OCAML] = "OCaml",
    [HYP_LANG_OBJC] = "Objective-C",
    [HYP_LANG_SWIFT] = "Swift",
    [HYP_LANG_DART] = "Dart",
    [HYP_LANG_PERL] = "Perl",
    [HYP_LANG_GROOVY] = "Groovy",
    [HYP_LANG_ERLANG] = "Erlang",
    [HYP_LANG_R] = "R",
    [HYP_LANG_HTML] = "HTML",
    [HYP_LANG_CSS] = "CSS",
    [HYP_LANG_SCSS] = "SCSS",
    [HYP_LANG_YAML] = "YAML",
    [HYP_LANG_TOML] = "TOML",
    [HYP_LANG_HCL] = "HCL",
    [HYP_LANG_SQL] = "SQL",
    [HYP_LANG_DOCKERFILE] = "Dockerfile",
    [HYP_LANG_CLOJURE] = "Clojure",
    [HYP_LANG_FSHARP] = "F#",
    [HYP_LANG_JULIA] = "Julia",
    [HYP_LANG_VIMSCRIPT] = "VimScript",
    [HYP_LANG_NIX] = "Nix",
    [HYP_LANG_COMMONLISP] = "Common Lisp",
    [HYP_LANG_ELM] = "Elm",
    [HYP_LANG_FORTRAN] = "Fortran",
    [HYP_LANG_CUDA] = "CUDA",
    [HYP_LANG_COBOL] = "COBOL",
    [HYP_LANG_VERILOG] = "Verilog",
    [HYP_LANG_EMACSLISP] = "Emacs Lisp",
    [HYP_LANG_JSON] = "JSON",
    [HYP_LANG_XML] = "XML",
    [HYP_LANG_MARKDOWN] = "Markdown",
    [HYP_LANG_MAKEFILE] = "Makefile",
    [HYP_LANG_CMAKE] = "CMake",
    [HYP_LANG_PROTOBUF] = "Protobuf",
    [HYP_LANG_GRAPHQL] = "GraphQL",
    [HYP_LANG_VUE] = "Vue",
    [HYP_LANG_SVELTE] = "Svelte",
    [HYP_LANG_MESON] = "Meson",
    [HYP_LANG_GLSL] = "GLSL",
    [HYP_LANG_INI] = "INI",
    [HYP_LANG_MATLAB] = "MATLAB",
    [HYP_LANG_LEAN] = "Lean",
    [HYP_LANG_FORM] = "FORM",
    [HYP_LANG_MAGMA] = "Magma",
    [HYP_LANG_WOLFRAM] = "Wolfram",
    [HYP_LANG_KUSTOMIZE] = "Kustomize",
    [HYP_LANG_K8S] = "Kubernetes",
    [HYP_LANG_PINE] = "PineScript",
    [HYP_LANG_SOLIDITY] = "Solidity",
    [HYP_LANG_TYPST] = "Typst",
    [HYP_LANG_GDSCRIPT] = "GDScript",
    [HYP_LANG_GLEAM] = "Gleam",
    [HYP_LANG_POWERSHELL] = "PowerShell",
    [HYP_LANG_PASCAL] = "Pascal",
    [HYP_LANG_DLANG] = "D",
    [HYP_LANG_NIM] = "Nim",
    [HYP_LANG_SCHEME] = "Scheme",
    [HYP_LANG_FENNEL] = "Fennel",
    [HYP_LANG_FISH] = "Fish",
    [HYP_LANG_AWK] = "AWK",
    [HYP_LANG_ZSH] = "Zsh",
    [HYP_LANG_TCL] = "Tcl",
    [HYP_LANG_ADA] = "Ada",
    [HYP_LANG_AGDA] = "Agda",
    [HYP_LANG_RACKET] = "Racket",
    [HYP_LANG_ODIN] = "Odin",
    [HYP_LANG_RESCRIPT] = "ReScript",
    [HYP_LANG_PURESCRIPT] = "PureScript",
    [HYP_LANG_NICKEL] = "Nickel",
    [HYP_LANG_CRYSTAL] = "Crystal",
    [HYP_LANG_TEAL] = "Teal",
    [HYP_LANG_HARE] = "Hare",
    [HYP_LANG_PONY] = "Pony",
    [HYP_LANG_LUAU] = "Luau",
    [HYP_LANG_QML] = "QML",
    [HYP_LANG_CFSCRIPT] = "CFML",
    [HYP_LANG_CFML] = "CFML",
    [HYP_LANG_JANET] = "Janet",
    [HYP_LANG_SWAY] = "Sway",
    [HYP_LANG_NASM] = "NASM",
    [HYP_LANG_ASSEMBLY] = "Assembly",
    [HYP_LANG_ASTRO] = "Astro",
    [HYP_LANG_BLADE] = "Blade",
    [HYP_LANG_JUST] = "Just",
    [HYP_LANG_GOTEMPLATE] = "Go Template",
    [HYP_LANG_TEMPL] = "Templ",
    [HYP_LANG_LIQUID] = "Liquid",
    [HYP_LANG_JINJA2] = "Jinja2",
    [HYP_LANG_PRISMA] = "Prisma",
    [HYP_LANG_HYPRLANG] = "Hyprlang",
    [HYP_LANG_DOTENV] = "DotEnv",
    [HYP_LANG_SYSTEMVERILOG] = "SystemVerilog",
    [HYP_LANG_DIFF] = "Diff",
    [HYP_LANG_WGSL] = "WGSL",
    [HYP_LANG_KDL] = "KDL",
    [HYP_LANG_JSON5] = "JSON5",
    [HYP_LANG_JSONNET] = "Jsonnet",
    [HYP_LANG_RON] = "RON",
    [HYP_LANG_THRIFT] = "Thrift",
    [HYP_LANG_CAPNP] = "Cap'n Proto",
    [HYP_LANG_PROPERTIES] = "Properties",
    [HYP_LANG_SSHCONFIG] = "SSH Config",
    [HYP_LANG_BIBTEX] = "BibTeX",
    [HYP_LANG_STARLARK] = "Starlark",
    [HYP_LANG_BICEP] = "Bicep",
    [HYP_LANG_CSV] = "CSV",
    [HYP_LANG_REQUIREMENTS] = "Requirements",
    [HYP_LANG_HLSL] = "HLSL",
    [HYP_LANG_VHDL] = "VHDL",
    [HYP_LANG_DEVICETREE] = "DeviceTree",
    [HYP_LANG_LINKERSCRIPT] = "Linker Script",
    [HYP_LANG_GN] = "GN",
    [HYP_LANG_KCONFIG] = "Kconfig",
    [HYP_LANG_BITBAKE] = "BitBake",
    [HYP_LANG_SMALI] = "Smali",
    [HYP_LANG_TABLEGEN] = "TableGen",
    [HYP_LANG_ISPC] = "ISPC",
    [HYP_LANG_CAIRO] = "Cairo",
    [HYP_LANG_MOVE] = "Move",
    [HYP_LANG_SQUIRREL] = "Squirrel",
    [HYP_LANG_FUNC] = "FunC",
    [HYP_LANG_REGEX] = "Regex",
    [HYP_LANG_JSDOC] = "JSDoc",
    [HYP_LANG_RST] = "reStructuredText",
    [HYP_LANG_BEANCOUNT] = "Beancount",
    [HYP_LANG_MERMAID] = "Mermaid",
    [HYP_LANG_PUPPET] = "Puppet",
    [HYP_LANG_PO] = "PO",
    [HYP_LANG_GITATTRIBUTES] = "gitattributes",
    [HYP_LANG_GITIGNORE] = "gitignore",
    [HYP_LANG_SLANG] = "Slang",
    [HYP_LANG_LLVM_IR] = "LLVM IR",
    [HYP_LANG_SMITHY] = "Smithy",
    [HYP_LANG_WIT] = "WIT",
    [HYP_LANG_TLAPLUS] = "TLA+",
    [HYP_LANG_PKL] = "Pkl",
    [HYP_LANG_GOMOD] = "Go Mod",
    [HYP_LANG_APEX] = "Apex",
    [HYP_LANG_SOQL] = "SOQL",
    [HYP_LANG_SOSL] = "SOSL",
    [HYP_LANG_MOJO] = "Mojo",
    [HYP_LANG_OBJECTSCRIPT_UDL] = "ObjectScript UDL",
    [HYP_LANG_OBJECTSCRIPT_ROUTINE] = "ObjectScript Routine",
    [HYP_LANG_OBJECTSCRIPT_EXPORT] = "ObjectScript Export XML",

};

/* ── Prompt names (the `ask` instruct prefix, NEXT-STEPS.md §2.1) ── */

/*
 * The `ask` lane encodes a query behind a per-language instruct prefix:
 *
 *   Instruct: Given a natural-language description of {language} code,
 *   retrieve the declaration it describes.
 *
 * {language} MUST be the display name a developer writes in prose — the
 * spelling the embedding model saw in training — never the grammar id.
 * The C++ rendering has to be byte-identical to the prefix ctxengine
 * measured R@1 0.7833 / R@10 0.9400 with (QWEN_INSTRUCT in
 * ctxengine/src/ctxengine/encoders.py); render "cpp" instead and the
 * prefix still looks fine, still returns ranked results, and the measured
 * number quietly stops applying to it. Silence is the whole failure mode,
 * which is why the unknown path below refuses rather than substitutes.
 *
 * LANG_NAMES above is ALREADY that name for 155 of the 163 languages
 * (it has said "C++", "C#", "Objective-C", "Common Lisp" since the Go
 * port), so it stays the single source of truth and this is a SPARSE
 * OVERRIDE holding only the entries where the file label and the prose
 * name genuinely differ. A NULL slot is not a gap — it is the claim that
 * LANG_NAMES already reads correctly in the sentence above, and
 * hyp_language_prompt_name() falls through to it.
 *
 * Keeping the override separate is also what makes it safe: nothing but
 * the prompt path reads this table, so a prompt-only spelling cannot
 * change `hyp architecture` output, the store's language rollup, or the
 * userconfig aliases in userconfig.c.
 */
static const char *LANG_PROMPT_NAMES[HYP_LANG_COUNT] = {
    /* .tsx is TypeScript with JSX syntax enabled, not a separate language;
     * §2.1 names this one explicitly. "TSX" stays in LANG_NAMES because it
     * is a useful FILE label (it distinguishes .ts from .tsx in tool
     * output) and a useless LANGUAGE name in a sentence about code. */
    [HYP_LANG_TSX] = "TypeScript",

    /* UDL, routine and Studio Export XML are three storage formats for one
     * language, InterSystems ObjectScript — the suffix names the file
     * format, not the language, and nobody writes "ObjectScript UDL code".
     * Note HYP_LANG_OBJECTSCRIPT_EXPORT has no grammar row at all
     * (lang_specs.c:2625): the pipeline transcodes Export XML to UDL and
     * re-extracts it as HYP_LANG_OBJECTSCRIPT_UDL, so its declarations
     * already reach the prefix tagged UDL. All three collapse to the one
     * name a person would say. */
    [HYP_LANG_OBJECTSCRIPT_UDL] = "ObjectScript",
    [HYP_LANG_OBJECTSCRIPT_ROUTINE] = "ObjectScript",
    [HYP_LANG_OBJECTSCRIPT_EXPORT] = "ObjectScript",

    /* "Go Mod" is a label nobody writes; the file is a Go module manifest
     * and the prose name is "Go module" (or "go.mod"). */
    [HYP_LANG_GOMOD] = "Go module",

    /* TradingView spells its language "Pine Script", two words. */
    [HYP_LANG_PINE] = "Pine Script",

    /* The convention is spelled "dotenv" (or ".env") everywhere it appears
     * — in the library name, the filename and the docs. "DotEnv" is a
     * camel-cased invention of this table. */
    [HYP_LANG_DOTENV] = "dotenv",

    /* "Requirements" alone names nothing; the artefact is a pip
     * requirements file. */
    [HYP_LANG_REQUIREMENTS] = "pip requirements",
};

/* Adding a language means adding a row to lang_specs.c (already gated by a
 * _Static_assert there) AND a name to LANG_NAMES above. A missing name is
 * the silent failure §2.1 warns about: hyp_language_name() would answer
 * "Unknown" and hyp_language_prompt_name() would answer NULL, so `ask`
 * would refuse to run on that language with nothing in the build saying
 * why. C cannot check a designated-initialiser array for NULL holes at
 * compile time (an array subscript is not a constant expression), so the
 * exhaustiveness gate is the runtime test lang_all_have_prompt_names in
 * tests/test_language.c — `make -f Makefile.hyp test`. This pin exists so
 * that adding language 164 cannot reach that test by accident: it stops
 * the build HERE, in the file that owns the names.
 *
 * When you bump it: add the LANG_NAMES entry, then decide whether the
 * display name is also the name a developer writes in prose. If it is not,
 * add an override above. */
/* Spelled as a subtraction rather than `==` on purpose: comparing two
 * different enum types trips GCC's -Wenum-compare, and casting either side
 * to int to silence it trips clang-tidy's readability-redundant-casting
 * (enum constants are already int in C). Subtraction offends neither and
 * says the same thing. */
enum { HYP_LANG_NAMED_COUNT = 163 };
_Static_assert(HYP_LANG_COUNT - HYP_LANG_NAMED_COUNT == 0,
               "a language was added: give it a LANG_NAMES entry, check whether it needs a "
               "LANG_PROMPT_NAMES override, then bump HYP_LANG_NAMED_COUNT");

/* ── Public API ──────────────────────────────────────────────────── */

HYPLanguage hyp_language_for_extension(const char *ext) {
    if (!ext || !ext[0]) {
        return HYP_LANG_COUNT;
    }

    /* Check user-defined overrides first */
    const hyp_userconfig_t *ucfg = hyp_get_user_lang_config();
    if (ucfg) {
        HYPLanguage ulang = hyp_userconfig_lookup(ucfg, ext);
        if (ulang != HYP_LANG_COUNT) {
            return ulang;
        }
    }

    for (size_t i = 0; i < EXT_TABLE_SIZE; i++) {
        if (strcmp(EXT_TABLE[i].ext, ext) == 0) {
            return EXT_TABLE[i].language;
        }
    }
    return HYP_LANG_COUNT;
}

HYPLanguage hyp_language_for_filename(const char *filename) {
    if (!filename || !filename[0]) {
        return HYP_LANG_COUNT;
    }

    /* Check special filenames first */
    for (size_t i = 0; i < FILENAME_TABLE_SIZE; i++) {
        if (strcmp(FILENAME_TABLE[i].filename, filename) == 0) {
            return FILENAME_TABLE[i].language;
        }
    }

    /* DotEnv variant filenames (".env.local", ".env.production", …): the
     * filename starts with ".env." but its last "extension" (e.g. ".local")
     * is not a real language extension.  Match the dotenv convention used by
     * pass_envscan/pass_infrascan (".env" exact, ".env." prefix, "*.env"
     * suffix) so file-index routing agrees with direct extraction. */
    if (strncmp(filename, ".env.", SLEN(".env.")) == 0) {
        return HYP_LANG_DOTENV;
    }

    /* Fall back to extension-based lookup.
     * For compound extensions (e.g. ".blade.php") defined in the user config,
     * scan from the first dot in the basename toward the last, checking user
     * config at each position.  Built-in extensions use the last dot only. */
    const char *last_dot = strrchr(filename, '.');
    if (!last_dot) {
        return HYP_LANG_COUNT;
    }

    /* Probe compound extensions (e.g. ".blade.php") from the first dot toward
     * the last. Built-in compounds are checked first so e.g. Laravel Blade
     * templates map to Blade rather than the single-extension fallback (PHP);
     * user config can still add more (#258). */
    static const struct {
        const char *ext;
        HYPLanguage lang;
    } COMPOUND_EXT_TABLE[] = {
        {".blade.php", HYP_LANG_BLADE},
    };
    const hyp_userconfig_t *ucfg = hyp_get_user_lang_config();
    const char *p = strchr(filename, '.');
    while (p && p < last_dot) {
        for (size_t i = 0; i < sizeof(COMPOUND_EXT_TABLE) / sizeof(COMPOUND_EXT_TABLE[0]); i++) {
            if (strcmp(p, COMPOUND_EXT_TABLE[i].ext) == 0) {
                return COMPOUND_EXT_TABLE[i].lang;
            }
        }
        if (ucfg) {
            HYPLanguage lang = hyp_userconfig_lookup(ucfg, p);
            if (lang != HYP_LANG_COUNT) {
                return lang;
            }
        }
        p = strchr(p + SKIP_ONE, '.');
    }

    /* Standard single-extension lookup (built-ins + user overrides). */
    return hyp_language_for_extension(last_dot);
}

const char *hyp_language_name(HYPLanguage lang) {
    if (lang < 0 || lang >= HYP_LANG_COUNT) {
        return "Unknown";
    }
    return LANG_NAMES[lang] ? LANG_NAMES[lang] : "Unknown";
}

const char *hyp_language_prompt_name(HYPLanguage lang) {
    if (lang < 0 || lang >= HYP_LANG_COUNT) {
        return NULL;
    }
    const char *name = LANG_PROMPT_NAMES[lang] ? LANG_PROMPT_NAMES[lang] : LANG_NAMES[lang];
    /* NULL and "" both mean "no name for this language". Returning
     * "Unknown" here — what hyp_language_name() does, correctly, for a
     * human reading tool output — would render a well-formed instruct
     * prefix that is not the one anything was measured with, and nothing
     * would fail. Refuse instead, and let the caller decide out loud. */
    if (!name || !name[0]) {
        return NULL;
    }
    return name;
}

/* ── .m file disambiguation ──────────────────────────────────────── */

/* Simple substring search helper */
static bool str_contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static bool has_objc_markers(const char *buf) {
    return str_contains(buf, "@interface") || str_contains(buf, "@implementation") ||
           str_contains(buf, "@protocol") || str_contains(buf, "@property") ||
           str_contains(buf, "#import") || str_contains(buf, "@selector") ||
           str_contains(buf, "@encode") || str_contains(buf, "@synthesize") ||
           str_contains(buf, "@dynamic");
}

static bool has_magma_end_markers(const char *buf) {
    return str_contains(buf, "end function;") || str_contains(buf, "end procedure;") ||
           str_contains(buf, "end intrinsic;") || str_contains(buf, "end if;") ||
           str_contains(buf, "end for;") || str_contains(buf, "end while;");
}

/* Check for "intrinsic Name(" or "procedure Name(" patterns. */
static bool has_magma_callable_pattern(const char *buf) {
    const char *markers[] = {"intrinsic ", "procedure "};
    for (int i = 0; i < LANG_SCAN_PASSES; i++) {
        const char *p = strstr(buf, markers[i]);
        if (!p) {
            continue;
        }
        p += strlen(markers[i]);
        while (*p && isalpha((unsigned char)*p)) {
            p++;
        }
        if (*p == '(') {
            return true;
        }
    }
    return false;
}

/* Scan lines for MATLAB-specific markers (function/classdef/%%). */
static bool has_matlab_line_markers(const char *buf) {
    const char *line = buf;
    while (*line) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "function ", SLEN("function ")) == 0 ||
            strncmp(p, "function\t", SLEN("function\t")) == 0 ||
            strncmp(p, "classdef ", SLEN("classdef ")) == 0 ||
            strncmp(p, "classdef\t", SLEN("classdef\t")) == 0 || strncmp(p, "%%", PAIR_LEN) == 0 ||
            (*p == '%' && *(p + SKIP_ONE) != '{')) {
            return true;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) {
            break;
        }
        line = nl + SKIP_ONE;
    }
    return false;
}

HYPLanguage hyp_disambiguate_m(const char *path) {
    if (!path) {
        return HYP_LANG_MATLAB;
    }

    FILE *f = hyp_fopen(path, "r");
    if (!f) {
        return HYP_LANG_MATLAB;
    }

    /* Read first 4KB */
    char buf[HYP_SZ_4K + SKIP_ONE];
    size_t n = fread(buf, SKIP_ONE, HYP_SZ_4K, f);
    buf[n] = '\0';
    (void)fclose(f);

    if (has_objc_markers(buf)) {
        return HYP_LANG_OBJC;
    }
    if (has_magma_end_markers(buf)) {
        return HYP_LANG_MAGMA;
    }
    if ((str_contains(buf, "intrinsic ") || str_contains(buf, "procedure ")) &&
        has_magma_callable_pattern(buf)) {
        return HYP_LANG_MAGMA;
    }
    if (has_matlab_line_markers(buf)) {
        return HYP_LANG_MATLAB;
    }

    return HYP_LANG_MATLAB;
}

/* Disambiguate .cls files: shared by InterSystems ObjectScript UDL and
 * Salesforce Apex. ObjectScript class files begin with a line of the form
 * "Class <UppercasePackage>...". Defaults to Apex on any doubt. */
HYPLanguage hyp_disambiguate_cls(const char *path) {
    if (!path) {
        return HYP_LANG_APEX;
    }

    FILE *f = hyp_fopen(path, "r");
    if (!f) {
        return HYP_LANG_APEX;
    }

    char buf[HYP_SZ_4K + SKIP_ONE];
    size_t n = fread(buf, SKIP_ONE, HYP_SZ_4K, f);
    buf[n] = '\0';
    (void)fclose(f);

    const char *line = buf;
    while (*line) {
        if (strncmp(line, "Class ", SLEN("Class ")) == 0 &&
            isupper((unsigned char)line[SLEN("Class ")])) {
            return HYP_LANG_OBJECTSCRIPT_UDL;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) {
            break;
        }
        line = nl + SKIP_ONE;
    }
    return HYP_LANG_APEX;
}

/* Disambiguate .inc files: shared by BitBake include fragments and
 * InterSystems ObjectScript include (macro) files. ObjectScript .inc files are
 * predominantly macro definitions ("#define NAME ..." / "#def1arg NAME ...");
 * some also carry a "ROUTINE <Name>" header. The macro-preprocessor directives
 * are the strongest signal because that is the primary content of an .inc file,
 * whereas BitBake uses '#' only for "# comment" lines (always '#' + space).
 * We therefore match ObjectScript preprocessor directives ('#' immediately
 * followed by 'def'/';'), which BitBake never produces. Defaults to BitBake on
 * any doubt (preserves existing behaviour). */
HYPLanguage hyp_disambiguate_inc(const char *path) {
    if (!path) {
        return HYP_LANG_BITBAKE;
    }

    FILE *f = hyp_fopen(path, "r");
    if (!f) {
        return HYP_LANG_BITBAKE;
    }

    char buf[HYP_SZ_4K + SKIP_ONE];
    size_t n = fread(buf, SKIP_ONE, HYP_SZ_4K, f);
    buf[n] = '\0';
    (void)fclose(f);

    const char *line = buf;
    while (*line) {
        /* ObjectScript include header: a line beginning "ROUTINE <Uppercase>". */
        if (strncmp(line, "ROUTINE ", SLEN("ROUTINE ")) == 0 &&
            isupper((unsigned char)line[SLEN("ROUTINE ")])) {
            return HYP_LANG_OBJECTSCRIPT_ROUTINE;
        }
        /* ObjectScript macro directives — the primary content of .inc files.
         * "#define"/"#def1arg" (macro defs) and "#;" (line comment). BitBake's
         * only '#' use is "# comment" (hash + space), so these never collide. */
        if (strncmp(line, "#define", SLEN("#define")) == 0 ||
            strncmp(line, "#def1arg", SLEN("#def1arg")) == 0 ||
            strncmp(line, "#;", SLEN("#;")) == 0) {
            return HYP_LANG_OBJECTSCRIPT_ROUTINE;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) {
            break;
        }
        line = nl + SKIP_ONE;
    }
    return HYP_LANG_BITBAKE;
}
