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
// Set to 1 once the workspace trust dialog has been dismissed for each agent.
// Persists for the lifetime of the process (trust is remembered by the CLI per workspace).
static int trust_accepted[MAX_AGENTS] = {0};

// Number of ticks to wait for CLI to warm up if no prompt is detected (safety timeout)
#define CLI_WARMUP_TIMEOUT_TICKS 15
// Minimum ticks before any pane output is accepted as "CLI ready" (for bash/script agents
// that have no standard prompt). Must be large enough to let real CLIs finish loading their
// TUI (opencode takes ~5-8s) so that pane_len > 0 alone is not triggered prematurely.
#define CLI_WARMUP_MIN_TICKS 5

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
            int matched_hash = (strcmp(tickets[t].assignee, agents[i].hash) == 0);
            int matched_name = (strcmp(tickets[t].assignee, agents[i].name) == 0);
            if (!matched_hash && !matched_name) continue;
            strncpy(agents[i].current_ticket, tickets[t].tkt_uuid, sizeof(agents[i].current_ticket) - 1);
            if (strcasecmp(tickets[t].status, "In Progress") == 0) {
                agents[i].state = AGENT_STATE_IN_PROGRESS;
                ticket_steps[i] = -1;
                warm_up_ticks[i] = 0;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed IN_PROGRESS on %s", agents[i].name, tickets[t].tkt_uuid);
                log_message(log);
            } else if (strcasecmp(tickets[t].status, "Blocked") == 0) {
                agents[i].state = AGENT_STATE_BLOCKED;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed BLOCKED on %s", agents[i].name, tickets[t].tkt_uuid);
                log_message(log);
            }
            // Planned / Review / Rework: agent stays OPEN; role routing picks it up on first tick.
            break;
        }
    }
}


void orchestrator_tick(Agent *agents, int agent_count) {
    FossilTicket tickets[100];
    int tkt_count = fossil_ticket_list_parsed(tickets, 100);

    for (int i = 0; i < agent_count; i++) {
        Agent *a = &agents[i];
        
        if (a->state == AGENT_STATE_OPEN) {
            // Role-based ticket routing:
            //   planner  → picks up Open unassigned tickets
            //   coder    → picks up Planned or Rework tickets delegated to them
            //   reviewer → picks up Review tickets delegated to them
            // Any role resumes its own In Progress ticket after a restart.
            for (int t = 0; t < tkt_count; t++) {
                int is_delegated  = (strcmp(tickets[t].assignee, a->hash) == 0) ||
                                    (strcmp(tickets[t].assignee, a->name) == 0);

                // A ticket is "unassigned" if it has no assignee, or if its assignee
                // hash doesn't match any currently loaded agent (orphaned from old run).
                int assignee_known = 0;
                if (strlen(tickets[t].assignee) > 0) {
                    for (int j = 0; j < agent_count; j++) {
                        if (strcmp(tickets[t].assignee, agents[j].hash) == 0 ||
                            strcmp(tickets[t].assignee, agents[j].name) == 0) {
                            assignee_known = 1;
                            break;
                        }
                    }
                }
                int is_unassigned = (strlen(tickets[t].assignee) == 0) || !assignee_known;

                int tkt_open     = (strcasecmp(tickets[t].status, "Open") == 0 || strlen(tickets[t].status) == 0);
                int tkt_progress = (strcasecmp(tickets[t].status, "In Progress") == 0);
                int tkt_planned  = (strcasecmp(tickets[t].status, "Planned") == 0);
                int tkt_review   = (strcasecmp(tickets[t].status, "Review") == 0);
                int tkt_rework   = (strcasecmp(tickets[t].status, "Rework") == 0);

                int role_accepts = 0;
                if (strcmp(a->role, "planner") == 0) {
                    // Planner owns Open tickets that are not sub-tasks.
                    // Sub-tickets (comment contains [parent:...]) were already created by a
                    // previous planning cycle and belong to a specific agent — don't re-plan them.
                    int is_subtask = strstr(tickets[t].comment, "[parent:") != NULL;
                    role_accepts = tkt_open && !is_subtask;
                } else if (strcmp(a->role, "coder") == 0) {
                    int is_subtask = (strstr(tickets[t].comment, "[parent:") != NULL);
                    // Also accept re-opened sub-tickets (user manually reset to Open)
                    role_accepts = is_delegated && (tkt_planned || tkt_rework || (tkt_open && is_subtask));
                } else if (strcmp(a->role, "reviewer") == 0) {
                    role_accepts = is_delegated && tkt_review;
                } else {
                    role_accepts = is_unassigned && tkt_open;
                }
                // Restart recovery: any agent resumes its own in-progress ticket.
                int is_recovery = is_delegated && tkt_progress;

                if (!role_accepts && !is_recovery) continue;

                // Normalize assignee to this agent's hash and mark as In Progress.
                if (strcmp(tickets[t].assignee, a->hash) != 0) {
                    fossil_ticket_assign(tickets[t].tkt_uuid, a->hash);
                    // Point 1: document WHY the assignment changed.
                    char note[256];
                    snprintf(note, sizeof(note), "Assigned to %s (%s) by orchestrator routing.",
                             a->name, a->role);
                    fossil_ticket_add_note(tickets[t].tkt_uuid, note);
                }
                if (!tkt_progress) {
                    fossil_ticket_set_status(tickets[t].tkt_uuid, "In Progress");
                    // Point 1: document the status transition.
                    char note[256];
                    snprintf(note, sizeof(note),
                             "Status set to In Progress. Agent %s (%s) started work.",
                             a->name, a->role);
                    fossil_ticket_add_note(tickets[t].tkt_uuid, note);
                }

                strncpy(a->current_ticket, tickets[t].tkt_uuid, sizeof(a->current_ticket) - 1);
                a->state = AGENT_STATE_IN_PROGRESS;
                ticket_steps[i] = -1;
                warm_up_ticks[i] = 0;

                char log[256];
                snprintf(log, sizeof(log), "Ticket %s → %s (%s). Waiting for CLI...",
                         tickets[t].tkt_uuid, a->name, a->role);
                log_message(log);
                break;
            }
        } else if (a->state == AGENT_STATE_IN_PROGRESS) {
            a->last_heartbeat = 0;

            // Detect ticket completion: monitor whether the current ticket is still
            // "In Progress" with this agent's hash.  When the agent delivers its work
            // (changes status to Planned/Review/Done/etc.), it releases the ticket and
            // the orchestrator must return this agent to OPEN so the pipeline continues.
            // Skip during warm-up (ticket_steps == -1) to avoid a false positive right
            // after assignment, before the AI has had a chance to do anything.
            if (ticket_steps[i] != -1) {
                int still_active = 0;
                for (int t = 0; t < tkt_count; t++) {
                    if (strcmp(tickets[t].tkt_uuid, a->current_ticket) == 0) {
                        still_active = (strcasecmp(tickets[t].status, "In Progress") == 0) &&
                                       (strcmp(tickets[t].assignee, a->hash) == 0);
                        break;
                    }
                }
                if (!still_active) {
                    char done_log[256];
                    snprintf(done_log, sizeof(done_log),
                             "[done] %s finished ticket %s → back to OPEN",
                             a->name, a->current_ticket);
                    log_message(done_log);
                    a->state = AGENT_STATE_OPEN;
                    strcpy(a->current_ticket, "None");
                    ticket_steps[i] = 0;
                    warm_up_ticks[i] = 0;
                    continue;
                }
            }

            // --- Warm-up phase: wait for CLI to be ready before sending PULSE ---
            if (ticket_steps[i] == -1) {
                warm_up_ticks[i]++;
                char pane_rdy[2048];
                int pane_len = tmux_capture_output(a->name, pane_rdy, sizeof(pane_rdy));

                // Check for common CLI ready indicators
                int cli_ready = 0;
                if (pane_len > 0) {
                    // Dismiss the Claude Code workspace trust dialog before detecting the real prompt.
                    // The dialog shows "\u276f 1. Yes, I trust this folder" \u2014 pressing Enter accepts it.
                    // Without this, the PULSE would be swallowed by the dialog and never processed.
                    if (!trust_accepted[i]) {
                        if (strstr(pane_rdy, "Quick safety check") != NULL ||
                            strstr(pane_rdy, "I trust this folder") != NULL) {
                            tmux_send_enter(a->name);
                            trust_accepted[i] = 1;
                            char log[256];
                            snprintf(log, sizeof(log), "[trust] Accepted workspace trust for %s", a->name);
                            log_message(log);
                            continue;
                        }
                    }

                    int found_prompt = (strstr(pane_rdy, "Ask anything") != NULL) ||
                                       (strstr(pane_rdy, "ask anything") != NULL) ||
                                       (strstr(pane_rdy, "\u276f")       != NULL) ||
                                       (strstr(pane_rdy, "> ")           != NULL) ||
                                       (strstr(pane_rdy, "$ ")           != NULL);
                    // Ready if a known prompt is found, or any output appeared after
                    // the minimum wait (handles bash/script agents that have no std prompt).
                    cli_ready = found_prompt || (warm_up_ticks[i] >= CLI_WARMUP_MIN_TICKS);
                }
                if (!cli_ready && warm_up_ticks[i] >= CLI_WARMUP_TIMEOUT_TICKS) {
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

                    // Derive branch name from first 8 chars of ticket UUID
                    char branch_name[32] = {0};
                    snprintf(branch_name, sizeof(branch_name), "tkt-%.8s", a->current_ticket);

                    // Point 5: collect next-role hashes AND build agent roster for planner.
                    char coder_hash[128]    = {0};
                    char reviewer_hash[128] = {0};
                    char agent_roster[MEI_TEXT_BUFFER_SIZE] = {0};
                    for (int j = 0; j < agent_count; j++) {
                        if (strcmp(agents[j].role, "coder") == 0 && !coder_hash[0])
                            strncpy(coder_hash, agents[j].hash, sizeof(coder_hash) - 1);
                        if (strcmp(agents[j].role, "reviewer") == 0 && !reviewer_hash[0])
                            strncpy(reviewer_hash, agents[j].hash, sizeof(reviewer_hash) - 1);
                        char entry[MEI_TEXT_BUFFER_SIZE];
                        snprintf(entry, sizeof(entry),
                                 "  name: %s | role: %s | cli: %s | hash: %.8s\n"
                                 "    desc: %s\n"
                                 "    capabilities: %s\n",
                                 agents[j].name, agents[j].role, agents[j].cli, agents[j].hash,
                                 agents[j].description[0] ? agents[j].description : "(none)",
                                 agents[j].capabilities[0] ? agents[j].capabilities : "(none)");
                        strncat(agent_roster, entry,
                                sizeof(agent_roster) - strlen(agent_roster) - 1);
                    }

                    // Point 2/3: collect sub-tickets (comment contains [parent:<uuid>])
                    // and dependency references ([depends:<uuid>]) for richer context.
                    char parent_tag[72];
                    snprintf(parent_tag, sizeof(parent_tag), "[parent:%s]", a->current_ticket);
                    char subtasks_ctx[MEI_TEXT_BUFFER_SIZE]  = {0};
                    char dep_ctx[MEI_TEXT_BUFFER_SIZE]       = {0};
                    for (int t2 = 0; t2 < tkt_count; t2++) {
                        if (strstr(tickets[t2].comment, parent_tag)) {
                            char sub[256];
                            snprintf(sub, sizeof(sub), "  [sub] %s: %s (status: %s)\n",
                                     tickets[t2].tkt_uuid, tickets[t2].title,
                                     tickets[t2].status);
                            strncat(subtasks_ctx, sub,
                                    sizeof(subtasks_ctx) - strlen(subtasks_ctx) - 1);
                        }
                        char dep_tag[72];
                        snprintf(dep_tag, sizeof(dep_tag), "[depends:%s]", a->current_ticket);
                        if (strstr(tickets[t2].comment, dep_tag)) {
                            char dep[MEI_TEXT_BUFFER_SIZE];
                            snprintf(dep, sizeof(dep), "  [dep] %s: %s (status: %s)\n",
                                     tickets[t2].tkt_uuid, tickets[t2].title,
                                     tickets[t2].status);
                            strncat(dep_ctx, dep, sizeof(dep_ctx) - strlen(dep_ctx) - 1);
                        }
                    }

                    // Truncate ticket description to leave room for instructions.
                    char desc_short[MEI_TEXT_BUFFER_SIZE];
                    strncpy(desc_short, tkt_info.comment, sizeof(desc_short) - 1);
                    desc_short[sizeof(desc_short) - 1] = '\0';

                    PulseMessage pmsg;
                    if (strcmp(a->role, "planner") == 0) {
                        strcpy(pmsg.intent, "Plan, Decompose, and Delegate Ticket");
                        // Point 5: planner sees full agent roster.
                        // Point 2: planner must create sub-tickets for each task.
                        // Point 3: planner sets dependency references between sub-tickets.
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== AVAILABLE AGENTS ===\n%s\n"
                                 "=== EXISTING SUB-TICKETS (if any) ===\n%s\n"
                                 "=== YOUR TASK (PLANNER) ===\n"
                                 "1. Analyze the ticket and decompose it into concrete sub-tasks.\n"
                                 "2. For EACH sub-task create a sub-ticket in your workspace checkout:\n"
                                 "     fossil ticket add title \"<sub-task title>\" \\\n"
                                 "       comment \"[parent:%s] <sub-task description>\" \\\n"
                                 "       status \"Planned\" \\\n"
                                 "       private_contact \"%s\"\n"
                                 "   The private_contact must be the hash of the coder listed above.\n"
                                 "3. If a sub-task depends on another, add [depends:<uuid>] in its comment.\n"
                                 "4. After creating all sub-tickets, close planning on the parent:\n"
                                 "     fossil ticket set %s status \"Planned\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Coder hash: %s",
                                 a->current_ticket, tkt_info.title, desc_short,
                                 agent_roster,
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none yet)\n",
                                 a->current_ticket, coder_hash,
                                 a->current_ticket, a->current_ticket, coder_hash, coder_hash);
                        strcpy(pmsg.current_state, "Planning");
                        strcpy(pmsg.next_action,
                               "Decompose into sub-tickets, create each with fossil ticket add, then delegate parent to coder");
                    } else if (strcmp(a->role, "coder") == 0) {
                        strcpy(pmsg.intent, "Implement Ticket on Branch");
                        // Point 4: explicit code-creation and commit requirement.
                        // Point 3: show dependency and sub-ticket context.
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== RELATED SUB-TICKETS ===\n%s\n"
                                 "=== DEPENDENCY CONTEXT ===\n%s\n"
                                 "=== YOUR TASK (CODER) ===\n"
                                 "1. Create a dedicated branch:\n"
                                 "     fossil branch new %s trunk\n"
                                 "     fossil update %s\n"
                                 "2. READ the ticket and all sub-tickets listed above.\n"
                                 "3. WRITE CODE: create or modify source files to implement the task.\n"
                                 "   YOU MUST PRODUCE REAL FILE CHANGES. Planning or describing the\n"
                                 "   solution is NOT sufficient — actual code must be written.\n"
                                 "4. COMMIT your changes (required before submitting for review):\n"
                                 "     fossil commit -m \"Implement %s: %s\"\n"
                                 "5. Submit for review only AFTER committing:\n"
                                 "     fossil ticket set %s status \"Review\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Reviewer hash: %s",
                                 a->current_ticket, tkt_info.title, desc_short,
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 branch_name, branch_name,
                                 a->current_ticket, tkt_info.title,
                                 a->current_ticket, a->current_ticket, reviewer_hash, reviewer_hash);
                        strcpy(pmsg.current_state, "Coding");
                        strcpy(pmsg.next_action,
                               "Create branch, write code files, commit, then set ticket to Review");
                    } else if (strcmp(a->role, "reviewer") == 0) {
                        strcpy(pmsg.intent, "Review and Merge Branch");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== SUB-TICKETS ===\n%s\n"
                                 "=== YOUR TASK (REVIEWER) ===\n"
                                 "1. Check out the implementation branch:\n"
                                 "     fossil update %s\n"
                                 "2. Review ALL sub-tickets listed above and their committed code.\n"
                                 "3. If acceptable, merge into trunk and close:\n"
                                 "     fossil update trunk\n"
                                 "     fossil merge %s\n"
                                 "     fossil commit -m \"Merge %s: %s\"\n"
                                 "     fossil ticket set %s status \"Done\"\n"
                                 "4. If rework is needed, explain why and return to coder:\n"
                                 "     fossil ticket set %s status \"Rework\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Coder hash (for rework): %s",
                                 a->current_ticket, tkt_info.title, desc_short,
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 branch_name,
                                 branch_name, branch_name, tkt_info.title,
                                 a->current_ticket,
                                 a->current_ticket, a->current_ticket, coder_hash, coder_hash);
                        strcpy(pmsg.current_state, "Reviewing");
                        strcpy(pmsg.next_action,
                               "Check branch, verify code and sub-tickets, then merge+Done or Rework");
                    } else {
                        strcpy(pmsg.intent, "Start Ticket");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "Ticket %s: %s\nDescription: %s",
                                 a->current_ticket, tkt_info.title, desc_short);
                        strcpy(pmsg.current_state, "New");
                        strcpy(pmsg.next_action, "Read repository, plan and execute the task");
                    }

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
                snprintf(log, sizeof(log), "[!] %s hit MAX_STEPS (%d) on ticket %s. Blocked.",
                         a->name, MAX_STEPS_PER_TICKET, a->current_ticket);
                log_message(log);
                fossil_ticket_set_status(a->current_ticket, "Blocked");
                // Point 1: document reason for Blocked transition.
                char note[256];
                snprintf(note, sizeof(note),
                         "BLOCKED: agent %s reached MAX_STEPS (%d). Manual intervention required.",
                         a->name, MAX_STEPS_PER_TICKET);
                fossil_ticket_add_note(a->current_ticket, note);
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
