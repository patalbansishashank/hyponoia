#ifndef HYP_GIT_CONTEXT_H
#define HYP_GIT_CONTEXT_H

#include <stdbool.h>

typedef struct {
    bool is_git;
    bool is_worktree;
    bool is_detached;
    bool root_exists;
    char *input_path;
    char *worktree_root;
    char *git_dir;
    char *git_common_dir;
    char *canonical_root;
    char *branch;
    char *branch_slug;
    char *head_sha;
    char *base_sha;
} hyp_git_context_t;

int hyp_git_context_resolve(const char *path, hyp_git_context_t *out);
void hyp_git_context_free(hyp_git_context_t *ctx);
char *hyp_git_context_branch_qn(const char *project_name, const hyp_git_context_t *ctx);
int hyp_git_context_props_json(const hyp_git_context_t *ctx, char *buf, int buf_size);

#endif
