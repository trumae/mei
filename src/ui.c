#include "ui.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>

#define MAX_LOG_MESSAGES 100

static WINDOW *win_list;
static WINDOW *win_details;
static WINDOW *win_log;

static char log_messages[MAX_LOG_MESSAGES][256];
static int log_count = 0;

void init_ui() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0); // Hide cursor

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);    // Selected
        init_pair(2, COLOR_GREEN, COLOR_BLACK);   // Running / OK
        init_pair(3, COLOR_RED, COLOR_BLACK);     // Offline / Error
        init_pair(4, COLOR_YELLOW, COLOR_BLACK);  // Blocked / Paused
        init_pair(5, COLOR_CYAN, COLOR_BLACK);    // Info
    }

    int y_max, x_max;
    getmaxyx(stdscr, y_max, x_max);

    win_list = newwin(y_max - 6, x_max / 3, 0, 0);
    win_details = newwin(y_max - 6, x_max - (x_max / 3), 0, x_max / 3);
    win_log = newwin(6, x_max, y_max - 6, 0);
}

void destroy_ui() {
    delwin(win_list);
    delwin(win_details);
    delwin(win_log);
    endwin();
}

void log_message(const char *msg) {
    if (log_count < MAX_LOG_MESSAGES) {
        strncpy(log_messages[log_count], msg, 255);
        log_count++;
    } else {
        for (int i = 0; i < MAX_LOG_MESSAGES - 1; i++) {
            strcpy(log_messages[i], log_messages[i+1]);
        }
        strncpy(log_messages[MAX_LOG_MESSAGES - 1], msg, 255);
    }
}

static void draw_borders(WINDOW *win, const char *title) {
    box(win, 0, 0);
    if (title) {
        mvwprintw(win, 0, 2, " %s ", title);
    }
}

void draw_main_screen(Agent *agents, int agent_count, int selected_agent) {
    // Clear windows
    werase(win_list);
    werase(win_details);
    werase(win_log);

    draw_borders(win_list, " Agents ");
    draw_borders(win_details, " Details & Monitoring ");
    draw_borders(win_log, " Logs & Actions (q: quit, a: attach, p: pause, r: resume, k: kill) ");

    // Draw Agent List
    for (int i = 0; i < agent_count; i++) {
        if (i == selected_agent) {
            wattron(win_list, COLOR_PAIR(1));
        }

        int color_pair = 0;
        if (agents[i].state == AGENT_STATE_IN_PROGRESS) color_pair = 2;
        else if (agents[i].state == AGENT_STATE_OFFLINE || agents[i].state == AGENT_STATE_BLOCKED) color_pair = 3;
        else if (agents[i].state == AGENT_STATE_PAUSED) color_pair = 4;

        if (i != selected_agent && color_pair != 0) {
            wattron(win_list, COLOR_PAIR(color_pair));
        }

        mvwprintw(win_list, i + 1, 1, "[%c] %-15s %-10s", 
                  (agents[i].state == AGENT_STATE_IN_PROGRESS) ? '*' : ' ',
                  agents[i].name, state_to_string(agents[i].state));

        if (i != selected_agent && color_pair != 0) {
            wattroff(win_list, COLOR_PAIR(color_pair));
        }

        if (i == selected_agent) {
            wattroff(win_list, COLOR_PAIR(1));
        }
    }

    // Draw Details
    if (selected_agent >= 0 && selected_agent < agent_count) {
        Agent *a = &agents[selected_agent];
        mvwprintw(win_details, 2, 2, "Name: %s", a->name);
        mvwprintw(win_details, 3, 2, "Role: %s", a->role);
        mvwprintw(win_details, 4, 2, "State: %s", state_to_string(a->state));
        mvwprintw(win_details, 5, 2, "Current Ticket: %s", a->current_ticket);
        mvwprintw(win_details, 6, 2, "Last Heartbeat: %d seconds ago", a->last_heartbeat);

        mvwprintw(win_details, 9, 2, "Tmux Session: mei");
        mvwprintw(win_details, 10, 2, "Tmux Window: %s", a->name);
        
        // Mock some stats/graphs
        mvwprintw(win_details, 11, 2, "CPU Usage: [||||||    ] 60%%");
        mvwprintw(win_details, 12, 2, "RAM Usage: [||||||||  ] 80%%");
    }

    // Draw Logs
    int log_start = log_count > 4 ? log_count - 4 : 0;
    for (int i = 0; i < 4 && (log_start + i) < log_count; i++) {
        mvwprintw(win_log, i + 1, 2, "%s", log_messages[log_start + i]);
    }

    // Refresh all windows
    wnoutrefresh(win_list);
    wnoutrefresh(win_details);
    wnoutrefresh(win_log);
    doupdate();
}
