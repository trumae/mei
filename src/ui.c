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
#define CP_SELECTED  1   // white on blue  — highlighted row
#define CP_OK        2   // green          — IN_PROGRESS, [done], [ok]
#define CP_ERROR     3   // red            — BLOCKED, OFFLINE, [!]
#define CP_WARN      4   // yellow         — PAUSED, [sync], [init], [wait]
#define CP_PULSE     5   // cyan           — [PULSE], Planned
#define CP_HEADER    6   // black on cyan  — header bar and status bar
#define CP_REVIEW    7   // magenta        — REVIEW state

static WINDOW *win_header  = NULL;
static WINDOW *win_list    = NULL;
static WINDOW *win_details = NULL;
static WINDOW *win_log     = NULL;
static WINDOW *win_status  = NULL;

static int ui_ready = 0;

typedef struct {
    char text[256];
    char timestamp[10];
} LogEntry;

static LogEntry log_messages[MAX_LOG_MESSAGES];
static int log_count = 0;

// ──────────────────────────────────────────────
// State symbol / color helpers — agents
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

// ──────────────────────────────────────────────
// State symbol / color helpers — tickets
// ──────────────────────────────────────────────

static const char *ticket_symbol(const char *status) {
    if (!status) return " ";
    if (strcmp(status, "In Progress") == 0) return "*";
    if (strcmp(status, "Blocked")     == 0) return "!";
    if (strcmp(status, "Review")      == 0) return "?";
    if (strcmp(status, "Rework")      == 0) return "~";
    if (strcmp(status, "Planned")     == 0) return "P";
    if (strcmp(status, "Done")        == 0) return "+";
    return " ";
}

static int ticket_color(const char *status) {
    if (!status) return 0;
    if (strcmp(status, "In Progress") == 0) return CP_OK;
    if (strcmp(status, "Blocked")     == 0) return CP_ERROR;
    if (strcmp(status, "Review")      == 0) return CP_REVIEW;
    if (strcmp(status, "Rework")      == 0) return CP_WARN;
    if (strcmp(status, "Planned")     == 0) return CP_PULSE;
    return 0;
}

// ──────────────────────────────────────────────
// Log color
// ──────────────────────────────────────────────

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

// ──────────────────────────────────────────────
// Box drawing
// ──────────────────────────────────────────────

static void draw_titled_box(WINDOW *win, const char *title) {
    box(win, 0, 0);
    if (title) {
        wattron(win, A_BOLD);
        mvwprintw(win, 0, 2, " %s ", title);
        wattroff(win, A_BOLD);
    }
}

// ──────────────────────────────────────────────
// Word-wrap printer — returns rows used
// ──────────────────────────────────────────────

static int draw_wrapped(WINDOW *win, int start_row, int col, int max_w, int max_rows,
                        const char *text) {
    if (!text || !*text || max_rows <= 0 || max_w <= 0) return 0;
    int row = 0;
    const char *p = text;
    while (*p && row < max_rows) {
        const char *nl = strchr(p, '\n');
        int seg_len = nl ? (int)(nl - p) : (int)strlen(p);
        if (seg_len == 0) {
            row++;
            if (nl) p = nl + 1;
            else break;
            continue;
        }
        while (seg_len > 0 && row < max_rows) {
            int chunk = (seg_len > max_w) ? max_w : seg_len;
            mvwprintw(win, start_row + row, col, "%.*s", chunk, p);
            p += chunk;
            seg_len -= chunk;
            row++;
        }
        if (nl) p = nl + 1;
        else break;
    }
    return row;
}

// ──────────────────────────────────────────────
// Shared header (tabs + stats)
// ──────────────────────────────────────────────

static void draw_header(int x_max, Agent *agents, int agent_count, ActiveScreen screen) {
    int active = 0;
    for (int i = 0; i < agent_count; i++)
        if (agents[i].state == AGENT_STATE_IN_PROGRESS) active++;

    time_t now = time(NULL);
    struct tm *tn = localtime(&now);

    wattron(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwhline(win_header, 0, 0, ' ', x_max);

    // App name
    mvwprintw(win_header, 0, 1, "MEI %s", MEI_VERSION_FULL);

    // Tabs
    static const char *tab_names[SCREEN_COUNT] = {"Agents", "Tickets"};
    int cx = 1 + 4 + (int)strlen(MEI_VERSION_FULL) + 2;
    for (int s = 0; s < SCREEN_COUNT; s++) {
        wmove(win_header, 0, cx);
        if (s == (int)screen) {
            wattron(win_header, A_REVERSE);
            wprintw(win_header, " %d:%s ", s + 1, tab_names[s]);
            wattroff(win_header, A_REVERSE);
        } else {
            wattroff(win_header, A_BOLD);
            wprintw(win_header, " %d:%s ", s + 1, tab_names[s]);
            wattron(win_header, A_BOLD);
        }
        cx += (int)strlen(tab_names[s]) + 4;
    }

    // Right-aligned stats
    char stats[80];
    snprintf(stats, sizeof(stats), "%d agent%s  %d active  %02d:%02d:%02d",
             agent_count, agent_count == 1 ? "" : "s",
             active, tn->tm_hour, tn->tm_min, tn->tm_sec);
    mvwprintw(win_header, 0, x_max - (int)strlen(stats) - 1, "%s", stats);
    wattroff(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
}

// ──────────────────────────────────────────────
// Shared log panel
// ──────────────────────────────────────────────

static void draw_log_panel(void) {
    draw_titled_box(win_log, "System Log");
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

// ──────────────────────────────────────────────
// Shared status bar
// ──────────────────────────────────────────────

static void draw_status_bar(int x_max, const char *hints) {
    wattron(win_status, COLOR_PAIR(CP_HEADER));
    mvwhline(win_status, 0, 0, ' ', x_max);
    mvwprintw(win_status, 0, 2, "%s", hints);
    wattroff(win_status, COLOR_PAIR(CP_HEADER));
}

// ──────────────────────────────────────────────
// Lifecycle
// ──────────────────────────────────────────────

void init_ui(void) {
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

    attron(A_BOLD);
    mvprintw(by + 2, (cols - tlen) / 2, "%s", title);
    attroff(A_BOLD);
    mvprintw(by + 3, (cols - vlen) / 2, "%s", MEI_VERSION_FULL);

    const char *label = "Initializing...";
    mvprintw(by + box_h + 1, (cols - (int)strlen(label)) / 2, "%s", label);

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

void destroy_ui(void) {
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

#define MEI_LOG_MAX_BYTES 524288

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

    if (!ui_ready) {
        draw_splash(msg);
    }

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
// Screen 1: Agents
// ──────────────────────────────────────────────

void draw_main_screen(Agent *agents, int agent_count, int selected_agent,
                      ActiveScreen active_screen) {
    int y_max, x_max;
    getmaxyx(stdscr, y_max, x_max);
    (void)y_max;

    werase(win_header);
    werase(win_list);
    werase(win_details);
    werase(win_log);
    werase(win_status);

    draw_header(x_max, agents, agent_count, active_screen);

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
    draw_titled_box(win_details, "Agent Details");
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

        row++;

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

    draw_log_panel();
    draw_status_bar(x_max,
        "q:quit  Tab:screens  \xe2\x86\x91\xe2\x86\x93:select  "
        "a:attach  p:pause  r:resume  k:kill");

    wnoutrefresh(win_header);
    wnoutrefresh(win_list);
    wnoutrefresh(win_details);
    wnoutrefresh(win_log);
    wnoutrefresh(win_status);
    doupdate();
}

// ──────────────────────────────────────────────
// Screen 2: Tickets
// ──────────────────────────────────────────────

static const char *sort_label(int sort_order) {
    switch (sort_order) {
        case TICKET_SORT_TITLE:  return "title";
        case TICKET_SORT_ASSIGN: return "assign";
        default:                 return "status";
    }
}

void draw_tickets_screen(FossilTicket *tickets, int count, int selected,
                         int sort_order, Agent *agents, int agent_count) {
    int y_max, x_max;
    getmaxyx(stdscr, y_max, x_max);
    (void)y_max;

    werase(win_header);
    werase(win_list);
    werase(win_details);
    werase(win_log);
    werase(win_status);

    draw_header(x_max, agents, agent_count, SCREEN_TICKETS);

    // ── Ticket list ─────────────────────────────
    {
        char list_title[64];
        snprintf(list_title, sizeof(list_title), "Tickets [%d]  sort:%s",
                 count, sort_label(sort_order));
        draw_titled_box(win_list, list_title);
    }

    {
        int lh, lw;
        getmaxyx(win_list, lh, lw);
        int usable_w = lw - 4;   // inner width (minus borders + indent)
        int sym_w    = 4;        // " [X] "
        int title_w  = usable_w - sym_w;
        if (title_w < 6) title_w = 6;

        // Scroll offset: keep selected row visible
        int visible_rows = lh - 3;
        int scroll = 0;
        if (selected >= visible_rows) scroll = selected - visible_rows + 1;

        for (int i = 0; i < visible_rows; i++) {
            int idx = scroll + i;
            if (idx >= count) break;

            FossilTicket *t  = &tickets[idx];
            const char   *sym = ticket_symbol(t->status);
            int           cp  = ticket_color(t->status);

            if (idx == selected) {
                wattron(win_list, COLOR_PAIR(CP_SELECTED) | A_BOLD);
                mvwprintw(win_list, i + 1, 1, " [%s] %-*.*s",
                          sym, title_w, title_w, t->title);
                wattroff(win_list, COLOR_PAIR(CP_SELECTED) | A_BOLD);
            } else {
                if (cp) wattron(win_list, COLOR_PAIR(cp));
                mvwprintw(win_list, i + 1, 1, " [%s] %-*.*s",
                          sym, title_w, title_w, t->title);
                if (cp) wattroff(win_list, COLOR_PAIR(cp));
            }
        }

        // Scroll indicator
        if (count > visible_rows) {
            wattron(win_list, A_DIM);
            mvwprintw(win_list, lh - 2, 2, "%d-%d / %d",
                      scroll + 1, scroll + visible_rows < count ? scroll + visible_rows : count,
                      count);
            wattroff(win_list, A_DIM);
        }
    }

    // ── Ticket details ──────────────────────────
    draw_titled_box(win_details, "Ticket Details");
    if (count == 0) {
        wattron(win_details, A_DIM);
        mvwprintw(win_details, 2, 3, "No active tickets.");
        wattroff(win_details, A_DIM);
    } else if (selected >= 0 && selected < count) {
        FossilTicket *t = &tickets[selected];
        int dh, dw;
        getmaxyx(win_details, dh, dw);
        int text_w = dw - 4;

        int row = 1;

        // UUID (short)
        wattron(win_details, A_DIM);
        mvwprintw(win_details, row++, 2, "%.32s", t->tkt_uuid);
        wattroff(win_details, A_DIM);

        row++;  // blank

        // Title
        wattron(win_details, A_BOLD);
        row += draw_wrapped(win_details, row, 2, text_w, 3, t->title);
        wattroff(win_details, A_BOLD);

        row++;  // blank

        // Status with color
        {
            int cp = ticket_color(t->status);
            mvwprintw(win_details, row, 2, "%-10s ", "Status:");
            if (cp) wattron(win_details, COLOR_PAIR(cp) | A_BOLD);
            wprintw(win_details, "%s", t->status);
            if (cp) wattroff(win_details, COLOR_PAIR(cp) | A_BOLD);
            row++;
        }

        // Assignee
        if (t->assignee[0]) {
            mvwprintw(win_details, row++, 2, "%-10s %.*s", "Assignee:", text_w - 10, t->assignee);
        }

        row++;  // blank

        // Separator + description
        if (t->comment[0]) {
            wattron(win_details, A_DIM);
            mvwhline(win_details, row, 2, ACS_HLINE, text_w);
            mvwprintw(win_details, row, 3, " Description ");
            wattroff(win_details, A_DIM);
            row++;

            int desc_rows = dh - row - (t->reviewer_notes[0] ? 5 : 2);
            if (desc_rows < 1) desc_rows = 1;
            if (desc_rows > dh - row - 1) desc_rows = dh - row - 1;
            row += draw_wrapped(win_details, row, 2, text_w, desc_rows, t->comment);
        }

        // Reviewer notes
        if (t->reviewer_notes[0] && row < dh - 3) {
            row++;
            wattron(win_details, A_DIM);
            mvwhline(win_details, row, 2, ACS_HLINE, text_w);
            mvwprintw(win_details, row, 3, " Reviewer Notes ");
            wattroff(win_details, A_DIM);
            row++;

            int notes_rows = dh - row - 1;
            if (notes_rows < 1) notes_rows = 1;
            wattron(win_details, COLOR_PAIR(CP_WARN));
            draw_wrapped(win_details, row, 2, text_w, notes_rows, t->reviewer_notes);
            wattroff(win_details, COLOR_PAIR(CP_WARN));
        }
    }

    draw_log_panel();
    draw_status_bar(x_max,
        "q:quit  Tab:screens  \xe2\x86\x91\xe2\x86\x93:select  "
        "s:next-status  S:prev-status  o:sort  r:reload");

    wnoutrefresh(win_header);
    wnoutrefresh(win_list);
    wnoutrefresh(win_details);
    wnoutrefresh(win_log);
    wnoutrefresh(win_status);
    doupdate();
}
