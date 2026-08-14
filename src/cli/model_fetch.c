/*
 * model_fetch.c — fetch-on-first-use for the `ask` lane's embedding weights.
 *
 * See model_fetch.h for the decision this implements and the three properties
 * it must not break. The shape here is deliberately boring:
 *
 *     probe → consent → space → transfer(.part) → size → DIGEST → rename
 *
 * with the digest as the only door into the final name. Every arrow that fails
 * says what to do next, because the failures are the product: a 639 MB
 * download over a slow link WILL be interrupted, and a fetcher that only works
 * on the happy path is a fetcher that does not work.
 */
#include "cli/model_fetch.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/statvfs.h>
#include <unistd.h>
#endif

enum {
    MODEL_HASH_CHUNK = HYP_SZ_64K, /* 64 KiB reads: 639 MB in ~10k syscalls */
    MODEL_DIR_MODE = 0700,         /* the cache is the user's, nobody else's */
    MODEL_ANSWER_MAX = HYP_SZ_16,  /* enough for "yes\n" and any typo of it */
    /* DECIMAL megabytes, deliberately: HYP_MODEL_ASK_SIZE_TEXT says "639 MB"
     * because that is what Hugging Face publishes and what T2 recorded, and
     * 639150592 / 1024^2 is 609. Mixing the two units prints "273 of 609 MB"
     * under a banner that promised 639, which reads as a bug in the count. */
    MODEL_MB = 1000 * 1000,
    MODEL_SPACE_SLACK_MB = 64, /* headroom demanded above the artifact */
    MODEL_HEX_PER_BYTE = 2,    /* two hex characters per digest byte */
    MODEL_HEX_HIGH_SHIFT = 4,  /* high nibble */
    MODEL_HEX_LOW_MASK = 0x0f  /* low nibble */
};

/* curl's documented exit codes. Named because the whole point of this table is
 * that a user gets "DNS did not resolve" instead of "curl exited 6". */
enum {
    MODEL_CURL_OK = 0,
    MODEL_CURL_RESOLVE_PROXY = 5,
    MODEL_CURL_RESOLVE_HOST = 6,
    MODEL_CURL_CONNECT = 7,
    MODEL_CURL_HTTP_ERROR = 22,
    MODEL_CURL_WRITE_ERROR = 23,
    MODEL_CURL_TIMEOUT = 28,
    MODEL_CURL_RANGE_ERROR = 33,
    MODEL_CURL_SSL_CONNECT = 35,
    MODEL_CURL_BAD_RESUME = 36,
    MODEL_CURL_SSL_CACERT = 60,
    MODEL_CURL_NOT_FOUND = 127 /* hyp_exec_no_shell's child could not execvp */
};

/* ── The pinned artifacts ────────────────────────────────────────── */

static const hyp_model_spec_t MODEL_ASK = {
    .model = HYP_MODEL_ASK_MODEL,
    .quant = HYP_MODEL_ASK_QUANT,
    .file = HYP_MODEL_ASK_FILE,
    .repo = HYP_MODEL_ASK_REPO,
    .revision = HYP_MODEL_ASK_REVISION,
    .sha256 = HYP_MODEL_ASK_SHA256,
    .bytes = HYP_MODEL_ASK_BYTES,
    .size_text = HYP_MODEL_ASK_SIZE_TEXT,
    .url = HYP_MODEL_ASK_URL,
    .command = HYP_MODEL_ASK_COMMAND,
    .what = "the ask lane's embedding weights",
    .without_it = "A question cannot be turned into a vector without them, so NOTHING was "
                  "searched",
};

/* The projection head. A separate artifact because GGUF cannot carry it, not
 * because it is optional — without it the encoder emits the pre-projection
 * hidden state, which is a different space and would be searched happily. */
static const hyp_model_spec_t MODEL_ASK_PROJ = {
    .model = HYP_MODEL_ASK_MODEL,
    .quant = "f32",
    .file = HYP_MODEL_ASK_PROJ_FILE,
    .repo = HYP_MODEL_ASK_REPO,
    .revision = HYP_MODEL_ASK_REVISION,
    .sha256 = HYP_MODEL_ASK_PROJ_SHA256,
    .bytes = HYP_MODEL_ASK_PROJ_BYTES,
    .size_text = HYP_MODEL_ASK_PROJ_SIZE_TEXT,
    .url = HYP_MODEL_ASK_PROJ_URL,
    .command = HYP_MODEL_ASK_COMMAND,
    .what = "the ask lane's projection head",
    .without_it = "The weights would emit their pre-projection hidden state — right shape, unit "
                  "length, WRONG SPACE — so nothing would be scored correctly",
};

const hyp_model_spec_t *hyp_model_ask_spec(void) {
    return &MODEL_ASK;
}

const hyp_model_spec_t *hyp_model_ask_proj_spec(void) {
    return &MODEL_ASK_PROJ;
}

/* ── Paths ───────────────────────────────────────────────────────── */

const char *hyp_model_ask_dir(char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return NULL;
    }
    out[0] = '\0';
    const char *cache = hyp_resolve_cache_dir();
    if (!cache || !cache[0]) {
        return NULL;
    }
    int written = snprintf(out, out_sz, "%s/%s", cache, HYP_MODEL_DIR_NAME);
    if (written <= 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return NULL;
    }
    return out;
}

const char *hyp_model_spec_path(const hyp_model_spec_t *spec, char *out, size_t out_sz) {
    if (!spec || !out || out_sz == 0) {
        return NULL;
    }
    char dir[HYP_MODEL_PATH_MAX];
    out[0] = '\0';
    if (!hyp_model_ask_dir(dir, sizeof(dir))) {
        return NULL;
    }
    int written = snprintf(out, out_sz, "%s/%s", dir, spec->file);
    if (written <= 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return NULL;
    }
    return out;
}

const char *hyp_model_ask_path(char *out, size_t out_sz) {
    return hyp_model_spec_path(&MODEL_ASK, out, out_sz);
}

static const char *model_part_path(const hyp_model_spec_t *spec, char *out, size_t out_sz) {
    char final_path[HYP_MODEL_PATH_MAX];
    if (!out || out_sz == 0) {
        return NULL;
    }
    out[0] = '\0';
    if (!hyp_model_spec_path(spec, final_path, sizeof(final_path))) {
        return NULL;
    }
    int written = snprintf(out, out_sz, "%s%s", final_path, HYP_MODEL_PART_SUFFIX);
    if (written <= 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return NULL;
    }
    return out;
}

/* ── Probe ───────────────────────────────────────────────────────── */

void hyp_model_probe_spec(const hyp_model_spec_t *spec, hyp_model_probe_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->bytes = -1;
    out->part_bytes = 0;
    if (!spec) {
        out->state = HYP_MODEL_NO_CACHE_DIR;
        return;
    }

    if (!hyp_model_spec_path(spec, out->path, sizeof(out->path)) ||
        !model_part_path(spec, out->part_path, sizeof(out->part_path))) {
        out->state = HYP_MODEL_NO_CACHE_DIR;
        return;
    }

    int64_t part_size = hyp_file_size(out->part_path);
    out->part_bytes = part_size > 0 ? part_size : 0;

    int64_t size = hyp_file_size(out->path);
    if (size < 0) {
        out->state = out->part_bytes > 0 ? HYP_MODEL_PARTIAL : HYP_MODEL_ABSENT;
        return;
    }
    out->bytes = size;
    /* Exact-size, not at-least: a file longer than the artifact is as wrong as
     * one shorter, and "close enough" is how a truncated blob gets loaded. */
    out->state = size == spec->bytes ? HYP_MODEL_PRESENT : HYP_MODEL_WRONG_SIZE;
}

void hyp_model_ask_probe(hyp_model_probe_t *out) {
    hyp_model_probe_spec(&MODEL_ASK, out);
}

bool hyp_model_spec_present(const hyp_model_spec_t *spec) {
    hyp_model_probe_t probe;
    hyp_model_probe_spec(spec, &probe);
    return probe.state == HYP_MODEL_PRESENT;
}

bool hyp_model_ask_present(void) {
    return hyp_model_spec_present(&MODEL_ASK);
}

/* ── Digest ──────────────────────────────────────────────────────── */

int hyp_model_sha256_file(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz < (size_t)HYP_MODEL_HEX_SIZE) {
        return HYP_NOT_FOUND;
    }
    out[0] = '\0';
    FILE *fp = hyp_fopen(path, "rb");
    if (!fp) {
        return HYP_NOT_FOUND;
    }
    hyp_sha256_ctx ctx;
    hyp_sha256_init(&ctx);
    static unsigned char buf[MODEL_HASH_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        hyp_sha256_update(&ctx, buf, n);
    }
    int read_err = ferror(fp);
    (void)fclose(fp);
    if (read_err) {
        /* A partial read is NOT a digest. Returning the hash of the prefix
         * would be the single worst bug this file could contain. */
        return HYP_NOT_FOUND;
    }
    uint8_t digest[HYP_SHA256_DIGEST_LEN];
    hyp_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < HYP_SHA256_DIGEST_LEN; i++) {
        out[i * MODEL_HEX_PER_BYTE] = hex[digest[i] >> MODEL_HEX_HIGH_SHIFT];
        out[(i * MODEL_HEX_PER_BYTE) + 1] = hex[digest[i] & MODEL_HEX_LOW_MASK];
    }
    out[HYP_SHA256_HEX_LEN] = '\0';
    return 0;
}

int hyp_model_verify_file(const char *path, const char *expect_hex, bool remove_on_mismatch,
                          char *err, size_t err_sz) {
    if (err && err_sz > 0) {
        err[0] = '\0';
    }
    if (!path || !expect_hex) {
        if (err && err_sz > 0) {
            (void)snprintf(err, err_sz, "no file or no expected digest to verify against");
        }
        return HYP_NOT_FOUND;
    }
    char actual[HYP_MODEL_HEX_SIZE];
    if (hyp_model_sha256_file(path, actual, sizeof(actual)) != 0) {
        if (err && err_sz > 0) {
            (void)snprintf(err, err_sz, "could not read %s to the end, so it cannot be verified",
                           path);
        }
        if (remove_on_mismatch) {
            (void)hyp_unlink(path);
        }
        return HYP_NOT_FOUND;
    }
    if (strcmp(actual, expect_hex) != 0) {
        if (err && err_sz > 0) {
            (void)snprintf(err, err_sz,
                           "SHA-256 MISMATCH\n  expected: %s\n  actual:   %s\n  file:     %s",
                           expect_hex, actual, path);
        }
        if (remove_on_mismatch) {
            (void)hyp_unlink(path);
        }
        return HYP_NOT_FOUND;
    }
    return 0;
}

/* ── The unavailable message ─────────────────────────────────────── */

void hyp_model_unavailable_text_spec(const hyp_model_spec_t *spec, const char *project,
                                     char *detail, size_t detail_sz, char *remedy,
                                     size_t remedy_sz) {
    if (!spec) {
        spec = &MODEL_ASK;
    }
    hyp_model_probe_t probe;
    hyp_model_probe_spec(spec, &probe);
    char fallback[HYP_MODEL_PATH_MAX];
    (void)snprintf(fallback, sizeof(fallback), "~/.cache/hyponoia/%s/%s", HYP_MODEL_DIR_NAME,
                   spec->file);
    const char *where = probe.path[0] ? probe.path : fallback;
    const char *name = project ? project : "";

    if (detail && detail_sz > 0) {
        switch (probe.state) {
        case HYP_MODEL_PRESENT:
            (void)snprintf(detail, detail_sz,
                           "%s ARE present at %s; whatever made this lane "
                           "unavailable, it was not the model.",
                           spec->what, where);
            break;
        case HYP_MODEL_PARTIAL:
            (void)snprintf(detail, detail_sz,
                           "%s are only partly downloaded — %" PRId64 " MB of %s are in %s. %s — "
                           "this is not a statement about the code in \"%s\".",
                           spec->what, probe.part_bytes / MODEL_MB, spec->size_text,
                           probe.part_path, spec->without_it, name);
            break;
        case HYP_MODEL_WRONG_SIZE:
            (void)snprintf(detail, detail_sz,
                           "a file sits at %s but it is %" PRId64 " bytes, not the pinned "
                           "%" PRId64 ". It is NOT the %s %s artifact and is refused rather than "
                           "loaded. %s.",
                           where, probe.bytes, spec->bytes, spec->model, spec->quant,
                           spec->without_it);
            break;
        case HYP_MODEL_NO_CACHE_DIR:
            (void)snprintf(detail, detail_sz,
                           "%s are missing AND the cache directory cannot be resolved, so there "
                           "is nowhere to keep them. %s — this is not a statement about the code "
                           "in \"%s\".",
                           spec->what, spec->without_it, name);
            break;
        case HYP_MODEL_ABSENT:
        default:
            (void)snprintf(detail, detail_sz,
                           "%s are not on this machine. %s (%s GGUF, %s) is fetched on first use "
                           "of this lane and kept at %s. %s — this is not a statement about the "
                           "code in \"%s\".",
                           spec->what, spec->model, spec->quant, spec->size_text, where,
                           spec->without_it, name);
            break;
        }
    }

    if (!remedy || remedy_sz == 0) {
        return;
    }
    switch (probe.state) {
    case HYP_MODEL_PRESENT:
        (void)snprintf(remedy, remedy_sz,
                       "Nothing to fetch. Re-run `%s --verify` if you suspect the file on disk.",
                       spec->command);
        break;
    case HYP_MODEL_NO_CACHE_DIR:
        (void)snprintf(remedy, remedy_sz,
                       "Set HOME, or point HYP_CACHE_DIR at a writable directory, then run "
                       "`%s`. Meanwhile search_graph(query=\"...\") and "
                       "search_graph(semantic_query=[\"a\",\"b\"]) need no model at all.",
                       spec->command);
        break;
    case HYP_MODEL_PARTIAL:
        (void)snprintf(remedy, remedy_sz,
                       "Run `%s` to RESUME the download (it continues from the %" PRId64
                       " MB already fetched). Hyponoia never downloads it on its own. Meanwhile "
                       "search_graph(query=\"...\") and search_graph(semantic_query=[\"a\",\"b\"]) "
                       "work with the index you already have.",
                       spec->command, probe.part_bytes / MODEL_MB);
        break;
    case HYP_MODEL_WRONG_SIZE:
    case HYP_MODEL_ABSENT:
    default:
        (void)snprintf(remedy, remedy_sz,
                       "Run `%s` once — it downloads %s from Hugging Face at a pinned revision, "
                       "verifies it against a SHA-256 compiled into this binary, and keeps it at "
                       "%s (delete it any time with `rm %s`). Hyponoia never downloads it on its "
                       "own. Meanwhile search_graph(query=\"...\") and "
                       "search_graph(semantic_query=[\"a\",\"b\"]) work with the index you "
                       "already have.",
                       spec->command, spec->size_text, where, where);
        break;
    }
}

void hyp_model_unavailable_text(const char *project, char *detail, size_t detail_sz, char *remedy,
                                size_t remedy_sz) {
    hyp_model_unavailable_text_spec(&MODEL_ASK, project, detail, detail_sz, remedy, remedy_sz);
}

/* ── Outcome messages ────────────────────────────────────────────── */

const char *hyp_model_fetch_result_text(hyp_model_fetch_result_t result) {
    switch (result) {
    case HYP_MODEL_FETCH_OK:
        return "fetched and verified";
    case HYP_MODEL_FETCH_ALREADY_PRESENT:
        return "already present — nothing to do (re-run with --force to fetch it again)";
    case HYP_MODEL_FETCH_DECLINED:
        return "declined — nothing was downloaded; re-run with --yes to consent up front";
    case HYP_MODEL_FETCH_NO_CACHE_DIR:
        return "no cache directory: set HOME, or point HYP_CACHE_DIR at a writable directory";
    case HYP_MODEL_FETCH_MKDIR_FAILED:
        return "could not create the models directory: check permissions and free space on the "
               "cache filesystem";
    case HYP_MODEL_FETCH_NO_SPACE:
        return "not enough free space for the model: free some, or point HYP_CACHE_DIR at a "
               "larger filesystem";
    case HYP_MODEL_FETCH_NO_DOWNLOADER:
        return "curl was not found on PATH: install curl, or copy the GGUF into the models "
               "directory yourself and re-run with --verify";
    case HYP_MODEL_FETCH_DNS:
        return "huggingface.co did not resolve: you are offline, or DNS/a proxy is blocking it";
    case HYP_MODEL_FETCH_OFFLINE:
        return "could not connect to huggingface.co: you are offline, or a firewall/proxy is "
               "blocking HTTPS";
    case HYP_MODEL_FETCH_HTTP:
        return "the pinned revision returned an HTTP error: the artifact moved or is gated. This "
               "binary pins one revision on purpose and will not silently fetch another";
    case HYP_MODEL_FETCH_TLS:
        return "TLS failed: check the system CA bundle, or the TLS-intercepting proxy in front of "
               "you";
    case HYP_MODEL_FETCH_DISK_FULL:
        return "the write failed part-way, which is usually a full disk: free space and re-run to "
               "resume";
    case HYP_MODEL_FETCH_INTERRUPTED:
        return "the download was interrupted; the partial file was KEPT — re-run to resume from "
               "where it stopped";
    case HYP_MODEL_FETCH_DIGEST_MISMATCH:
        return "SHA-256 did not match and the file was DELETED. Re-run to fetch it again; if it "
               "keeps failing, something between you and Hugging Face is altering the bytes";
    case HYP_MODEL_FETCH_INSTALL_FAILED:
        return "the download verified but could not be moved into place: check permissions on the "
               "models directory";
    case HYP_MODEL_FETCH_TRANSFER_FAILED:
    default:
        return "the download failed; re-run to resume from where it stopped";
    }
}

/* ── Preflight ───────────────────────────────────────────────────── */

/* Refuse BEFORE moving bytes when the filesystem obviously cannot hold them.
 * Finding out at 600 MB costs the user the whole download; finding out at zero
 * costs a stat. Unknown (Windows, or statvfs failing) means "proceed" — a
 * check that cannot answer must not become a new way to fail. */
static bool model_space_ok(const char *dir, int64_t need_bytes, int64_t *free_mb_out) {
    if (free_mb_out) {
        *free_mb_out = -1;
    }
#ifndef _WIN32
    struct statvfs vfs;
    if (statvfs(dir, &vfs) != 0) {
        return true;
    }
    int64_t avail = (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
    if (free_mb_out) {
        *free_mb_out = avail / MODEL_MB;
    }
    return avail >= need_bytes + ((int64_t)MODEL_SPACE_SLACK_MB * MODEL_MB);
#else
    (void)dir;
    (void)need_bytes;
    return true;
#endif
}

/* Consent. Typing the command is the decision; this is the confirmation that
 * the decision was informed — the size, the source and the destination are on
 * screen before a byte moves. Unattended (no TTY) without --yes is a REFUSAL,
 * not a silent yes: 639 MB must never be the default answer to a question. */
static bool model_confirm(const hyp_model_spec_t *spec, const hyp_model_fetch_opts_t *opts,
                          const char *dest, int64_t resume_mb) {
    printf("Fetch %s?\n\n", spec->what);
    printf("  model     %s (%s GGUF)\n", spec->model, spec->quant);
    printf("  size      %s (%" PRId64 " bytes)\n", spec->size_text, spec->bytes);
    printf("  from      %s\n", spec->url);
    printf("  revision  %s\n", spec->revision);
    printf("  sha256    %s\n", spec->sha256);
    printf("  into      %s\n", dest);
    if (resume_mb > 0) {
        printf("  resuming  %" PRId64 " MB already fetched\n", resume_mb);
    }
    printf("\nDelete it any time with `rm %s`.\n", dest);

    if (opts && opts->assume_yes) {
        printf("Proceeding (--yes).\n\n");
        return true;
    }
#ifndef _WIN32
    if (!isatty(0)) {
        (void)fprintf(stderr,
                      "\nrefusing to download %s unattended: re-run with --yes if that is what "
                      "you want.\n",
                      spec->size_text);
        return false;
    }
#endif
    printf("\nDownload %s now? [y/N] ", spec->size_text);
    (void)fflush(stdout);
    char answer[MODEL_ANSWER_MAX];
    if (!fgets(answer, sizeof(answer), stdin)) {
        (void)fprintf(stderr, "no answer read — nothing was downloaded.\n");
        return false;
    }
    if (answer[0] != 'y' && answer[0] != 'Y') {
        printf("Nothing was downloaded.\n");
        return false;
    }
    printf("\n");
    return true;
}

/* ── The transfer ────────────────────────────────────────────────── */

/* curl, through hyp_exec_no_shell — the same shape src/cli/cli.c already uses
 * for release downloads. No shell, so the URL and destination are argv
 * elements and nothing can interpolate into them.
 *
 * --proto/--proto-redir =https: plain HTTP is refused even as a redirect
 *   target, and Hugging Face DOES redirect (to a CDN host) for every LFS file.
 * -C -: resume from whatever is already in `dest`. This is the whole of the
 *   resume story — curl reads the size itself, so `resume_from` is only here
 *   to keep the seam signature honest about what happens.
 * -f: HTTP errors become exit 22 instead of a 1 KB HTML file on disk that
 *   would later fail the digest for a confusing reason. */
static int model_transfer_curl(const char *url, const char *dest, int64_t resume_from, void *ud) {
    (void)resume_from;
    const bool quiet = ud ? *(const bool *)ud : false;
    const char *loud[] = {"curl",
                          "-fL",
                          "--progress-bar",
                          "--proto",
                          "=https",
                          "--proto-redir",
                          "=https",
                          "-C",
                          "-",
                          "--connect-timeout",
                          "30",
                          "--retry",
                          "3",
                          "--retry-delay",
                          "2",
                          "-o",
                          dest,
                          url,
                          NULL};
    const char *silent[] = {"curl",
                            "-fsSL",
                            "--proto",
                            "=https",
                            "--proto-redir",
                            "=https",
                            "-C",
                            "-",
                            "--connect-timeout",
                            "30",
                            "--retry",
                            "3",
                            "--retry-delay",
                            "2",
                            "-o",
                            dest,
                            url,
                            NULL};
    return hyp_exec_no_shell(quiet ? silent : loud);
}

#ifdef HYP_ENABLE_TEST_SEAMS
static hyp_model_transfer_fn g_model_transfer = NULL;
static void *g_model_transfer_ud = NULL;

void hyp_model_set_transfer_for_test(hyp_model_transfer_fn fn, void *ud) {
    g_model_transfer = fn;
    g_model_transfer_ud = ud;
}
#endif

static int model_transfer(const char *url, const char *dest, int64_t resume_from, bool quiet) {
#ifdef HYP_ENABLE_TEST_SEAMS
    if (g_model_transfer) {
        return g_model_transfer(url, dest, resume_from, g_model_transfer_ud);
    }
#endif
    bool q = quiet;
    return model_transfer_curl(url, dest, resume_from, &q);
}

/* `progressed` is whether the .part file grew during the attempt, and it is
 * load-bearing for exactly one case: hyp_exec_no_shell cannot distinguish "no
 * curl on PATH" from "curl died on a signal" — both come back as
 * HYP_NOT_FOUND. Telling someone whose download was killed by the OOM killer
 * that curl is not installed sends them to fix the wrong thing. Bytes on disk
 * settle it: curl obviously ran. */
static hyp_model_fetch_result_t model_classify(int exit_code, bool progressed) {
    if (exit_code == HYP_NOT_FOUND) {
        return progressed ? HYP_MODEL_FETCH_INTERRUPTED : HYP_MODEL_FETCH_NO_DOWNLOADER;
    }
    switch (exit_code) {
    case MODEL_CURL_RESOLVE_PROXY:
    case MODEL_CURL_RESOLVE_HOST:
        return HYP_MODEL_FETCH_DNS;
    case MODEL_CURL_CONNECT:
    case MODEL_CURL_TIMEOUT:
        return HYP_MODEL_FETCH_OFFLINE;
    case MODEL_CURL_HTTP_ERROR:
        return HYP_MODEL_FETCH_HTTP;
    case MODEL_CURL_WRITE_ERROR:
        return HYP_MODEL_FETCH_DISK_FULL;
    case MODEL_CURL_SSL_CONNECT:
    case MODEL_CURL_SSL_CACERT:
        return HYP_MODEL_FETCH_TLS;
    case MODEL_CURL_NOT_FOUND:
        return HYP_MODEL_FETCH_NO_DOWNLOADER;
    default:
        return HYP_MODEL_FETCH_TRANSFER_FAILED;
    }
}

/* ── Placement ───────────────────────────────────────────────────── */

/* Push the bytes to the platter before the name goes live. Without this a
 * power cut between rename and writeback leaves a full-length file at the good
 * name whose contents were never written — the exact state the digest gate
 * exists to make impossible. */
static void model_sync_file(const char *path) {
#ifndef _WIN32
    FILE *fp = hyp_fopen(path, "rb");
    if (!fp) {
        return;
    }
    (void)fsync(fileno(fp));
    (void)fclose(fp);
#else
    (void)path;
#endif
}

/* Size, then digest, then rename. In that order, because a short file has a
 * cheaper and far more useful explanation than "the hash did not match", and
 * because the rename is the ONLY way bytes acquire the trusted name. */
static hyp_model_fetch_result_t model_install(const hyp_model_spec_t *spec, const char *part,
                                              const char *final_path) {
    int64_t got = hyp_file_size(part);
    if (got != spec->bytes) {
        (void)fprintf(stderr,
                      "\nincomplete: %" PRId64 " of %" PRId64 " bytes (%" PRId64 " of "
                      "%" PRId64 " MB). The partial file was KEPT at %s — re-run `%s` to "
                      "resume.\n",
                      got < 0 ? 0 : got, spec->bytes, (got < 0 ? 0 : got) / MODEL_MB,
                      spec->bytes / MODEL_MB, part, spec->command);
        return HYP_MODEL_FETCH_INTERRUPTED;
    }

    printf("Verifying SHA-256 (%" PRId64 " MB)... ", spec->bytes / MODEL_MB);
    (void)fflush(stdout);
    char err[HYP_MODEL_MSG_MAX];
    if (hyp_model_verify_file(part, spec->sha256, true, err, sizeof(err)) != 0) {
        printf("REJECTED\n");
        (void)fprintf(stderr, "%s\n", err);
        (void)fprintf(stderr, "the file was DELETED rather than left where a later run would "
                              "treat it as valid.\n");
        return HYP_MODEL_FETCH_DIGEST_MISMATCH;
    }
    printf("ok\n");

    model_sync_file(part);
    if (hyp_rename_replace(part, final_path) != 0) {
        (void)fprintf(stderr, "could not move the verified file into place: %s -> %s\n", part,
                      final_path);
        return HYP_MODEL_FETCH_INSTALL_FAILED;
    }
    return HYP_MODEL_FETCH_OK;
}

/* ── Verify-only ─────────────────────────────────────────────────── */

static hyp_model_fetch_result_t model_verify_only(const hyp_model_spec_t *spec,
                                                  const hyp_model_probe_t *probe) {
    if (probe->state == HYP_MODEL_NO_CACHE_DIR) {
        (void)fprintf(stderr, "%s\n", hyp_model_fetch_result_text(HYP_MODEL_FETCH_NO_CACHE_DIR));
        return HYP_MODEL_FETCH_NO_CACHE_DIR;
    }
    if (probe->state != HYP_MODEL_PRESENT) {
        (void)fprintf(stderr, "nothing to verify: no full-size model at %s\n", probe->path);
        return HYP_MODEL_FETCH_INTERRUPTED;
    }
    printf("Verifying %s\n", probe->path);
    char err[HYP_MODEL_MSG_MAX];
    /* Do NOT delete on a --verify mismatch: the user asked a question, not for
     * a repair, and unlinking 639 MB they may want to inspect is not an answer
     * to it. The fetch path deletes because it owns the bytes it just wrote. */
    if (hyp_model_verify_file(probe->path, spec->sha256, false, err, sizeof(err)) != 0) {
        (void)fprintf(stderr, "%s\n", err);
        (void)fprintf(stderr,
                      "this file is NOT the pinned artifact. Delete it (`rm %s`) and "
                      "re-run `%s`.\n",
                      probe->path, spec->command);
        return HYP_MODEL_FETCH_DIGEST_MISMATCH;
    }
    printf("ok — %s\n", spec->sha256);
    return HYP_MODEL_FETCH_OK;
}

/* ── The fetch ───────────────────────────────────────────────────── */

hyp_model_fetch_result_t hyp_model_fetch_spec(const hyp_model_spec_t *spec,
                                              const hyp_model_fetch_opts_t *opts) {
    const hyp_model_fetch_opts_t defaults = {0};
    const hyp_model_fetch_opts_t *o = opts ? opts : &defaults;
    if (!spec) {
        spec = &MODEL_ASK;
    }

    hyp_model_probe_t probe;
    hyp_model_probe_spec(spec, &probe);

    if (o->verify_only) {
        return model_verify_only(spec, &probe);
    }
    if (probe.state == HYP_MODEL_NO_CACHE_DIR) {
        (void)fprintf(stderr, "%s\n", hyp_model_fetch_result_text(HYP_MODEL_FETCH_NO_CACHE_DIR));
        return HYP_MODEL_FETCH_NO_CACHE_DIR;
    }
    if (probe.state == HYP_MODEL_PRESENT && !o->force) {
        printf("Already present: %s (%s)\n", probe.path, spec->size_text);
        printf("%s\n", hyp_model_fetch_result_text(HYP_MODEL_FETCH_ALREADY_PRESENT));
        return HYP_MODEL_FETCH_ALREADY_PRESENT;
    }

    char dir[HYP_MODEL_PATH_MAX];
    if (!hyp_model_ask_dir(dir, sizeof(dir)) || !hyp_mkdir_p(dir, MODEL_DIR_MODE)) {
        (void)fprintf(stderr, "%s\n", hyp_model_fetch_result_text(HYP_MODEL_FETCH_MKDIR_FAILED));
        return HYP_MODEL_FETCH_MKDIR_FAILED;
    }

    /* Two things may be in the way, and NEITHER is removed before consent:
     * "declined" has to mean nothing happened, including nothing deleted.
     * Decide here, act after the user says yes.
     *
     *   - a file at the final name that is not the artifact blocks the rename
     *     and cannot be resumed into;
     *   - a .part longer than the artifact cannot be resumed into either, since
     *     a byte range past the end of the object is a 416, not a download. */
    const bool clear_wrong_size = probe.state == HYP_MODEL_WRONG_SIZE;
    const bool clear_oversized_part = probe.part_bytes > spec->bytes;
    int64_t resume_from = clear_oversized_part ? 0 : probe.part_bytes;

    int64_t free_mb = -1;
    int64_t need = spec->bytes - resume_from;
    if (!model_space_ok(dir, need, &free_mb)) {
        (void)fprintf(stderr, "%s\n  need ~%" PRId64 " MB free in %s, have %" PRId64 " MB\n",
                      hyp_model_fetch_result_text(HYP_MODEL_FETCH_NO_SPACE),
                      (need / MODEL_MB) + MODEL_SPACE_SLACK_MB, dir, free_mb);
        return HYP_MODEL_FETCH_NO_SPACE;
    }

    if (clear_wrong_size) {
        (void)fprintf(stderr,
                      "%s is %" PRId64 " bytes, not the pinned %" PRId64 " — it is not "
                      "the pinned artifact and will be replaced.\n",
                      probe.path, probe.bytes, spec->bytes);
    }
    if (clear_oversized_part) {
        (void)fprintf(stderr,
                      "a partial download of %" PRId64 " bytes is LONGER than the artifact and "
                      "cannot be resumed — it will be discarded and the download restarted.\n",
                      probe.part_bytes);
    }

    if (!model_confirm(spec, o, probe.path, resume_from / MODEL_MB)) {
        return HYP_MODEL_FETCH_DECLINED;
    }

    if (clear_wrong_size) {
        (void)hyp_unlink(probe.path);
    }
    if (clear_oversized_part) {
        (void)hyp_unlink(probe.part_path);
        probe.part_bytes = 0;
    }

    int rc = model_transfer(spec->url, probe.part_path, probe.part_bytes, o->quiet);
    /* A server that will not resume, or a partial file the server disagrees
     * with, is recoverable exactly once and only by starting over. Doing it
     * automatically is right: the alternative is telling a user to delete a
     * file in a directory they have never heard of. */
    if (rc == MODEL_CURL_RANGE_ERROR || rc == MODEL_CURL_BAD_RESUME) {
        (void)fprintf(stderr, "\nthe partial download could not be resumed; restarting it.\n");
        (void)hyp_unlink(probe.part_path);
        rc = model_transfer(spec->url, probe.part_path, 0, o->quiet);
    }
    if (rc != MODEL_CURL_OK) {
        int64_t now = hyp_file_size(probe.part_path);
        bool progressed = now > probe.part_bytes;
        hyp_model_fetch_result_t result = model_classify(rc, progressed);
        (void)fprintf(stderr, "\ndownload failed (curl exit %d): %s\n", rc,
                      hyp_model_fetch_result_text(result));
        if (progressed) {
            (void)fprintf(stderr, "%" PRId64 " of %" PRId64 " MB are on disk at %s.\n",
                          now / MODEL_MB, spec->bytes / MODEL_MB, probe.part_path);
        }
        return result;
    }

    hyp_model_fetch_result_t result = model_install(spec, probe.part_path, probe.path);
    if (result == HYP_MODEL_FETCH_OK) {
        printf("\nInstalled: %s\n", probe.path);
        printf("The `ask` lane can now encode questions. Delete it any time with "
               "`rm %s`.\n",
               probe.path);
    }
    return result;
}

hyp_model_fetch_result_t hyp_model_fetch(const hyp_model_fetch_opts_t *opts) {
    return hyp_model_fetch_spec(&MODEL_ASK, opts);
}

/* ── The command ─────────────────────────────────────────────────── */

static void model_fetch_help(void) {
    printf("Usage: hyponoia fetch-model [--yes] [--force] [--verify] [--path]\n\n");
    printf("Download the model the `ask` lane uses:\n\n");
    printf("  " HYP_MODEL_ASK_MODEL " (" HYP_MODEL_ASK_QUANT " GGUF), " HYP_MODEL_ASK_SIZE_TEXT
           "\n");
    printf("  Without it `ask` cannot turn a question into a vector and answers\n");
    printf("  available=false.\n\n");
    printf("Fetched once and kept in the Hyponoia cache. Nothing else downloads it:\n");
    printf("no index, no tool call and no daemon session ever fetches "
           "" HYP_MODEL_ASK_SIZE_TEXT " on\n");
    printf("your behalf.\n\n");
    printf("Options:\n");
    printf("  --yes, -y   Consent up front (required when stdin is not a terminal)\n");
    printf("  --force     Re-download even if the model is already present\n");
    printf("  --verify    Re-check the SHA-256 of what is on disk; download nothing\n");
    printf("  --path      Print where the model lives (or would live) and exit\n");
    printf("  --quiet     No progress bar\n");
    printf("  --help      This text\n\n");
    printf("Pinned revision " HYP_MODEL_ASK_REVISION "\n");
    printf("Pinned sha256   " HYP_MODEL_ASK_SHA256 "\n");
}

static int model_print_path(const hyp_model_spec_t *spec) {
    hyp_model_probe_t probe;
    hyp_model_probe_spec(spec, &probe);
    if (probe.state == HYP_MODEL_NO_CACHE_DIR) {
        (void)fprintf(stderr, "%s\n", hyp_model_fetch_result_text(HYP_MODEL_FETCH_NO_CACHE_DIR));
        return 1;
    }
    printf("%s\n", probe.path);
    switch (probe.state) {
    case HYP_MODEL_PRESENT:
        printf("present, %" PRId64 " bytes\n", probe.bytes);
        break;
    case HYP_MODEL_PARTIAL:
        printf("not present; a partial download of %" PRId64 " MB is at %s\n",
               probe.part_bytes / MODEL_MB, probe.part_path);
        break;
    case HYP_MODEL_WRONG_SIZE:
        printf("a file is there but is %" PRId64 " bytes, not the pinned %" PRId64 "\n",
               probe.bytes, spec->bytes);
        break;
    case HYP_MODEL_ABSENT:
    case HYP_MODEL_NO_CACHE_DIR:
    default:
        printf("not present\n");
        break;
    }
    return 0;
}

int hyp_cmd_fetch_model(int argc, char **argv) {
    hyp_model_fetch_opts_t opts = {0};
    bool want_path = false;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            model_fetch_help();
            return 0;
        }
        if (strcmp(arg, "--yes") == 0 || strcmp(arg, "-y") == 0) {
            opts.assume_yes = true;
        } else if (strcmp(arg, "--force") == 0) {
            opts.force = true;
        } else if (strcmp(arg, "--verify") == 0) {
            opts.verify_only = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            opts.quiet = true;
        } else if (strcmp(arg, "--path") == 0) {
            want_path = true;
        } else {
            /* An ignored unknown flag is how `install --help` once performed a
             * real installation (NEXT-STEPS.md §1). Not again, and especially
             * not on a command whose accident costs 639 MB. */
            (void)fprintf(stderr, "hyponoia fetch-model: unknown option '%s'\n", arg);
            model_fetch_help();
            return 2;
        }
    }

    /* BOTH artifacts, always. The weights alone are not a working encoder:
     * voyage-4-nano emits its pre-projection hidden state without the head,
     * which is the right width and unit length and the wrong space. Fetching
     * one and not the other would leave a lane that loads and ranks badly —
     * the failure mode with no downstream signal. */
    const hyp_model_spec_t *chosen[2];
    int n_chosen = 0;
    chosen[n_chosen++] = &MODEL_ASK;
    chosen[n_chosen++] = &MODEL_ASK_PROJ;

    if (want_path) {
        int rc = 0;
        for (int i = 0; i < n_chosen; i++) {
            if (model_print_path(chosen[i]) != 0) {
                rc = 1;
            }
        }
        return rc;
    }

    /* Each model is fetched, verified and reported on its own. A failure on the
     * second does not undo the first — 639 MB that arrived intact stay on disk
     * and the exit code says something went wrong. */
    int rc = 0;
    for (int i = 0; i < n_chosen; i++) {
        if (n_chosen > 1) {
            printf("%s[%d/%d] %s\n", i ? "\n" : "", i + 1, n_chosen, chosen[i]->model);
        }
        hyp_model_fetch_result_t result = hyp_model_fetch_spec(chosen[i], &opts);
        switch (result) {
        case HYP_MODEL_FETCH_OK:
        case HYP_MODEL_FETCH_ALREADY_PRESENT:
            break;
        default:
            rc = 1;
            break;
        }
    }
    return rc;
}
