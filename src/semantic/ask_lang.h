/*
 * ask_lang.h — which language the `ask` instruct prefix is rendered for.
 *
 * §2.1's one trap: `{language}` must be the DISPLAY NAME a developer writes
 * in prose, never the grammar id. `cpp` -> `C++`, `csharp` -> `C#`, `tsx` ->
 * `TypeScript`. Rendered with the grammar id the prefix still encodes, still
 * ranks and still returns answers — it is simply no longer the 106 bytes the
 * recall figures were measured against, and nothing downstream can notice.
 *
 * So there are two jobs here and both of them refuse rather than guess:
 *
 *   resolve  — a caller-supplied token ("cpp", "c++", "Python", ".rs") to a
 *              HYPLanguage, or HYP_LANG_COUNT. Never a default.
 *   dominant — the language of a project, counted over the files its graph
 *              actually contains, or HYP_LANG_COUNT when nothing is
 *              recognisable. Never a default.
 *
 * `ask` turns either refusal into an explicit, caller-facing message naming
 * the `language` argument. A wrong-but-plausible language is the one failure
 * mode with no downstream signal at all, so it is the one thing this lane
 * will not do quietly.
 *
 * MERGE NOTE (feat/ask-language-map, T5): that branch adds
 * `hyp_language_prompt_name()` — a sparse 8-entry override over LANG_NAMES
 * for the cases where the file label and the prose name genuinely differ —
 * and `hyp_ask_render_instruct_prefix()`. When it lands,
 * `hyp_ask_language_display()` below repoints at `hyp_language_prompt_name()`
 * (one line, marked in the .c) so the name `ask` DISCLOSES is byte-identical
 * to the name the prefix RENDERS. Until then it is `hyp_language_name()`,
 * which already agrees for every language except those eight.
 */
#ifndef HYP_ASK_LANG_H
#define HYP_ASK_LANG_H

#include <stddef.h>

#include "hyp.h" /* HYPLanguage, HYP_LANG_COUNT */

/* Resolve a caller-supplied language token.
 *
 * Accepts, in this order: an extension with or without the dot (`cpp`, `.cpp`,
 * `rs`, `py`), then a case-insensitive display name (`C++`, `python`,
 * `TypeScript`). Both spellings are what agents actually type, and neither
 * needs a new table — the registry in src/discover/language.c already holds
 * every one of them.
 *
 * Returns HYP_LANG_COUNT for NULL, empty, or unrecognised input. There is no
 * fallback to a default language: a question about Rust encoded behind the C++
 * prefix is a silently worse answer, which is exactly what this lane exists to
 * avoid. */
HYPLanguage hyp_ask_resolve_language(const char *token);

/* The dominant language over `paths` — the one the most files map to via the
 * same `hyp_language_for_filename()` the indexer uses.
 *
 * Pure on purpose: the caller pulls the file list out of the graph and this
 * decides, so the decision is testable with an array of strings and no store.
 *
 * Ties break toward the lower HYPLanguage value, which is arbitrary but
 * STABLE — an unstable tie-break would make the rendered prefix, and therefore
 * the ranking, depend on row order. Returns HYP_LANG_COUNT when `paths` is
 * empty or nothing in it is a recognised source file. */
HYPLanguage hyp_ask_dominant_language(const char *const *paths, int count);

/* The display name to render into the prefix and to disclose on the answer.
 * NULL — never "Unknown", never the grammar id — for an unnamed or
 * out-of-range language, so a caller that ignores the result gets an
 * obviously broken prefix rather than a plausible wrong one. */
const char *hyp_ask_language_display(HYPLanguage lang);

#endif /* HYP_ASK_LANG_H */
