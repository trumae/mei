#include "cmd/status.h"
#include "core/fossil_skill.h"
#include "core/agent_mgr.h"
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ANSI color helpers
#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_MAGENTA "\033[35m"

static const char *status_color(const char *status) {
    if (strcasecmp(status, "Blocked")     == 0) return ANSI_RED;
    if (strcasecmp(status, "In Progress") == 0) return ANSI_GREEN;
    if (strcasecmp(status, "Review")      == 0) return ANSI_MAGENTA;
    if (strcasecmp(status, "Rework")      == 0) return ANSI_YELLOW;
    if (strcasecmp(status, "Open")        == 0) return ANSI_YELLOW;
    if (strcasecmp(status, "Planned")     == 0) return ANSI_CYAN;
    if (strcasecmp(status, "Done")        == 0) return ANSI_DIM;
    return "";
}

// Groups in display order (most urgent first)
static const char *GROUPS[] = {
    "Blocked", "In Progress", "Review", "Rework", "Open", "Planned", "Done", NULL
};

int cmd_status(int argc, char *argv[]) {
    const char *repo_arg = (argc > 1) ? argv[1] : NULL;
    if (!repo_arg) {
        fprintf(stderr, "Usage: mei status <repo.fossil>\n");
        return 1;
    }

    char abs_path[4096];
    if (!realpath(repo_arg, abs_path)) {
        fprintf(stderr, "Error: could not resolve '%s'\n", repo_arg);
        return 1;
    }
    fossil_set_repo_path(abs_path);

    // Load agents for hash → name resolution
    Agent agents[MAX_AGENTS];
    int agent_count = agent_mgr_load_all(agents);

    FossilTicket tickets[200];
    int tkt_count = fossil_ticket_list_parsed(tickets, 200);

    if (tkt_count == 0) {
        printf("No tickets found in %s\n", abs_path);
        return 0;
    }

    printf("\n  %s%s%s\n\n", ANSI_BOLD, abs_path, ANSI_RESET);

    for (int g = 0; GROUPS[g]; g++) {
        // Count tickets in this group
        int count = 0;
        for (int t = 0; t < tkt_count; t++)
            if (strcasecmp(tickets[t].status, GROUPS[g]) == 0) count++;
        if (count == 0) continue;

        // Collapse Done to a single summary line
        if (strcasecmp(GROUPS[g], "Done") == 0) {
            printf("  %s%-13s%s (%d)\n\n", ANSI_DIM, "Done", ANSI_RESET, count);
            continue;
        }

        const char *col = status_color(GROUPS[g]);
        printf("  %s%s%-13s%s(%d)\n", ANSI_BOLD, col, GROUPS[g], ANSI_RESET, count);

        for (int t = 0; t < tkt_count; t++) {
            if (strcasecmp(tickets[t].status, GROUPS[g]) != 0) continue;

            // Resolve assignee hash → agent name
            char agent_label[64] = "";
            for (int a = 0; a < agent_count; a++) {
                if (strcmp(tickets[t].assignee, agents[a].hash) == 0 ||
                    strcmp(tickets[t].assignee, agents[a].name) == 0) {
                    snprintf(agent_label, sizeof(agent_label),
                             "[%s]", agents[a].name);
                    break;
                }
            }

            printf("    %s%.8s%s  %-40.40s  %s\n",
                   ANSI_DIM, tickets[t].tkt_uuid, ANSI_RESET,
                   tickets[t].title,
                   agent_label);
        }
        printf("\n");
    }

    return 0;
}
