/*
 * java_stdlib_data.c — Curated Java standard-library type/method registry.
 *
 * Strategy:
 *   - java.lang.* — fully covered (the implicit-import package).
 *     Object, String, StringBuilder, StringBuffer, CharSequence, Class,
 *     Throwable + the common subclass tree, Number + boxed primitives,
 *     Math, System, Thread, Iterable, Comparable, Cloneable, Enum, Record,
 *     AutoCloseable, the common Exception types.
 *   - java.util.* — collections + iterators + Optional + Date/Calendar +
 *     Arrays/Collections + Scanner/Random/UUID + Map.Entry.
 *   - java.io.* — streams, readers, writers, File, IOException family.
 *   - java.nio.file.* — Path, Paths, Files (often-used helpers).
 *   - java.util.function — the 21 functional interfaces.
 *   - java.util.stream  — Stream + Collectors entry points.
 *   - java.util.concurrent — ExecutorService, Future, CompletableFuture,
 *     ConcurrentHashMap, the concurrent collection set.
 *   - java.time — LocalDate/LocalTime/LocalDateTime/Duration/Instant.
 *
 * Method signatures use registry-level fidelity: receiver, short name,
 * return type. Param types are intentionally unmodeled (the resolver
 * chooses overloads by arity, with type compatibility scoring breaking
 * ties — see hyp_registry_lookup_method_by_args).
 *
 * This is the JLS-spec-aligned slice of the stdlib that 90%+ of real-world
 * Java code touches.
 */

#include "../type_rep.h"
#include "../type_registry.h"
#include "../../arena.h"
#include "../java_lsp.h"
#include <string.h>

#define REG_TYPE(qn_, short_, is_iface_, parents_)            \
    do {                                                      \
        memset(&rt, 0, sizeof(rt));                           \
        rt.qualified_name = (qn_);                            \
        rt.short_name = (short_);                             \
        rt.is_interface = (is_iface_);                        \
        rt.embedded_types = (parents_);                       \
        hyp_registry_add_type(reg, rt);                       \
    } while (0)

#define REG_METHOD(class_qn_, method_name_, ret_type_)                                          \
    do {                                                                                        \
        memset(&rf, 0, sizeof(rf));                                                             \
        rf.min_params = -1;                                                                     \
        rf.qualified_name =                                                                     \
            hyp_arena_sprintf(arena, "%s.%s", (class_qn_), (method_name_));                     \
        rf.short_name = (method_name_);                                                         \
        rf.receiver_type = (class_qn_);                                                         \
        {                                                                                       \
            const HYPType **rets =                                                              \
                (const HYPType **)hyp_arena_alloc(arena, 2 * sizeof(*rets));                    \
            rets[0] = (ret_type_);                                                              \
            rets[1] = NULL;                                                                     \
            rf.signature = hyp_type_func(arena, NULL, NULL, rets);                              \
        }                                                                                       \
        hyp_registry_add_func(reg, rf);                                                         \
    } while (0)

#define REG_CTOR(class_qn_, short_name_)                                              \
    do {                                                                              \
        memset(&rf, 0, sizeof(rf));                                                   \
        rf.min_params = -1;                                                           \
        rf.qualified_name =                                                           \
            hyp_arena_sprintf(arena, "%s.%s", (class_qn_), (short_name_));            \
        rf.short_name = (short_name_);                                                \
        rf.receiver_type = (class_qn_);                                               \
        {                                                                             \
            const HYPType **rets =                                                    \
                (const HYPType **)hyp_arena_alloc(arena, 2 * sizeof(*rets));          \
            rets[0] = hyp_type_named(arena, (class_qn_));                             \
            rets[1] = NULL;                                                           \
            rf.signature = hyp_type_func(arena, NULL, NULL, rets);                    \
        }                                                                             \
        hyp_registry_add_func(reg, rf);                                               \
    } while (0)

#define REG_FIELD(class_qn_, name_, type_)                                            \
    do {                                                                              \
        const HYPRegisteredType *_existing =                                          \
            hyp_registry_lookup_type(reg, (class_qn_));                               \
        (void)_existing;                                                              \
        /* Field append handled by REG_TYPE_FIELDS below. */                          \
        /* Placeholder for future per-field appends. */                               \
    } while (0)

void hyp_java_stdlib_register(HYPTypeRegistry *reg, HYPArena *arena) {
    HYPRegisteredType rt;
    HYPRegisteredFunc rf;

    /* ── Type-parent lists (must be static so addresses outlive the call) ── */
    static const char *no_parents[] = {NULL};
    static const char *parents_object[] = {"java.lang.Object", NULL};
    static const char *parents_throwable[] = {"java.lang.Object", NULL};
    static const char *parents_exception[] = {"java.lang.Throwable", NULL};
    static const char *parents_error[] = {"java.lang.Throwable", NULL};
    static const char *parents_runtime_exc[] = {"java.lang.Exception", NULL};
    static const char *parents_io_exc[] = {"java.lang.Exception", NULL};
    static const char *parents_number[] = {"java.lang.Object", NULL};
    static const char *parents_integer[] = {"java.lang.Number", NULL};
    static const char *parents_long[] = {"java.lang.Number", NULL};
    static const char *parents_double[] = {"java.lang.Number", NULL};
    static const char *parents_float[] = {"java.lang.Number", NULL};
    static const char *parents_short[] = {"java.lang.Number", NULL};
    static const char *parents_byte[] = {"java.lang.Number", NULL};
    static const char *parents_string[] = {"java.lang.Object", NULL};
    static const char *parents_charseq[] = {NULL};
    static const char *parents_iterable[] = {NULL};
    static const char *parents_collection[] = {"java.lang.Iterable", NULL};
    static const char *parents_list[] = {"java.util.Collection", NULL};
    static const char *parents_set[] = {"java.util.Collection", NULL};
    static const char *parents_queue[] = {"java.util.Collection", NULL};
    static const char *parents_deque[] = {"java.util.Queue", NULL};
    static const char *parents_map[] = {NULL};
    static const char *parents_map_entry[] = {NULL};
    static const char *parents_iterator[] = {NULL};
    static const char *parents_arraylist[] = {"java.util.List", NULL};
    static const char *parents_linkedlist[] = {"java.util.List", NULL};
    static const char *parents_hashset[] = {"java.util.Set", NULL};
    static const char *parents_treeset[] = {"java.util.Set", NULL};
    static const char *parents_linkedhashset[] = {"java.util.Set", NULL};
    static const char *parents_hashmap[] = {"java.util.Map", NULL};
    static const char *parents_treemap[] = {"java.util.Map", NULL};
    static const char *parents_linkedhashmap[] = {"java.util.Map", NULL};
    static const char *parents_concurrent_hashmap[] = {"java.util.Map", NULL};

    static const char *parents_inputstream[] = {"java.lang.AutoCloseable", NULL};
    static const char *parents_outputstream[] = {"java.lang.AutoCloseable", NULL};
    static const char *parents_reader[] = {"java.lang.AutoCloseable", NULL};
    static const char *parents_writer[] = {"java.lang.AutoCloseable", NULL};
    static const char *parents_buffered_reader[] = {"java.io.Reader", NULL};
    static const char *parents_buffered_writer[] = {"java.io.Writer", NULL};
    static const char *parents_print_stream[] = {"java.io.OutputStream", NULL};
    static const char *parents_print_writer[] = {"java.io.Writer", NULL};
    static const char *parents_file_input_stream[] = {"java.io.InputStream", NULL};
    static const char *parents_file_output_stream[] = {"java.io.OutputStream", NULL};
    static const char *parents_file_reader[] = {"java.io.Reader", NULL};
    static const char *parents_file_writer[] = {"java.io.Writer", NULL};
    static const char *parents_io_exception[] = {"java.lang.Exception", NULL};
    static const char *parents_runtime_exc_chain[] = {"java.lang.RuntimeException", NULL};
    /* Parent lists for types previously registered with inline compound
     * literals. A compound literal has automatic (block) storage duration,
     * so storing its address into the registry left a dangling stack pointer
     * once the REG_TYPE statement's block ended — an AddressSanitizer
     * stack-use-after-scope when the inheritance walk later read
     * rt->embedded_types[0]. These must be static so their addresses outlive
     * the call, exactly like the parent lists above. */
    static const char *parents_gregorian_calendar[] = {"java.util.Calendar", NULL};
    static const char *parents_file_not_found_exc[] = {"java.io.IOException", NULL};
    static const char *parents_closeable[] = {"java.lang.AutoCloseable", NULL};
    static const char *parents_unary_operator[] = {"java.util.function.Function", NULL};
    static const char *parents_binary_operator[] = {"java.util.function.BiFunction", NULL};
    static const char *parents_completable_future[] = {"java.util.concurrent.Future", NULL};
    static const char *parents_reentrant_lock[] = {"java.util.concurrent.locks.Lock", NULL};

    /* ── java.lang ─────────────────────────────────────────────── */
    REG_TYPE("java.lang.Object", "Object", false, no_parents);
    REG_TYPE("java.lang.Class", "Class", false, parents_object);
    REG_TYPE("java.lang.ClassLoader", "ClassLoader", false, parents_object);
    REG_TYPE("java.lang.CharSequence", "CharSequence", true, parents_charseq);
    REG_TYPE("java.lang.String", "String", false, parents_string);
    REG_TYPE("java.lang.StringBuilder", "StringBuilder", false, parents_object);
    REG_TYPE("java.lang.StringBuffer", "StringBuffer", false, parents_object);
    REG_TYPE("java.lang.Number", "Number", false, parents_number);
    REG_TYPE("java.lang.Integer", "Integer", false, parents_integer);
    REG_TYPE("java.lang.Long", "Long", false, parents_long);
    REG_TYPE("java.lang.Short", "Short", false, parents_short);
    REG_TYPE("java.lang.Byte", "Byte", false, parents_byte);
    REG_TYPE("java.lang.Float", "Float", false, parents_float);
    REG_TYPE("java.lang.Double", "Double", false, parents_double);
    REG_TYPE("java.lang.Boolean", "Boolean", false, parents_object);
    REG_TYPE("java.lang.Character", "Character", false, parents_object);
    REG_TYPE("java.lang.Void", "Void", false, parents_object);
    REG_TYPE("java.lang.Iterable", "Iterable", true, parents_iterable);
    REG_TYPE("java.lang.Comparable", "Comparable", true, no_parents);
    REG_TYPE("java.lang.Cloneable", "Cloneable", true, no_parents);
    REG_TYPE("java.lang.Runnable", "Runnable", true, no_parents);
    REG_TYPE("java.lang.AutoCloseable", "AutoCloseable", true, no_parents);
    REG_TYPE("java.lang.Math", "Math", false, parents_object);
    REG_TYPE("java.lang.System", "System", false, parents_object);
    REG_TYPE("java.lang.Thread", "Thread", false, parents_object);
    REG_TYPE("java.lang.Process", "Process", false, parents_object);
    REG_TYPE("java.lang.ProcessBuilder", "ProcessBuilder", false, parents_object);
    REG_TYPE("java.lang.StackTraceElement", "StackTraceElement", false, parents_object);
    REG_TYPE("java.lang.Enum", "Enum", false, parents_object);
    REG_TYPE("java.lang.Record", "Record", false, parents_object);
    REG_TYPE("java.lang.Throwable", "Throwable", false, parents_throwable);
    REG_TYPE("java.lang.Exception", "Exception", false, parents_exception);
    REG_TYPE("java.lang.Error", "Error", false, parents_error);
    REG_TYPE("java.lang.RuntimeException", "RuntimeException", false, parents_runtime_exc);
    REG_TYPE("java.lang.NullPointerException", "NullPointerException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.IllegalArgumentException", "IllegalArgumentException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.IllegalStateException", "IllegalStateException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.IndexOutOfBoundsException", "IndexOutOfBoundsException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.ArrayIndexOutOfBoundsException", "ArrayIndexOutOfBoundsException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.ArithmeticException", "ArithmeticException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.ClassCastException", "ClassCastException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.ClassNotFoundException", "ClassNotFoundException", false,
             parents_exception);
    REG_TYPE("java.lang.NumberFormatException", "NumberFormatException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.UnsupportedOperationException", "UnsupportedOperationException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.InterruptedException", "InterruptedException", false, parents_exception);
    REG_TYPE("java.lang.SecurityException", "SecurityException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.lang.NoSuchMethodException", "NoSuchMethodException", false, parents_exception);
    REG_TYPE("java.lang.NoSuchFieldException", "NoSuchFieldException", false, parents_exception);

    /* Annotation-marker types. */
    REG_TYPE("java.lang.Override", "Override", true, no_parents);
    REG_TYPE("java.lang.Deprecated", "Deprecated", true, no_parents);
    REG_TYPE("java.lang.SuppressWarnings", "SuppressWarnings", true, no_parents);
    REG_TYPE("java.lang.FunctionalInterface", "FunctionalInterface", true, no_parents);

    /* ── Object methods ───────────────────────────────────────── */
    REG_METHOD("java.lang.Object", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Object", "hashCode", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Object", "equals", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Object", "getClass", hyp_type_named(arena, "java.lang.Class"));
    REG_METHOD("java.lang.Object", "wait", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Object", "notify", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Object", "notifyAll", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Object", "clone", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.lang.Object", "finalize", hyp_type_builtin(arena, "void"));

    /* ── String methods ───────────────────────────────────────── */
    REG_METHOD("java.lang.String", "length", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "isBlank", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "charAt", hyp_type_builtin(arena, "char"));
    REG_METHOD("java.lang.String", "codePointAt", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "equals", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "equalsIgnoreCase", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "compareTo", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "compareToIgnoreCase", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "indexOf", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "lastIndexOf", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "contains", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "startsWith", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "endsWith", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "matches", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.String", "concat", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "substring", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "trim", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "strip", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "stripLeading", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "stripTrailing", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "toLowerCase", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "toUpperCase", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "replace", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "replaceAll", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "replaceFirst", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "split",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.String")));
    REG_METHOD("java.lang.String", "toCharArray", hyp_type_slice(arena, hyp_type_builtin(arena, "char")));
    REG_METHOD("java.lang.String", "getBytes", hyp_type_slice(arena, hyp_type_builtin(arena, "byte")));
    REG_METHOD("java.lang.String", "intern", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "format", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "valueOf", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "join", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "repeat", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "lines", hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.lang.String", "chars", hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.lang.String", "codePoints",
               hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.lang.String", "hashCode", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.String", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.String", "toCharArray", hyp_type_slice(arena, hyp_type_builtin(arena, "char")));
    REG_CTOR("java.lang.String", "String");

    /* ── StringBuilder / StringBuffer ─────────────────────────── */
    REG_METHOD("java.lang.StringBuilder", "append",
               hyp_type_named(arena, "java.lang.StringBuilder"));
    REG_METHOD("java.lang.StringBuilder", "insert",
               hyp_type_named(arena, "java.lang.StringBuilder"));
    REG_METHOD("java.lang.StringBuilder", "delete",
               hyp_type_named(arena, "java.lang.StringBuilder"));
    REG_METHOD("java.lang.StringBuilder", "deleteCharAt",
               hyp_type_named(arena, "java.lang.StringBuilder"));
    REG_METHOD("java.lang.StringBuilder", "replace",
               hyp_type_named(arena, "java.lang.StringBuilder"));
    REG_METHOD("java.lang.StringBuilder", "reverse",
               hyp_type_named(arena, "java.lang.StringBuilder"));
    REG_METHOD("java.lang.StringBuilder", "toString",
               hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.StringBuilder", "length", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.StringBuilder", "charAt", hyp_type_builtin(arena, "char"));
    REG_METHOD("java.lang.StringBuilder", "setLength", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.StringBuilder", "indexOf", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.StringBuilder", "substring",
               hyp_type_named(arena, "java.lang.String"));
    REG_CTOR("java.lang.StringBuilder", "StringBuilder");

    REG_METHOD("java.lang.StringBuffer", "append",
               hyp_type_named(arena, "java.lang.StringBuffer"));
    REG_METHOD("java.lang.StringBuffer", "toString",
               hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.StringBuffer", "length", hyp_type_builtin(arena, "int"));
    REG_CTOR("java.lang.StringBuffer", "StringBuffer");

    /* ── CharSequence ─────────────────────────────────────────── */
    REG_METHOD("java.lang.CharSequence", "length", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.CharSequence", "charAt", hyp_type_builtin(arena, "char"));
    REG_METHOD("java.lang.CharSequence", "subSequence",
               hyp_type_named(arena, "java.lang.CharSequence"));
    REG_METHOD("java.lang.CharSequence", "toString",
               hyp_type_named(arena, "java.lang.String"));

    /* ── Number + boxed types ─────────────────────────────────── */
    REG_METHOD("java.lang.Number", "intValue", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Number", "longValue", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Number", "doubleValue", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Number", "floatValue", hyp_type_builtin(arena, "float"));
    REG_METHOD("java.lang.Number", "shortValue", hyp_type_builtin(arena, "short"));
    REG_METHOD("java.lang.Number", "byteValue", hyp_type_builtin(arena, "byte"));

    REG_METHOD("java.lang.Integer", "intValue", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "parseInt", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "valueOf", hyp_type_named(arena, "java.lang.Integer"));
    REG_METHOD("java.lang.Integer", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Integer", "compare", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "compareTo", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "equals", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Integer", "hashCode", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "max", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "min", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "sum", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "bitCount", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Integer", "toBinaryString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Integer", "toHexString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Integer", "toOctalString", hyp_type_named(arena, "java.lang.String"));

    REG_METHOD("java.lang.Long", "longValue", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Long", "parseLong", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Long", "valueOf", hyp_type_named(arena, "java.lang.Long"));
    REG_METHOD("java.lang.Long", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Long", "compareTo", hyp_type_builtin(arena, "int"));

    REG_METHOD("java.lang.Double", "doubleValue", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Double", "parseDouble", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Double", "valueOf", hyp_type_named(arena, "java.lang.Double"));
    REG_METHOD("java.lang.Double", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Double", "isNaN", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Double", "isInfinite", hyp_type_builtin(arena, "boolean"));

    REG_METHOD("java.lang.Float", "floatValue", hyp_type_builtin(arena, "float"));
    REG_METHOD("java.lang.Float", "parseFloat", hyp_type_builtin(arena, "float"));
    REG_METHOD("java.lang.Float", "valueOf", hyp_type_named(arena, "java.lang.Float"));

    REG_METHOD("java.lang.Boolean", "booleanValue", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Boolean", "parseBoolean", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Boolean", "valueOf", hyp_type_named(arena, "java.lang.Boolean"));
    REG_METHOD("java.lang.Boolean", "toString", hyp_type_named(arena, "java.lang.String"));

    REG_METHOD("java.lang.Character", "charValue", hyp_type_builtin(arena, "char"));
    REG_METHOD("java.lang.Character", "isDigit", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Character", "isLetter", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Character", "isLetterOrDigit", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Character", "isWhitespace", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Character", "isUpperCase", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Character", "isLowerCase", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Character", "toUpperCase", hyp_type_builtin(arena, "char"));
    REG_METHOD("java.lang.Character", "toLowerCase", hyp_type_builtin(arena, "char"));
    REG_METHOD("java.lang.Character", "getNumericValue", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.Character", "valueOf", hyp_type_named(arena, "java.lang.Character"));

    REG_METHOD("java.lang.Byte", "byteValue", hyp_type_builtin(arena, "byte"));
    REG_METHOD("java.lang.Byte", "parseByte", hyp_type_builtin(arena, "byte"));
    REG_METHOD("java.lang.Byte", "valueOf", hyp_type_named(arena, "java.lang.Byte"));

    REG_METHOD("java.lang.Short", "shortValue", hyp_type_builtin(arena, "short"));
    REG_METHOD("java.lang.Short", "parseShort", hyp_type_builtin(arena, "short"));
    REG_METHOD("java.lang.Short", "valueOf", hyp_type_named(arena, "java.lang.Short"));

    /* ── Math ─────────────────────────────────────────────────── */
    REG_METHOD("java.lang.Math", "abs", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "min", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "max", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "sqrt", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "cbrt", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "pow", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "exp", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "log", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "log10", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "sin", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "cos", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "tan", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "asin", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "acos", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "atan", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "atan2", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "floor", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "ceil", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "round", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Math", "random", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "signum", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "hypot", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "floorDiv", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Math", "floorMod", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Math", "addExact", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Math", "subtractExact", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Math", "multiplyExact", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Math", "toRadians", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.lang.Math", "toDegrees", hyp_type_builtin(arena, "double"));

    /* ── System ───────────────────────────────────────────────── */
    REG_METHOD("java.lang.System", "currentTimeMillis", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.System", "nanoTime", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.System", "exit", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.System", "getenv", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.System", "getProperty", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.System", "setProperty", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.System", "lineSeparator", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.System", "arraycopy", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.System", "identityHashCode", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.lang.System", "gc", hyp_type_builtin(arena, "void"));

    /* ── Thread ───────────────────────────────────────────────── */
    REG_METHOD("java.lang.Thread", "start", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "run", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "join", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "interrupt", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "isAlive", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Thread", "sleep", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "currentThread", hyp_type_named(arena, "java.lang.Thread"));
    REG_METHOD("java.lang.Thread", "yield", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "getName", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Thread", "setName", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Thread", "getId", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.lang.Thread", "isInterrupted", hyp_type_builtin(arena, "boolean"));

    /* ── Class ────────────────────────────────────────────────── */
    REG_METHOD("java.lang.Class", "getName", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Class", "getSimpleName", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Class", "getCanonicalName", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Class", "isInterface", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Class", "isArray", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Class", "isAssignableFrom", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Class", "isInstance", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.lang.Class", "newInstance", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.lang.Class", "forName", hyp_type_named(arena, "java.lang.Class"));
    REG_METHOD("java.lang.Class", "getSuperclass", hyp_type_named(arena, "java.lang.Class"));
    REG_METHOD("java.lang.Class", "getInterfaces",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.Class")));

    /* ── Iterable / Iterator ──────────────────────────────────── */
    REG_METHOD("java.lang.Iterable", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_METHOD("java.lang.Iterable", "forEach", hyp_type_builtin(arena, "void"));

    /* ── Throwable methods ────────────────────────────────────── */
    REG_METHOD("java.lang.Throwable", "getMessage", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Throwable", "getLocalizedMessage",
               hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Throwable", "getCause", hyp_type_named(arena, "java.lang.Throwable"));
    REG_METHOD("java.lang.Throwable", "initCause",
               hyp_type_named(arena, "java.lang.Throwable"));
    REG_METHOD("java.lang.Throwable", "printStackTrace", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.lang.Throwable", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.lang.Throwable", "getStackTrace",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.StackTraceElement")));

    /* ── AutoCloseable ────────────────────────────────────────── */
    REG_METHOD("java.lang.AutoCloseable", "close", hyp_type_builtin(arena, "void"));

    /* ── Comparable ───────────────────────────────────────────── */
    REG_METHOD("java.lang.Comparable", "compareTo", hyp_type_builtin(arena, "int"));

    /* ── Runnable ─────────────────────────────────────────────── */
    REG_METHOD("java.lang.Runnable", "run", hyp_type_builtin(arena, "void"));

    /* ── java.util ────────────────────────────────────────────── */
    REG_TYPE("java.util.Collection", "Collection", true, parents_collection);
    REG_TYPE("java.util.List", "List", true, parents_list);
    REG_TYPE("java.util.Set", "Set", true, parents_set);
    REG_TYPE("java.util.Queue", "Queue", true, parents_queue);
    REG_TYPE("java.util.Deque", "Deque", true, parents_deque);
    REG_TYPE("java.util.Map", "Map", true, parents_map);
    REG_TYPE("java.util.Map.Entry", "Entry", true, parents_map_entry);
    REG_TYPE("java.util.Iterator", "Iterator", true, parents_iterator);
    REG_TYPE("java.util.ListIterator", "ListIterator", true, parents_iterator);
    REG_TYPE("java.util.Spliterator", "Spliterator", true, no_parents);
    REG_TYPE("java.util.Comparator", "Comparator", true, no_parents);

    REG_TYPE("java.util.ArrayList", "ArrayList", false, parents_arraylist);
    REG_TYPE("java.util.LinkedList", "LinkedList", false, parents_linkedlist);
    REG_TYPE("java.util.Vector", "Vector", false, parents_arraylist);
    REG_TYPE("java.util.Stack", "Stack", false, parents_arraylist);
    REG_TYPE("java.util.HashSet", "HashSet", false, parents_hashset);
    REG_TYPE("java.util.TreeSet", "TreeSet", false, parents_treeset);
    REG_TYPE("java.util.LinkedHashSet", "LinkedHashSet", false, parents_linkedhashset);
    REG_TYPE("java.util.HashMap", "HashMap", false, parents_hashmap);
    REG_TYPE("java.util.TreeMap", "TreeMap", false, parents_treemap);
    REG_TYPE("java.util.LinkedHashMap", "LinkedHashMap", false, parents_linkedhashmap);
    REG_TYPE("java.util.ArrayDeque", "ArrayDeque", false, parents_deque);
    REG_TYPE("java.util.PriorityQueue", "PriorityQueue", false, parents_queue);

    REG_TYPE("java.util.Optional", "Optional", false, parents_object);
    REG_TYPE("java.util.OptionalInt", "OptionalInt", false, parents_object);
    REG_TYPE("java.util.OptionalLong", "OptionalLong", false, parents_object);
    REG_TYPE("java.util.OptionalDouble", "OptionalDouble", false, parents_object);
    REG_TYPE("java.util.Date", "Date", false, parents_object);
    REG_TYPE("java.util.Calendar", "Calendar", false, parents_object);
    REG_TYPE("java.util.GregorianCalendar", "GregorianCalendar", false,
             parents_gregorian_calendar);
    REG_TYPE("java.util.TimeZone", "TimeZone", false, parents_object);
    REG_TYPE("java.util.Locale", "Locale", false, parents_object);
    REG_TYPE("java.util.UUID", "UUID", false, parents_object);
    REG_TYPE("java.util.Random", "Random", false, parents_object);
    REG_TYPE("java.util.Scanner", "Scanner", false, parents_object);
    REG_TYPE("java.util.Arrays", "Arrays", false, parents_object);
    REG_TYPE("java.util.Collections", "Collections", false, parents_object);
    REG_TYPE("java.util.Objects", "Objects", false, parents_object);
    REG_TYPE("java.util.Properties", "Properties", false, parents_hashmap);
    REG_TYPE("java.util.regex.Pattern", "Pattern", false, parents_object);
    REG_TYPE("java.util.regex.Matcher", "Matcher", false, parents_object);

    /* ── Collection methods ───────────────────────────────────── */
    REG_METHOD("java.util.Collection", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Collection", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "contains", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "containsAll", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_METHOD("java.util.Collection", "toArray",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.Object")));
    REG_METHOD("java.util.Collection", "add", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "addAll", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "remove", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "removeAll", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "retainAll", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Collection", "clear", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Collection", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.Collection", "parallelStream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.Collection", "forEach", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Collection", "removeIf", hyp_type_builtin(arena, "boolean"));

    /* ── List methods ─────────────────────────────────────────── */
    REG_METHOD("java.util.List", "get", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.List", "set", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.List", "add", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.List", "remove", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.List", "indexOf", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.List", "lastIndexOf", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.List", "subList", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.List", "of", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.List", "copyOf", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.List", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.List", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.List", "contains", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.List", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_METHOD("java.util.List", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.List", "forEach", hyp_type_builtin(arena, "void"));

    /* ── ArrayList ────────────────────────────────────────────── */
    REG_METHOD("java.util.ArrayList", "get", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.ArrayList", "set", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.ArrayList", "add", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.ArrayList", "remove", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.ArrayList", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.ArrayList", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.ArrayList", "indexOf", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.ArrayList", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_METHOD("java.util.ArrayList", "clear", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.ArrayList", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.ArrayList", "toArray",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.Object")));
    REG_METHOD("java.util.ArrayList", "subList", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.ArrayList", "trimToSize", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.ArrayList", "ensureCapacity", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.ArrayList", "forEach", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.ArrayList", "removeIf", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.List", "removeIf", hyp_type_builtin(arena, "boolean"));
    REG_CTOR("java.util.ArrayList", "ArrayList");

    REG_METHOD("java.util.LinkedList", "addFirst", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.LinkedList", "addLast", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.LinkedList", "removeFirst", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.LinkedList", "removeLast", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.LinkedList", "getFirst", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.LinkedList", "getLast", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.LinkedList", "peek", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.LinkedList", "poll", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.LinkedList", "offer", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.LinkedList", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.LinkedList", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_CTOR("java.util.LinkedList", "LinkedList");

    /* ── Set methods ──────────────────────────────────────────── */
    REG_METHOD("java.util.Set", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Set", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Set", "contains", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Set", "add", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Set", "remove", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Set", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_METHOD("java.util.Set", "of", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Set", "copyOf", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Set", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.Set", "forEach", hyp_type_builtin(arena, "void"));

    REG_METHOD("java.util.HashSet", "add", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashSet", "remove", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashSet", "contains", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashSet", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.HashSet", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashSet", "iterator", hyp_type_named(arena, "java.util.Iterator"));
    REG_METHOD("java.util.HashSet", "clear", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.HashSet", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_CTOR("java.util.HashSet", "HashSet");

    REG_METHOD("java.util.TreeSet", "first", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.TreeSet", "last", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.TreeSet", "headSet", hyp_type_named(arena, "java.util.SortedSet"));
    REG_METHOD("java.util.TreeSet", "tailSet", hyp_type_named(arena, "java.util.SortedSet"));
    REG_CTOR("java.util.TreeSet", "TreeSet");

    REG_CTOR("java.util.LinkedHashSet", "LinkedHashSet");

    /* ── Map methods ──────────────────────────────────────────── */
    REG_METHOD("java.util.Map", "get", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "put", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "remove", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "containsKey", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Map", "containsValue", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Map", "keySet", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Map", "values", hyp_type_named(arena, "java.util.Collection"));
    REG_METHOD("java.util.Map", "entrySet", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Map", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Map", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Map", "putAll", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Map", "clear", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Map", "getOrDefault", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "putIfAbsent", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "computeIfAbsent", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "compute", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "merge", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map", "of", hyp_type_named(arena, "java.util.Map"));
    REG_METHOD("java.util.Map", "copyOf", hyp_type_named(arena, "java.util.Map"));
    REG_METHOD("java.util.Map", "ofEntries", hyp_type_named(arena, "java.util.Map"));
    REG_METHOD("java.util.Map", "entry", hyp_type_named(arena, "java.util.Map.Entry"));
    REG_METHOD("java.util.Map", "forEach", hyp_type_builtin(arena, "void"));

    REG_METHOD("java.util.Map.Entry", "getKey", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map.Entry", "getValue", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Map.Entry", "setValue", hyp_type_named(arena, "java.lang.Object"));

    REG_METHOD("java.util.HashMap", "get", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.HashMap", "put", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.HashMap", "remove", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.HashMap", "containsKey", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashMap", "containsValue", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashMap", "size", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.HashMap", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.HashMap", "keySet", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.HashMap", "values", hyp_type_named(arena, "java.util.Collection"));
    REG_METHOD("java.util.HashMap", "entrySet", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.HashMap", "clear", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.HashMap", "getOrDefault", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.HashMap", "putIfAbsent", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.HashMap", "forEach", hyp_type_builtin(arena, "void"));
    REG_CTOR("java.util.HashMap", "HashMap");

    REG_METHOD("java.util.TreeMap", "firstKey", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.TreeMap", "lastKey", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.TreeMap", "headMap", hyp_type_named(arena, "java.util.SortedMap"));
    REG_METHOD("java.util.TreeMap", "tailMap", hyp_type_named(arena, "java.util.SortedMap"));
    REG_CTOR("java.util.TreeMap", "TreeMap");

    REG_CTOR("java.util.LinkedHashMap", "LinkedHashMap");

    /* ── Iterator methods ─────────────────────────────────────── */
    REG_METHOD("java.util.Iterator", "hasNext", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Iterator", "next", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Iterator", "remove", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Iterator", "forEachRemaining", hyp_type_builtin(arena, "void"));

    /* ── Optional ─────────────────────────────────────────────── */
    REG_METHOD("java.util.Optional", "get", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Optional", "isPresent", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Optional", "isEmpty", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Optional", "orElse", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Optional", "orElseGet", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Optional", "orElseThrow", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Optional", "ifPresent", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Optional", "ifPresentOrElse", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Optional", "map", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.Optional", "flatMap", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.Optional", "filter", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.Optional", "of", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.Optional", "ofNullable", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.Optional", "empty", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.Optional", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));

    /* ── Arrays / Collections / Objects helpers ───────────────── */
    REG_METHOD("java.util.Arrays", "asList", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.Arrays", "stream",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.Arrays", "sort", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Arrays", "binarySearch", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Arrays", "fill", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Arrays", "copyOf",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.Object")));
    REG_METHOD("java.util.Arrays", "copyOfRange",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.Object")));
    REG_METHOD("java.util.Arrays", "equals", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Arrays", "hashCode", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Arrays", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Arrays", "deepEquals", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Arrays", "deepToString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Arrays", "deepHashCode", hyp_type_builtin(arena, "int"));

    REG_METHOD("java.util.Collections", "sort", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Collections", "reverse", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Collections", "shuffle", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Collections", "min", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Collections", "max", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Collections", "emptyList", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.Collections", "emptySet", hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Collections", "emptyMap", hyp_type_named(arena, "java.util.Map"));
    REG_METHOD("java.util.Collections", "singletonList",
               hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.Collections", "singleton",
               hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Collections", "unmodifiableList",
               hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.Collections", "unmodifiableSet",
               hyp_type_named(arena, "java.util.Set"));
    REG_METHOD("java.util.Collections", "unmodifiableMap",
               hyp_type_named(arena, "java.util.Map"));
    REG_METHOD("java.util.Collections", "frequency", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Collections", "binarySearch", hyp_type_builtin(arena, "int"));

    REG_METHOD("java.util.Objects", "equals", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Objects", "hashCode", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Objects", "hash", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Objects", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Objects", "isNull", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Objects", "nonNull", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Objects", "requireNonNull", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.Objects", "requireNonNullElse",
               hyp_type_named(arena, "java.lang.Object"));

    /* ── UUID, Random, Scanner ────────────────────────────────── */
    REG_METHOD("java.util.UUID", "randomUUID", hyp_type_named(arena, "java.util.UUID"));
    REG_METHOD("java.util.UUID", "fromString", hyp_type_named(arena, "java.util.UUID"));
    REG_METHOD("java.util.UUID", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.UUID", "getMostSignificantBits", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.UUID", "getLeastSignificantBits", hyp_type_builtin(arena, "long"));
    REG_CTOR("java.util.UUID", "UUID");

    REG_METHOD("java.util.Random", "nextInt", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Random", "nextLong", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.Random", "nextDouble", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.util.Random", "nextFloat", hyp_type_builtin(arena, "float"));
    REG_METHOD("java.util.Random", "nextBoolean", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Random", "nextGaussian", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.util.Random", "setSeed", hyp_type_builtin(arena, "void"));
    REG_CTOR("java.util.Random", "Random");

    REG_METHOD("java.util.Scanner", "next", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Scanner", "nextLine", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Scanner", "nextInt", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Scanner", "nextLong", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.Scanner", "nextDouble", hyp_type_builtin(arena, "double"));
    REG_METHOD("java.util.Scanner", "hasNext", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Scanner", "hasNextLine", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Scanner", "hasNextInt", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Scanner", "close", hyp_type_builtin(arena, "void"));
    REG_CTOR("java.util.Scanner", "Scanner");

    /* ── Locale / Date / Calendar / TimeZone ──────────────────── */
    REG_METHOD("java.util.Locale", "getLanguage", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Locale", "getCountry", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Locale", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.Locale", "getDefault", hyp_type_named(arena, "java.util.Locale"));
    REG_CTOR("java.util.Locale", "Locale");

    REG_METHOD("java.util.Date", "getTime", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.Date", "setTime", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Date", "before", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Date", "after", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.Date", "compareTo", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Date", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_CTOR("java.util.Date", "Date");

    REG_METHOD("java.util.Calendar", "getInstance", hyp_type_named(arena, "java.util.Calendar"));
    REG_METHOD("java.util.Calendar", "getTime", hyp_type_named(arena, "java.util.Date"));
    REG_METHOD("java.util.Calendar", "set", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.Calendar", "get", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.Calendar", "add", hyp_type_builtin(arena, "void"));

    REG_METHOD("java.util.TimeZone", "getDefault", hyp_type_named(arena, "java.util.TimeZone"));
    REG_METHOD("java.util.TimeZone", "getID", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.TimeZone", "getTimeZone", hyp_type_named(arena, "java.util.TimeZone"));

    /* ── regex ────────────────────────────────────────────────── */
    REG_METHOD("java.util.regex.Pattern", "compile",
               hyp_type_named(arena, "java.util.regex.Pattern"));
    REG_METHOD("java.util.regex.Pattern", "matcher",
               hyp_type_named(arena, "java.util.regex.Matcher"));
    REG_METHOD("java.util.regex.Pattern", "matches", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.regex.Pattern", "split",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.String")));
    REG_METHOD("java.util.regex.Pattern", "pattern", hyp_type_named(arena, "java.lang.String"));

    REG_METHOD("java.util.regex.Matcher", "matches", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.regex.Matcher", "find", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.regex.Matcher", "group", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.regex.Matcher", "groupCount", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.regex.Matcher", "start", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.regex.Matcher", "end", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.regex.Matcher", "replaceAll", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.util.regex.Matcher", "replaceFirst",
               hyp_type_named(arena, "java.lang.String"));

    /* ── java.io ──────────────────────────────────────────────── */
    REG_TYPE("java.io.InputStream", "InputStream", false, parents_inputstream);
    REG_TYPE("java.io.OutputStream", "OutputStream", false, parents_outputstream);
    REG_TYPE("java.io.Reader", "Reader", false, parents_reader);
    REG_TYPE("java.io.Writer", "Writer", false, parents_writer);
    REG_TYPE("java.io.BufferedReader", "BufferedReader", false, parents_buffered_reader);
    REG_TYPE("java.io.BufferedWriter", "BufferedWriter", false, parents_buffered_writer);
    REG_TYPE("java.io.PrintStream", "PrintStream", false, parents_print_stream);
    REG_TYPE("java.io.PrintWriter", "PrintWriter", false, parents_print_writer);
    REG_TYPE("java.io.FileInputStream", "FileInputStream", false, parents_file_input_stream);
    REG_TYPE("java.io.FileOutputStream", "FileOutputStream", false, parents_file_output_stream);
    REG_TYPE("java.io.FileReader", "FileReader", false, parents_file_reader);
    REG_TYPE("java.io.FileWriter", "FileWriter", false, parents_file_writer);
    REG_TYPE("java.io.File", "File", false, parents_object);
    REG_TYPE("java.io.IOException", "IOException", false, parents_io_exception);
    REG_TYPE("java.io.FileNotFoundException", "FileNotFoundException", false,
             parents_file_not_found_exc);
    REG_TYPE("java.io.UncheckedIOException", "UncheckedIOException", false,
             parents_runtime_exc_chain);
    REG_TYPE("java.io.Serializable", "Serializable", true, no_parents);
    REG_TYPE("java.io.Closeable", "Closeable", true,
             parents_closeable);
    REG_TYPE("java.io.Flushable", "Flushable", true, no_parents);

    REG_METHOD("java.io.PrintStream", "println", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintStream", "print", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintStream", "printf", hyp_type_named(arena, "java.io.PrintStream"));
    REG_METHOD("java.io.PrintStream", "format", hyp_type_named(arena, "java.io.PrintStream"));
    REG_METHOD("java.io.PrintStream", "write", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintStream", "flush", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintStream", "close", hyp_type_builtin(arena, "void"));

    REG_METHOD("java.io.PrintWriter", "println", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintWriter", "print", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintWriter", "printf", hyp_type_named(arena, "java.io.PrintWriter"));
    REG_METHOD("java.io.PrintWriter", "flush", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.PrintWriter", "close", hyp_type_builtin(arena, "void"));
    REG_CTOR("java.io.PrintWriter", "PrintWriter");

    REG_METHOD("java.io.InputStream", "read", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.io.InputStream", "close", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.InputStream", "available", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.io.InputStream", "skip", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.io.InputStream", "readAllBytes",
               hyp_type_slice(arena, hyp_type_builtin(arena, "byte")));

    REG_METHOD("java.io.OutputStream", "write", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.OutputStream", "flush", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.OutputStream", "close", hyp_type_builtin(arena, "void"));

    REG_METHOD("java.io.Reader", "read", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.io.Reader", "close", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.Reader", "ready", hyp_type_builtin(arena, "boolean"));

    REG_METHOD("java.io.Writer", "write", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.Writer", "flush", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.Writer", "close", hyp_type_builtin(arena, "void"));

    REG_METHOD("java.io.BufferedReader", "readLine", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.io.BufferedReader", "lines",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.io.BufferedReader", "close", hyp_type_builtin(arena, "void"));
    REG_CTOR("java.io.BufferedReader", "BufferedReader");

    REG_METHOD("java.io.BufferedWriter", "write", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.BufferedWriter", "newLine", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.BufferedWriter", "flush", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.io.BufferedWriter", "close", hyp_type_builtin(arena, "void"));
    REG_CTOR("java.io.BufferedWriter", "BufferedWriter");

    REG_METHOD("java.io.File", "exists", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "isFile", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "isDirectory", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "canRead", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "canWrite", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "getName", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.io.File", "getPath", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.io.File", "getAbsolutePath", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.io.File", "getCanonicalPath", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.io.File", "getParent", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.io.File", "getParentFile", hyp_type_named(arena, "java.io.File"));
    REG_METHOD("java.io.File", "length", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.io.File", "lastModified", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.io.File", "mkdir", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "mkdirs", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "delete", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "renameTo", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.io.File", "list",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.String")));
    REG_METHOD("java.io.File", "listFiles",
               hyp_type_slice(arena, hyp_type_named(arena, "java.io.File")));
    REG_METHOD("java.io.File", "toPath", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.io.File", "toURI", hyp_type_named(arena, "java.net.URI"));
    REG_CTOR("java.io.File", "File");

    /* ── java.nio.file ───────────────────────────────────────── */
    REG_TYPE("java.nio.file.Path", "Path", true, no_parents);
    REG_TYPE("java.nio.file.Paths", "Paths", false, parents_object);
    REG_TYPE("java.nio.file.Files", "Files", false, parents_object);

    REG_METHOD("java.nio.file.Path", "getFileName", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "getParent", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "getRoot", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "resolve", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "resolveSibling",
               hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "relativize", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "normalize", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "toAbsolutePath",
               hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Path", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.nio.file.Path", "toFile", hyp_type_named(arena, "java.io.File"));
    REG_METHOD("java.nio.file.Path", "of", hyp_type_named(arena, "java.nio.file.Path"));

    REG_METHOD("java.nio.file.Paths", "get", hyp_type_named(arena, "java.nio.file.Path"));

    REG_METHOD("java.nio.file.Files", "exists", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.nio.file.Files", "isDirectory", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.nio.file.Files", "isRegularFile", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.nio.file.Files", "readString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.nio.file.Files", "writeString", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Files", "readAllLines", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.nio.file.Files", "readAllBytes",
               hyp_type_slice(arena, hyp_type_builtin(arena, "byte")));
    REG_METHOD("java.nio.file.Files", "lines",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.nio.file.Files", "list",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.nio.file.Files", "walk",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.nio.file.Files", "createDirectory",
               hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Files", "createDirectories",
               hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Files", "createFile",
               hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Files", "delete", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.nio.file.Files", "deleteIfExists", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.nio.file.Files", "copy", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Files", "move", hyp_type_named(arena, "java.nio.file.Path"));
    REG_METHOD("java.nio.file.Files", "size", hyp_type_builtin(arena, "long"));

    /* ── java.util.function (the 21 functional interfaces) ──── */
    REG_TYPE("java.util.function.Function", "Function", true, no_parents);
    REG_TYPE("java.util.function.BiFunction", "BiFunction", true, no_parents);
    REG_TYPE("java.util.function.Predicate", "Predicate", true, no_parents);
    REG_TYPE("java.util.function.BiPredicate", "BiPredicate", true, no_parents);
    REG_TYPE("java.util.function.Consumer", "Consumer", true, no_parents);
    REG_TYPE("java.util.function.BiConsumer", "BiConsumer", true, no_parents);
    REG_TYPE("java.util.function.Supplier", "Supplier", true, no_parents);
    REG_TYPE("java.util.function.UnaryOperator", "UnaryOperator", true,
             parents_unary_operator);
    REG_TYPE("java.util.function.BinaryOperator", "BinaryOperator", true,
             parents_binary_operator);
    REG_TYPE("java.util.function.IntFunction", "IntFunction", true, no_parents);
    REG_TYPE("java.util.function.LongFunction", "LongFunction", true, no_parents);
    REG_TYPE("java.util.function.DoubleFunction", "DoubleFunction", true, no_parents);
    REG_TYPE("java.util.function.IntPredicate", "IntPredicate", true, no_parents);
    REG_TYPE("java.util.function.LongPredicate", "LongPredicate", true, no_parents);
    REG_TYPE("java.util.function.DoublePredicate", "DoublePredicate", true, no_parents);
    REG_TYPE("java.util.function.IntConsumer", "IntConsumer", true, no_parents);
    REG_TYPE("java.util.function.LongConsumer", "LongConsumer", true, no_parents);
    REG_TYPE("java.util.function.DoubleConsumer", "DoubleConsumer", true, no_parents);
    REG_TYPE("java.util.function.IntSupplier", "IntSupplier", true, no_parents);
    REG_TYPE("java.util.function.LongSupplier", "LongSupplier", true, no_parents);
    REG_TYPE("java.util.function.DoubleSupplier", "DoubleSupplier", true, no_parents);
    REG_TYPE("java.util.function.BooleanSupplier", "BooleanSupplier", true, no_parents);
    REG_TYPE("java.util.function.ToIntFunction", "ToIntFunction", true, no_parents);
    REG_TYPE("java.util.function.ToLongFunction", "ToLongFunction", true, no_parents);
    REG_TYPE("java.util.function.ToDoubleFunction", "ToDoubleFunction", true, no_parents);

    REG_METHOD("java.util.function.Function", "apply", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.function.Function", "compose",
               hyp_type_named(arena, "java.util.function.Function"));
    REG_METHOD("java.util.function.Function", "andThen",
               hyp_type_named(arena, "java.util.function.Function"));
    REG_METHOD("java.util.function.Function", "identity",
               hyp_type_named(arena, "java.util.function.Function"));

    REG_METHOD("java.util.function.BiFunction", "apply",
               hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.function.BiFunction", "andThen",
               hyp_type_named(arena, "java.util.function.BiFunction"));

    REG_METHOD("java.util.function.Predicate", "test", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.function.Predicate", "and",
               hyp_type_named(arena, "java.util.function.Predicate"));
    REG_METHOD("java.util.function.Predicate", "or",
               hyp_type_named(arena, "java.util.function.Predicate"));
    REG_METHOD("java.util.function.Predicate", "negate",
               hyp_type_named(arena, "java.util.function.Predicate"));
    REG_METHOD("java.util.function.Predicate", "isEqual",
               hyp_type_named(arena, "java.util.function.Predicate"));

    REG_METHOD("java.util.function.Consumer", "accept", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.function.Consumer", "andThen",
               hyp_type_named(arena, "java.util.function.Consumer"));

    REG_METHOD("java.util.function.Supplier", "get", hyp_type_named(arena, "java.lang.Object"));

    REG_METHOD("java.util.function.UnaryOperator", "identity",
               hyp_type_named(arena, "java.util.function.UnaryOperator"));
    REG_METHOD("java.util.function.UnaryOperator", "apply",
               hyp_type_named(arena, "java.lang.Object"));

    /* ── java.util.stream ────────────────────────────────────── */
    REG_TYPE("java.util.stream.Stream", "Stream", true, no_parents);
    REG_TYPE("java.util.stream.IntStream", "IntStream", true, no_parents);
    REG_TYPE("java.util.stream.LongStream", "LongStream", true, no_parents);
    REG_TYPE("java.util.stream.DoubleStream", "DoubleStream", true, no_parents);
    REG_TYPE("java.util.stream.Collectors", "Collectors", false, parents_object);
    REG_TYPE("java.util.stream.Collector", "Collector", true, no_parents);

    REG_METHOD("java.util.stream.Stream", "filter",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "map",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "flatMap",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "mapToInt",
               hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.util.stream.Stream", "mapToLong",
               hyp_type_named(arena, "java.util.stream.LongStream"));
    REG_METHOD("java.util.stream.Stream", "mapToDouble",
               hyp_type_named(arena, "java.util.stream.DoubleStream"));
    REG_METHOD("java.util.stream.Stream", "sorted",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "distinct",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "limit",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "skip",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "peek",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "forEach", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.stream.Stream", "forEachOrdered", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.stream.Stream", "toArray",
               hyp_type_slice(arena, hyp_type_named(arena, "java.lang.Object")));
    REG_METHOD("java.util.stream.Stream", "toList", hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.stream.Stream", "reduce", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.stream.Stream", "collect", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.stream.Stream", "count", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.stream.Stream", "anyMatch", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.stream.Stream", "allMatch", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.stream.Stream", "noneMatch", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.stream.Stream", "findFirst",
               hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.stream.Stream", "findAny",
               hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.stream.Stream", "min", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.stream.Stream", "max", hyp_type_named(arena, "java.util.Optional"));
    REG_METHOD("java.util.stream.Stream", "of",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "empty",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "concat",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "iterate",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.Stream", "generate",
               hyp_type_named(arena, "java.util.stream.Stream"));

    REG_METHOD("java.util.stream.IntStream", "sum", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.stream.IntStream", "average",
               hyp_type_named(arena, "java.util.OptionalDouble"));
    REG_METHOD("java.util.stream.IntStream", "max",
               hyp_type_named(arena, "java.util.OptionalInt"));
    REG_METHOD("java.util.stream.IntStream", "min",
               hyp_type_named(arena, "java.util.OptionalInt"));
    REG_METHOD("java.util.stream.IntStream", "count", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.stream.IntStream", "boxed",
               hyp_type_named(arena, "java.util.stream.Stream"));
    REG_METHOD("java.util.stream.IntStream", "filter",
               hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.util.stream.IntStream", "map",
               hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.util.stream.IntStream", "range",
               hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.util.stream.IntStream", "rangeClosed",
               hyp_type_named(arena, "java.util.stream.IntStream"));
    REG_METHOD("java.util.stream.IntStream", "of",
               hyp_type_named(arena, "java.util.stream.IntStream"));

    REG_METHOD("java.util.stream.Collectors", "toList",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "toSet",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "toMap",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "joining",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "groupingBy",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "partitioningBy",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "counting",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "summingInt",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "averagingDouble",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "mapping",
               hyp_type_named(arena, "java.util.stream.Collector"));
    REG_METHOD("java.util.stream.Collectors", "reducing",
               hyp_type_named(arena, "java.util.stream.Collector"));

    /* ── java.util.concurrent ────────────────────────────────── */
    REG_TYPE("java.util.concurrent.ExecutorService", "ExecutorService", true, no_parents);
    REG_TYPE("java.util.concurrent.Executors", "Executors", false, parents_object);
    REG_TYPE("java.util.concurrent.Future", "Future", true, no_parents);
    REG_TYPE("java.util.concurrent.CompletableFuture", "CompletableFuture", false,
             parents_completable_future);
    REG_TYPE("java.util.concurrent.ConcurrentHashMap", "ConcurrentHashMap", false,
             parents_concurrent_hashmap);
    REG_TYPE("java.util.concurrent.ConcurrentMap", "ConcurrentMap", true, parents_map);
    REG_TYPE("java.util.concurrent.TimeUnit", "TimeUnit", false, parents_object);
    REG_TYPE("java.util.concurrent.atomic.AtomicInteger", "AtomicInteger", false, parents_object);
    REG_TYPE("java.util.concurrent.atomic.AtomicLong", "AtomicLong", false, parents_object);
    REG_TYPE("java.util.concurrent.atomic.AtomicBoolean", "AtomicBoolean", false, parents_object);
    REG_TYPE("java.util.concurrent.atomic.AtomicReference", "AtomicReference", false,
             parents_object);
    REG_TYPE("java.util.concurrent.locks.Lock", "Lock", true, no_parents);
    REG_TYPE("java.util.concurrent.locks.ReentrantLock", "ReentrantLock", false,
             parents_reentrant_lock);
    REG_TYPE("java.util.concurrent.locks.ReadWriteLock", "ReadWriteLock", true, no_parents);

    REG_METHOD("java.util.concurrent.ExecutorService", "submit",
               hyp_type_named(arena, "java.util.concurrent.Future"));
    REG_METHOD("java.util.concurrent.ExecutorService", "execute", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.concurrent.ExecutorService", "shutdown", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.concurrent.ExecutorService", "shutdownNow",
               hyp_type_named(arena, "java.util.List"));
    REG_METHOD("java.util.concurrent.ExecutorService", "awaitTermination",
               hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.concurrent.ExecutorService", "isShutdown",
               hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.concurrent.ExecutorService", "isTerminated",
               hyp_type_builtin(arena, "boolean"));

    REG_METHOD("java.util.concurrent.Executors", "newFixedThreadPool",
               hyp_type_named(arena, "java.util.concurrent.ExecutorService"));
    REG_METHOD("java.util.concurrent.Executors", "newSingleThreadExecutor",
               hyp_type_named(arena, "java.util.concurrent.ExecutorService"));
    REG_METHOD("java.util.concurrent.Executors", "newCachedThreadPool",
               hyp_type_named(arena, "java.util.concurrent.ExecutorService"));
    REG_METHOD("java.util.concurrent.Executors", "newScheduledThreadPool",
               hyp_type_named(arena, "java.util.concurrent.ExecutorService"));

    REG_METHOD("java.util.concurrent.Future", "get", hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.concurrent.Future", "isDone", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.concurrent.Future", "cancel", hyp_type_builtin(arena, "boolean"));

    REG_METHOD("java.util.concurrent.CompletableFuture", "thenApply",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "thenAccept",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "thenCompose",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "thenCombine",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "exceptionally",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "join",
               hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "supplyAsync",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "runAsync",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "completedFuture",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "allOf",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));
    REG_METHOD("java.util.concurrent.CompletableFuture", "anyOf",
               hyp_type_named(arena, "java.util.concurrent.CompletableFuture"));

    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "get", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "set", hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "incrementAndGet",
               hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "decrementAndGet",
               hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "getAndIncrement",
               hyp_type_builtin(arena, "int"));
    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "compareAndSet",
               hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.util.concurrent.atomic.AtomicInteger", "addAndGet",
               hyp_type_builtin(arena, "int"));
    REG_CTOR("java.util.concurrent.atomic.AtomicInteger", "AtomicInteger");

    REG_METHOD("java.util.concurrent.atomic.AtomicLong", "get",
               hyp_type_builtin(arena, "long"));
    REG_METHOD("java.util.concurrent.atomic.AtomicLong", "incrementAndGet",
               hyp_type_builtin(arena, "long"));
    REG_CTOR("java.util.concurrent.atomic.AtomicLong", "AtomicLong");

    REG_METHOD("java.util.concurrent.atomic.AtomicReference", "get",
               hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.concurrent.atomic.AtomicReference", "set",
               hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.concurrent.atomic.AtomicReference", "compareAndSet",
               hyp_type_builtin(arena, "boolean"));
    REG_CTOR("java.util.concurrent.atomic.AtomicReference", "AtomicReference");

    REG_METHOD("java.util.concurrent.locks.ReentrantLock", "lock",
               hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.concurrent.locks.ReentrantLock", "unlock",
               hyp_type_builtin(arena, "void"));
    REG_METHOD("java.util.concurrent.locks.ReentrantLock", "tryLock",
               hyp_type_builtin(arena, "boolean"));
    REG_CTOR("java.util.concurrent.locks.ReentrantLock", "ReentrantLock");

    REG_METHOD("java.util.concurrent.ConcurrentHashMap", "put",
               hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.concurrent.ConcurrentHashMap", "get",
               hyp_type_named(arena, "java.lang.Object"));
    REG_METHOD("java.util.concurrent.ConcurrentHashMap", "putIfAbsent",
               hyp_type_named(arena, "java.lang.Object"));
    REG_CTOR("java.util.concurrent.ConcurrentHashMap", "ConcurrentHashMap");

    /* ── java.time ───────────────────────────────────────────── */
    REG_TYPE("java.time.LocalDate", "LocalDate", false, parents_object);
    REG_TYPE("java.time.LocalTime", "LocalTime", false, parents_object);
    REG_TYPE("java.time.LocalDateTime", "LocalDateTime", false, parents_object);
    REG_TYPE("java.time.ZonedDateTime", "ZonedDateTime", false, parents_object);
    REG_TYPE("java.time.OffsetDateTime", "OffsetDateTime", false, parents_object);
    REG_TYPE("java.time.Instant", "Instant", false, parents_object);
    REG_TYPE("java.time.Duration", "Duration", false, parents_object);
    REG_TYPE("java.time.Period", "Period", false, parents_object);
    REG_TYPE("java.time.ZoneId", "ZoneId", false, parents_object);
    REG_TYPE("java.time.format.DateTimeFormatter", "DateTimeFormatter", false, parents_object);

    REG_METHOD("java.time.LocalDate", "now", hyp_type_named(arena, "java.time.LocalDate"));
    REG_METHOD("java.time.LocalDate", "of", hyp_type_named(arena, "java.time.LocalDate"));
    REG_METHOD("java.time.LocalDate", "parse", hyp_type_named(arena, "java.time.LocalDate"));
    REG_METHOD("java.time.LocalDate", "plusDays", hyp_type_named(arena, "java.time.LocalDate"));
    REG_METHOD("java.time.LocalDate", "minusDays", hyp_type_named(arena, "java.time.LocalDate"));
    REG_METHOD("java.time.LocalDate", "getYear", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.time.LocalDate", "getMonth", hyp_type_named(arena, "java.time.Month"));
    REG_METHOD("java.time.LocalDate", "getDayOfMonth", hyp_type_builtin(arena, "int"));
    REG_METHOD("java.time.LocalDate", "getDayOfWeek",
               hyp_type_named(arena, "java.time.DayOfWeek"));
    REG_METHOD("java.time.LocalDate", "isAfter", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.time.LocalDate", "isBefore", hyp_type_builtin(arena, "boolean"));
    REG_METHOD("java.time.LocalDate", "format", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.time.LocalDate", "toString", hyp_type_named(arena, "java.lang.String"));

    REG_METHOD("java.time.LocalDateTime", "now",
               hyp_type_named(arena, "java.time.LocalDateTime"));
    REG_METHOD("java.time.LocalDateTime", "of",
               hyp_type_named(arena, "java.time.LocalDateTime"));
    REG_METHOD("java.time.LocalDateTime", "parse",
               hyp_type_named(arena, "java.time.LocalDateTime"));
    REG_METHOD("java.time.LocalDateTime", "plusHours",
               hyp_type_named(arena, "java.time.LocalDateTime"));
    REG_METHOD("java.time.LocalDateTime", "minusHours",
               hyp_type_named(arena, "java.time.LocalDateTime"));
    REG_METHOD("java.time.LocalDateTime", "format", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.time.LocalDateTime", "toString", hyp_type_named(arena, "java.lang.String"));

    REG_METHOD("java.time.Instant", "now", hyp_type_named(arena, "java.time.Instant"));
    REG_METHOD("java.time.Instant", "ofEpochMilli", hyp_type_named(arena, "java.time.Instant"));
    REG_METHOD("java.time.Instant", "ofEpochSecond", hyp_type_named(arena, "java.time.Instant"));
    REG_METHOD("java.time.Instant", "toEpochMilli", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.time.Instant", "getEpochSecond", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.time.Instant", "plus", hyp_type_named(arena, "java.time.Instant"));
    REG_METHOD("java.time.Instant", "minus", hyp_type_named(arena, "java.time.Instant"));

    REG_METHOD("java.time.Duration", "ofSeconds", hyp_type_named(arena, "java.time.Duration"));
    REG_METHOD("java.time.Duration", "ofMillis", hyp_type_named(arena, "java.time.Duration"));
    REG_METHOD("java.time.Duration", "ofMinutes", hyp_type_named(arena, "java.time.Duration"));
    REG_METHOD("java.time.Duration", "ofHours", hyp_type_named(arena, "java.time.Duration"));
    REG_METHOD("java.time.Duration", "ofDays", hyp_type_named(arena, "java.time.Duration"));
    REG_METHOD("java.time.Duration", "between", hyp_type_named(arena, "java.time.Duration"));
    REG_METHOD("java.time.Duration", "toMillis", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.time.Duration", "toSeconds", hyp_type_builtin(arena, "long"));
    REG_METHOD("java.time.Duration", "toMinutes", hyp_type_builtin(arena, "long"));

    REG_METHOD("java.time.ZoneId", "of", hyp_type_named(arena, "java.time.ZoneId"));
    REG_METHOD("java.time.ZoneId", "systemDefault", hyp_type_named(arena, "java.time.ZoneId"));
    REG_METHOD("java.time.ZoneId", "getId", hyp_type_named(arena, "java.lang.String"));

    REG_METHOD("java.time.format.DateTimeFormatter", "ofPattern",
               hyp_type_named(arena, "java.time.format.DateTimeFormatter"));
    REG_METHOD("java.time.format.DateTimeFormatter", "format",
               hyp_type_named(arena, "java.lang.String"));

    /* ── java.net (minimal) ──────────────────────────────────── */
    REG_TYPE("java.net.URI", "URI", false, parents_object);
    REG_TYPE("java.net.URL", "URL", false, parents_object);
    REG_METHOD("java.net.URI", "create", hyp_type_named(arena, "java.net.URI"));
    REG_METHOD("java.net.URI", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_METHOD("java.net.URI", "toURL", hyp_type_named(arena, "java.net.URL"));
    REG_METHOD("java.net.URL", "openStream", hyp_type_named(arena, "java.io.InputStream"));
    REG_METHOD("java.net.URL", "toString", hyp_type_named(arena, "java.lang.String"));
    REG_CTOR("java.net.URL", "URL");
    REG_CTOR("java.net.URI", "URI");
}
