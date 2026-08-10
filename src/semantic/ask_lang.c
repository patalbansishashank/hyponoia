/*
 * ask_lang.c — see ask_lang.h. Resolution and derivation, both refusing.
 */
#include "semantic/ask_lang.h"

#include "discover/discover.h"
#include "foundation/constants.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { ASK_LANG_EXT_BUF = HYP_SZ_64 };

static char ask_lower(char c) {
    return (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
}

static bool ask_str_ieq(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    for (; *a && *b; a++, b++) {
        if (ask_lower(*a) != ask_lower(*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

HYPLanguage hyp_ask_resolve_language(const char *token) {
    if (!token || !token[0]) {
        return HYP_LANG_COUNT;
    }

    /* Extension first, dot optional. `hyp_language_for_extension` wants the
     * dot, and an agent writing `language="cpp"` is by far the commonest
     * spelling, so the dot is supplied rather than demanded. */
    char ext[ASK_LANG_EXT_BUF];
    if (token[0] == '.') {
        snprintf(ext, sizeof(ext), "%s", token);
    } else {
        snprintf(ext, sizeof(ext), ".%s", token);
    }
    for (char *p = ext; *p; p++) {
        *p = ask_lower(*p);
    }
    HYPLanguage by_ext = hyp_language_for_extension(ext);
    if (by_ext != HYP_LANG_COUNT) {
        return by_ext;
    }

    /* Then the display name, case-insensitively. This is what makes `C++`,
     * `c#` and `TypeScript` work — the exact strings the prefix renders, so a
     * caller can copy the disclosed language straight back into the next
     * call. */
    for (int l = 0; l < (int)HYP_LANG_COUNT; l++) {
        const char *name = hyp_ask_language_display((HYPLanguage)l);
        if (name && ask_str_ieq(name, token)) {
            return (HYPLanguage)l;
        }
    }

    return HYP_LANG_COUNT;
}

HYPLanguage hyp_ask_dominant_language(const char *const *paths, int count) {
    if (!paths || count <= 0) {
        return HYP_LANG_COUNT;
    }

    int counts[HYP_LANG_COUNT];
    memset(counts, 0, sizeof(counts));

    for (int i = 0; i < count; i++) {
        const char *path = paths[i];
        if (!path || !path[0]) {
            continue;
        }
        /* Basename: `hyp_language_for_filename` documents that it takes one,
         * and it has to, because the special-filename table (Makefile,
         * CMakeLists.txt, Dockerfile) matches whole names. Passing a full
         * path would silently degrade those to extension lookups. */
        const char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
#ifdef _WIN32
        const char *back = strrchr(base, '\\');
        if (back) {
            base = back + 1;
        }
#endif
        HYPLanguage lang = hyp_language_for_filename(base);
        if (lang >= 0 && lang < HYP_LANG_COUNT) {
            counts[lang]++;
        }
    }

    HYPLanguage best = HYP_LANG_COUNT;
    int best_n = 0;
    for (int l = 0; l < (int)HYP_LANG_COUNT; l++) {
        /* Strictly-greater keeps the tie-break at the LOWER enum value, which
         * is arbitrary but does not move with row order. */
        if (counts[l] > best_n) {
            best_n = counts[l];
            best = (HYPLanguage)l;
        }
    }
    return best;
}

const char *hyp_ask_language_display(HYPLanguage lang) {
    if (lang < 0 || lang >= HYP_LANG_COUNT) {
        return NULL;
    }
    /* MERGE POINT (feat/ask-language-map): this becomes
     *     const char *name = hyp_language_prompt_name(lang);
     * which is the same table plus an 8-entry override and which already
     * returns NULL rather than "Unknown". Until then the guard below does
     * that job, so no caller sees "Unknown" reach a prompt either way. */
    const char *name = hyp_language_name(lang);
    if (!name || strcmp(name, "Unknown") == 0) {
        return NULL;
    }
    return name;
}
