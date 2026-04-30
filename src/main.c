#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ncurses.h>
#include "ui.h"
#include "version.h"
#include "agent.h"
#include "core/orchestrator.h"
#include "core/fossil_skill.h"

// Global state for signal handler
static Agent *g_agents = NULL;
static int g_agent_count = 0;
static int g_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <path_to_fossil_repo.fossil> [--clean]\n", argv[0]);
        return 1;
    }

    const char *repo_arg = NULL;
    int clean_workspaces = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--clean") == 0) {
            clean_workspaces = 1;
        } else if (repo_arg == NULL) {
            repo_arg = argv[i];
        }
    }

    if (!repo_arg) {
        printf("Error: Missing fossil repository path.\n");
        return 1;
    }

    char abs_repo_path[4096];
    if (realpath(repo_arg, abs_repo_path) == NULL) {
        printf("Error: Could not resolve path to %s\n", repo_arg);
        return 1;
    }

    if (clean_workspaces) {
        printf("Cleaning workspaces in /tmp/workspaces/...\n");
        system("rm -rf /tmp/workspaces");
    }

    const char *repo_path = abs_repo_path;
    fossil_set_repo_path(repo_path);

    Agent agents[MAX_AGENTS];
    g_agents = agents;
    int agent_count = 0;

    init_ui();
    signal(SIGINT, handle_sigint);

    char init_msg[512];
    snprintf(init_msg, sizeof(init_msg), "System initializing... Target Repo: %s", repo_path);
    log_message(init_msg);
    
    orchestrator_init(agents, &agent_count);
    g_agent_count = agent_count;
    
    log_message("Orchestrator online. Waiting for heartbeat...");

    int selected_agent = 0;
    // Set non-blocking input for the heartbeat loop matching TICK_INTERVAL_MS
    timeout(TICK_INTERVAL_MS);

    // Wall-clock guard: even if getch() returns ERR immediately (no proper
    // terminal / stdin redirected), the tick runs at most once per TICK_INTERVAL_MS.
    struct timespec last_tick = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (g_running) {
        draw_main_screen(agents, agent_count, selected_agent);

        int ch = getch();
        switch (ch) {
            case 'q':
            case 'Q':
                g_running = 0;
                break;
            case KEY_UP:
                if (selected_agent > 0) selected_agent--;
                break;
            case KEY_DOWN:
                if (selected_agent < agent_count - 1) selected_agent++;
                break;
            case 'a':
            case 'A':
                if (agent_count > 0) {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "tmux select-window -t mei:\"%s\"; tmux attach -t mei", agents[selected_agent].name);

                    // Warn: the orchestrator tick loop is blocked for the entire duration
                    // of the attach.  No PULSE delivery or ticket routing will happen until
                    // the user detaches (Ctrl+B, D).
                    char attach_log[256];
                    snprintf(attach_log, sizeof(attach_log),
                             "[!] Ticks PAUSED — attached to %s. Detach with Ctrl+B, D to resume.",
                             agents[selected_agent].name);
                    log_message(attach_log);

                    def_prog_mode();
                    endwin();
                    system(cmd);
                    reset_prog_mode();
                    refresh();

                    char log[256];
                    snprintf(log, sizeof(log), "[ok] Detached from %s — ticks resuming.", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case 'p':
            case 'P':
                if (agent_count > 0) {
                    agents[selected_agent].state = AGENT_STATE_PAUSED;
                    char log[256];
                    snprintf(log, sizeof(log), "Paused agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case 'r':
            case 'R':
                if (agent_count > 0) {
                    agents[selected_agent].state = AGENT_STATE_IN_PROGRESS;
                    char log[256];
                    snprintf(log, sizeof(log), "Resumed agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case 'k':
            case 'K':
                if (agent_count > 0) {
                    agents[selected_agent].state = AGENT_STATE_OFFLINE;
                    char log[256];
                    snprintf(log, sizeof(log), "Killed agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case ERR: {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec  - last_tick.tv_sec)  * 1000
                                + (now.tv_nsec - last_tick.tv_nsec) / 1000000;
                if (elapsed_ms >= TICK_INTERVAL_MS) {
                    last_tick = now;
                    orchestrator_tick(agents, agent_count);
                }
                break;
            }
        }
    }

    log_message("Shutting down orchestrator...");
    orchestrator_shutdown(agents, agent_count);

    destroy_ui();
    return 0;
}

