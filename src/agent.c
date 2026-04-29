#include "agent.h"
#include <string.h>

void mock_agents(Agent *agents, int *count) {
    *count = 4;
    
    strcpy(agents[0].name, "planner-1");
    strcpy(agents[0].role, "planner");
    agents[0].state = AGENT_STATE_IN_PROGRESS;
    agents[0].last_heartbeat = 2;
    strcpy(agents[0].current_ticket, "TKT-101");

    strcpy(agents[1].name, "coder-1");
    strcpy(agents[1].role, "coder");
    agents[1].state = AGENT_STATE_BLOCKED;
    agents[1].last_heartbeat = 5;
    strcpy(agents[1].current_ticket, "TKT-102");

    strcpy(agents[2].name, "reviewer-1");
    strcpy(agents[2].role, "reviewer");
    agents[2].state = AGENT_STATE_OPEN;
    agents[2].last_heartbeat = 1;
    strcpy(agents[2].current_ticket, "None");

    strcpy(agents[3].name, "coder-2");
    strcpy(agents[3].role, "coder");
    agents[3].state = AGENT_STATE_OFFLINE;
    agents[3].last_heartbeat = 300;
    strcpy(agents[3].current_ticket, "TKT-099");
}

const char* state_to_string(AgentState state) {
    switch (state) {
        case AGENT_STATE_OPEN: return "OPEN";
        case AGENT_STATE_IN_PROGRESS: return "IN_PROGRESS";
        case AGENT_STATE_REVIEW: return "REVIEW";
        case AGENT_STATE_BLOCKED: return "BLOCKED";
        case AGENT_STATE_DONE: return "DONE";
        case AGENT_STATE_PAUSED: return "PAUSED";
        case AGENT_STATE_OFFLINE: return "OFFLINE";
        default: return "UNKNOWN";
    }
}
