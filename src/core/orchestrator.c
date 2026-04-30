#include "core/orchestrator.h"
#include "core/tmux_mgr.h"
#include "core/fossil_skill.h"
#include "core/pulse.h"
#include "core/agent_mgr.h"
#include "core/config.h"
#include "ui.h" // For log_message integration
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to keep track of steps taken per ticket by an agent
// -1 means agent is in warm-up phase (waiting for CLI to be ready before sending first PULSE)
static int ticket_steps[MAX_AGENTS] = {0};
static int warm_up_ticks[MAX_AGENTS] = {0};

// Number of ticks to wait for CLI to warm up if no prompt is detected (safety timeout)
#define CLI_WARMUP_TIMEOUT_TICKS 15

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
            // Write a small runner script to prevent window from closing immediately on error
            char runner_path[512];
            snprintf(runner_path, sizeof(runner_path), "%s/.mei_runner.sh", workspace);
            FILE *f = fopen(runner_path, "w");
            if (f) {
                fprintf(f, "#!/bin/bash\n%s\necho \"\"\necho \"Agent process exited ($?). Press Enter to close.\"\nread\n", agents[i].cmd);
                fclose(f);
                char chmod_cmd[512];
                snprintf(chmod_cmd, sizeof(chmod_cmd), "chmod +x %s", runner_path);
                system(chmod_cmd);
                
                tmux_spawn_agent(agents[i].name, "./.mei_runner.sh", workspace);
            } else {
                // Fallback
                tmux_spawn_agent(agents[i].name, agents[i].cmd, workspace);
            }
            
            char log[256];
            snprintf(log, sizeof(log), "Spawned tmux for %s in %s", agents[i].name, workspace);
            log_message(log);
        }
    }

    FossilTicket tickets[100];
    int tkt_count = fossil_ticket_list_parsed(tickets, 100);
    for (int i = 0; i < *agent_count; i++) {
        char dbg_init[256];
        snprintf(dbg_init, sizeof(dbg_init), "[init] Agent %s hash: %s", agents[i].name, agents[i].hash);
        log_message(dbg_init);
        
        for (int t = 0; t < tkt_count; t++) {
            if (strcmp(tickets[t].assignee, agents[i].hash) != 0) continue;
            // This ticket is delegated to this agent
            strncpy(agents[i].current_ticket, tickets[t].tkt_uuid, sizeof(agents[i].current_ticket) - 1);
            if (strcasecmp(tickets[t].status, "In Progress") == 0) {
                agents[i].state = AGENT_STATE_IN_PROGRESS;
                ticket_steps[i] = -1; // Force warmup to send initial PULSE
                warm_up_ticks[i] = 0;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed IN_PROGRESS on %s", agents[i].name, tickets[t].tkt_uuid);
                log_message(log);
            } else if (strcasecmp(tickets[t].status, "Review") == 0) {
                agents[i].state = AGENT_STATE_REVIEW;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed REVIEW on %s", agents[i].name, tickets[t].tkt_uuid);
                log_message(log);
            } else if (strcasecmp(tickets[t].status, "Blocked") == 0) {
                agents[i].state = AGENT_STATE_BLOCKED;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed BLOCKED on %s", agents[i].name, tickets[t].tkt_uuid);
                log_message(log);
            }
            break; // One active ticket per agent is enough
        }
    }
}


void orchestrator_tick(Agent *agents, int agent_count) {
    FossilTicket tickets[100];
    int tkt_count = fossil_ticket_list_parsed(tickets, 100);

    for (int i = 0; i < agent_count; i++) {
        Agent *a = &agents[i];
        
        if (a->state == AGENT_STATE_OPEN) {
            // Find an unassigned open ticket, or one already delegated to this agent
            for (int t = 0; t < tkt_count; t++) {
                int is_unassigned = (strlen(tickets[t].assignee) == 0);
                int is_delegated  = (strcmp(tickets[t].assignee, a->hash) == 0);
                
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "Agent %s (%s) saw tkt %s (assignee: %s).", a->name, a->hash, tickets[t].tkt_uuid, tickets[t].assignee);
                log_message(dbg);

                int ticket_is_open        = (strcasecmp(tickets[t].status, "Open") == 0 || strlen(tickets[t].status) == 0);
                int ticket_is_in_progress = (strcasecmp(tickets[t].status, "In Progress") == 0);

                // Accept: unassigned+open, delegated+open, or delegated+in_progress (restart recovery)
                if ((is_unassigned && ticket_is_open) ||
                    (is_delegated  && (ticket_is_open || ticket_is_in_progress))) {
                    
                    // Assign to this agent if still unassigned
                    if (is_unassigned) {
                        fossil_ticket_assign(tickets[t].tkt_uuid, a->name);
                    }
                    // Ensure status is In Progress
                    if (!ticket_is_in_progress) {
                        fossil_ticket_set_status(tickets[t].tkt_uuid, "In Progress");
                    }
                    
                    strncpy(a->current_ticket, tickets[t].tkt_uuid, sizeof(a->current_ticket) - 1);
                    a->state = AGENT_STATE_IN_PROGRESS;
                    ticket_steps[i] = -1; // Enter warm-up phase: wait for CLI to be ready
                    warm_up_ticks[i] = 0;
                    
                    char log[256];
                    snprintf(log, sizeof(log), "Ticket %s picked up by %s. Waiting for CLI...", tickets[t].tkt_uuid, a->name);
                    log_message(log);
                    break;
                }
            }
        } else if (a->state == AGENT_STATE_IN_PROGRESS) {
            a->last_heartbeat = 0;

            // --- Warm-up phase: wait for CLI to be ready before sending PULSE ---
            if (ticket_steps[i] == -1) {
                warm_up_ticks[i]++;
                char pane_rdy[2048];
                int pane_len = tmux_capture_output(a->name, pane_rdy, sizeof(pane_rdy));
                
                // Check for common CLI ready indicators
                int cli_ready = 0;
                if (pane_len > 0) {
                    // opencode: shows prompt line / ask anything
                    // claude: shows > prompt
                    if (strstr(pane_rdy, "Ask anything") ||
                        strstr(pane_rdy, "ask anything") ||
                        strstr(pane_rdy, "\u276f") ||
                        strstr(pane_rdy, "> ") ||
                        strstr(pane_rdy, "$ ") ||
                        warm_up_ticks[i] >= CLI_WARMUP_TIMEOUT_TICKS) {
                        cli_ready = 1;
                    }
                } else if (warm_up_ticks[i] >= CLI_WARMUP_TIMEOUT_TICKS) {
                    cli_ready = 1;
                }
                
                if (cli_ready) {
                    // Fetch ticket info to build PULSE
                    FossilTicket tkt_info;
                    memset(&tkt_info, 0, sizeof(tkt_info));
                    strncpy(tkt_info.tkt_uuid, a->current_ticket, sizeof(tkt_info.tkt_uuid) - 1);
                    // Scan tickets array for this uuid
                    for (int t = 0; t < tkt_count; t++) {
                        if (strcmp(tickets[t].tkt_uuid, a->current_ticket) == 0) {
                            tkt_info = tickets[t];
                            break;
                        }
                    }

                    PulseMessage pmsg;
                    strcpy(pmsg.intent, "Start Ticket");
                    snprintf(pmsg.context, sizeof(pmsg.context),
                             "Ticket %s: %s\nDescription: %s",
                             a->current_ticket, tkt_info.title, tkt_info.comment);
                    strcpy(pmsg.current_state, "New");
                    strcpy(pmsg.next_action, "Read repository, plan and execute the task");

                    char payload[MEI_TEXT_BUFFER_SIZE + 512];
                    pulse_format(&pmsg, payload, sizeof(payload));
                    tmux_send_pulse(a->name, payload);

                    ticket_steps[i] = 0;
                    char log[256];
                    snprintf(log, sizeof(log), "[PULSE] Sent initial PULSE to %s after %d warmup ticks", a->name, warm_up_ticks[i]);
                    log_message(log);
                } else {
                    char log[256];
                    snprintf(log, sizeof(log), "[wait] %s CLI not ready yet (tick %d)", a->name, warm_up_ticks[i]);
                    log_message(log);
                }
                continue; // Don't execute the rest of IN_PROGRESS logic during warm-up
            }

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

            // For real agents, we should only send PULSEs when they are ready.
            // For now, let's just stop the automatic step incrementing to avoid hitting MAX_STEPS.
            // ticket_steps[i]++ was causing it to hit MAX_STEPS too fast.
            
            char pane_out[2048];
            if (tmux_capture_output(a->name, pane_out, sizeof(pane_out)) > 0) {
                // Here we could parse pane_out for specific agent responses
                // but for now we just avoid the infinite loop.
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
