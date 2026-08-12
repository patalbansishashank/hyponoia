/*
 * ask_prefix.h — The `ask` query-side instruct prefix (NEXT-STEPS.md §2.1).
 *
 * Documents are encoded bare; queries are encoded behind an instruct
 * prefix. That asymmetry is the mechanism — it is what lets a question
 * match code sharing none of its words — and the prefix's exact wording is
 * PART OF THE MEASUREMENT, not decoration. HYP_ASK_INSTRUCT_TEMPLATE is
 * copied verbatim from QWEN_INSTRUCT in
 * ctxengine/src/ctxengine/encoders.py, which is itself copied verbatim
 * from harness/m7_recall.py, where R@1 0.7833 / R@10 0.9400 / MRR@10
 * 0.8378 were measured on C++. Reword it — even "harmlessly" — and those
 * figures stop describing this code, silently. tests/test_language.c pins
 * the rendered C++ string byte-for-byte so a reword fails a test instead.
 *
 * The one substitution is {language}, and it must be a DISPLAY NAME:
 * hyp_language_prompt_name() supplies it, and refuses (NULL) rather than
 * hand back a grammar id or "Unknown".
 */
#ifndef HYP_ASK_PREFIX_H
#define HYP_ASK_PREFIX_H

#include <stddef.h>

#include "foundation/constants.h"
#include "hyp.h" /* HYPLanguage */

/* printf template with exactly one %s (the display name). Split across two
 * adjacent literals only to stay inside the 100-column limit; the
 * concatenation is byte-exact with encoders.py, trailing space included. */
#define HYP_ASK_INSTRUCT_TEMPLATE                                                             \
    "Instruct: Given a natural-language description of %s code, retrieve the declaration it " \
    "describes.\nQuery: "

/* The name of the contract above, stamped on every index built under it and
 * gated on at read (NEXT-STEPS.md §2.10 step 3).
 *
 * It exists because the model id does not identify the vector space on its own.
 * The same weights fed differently produce incomparable vectors, and this is
 * measured, not hypothetical: the template above is worth +0.206 MRR@10 on the
 * frozen 60, pplx is DESTROYED by it, and §2.9 found the very same Qwen3
 * weights wanting it on one corpus and losing 2.7-3.3 NDCG@10 to it on another.
 *
 * CHANGE THIS TOKEN WHENEVER THE TEMPLATE OR THE DOCUMENT SIDE CHANGES. That is
 * the whole mechanism: a stale index is then refused by name instead of being
 * silently searched with queries it was never built to answer. The "-bare-doc"
 * half records the other half of the asymmetry — documents get no prefix — so
 * that a future decision to prefix documents also lands as a new token. */
#define HYP_ASK_PREFIX_CONTRACT "qwen3-instruct-v1-bare-doc"

/* Comfortably above template length (103 bytes with the %s removed; 106
 * rendered with "C++") plus the longest display name;
 * hyp_ask_render_instruct_prefix() refuses on truncation regardless, so
 * this is a convenience size for callers, not a load-bearing bound. */
enum { HYP_ASK_PREFIX_MAX = HYP_SZ_256 };

/* Render the instruct prefix for `lang` into `buf`.
 *
 * Returns the number of bytes written (excluding the NUL) on success, or
 * HYP_NOT_FOUND when it REFUSES: `lang` has no display name, `buf` is NULL
 * or zero-length, or the result would not fit. `buf` is left empty ("") on
 * every refusal, so a caller that ignores the return value gets an obviously
 * broken prefix rather than a plausible wrong one.
 *
 * Refusing is the design. A prefix built with the grammar id, with
 * "Unknown", or truncated mid-sentence still encodes, still ranks, and
 * still returns answers — it just is not the prefix any number was
 * measured against. There is no failure signal available downstream, so
 * the signal has to be here. */
int hyp_ask_render_instruct_prefix(HYPLanguage lang, char *buf, size_t buflen);

#endif /* HYP_ASK_PREFIX_H */
