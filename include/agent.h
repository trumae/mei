#ifndef AGENT_H
#define AGENT_H

#include "core/config.h"

typedef enum {
    AGENT_STATE_OPEN,
    AGENT_STATE_IN_PROGRESS,
    AGENT_STATE_REVIEW,
    AGENT_STATE_BLOCKED,
    AGENT_STATE_DONE,
    AGENT_STATE_PAUSED,
    AGENT_STATE_OFFLINE
} AgentState;

typedef struct {
    char name[64];
    char role[64];
    char cli[64];
    char cmd[512];
    char hash[41];
    char description[MEI_TEXT_BUFFER_SIZE];
    char capabilities[256];
    AgentState state;
    int last_heartbeat;
    int step_count;
    char current_ticket[64];
    int resolving_block;
    int doing_review;
    int doing_qa;
    int doing_phase2;
    char pending_review_ticket[64]; // ticket submitted for review; empty when none pending
} Agent;

#define MAX_AGENTS 10

// Mock data generation
void mock_agents(Agent *agents, int *count);
const char* state_to_string(AgentState state);

#endif // AGENT_H
