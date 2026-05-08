#include "cmd/run.h"
#include "ui.h"
#include "agent.h"
#include "core/orchestrator.h"
#include "core/fossil_skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <ncurses.h>
#include <pthread.h>

static Agent       *g_agents      = NULL;
static int          g_agent_count = 0;
static volatile int g_running     = 1;

pthread_mutex_t g_agents_mutex = PTHREAD_MUTEX_INITIALIZER;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void *tick_thread_fn(void *arg) {
    (void)arg;
    struct timespec last = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (g_running) {
        usleep(100000); // poll every 100ms without blocking

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec  - last.tv_sec)  * 1000
                        + (now.tv_nsec - last.tv_nsec) / 1000000;

        if (elapsed_ms >= TICK_INTERVAL_MS) {
            last = now;
            pthread_mutex_lock(&g_agents_mutex);
            orchestrator_tick(g_agents, g_agent_count);
            pthread_mutex_unlock(&g_agents_mutex);
        }
    }
    return NULL;
}

int cmd_run(int argc, char *argv[]) {
    const char *repo_arg = NULL;
    int clean_workspaces = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--clean") == 0)
            clean_workspaces = 1;
        else if (!repo_arg)
            repo_arg = argv[i];
    }

    if (!repo_arg) {
        fprintf(stderr, "Usage: mei run <repo.fossil> [--clean]\n");
        return 1;
    }

    char abs_repo[4096];
    if (!realpath(repo_arg, abs_repo)) {
        fprintf(stderr, "Error: could not resolve '%s'\n", repo_arg);
        return 1;
    }

    if (clean_workspaces) {
        printf("Cleaning workspaces in /tmp/workspaces/...\n");
        system("rm -rf /tmp/workspaces");
    }

    fossil_set_repo_path(abs_repo);

    Agent agents[MAX_AGENTS];
    g_agents = agents;
    int agent_count = 0;

    init_ui();
    draw_splash(NULL);
    signal(SIGINT, handle_sigint);

    char msg[512];
    snprintf(msg, sizeof(msg), "System initializing... Target Repo: %s", abs_repo);
    log_message(msg);

    orchestrator_init(agents, &agent_count);
    g_agent_count = agent_count;

    log_message("Orchestrator online. Waiting for heartbeat...");
    ui_set_ready();

    int selected_agent = 0;
    timeout(200); // short timeout so UI refreshes ~5x/s regardless of input

    pthread_t tick_thread;
    pthread_create(&tick_thread, NULL, tick_thread_fn, NULL);

    while (g_running) {
        pthread_mutex_lock(&g_agents_mutex);
        draw_main_screen(agents, agent_count, selected_agent);
        pthread_mutex_unlock(&g_agents_mutex);

        int ch = getch();
        switch (ch) {
            case 'q': case 'Q':
                g_running = 0;
                break;
            case KEY_UP:
                if (selected_agent > 0) selected_agent--;
                break;
            case KEY_DOWN:
                if (selected_agent < agent_count - 1) selected_agent++;
                break;
            case 'a': case 'A':
                if (agent_count > 0) {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd),
                             "tmux select-window -t mei:\"%s\"; tmux attach -t mei",
                             agents[selected_agent].name);
                    char log[256];
                    snprintf(log, sizeof(log),
                             "[!] Ticks PAUSED — attached to %s. Detach with Ctrl+B, D to resume.",
                             agents[selected_agent].name);
                    log_message(log);
                    def_prog_mode();
                    endwin();
                    system(cmd);
                    reset_prog_mode();
                    refresh();
                    snprintf(log, sizeof(log), "[ok] Detached from %s — ticks resuming.",
                             agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case 'p': case 'P':
                if (agent_count > 0) {
                    pthread_mutex_lock(&g_agents_mutex);
                    agents[selected_agent].state = AGENT_STATE_PAUSED;
                    pthread_mutex_unlock(&g_agents_mutex);
                    char log[256];
                    snprintf(log, sizeof(log), "Paused agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case 'r': case 'R':
                if (agent_count > 0) {
                    pthread_mutex_lock(&g_agents_mutex);
                    agents[selected_agent].state = AGENT_STATE_IN_PROGRESS;
                    pthread_mutex_unlock(&g_agents_mutex);
                    char log[256];
                    snprintf(log, sizeof(log), "Resumed agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case 'k': case 'K':
                if (agent_count > 0) {
                    pthread_mutex_lock(&g_agents_mutex);
                    agents[selected_agent].state = AGENT_STATE_OFFLINE;
                    pthread_mutex_unlock(&g_agents_mutex);
                    char log[256];
                    snprintf(log, sizeof(log), "Killed agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;
            case ERR:
                break; // tick is handled by the background thread
        }
    }

    pthread_join(tick_thread, NULL);

    log_message("Shutting down orchestrator...");
    orchestrator_shutdown(agents, agent_count);
    destroy_ui();
    return 0;
}
