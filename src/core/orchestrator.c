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
// Pane inactivity tracking: hash of last captured pane content and how many
// consecutive ticks it has been unchanged. CLI-agnostic idle detection.
static unsigned long last_pane_hash[MAX_AGENTS] = {0};
static int pane_idle_ticks[MAX_AGENTS] = {0};

// djb2 hash — fast, no dependencies, good enough for change detection.
static unsigned long hash_pane(const char *s, int len) {
    unsigned long h = 5381;
    for (int i = 0; i < len; i++)
        h = ((h << 5) + h) ^ (unsigned char)s[i];
    return h;
}

// Number of ticks to wait for CLI to warm up if no prompt is detected (safety timeout)
#define CLI_WARMUP_TIMEOUT_TICKS 15
// Minimum ticks before any pane output is accepted as "CLI ready" (for bash/script agents
// that have no standard prompt). Must be large enough to let real CLIs finish loading their
// TUI (opencode takes ~5-8s) so that pane_len > 0 alone is not triggered prematurely.
#define CLI_WARMUP_MIN_TICKS 5
// Idle-nudge: send a follow-up when the pane has been static for NUDGE_IDLE_TICKS
// consecutive ticks while the ticket is still In Progress. Repeat every NUDGE_REPEAT_TICKS.
// Completely CLI-agnostic — detects inactivity, not specific strings.
#define NUDGE_MIN_TICKS         15   // ignore first ~30s (agent may still be generating)
#define NUDGE_IDLE_TICKS        30   // pane must be static for ~60s before nudging
#define NUDGE_REPEAT_TICKS      60   // re-nudge every ~120s while still idle
// Planner uses a much higher idle threshold: planners do multi-step reasoning where
// the LLM finishes one analytical turn and pauses before executing commands.
// 30 ticks (~1 min) gives enough room for generation without leaving it stuck forever.
#define NUDGE_IDLE_TICKS_PLANNER  30  // ~1 min idle before nudging planner

// Check whether all [depends:uuid] tags in a ticket comment point to tickets
// that are Done. Tickets not found in the active array are assumed Done (they
// were filtered out of the query because they are already closed).
static int deps_satisfied(FossilTicket *tickets, int tkt_count, const char *comment) {
    const char *p = comment;
    while ((p = strstr(p, "[depends:")) != NULL) {
        p += 9;
        const char *end = strchr(p, ']');
        if (!end) break;
        char dep_uuid[64] = {0};
        size_t len = (size_t)(end - p);
        if (len == 0 || len >= sizeof(dep_uuid)) { p = end + 1; continue; }
        strncpy(dep_uuid, p, len);
        // Search for the dependency in the active tickets array.
        for (int t = 0; t < tkt_count; t++) {
            if (strncmp(tickets[t].tkt_uuid, dep_uuid, len) == 0) {
                // Found — it is still open. Satisfied if Done or closed.
                if (strcasecmp(tickets[t].status, "Done")   != 0 &&
                    strcasecmp(tickets[t].status, "closed") != 0) return 0;
                break;
            }
        }
        // Not found in active tickets → Done/Closed (filtered by the SQL query).
        p = end + 1;
    }
    return 1;
}

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
    // Large ticket array lives in BSS (static) to avoid blowing the tick thread's
    // stack. orchestrator_tick is always called from a single thread sequentially,
    // so static storage is safe.
    static FossilTicket tickets[100];
    static int tick_counter = 0;
    tick_counter++;
    int tkt_count = fossil_ticket_list_parsed(tickets, 100);

    for (int i = 0; i < agent_count; i++) {
        Agent *a = &agents[i];
        a->step_count = (ticket_steps[i] > 0) ? ticket_steps[i] : 0;

        if (a->state == AGENT_STATE_OPEN) {
            // Clear pending_review_ticket only when the review cycle has ended:
            // Done (approved), Rework (rejected), Planned (planner unblocked), or closed.
            // "In Progress" means the reviewer is actively working — keep the gate.
            if (a->pending_review_ticket[0] != '\0') {
                for (int t = 0; t < tkt_count; t++) {
                    if (strcmp(tickets[t].tkt_uuid, a->pending_review_ticket) == 0) {
                        int cycle_ended = (strcasecmp(tickets[t].status, "Done")    == 0 ||
                                           strcasecmp(tickets[t].status, "Rework")  == 0 ||
                                           strcasecmp(tickets[t].status, "closed")  == 0 ||
                                           strcasecmp(tickets[t].status, "Planned") == 0);
                        if (cycle_ended) {
                            char pr_log[256];
                            snprintf(pr_log, sizeof(pr_log),
                                     "[review-gate] %s: review cycle ended (%s), agent unblocked",
                                     a->name, tickets[t].status);
                            log_message(pr_log);
                            a->pending_review_ticket[0] = '\0';
                        }
                        break;
                    }
                }
            }

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
                int tkt_blocked  = (strcasecmp(tickets[t].status, "Blocked") == 0);

                int role_accepts = 0;
                if (strcmp(a->role, "planner") == 0) {
                    // Planner owns Open tickets (not sub-tasks) and any Blocked ticket
                    // where an executor got stuck and needs a decision.
                    int is_subtask = strstr(tickets[t].comment, "[parent:") != NULL;
                    role_accepts = (tkt_open && !is_subtask) || tkt_blocked;
                } else if (strcmp(a->role, "coder") == 0) {
                    int is_subtask = (strstr(tickets[t].comment, "[parent:") != NULL);
                    // Also accept re-opened sub-tickets (user manually reset to Open)
                    // Also accept Review tickets when explicitly delegated by planner.
                    role_accepts = is_delegated && (tkt_planned || tkt_rework || tkt_review || (tkt_open && is_subtask));
                    // Review gate: don't pick up new work while a prior submission is still
                    // under review. Exception: the same ticket returned as Rework (the coder
                    // must be able to act on reviewer feedback for its own submission).
                    if (role_accepts && a->pending_review_ticket[0] != '\0') {
                        int is_own_rework = (strcmp(tickets[t].tkt_uuid, a->pending_review_ticket) == 0);
                        if (!is_own_rework) role_accepts = 0;
                    }
                } else if (strcmp(a->role, "reviewer") == 0) {
                    // Two paths for a reviewer:
                    // 1. Any Review-status ticket not assigned to another known agent
                    //    (handles unassigned or directly delegated tickets).
                    //    If the ticket is assigned to a coder/researcher for review, they handle it.
                    // 2. Planned/Rework tickets explicitly delegated to this reviewer by the planner.
                    int assigned_to_other = !is_delegated && assignee_known;
                    role_accepts = (tkt_review && !assigned_to_other) ||
                                   (is_delegated && (tkt_planned || tkt_rework));
                } else {
                    // Researcher/catch-all: picks up unassigned Open tickets (original
                    // catch-all) AND Planned/Rework/Review tickets explicitly delegated
                    // to it by the planner — symmetric with the coder routing.
                    role_accepts = (is_unassigned && tkt_open) ||
                                   (is_delegated && (tkt_planned || tkt_rework || tkt_review));
                }
                // Restart recovery: any agent resumes its own in-progress ticket.
                int is_recovery = is_delegated && tkt_progress;

                // Dependency gate: executor agents may not start a ticket until all
                // [depends:uuid] entries in its comment are Done. Recovery (resuming
                // an already in-progress ticket) bypasses this — work was already started.
                if (role_accepts && !is_recovery &&
                    strcmp(a->role, "planner")  != 0 &&
                    strcmp(a->role, "reviewer") != 0) {
                    if (!deps_satisfied(tickets, tkt_count, tickets[t].comment)) {
                        role_accepts = 0;
                        // Rate-limit dep-gate log: one entry per ticket per 30 ticks (~60s).
                        // Without this, 7 blocked tickets × every tick = 210 log lines/minute.
                        static char dep_gate_logged_uuid[100][41];
                        static int  dep_gate_logged_tick[100];
                        static int  dep_gate_slots = 0;
                        int slot = -1;
                        for (int s = 0; s < dep_gate_slots; s++) {
                            if (strncmp(dep_gate_logged_uuid[s], tickets[t].tkt_uuid, 40) == 0) {
                                slot = s; break;
                            }
                        }
                        if (slot < 0 && dep_gate_slots < 100) {
                            slot = dep_gate_slots++;
                            strncpy(dep_gate_logged_uuid[slot], tickets[t].tkt_uuid, 40);
                            dep_gate_logged_tick[slot] = tick_counter - 30;
                        }
                        if (slot >= 0 && tick_counter - dep_gate_logged_tick[slot] >= 30) {
                            dep_gate_logged_tick[slot] = tick_counter;
                            char dep_log[256];
                            snprintf(dep_log, sizeof(dep_log),
                                     "[dep-gate] %s skipping ticket %.10s — dependency not Done",
                                     a->name, tickets[t].tkt_uuid);
                            log_message(dep_log);
                        }
                    }
                }

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
                a->resolving_block = tkt_blocked;
                a->doing_review    = tkt_review && (strcmp(a->role, "reviewer") != 0);
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
            if (ticket_steps[i] >= 1) {
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

                    // When a non-reviewer agent submits work for review (sets ticket to
                    // Review status), clear the assignee so the reviewer role can pick it
                    // up freely. Without this, the ticket stays assigned to the coder/
                    // researcher and the reviewer would skip it (assigned_to_other = true).
                    if (strcasecmp(final_status, "Review") == 0 &&
                        strcmp(a->role, "reviewer") != 0) {
                        fossil_ticket_assign(a->current_ticket, "");
                        // Update in-memory snapshot so routing this same tick is correct.
                        for (int t = 0; t < tkt_count; t++) {
                            if (strcmp(tickets[t].tkt_uuid, a->current_ticket) == 0) {
                                tickets[t].assignee[0] = '\0';
                                break;
                            }
                        }
                        // Track the pending review so the agent won't accept new work
                        // until this review cycle completes (Done or Rework).
                        strncpy(a->pending_review_ticket, a->current_ticket,
                                sizeof(a->pending_review_ticket) - 1);
                        a->pending_review_ticket[sizeof(a->pending_review_ticket) - 1] = '\0';
                        log_message("[review] Cleared assignee — ticket submitted for review");
                    }

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
                    a->doing_review = 0;
                    strcpy(a->current_ticket, "None");
                    ticket_steps[i] = 0;
                    warm_up_ticks[i] = 0;
                    pane_idle_ticks[i] = 0;
                    last_pane_hash[i] = 0;
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
                    // Branch name is ticket-scoped and used for both workspace setup
                    // and PULSE construction — declare it once here.
                    char branch_name[32] = {0};
                    snprintf(branch_name, sizeof(branch_name),
                             "tkt-%.8s", a->current_ticket);

                    // For implementation roles, create the ticket branch in the
                    // repository and switch the workspace to it before sending the
                    // PULSE. The agent only needs to run `fossil update <branch>` —
                    // it never creates branches. This prevents ghost branches caused
                    // by agents committing to trunk after a failed branch creation.
                    if (strcmp(a->role, "coder") == 0 ||
                        strcmp(a->role, "researcher") == 0) {
                        char branch_cmd[512];
                        snprintf(branch_cmd, sizeof(branch_cmd),
                                 "cd /tmp/workspaces/%s && "
                                 "fossil branch new %s trunk >/dev/null 2>&1 || true && "
                                 "fossil update %s >/dev/null 2>&1",
                                 a->name, branch_name, branch_name);
                        system(branch_cmd);
                        char branch_log[128];
                        snprintf(branch_log, sizeof(branch_log),
                                 "[branch] Created/switched workspace %s → %s",
                                 a->name, branch_name);
                        log_message(branch_log);
                    } else {
                        // Planner/reviewer: sync to trunk as before.
                        char upd_cmd[512];
                        snprintf(upd_cmd, sizeof(upd_cmd),
                                 "cd /tmp/workspaces/%s && fossil update trunk >/dev/null 2>&1",
                                 a->name);
                        system(upd_cmd);
                    }
                    char upd_log[128];
                    snprintf(upd_log, sizeof(upd_log),
                             "[sync] Workspace updated for %s before PULSE", a->name);
                    log_message(upd_log);

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

                    // Point 5: collect next-role hashes AND build agent roster for planner.
                    // Also build the pending-reviews summary so the planner can sequence
                    // work knowing which agents are waiting for review to complete.
                    char coder_hash[128]    = {0};
                    char reviewer_hash[128] = {0};
                    static char agent_roster[MEI_TEXT_BUFFER_SIZE];
                    static char pending_reviews_ctx[4096];
                    memset(agent_roster, 0, sizeof(agent_roster));
                    memset(pending_reviews_ctx, 0, sizeof(pending_reviews_ctx));
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

                        if (agents[j].pending_review_ticket[0] != '\0') {
                            const char *rev_title  = "(unknown)";
                            const char *rev_status = "Review";
                            for (int t2 = 0; t2 < tkt_count; t2++) {
                                if (strcmp(tickets[t2].tkt_uuid,
                                           agents[j].pending_review_ticket) == 0) {
                                    rev_title  = tickets[t2].title;
                                    rev_status = tickets[t2].status;
                                    break;
                                }
                            }
                            char pr_entry[512];
                            snprintf(pr_entry, sizeof(pr_entry),
                                     "  %s (%s): ticket %.10s (\"%s\") — status: %s\n"
                                     "    → blocked from new assignments until review resolves\n",
                                     agents[j].name, agents[j].role,
                                     agents[j].pending_review_ticket, rev_title, rev_status);
                            strncat(pending_reviews_ctx, pr_entry,
                                    sizeof(pending_reviews_ctx) - strlen(pending_reviews_ctx) - 1);
                        }
                    }
                    if (!pending_reviews_ctx[0])
                        strncpy(pending_reviews_ctx, "  (none — all agents free to accept new work)\n",
                                sizeof(pending_reviews_ctx) - 1);

                    // Point 2/3: collect sub-tickets (comment contains [parent:<uuid>])
                    // and dependency references ([depends:<uuid>]) for richer context.
                    char parent_tag[72];
                    snprintf(parent_tag, sizeof(parent_tag), "[parent:%s]", a->current_ticket);
                    static char subtasks_ctx[MEI_TEXT_BUFFER_SIZE];
                    static char dep_ctx[MEI_TEXT_BUFFER_SIZE];
                    memset(subtasks_ctx, 0, sizeof(subtasks_ctx));
                    memset(dep_ctx, 0, sizeof(dep_ctx));
                    for (int t2 = 0; t2 < tkt_count; t2++) {
                        if (strstr(tickets[t2].comment, parent_tag)) {
                            char sub[768];
                            snprintf(sub, sizeof(sub),
                                     "  [sub] %s | %s | status: %s\n"
                                     "        desc: %.500s\n",
                                     tickets[t2].tkt_uuid, tickets[t2].title,
                                     tickets[t2].status, tickets[t2].comment);
                            strncat(subtasks_ctx, sub,
                                    sizeof(subtasks_ctx) - strlen(subtasks_ctx) - 1);
                        }
                        char dep_tag[72];
                        snprintf(dep_tag, sizeof(dep_tag), "[depends:%s]", a->current_ticket);
                        if (strstr(tickets[t2].comment, dep_tag)) {
                            char dep[768];
                            snprintf(dep, sizeof(dep),
                                     "  [dep] %s | %s | status: %s\n"
                                     "        desc: %.500s\n",
                                     tickets[t2].tkt_uuid, tickets[t2].title,
                                     tickets[t2].status, tickets[t2].comment);
                            strncat(dep_ctx, dep, sizeof(dep_ctx) - strlen(dep_ctx) - 1);
                        }
                    }

                    // Get the ticket description. Agent-created tickets use the comment
                    // field; web-UI tickets store it as icomment in the creation artifact.
                    char tkt_description[2048] = {0};
                    if (tkt_info.comment[0]) {
                        strncpy(tkt_description, tkt_info.comment, sizeof(tkt_description) - 1);
                    } else {
                        fossil_ticket_read_icomment_from_artifact(
                            a->current_ticket, tkt_description, sizeof(tkt_description));
                    }

                    char tkt_full[4096];
                    snprintf(tkt_full, sizeof(tkt_full),
                             "uuid:     %s\ntitle:    %s\nstatus:   %s\nassignee: %s\n\n"
                             "--- DESCRIPTION ---\n%s\n\n"
                             "--- REVIEWER NOTES ---\n%s",
                             tkt_info.tkt_uuid, tkt_info.title, tkt_info.status,
                             tkt_info.assignee,
                             tkt_description[0] ? tkt_description : "(no description in ticket)",
                             tkt_info.reviewer_notes[0] ? tkt_info.reviewer_notes : "(none)");

                    // Read accumulated discussion history from the ticket's wiki page.
                    char discussion_log[4096] = {0};
                    fossil_ticket_read_wiki_log(a->current_ticket, discussion_log, sizeof(discussion_log));

                    // Parent ticket context: when this is a sub-ticket ([parent:UUID] in comment),
                    // include the parent's full description and planning wiki.
                    char parent_uuid[72] = {0};
                    const char *pp = strstr(tkt_info.comment, "[parent:");
                    if (pp) {
                        const char *start = pp + 8;
                        const char *end   = strchr(start, ']');
                        if (end && (size_t)(end - start) < sizeof(parent_uuid) - 1)
                            strncpy(parent_uuid, start, end - start);
                    }
                    char parent_ctx[4096] = {0};
                    if (parent_uuid[0]) {
                        // Find parent in the already-loaded tickets array
                        FossilTicket *parent_tkt = NULL;
                        for (int t2 = 0; t2 < tkt_count; t2++) {
                            if (strncmp(tickets[t2].tkt_uuid, parent_uuid,
                                        strlen(parent_uuid)) == 0) {
                                parent_tkt = &tickets[t2];
                                break;
                            }
                        }
                        char parent_desc[2048] = {0};
                        if (parent_tkt && parent_tkt->comment[0]) {
                            strncpy(parent_desc, parent_tkt->comment, sizeof(parent_desc) - 1);
                        } else {
                            fossil_ticket_read_icomment_from_artifact(
                                parent_uuid, parent_desc, sizeof(parent_desc));
                        }
                        char parent_wiki[2048] = {0};
                        fossil_ticket_read_wiki_log(parent_uuid, parent_wiki, sizeof(parent_wiki));
                        snprintf(parent_ctx, sizeof(parent_ctx),
                                 "--- PARENT TICKET ---\n"
                                 "uuid:  %s\ntitle: %s\nstatus: %s\n\n"
                                 "--- DESCRIPTION ---\n%s\n\n"
                                 "--- PARENT PLANNING NOTES (wiki) ---\n%s\n",
                                 parent_uuid,
                                 parent_tkt ? parent_tkt->title  : "(unknown)",
                                 parent_tkt ? parent_tkt->status : "(unknown)",
                                 parent_desc[0] ? parent_desc : "(no description)",
                                 parent_wiki[0] ? parent_wiki : "(no wiki yet)");
                    }

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

                    // Build planner task instructions.
                    char planner_task[2048] = {0};
                    if (a->resolving_block) {
                        snprintf(planner_task, sizeof(planner_task),
                                 "*** THIS TICKET IS BLOCKED — YOUR ROLE IS UNLOCKER, NOT PLANNER ***\n\n"
                                 "An executor agent recorded a blocker or question it could not resolve.\n"
                                 "READ the DISCUSSION HISTORY above to understand what stopped the agent.\n\n"
                                 "1. IDENTIFY the blocker: find the agent's question or stall reason\n"
                                 "   in the discussion history or ticket changelog.\n"
                                 "2. DECIDE autonomously — you have full authority. Do not ask for human\n"
                                 "   input unless the blocker is a missing external resource (credentials,\n"
                                 "   access rights) that you genuinely cannot resolve yourself.\n"
                                 "3. DOCUMENT your decision in the wiki page \"%s\":\n"
                                 "%s\n"
                                 "   Write: what the blocker was, your decision, and the rationale.\n"
                                 "4. RESET the ticket so the original agent can resume:\n"
                                 "     fossil ticket set %s status \"Planned\"\n"
                                 "   Do NOT change private_contact — the original assignee picks it up.\n"
                                 "5. If truly unresolvable without human input, leave status as \"Blocked\"\n"
                                 "   and document \"ESCALATION: <exact reason>\" in the wiki.\n"
                                 "!! DO NOT create sub-tickets. DO NOT reassign the ticket.\n",
                                 wiki_page, wiki_cmd,
                                 a->current_ticket);
                    } else if (subtasks_ctx[0]) {
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
                                 "!! EXECUTE IMMEDIATELY — do NOT describe what you will do.\n"
                                 "!! Do NOT output 'Próximos Passos' or any roadmap narration.\n"
                                 "!! Run the fossil commands now, in this response.\n\n"
                                 "STEP 1 — CREATE SUB-TICKETS NOW:\n"
                                 "For each sub-task, pick the agent from AVAILABLE AGENTS whose\n"
                                 "capabilities best match. Do NOT default everything to one agent.\n"
                                 "YOUR OWN HASH IS %s — NEVER assign a sub-ticket to yourself.\n"
                                 "Executors are: coder, researcher, reviewer.\n\n"
                                 "Run this command once per sub-task:\n"
                                 "  fossil ticket add title \"<sub-task title>\" \\\n"
                                 "    comment \"[parent:%s] <sub-task description>\" \\\n"
                                 "    status \"Planned\" \\\n"
                                 "    private_contact \"<full agent hash from AVAILABLE AGENTS>\"\n\n"
                                 "SEQUENCING — if sub-task B must wait for sub-task A, add to B's comment:\n"
                                 "  [depends:<UUID that fossil printed for A>]\n"
                                 "  UUID = exact 40-char hex from the 'fossil ticket add' output.\n"
                                 "  NEVER write [depends:T1] or any placeholder. Only real UUIDs work.\n"
                                 "  When unsure, make tasks sequential — it avoids wasted work.\n\n"
                                 "STEP 2 — DOCUMENT in wiki page \"%s\":\n"
                                 "%s\n"
                                 "  - Why this decomposition, which agent per sub-task and why\n"
                                 "  - Dependencies and execution order\n"
                                 "  - Key risks and success criteria\n\n"
                                 "STEP 3 — CLOSE the parent ticket:\n"
                                 "  fossil ticket set %s status \"Done\"\n"
                                 "The parent's job is done once sub-tickets exist. Do NOT assign it\n"
                                 "to any agent for implementation — all work lives in sub-tickets.\n",
                                 a->hash, a->current_ticket,
                                 wiki_page, wiki_cmd,
                                 a->current_ticket);
                    }

                    // Persona header: inject the agent's own description so the LLM
                    // operates as the defined persona for every task it receives.
                    // Executor agents also receive the DECISION PROTOCOL so they never
                    // stall waiting for input — they decide, document, and proceed.
                    // Buffer sized for persona (≤4096) + protocol (≤700) + headers.
                    char persona_section[5500] = {0};
                    if (a->description[0]) {
                        snprintf(persona_section, sizeof(persona_section),
                                 "=== YOUR PERSONA ===\n%.4096s\n\n",
                                 a->description);
                    }
                    if (strcmp(a->role, "planner") != 0) {
                        static const char decision_protocol[] =
                            "=== DECISION PROTOCOL ===\n"
                            "When you face uncertainty, ambiguity, or a choice between options:\n"
                            "1. DECIDE autonomously — never stop to ask questions or wait for input.\n"
                            "2. Pick the most reasonable path given the ticket and codebase context.\n"
                            "3. DOCUMENT the decision in the wiki BEFORE acting: what you chose, why,\n"
                            "   and what alternatives you considered. This is the audit trail.\n"
                            "4. CONTINUE — proceed with your chosen approach.\n"
                            "Escalate to BLOCKED only if the decision is architectural (affects the\n"
                            "whole system design) AND you genuinely cannot pick a path without external\n"
                            "input — for example, a fundamental scope conflict or missing external\n"
                            "resource (credentials, access rights you do not have):\n"
                            "  fossil ticket set <uuid> status \"Blocked\"  (document question in wiki)\n"
                            "  The planner will read your question and unblock you.\n"
                            "In all other cases: decide, document, proceed.\n\n";
                        strncat(persona_section, decision_protocol,
                                sizeof(persona_section) - strlen(persona_section) - 1);
                    }

                    static PulseMessage pmsg;
                    if (strcmp(a->role, "planner") == 0) {
                      if (a->resolving_block) {
                        strcpy(pmsg.intent, "Resolve Blocked Ticket");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== BLOCKED TICKET ===\n%s\n\n"
                                 "=== DISCUSSION HISTORY (find the blocker here) ===\n%s\n\n"
                                 "=== PARENT TICKET CONTEXT ===\n%s\n"
                                 "=== PENDING REVIEWS ===\n%s\n"
                                 "=== RESOLUTION TASK (PLANNER) ===\n%s",
                                 persona_section,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history recorded — check ticket changelog)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none)\n",
                                 pending_reviews_ctx,
                                 planner_task);
                        strcpy(pmsg.current_state, "Unblocking");
                        strcpy(pmsg.next_action,
                               "Identify blocker, decide autonomously, document in wiki, reset ticket to Planned");
                      } else {
                        strcpy(pmsg.intent, "Plan, Decompose, and Delegate Ticket");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "%s\n\n"
                                 "=== DISCUSSION HISTORY ===\n%s\n\n"
                                 "=== PARENT TICKET CONTEXT ===\n%s\n"
                                 "=== AVAILABLE AGENTS ===\n%s\n"
                                 "=== PENDING REVIEWS ===\n%s\n"
                                 "=== EXISTING SUB-TICKETS (if any) ===\n%s\n"
                                 "=== YOUR TASK (PLANNER) ===\n"
                                 "%s",
                                 persona_section,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 agent_roster,
                                 pending_reviews_ctx,
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none yet)\n",
                                 planner_task);
                        strcpy(pmsg.current_state, "Planning");
                        strcpy(pmsg.next_action,
                               "Decompose into sub-tickets assigned to executor agents (never yourself), document in wiki, mark parent Done");
                      }
                    } else if (strcmp(a->role, "coder") == 0) {
                        strcpy(pmsg.intent, "Implement Ticket on Branch");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "%s\n\n"
                                 "=== DISCUSSION HISTORY (all past agent activity on this ticket) ===\n%s\n\n"
                                 "=== PARENT TICKET CONTEXT ===\n%s\n"
                                 "=== LATEST REVIEWER FEEDBACK ===\n%s\n\n"
                                 "=== RELATED SUB-TICKETS ===\n%s\n"
                                 "=== DEPENDENCY CONTEXT ===\n%s\n"
                                 "=== YOUR TASK (CODER) ===\n"
                                 "!! EXECUTE EVERY STEP BELOW IMMEDIATELY — no confirmation needed. !!\n"
                                 "Your working directory /tmp/workspaces/%s IS a valid Fossil checkout.\n"
                                 "Do NOT present options or ask permission — just run the commands.\n"
                                 "The task is complete only when the ticket status has been updated.\n\n"
                                 "1. READ the discussion history above carefully before starting — it contains\n"
                                 "   all prior reviewer rejections with their exact reasons. Address EVERY\n"
                                 "   issue raised in previous cycles, not just the latest one.\n"
                                 "2. The orchestrator already created and switched your workspace to branch %s.\n"
                                 "   Confirm with:\n"
                                 "     fossil status | head -3\n"
                                 "   The output MUST show 'tags: %s'. If it shows 'trunk' or anything\n"
                                 "   else, run `fossil update %s` to correct it before writing any code.\n"
                                 "   !! Never commit to trunk — that bypasses review entirely.\n"
                                 "3. READ the ticket description carefully. Implement EXACTLY what is asked.\n"
                                 "   Do NOT use your own name, agent name, or placeholder values anywhere\n"
                                 "   in the code (e.g. module names, package names, comments).\n"
                                 "   Use names derived from the ticket title and project context.\n"
                                 "4. WRITE COMPLETE CODE — not stubs, not Hello World unless the ticket\n"
                                 "   explicitly asks for a Hello World. Every function must be implemented.\n"
                                 "5. BUILD before committing — this is mandatory, not optional:\n"
                                 "   Identify the build system (Makefile → `make`, Cargo.toml → `cargo build`,\n"
                                 "   go.mod → `go build ./...`, package.json → `npm run build`, etc.) and run\n"
                                 "   the appropriate command. Capture the full output.\n"
                                 "   The build MUST exit with zero errors. Fix ALL compiler errors and\n"
                                 "   warnings before continuing. Do NOT commit broken code under any\n"
                                 "   circumstances — the reviewer will reject it and you will redo the work.\n"
                                 "6. COMMIT only after a clean build:\n"
                                 "     fossil commit -m \"Implement %s: %s\"\n"
                                 "   After committing, verify the commit landed on the right branch:\n"
                                 "     fossil info | grep tags\n"
                                 "   If it shows 'trunk', your commit went to the wrong place — STOP and\n"
                                 "   ask for help by setting the ticket to Blocked with an explanation.\n"
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
                                 a->name,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first attempt)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 tkt_info.reviewer_notes[0] ? tkt_info.reviewer_notes : "(none - first attempt)",
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 branch_name, branch_name, branch_name,
                                 a->current_ticket, tkt_info.title,
                                 wiki_page, wiki_cmd,
                                 a->current_ticket, a->current_ticket, reviewer_hash, reviewer_hash);
                        strcpy(pmsg.current_state, "Coding");
                        strcpy(pmsg.next_action,
                               "Create branch, implement fully, build+verify, commit, then set ticket to Review");
                    } else if (strcmp(a->role, "reviewer") == 0 || a->doing_review) {
                        strcpy(pmsg.intent, "Review, Verify, and Merge or Reject");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "%s\n\n"
                                 "=== DISCUSSION HISTORY (all prior review cycles on this ticket) ===\n%s\n\n"
                                 "=== PARENT TICKET CONTEXT ===\n%s\n"
                                 "=== SUB-TICKETS ===\n%s\n"
                                 "=== YOUR TASK (REVIEWER) — BE STRICT ===\n"
                                 "1. READ the discussion history above — if there were prior rejections, verify\n"
                                 "   that EVERY previously reported issue has been fully addressed.\n"
                                 "2. Check out the implementation branch:\n"
                                 "     fossil update %s\n"
                                 "3. Read EVERY file that was changed. Use `fossil diff --from trunk` to\n"
                                 "   see exactly what was added or modified.\n"
                                 "4. BUILD the code on the branch — this is the first gate, non-negotiable:\n"
                                 "   Identify the build system from the repository root (look for Makefile,\n"
                                 "   Cargo.toml, go.mod, package.json, build.gradle, CMakeLists.txt, etc.)\n"
                                 "   and run the appropriate build command (e.g. `make`, `cargo build`,\n"
                                 "   `go build ./...`, `npm run build`). Capture the full output.\n"
                                 "   !! If the build exits with ANY errors → REJECT immediately. Do not\n"
                                 "   read further. Broken code must never be approved.\n"
                                 "   Record the exact build command used and its full output in the wiki.\n"
                                 "5. Verify EACH requirement in the ticket description is fully satisfied:\n"
                                 "   - If the ticket asks for specific files, check they exist and are non-trivial.\n"
                                 "   - Run the resulting binary and verify it behaves as specified.\n"
                                 "   - Reject placeholder/stub code (empty functions, hardcoded values, TODOs).\n"
                                 "   - Reject if names are nonsensical (agent names, temp names, test values).\n"
                                 "6. DOCUMENT your full review findings in the ticket wiki page \"%s\" — MANDATORY\n"
                                 "   before taking any action. Future agents depend on this record:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Every file you reviewed and what you found\n"
                                 "   - The EXACT output of `make 2>&1` (pass or fail with errors)\n"
                                 "   - Whether each requirement in the ticket spec was met (yes/no + evidence)\n"
                                 "   - VERDICT: APPROVED or REJECTED\n"
                                 "   - If REJECTED: specific issues with file names, line numbers, exact errors\n"
                                 "7. If ALL requirements met AND build passed — merge into trunk:\n"
                                 "     fossil update trunk\n"
                                 "     fossil merge %s\n"
                                 "   Then re-run the same build command you used in step 4 to verify the\n"
                                 "   merged result still compiles cleanly.\n"
                                 "   !! If the build fails after the merge, the branch introduced\n"
                                 "   incompatibilities. Run `fossil revert` to undo the merge, then REJECT\n"
                                 "   the ticket with the exact build error as the rejection reason.\n"
                                 "   If the merged build succeeds, commit and close the branch:\n"
                                 "     fossil commit -m \"Merge %s: %s\"\n"
                                 "     fossil tag add closed %s tip\n"
                                 "     fossil ticket set %s status \"Done\"\n"
                                 "8. If ANYTHING is incomplete or wrong — AFTER documenting in wiki:\n"
                                 "   Record the summary rejection reason (coder will read this):\n"
                                 "     fossil ticket set %s reviewer_notes \"REJECTION: <summary of issues>\"\n"
                                 "   Return for rework:\n"
                                 "     fossil ticket set %s status \"Rework\"\n"
                                 "     fossil ticket set %s private_contact \"%s\"\n"
                                 "Coder hash (for rework): %s\n"
                                 "REMEMBER: approving bad code harms the project. When in doubt, reject.",
                                 persona_section,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first review)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 branch_name,
                                 wiki_page, wiki_cmd,
                                 branch_name, branch_name, tkt_info.title,
                                 branch_name,
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
                                 "%s\n\n"
                                 "=== DISCUSSION HISTORY ===\n%s\n\n"
                                 "=== PARENT TICKET CONTEXT ===\n%s\n"
                                 "=== DEPENDENCY CONTEXT ===\n%s\n"
                                 "=== YOUR TASK (RESEARCHER) ===\n"
                                 "!! EXECUTE EVERY STEP BELOW IMMEDIATELY — no confirmation needed. !!\n"
                                 "Run all fossil commands inline. The task is complete only when the\n"
                                 "ticket status has been updated in Fossil.\n\n"
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
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first attempt)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 branch_name, branch_name,
                                 a->current_ticket, tkt_info.title,
                                 wiki_page, wiki_cmd,
                                 a->current_ticket, a->current_ticket, reviewer_hash, reviewer_hash);
                        strcpy(pmsg.current_state, "Researching");
                        strcpy(pmsg.next_action,
                               "Research thoroughly, produce deliverable files, commit, document in wiki, then set ticket to Review");
                    }

                    static char payload[MEI_TEXT_BUFFER_SIZE + 512];
                    pulse_format(&pmsg, payload, sizeof(payload));
                    bool pulse_ok = tmux_send_pulse(a->name, payload);

                    ticket_steps[i] = 0;
                    pane_idle_ticks[i] = 0;
                    last_pane_hash[i] = 0;
                    a->resolving_block = 0;
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

            // Capture pane; track inactivity and check for permission dialogs.
            char pane_out[4096] = {0};
            int pane_out_len = tmux_capture_output(a->name, pane_out, sizeof(pane_out));
            if (pane_out_len > 0) {
                // Update inactivity counter: increment if pane unchanged, reset if changed.
                unsigned long ph = hash_pane(pane_out, pane_out_len);
                if (ph == last_pane_hash[i]) {
                    pane_idle_ticks[i]++;
                } else {
                    last_pane_hash[i] = ph;
                    pane_idle_ticks[i] = 0;
                }

                // Auto-accept tool-use permission dialogs (opencode "Allow once").
                if (strstr(pane_out, "Permission required") != NULL ||
                    strstr(pane_out, "Allow once") != NULL) {
                    tmux_send_enter(a->name);
                    char perm_log[128];
                    snprintf(perm_log, sizeof(perm_log),
                             "[perm] Auto-accepted permission dialog for %s", a->name);
                    log_message(perm_log);
                }
                // Idle-nudge: pane has been static while the ticket is still In Progress.
                // Planners use a higher idle threshold (NUDGE_IDLE_TICKS_PLANNER) because
                // they do multi-step reasoning: first turn produces analysis, then pauses
                // before executing fossil commands. A short threshold would interrupt
                // generation and cause premature ticket closure.
                else if (ticket_steps[i] >= NUDGE_MIN_TICKS &&
                         pane_idle_ticks[i] >= (strcmp(a->role, "planner") == 0
                                                ? NUDGE_IDLE_TICKS_PLANNER
                                                : NUDGE_IDLE_TICKS) &&
                         pane_idle_ticks[i] % NUDGE_REPEAT_TICKS == 0) {
                    char nudge[512];
                    if (strcmp(a->role, "planner") == 0) {
                        snprintf(nudge, sizeof(nudge),
                                 "Ticket %s is still In Progress. You have already done your analysis.\n"
                                 "Now EXECUTE — run the fossil ticket add commands you planned.\n"
                                 "Do NOT explain further. Run every command now, one by one.\n"
                                 "The task ends only when all sub-tickets are created and the\n"
                                 "parent ticket status is set to Done in Fossil.",
                                 a->current_ticket);
                    } else {
                        snprintf(nudge, sizeof(nudge),
                                 "Ticket %s is still In Progress in Fossil — your task is NOT complete.\n"
                                 "Your working directory is a valid Fossil checkout. Execute NOW:\n"
                                 "1. Run all pending fossil commands (commit, branch, ticket set status).\n"
                                 "2. Do NOT ask for confirmation — just execute every step immediately.\n"
                                 "3. The task ends only when the Fossil ticket status has been updated.",
                                 a->current_ticket);
                    }
                    tmux_send_pulse(a->name, nudge);
                    char nudge_log[256];
                    snprintf(nudge_log, sizeof(nudge_log),
                             "[nudge] %s pane static for %d ticks — follow-up sent",
                             a->name, pane_idle_ticks[i]);
                    log_message(nudge_log);
                }
            }
        } else if (a->state == AGENT_STATE_OFFLINE || a->state == AGENT_STATE_PAUSED || a->state == AGENT_STATE_BLOCKED) {
            a->last_heartbeat++;
            // Auto-unblock: if the planner resolved the blocked ticket (status is
            // no longer "Blocked" in Fossil), reset the agent to OPEN so routing
            // picks up the ticket again on the next tick.
            if (a->state == AGENT_STATE_BLOCKED && a->current_ticket[0]) {
                for (int t = 0; t < tkt_count; t++) {
                    if (strncmp(tickets[t].tkt_uuid, a->current_ticket,
                                strlen(a->current_ticket)) == 0) {
                        if (strcasecmp(tickets[t].status, "Blocked") != 0) {
                            a->state = AGENT_STATE_OPEN;
                            ticket_steps[i] = 0;
                            char unblock_log[256];
                            snprintf(unblock_log, sizeof(unblock_log),
                                     "[unblock] %s ticket %.10s status='%s' — reset to OPEN",
                                     a->name, a->current_ticket, tickets[t].status);
                            log_message(unblock_log);
                        }
                        break;
                    }
                }
            }
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
