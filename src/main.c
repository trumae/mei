#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "version.h"
#include "cmd/run.h"
#include "cmd/new.h"
#include "cmd/status.h"
#include "cmd/agents.h"
#include "cmd/log.h"
#include "core/vcs_backend.h"

static void print_usage(const char *prog) {
    fprintf(stderr,
            "MEI %s — Multi-Agent Environment Integrator\n\n"
            "Usage:\n"
            "  %s fossil run    <repo.fossil> [--clean]   Start the orchestrator TUI\n"
            "  %s fossil new    <name> [--dir <path>]     Create a new MEI Fossil repository\n"
            "  %s fossil status <repo.fossil>             Show ticket queue status\n"
            "  %s fossil agents <repo.fossil>             List configured agents\n"
            "  %s github run    <owner/repo>              Start the orchestrator TUI (GitHub)\n"
            "  %s github status <owner/repo>              Show ticket queue status (GitHub)\n"
            "  %s github agents <owner/repo>              List configured agents (GitHub)\n"
            "  %s log    [-f|--follow] [-n <lines>]       View the system log\n"
            "\nLegacy (backward-compatible):\n"
            "  %s run    <repo.fossil> [--clean]\n"
            "  %s status <repo.fossil>\n"
            "  %s agents <repo.fossil>\n",
            MEI_VERSION_FULL,
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

// Resolve repo, set up g_backend, then dispatch subcommand.
// argc/argv: argv[0]=sub, argv[1]=repo [, extra flags ...]
static int dispatch_backend(VCSType type, const char *sub, int argc, char *argv[]) {
    const char *repo = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { repo = argv[i]; break; }
    }

    if (strcmp(sub, "new") != 0) {
        // new doesn't need a pre-existing repo
        if (!repo) {
            fprintf(stderr, "Error: missing repository argument for '%s'.\n", sub);
            return 1;
        }
        if (type == VCS_FOSSIL) {
            char abs_repo[4096];
            if (!realpath(repo, abs_repo)) {
                fprintf(stderr, "Error: could not resolve '%s'\n", repo);
                return 1;
            }
            if (!vcs_backend_create(VCS_FOSSIL, abs_repo)) return 1;
        } else {
            if (!vcs_backend_create(type, repo)) return 1;
        }
    }

    if (strcmp(sub, "run")    == 0) return cmd_run   (argc, argv);
    if (strcmp(sub, "new")    == 0) return cmd_new   (argc, argv);
    if (strcmp(sub, "status") == 0) return cmd_status(argc, argv);
    if (strcmp(sub, "agents") == 0) return cmd_agents(argc, argv);

    fprintf(stderr, "Unknown subcommand: '%s'\n\n", sub);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *first = argv[1];

    // New two-level format: mei fossil <sub> <repo> | mei github <sub> <repo>
    if (strcmp(first, "fossil") == 0 || strcmp(first, "github") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        VCSType type = (strcmp(first, "github") == 0) ? VCS_GITHUB : VCS_FOSSIL;
        const char *sub = argv[2];
        return dispatch_backend(type, sub, argc - 2, argv + 2);
    }

    // Legacy single-level format: mei run <repo> | mei status <repo> | ...
    if (strcmp(first, "log")    == 0) return cmd_log   (argc - 1, argv + 1);
    if (strcmp(first, "run")    == 0) return cmd_run   (argc - 1, argv + 1);
    if (strcmp(first, "new")    == 0) return cmd_new   (argc - 1, argv + 1);
    if (strcmp(first, "status") == 0) return cmd_status(argc - 1, argv + 1);
    if (strcmp(first, "agents") == 0) return cmd_agents(argc - 1, argv + 1);

    fprintf(stderr, "Unknown subcommand: '%s'\n\n", first);
    print_usage(argv[0]);
    return 1;
}
