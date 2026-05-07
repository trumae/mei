#include "ui.h"
#include "version.h"
#include "core/orchestrator.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define MAX_LOG_MESSAGES 200

// Color pair IDs
#define CP_SELECTED  1   // white on blue  — highlighted agent row
#define CP_OK        2   // green          — IN_PROGRESS, [done], [ok]
#define CP_ERROR     3   // red            — BLOCKED, OFFLINE, [!]
#define CP_WARN      4   // yellow         — PAUSED, [sync], [init], [wait]
#define CP_PULSE     5   // cyan           — [PULSE]
#define CP_HEADER    6   // black on cyan  — header bar and status bar
#define CP_REVIEW    7   // magenta        — REVIEW state

static WINDOW *win_header  = NULL;
static WINDOW *win_list    = NULL;
static WINDOW *win_details = NULL;
static WINDOW *win_log     = NULL;
static WINDOW *win_status  = NULL;

static int ui_ready = 0;  // 0 = splash mode, 1 = main layout active

typedef struct {
    char text[256];
    char timestamp[10];  // "HH:MM:SS\0"
} LogEntry;

static LogEntry log_messages[MAX_LOG_MESSAGES];
static int log_count = 0;

// ──────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────

static const char *state_symbol(AgentState state) {
    switch (state) {
        case AGENT_STATE_IN_PROGRESS: return "*";
        case AGENT_STATE_PAUSED:      return "~";
        case AGENT_STATE_BLOCKED:     return "!";
        case AGENT_STATE_DONE:        return "+";
        case AGENT_STATE_REVIEW:      return "?";
        case AGENT_STATE_OFFLINE:     return "x";
        default:                      return " ";
    }
}

static int state_color(AgentState state) {
    switch (state) {
        case AGENT_STATE_IN_PROGRESS: return CP_OK;
        case AGENT_STATE_BLOCKED:
        case AGENT_STATE_OFFLINE:     return CP_ERROR;
        case AGENT_STATE_PAUSED:      return CP_WARN;
        case AGENT_STATE_REVIEW:      return CP_REVIEW;
        default:                      return 0;
    }
}

static int log_color(const char *text) {
    if (strstr(text, "[!]")    || strstr(text, "BLOCKED") || strstr(text, "FAILED"))
        return CP_ERROR;
    if (strstr(text, "[PULSE]"))
        return CP_PULSE;
    if (strstr(text, "[done]") || strstr(text, "[ok]"))
        return CP_OK;
    if (strstr(text, "[sync]") || strstr(text, "[init]") ||
        strstr(text, "[wait]") || strstr(text, "[trust]") || strstr(text, "[perm]"))
        return CP_WARN;
    return 0;
}

static void draw_titled_box(WINDOW *win, const char *title) {
    box(win, 0, 0);
    if (title) {
        wattron(win, A_BOLD);
        mvwprintw(win, 0, 2, " %s ", title);
        wattroff(win, A_BOLD);
    }
}

// ──────────────────────────────────────────────
// Lifecycle
// ──────────────────────────────────────────────

void init_ui() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        init_pair(CP_SELECTED, COLOR_WHITE,   COLOR_BLUE);
        init_pair(CP_OK,       COLOR_GREEN,   COLOR_BLACK);
        init_pair(CP_ERROR,    COLOR_RED,     COLOR_BLACK);
        init_pair(CP_WARN,     COLOR_YELLOW,  COLOR_BLACK);
        init_pair(CP_PULSE,    COLOR_CYAN,    COLOR_BLACK);
        init_pair(CP_HEADER,   COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_REVIEW,   COLOR_MAGENTA, COLOR_BLACK);
    }
    // Windows are created in ui_set_ready() after init completes.
}

void draw_splash(const char *msg) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    clear();

    const char *title = "MEI - Multi-Agent Environment Integrator";
    int tlen = (int)strlen(title);
    int vlen = (int)strlen(MEI_VERSION_FULL);
    int inner = tlen > vlen ? tlen : vlen;
    int box_w = inner + 8;
    int box_h = 6;
    int by = (rows - box_h) / 2 - 2;
    int bx = (cols - box_w) / 2;
    if (by < 0) by = 0;
    if (bx < 0) bx = 0;

    // Box border (cyan/bold)
    if (has_colors()) attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvaddch(by,           bx,           ACS_ULCORNER);
    mvaddch(by,           bx + box_w-1, ACS_URCORNER);
    mvaddch(by + box_h-1, bx,           ACS_LLCORNER);
    mvaddch(by + box_h-1, bx + box_w-1, ACS_LRCORNER);
    mvhline(by,           bx + 1,       ACS_HLINE, box_w - 2);
    mvhline(by + box_h-1, bx + 1,       ACS_HLINE, box_w - 2);
    mvvline(by + 1,       bx,           ACS_VLINE, box_h - 2);
    mvvline(by + 1,       bx + box_w-1, ACS_VLINE, box_h - 2);
    if (has_colors()) attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);

    // Title and version centered inside box
    attron(A_BOLD);
    mvprintw(by + 2, (cols - tlen) / 2, "%s", title);
    attroff(A_BOLD);
    mvprintw(by + 3, (cols - vlen) / 2, "%s", MEI_VERSION_FULL);

    // "Initializing..." label below box
    const char *label = "Initializing...";
    mvprintw(by + box_h + 1, (cols - (int)strlen(label)) / 2, "%s", label);

    // Last log message as progress indicator
    if (msg && msg[0]) {
        int max_msg = cols - 6;
        if (max_msg < 10) max_msg = 10;
        attron(A_DIM);
        mvprintw(by + box_h + 3, 3, "> %.*s", max_msg, msg);
        attroff(A_DIM);
    }

    refresh();
}

void ui_set_ready(void) {
    ui_ready = 1;

    int y_max, x_max;
    getmaxyx(stdscr, y_max, x_max);

    int log_h = y_max / 3;
    if (log_h < 6)  log_h = 6;
    if (log_h > 20) log_h = 20;
    int top_h = y_max - 1 - log_h - 1;
    if (top_h < 4)  top_h = 4;
    int list_w = x_max / 3;
    int det_w  = x_max - list_w;

    win_header  = newwin(1,      x_max,  0,          0);
    win_list    = newwin(top_h,  list_w, 1,          0);
    win_details = newwin(top_h,  det_w,  1,          list_w);
    win_log     = newwin(log_h,  x_max,  1 + top_h,  0);
    win_status  = newwin(1,      x_max,  y_max - 1,  0);

    clear();
    refresh();
}

void destroy_ui() {
    if (win_header)  delwin(win_header);
    if (win_list)    delwin(win_list);
    if (win_details) delwin(win_details);
    if (win_log)     delwin(win_log);
    if (win_status)  delwin(win_status);
    endwin();
}

// ──────────────────────────────────────────────
// Logging
// ──────────────────────────────────────────────

#define MEI_LOG_MAX_BYTES 524288  /* 512 KB – rotate when exceeded */

void log_message(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    LogEntry *entry;
    if (log_count < MAX_LOG_MESSAGES) {
        entry = &log_messages[log_count++];
    } else {
        for (int i = 0; i < MAX_LOG_MESSAGES - 1; i++)
            log_messages[i] = log_messages[i + 1];
        entry = &log_messages[MAX_LOG_MESSAGES - 1];
    }
    strncpy(entry->text, msg, sizeof(entry->text) - 1);
    entry->text[sizeof(entry->text) - 1] = '\0';
    snprintf(entry->timestamp, sizeof(entry->timestamp),
             "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);

    // During init: update the splash with the latest message.
    if (!ui_ready) {
        draw_splash(msg);
    }

    // File sink with rotation
    FILE *f = fopen("/tmp/mei.log", "a");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > MEI_LOG_MAX_BYTES) {
        fclose(f);
        f = fopen("/tmp/mei.log", "w");
        if (!f) return;
        fprintf(f, "[rotated]\n");
    }
    fprintf(f, "[%02d:%02d:%02d] %s\n", t->tm_hour, t->tm_min, t->tm_sec, msg);
    fclose(f);
}

// ──────────────────────────────────────────────
// Main layout
// ──────────────────────────────────────────────

void draw_main_screen(Agent *agents, int agent_count, int selected_agent) {
    int y_max, x_max;
    getmaxyx(stdscr, y_max, x_max);
    (void)y_max;

    werase(win_header);
    werase(win_list);
    werase(win_details);
    werase(win_log);
    werase(win_status);

    // ── Header bar ───────────────────────────────
    {
        int active = 0;
        for (int i = 0; i < agent_count; i++)
            if (agents[i].state == AGENT_STATE_IN_PROGRESS) active++;

        time_t now = time(NULL);
        struct tm *tn = localtime(&now);

        wattron(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
        mvwhline(win_header, 0, 0, ' ', x_max);
        mvwprintw(win_header, 0, 2,
                  "MEI %s   %d agent%s   %d in-progress   %02d:%02d:%02d",
                  MEI_VERSION_FULL,
                  agent_count, agent_count == 1 ? "" : "s",
                  active,
                  tn->tm_hour, tn->tm_min, tn->tm_sec);
        wattroff(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
    }

    // ── Agent list ───────────────────────────────
    draw_titled_box(win_list, "Agents");
    {
        int lh, lw;
        getmaxyx(win_list, lh, lw);
        (void)lw;

        for (int i = 0; i < agent_count; i++) {
            if (i >= lh - 3) break;

            const char *sym    = state_symbol(agents[i].state);
            int          cpair = state_color(agents[i].state);

            char tkt[10] = "---";
            if (agents[i].current_ticket[0] &&
                strcmp(agents[i].current_ticket, "None") != 0)
                snprintf(tkt, sizeof(tkt), "%.8s", agents[i].current_ticket);

            if (i == selected_agent) {
                wattron(win_list, COLOR_PAIR(CP_SELECTED) | A_BOLD);
                mvwprintw(win_list, i + 1, 1, " [%s] %-16s %s", sym, agents[i].name, tkt);
                wattroff(win_list, COLOR_PAIR(CP_SELECTED) | A_BOLD);
            } else {
                if (cpair) wattron(win_list, COLOR_PAIR(cpair));
                mvwprintw(win_list, i + 1, 1, " [%s] %-16s %s", sym, agents[i].name, tkt);
                if (cpair) wattroff(win_list, COLOR_PAIR(cpair));
            }
        }

        wattron(win_list, A_DIM);
        mvwprintw(win_list, lh - 2, 2, "*run ~pause !block ?review");
        wattroff(win_list, A_DIM);
    }

    // ── Details panel ───────────────────────────
    draw_titled_box(win_details, "Details");
    if (selected_agent >= 0 && selected_agent < agent_count) {
        Agent *a = &agents[selected_agent];
        int dh, dw;
        getmaxyx(win_details, dh, dw);
        (void)dh;

        int row = 1;

        wattron(win_details, A_BOLD);
        mvwprintw(win_details, row++, 2, "%-12s %s", "Name:", a->name);
        wattroff(win_details, A_BOLD);

        mvwprintw(win_details, row++, 2, "%-12s %s", "Role:", a->role);

        {
            int sc = state_color(a->state);
            mvwprintw(win_details, row, 2, "%-12s ", "State:");
            if (sc) wattron(win_details, COLOR_PAIR(sc) | A_BOLD);
            wprintw(win_details, "%s", state_to_string(a->state));
            if (sc) wattroff(win_details, COLOR_PAIR(sc) | A_BOLD);
            row++;
        }

        const char *tkt = (a->current_ticket[0] &&
                            strcmp(a->current_ticket, "None") != 0)
                          ? a->current_ticket : "---";
        mvwprintw(win_details, row++, 2, "%-12s %s", "Ticket:", tkt);

        if (a->state == AGENT_STATE_IN_PROGRESS && a->step_count > 0) {
            int bar_w = dw - 28;
            if (bar_w < 5)  bar_w = 5;
            if (bar_w > 40) bar_w = 40;
            int filled = (a->step_count * bar_w) / MAX_STEPS_PER_TICKET;
            if (filled > bar_w) filled = bar_w;

            char bar[64];
            bar[0] = '[';
            for (int b = 0; b < bar_w; b++)
                bar[b + 1] = (b < filled) ? '|' : ' ';
            bar[bar_w + 1] = ']';
            bar[bar_w + 2] = '\0';

            int cp = (filled >= bar_w - 2) ? CP_ERROR : CP_OK;
            mvwprintw(win_details, row, 2, "%-12s ", "Steps:");
            wattron(win_details, COLOR_PAIR(cp));
            wprintw(win_details, "%s", bar);
            wattroff(win_details, COLOR_PAIR(cp));
            wprintw(win_details, " %d/%d", a->step_count, MAX_STEPS_PER_TICKET);
            row++;
        } else {
            mvwprintw(win_details, row++, 2, "%-12s %ds ago", "Heartbeat:", a->last_heartbeat);
        }

        row++;  // blank separator

        mvwprintw(win_details, row++, 2, "%-12s %s", "CLI:", a->cli);
        if (a->cmd[0]) {
            int max_cmd = dw - 16;
            if (max_cmd < 8) max_cmd = 8;
            mvwprintw(win_details, row++, 2, "%-12s %.*s", "Command:", max_cmd, a->cmd);
        }
        if (a->capabilities[0]) {
            int max_cap = dw - 16;
            if (max_cap < 8) max_cap = 8;
            mvwprintw(win_details, row++, 2, "%-12s %.*s", "Caps:", max_cap, a->capabilities);
        }
        mvwprintw(win_details, row, 2, "%-12s /tmp/workspaces/%s", "Workspace:", a->name);
    }

    // ── Log panel ───────────────────────────────
    draw_titled_box(win_log, "System Log");
    {
        int lh, lw;
        getmaxyx(win_log, lh, lw);
        int visible = lh - 2;

        int start = (log_count > visible) ? log_count - visible : 0;
        for (int i = 0; i < visible && (start + i) < log_count; i++) {
            LogEntry *e = &log_messages[start + i];
            int cp = log_color(e->text);

            wattron(win_log, A_DIM);
            mvwprintw(win_log, i + 1, 2, "[%s] ", e->timestamp);
            wattroff(win_log, A_DIM);

            int msg_max = lw - 14;
            if (msg_max < 10) msg_max = 10;
            if (cp) wattron(win_log, COLOR_PAIR(cp));
            wprintw(win_log, "%.*s", msg_max, e->text);
            if (cp) wattroff(win_log, COLOR_PAIR(cp));
        }
    }

    // ── Status bar ──────────────────────────────
    {
        wattron(win_status, COLOR_PAIR(CP_HEADER));
        mvwhline(win_status, 0, 0, ' ', x_max);
        mvwprintw(win_status, 0, 2,
                  "q:quit   a:attach   p:pause   r:resume   k:kill   ↑↓:navigate");
        wattroff(win_status, COLOR_PAIR(CP_HEADER));
    }

    wnoutrefresh(win_header);
    wnoutrefresh(win_list);
    wnoutrefresh(win_details);
    wnoutrefresh(win_log);
    wnoutrefresh(win_status);
    doupdate();
}
