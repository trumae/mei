#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ncurses.h>
#include "ui.h"
#include "agent.h"
#include "core/orchestrator.h"
#include "core/fossil_skill.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <path_to_fossil_repo.fossil>\n", argv[0]);
        return 1;
    }

    const char *repo_path = argv[1];
    fossil_set_repo_path(repo_path);

    Agent agents[MAX_AGENTS];
    int agent_count = 0;

    init_ui();
    char init_msg[512];
    snprintf(init_msg, sizeof(init_msg), "System initializing... Target Repo: %s", repo_path);
    log_message(init_msg);
    
    orchestrator_init(agents, &agent_count);
    
    log_message("Orchestrator online. Waiting for heartbeat...");

    int selected_agent = 0;
    int running = 1;

    // Set non-blocking input for the heartbeat loop matching TICK_INTERVAL_MS
    timeout(TICK_INTERVAL_MS);

    while (running) {
        draw_main_screen(agents, agent_count, selected_agent);

        int ch = getch();
        switch (ch) {
            case 'q':
            case 'Q':
                running = 0;
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
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "tmux attach -t agent:%s", agents[selected_agent].name);
                    
                    def_prog_mode();
                    endwin();
                    
                    system(cmd);
                    
                    reset_prog_mode();
                    refresh();
                    
                    char log[256];
                    snprintf(log, sizeof(log), "Detached from %s", agents[selected_agent].name);
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
            case ERR:
                // Timeout reached, perform Heartbeat TICK
                orchestrator_tick(agents, agent_count);
                break;
        }
    }

    log_message("Shutting down orchestrator...");
    orchestrator_shutdown(agents, agent_count);

    destroy_ui();
    return 0;
}
