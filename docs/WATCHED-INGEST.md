# Watched-directory ingest — the generic feed format

[← Documentation index](README.md)

A directory of append-only JSONL files is the integration surface every tool
already has: anything that can append a line to a file can feed the memory
store — Codex, Claude Code, a shell hook, a CI job — with no client library,
no socket, and no schema negotiation. This page is the contract for that
directory. The implementation is `src/ingest/watched_ingest.h`, whose header
comment carries the normative summary; the record shape it feeds is
`src/foundation/record.h` (the append-only record contract).

Status: the adapter, its format and its tests are in-tree; wiring it to the
live store and to the index watcher's trigger is separate work. Nothing here
depends on which harness wrote the file — that is the point.

## The shape of the directory

```
<watch-root>/
  anything/at/any/depth/<session>.jsonl
```

- Only files ending `.jsonl` are read. Dotfiles and symlinks are ignored.
- **One file per session** is the intended layout. The file *is* the
  conversation: rows that do not say otherwise inherit it as their `thread`.
- Files are **append-only**. Never rewrite, truncate or rename a feed file —
  a shrunk file is refused by name and stays refused; a renamed file is a new
  origin space and re-ingests as duplicates (the union keeps both, so nothing
  is lost, but the audit will show you what you did).

## The line format (hyp feed v1)

UTF-8 [JSON Lines](https://jsonlines.org/): one JSON object per
`\n`-terminated line. A trailing `\r` is tolerated; blank lines are skipped.
A line larger than 16 MiB is refused, never truncated.

The writer supplies **what was said**. The adapter derives **where it came
from**. The field names are exactly the record-input names, minus the two a
writer is not allowed to touch:

| field          | required | type          | meaning                                                                 |
|----------------|----------|---------------|-------------------------------------------------------------------------|
| `v`            | no       | integer       | Format version. Absent means `1`. Anything else is refused, not guessed. |
| `kind`         | no       | string        | `decision` \| `verdict` \| `summary` \| `signal` \| `message`. Absent means `message`. |
| `author`       | **yes**  | string        | Who spoke: an agent id, a person, a harness name.                        |
| `timestamp_ms` | **yes**  | integer       | When the event happened — UTC milliseconds since the Unix epoch, from the **source's own clock at the time of the event**. The adapter never reads a clock. |
| `content`      | **yes**  | string        | The text. Non-empty; at most 4 MiB.                                      |
| `anchor`       | no       | string        | An identity-contract address, passed through opaquely.                   |
| `thread`       | no       | string / null | Conversation membership. **Absent** = "this file's session" (derived, see below). **`null`** = "belongs to no conversation". Absent and null are different answers. |
| `parent`       | no       | string / null | A hyp record id (64 lowercase hex) this row points at. Most writers omit it — a harness cannot know record ids, and session grouping is `thread`'s job. |

Rules that are refusals, not conventions:

- **`origin`, `redactions` and `id` are refused if a line supplies them.**
  They are derived (below); silently ignoring an attempt to set them would be
  provenance forgery that appears to work.
- **Unknown other keys are ignored**, so a harness may keep its own
  bookkeeping fields in the same line.
- Required fields must be present with the right type; `kind` must be one of
  the five; embedded NUL bytes in any string are refused (they would
  silently truncate at record construction).
- Every cap is a refusal, never a truncation.

A complete line (wrapped here for readability; on disk it is one line):

```json
{"author": "codex", "timestamp_ms": 1770985600000,
 "content": "chose the union over merge resolution",
 "kind": "decision", "anchor": "hyp1:ws/repo#path.to.symbol"}
```

## What the adapter derives

- **`origin`** = `wd1:<relpath>@<byte-offset>` — the file's path relative to
  the watch root ('/'-separated) plus the decimal offset of the line's first
  byte. Positional on purpose: the same file position re-ingests to the same
  record id (idempotence with no dedup table), and two byte-identical rows at
  two positions stay two records, which is what lets a completeness audit
  compare sets of origins instead of counts — dedup and loss look the same
  from a count.
- **`thread`** (when the line does not say) = `wd1:<relpath>` — the file's
  own session.
- **`redactions`** = the number of spans the secret scrubber replaced in
  `content` before the record was constructed. The scrubber is a
  **precondition**: the scanner cannot be constructed without one, because
  record ids commit to content and records are permanent. The identity
  scrubber exists for tests only.

## Failure is per file, named, and never blocking

- A malformed line quarantines **its file** with a named reason and the
  offending byte offset. The valid prefix stays ingested; other files are
  untouched; a later scan re-examines the file, so a fixed line heals it.
- A **partial last line** — a harness mid-append — is not an error. It is
  "not yet", and is picked up when the newline lands.
- A **shrunk** file is refused permanently for the scanner's lifetime: its
  ingested offsets can no longer be trusted, and regrowing it does not
  restore trust.

## Writing a translator: Claude Code's transcripts

Claude Code already drops one JSONL file per session at
`~/.claude/projects/<project-slug>/<session-id>.jsonl`, but in its **own
schema** (observed 2026-08; unversioned, and it changes with the harness):
lines carry a `type` discriminator (`user`, `assistant`, plus bookkeeping
types such as `file-history-snapshot`, `attachment`, `system`, `mode`),
ISO-8601 `timestamp` strings, and message bodies under `message.role` /
`message.content` where content may be a string or a list of typed blocks.

That schema is deliberately **not** parsed here. Teaching the adapter one
harness's private, unversioned schema is exactly the boundary violation the
record contract forbids one level down (`I7`): the next harness would then
have to speak the first one's dialect. Instead, a small per-harness
translator emits this page's format:

| Claude Code                          | hyp feed v1                                        |
|--------------------------------------|----------------------------------------------------|
| line with `type: "user"`/`"assistant"` | one output line, `kind: "message"`               |
| every other `type`                   | skipped (harness bookkeeping, not conversation)    |
| `message.role` (+ model if wanted)   | `author` (e.g. `claude-code:assistant`)            |
| `timestamp` (ISO-8601 string)        | `timestamp_ms` (epoch milliseconds)                |
| `message.content` (string or blocks) | `content` — text blocks flattened; secrets are the scrubber's job, not the translator's |
| `sessionId`                          | `thread` — or omit it and let the per-session output file derive it |
| `uuid` / `parentUuid`                | dropped — `parent` takes hyp record ids only; ordering and membership are `thread` + `timestamp_ms` |

Two rules make a translator honest:

1. **Append-only, deterministic output.** Translate each source line at most
   once, append exactly one output line per translated source line, and never
   rewrite what was already emitted — origins are positional, so a reordered
   output file is a different set of records.
2. **The source's time, never the translation's.** `timestamp_ms` converts
   the source `timestamp`; a translator that stamps "now" makes ids depend on
   when the translator ran, and re-translation would double the store.

## What this adapter is not

The inotify/kqueue trigger belongs to the index watcher; this module is the
scan-and-ingest pass the watcher calls (`hyp_watched_scan_once`). The scanner
yields validated record inputs to a sink callback and never touches the store
itself. Progress state is in-memory only — losing it is safe, because a cold
rescan derives the same origins and therefore the same ids.
