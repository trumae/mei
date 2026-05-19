#ifndef GITHUB_SKILL_H
#define GITHUB_SKILL_H

#include "core/vcs_backend.h"

// Factory: allocates and returns a VCSBackend* wired to the GitHub implementation.
// owner_repo: "owner/repo" string (e.g. "trumae/valente").
VCSBackend *github_backend_create(const char *owner_repo);

#endif // GITHUB_SKILL_H
