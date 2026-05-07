#include <stdio.h>
#include <string.h>
#include "version.h"
#include "cmd/run.h"
#include "cmd/new.h"
#include "cmd/status.h"
#include "cmd/agents.h"
#include "cmd/log.h"

static void print_usage(const char *prog) {
    fprintf(stderr,
            "MEI %s — Multi-Agent Environment Integrator\n\n"
            "Usage:\n"
            "  %s run    <repo.fossil> [--clean]      Start the orchestrator TUI\n"
            "  %s new    <name> [--dir <path>]        Create a new MEI repository\n"
            "  %s status <repo.fossil>                Show ticket queue status\n"
            "  %s agents <repo.fossil>                List configured agents\n"
            "  %s log    [-f|--follow] [-n <lines>]   View the system log\n",
            MEI_VERSION_FULL,
            prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "run")    == 0) return cmd_run   (argc - 1, argv + 1);
    if (strcmp(sub, "new")    == 0) return cmd_new   (argc - 1, argv + 1);
    if (strcmp(sub, "status") == 0) return cmd_status(argc - 1, argv + 1);
    if (strcmp(sub, "agents") == 0) return cmd_agents(argc - 1, argv + 1);
    if (strcmp(sub, "log")    == 0) return cmd_log   (argc - 1, argv + 1);

    fprintf(stderr, "Unknown subcommand: '%s'\n\n", sub);
    print_usage(argv[0]);
    return 1;
}
