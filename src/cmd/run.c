#include "cmd/run.h"
#include "ui.h"
#include "agent.h"
#include "core/orchestrator.h"
#include "core/vcs_backend.h"
#include "core/fossil_skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>

static Agent       *g_agents      = NULL;
static int          g_agent_count = 0;
static volatile int g_running     = 1;

pthread_mutex_t g_agents_mutex = PTHREAD_MUTEX_INITIALIZER;

// ──────────────────────────────────────────────
// Ticket screen state — static to avoid large stack frames
// ──────────────────────────────────────────────

#define MAX_UI_TICKETS 200

static ActiveScreen  g_screen          = SCREEN_AGENTS;
static VCSTicket     g_tickets[MAX_UI_TICKETS];
static int           g_ticket_count    = 0;
static int           g_selected_ticket = 0;
static int           g_sort_order      = TICKET_SORT_STATUS;

// ──────────────────────────────────────────────
// Ticket sort
// ──────────────────────────────────────────────

static int status_priority(const char *s) {
    if (strcmp(s, "Blocked")     == 0) return 0;
    if (strcmp(s, "In Progress") == 0) return 1;
    if (strcmp(s, "Review")      == 0) return 2;
    if (strcmp(s, "Rework")      == 0) return 3;
    if (strcmp(s, "Planned")     == 0) return 4;
    if (strcmp(s, "Open")        == 0) return 5;
    return 6;
}

static int sort_order_ref;  // set before qsort call

static int cmp_tickets(const void *a, const void *b) {
    const VCSTicket *ta = (const VCSTicket *)a;
    const VCSTicket *tb = (const VCSTicket *)b;
    switch (sort_order_ref) {
        case TICKET_SORT_TITLE:
            return strncasecmp(ta->title, tb->title, sizeof(ta->title));
        case TICKET_SORT_ASSIGN:
            return strncasecmp(ta->assignee, tb->assignee, sizeof(ta->assignee));
        default: { // TICKET_SORT_STATUS
            int pa = status_priority(ta->status);
            int pb = status_priority(tb->status);
            if (pa != pb) return pa - pb;
            return strncasecmp(ta->title, tb->title, sizeof(ta->title));
        }
    }
}

static void reload_tickets(void) {
    g_ticket_count = g_backend->ticket_list(g_backend, g_tickets, MAX_UI_TICKETS);
    sort_order_ref = g_sort_order;
    if (g_ticket_count > 1)
        qsort(g_tickets, g_ticket_count, sizeof(VCSTicket), cmp_tickets);
    if (g_selected_ticket >= g_ticket_count)
        g_selected_ticket = g_ticket_count > 0 ? g_ticket_count - 1 : 0;
}

// ──────────────────────────────────────────────
// Status cycle
// ──────────────────────────────────────────────

static const char *status_order[] = {
    "Open", "Planned", "In Progress", "Review", "Rework", "Blocked", "Done"
};
static const int STATUS_COUNT = 7;

static const char *next_status(const char *current, int delta) {
    for (int i = 0; i < STATUS_COUNT; i++) {
        if (strcmp(status_order[i], current) == 0) {
            int next = (i + delta + STATUS_COUNT) % STATUS_COUNT;
            return status_order[next];
        }
    }
    return "Open";
}

// ──────────────────────────────────────────────
// Signal handlers
// ──────────────────────────────────────────────

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void handle_fatal(int sig) {
    const char *msg;
    if (sig == SIGSEGV) msg = "[CRASH] SIGSEGV — segmentation fault\n";
    else if (sig == SIGBUS)  msg = "[CRASH] SIGBUS — bus error\n";
    else if (sig == SIGTERM) msg = "[CRASH] SIGTERM — terminated\n";
    else if (sig == SIGHUP)  msg = "[CRASH] SIGHUP — hangup\n";
    else                     msg = "[CRASH] fatal signal\n";
    int fd = open("/tmp/mei.log", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { write(fd, msg, strlen(msg)); close(fd); }
    signal(sig, SIG_DFL);
    raise(sig);
}

// ──────────────────────────────────────────────
// Tick thread
// ──────────────────────────────────────────────

static void *tick_thread_fn(void *arg) {
    (void)arg;
    struct timespec last = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (g_running) {
        usleep(100000);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec  - last.tv_sec)  * 1000
                        + (now.tv_nsec - last.tv_nsec) / 1000000;

        if (elapsed_ms >= TICK_INTERVAL_MS) {
            last = now;
            orchestrator_tick(g_agents, g_agent_count);
        }
    }
    return NULL;
}

// ──────────────────────────────────────────────
// Main command
// ──────────────────────────────────────────────

int cmd_run(int argc, char *argv[]) {
    const char *repo_arg = NULL;
    int clean_workspaces = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--clean") == 0)
            clean_workspaces = 1;
        else if (!repo_arg)
            repo_arg = argv[i];
    }

    if (!g_backend) {
        if (!repo_arg) {
            fprintf(stderr, "Usage: mei run <repo.fossil> [--clean]\n");
            return 1;
        }
        char abs_repo[4096];
        if (!realpath(repo_arg, abs_repo)) {
            fprintf(stderr, "Error: could not resolve '%s'\n", repo_arg);
            return 1;
        }
        if (!vcs_backend_create(VCS_FOSSIL, abs_repo)) return 1;
    }

    if (clean_workspaces) {
        printf("Cleaning workspaces in /tmp/workspaces/...\n");
        system("rm -rf /tmp/workspaces");
    }

    Agent agents[MAX_AGENTS];
    g_agents = agents;
    int agent_count = 0;

    init_ui();
    draw_splash(NULL);
    signal(SIGINT,  handle_sigint);
    signal(SIGHUP,  handle_fatal);
    signal(SIGTERM, handle_fatal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, handle_fatal);
    signal(SIGBUS,  handle_fatal);

    char msg[512];
    snprintf(msg, sizeof(msg), "System initializing... Target Repo: %s", g_backend->repo_id);
    log_message(msg);

    orchestrator_init(agents, &agent_count);
    g_agent_count = agent_count;

    log_message("Orchestrator online. Waiting for heartbeat...");
    ui_set_ready();

    int selected_agent = 0;
    timeout(200);

    pthread_t      tick_thread;
    pthread_attr_t tick_attr;
    pthread_attr_init(&tick_attr);
    pthread_attr_setstacksize(&tick_attr, 2 * 1024 * 1024);
    pthread_attr_setdetachstate(&tick_attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tick_thread, &tick_attr, tick_thread_fn, NULL);
    pthread_attr_destroy(&tick_attr);

    while (g_running) {
        if (g_screen == SCREEN_TICKETS) {
            draw_tickets_screen(g_tickets, g_ticket_count, g_selected_ticket,
                                g_sort_order, agents, agent_count);
        } else {
            draw_main_screen(agents, agent_count, selected_agent, g_screen);
        }

        int ch = getch();
        switch (ch) {

            // ── Universal ─────────────────────────────
            case 'q': case 'Q':
                g_running = 0;
                break;

            case '\t':  // Tab — cycle screens
            case KEY_BTAB: {
                int next = ((int)g_screen + 1) % SCREEN_COUNT;
                if (next == SCREEN_TICKETS) reload_tickets();
                g_screen = (ActiveScreen)next;
                break;
            }
            case '1':
                g_screen = SCREEN_AGENTS;
                break;
            case '2':
                if (g_screen != SCREEN_TICKETS) reload_tickets();
                g_screen = SCREEN_TICKETS;
                break;

            // ── Agents screen ─────────────────────────
            case KEY_UP:
                if (g_screen == SCREEN_AGENTS) {
                    if (selected_agent > 0) selected_agent--;
                } else {
                    if (g_selected_ticket > 0) g_selected_ticket--;
                }
                break;
            case KEY_DOWN:
                if (g_screen == SCREEN_AGENTS) {
                    if (selected_agent < agent_count - 1) selected_agent++;
                } else {
                    if (g_selected_ticket < g_ticket_count - 1) g_selected_ticket++;
                }
                break;

            case 'a': case 'A':
                if (g_screen == SCREEN_AGENTS && agent_count > 0) {
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
                    flushinp();
                    snprintf(log, sizeof(log), "[ok] Detached from %s — ticks resuming.",
                             agents[selected_agent].name);
                    log_message(log);
                }
                break;

            case 'p': case 'P':
                if (g_screen == SCREEN_AGENTS && agent_count > 0) {
                    pthread_mutex_lock(&g_agents_mutex);
                    agents[selected_agent].state = AGENT_STATE_PAUSED;
                    pthread_mutex_unlock(&g_agents_mutex);
                    char log[256];
                    snprintf(log, sizeof(log), "Paused agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;

            case 'r': case 'R':
                if (g_screen == SCREEN_AGENTS && agent_count > 0) {
                    pthread_mutex_lock(&g_agents_mutex);
                    agents[selected_agent].state = AGENT_STATE_IN_PROGRESS;
                    pthread_mutex_unlock(&g_agents_mutex);
                    char log[256];
                    snprintf(log, sizeof(log), "Resumed agent: %s", agents[selected_agent].name);
                    log_message(log);
                } else if (g_screen == SCREEN_TICKETS) {
                    // Reload tickets from Fossil
                    reload_tickets();
                    log_message("[sync] Ticket list reloaded.");
                }
                break;

            case 'k': case 'K':
                if (g_screen == SCREEN_AGENTS && agent_count > 0) {
                    pthread_mutex_lock(&g_agents_mutex);
                    agents[selected_agent].state = AGENT_STATE_OFFLINE;
                    pthread_mutex_unlock(&g_agents_mutex);
                    char log[256];
                    snprintf(log, sizeof(log), "Killed agent: %s", agents[selected_agent].name);
                    log_message(log);
                }
                break;

            // ── Tickets screen ────────────────────────
            case 's':   // next status
                if (g_screen == SCREEN_TICKETS && g_ticket_count > 0) {
                    VCSTicket *t = &g_tickets[g_selected_ticket];
                    const char *ns = next_status(t->status, +1);
                    if (g_backend->ticket_set_status(g_backend, t->uuid, ns)) {
                        char log[256];
                        snprintf(log, sizeof(log), "[ok] Ticket %.12s → %s", t->uuid, ns);
                        log_message(log);
                        reload_tickets();
                    }
                }
                break;

            case 'S':   // prev status
                if (g_screen == SCREEN_TICKETS && g_ticket_count > 0) {
                    VCSTicket *t = &g_tickets[g_selected_ticket];
                    const char *ns = next_status(t->status, -1);
                    if (g_backend->ticket_set_status(g_backend, t->uuid, ns)) {
                        char log[256];
                        snprintf(log, sizeof(log), "[ok] Ticket %.12s → %s", t->uuid, ns);
                        log_message(log);
                        reload_tickets();
                    }
                }
                break;

            case 'o': case 'O':   // cycle sort order
                if (g_screen == SCREEN_TICKETS) {
                    g_sort_order = (g_sort_order + 1) % 3;
                    sort_order_ref = g_sort_order;
                    if (g_ticket_count > 1)
                        qsort(g_tickets, g_ticket_count, sizeof(VCSTicket), cmp_tickets);
                    g_selected_ticket = 0;
                }
                break;

            case 'd': case 'D':
                if (g_screen == SCREEN_TICKETS && g_ticket_count > 0) {
                    VCSTicket *t = &g_tickets[g_selected_ticket];
                    int chosen = ui_redirect_dialog(agents, agent_count);
                    if (chosen >= 0) {
                        if (g_backend->ticket_assign(g_backend, t->uuid, agents[chosen].hash)) {
                            char log[256];
                            snprintf(log, sizeof(log),
                                     "[ok] Ticket %.12s redirected → %s",
                                     t->uuid, agents[chosen].name);
                            log_message(log);
                            reload_tickets();
                        }
                    }
                }
                break;

            case ERR:
                break;
        }
    }

    log_message("Shutting down orchestrator...");
    orchestrator_shutdown(agents, agent_count);
    destroy_ui();
    return 0;
}
