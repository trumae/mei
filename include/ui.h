#ifndef UI_H
#define UI_H

#include "agent.h"
#include "core/vcs_backend.h"

typedef enum {
    SCREEN_AGENTS  = 0,
    SCREEN_TICKETS = 1,
    SCREEN_COUNT
} ActiveScreen;

#define TICKET_SORT_STATUS 0
#define TICKET_SORT_TITLE  1
#define TICKET_SORT_ASSIGN 2

void init_ui(void);
void draw_splash(const char *msg);
void ui_set_ready(void);
void destroy_ui(void);

void draw_main_screen(Agent *agents, int agent_count, int selected_agent,
                      ActiveScreen active_screen);
void draw_tickets_screen(VCSTicket *tickets, int count, int selected,
                         int sort_order, Agent *agents, int agent_count);
int ui_redirect_dialog(Agent *agents, int agent_count);
void log_message(const char *msg);

#endif // UI_H
