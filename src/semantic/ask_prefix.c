/*
 * ask_prefix.c — Render the `ask` instruct prefix. See ask_prefix.h for why
 * the wording is frozen and why every failure path refuses instead of
 * falling back.
 */
#include "semantic/ask_prefix.h"

#include "discover/discover.h"
#include "foundation/constants.h"
#include "hyp.h" /* HYPLanguage */

#include <stdio.h>

int hyp_ask_render_instruct_prefix(HYPLanguage lang, char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        return HYP_NOT_FOUND;
    }
    buf[0] = '\0';

    const char *name = hyp_language_prompt_name(lang);
    if (!name) {
        return HYP_NOT_FOUND;
    }

    int written = snprintf(buf, buflen, HYP_ASK_INSTRUCT_TEMPLATE, name);
    if (written < 0 || (size_t)written >= buflen) {
        /* Truncation is the silent-corruption case in miniature: a prefix
         * cut at "…retrieve the decl" is still a valid prompt. Refuse. */
        buf[0] = '\0';
        return HYP_NOT_FOUND;
    }
    return written;
}
