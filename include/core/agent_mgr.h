#ifndef AGENT_MGR_H
#define AGENT_MGR_H

#include "agent.h"

// Scans the .agents/ directory and parses the markdown files
// Populates the given array of agents up to MAX_AGENTS.
// Returns the number of agents successfully loaded.
int agent_mgr_load_all(Agent *agents);

#endif // AGENT_MGR_H
