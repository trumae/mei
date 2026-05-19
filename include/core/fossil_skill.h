#ifndef FOSSIL_SKILL_H
#define FOSSIL_SKILL_H

#include "core/vcs_backend.h"

// Create and return a Fossil VCSBackend instance.
// Initialises global_repo_path and populates the vtable with Fossil implementations.
VCSBackend *fossil_backend_create(const char *repo_path);

// Initialise the fossil repository file (used by cmd_new before a backend exists).
bool fossil_init(const char *repo_path);

#endif // FOSSIL_SKILL_H
