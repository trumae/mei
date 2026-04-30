#include "core/orchestrator.h"
#include "core/tmux_mgr.h"
#include "core/fossil_skill.h"
#include "core/pulse.h"
#include "core/agent_mgr.h"
#include "ui.h" // For log_message integration
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to keep track of steps taken per ticket by an agent
static int ticket_steps[MAX_AGENTS] = {0};

void orchestrator_init(Agent *agents, int *agent_count) {
    // Scan /.agents/*.md to populate real agents
    *agent_count = agent_mgr_load_all(agents);

    const char *repo_path = fossil_get_repo_path();

    for (int i = 0; i < *agent_count; i++) {
        ticket_steps[i] = 0;
        
        // Spawn the agent tmux session if it doesn't exist
        char workspace[256];
        snprintf(workspace, sizeof(workspace), "/tmp/workspaces/%s", agents[i].name);
        
        // Ensure workspace dir exists
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s", workspace);
        system(cmd);

        // Open fossil repo in workspace if not already open
        if (repo_path[0]) {
            snprintf(cmd, sizeof(cmd), "cd %s && if [ ! -f .fslckout ]; then fossil open %s > /dev/null 2>&1; fi", workspace, repo_path);
            system(cmd);
        }

        if (!tmux_session_exists(agents[i].name)) {
            // For the prototype, we just spawn a shell. In reality, it would be the CLI tool (e.g. opencode, aider)
            tmux_spawn_agent(agents[i].name, "", workspace);
            char log[256];
            snprintf(log, sizeof(log), "Spawned tmux for %s in %s", agents[i].name, workspace);
            log_message(log);
        }
    }
}

void orchestrator_tick(Agent *agents, int agent_count) {
    FossilTicket tickets[100];
    int tkt_count = fossil_ticket_list_parsed(tickets, 100);

    for (int i = 0; i < agent_count; i++) {
        Agent *a = &agents[i];
        
        if (a->state == AGENT_STATE_OPEN) {
            // Find an unassigned open ticket for this agent
            for (int t = 0; t < tkt_count; t++) {
                if (strlen(tickets[t].assignee) == 0 && 
                    (strcasecmp(tickets[t].status, "Open") == 0 || strlen(tickets[t].status) == 0)) {
                    
                    // Assign to this agent
                    fossil_ticket_assign(tickets[t].tkt_uuid, a->name);
                    fossil_ticket_set_status(tickets[t].tkt_uuid, "In Progress");
                    
                    strncpy(a->current_ticket, tickets[t].tkt_uuid, sizeof(a->current_ticket) - 1);
                    a->state = AGENT_STATE_IN_PROGRESS;
                    ticket_steps[i] = 0;
                    
                    char log[256];
                    snprintf(log, sizeof(log), "Assigned %s to %s", tickets[t].tkt_uuid, a->name);
                    log_message(log);

                    // Send initial PULSE
                    PulseMessage pmsg;
                    strcpy(pmsg.intent, "Start Ticket");
                    snprintf(pmsg.context, sizeof(pmsg.context), "Ticket %s: %s", a->current_ticket, tickets[t].title);
                    strcpy(pmsg.current_state, "New");
                    strcpy(pmsg.next_action, "Read repository and plan execution");

                    char payload[1024];
                    pulse_format(&pmsg, payload, sizeof(payload));
                    tmux_send_pulse(a->name, payload);
                    
                    break;
                }
            }
        } else if (a->state == AGENT_STATE_IN_PROGRESS) {
            a->last_heartbeat = 0; // Reset heartbeat to indicate activity tracking

            // Check if agent hit the MAX_STEPS_PER_TICKET
            if (ticket_steps[i] >= MAX_STEPS_PER_TICKET) {
                a->state = AGENT_STATE_BLOCKED;
                char log[256];
                snprintf(log, sizeof(log), "[!] %s hit MAX_STEPS (%d) on ticket %s. Blocked.", a->name, MAX_STEPS_PER_TICKET, a->current_ticket);
                log_message(log);
                
                // Inform via fossil
                fossil_ticket_set_status(a->current_ticket, "Blocked");
                continue;
            }

            // In a real scenario, we check if the agent is ready for input (e.g., waiting at shell prompt).
            // We simulate reading the pane.
            char pane_out[1024];
            if (tmux_capture_output(a->name, pane_out, sizeof(pane_out)) > 0) {
                // If agent output indicates readiness, send next PULSE
                // For demonstration, we just increment step count randomly (mocking progress)
                ticket_steps[i]++;
                
                // Only send mock pulses every few ticks to avoid flooding
                if (ticket_steps[i] % 5 == 0) {
                    PulseMessage pmsg;
                    strcpy(pmsg.intent, "Execute Next Step");
                    snprintf(pmsg.context, sizeof(pmsg.context), "Ticket %s", a->current_ticket);
                    snprintf(pmsg.current_state, sizeof(pmsg.current_state), "Step %d/%d", ticket_steps[i], MAX_STEPS_PER_TICKET);
                    strcpy(pmsg.next_action, "Analyze workspace and act");

                    char payload[1024];
                    pulse_format(&pmsg, payload, sizeof(payload));
                    tmux_send_pulse(a->name, payload);
                    
                    char log[256];
                    snprintf(log, sizeof(log), "Sent PULSE to %s for step %d", a->name, ticket_steps[i]);
                    log_message(log);
                }
            }
        } else if (a->state == AGENT_STATE_OFFLINE || a->state == AGENT_STATE_PAUSED || a->state == AGENT_STATE_BLOCKED) {
            a->last_heartbeat++;
        }
    }
}

void orchestrator_shutdown(Agent *agents, int agent_count) {
    for (int i = 0; i < agent_count; i++) {
        if (tmux_session_exists(agents[i].name)) {
            tmux_kill_agent(agents[i].name);
        }
    }
    // Finally, kill the entire session
    system("tmux kill-session -t mei 2>/dev/null");
}
