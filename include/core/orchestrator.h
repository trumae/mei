#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "agent.h"

// Configuration constants for the Orchestrator
#define TICK_INTERVAL_MS 2000
// 1500 ticks × 2s = 50 minutes before an agent is declared stalled.
// Complex implementation tasks (monitoring systems, test suites, runbooks) can
// legitimately take 30-45 minutes — 10 minutes was too aggressive.
#define MAX_STEPS_PER_TICKET 1500

// Initializes the orchestrator and loads agents from disk/config
void orchestrator_init(Agent *agents, int *agent_count);

// The core heartbeat function called every TICK_INTERVAL_MS
// It reads from fossil, checks tmux processes, and sends pulses.
void orchestrator_tick(Agent *agents, int agent_count);

// Cleans up orchestrator resources
void orchestrator_shutdown(Agent *agents, int agent_count);

#endif // ORCHESTRATOR_H
