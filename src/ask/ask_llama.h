/*
 * ask_llama.h — the llama.cpp/GGUF encoder behind the `ask` lane's two seams.
 *
 * NEXT-STEPS.md §2.1 asks for Qwen3-Embedding-0.6B to run over WHOLE
 * declarations inside the binary, because encoding one token at a time deletes
 * attention, which is the only thing the model was for.
 * runs/ASK/T2-inference-runtime.json settled the vehicle with a working
 * prototype: minimum cosine 0.999980 against sentence-transformers fp32,
 * last-token pooling honoured from the GGUF's own metadata, 24.09 docs/s on
 * Vulkan against 1.581 on CPU.
 *
 * THIS FILE IS AN ADAPTER, NOT A DESIGN. Two seams already exist and neither
 * moves:
 *
 *   hyp_ask_backend_t     (src/semantic/ask_embed.h)  — the `ask` TOOL's
 *       query path. Installed once at startup; NULL is the whole of the
 *       no_encoder unavailable state.
 *   hyp_ask_encoder_t     (src/ask/ask_encoder.h)     — the embed PASS's
 *       document path, plus device reporting and the two token-length
 *       questions the truncation counter is built on.
 *
 * Both spell the asymmetry structurally: encode_query takes a language and
 * renders the instruct prefix; encode_documents takes NO language at all, so
 * the commonest way to lose recall is not merely discouraged, it is
 * unspellable. That property is the point and it is preserved here — one
 * internal encode() exists, and it takes the already-rendered prefix as an
 * explicit argument that the document path passes as "".
 *
 * ── What this file owns, because nothing above it can ──
 *
 *   1. n_ctx sizing. `n_ctx` is TOTAL KV capacity and llama.cpp divides it by
 *      n_seq_max. `n_ctx = n_batch * n_seq_max` asked for ~57 GB and reset the
 *      user's GPU twice. See ask_batch.h.
 *   2. n_ubatch, the second allocation knob, which bounds the compute buffer
 *      the way n_ctx bounds the KV. See hyp_ask_ubatch_for().
 *   3. Refusing before allocating, against a ceiling taken from what VRAM is
 *      FREE right now — this card also drives the display.
 *   4. Truncating over-long documents at the effective window AND COUNTING IT.
 *      The prototype SKIPS them and emits no row, which silently shifts every
 *      later vector onto the wrong declaration (runs/ASK T8-D1). A row is
 *      never dropped here.
 */
#ifndef HYP_ASK_LLAMA_H
#define HYP_ASK_LLAMA_H

#include "ask/ask_encoder.h"

#include <stdbool.h>
#include <stddef.h>

/* True when this build has the inference runtime compiled in (HYP_ASK_LLAMA).
 * False in the test build and in any build made with the backend switched off:
 * then the `ask` lane reports `no_encoder` and says a different build is the
 * remedy, which is a true statement about the binary in hand. */
bool hyp_ask_llama_compiled_in(void);

/* One line naming the runtime and the devices it can reach, for `--version`
 * style reporting. Never NULL. */
const char *hyp_ask_llama_build_note(void);

/* Create an encoder over the fetched weights.
 *
 * `pref` is the REQUEST. What the encoder could honour is reported through
 * device_note()/device_is_gpu() and the two are never conflated:
 * HYP_ASK_DEVICE_GPU REFUSES rather than falling back, HYP_ASK_DEVICE_AUTO
 * falls back and says so.
 *
 * Returns NULL on failure with a caller-facing sentence in `err`. The weights
 * not being on disk is one such failure and it has its own remedy — callers
 * should check hyp_model_ask_present() first so the user is told to fetch the
 * model rather than told that inference failed.
 *
 * Loading is ~639 MB off disk and takes seconds; create one per run, not one
 * per call. */
hyp_ask_encoder_t *hyp_ask_llama_encoder_create(hyp_ask_device_pref_t pref, char *err,
                                                size_t errlen);

/* Install the process-wide query backend for the `ask` TOOL.
 *
 * Deliberately LAZY: this registers the vtable, it does not load 639 MB. The
 * model is loaded on the first encode_query, so `hyponoia mcp` starting up
 * costs nothing and a user who never runs `ask` never pays for it. Returns 0
 * on success (including when the weights are absent — that is reported as the
 * `model_not_fetched` unavailable cause, not as an install failure). */
int hyp_ask_llama_backend_install(void);

/* Release the lazily-loaded query model, if one was loaded. Idempotent. */
void hyp_ask_llama_backend_shutdown(void);

#endif /* HYP_ASK_LLAMA_H */
