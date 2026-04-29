#ifndef UI_H
#define UI_H

#include "agent.h"

void init_ui();
void destroy_ui();
void draw_main_screen(Agent *agents, int agent_count, int selected_agent);
void log_message(const char *msg);

#endif // UI_H
