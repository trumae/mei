#ifndef TMUX_MGR_H
#define TMUX_MGR_H

#include <stdbool.h>
#include <stddef.h>

// Checks if a tmux session for the given agent name already exists.
bool tmux_session_exists(const char *agent_name);

// Spawns a new tmux session for the agent, starting the given CLI command in the specified workspace.
bool tmux_spawn_agent(const char *agent_name, const char *cli_command, const char *workspace);

// Kills the tmux session associated with the agent.
bool tmux_kill_agent(const char *agent_name);

// Sends a string (typically a PULSE protocol payload) to the agent's tmux session, followed by Enter (C-m).
bool tmux_send_pulse(const char *agent_name, const char *pulse_payload);

// Captures the current visible pane output of the agent's tmux session.
// Returns the number of bytes written to buffer, or -1 on error.
int tmux_capture_output(const char *agent_name, char *buffer, size_t max_size);

#endif // TMUX_MGR_H
