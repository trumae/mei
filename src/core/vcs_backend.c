#include "core/vcs_backend.h"
#include "core/fossil_skill.h"
#include "core/github_skill.h"
#include <stdio.h>
#include <stdlib.h>

VCSBackend *g_backend = NULL;

VCSBackend *vcs_backend_create(VCSType type, const char *repo_id) {
    VCSBackend *b = NULL;
    switch (type) {
        case VCS_FOSSIL:
            b = fossil_backend_create(repo_id);
            break;
        case VCS_GITHUB:
            b = github_backend_create(repo_id);
            break;
    }
    if (b) g_backend = b;
    return b;
}

void vcs_backend_destroy(VCSBackend *b) {
    free(b);
}
