#ifndef HYP_AC_H
#define HYP_AC_H

#include <stdint.h>

// Forward declaration — full struct in ac.c
typedef struct HYPAutomaton HYPAutomaton;

// Input for batch LZ4 scanning.
typedef struct {
    const char *data;
    int compressed_len;
    int original_len;
} HYPLz4Entry;

// Output for batch LZ4 scanning.
typedef struct {
    int file_index;
    uint64_t bitmask;
} HYPLz4Match;

// Output for batch name scanning.
typedef struct {
    int name_index;
    int pattern_id;
} HYPMatchResult;

// Build an Aho-Corasick automaton from patterns.
HYPAutomaton *hyp_ac_build(const char **patterns, const int *lengths, int count,
                           const uint8_t *alpha_map, int alpha_size);
void hyp_ac_free(HYPAutomaton *ac);

// Single-text scanning (returns bitmask of matched pattern IDs).
uint64_t hyp_ac_scan_bitmask(const HYPAutomaton *ac, const char *text, int text_len);

// LZ4-compressed scanning.
uint64_t hyp_ac_scan_lz4_bitmask(const HYPAutomaton *ac, const char *compressed, int compressed_len,
                                 int original_len);
int hyp_ac_scan_lz4_batch(const HYPAutomaton *ac, const HYPLz4Entry *entries, int num_entries,
                          HYPLz4Match *out_matches, int max_matches);

// Batch name scanning.
int hyp_ac_scan_batch(const HYPAutomaton *ac, const char *names_buf, const int *name_offsets,
                      const int *name_lengths, int num_names, HYPMatchResult *out_matches,
                      int max_matches);

// Introspection.
int hyp_ac_num_states(const HYPAutomaton *ac);
int hyp_ac_num_patterns(const HYPAutomaton *ac);
int hyp_ac_table_bytes(const HYPAutomaton *ac);

#endif // HYP_AC_H
