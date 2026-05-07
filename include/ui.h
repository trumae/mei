#ifndef UI_H
#define UI_H

#include "agent.h"

void init_ui();
void draw_splash(const char *msg);
void ui_set_ready(void);
void destroy_ui();
void draw_main_screen(Agent *agents, int agent_count, int selected_agent);
void log_message(const char *msg);

#endif // UI_H
