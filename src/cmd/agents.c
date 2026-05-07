#include "cmd/agents.h"
#include "core/fossil_skill.h"
#include "core/agent_mgr.h"
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD  "\033[1m"
#define ANSI_DIM   "\033[2m"
#define ANSI_CYAN  "\033[36m"

int cmd_agents(int argc, char *argv[]) {
    const char *repo_arg = (argc > 1) ? argv[1] : NULL;
    if (!repo_arg) {
        fprintf(stderr, "Usage: mei agents <repo.fossil>\n");
        return 1;
    }

    char abs_path[4096];
    if (!realpath(repo_arg, abs_path)) {
        fprintf(stderr, "Error: could not resolve '%s'\n", repo_arg);
        return 1;
    }
    fossil_set_repo_path(abs_path);

    Agent agents[MAX_AGENTS];
    int count = agent_mgr_load_all(agents);

    if (count == 0) {
        printf("No agents found. Add .md files to /.agents/ in the repository trunk.\n");
        return 0;
    }

    printf("\n  %s%-20s  %-12s  %-12s  %s%s\n",
           ANSI_BOLD, "NAME", "ROLE", "CLI", "HASH", ANSI_RESET);
    printf("  %-20s  %-12s  %-12s  %s\n",
           "--------------------", "------------", "------------",
           "----------------------------------------");

    for (int i = 0; i < count; i++) {
        // Colorize role
        const char *col = ANSI_RESET;
        if (strcmp(agents[i].role, "planner")  == 0) col = ANSI_CYAN;
        if (strcmp(agents[i].role, "reviewer") == 0) col = "\033[35m";  // magenta

        printf("  %-20s  %s%-12s%s  %-12s  %s%s%s\n",
               agents[i].name,
               col, agents[i].role, ANSI_RESET,
               agents[i].cli,
               ANSI_DIM, agents[i].hash, ANSI_RESET);
    }
    printf("\n");
    return 0;
}
