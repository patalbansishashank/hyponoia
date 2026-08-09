/* rust_crates_seed.c — Common-crate API seeds for the Rust LSP.
 *
 * Per RUST_LSP_FOLLOWUP.md A3: we don't run Cargo or download deps.
 * What we CAN do is hand-curate a small registered-method seed for the
 * most-used external crates (serde, tokio, anyhow, clap, regex, log,
 * futures, thiserror, reqwest, hyper, async-trait, parking_lot,
 * chrono, uuid, lazy_static, once_cell, rayon, …) so a project that
 * just `use`s these crates resolves the obvious calls instead of
 * leaving them all `unresolved`.
 *
 * Each entry is "best-effort" — we register the type with its trait
 * QNs in embedded_types and synthesize the high-frequency methods
 * with vaguely-correct return shapes. Anything we get wrong stays
 * `unresolved` rather than a wrong edge (per the doc's no-false-edge
 * policy: we only register what we're confident about).
 *
 * Designed to be appended to the existing stdlib register: takes the
 * same (reg, arena) pair. */

#include "../type_rep.h"
#include "../type_registry.h"
#include "../rust_lsp.h"
#include <string.h>

/* Shorthand. Mirrors the macros in rust_stdlib_data.c. */
#ifndef RUST_CRATES_SEED_INCLUDED
#define RUST_CRATES_SEED_INCLUDED 1

#define CADD_TYPE(qn, sn, iface) do {                              \
    HYPRegisteredType _rt;                                          \
    memset(&_rt, 0, sizeof(_rt));                                   \
    _rt.qualified_name = (qn);                                      \
    _rt.short_name = (sn);                                          \
    _rt.is_interface = (iface);                                     \
    hyp_registry_add_type(reg, _rt);                                \
} while (0)

#define CADD_FUNC(rcv, sn, qn, ret_type) do {                       \
    HYPRegisteredFunc _rf;                                           \
    memset(&_rf, 0, sizeof(_rf));                                    \
    _rf.qualified_name = (qn);                                       \
    _rf.short_name = (sn);                                            \
    _rf.receiver_type = (rcv);                                        \
    _rf.min_params = -1;                                              \
    const HYPType** _rta = (const HYPType**)hyp_arena_alloc(arena,    \
        2 * sizeof(const HYPType*));                                  \
    _rta[0] = (ret_type);                                             \
    _rta[1] = NULL;                                                   \
    _rf.signature = hyp_type_func(arena, NULL, NULL, _rta);           \
    hyp_registry_add_func(reg, _rf);                                  \
} while (0)

void hyp_rust_crates_register(HYPTypeRegistry* reg, HYPArena* arena) {
    const HYPType* t_unit  = hyp_type_builtin(arena, "()");
    const HYPType* t_bool  = hyp_type_builtin(arena, "bool");
    const HYPType* t_usize = hyp_type_builtin(arena, "usize");
    const HYPType* t_str_ref = hyp_type_reference(arena,
        hyp_type_builtin(arena, "str"));
    const HYPType* t_string = hyp_type_named(arena, "alloc.string.String");

    /* ── serde — Serialize / Deserialize / json ─────────────── */
    CADD_TYPE("serde.Serialize",      "Serialize",      true);
    CADD_TYPE("serde.Deserialize",    "Deserialize",    true);
    CADD_TYPE("serde.Serializer",     "Serializer",     true);
    CADD_TYPE("serde.Deserializer",   "Deserializer",   true);
    CADD_TYPE("serde.de.Error",       "Error",          true);
    CADD_TYPE("serde.ser.Error",      "Error",          true);
    CADD_TYPE("serde_json.Value",     "Value",          false);
    CADD_TYPE("serde_json.Map",       "Map",            false);
    CADD_TYPE("serde_json.Number",    "Number",         false);
    CADD_TYPE("serde_json.Error",     "Error",          false);

    CADD_FUNC("serde.Serialize",   "serialize",   "serde.Serialize.serialize",   hyp_type_unknown());
    CADD_FUNC("serde.Deserialize", "deserialize", "serde.Deserialize.deserialize", hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "is_null",      "serde_json.Value.is_null",     t_bool);
    CADD_FUNC("serde_json.Value", "is_bool",      "serde_json.Value.is_bool",     t_bool);
    CADD_FUNC("serde_json.Value", "is_number",    "serde_json.Value.is_number",   t_bool);
    CADD_FUNC("serde_json.Value", "is_string",    "serde_json.Value.is_string",   t_bool);
    CADD_FUNC("serde_json.Value", "is_array",     "serde_json.Value.is_array",    t_bool);
    CADD_FUNC("serde_json.Value", "is_object",    "serde_json.Value.is_object",   t_bool);
    CADD_FUNC("serde_json.Value", "as_bool",      "serde_json.Value.as_bool",     hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "as_i64",       "serde_json.Value.as_i64",      hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "as_f64",       "serde_json.Value.as_f64",      hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "as_str",       "serde_json.Value.as_str",      hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "as_array",     "serde_json.Value.as_array",    hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "as_object",    "serde_json.Value.as_object",   hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "get",          "serde_json.Value.get",         hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "pointer",      "serde_json.Value.pointer",     hyp_type_unknown());
    CADD_FUNC("serde_json.Value", "to_string",    "serde_json.Value.to_string",   t_string);
    /* Free functions in serde_json. */
    CADD_FUNC(NULL, "from_str",  "serde_json.from_str",  hyp_type_unknown());
    CADD_FUNC(NULL, "to_string", "serde_json.to_string", hyp_type_unknown());
    CADD_FUNC(NULL, "from_value","serde_json.from_value",hyp_type_unknown());
    CADD_FUNC(NULL, "to_value",  "serde_json.to_value",  hyp_type_unknown());
    CADD_FUNC(NULL, "json",      "serde_json.json",      hyp_type_named(arena, "serde_json.Value"));

    /* ── anyhow — Error / Result / ensure!/bail!/anyhow! ───── */
    CADD_TYPE("anyhow.Error",   "Error",   false);
    CADD_TYPE("anyhow.Result",  "Result",  false);
    CADD_TYPE("anyhow.Chain",   "Chain",   false);
    CADD_FUNC("anyhow.Error",   "new",      "anyhow.Error.new",      hyp_type_named(arena, "anyhow.Error"));
    CADD_FUNC("anyhow.Error",   "from",     "anyhow.Error.from",     hyp_type_named(arena, "anyhow.Error"));
    CADD_FUNC("anyhow.Error",   "msg",      "anyhow.Error.msg",      hyp_type_named(arena, "anyhow.Error"));
    CADD_FUNC("anyhow.Error",   "context",  "anyhow.Error.context",  hyp_type_named(arena, "anyhow.Error"));
    CADD_FUNC("anyhow.Error",   "downcast", "anyhow.Error.downcast", hyp_type_unknown());
    CADD_FUNC("anyhow.Error",   "downcast_ref","anyhow.Error.downcast_ref", hyp_type_unknown());
    CADD_FUNC("anyhow.Error",   "is",       "anyhow.Error.is",       t_bool);
    CADD_FUNC("anyhow.Error",   "chain",    "anyhow.Error.chain",    hyp_type_unknown());
    CADD_FUNC("anyhow.Error",   "root_cause","anyhow.Error.root_cause", hyp_type_unknown());
    CADD_FUNC("anyhow.Error",   "source",   "anyhow.Error.source",   hyp_type_unknown());
    /* Free functions. */
    CADD_FUNC(NULL, "anyhow",  "anyhow.anyhow",  hyp_type_named(arena, "anyhow.Error"));
    CADD_FUNC(NULL, "bail",    "anyhow.bail",    hyp_type_unknown());
    CADD_FUNC(NULL, "ensure",  "anyhow.ensure",  hyp_type_unknown());

    /* ── thiserror — derive-only crate, but the Error trait. ─── */
    /* (Error trait is already covered under core.error.Error.) */

    /* ── tokio — runtime + I/O + sync primitives. ───────────── */
    CADD_TYPE("tokio.runtime.Runtime",   "Runtime",   false);
    CADD_TYPE("tokio.runtime.Handle",    "Handle",    false);
    CADD_TYPE("tokio.runtime.Builder",   "Builder",   false);
    CADD_TYPE("tokio.task.JoinHandle",   "JoinHandle",false);
    CADD_TYPE("tokio.sync.Mutex",        "Mutex",     false);
    CADD_TYPE("tokio.sync.RwLock",       "RwLock",    false);
    CADD_TYPE("tokio.sync.Semaphore",    "Semaphore", false);
    CADD_TYPE("tokio.sync.Notify",       "Notify",    false);
    CADD_TYPE("tokio.sync.oneshot.Sender","Sender",   false);
    CADD_TYPE("tokio.sync.oneshot.Receiver","Receiver", false);
    CADD_TYPE("tokio.sync.mpsc.Sender",   "Sender",   false);
    CADD_TYPE("tokio.sync.mpsc.Receiver", "Receiver", false);
    CADD_TYPE("tokio.sync.mpsc.UnboundedSender","UnboundedSender", false);
    CADD_TYPE("tokio.sync.mpsc.UnboundedReceiver","UnboundedReceiver", false);
    CADD_TYPE("tokio.net.TcpListener",   "TcpListener", false);
    CADD_TYPE("tokio.net.TcpStream",     "TcpStream",   false);
    CADD_TYPE("tokio.io.BufReader",      "BufReader",   false);
    CADD_TYPE("tokio.io.BufWriter",      "BufWriter",   false);
    CADD_TYPE("tokio.time.Sleep",        "Sleep",       false);
    CADD_TYPE("tokio.time.Interval",     "Interval",    false);
    CADD_TYPE("tokio.fs.File",           "File",        false);
    CADD_TYPE("tokio.process.Command",   "Command",     false);
    CADD_TYPE("tokio.process.Child",     "Child",       false);

    CADD_FUNC("tokio.runtime.Runtime", "new",       "tokio.runtime.Runtime.new",      hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Runtime", "block_on", "tokio.runtime.Runtime.block_on",  hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Runtime", "spawn",    "tokio.runtime.Runtime.spawn",     hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Runtime", "handle",   "tokio.runtime.Runtime.handle",    hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Runtime", "shutdown_timeout","tokio.runtime.Runtime.shutdown_timeout", t_unit);
    CADD_FUNC("tokio.runtime.Builder", "new_multi_thread","tokio.runtime.Builder.new_multi_thread", hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Builder", "new_current_thread","tokio.runtime.Builder.new_current_thread", hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Builder", "worker_threads","tokio.runtime.Builder.worker_threads", hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Builder", "enable_all","tokio.runtime.Builder.enable_all", hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Builder", "build",    "tokio.runtime.Builder.build",     hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Handle",  "current",  "tokio.runtime.Handle.current",    hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Handle",  "spawn",    "tokio.runtime.Handle.spawn",      hyp_type_unknown());
    CADD_FUNC("tokio.runtime.Handle",  "block_on", "tokio.runtime.Handle.block_on",   hyp_type_unknown());
    CADD_FUNC("tokio.task.JoinHandle", "await",    "tokio.task.JoinHandle.await",     hyp_type_unknown());
    CADD_FUNC("tokio.task.JoinHandle", "abort",    "tokio.task.JoinHandle.abort",     t_unit);
    CADD_FUNC("tokio.task.JoinHandle", "is_finished","tokio.task.JoinHandle.is_finished", t_bool);
    CADD_FUNC("tokio.sync.Mutex",      "new",      "tokio.sync.Mutex.new",            hyp_type_unknown());
    CADD_FUNC("tokio.sync.Mutex",      "lock",     "tokio.sync.Mutex.lock",           hyp_type_unknown());
    CADD_FUNC("tokio.sync.Mutex",      "try_lock", "tokio.sync.Mutex.try_lock",       hyp_type_unknown());
    CADD_FUNC("tokio.sync.RwLock",     "new",      "tokio.sync.RwLock.new",           hyp_type_unknown());
    CADD_FUNC("tokio.sync.RwLock",     "read",     "tokio.sync.RwLock.read",          hyp_type_unknown());
    CADD_FUNC("tokio.sync.RwLock",     "write",    "tokio.sync.RwLock.write",         hyp_type_unknown());
    CADD_FUNC("tokio.sync.Semaphore",  "new",      "tokio.sync.Semaphore.new",        hyp_type_unknown());
    CADD_FUNC("tokio.sync.Semaphore",  "acquire",  "tokio.sync.Semaphore.acquire",    hyp_type_unknown());
    CADD_FUNC("tokio.sync.Semaphore",  "add_permits","tokio.sync.Semaphore.add_permits", t_unit);
    CADD_FUNC("tokio.sync.Notify",     "new",      "tokio.sync.Notify.new",           hyp_type_unknown());
    CADD_FUNC("tokio.sync.Notify",     "notified", "tokio.sync.Notify.notified",      hyp_type_unknown());
    CADD_FUNC("tokio.sync.Notify",     "notify_one","tokio.sync.Notify.notify_one",   t_unit);
    CADD_FUNC("tokio.sync.Notify",     "notify_waiters","tokio.sync.Notify.notify_waiters", t_unit);
    CADD_FUNC("tokio.sync.mpsc.Sender",  "send",   "tokio.sync.mpsc.Sender.send",     hyp_type_unknown());
    CADD_FUNC("tokio.sync.mpsc.Sender",  "try_send","tokio.sync.mpsc.Sender.try_send",hyp_type_unknown());
    CADD_FUNC("tokio.sync.mpsc.Receiver","recv",   "tokio.sync.mpsc.Receiver.recv",   hyp_type_unknown());
    CADD_FUNC("tokio.sync.mpsc.Receiver","try_recv","tokio.sync.mpsc.Receiver.try_recv",hyp_type_unknown());
    CADD_FUNC("tokio.sync.oneshot.Sender","send",  "tokio.sync.oneshot.Sender.send",  hyp_type_unknown());
    CADD_FUNC("tokio.sync.oneshot.Receiver","await","tokio.sync.oneshot.Receiver.await", hyp_type_unknown());
    CADD_FUNC("tokio.net.TcpListener", "bind",     "tokio.net.TcpListener.bind",      hyp_type_unknown());
    CADD_FUNC("tokio.net.TcpListener", "accept",   "tokio.net.TcpListener.accept",    hyp_type_unknown());
    CADD_FUNC("tokio.net.TcpListener", "local_addr","tokio.net.TcpListener.local_addr",hyp_type_unknown());
    CADD_FUNC("tokio.net.TcpStream",   "connect",  "tokio.net.TcpStream.connect",     hyp_type_unknown());
    CADD_FUNC("tokio.net.TcpStream",   "peer_addr","tokio.net.TcpStream.peer_addr",   hyp_type_unknown());
    CADD_FUNC("tokio.time.Sleep",      "await",    "tokio.time.Sleep.await",          t_unit);
    CADD_FUNC("tokio.time.Interval",   "tick",     "tokio.time.Interval.tick",        hyp_type_unknown());
    CADD_FUNC("tokio.fs.File",         "open",     "tokio.fs.File.open",              hyp_type_unknown());
    CADD_FUNC("tokio.fs.File",         "create",   "tokio.fs.File.create",            hyp_type_unknown());
    /* Free functions. */
    CADD_FUNC(NULL, "spawn",    "tokio.spawn",     hyp_type_named(arena, "tokio.task.JoinHandle"));
    CADD_FUNC(NULL, "spawn_blocking","tokio.task.spawn_blocking", hyp_type_named(arena, "tokio.task.JoinHandle"));
    CADD_FUNC(NULL, "yield_now","tokio.task.yield_now", t_unit);
    CADD_FUNC(NULL, "sleep",    "tokio.time.sleep", hyp_type_named(arena, "tokio.time.Sleep"));
    CADD_FUNC(NULL, "sleep_until","tokio.time.sleep_until", hyp_type_named(arena, "tokio.time.Sleep"));
    CADD_FUNC(NULL, "interval", "tokio.time.interval", hyp_type_named(arena, "tokio.time.Interval"));
    CADD_FUNC(NULL, "timeout",  "tokio.time.timeout", hyp_type_unknown());
    CADD_FUNC(NULL, "read_to_string","tokio.fs.read_to_string", hyp_type_unknown());
    CADD_FUNC(NULL, "write",    "tokio.fs.write",  hyp_type_unknown());
    CADD_FUNC(NULL, "read",     "tokio.fs.read",   hyp_type_unknown());
    CADD_FUNC(NULL, "channel",  "tokio.sync.mpsc.channel", hyp_type_unknown());
    CADD_FUNC(NULL, "unbounded_channel","tokio.sync.mpsc.unbounded_channel", hyp_type_unknown());
    CADD_FUNC(NULL, "join",     "tokio.join",      hyp_type_unknown());
    CADD_FUNC(NULL, "try_join", "tokio.try_join",  hyp_type_unknown());
    CADD_FUNC(NULL, "select",   "tokio.select",    hyp_type_unknown());

    /* ── clap — argument parsing. ─────────────────────────── */
    CADD_TYPE("clap.Parser",     "Parser",     true);
    CADD_TYPE("clap.Args",       "Args",       true);
    CADD_TYPE("clap.Subcommand", "Subcommand", true);
    CADD_TYPE("clap.ValueEnum",  "ValueEnum",  true);
    CADD_TYPE("clap.Command",    "Command",    false);
    CADD_TYPE("clap.Arg",        "Arg",        false);
    CADD_TYPE("clap.ArgMatches", "ArgMatches", false);

    CADD_FUNC("clap.Parser", "parse",         "clap.Parser.parse",         hyp_type_unknown());
    CADD_FUNC("clap.Parser", "try_parse",     "clap.Parser.try_parse",     hyp_type_unknown());
    CADD_FUNC("clap.Parser", "parse_from",    "clap.Parser.parse_from",    hyp_type_unknown());
    CADD_FUNC("clap.Parser", "try_parse_from","clap.Parser.try_parse_from",hyp_type_unknown());
    CADD_FUNC("clap.Parser", "command",       "clap.Parser.command",       hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","new",           "clap.Command.new",          hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","arg",           "clap.Command.arg",          hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","subcommand",    "clap.Command.subcommand",   hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","about",         "clap.Command.about",        hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","version",       "clap.Command.version",      hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","author",        "clap.Command.author",       hyp_type_named(arena, "clap.Command"));
    CADD_FUNC("clap.Command","get_matches",   "clap.Command.get_matches",  hyp_type_named(arena, "clap.ArgMatches"));
    CADD_FUNC("clap.Command","get_matches_from","clap.Command.get_matches_from", hyp_type_named(arena, "clap.ArgMatches"));
    CADD_FUNC("clap.Arg",    "new",           "clap.Arg.new",              hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.Arg",    "short",         "clap.Arg.short",            hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.Arg",    "long",          "clap.Arg.long",             hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.Arg",    "value_name",    "clap.Arg.value_name",       hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.Arg",    "required",      "clap.Arg.required",         hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.Arg",    "default_value", "clap.Arg.default_value",    hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.Arg",    "help",          "clap.Arg.help",             hyp_type_named(arena, "clap.Arg"));
    CADD_FUNC("clap.ArgMatches","get_one",    "clap.ArgMatches.get_one",   hyp_type_unknown());
    CADD_FUNC("clap.ArgMatches","get_many",   "clap.ArgMatches.get_many",  hyp_type_unknown());
    CADD_FUNC("clap.ArgMatches","get_flag",   "clap.ArgMatches.get_flag",  t_bool);
    CADD_FUNC("clap.ArgMatches","contains_id","clap.ArgMatches.contains_id",t_bool);
    CADD_FUNC("clap.ArgMatches","subcommand", "clap.ArgMatches.subcommand",hyp_type_unknown());

    /* ── regex — pattern matching. ────────────────────────── */
    CADD_TYPE("regex.Regex",     "Regex",     false);
    CADD_TYPE("regex.Captures",  "Captures",  false);
    CADD_TYPE("regex.Match",     "Match",     false);
    CADD_TYPE("regex.RegexBuilder","RegexBuilder", false);

    CADD_FUNC("regex.Regex", "new",         "regex.Regex.new",         hyp_type_unknown());
    CADD_FUNC("regex.Regex", "is_match",    "regex.Regex.is_match",    t_bool);
    CADD_FUNC("regex.Regex", "find",        "regex.Regex.find",        hyp_type_unknown());
    CADD_FUNC("regex.Regex", "find_iter",   "regex.Regex.find_iter",   hyp_type_unknown());
    CADD_FUNC("regex.Regex", "captures",    "regex.Regex.captures",    hyp_type_unknown());
    CADD_FUNC("regex.Regex", "captures_iter","regex.Regex.captures_iter", hyp_type_unknown());
    CADD_FUNC("regex.Regex", "replace",     "regex.Regex.replace",     t_string);
    CADD_FUNC("regex.Regex", "replace_all", "regex.Regex.replace_all", t_string);
    CADD_FUNC("regex.Regex", "split",       "regex.Regex.split",       hyp_type_unknown());
    CADD_FUNC("regex.Regex", "as_str",      "regex.Regex.as_str",      t_str_ref);
    CADD_FUNC("regex.Captures","get",       "regex.Captures.get",      hyp_type_unknown());
    CADD_FUNC("regex.Captures","name",      "regex.Captures.name",     hyp_type_unknown());
    CADD_FUNC("regex.Captures","len",       "regex.Captures.len",      t_usize);
    CADD_FUNC("regex.Match",   "as_str",    "regex.Match.as_str",      t_str_ref);
    CADD_FUNC("regex.Match",   "start",     "regex.Match.start",       t_usize);
    CADD_FUNC("regex.Match",   "end",       "regex.Match.end",         t_usize);
    CADD_FUNC("regex.Match",   "range",     "regex.Match.range",       hyp_type_unknown());

    /* ── log — logging macros + Logger trait. ─────────────── */
    CADD_TYPE("log.Logger",      "Logger",      true);
    CADD_TYPE("log.Level",       "Level",       false);
    CADD_TYPE("log.LevelFilter", "LevelFilter", false);
    CADD_TYPE("log.Metadata",    "Metadata",    false);
    CADD_TYPE("log.Record",      "Record",      false);
    /* The macros log!/info!/warn!/error!/debug!/trace! are matched by
     * name in rust_lsp.c's macro handler — register them as void
     * synthetic functions so the resolver can attribute them. */
    CADD_FUNC(NULL, "info",  "log.info",  t_unit);
    CADD_FUNC(NULL, "warn",  "log.warn",  t_unit);
    CADD_FUNC(NULL, "error", "log.error", t_unit);
    CADD_FUNC(NULL, "debug", "log.debug", t_unit);
    CADD_FUNC(NULL, "trace", "log.trace", t_unit);
    CADD_FUNC(NULL, "log",   "log.log",   t_unit);
    CADD_FUNC(NULL, "set_logger","log.set_logger", hyp_type_unknown());
    CADD_FUNC(NULL, "set_max_level","log.set_max_level", t_unit);

    /* ── futures — Future combinators + stream / sink. ────── */
    CADD_TYPE("futures.Future",   "Future",   true);
    CADD_TYPE("futures.Stream",   "Stream",   true);
    CADD_TYPE("futures.Sink",     "Sink",     true);
    CADD_TYPE("futures.StreamExt","StreamExt",true);
    CADD_TYPE("futures.FutureExt","FutureExt",true);
    CADD_TYPE("futures.SinkExt",  "SinkExt",  true);
    CADD_TYPE("futures.executor.LocalPool","LocalPool", false);

    CADD_FUNC("futures.StreamExt", "next",        "futures.StreamExt.next",        hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "map",         "futures.StreamExt.map",         hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "filter",      "futures.StreamExt.filter",      hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "filter_map",  "futures.StreamExt.filter_map",  hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "for_each",    "futures.StreamExt.for_each",    hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "collect",     "futures.StreamExt.collect",     hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "fold",        "futures.StreamExt.fold",        hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "take",        "futures.StreamExt.take",        hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "skip",        "futures.StreamExt.skip",        hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "chunks",      "futures.StreamExt.chunks",      hyp_type_unknown());
    CADD_FUNC("futures.StreamExt", "buffered",    "futures.StreamExt.buffered",    hyp_type_unknown());
    CADD_FUNC("futures.SinkExt",   "send",        "futures.SinkExt.send",          hyp_type_unknown());
    CADD_FUNC("futures.SinkExt",   "send_all",    "futures.SinkExt.send_all",      hyp_type_unknown());
    CADD_FUNC("futures.SinkExt",   "flush",       "futures.SinkExt.flush",         hyp_type_unknown());
    CADD_FUNC("futures.SinkExt",   "close",       "futures.SinkExt.close",         hyp_type_unknown());
    CADD_FUNC("futures.FutureExt", "boxed",       "futures.FutureExt.boxed",       hyp_type_unknown());
    CADD_FUNC("futures.FutureExt", "fuse",        "futures.FutureExt.fuse",        hyp_type_unknown());
    CADD_FUNC("futures.FutureExt", "shared",      "futures.FutureExt.shared",      hyp_type_unknown());

    CADD_FUNC(NULL, "join_all",     "futures.future.join_all",       hyp_type_unknown());
    CADD_FUNC(NULL, "try_join_all", "futures.future.try_join_all",   hyp_type_unknown());
    CADD_FUNC(NULL, "select",       "futures.future.select",         hyp_type_unknown());
    CADD_FUNC(NULL, "ready",        "futures.future.ready",          hyp_type_unknown());

    /* ── parking_lot — drop-in std::sync alternatives. ────── */
    CADD_TYPE("parking_lot.Mutex",        "Mutex",        false);
    CADD_TYPE("parking_lot.RwLock",       "RwLock",       false);
    CADD_TYPE("parking_lot.MutexGuard",   "MutexGuard",   false);
    CADD_TYPE("parking_lot.RwLockReadGuard","RwLockReadGuard", false);
    CADD_TYPE("parking_lot.RwLockWriteGuard","RwLockWriteGuard", false);
    CADD_TYPE("parking_lot.Condvar",      "Condvar",      false);
    CADD_TYPE("parking_lot.Once",         "Once",         false);

    CADD_FUNC("parking_lot.Mutex",  "new",      "parking_lot.Mutex.new",      hyp_type_unknown());
    CADD_FUNC("parking_lot.Mutex",  "lock",     "parking_lot.Mutex.lock",     hyp_type_unknown());
    CADD_FUNC("parking_lot.Mutex",  "try_lock", "parking_lot.Mutex.try_lock", hyp_type_unknown());
    CADD_FUNC("parking_lot.RwLock", "new",      "parking_lot.RwLock.new",     hyp_type_unknown());
    CADD_FUNC("parking_lot.RwLock", "read",     "parking_lot.RwLock.read",    hyp_type_unknown());
    CADD_FUNC("parking_lot.RwLock", "write",    "parking_lot.RwLock.write",   hyp_type_unknown());

    /* ── lazy_static / once_cell — singleton init. ────────── */
    CADD_TYPE("once_cell.sync.Lazy",  "Lazy",  false);
    CADD_TYPE("once_cell.sync.OnceCell","OnceCell", false);
    CADD_TYPE("once_cell.unsync.Lazy","Lazy",  false);
    CADD_FUNC("once_cell.sync.Lazy","new","once_cell.sync.Lazy.new",hyp_type_unknown());
    CADD_FUNC("once_cell.sync.OnceCell","new","once_cell.sync.OnceCell.new",hyp_type_unknown());
    CADD_FUNC("once_cell.sync.OnceCell","get","once_cell.sync.OnceCell.get",hyp_type_unknown());
    CADD_FUNC("once_cell.sync.OnceCell","set","once_cell.sync.OnceCell.set",hyp_type_unknown());
    CADD_FUNC("once_cell.sync.OnceCell","get_or_init","once_cell.sync.OnceCell.get_or_init", hyp_type_unknown());

    /* ── chrono — date/time. ────────────────────────────── */
    CADD_TYPE("chrono.DateTime",     "DateTime",     false);
    CADD_TYPE("chrono.NaiveDate",    "NaiveDate",    false);
    CADD_TYPE("chrono.NaiveTime",    "NaiveTime",    false);
    CADD_TYPE("chrono.NaiveDateTime","NaiveDateTime",false);
    CADD_TYPE("chrono.Duration",     "Duration",     false);
    CADD_TYPE("chrono.Utc",          "Utc",          false);
    CADD_TYPE("chrono.Local",        "Local",        false);

    CADD_FUNC("chrono.DateTime",     "now",       "chrono.DateTime.now",       hyp_type_unknown());
    CADD_FUNC("chrono.DateTime",     "format",    "chrono.DateTime.format",    hyp_type_unknown());
    CADD_FUNC("chrono.DateTime",     "timestamp", "chrono.DateTime.timestamp", hyp_type_unknown());
    CADD_FUNC("chrono.DateTime",     "to_rfc3339","chrono.DateTime.to_rfc3339",t_string);
    CADD_FUNC("chrono.Utc",          "now",       "chrono.Utc.now",            hyp_type_unknown());
    CADD_FUNC("chrono.Local",        "now",       "chrono.Local.now",          hyp_type_unknown());
    CADD_FUNC("chrono.Duration",     "seconds",   "chrono.Duration.seconds",   hyp_type_unknown());
    CADD_FUNC("chrono.Duration",     "minutes",   "chrono.Duration.minutes",   hyp_type_unknown());
    CADD_FUNC("chrono.Duration",     "hours",     "chrono.Duration.hours",     hyp_type_unknown());
    CADD_FUNC("chrono.Duration",     "days",      "chrono.Duration.days",      hyp_type_unknown());

    /* ── uuid — IDs. ────────────────────────────────────── */
    CADD_TYPE("uuid.Uuid", "Uuid", false);
    CADD_FUNC("uuid.Uuid", "new_v4",     "uuid.Uuid.new_v4",     hyp_type_unknown());
    CADD_FUNC("uuid.Uuid", "nil",        "uuid.Uuid.nil",        hyp_type_unknown());
    CADD_FUNC("uuid.Uuid", "parse_str",  "uuid.Uuid.parse_str",  hyp_type_unknown());
    CADD_FUNC("uuid.Uuid", "to_string",  "uuid.Uuid.to_string",  t_string);
    CADD_FUNC("uuid.Uuid", "to_hyphenated","uuid.Uuid.to_hyphenated", hyp_type_unknown());
    CADD_FUNC("uuid.Uuid", "as_bytes",   "uuid.Uuid.as_bytes",   hyp_type_unknown());

    /* ── reqwest — HTTP client. ──────────────────────────── */
    CADD_TYPE("reqwest.Client",       "Client",       false);
    CADD_TYPE("reqwest.RequestBuilder","RequestBuilder", false);
    CADD_TYPE("reqwest.Response",     "Response",     false);
    CADD_TYPE("reqwest.Error",        "Error",        false);
    CADD_TYPE("reqwest.Url",          "Url",          false);
    CADD_TYPE("reqwest.header.HeaderMap","HeaderMap", false);

    CADD_FUNC("reqwest.Client",  "new",      "reqwest.Client.new",      hyp_type_named(arena, "reqwest.Client"));
    CADD_FUNC("reqwest.Client",  "builder",  "reqwest.Client.builder",  hyp_type_unknown());
    CADD_FUNC("reqwest.Client",  "get",      "reqwest.Client.get",      hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.Client",  "post",     "reqwest.Client.post",     hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.Client",  "put",      "reqwest.Client.put",      hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.Client",  "delete",   "reqwest.Client.delete",   hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.Client",  "request",  "reqwest.Client.request",  hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","header","reqwest.RequestBuilder.header", hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","headers","reqwest.RequestBuilder.headers", hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","body", "reqwest.RequestBuilder.body", hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","json", "reqwest.RequestBuilder.json", hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","form", "reqwest.RequestBuilder.form", hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","query","reqwest.RequestBuilder.query", hyp_type_named(arena, "reqwest.RequestBuilder"));
    CADD_FUNC("reqwest.RequestBuilder","send", "reqwest.RequestBuilder.send", hyp_type_named(arena, "reqwest.Response"));
    CADD_FUNC("reqwest.Response","status",     "reqwest.Response.status",     hyp_type_unknown());
    CADD_FUNC("reqwest.Response","headers",    "reqwest.Response.headers",    hyp_type_unknown());
    CADD_FUNC("reqwest.Response","text",       "reqwest.Response.text",       hyp_type_unknown());
    CADD_FUNC("reqwest.Response","json",       "reqwest.Response.json",       hyp_type_unknown());
    CADD_FUNC("reqwest.Response","bytes",      "reqwest.Response.bytes",      hyp_type_unknown());
    CADD_FUNC("reqwest.Url",     "parse",      "reqwest.Url.parse",           hyp_type_unknown());

    /* ── rayon — parallel iteration. ────────────────────── */
    CADD_TYPE("rayon.iter.ParallelIterator","ParallelIterator", true);
    CADD_TYPE("rayon.iter.IntoParallelIterator","IntoParallelIterator", true);

    CADD_FUNC("rayon.iter.ParallelIterator","map",      "rayon.iter.ParallelIterator.map",      hyp_type_unknown());
    CADD_FUNC("rayon.iter.ParallelIterator","filter",   "rayon.iter.ParallelIterator.filter",   hyp_type_unknown());
    CADD_FUNC("rayon.iter.ParallelIterator","for_each", "rayon.iter.ParallelIterator.for_each", t_unit);
    CADD_FUNC("rayon.iter.ParallelIterator","collect",  "rayon.iter.ParallelIterator.collect",  hyp_type_unknown());
    CADD_FUNC("rayon.iter.ParallelIterator","reduce",   "rayon.iter.ParallelIterator.reduce",   hyp_type_unknown());
    CADD_FUNC("rayon.iter.ParallelIterator","sum",      "rayon.iter.ParallelIterator.sum",      hyp_type_unknown());
    CADD_FUNC("rayon.iter.ParallelIterator","count",    "rayon.iter.ParallelIterator.count",    t_usize);
    CADD_FUNC("rayon.iter.ParallelIterator","any",      "rayon.iter.ParallelIterator.any",      t_bool);
    CADD_FUNC("rayon.iter.ParallelIterator","all",      "rayon.iter.ParallelIterator.all",      t_bool);
    CADD_FUNC("rayon.iter.IntoParallelIterator","into_par_iter","rayon.iter.IntoParallelIterator.into_par_iter", hyp_type_unknown());

    CADD_FUNC(NULL, "scope",       "rayon.scope",        hyp_type_unknown());
    CADD_FUNC(NULL, "par_iter",    "rayon.par_iter",     hyp_type_unknown());
    CADD_FUNC(NULL, "spawn",       "rayon.spawn",        t_unit);
    CADD_FUNC(NULL, "join",        "rayon.join",         hyp_type_unknown());

    /* ── async-trait / async_trait — typically derive-only. ──
     * Calls to trait methods are resolved through the normal trait
     * dispatch since async_trait emits real Rust impl blocks. No
     * extra registration needed. */
    (void)t_str_ref;
}

#endif  /* RUST_CRATES_SEED_INCLUDED */
