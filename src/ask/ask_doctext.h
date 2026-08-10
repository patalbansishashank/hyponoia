/*
 * ask_doctext.h — what text an `ask` document is made of (NEXT-STEPS §2.2,
 * lever 1).
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE DEPARTURE, AND WHY
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The reference implementation embeds the VERBATIM SOURCE LINES of a
 * declaration and nothing else, and §2.1's port copied that deliberately —
 * a different text unit invalidates every recorded number, so the port had
 * to reproduce the unit exactly before it could reproduce the result.
 *
 * §2.2 changes it on purpose. The rank distribution says every gold answer
 * is already findable (recall@1000 = 1.000) and the work is getting it to
 * the top, and the largest natural-language signal in a commented codebase
 * is being thrown away twice over:
 *
 *   - the leading DOC COMMENT falls OUTSIDE the node's line range, so a
 *     declaration whose comment says "folds together read-only sections
 *     that hold identical contents" is embedded without that sentence;
 *   - the QUALIFIED NAME is not in the text either, so an in-class method
 *     body embeds as `void run();` with no trace of the class it belongs
 *     to, the file it lives in, or what kind of thing it is.
 *
 * Two departures, and each is expected to help for a different reason:
 *
 *   1. THE LEADING COMMENT IS PROSE ABOUT THE DECLARATION. The queries are
 *      natural language; the bodies are not. On the pinned lld/ELF corpus
 *      914 of 4,117 declarations (22.2%) carry one — but 56 of the 75 gold
 *      declarations (74.7%) do, because the declarations people ask about
 *      are the ones people documented.
 *
 *   2. THE HEADER LINE IS THE CONTEXT THE SPAN CANNOT CARRY. A field is
 *      one line of C++; which struct it belongs to is not in that line.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE COMPOSITION, AND WHY IT IS IN THIS ORDER
 * ═════════════════════════════════════════════════════════════════════════
 *
 *     // <Label> <qualified name, project segment removed> in <file path>
 *     <the leading comment block, verbatim>
 *     <the declaration's source lines, verbatim>
 *
 * FILE ORDER, NOT QUERY-FIRST OR SUMMARY-LAST. In every real source file the
 * comment precedes the code it documents; a document whose comment follows
 * its body is a shape that occurs in no corpus the model was trained on.
 * Keeping file order also keeps the change honest: everything below the
 * header line is a CONTIGUOUS SLICE of the file, so this is "extend the span
 * upward to take the comment with it" rather than a synthesised document.
 *
 * PUTTING IT FIRST IS ALSO WHAT SURVIVES TRUNCATION. Documents over the
 * model's window are encoded from their FIRST tokens; text appended after
 * the body would be the first thing lost by exactly the rows that can least
 * afford to lose it.
 *
 * THE HEADER IS WRAPPED IN THE LANGUAGE'S OWN COMMENT MARKER, so the whole
 * document is still syntactically a source fragment. `// Method ICF.run in
 * ICF.cpp` above a doc comment is the banner idiom LLVM itself uses; a bare
 * title line above C++ is not something source files contain.
 *
 * WHAT IS DELIBERATELY NOT IN IT:
 *
 *   - LINE NUMBERS. No semantic content, and they change whenever anything
 *     above the declaration moves — which would invalidate the content hash
 *     that licenses vector REUSE for every declaration below an edit. A
 *     one-line insertion at the top of a file would cost a full re-encode.
 *   - THE LANGUAGE NAME. Constant across a single-language corpus, so it
 *     adds no discriminative power, only tokens. The query side already
 *     carries it, in the instruct prefix, where it is not constant.
 *   - THE PROJECT SEGMENT of the qualified name. It is the same string in
 *     every row of a project — and on this corpus it is a filesystem path
 *     (`home-shashank-ctxbench-repos-llvm-lld-ELF`), which is noise with a
 *     token cost.
 *   - THE FILE'S OWN HEADER COMMENT, when it does not sit directly above the
 *     declaration. LLVM's file banners describe the file, and attaching one
 *     to all 90 declarations inside that file would push every one of them
 *     toward every query about the file while adding nothing that separates
 *     them from each other. The whole-file `Module` row already carries it.
 *     (This is also what keeps the Apache licence boilerplate out: on the
 *     pinned corpus the scan below picks it up ZERO times, because #includes
 *     and blank lines always separate it from the first declaration.)
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT COUNTS AS THE LEADING COMMENT
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The contiguous run of whole-line comments immediately above the
 * declaration. A BLANK LINE ENDS IT, and so does any line that is not
 * entirely a comment. That is the convention the comment is written under —
 * a comment separated from a declaration by a blank line is, by long
 * agreement between programmers, not that declaration's comment — and it is
 * what keeps file banners from leaking in.
 *
 * The scan is LEXICAL, not tree-sitter. The embed pass reads the graph
 * read-only and the source off disk; it does not parse, and giving it a
 * parser to recover a comment it can find by looking at the first two
 * characters of a line would be a large amount of new machinery for a
 * smaller amount of accuracy. The failure mode of getting it wrong is one
 * extra or one missing line of embedded text, not a wrong answer.
 *
 * COMMENT SYNTAX IS PER LANGUAGE AND THE TABLE IS DELIBERATELY INCOMPLETE.
 * A language whose syntax is not listed gets NO leading comment — that is,
 * exactly today's behaviour. Guessing `//` for a language that does not have
 * it would silently prepend code to the document; declining costs nothing
 * that is not already being lost.
 */
#ifndef HYP_ASK_DOCTEXT_H
#define HYP_ASK_DOCTEXT_H

#include "hyp.h" /* HYPLanguage */

#include <stdbool.h>
#include <stddef.h>

/* How the embedded text for one declaration is composed. The default is
 * HYP_ASK_COMPOSE_FULL; the other three exist so that §2.2's requirement to
 * "report each lever's delta separately" is reproducible from the shipped
 * binary rather than from a patch somebody has to re-apply. */
typedef enum {
    /* Verbatim source lines only — the reference implementation's unit, and
     * what every number recorded before §2.2 was measured on. */
    HYP_ASK_COMPOSE_SOURCE = 0,
    /* Header line + source. */
    HYP_ASK_COMPOSE_HEADER = 1,
    /* Leading comment + source. */
    HYP_ASK_COMPOSE_COMMENT = 2,
    /* Header line + leading comment + source. */
    HYP_ASK_COMPOSE_FULL = 3,
} hyp_ask_compose_t;

/* Parse a --compose value. Returns false on an unknown name. */
bool hyp_ask_compose_parse(const char *name, hyp_ask_compose_t *out);
const char *hyp_ask_compose_name(hyp_ask_compose_t c);

/* One language's comment openers. All members NULL means "not known", which
 * is a real answer and not a failure: the caller then takes no comment. */
typedef struct {
    const char *line;  /* whole-line comment opener, e.g. "//" */
    const char *open;  /* block opener, e.g. "/" "*" */
    const char *close; /* block closer, e.g. "*" "/" */
} hyp_ask_comment_syntax_t;

hyp_ask_comment_syntax_t hyp_ask_comment_syntax(HYPLanguage lang);

/* Bounds on the leading comment block. Neither binds on the pinned corpus
 * (the longest block there is 58 lines / ~3.5 KB); they are here so that one
 * pathological file cannot turn a declaration into a document that is
 * mostly prologue. When a block is clipped the lines NEAREST the declaration
 * are the ones kept, because that is where the specific prose is — a long
 * block's opening lines are its banner. */
#define HYP_ASK_DOC_MAX_LINES 64
#define HYP_ASK_DOC_MAX_BYTES 8192

/* Longest header line emitted. Anything past this is dropped rather than
 * allowed to dominate the document. */
#define HYP_ASK_DOC_HEADER_MAX 512

/* The 1-based first line of the comment block immediately above `start`, or
 * `start` itself when there is none. `file_text` is the whole file, '\n'
 * separated (a trailing '\r' on a line is tolerated). Pure: no I/O, so the
 * rule can be tested without a filesystem. */
int hyp_ask_leading_comment_start(const char *file_text, HYPLanguage lang, int start);

/* Write the one synthetic line into `buf`. Returns its length, or 0 when
 * nothing could be written. `project` may be NULL; when it is not, a leading
 * "<project>." is removed from `qualified_name` — an exact prefix match, not
 * a split on the first dot, so a project whose name is absent cannot cost a
 * real segment. No trailing newline. */
size_t hyp_ask_header_line(char *buf, size_t cap, HYPLanguage lang, const char *label,
                           const char *qualified_name, const char *project,
                           const char *rel_path);

#endif /* HYP_ASK_DOCTEXT_H */
