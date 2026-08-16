/*
 * fqn_core.h — the qualified-name derivation, in ONE place.
 *
 * A qualified name is `project.dir.parts.stem.name`. Two layers need to derive
 * it: the pipeline derives QNs to LOOK UP (malloc'd, `src/pipeline/fqn.c`) and
 * extraction derives the QNs it WRITES (arena-allocated,
 * `internal/hyp/helpers.c`). The two meet in the registry, which builds
 * "<module_qn>.<callee>" from a pipeline-derived module QN and looks it up
 * against extraction-written keys — so a byte of disagreement between them is
 * not an error anywhere, it is a silently missing edge.
 *
 * Two implementations that must agree can only be kept in agreement by a test
 * that thinks of the input. This header exists so there is nothing to keep in
 * agreement: both sides are thin allocator wrappers over the functions below,
 * and a difference between them is a compile-time impossibility rather than a
 * test's responsibility. tests/test_fqn_differential.c still compares the two
 * wrappers, because "thin" is a property that can rot.
 *
 * ── The rules, and why each one is what it is ─────────────────────
 *
 * SEPARATORS. '/' and '\\' both separate. A repository-relative path is built
 * by joining readdir components with '/', so a backslash reaching here came
 * from stored data, a cross-platform database file, or a Windows-style
 * argument — the same reasoning `hyp_normalize_path_sep` states for every
 * other path in the tree.
 *
 * A DOT AT THE START OF A SEGMENT IS A HIDDEN-FILE MARKER, NOT AN EXTENSION
 * SEPARATOR. Treating it as an extension leaves ".env" with an EMPTY stem,
 * whose module QN is byte-identical to the project QN — one address for the
 * project root and for every dotfile in it. The rule is structural (position 0
 * of a segment), never a list of known dotfile names.
 *
 * A HIDDEN FILE HAS NO EXTENSION AT ALL. ".env.local" and ".env.production"
 * are two files, not one module with two flavours; stripping at the last dot
 * gives both the stem ".env" and collapses them onto each other and onto
 * ".env". Distinctness is the whole job of an address, so a segment that opens
 * with a dot is kept whole.
 *
 * THE LEADING DOT IS KEPT. ".env" contributes ".env" and joins as
 * "proj..env". Dropping it to "env" would read better and would collapse
 * ".env" onto "env.py" and ".github/ci" onto "github/ci" — trading a fixed
 * collision for a new one. The File-node QN for a dotfile already carries the
 * doubled dot (see the `__file__` rule below), so keeping it is also what
 * makes module QNs and File QNs agree with each other.
 *
 * "." CONTRIBUTES NOTHING and ".." HAS NO ADDRESS. A repository-relative path
 * never contains either; a caller handing one over is passing an UNRESOLVED
 * import specifier ("./config") where a path belongs. Dropping ".." would
 * address "a/../b.py" as `proj.a.b`, which is a real and different file, and
 * popping it in a single forward pass costs state this must not carry. So the
 * derivation refuses: the empty string, which matches nothing, in preference
 * to a plausible name that matches the wrong thing. Callers resolve the
 * specifier against the importing file first (`hyp_pipeline_resolve_relative_
 * import`) — that is where a relative import actually becomes a path.
 *
 * `__file__` KEEPS THE WHOLE FILENAME. File nodes are addressed
 * `<path>.<filename>.__file__`, extension included, so that sibling files
 * sharing a stem stay distinct: .env / .env.local / .env.production, and a
 * C/C++ header against its same-stem .cpp (#1077/#964). Extension stripping
 * stays for module and symbol QNs, where stem unification is load-bearing for
 * C/C++ declaration-to-definition resolution.
 *
 * `__init__` AND `index` DROP OUT of a symbol QN so `pkg/__init__.py:Foo` is
 * `proj.pkg.Foo`. They are KEPT when there is no symbol name, or the module QN
 * of the file would collide with the Folder QN of its own directory.
 *
 * NO SEGMENT CAP. The output is bounded by the input, so a deep path costs
 * bytes rather than losing segments; a cap would silently address two deep
 * paths sharing a prefix identically.
 *
 * A NULL PROJECT HAS NO QN. Emitting the path alone would produce ".foo.bar",
 * which is the same string in every project — a cross-project collision. The
 * empty string is returned instead: it matches nothing, and fails closed.
 */
#ifndef HYP_FQN_CORE_H
#define HYP_FQN_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
    HYP_FQN_INIT_LEN = 8,  /* strlen("__init__") */
    HYP_FQN_INDEX_LEN = 5, /* strlen("index") */
};

/* Both separators, on every platform — see the header comment. */
static inline bool hyp_fqn_core_is_sep(char c) {
    return c == '/' || c == '\\';
}

/* Length of `path` with trailing separators removed, so "src/" and "src"
 * address the same directory and "index/" is still a final segment. */
static inline size_t hyp_fqn_core_trim(const char *path, size_t len) {
    while (len > 0 && hyp_fqn_core_is_sep(path[len - 1])) {
        len--;
    }
    return len;
}

/* Length of `path` up to the extension of its FINAL segment. A segment opening
 * with '.' is a hidden file and has no extension. */
static inline size_t hyp_fqn_core_stem_len(const char *path, size_t len) {
    size_t seg = 0;
    for (size_t i = len; i > 0; i--) {
        if (hyp_fqn_core_is_sep(path[i - 1])) {
            seg = i;
            break;
        }
    }
    if (seg < len && path[seg] == '.') {
        return len;
    }
    for (size_t i = len; i > seg; i--) {
        if (path[i - 1] == '.') {
            return i - 1;
        }
    }
    return len;
}

/* Package-entry segments that a symbol QN addresses through rather than into. */
static inline bool hyp_fqn_core_is_package_entry(const char *seg, size_t len) {
    if (len == HYP_FQN_INIT_LEN && memcmp(seg, "__init__", HYP_FQN_INIT_LEN) == 0) {
        return true;
    }
    return len == HYP_FQN_INDEX_LEN && memcmp(seg, "index", HYP_FQN_INDEX_LEN) == 0;
}

/* Offset of the final path separator in `path`, or `len` when it has none.
 * The directory-module languages (Java package, Go package) address a file by
 * its CONTAINING DIRECTORY, and both layers must cut the basename at the same
 * byte or a cross-file caller QN cannot match its callee's def QN. */
static inline size_t hyp_fqn_core_dir_len(const char *path, size_t len) {
    for (size_t i = len; i > 0; i--) {
        if (hyp_fqn_core_is_sep(path[i - 1])) {
            return i - 1;
        }
    }
    return len;
}

/* Append `plen` bytes of `path` to `out` as dotted segments. Returns false when
 * the path carries a ".." segment and therefore has no address; `*len` is then
 * unusable. */
static inline bool hyp_fqn_core_append(char *out, size_t *len, const char *path, size_t plen,
                                       bool has_name) {
    size_t i = 0;
    while (i < plen) {
        size_t start = i;
        while (i < plen && !hyp_fqn_core_is_sep(path[i])) {
            i++;
        }
        size_t seg_len = i - start;
        bool is_last = (i >= plen);
        i++; /* step over the separator (or past the end, ending the loop) */
        if (seg_len == 0) {
            continue;
        }
        const char *seg = path + start;
        if (seg_len == 1 && seg[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            return false;
        }
        if (has_name && is_last && hyp_fqn_core_is_package_entry(seg, seg_len)) {
            continue;
        }
        out[*len] = '.';
        memcpy(out + *len + 1, seg, seg_len);
        *len += seg_len + 1;
    }
    return true;
}

/* Bytes a symbol/module QN needs, terminator included. Every segment costs its
 * own bytes plus one separator, and a separator in the path pays for it, so the
 * output can exceed the input by at most one dot per side. */
static inline size_t hyp_fqn_core_bound(const char *project, const char *rel_path,
                                        const char *name) {
    return (project ? strlen(project) : 0) + (rel_path ? strlen(rel_path) : 0) +
           (name ? strlen(name) : 0) + 3;
}

/* Bytes a folder QN needs, terminator included. */
static inline size_t hyp_fqn_core_folder_bound(const char *project, const char *rel_dir) {
    return (project ? strlen(project) : 0) + (rel_dir ? strlen(rel_dir) : 0) + 2;
}

/* Write `project.path.parts.stem.name` into `out`, which must hold
 * hyp_fqn_core_bound() bytes. Returns the length written. */
static inline size_t hyp_fqn_core_write(char *out, const char *project, const char *rel_path,
                                        const char *name) {
    out[0] = '\0';
    if (!project) {
        return 0;
    }
    if (!rel_path) {
        rel_path = "";
    }
    size_t name_len = name ? strlen(name) : 0;
    bool has_name = name_len > 0;
    /* All File-node QN sites route through here, so creation and every lookup
     * keep the same shape. */
    bool is_file_qn = has_name && strcmp(name, "__file__") == 0;

    size_t len = strlen(project);
    memcpy(out, project, len);

    size_t plen = hyp_fqn_core_trim(rel_path, strlen(rel_path));
    if (!is_file_qn) {
        plen = hyp_fqn_core_stem_len(rel_path, plen);
    }
    if (!hyp_fqn_core_append(out, &len, rel_path, plen, has_name)) {
        out[0] = '\0';
        return 0;
    }
    if (has_name) {
        out[len] = '.';
        memcpy(out + len + 1, name, name_len);
        len += name_len + 1;
    }
    out[len] = '\0';
    return len;
}

/* Write `project.dir.parts` into `out`, which must hold
 * hyp_fqn_core_folder_bound() bytes. Returns the length written. */
static inline size_t hyp_fqn_core_folder_write(char *out, const char *project,
                                               const char *rel_dir) {
    out[0] = '\0';
    if (!project) {
        return 0;
    }
    if (!rel_dir) {
        rel_dir = "";
    }
    size_t len = strlen(project);
    memcpy(out, project, len);

    size_t dlen = hyp_fqn_core_trim(rel_dir, strlen(rel_dir));
    if (!hyp_fqn_core_append(out, &len, rel_dir, dlen, false)) {
        out[0] = '\0';
        return 0;
    }
    out[len] = '\0';
    return len;
}

#endif /* HYP_FQN_CORE_H */
