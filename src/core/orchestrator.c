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
                    // Researcher/catch-all: picks up unassigned Open tickets (original
                    // catch-all) AND Planned/Rework tickets explicitly delegated to it
                    // by the planner — symmetric with the coder routing.
                    role_accepts = (is_unassigned && tkt_open) ||
                                   (is_delegated && (tkt_planned || tkt_rework));
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

                // Update the in-memory snapshot so other agents in this same tick
                // don't see this ticket as available and claim it concurrently.
                strncpy(tickets[t].assignee, a->hash,      sizeof(tickets[t].assignee) - 1);
                strncpy(tickets[t].status,   "In Progress", sizeof(tickets[t].status)   - 1);

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
                char final_status[64]   = {0};
                char final_notes[4096]  = {0};
                int still_active = 0;
                for (int t = 0; t < tkt_count; t++) {
                    if (strcmp(tickets[t].tkt_uuid, a->current_ticket) == 0) {
                        still_active = (strcasecmp(tickets[t].status, "In Progress") == 0) &&
                                       (strcmp(tickets[t].assignee, a->hash) == 0);
                        strncpy(final_status, tickets[t].status,        sizeof(final_status) - 1);
                        strncpy(final_notes,  tickets[t].reviewer_notes, sizeof(final_notes)  - 1);
                        break;
                    }
                }
                if (!still_active) {
                    char done_log[256];
                    snprintf(done_log, sizeof(done_log),
                             "[done] %s finished ticket %s → back to OPEN",
                             a->name, a->current_ticket);
                    log_message(done_log);

                    // Log the outcome to the ticket's wiki page so it's visible in Fossil web.
                    char wiki_msg[1024];
                    if (final_notes[0]) {
                        snprintf(wiki_msg, sizeof(wiki_msg),
                                 "(%s) completed work. Ticket moved to **%s**. Reviewer notes: %s",
                                 a->role, final_status[0] ? final_status : "unknown", final_notes);
                    } else {
                        snprintf(wiki_msg, sizeof(wiki_msg),
                                 "(%s) completed work. Ticket moved to **%s**.",
                                 a->role, final_status[0] ? final_status : "unknown");
                    }
                    fossil_wiki_append_log(a->current_ticket, a->name, wiki_msg);

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
                        // Truncate description to 800 chars in the roster so the
                        // PULSE stays manageable when personas are long.
                        char desc_preview[820];
                        if (strlen(agents[j].description) > 800) {
                            snprintf(desc_preview, sizeof(desc_preview),
                                     "%.800s\n    [... truncated]", agents[j].description);
                        } else {
                            strncpy(desc_preview,
                                    agents[j].description[0] ? agents[j].description : "(none)",
                                    sizeof(desc_preview) - 1);
                        }
                        char entry[2048];
                        snprintf(entry, sizeof(entry),
                                 "  name: %s | role: %s | hash: %s\n"
                                 "    capabilities: %s\n"
                                 "    desc: %s\n",
                                 agents[j].name, agents[j].role, agents[j].hash,
                                 agents[j].capabilities[0] ? agents[j].capabilities : "(none)",
                                 desc_preview);
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

                    // Read accumulated discussion history from the ticket's wiki page.
                    // The orchestrator writes to this page on every status transition, so it
                    // contains a chronological log of all agent actions and reviewer rejections.
                    char discussion_log[4096] = {0};
                    fossil_ticket_read_wiki_log(a->current_ticket, discussion_log, sizeof(discussion_log));

                    // Wiki page name for this ticket (first 10 chars, same as fossil_wiki_append_log).
                    // Agents must write their reasoning here as a mandatory workflow step.
                    char wiki_page[32];
                    snprintf(wiki_page, sizeof(wiki_page), "ticket-%.10s", a->current_ticket);

                    // Reusable shell command block agents paste into their terminal to append
                    // a wiki entry.  Uses the open fossil checkout in their workspace (no -R needed).
                    // IMPORTANT: use ./ prefix so the temp file is created inside the workspace
                    // directory — opencode treats /tmp as an "external directory" requiring a
                    // permission prompt, but files inside the workspace are always allowed.
                    char wiki_cmd[512];
                    snprintf(wiki_cmd, sizeof(wiki_cmd),
                             "     TMP=$(mktemp ./.mei_wiki_XXXXXX)\n"
                             "     fossil wiki export \"%s\" \"$TMP\" 2>/dev/null || true\n"
                             "     # Append your entry to $TMP (echo, printf, or redirect a file)\n"
                             "     fossil wiki commit \"%s\" \"$TMP\" --mimetype text/x-markdown \\\n"
                             "       || fossil wiki create \"%s\" \"$TMP\" --mimetype text/x-markdown\n"
                             "     rm -f \"$TMP\"",
                             wiki_page, wiki_page, wiki_page);

                    // Build planner task instructions: if sub-tickets already exist for this
                    // parent, the planner must NOT create new ones (would cause duplicates).
                    // Instead it only needs to ensure wiki docs exist and close planning.
                    char planner_task[2048] = {0};
                    if (subtasks_ctx[0]) {
                        snprintf(planner_task, sizeof(planner_task),
                                 "*** SUB-TICKETS ALREADY EXIST — DO NOT CREATE MORE ***\n"
                                 "Creating additional sub-tickets would produce duplicates. Your task:\n\n"
                                 "1. Review the EXISTING SUB-TICKETS listed above to understand current state.\n"
                                 "2. If planning notes are absent from the wiki page \"%s\", add them now:\n"
                                 "%s\n"
                                 "   Write why this decomposition was chosen, key risks, and success criteria.\n"
                                 "3. Mark the parent ticket as Done — its work is complete once planned:\n"
                                 "     fossil ticket set %s status \"Done\"\n"
                                 "   The sub-tickets carry all remaining work. Do NOT assign the parent\n"
                                 "   to any agent — it must not be picked up for implementation.\n",
                                 wiki_page, wiki_cmd,
                                 a->current_ticket);
                    } else {
                        snprintf(planner_task, sizeof(planner_task),
                                 "1. Analyze the ticket and decompose it into concrete sub-tasks.\n"
                                 "   For each sub-task, read AVAILABLE AGENTS above and choose the\n"
                                 "   agent whose 'desc' and 'capabilities' best match the work needed.\n"
                                 "   Do NOT default every sub-task to the same agent — use the full\n"
                                 "   roster. The right agent is the one whose capabilities fit the task.\n"
                                 "2. For EACH sub-task create a sub-ticket using the chosen agent's hash:\n"
                                 "     fossil ticket add title \"<sub-task title>\" \\\n"
                                 "       comment \"[parent:%s] <sub-task description>\" \\\n"
                                 "       status \"Planned\" \\\n"
                                 "       private_contact \"<full hash from AVAILABLE AGENTS>\"\n"
                                 "   Copy the full hash exactly as shown in the AVAILABLE AGENTS list.\n"
                                 "3. If a sub-task depends on another, add [depends:<uuid>] in its comment.\n"
                                 "4. DOCUMENT your planning rationale in the ticket wiki page \"%s\" —\n"
                                 "   MANDATORY so agents understand your thinking:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Why you chose this decomposition (reasoning, not just a list)\n"
                                 "   - Which agent handles each sub-task and why that agent was chosen\n"
                                 "   - Key technical risks and open questions you identified\n"
                                 "   - Dependencies between sub-tasks and suggested execution order\n"
                                 "   - Success criteria: what each sub-task must deliver to be done\n"
                                 "   - Any assumptions you made about the requirements\n"
                                 "5. After documenting, mark the parent ticket as Done:\n"
                                 "     fossil ticket set %s status \"Done\"\n"
                                 "   The parent's job is to produce the sub-tickets. Once that is done,\n"
                                 "   it must be closed. Do NOT assign it to any agent for implementation.\n"
                                 "   All remaining work lives in the sub-tickets.\n",
                                 a->current_ticket,
                                 wiki_page, wiki_cmd,
                                 a->current_ticket);
                    }

                    // Persona header: inject the agent's own description so the LLM
                    // operates as the defined persona for every task it receives.
                    // Capped at 4096 chars — enough for rich personas, leaves ample
                    // room in the 64KB context buffer for ticket + instructions.
                    char persona_section[4160] = {0};
                    if (a->description[0]) {
                        snprintf(persona_section, sizeof(persona_section),
                                 "=== YOUR PERSONA ===\n%.4096s\n\n",
                                 a->description);
                    }

                    PulseMessage pmsg;
                    if (strcmp(a->role, "planner") == 0) {
                        strcpy(pmsg.intent, "Plan, Decompose, and Delegate Ticket");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== DISCUSSION HISTORY ===\n%s\n\n"
                                 "=== AVAILABLE AGENTS ===\n%s\n"
                                 "=== EXISTING SUB-TICKETS (if any) ===\n%s\n"
                                 "=== YOUR TASK (PLANNER) ===\n"
                                 "%s",
                                 persona_section,
                                 a->current_ticket, tkt_info.title, desc_short,
                                 discussion_log[0] ? discussion_log : "  (no history yet)\n",
                                 agent_roster,
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none yet)\n",
                                 planner_task);
                        strcpy(pmsg.current_state, "Planning");
                        strcpy(pmsg.next_action,
                               "Decompose into sub-tickets, create each with fossil ticket add, then delegate parent to coder");
                    } else if (strcmp(a->role, "coder") == 0) {
                        strcpy(pmsg.intent, "Implement Ticket on Branch");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== DISCUSSION HISTORY (all past agent activity on this ticket) ===\n%s\n\n"
                                 "=== LATEST REVIEWER FEEDBACK ===\n%s\n\n"
                                 "=== RELATED SUB-TICKETS ===\n%s\n"
                                 "=== DEPENDENCY CONTEXT ===\n%s\n"
                                 "=== YOUR TASK (CODER) ===\n"
                                 "1. READ the discussion history above carefully before starting — it contains\n"
                                 "   all prior reviewer rejections with their exact reasons. Address EVERY\n"
                                 "   issue raised in previous cycles, not just the latest one.\n"
                                 "2. Create a dedicated branch:\n"
                                 "     fossil branch new %s trunk\n"
                                 "     fossil update %s\n"
                                 "3. READ the ticket description carefully. Implement EXACTLY what is asked.\n"
                                 "   Do NOT use your own name, agent name, or placeholder values anywhere\n"
                                 "   in the code (e.g. module names, package names, comments).\n"
                                 "   Use names derived from the ticket title and project context.\n"
                                 "4. WRITE COMPLETE CODE — not stubs, not Hello World unless the ticket\n"
                                 "   explicitly asks for a Hello World. Every function must be implemented.\n"
                                 "   The code must compile and run without errors.\n"
                                 "5. VERIFY before submitting: build and run the code to confirm it works.\n"
                                 "6. COMMIT your changes:\n"
                                 "     fossil commit -m \"Implement %s: %s\"\n"
                                 "7. DOCUMENT your implementation in the ticket wiki page \"%s\" — MANDATORY\n"
                                 "   before submitting. The reviewer and future agents will read this:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Approach taken and why (not just what, but why this design)\n"
                                 "   - Key decisions and alternatives you considered and discarded\n"
                                 "   - Exact commands used to build and test, and their output/result\n"
                                 "   - Known limitations, assumptions, or technical debt introduced\n"
                                 "   - If this is a rework: what specifically changed from the previous attempt\n"
                                 "8. Submit for review only AFTER documenting:\n"
                                 "     fossil ticket set %s status \"Review\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Reviewer hash: %s",
                                 persona_section,
                                 a->current_ticket, tkt_info.title, desc_short,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first attempt)\n",
                                 tkt_info.reviewer_notes[0] ? tkt_info.reviewer_notes : "(none - first attempt)",
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 branch_name, branch_name,
                                 a->current_ticket, tkt_info.title,
                                 wiki_page, wiki_cmd,
                                 a->current_ticket, a->current_ticket, reviewer_hash, reviewer_hash);
                        strcpy(pmsg.current_state, "Coding");
                        strcpy(pmsg.next_action,
                               "Create branch, implement fully, build+verify, commit, then set ticket to Review");
                    } else if (strcmp(a->role, "reviewer") == 0) {
                        strcpy(pmsg.intent, "Review, Verify, and Merge or Reject");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== DISCUSSION HISTORY (all prior review cycles on this ticket) ===\n%s\n\n"
                                 "=== SUB-TICKETS ===\n%s\n"
                                 "=== YOUR TASK (REVIEWER) — BE STRICT ===\n"
                                 "1. READ the discussion history above — if there were prior rejections, verify\n"
                                 "   that EVERY previously reported issue has been fully addressed.\n"
                                 "2. Check out the implementation branch:\n"
                                 "     fossil update %s\n"
                                 "3. Read EVERY file that was changed. Use `fossil diff --from trunk` to\n"
                                 "   see exactly what was added or modified.\n"
                                 "4. Verify EACH requirement in the ticket description is fully satisfied:\n"
                                 "   - If the ticket asks for specific files, check they exist and are non-trivial.\n"
                                 "   - If the ticket asks for working code, BUILD and RUN it (e.g. `go build ./...`,\n"
                                 "     `go vet ./...`, run tests if present).\n"
                                 "   - Reject placeholder/stub code (e.g. empty functions, Hello World where real\n"
                                 "     logic was expected, hardcoded values, TODO comments left in).\n"
                                 "   - Reject if module/package names are nonsensical (agent names, temp names).\n"
                                 "5. DOCUMENT your full review findings in the ticket wiki page \"%s\" — MANDATORY\n"
                                 "   before taking any action. Future agents depend on this record:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Every file you reviewed and what you found\n"
                                 "   - Every build/test command run and the exact output (pass/fail)\n"
                                 "   - Whether each requirement in the ticket spec was met (yes/no + evidence)\n"
                                 "   - VERDICT: APPROVED or REJECTED\n"
                                 "   - If REJECTED: specific issues with file names, line numbers, exact problems\n"
                                 "6. If ALL requirements met — merge into trunk AFTER documenting:\n"
                                 "     fossil update trunk\n"
                                 "     fossil merge %s\n"
                                 "     fossil commit -m \"Merge %s: %s\"\n"
                                 "     fossil ticket set %s status \"Done\"\n"
                                 "7. If ANYTHING is incomplete or wrong — AFTER documenting in wiki:\n"
                                 "   Record the summary rejection reason (coder will read this):\n"
                                 "     fossil ticket set %s reviewer_notes \"REJECTION: <summary of issues>\"\n"
                                 "   Return for rework:\n"
                                 "     fossil ticket set %s status \"Rework\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Coder hash (for rework): %s\n"
                                 "REMEMBER: approving bad code harms the project. When in doubt, reject.",
                                 persona_section,
                                 a->current_ticket, tkt_info.title, desc_short,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first review)\n",
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 branch_name,
                                 wiki_page, wiki_cmd,
                                 branch_name, branch_name, tkt_info.title,
                                 a->current_ticket,
                                 a->current_ticket,
                                 a->current_ticket, a->current_ticket, coder_hash, coder_hash);
                        strcpy(pmsg.current_state, "Reviewing");
                        strcpy(pmsg.next_action,
                               "Read all changes, build/run code, verify every requirement — merge only if fully satisfied, otherwise Rework");
                    } else {
                        strcpy(pmsg.intent, "Research, Analyze, and Document Findings");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "UUID: %s\nTitle: %s\nDescription:\n%s\n\n"
                                 "=== DISCUSSION HISTORY ===\n%s\n\n"
                                 "=== DEPENDENCY CONTEXT ===\n%s\n"
                                 "=== YOUR TASK (RESEARCHER) ===\n"
                                 "1. READ the ticket description and discussion history carefully.\n"
                                 "   Understand exactly what deliverables are required.\n"
                                 "2. RESEARCH thoroughly — use all available tools:\n"
                                 "   - Read files in the repository (fossil update trunk first)\n"
                                 "   - Search documentation, run commands, inspect data\n"
                                 "   - Gather ALL information the ticket asks for\n"
                                 "3. PRODUCE the required deliverables as files in your workspace:\n"
                                 "   - Write markdown documents, Python scripts, data files as needed\n"
                                 "   - Every deliverable must be non-trivial and complete\n"
                                 "   - Use meaningful names derived from the ticket title\n"
                                 "4. COMMIT your deliverables:\n"
                                 "     fossil branch new %s trunk\n"
                                 "     fossil update %s\n"
                                 "     fossil commit -m \"Research %s: %s\"\n"
                                 "5. DOCUMENT your findings in the ticket wiki page \"%s\" — MANDATORY:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Summary of what was researched and key findings\n"
                                 "   - Every source consulted and what was learned from each\n"
                                 "   - Files produced: name, purpose, and key contents\n"
                                 "   - Open questions or limitations in the findings\n"
                                 "6. Submit for review AFTER documenting:\n"
                                 "     fossil ticket set %s status \"Review\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Reviewer hash: %s",
                                 persona_section,
                                 a->current_ticket, tkt_info.title, desc_short,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first attempt)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 branch_name, branch_name,
                                 a->current_ticket, tkt_info.title,
                                 wiki_page, wiki_cmd,
                                 a->current_ticket, a->current_ticket, reviewer_hash, reviewer_hash);
                        strcpy(pmsg.current_state, "Researching");
                        strcpy(pmsg.next_action,
                               "Research thoroughly, produce deliverable files, commit, document in wiki, then set ticket to Review");
                    }

                    char payload[MEI_TEXT_BUFFER_SIZE + 512];
                    pulse_format(&pmsg, payload, sizeof(payload));
                    bool pulse_ok = tmux_send_pulse(a->name, payload);

                    ticket_steps[i] = 0;
                    char log[256];
                    if (pulse_ok) {
                        snprintf(log, sizeof(log), "[PULSE] Sent initial PULSE to %s after %d warmup ticks", a->name, warm_up_ticks[i]);
                    } else {
                        snprintf(log, sizeof(log), "[PULSE] FAILED to send PULSE to %s — tmux paste error (check /tmp/tmux_err.log)", a->name);
                    }
                    log_message(log);

                    // Record the dispatch in the ticket's wiki page for human visibility.
                    char wiki_msg[512];
                    snprintf(wiki_msg, sizeof(wiki_msg),
                             "(%s) received task — %s", a->role, pmsg.next_action);
                    fossil_wiki_append_log(a->current_ticket, a->name, wiki_msg);
                } else {
                    char log[256];
                    snprintf(log, sizeof(log), "[wait] %s CLI not ready yet (tick %d)", a->name, warm_up_ticks[i]);
                    log_message(log);
                }
                continue; // Don't execute the rest of IN_PROGRESS logic during warm-up
            }

            // Count active ticks so a stalled agent is eventually unblocked.
            ticket_steps[i]++;

            // Stall detection: if the agent has been In Progress for MAX_STEPS ticks
            // without completing, declare it blocked and record the failure.
            if (ticket_steps[i] >= MAX_STEPS_PER_TICKET) {
                a->state = AGENT_STATE_BLOCKED;
                char log[256];
                snprintf(log, sizeof(log), "[!] %s hit MAX_STEPS (%d) on ticket %s. Blocked.",
                         a->name, MAX_STEPS_PER_TICKET, a->current_ticket);
                log_message(log);
                fossil_ticket_set_status(a->current_ticket, "Blocked");
                char note[256];
                snprintf(note, sizeof(note),
                         "BLOCKED: agent %s stalled after %d ticks (~%d min). Manual intervention required.",
                         a->name, MAX_STEPS_PER_TICKET, (MAX_STEPS_PER_TICKET * TICK_INTERVAL_MS) / 60000);
                fossil_ticket_add_note(a->current_ticket, note);
                char wiki_msg[256];
                snprintf(wiki_msg, sizeof(wiki_msg),
                         "(%s) **STALLED** after %d ticks (~%d min) without completing. Ticket set to Blocked — human intervention required.",
                         a->role, MAX_STEPS_PER_TICKET, (MAX_STEPS_PER_TICKET * TICK_INTERVAL_MS) / 60000);
                fossil_wiki_append_log(a->current_ticket, a->name, wiki_msg);
                continue;
            }

            // Monitor the pane for tool-use permission dialogs (opencode shows
            // "Permission required" / "Allow once" when it wants to access files
            // or run commands outside the workspace).  Auto-accept with Enter so
            // the pipeline isn't blocked waiting for a human.
            char pane_out[4096];
            if (tmux_capture_output(a->name, pane_out, sizeof(pane_out)) > 0) {
                if (strstr(pane_out, "Permission required") != NULL ||
                    strstr(pane_out, "Allow once") != NULL) {
                    tmux_send_enter(a->name);
                    char perm_log[128];
                    snprintf(perm_log, sizeof(perm_log),
                             "[perm] Auto-accepted permission dialog for %s", a->name);
                    log_message(perm_log);
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
