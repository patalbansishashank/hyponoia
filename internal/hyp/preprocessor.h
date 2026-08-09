#ifndef HYP_PREPROCESSOR_H
#define HYP_PREPROCESSOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *source;
    uint32_t *original_line_by_expanded_line; // 1-based; 0 means directive/unmapped.
    uint8_t *belongs_to_main_file;            // 1-based; true only for the original input file.
    int expanded_line_count;
} HYPPreprocessedSource;

// Preprocess C/C++ source: expand macros, evaluate #ifdef, resolve #include.
// Returns malloc-allocated expanded source, or NULL if no expansion needed/on failure.
// extra_defines: NULL-terminated array of "NAME=VALUE" strings (can be NULL).
// include_paths: NULL-terminated array of directory paths for #include resolution (can be NULL).
// The returned string must be freed with hyp_preprocess_free().
char *hyp_preprocess(const char *source, int source_len, const char *filename,
                     const char **extra_defines, const char **include_paths, int cpp_mode);

// Preprocess and return source plus expanded-line -> original-line ownership map.
// Returns NULL if no expansion is needed or preprocessing fails.
// Free with hyp_preprocessed_source_free().
HYPPreprocessedSource *hyp_preprocess_with_map(const char *source, int source_len,
                                               const char *filename, const char **extra_defines,
                                               const char **include_paths, int cpp_mode);

// Free preprocessed source returned by hyp_preprocess.
void hyp_preprocess_free(char *expanded);

// Free preprocessed source and line maps returned by hyp_preprocess_with_map.
void hyp_preprocessed_source_free(HYPPreprocessedSource *pp);

#ifdef __cplusplus
}
#endif

#endif // HYP_PREPROCESSOR_H
