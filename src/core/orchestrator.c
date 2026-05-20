#include "core/orchestrator.h"
#include "core/tmux_mgr.h"
#include "core/vcs_backend.h"
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
// Counts consecutive ticks where still_active==0 but final_status is "Open"/empty.
// Used to filter out transient label-removal windows before the add-label arrives.
static int open_settling_ticks[MAX_AGENTS] = {0};
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
static int deps_satisfied(VCSTicket *tickets, int tkt_count, const char *description) {
    const char *p = description;
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
            if (strncmp(tickets[t].uuid, dep_uuid, len) == 0) {
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

    for (int i = 0; i < *agent_count; i++) {
        ticket_steps[i] = 0;

        char workspace[256];
        snprintf(workspace, sizeof(workspace), "/tmp/workspaces/%s", agents[i].name);

        // Initialise workspace checkout (mkdir + vcs open)
        g_backend->workspace_init(g_backend, workspace);

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

    static VCSTicket tickets[100];
    int tkt_count = g_backend->ticket_list(g_backend, tickets, 100);
    for (int i = 0; i < *agent_count; i++) {
        char dbg_init[256];
        snprintf(dbg_init, sizeof(dbg_init), "[init] Agent %s hash: %s", agents[i].name, agents[i].hash);
        log_message(dbg_init);

        for (int t = 0; t < tkt_count; t++) {
            int matched_hash = (strcmp(tickets[t].assignee, agents[i].hash) == 0);
            int matched_name = (strcmp(tickets[t].assignee, agents[i].name) == 0);
            if (!matched_hash && !matched_name) continue;
            strncpy(agents[i].current_ticket, tickets[t].uuid, sizeof(agents[i].current_ticket) - 1);
            if (strcasecmp(tickets[t].status, "In Progress") == 0) {
                agents[i].state = AGENT_STATE_IN_PROGRESS;
                ticket_steps[i] = -1;
                warm_up_ticks[i] = 0;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed IN_PROGRESS on %s", agents[i].name, tickets[t].uuid);
                log_message(log);
            } else if (strcasecmp(tickets[t].status, "Blocked") == 0) {
                agents[i].state = AGENT_STATE_BLOCKED;
                char log[256];
                snprintf(log, sizeof(log), "[sync] %s resumed BLOCKED on %s", agents[i].name, tickets[t].uuid);
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
    static VCSTicket tickets[100];
    static int tick_counter = 0;
    tick_counter++;
    int tkt_count = g_backend->ticket_list(g_backend, tickets, 100);

    // When a "Pending Approval" ticket is routed to the planner again due to the
    // orchestrator's direct SQL UPDATE racing with the planner's artifact-based status
    // change, force it back to "Pending Approval" so it waits for human action.
    for (int t = 0; t < tkt_count; t++) {
        if (strcasecmp(tickets[t].status, "In Progress") != 0) continue;
        if (tickets[t].reviewer_notes[0] == '\0') continue;
        if (strncmp(tickets[t].reviewer_notes, "[PLAN_PENDING]", 14) != 0) continue;
        g_backend->ticket_set_status(g_backend, tickets[t].uuid, "Pending Approval");
        strncpy(tickets[t].status, "Pending Approval", sizeof(tickets[t].status) - 1);
    }

    for (int i = 0; i < agent_count; i++) {
        Agent *a = &agents[i];
        a->step_count = (ticket_steps[i] > 0) ? ticket_steps[i] : 0;
        // Preserved across goto route_open so is_recovery re-dispatch keeps phase flags.
        int saved_doing_phase2 = 0;

        route_open:
        if (a->state == AGENT_STATE_OPEN) {
            // Clear pending_review_ticket only when the review cycle is truly over.
            // "Done"/"closed" → approved, free to take new work.
            // "Planned" → planner reset it, free to take new work.
            // "Rework" is intentionally excluded: the coder still owns the ticket and
            // must fix it. The gate stays so no other ticket is picked up, and the
            // is_own_rework exception in role_accepts lets the coder re-acquire it.
            if (a->pending_review_ticket[0] != '\0') {
                for (int t = 0; t < tkt_count; t++) {
                    if (strcmp(tickets[t].uuid, a->pending_review_ticket) == 0) {
                        int cycle_ended = (strcasecmp(tickets[t].status, "Done")    == 0 ||
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
                int is_delegated = (strcmp(tickets[t].assignee, a->hash) == 0) ||
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

                int tkt_open             = (strcasecmp(tickets[t].status, "Open") == 0 || strlen(tickets[t].status) == 0);
                int tkt_verified         = (strcasecmp(tickets[t].status, "Verified") == 0);
                int tkt_progress         = (strcasecmp(tickets[t].status, "In Progress") == 0);
                int tkt_planned          = (strcasecmp(tickets[t].status, "Planned") == 0);
                int tkt_review           = (strcasecmp(tickets[t].status, "Review") == 0);
                int tkt_rework           = (strcasecmp(tickets[t].status, "Rework") == 0);
                int tkt_blocked          = (strcasecmp(tickets[t].status, "Blocked") == 0);
                int tkt_pending_approval = (strcasecmp(tickets[t].status, "Pending Approval") == 0);
                int tkt_qa_ready         = (strcasecmp(tickets[t].status, "QA Ready") == 0);
                (void)tkt_pending_approval;

                int role_accepts = 0;
                if (strcmp(a->role, "planner") == 0) {
                    // Planner owns:
                    //   tkt_open     — fresh top-level ticket (Phase 1: write plan)
                    //   tkt_verified — human approved/edited the plan (Phase 2 or re-plan)
                    //   tkt_blocked  — executor escalated; planner unblocks
                    //   tkt_qa_ready — all sub-tasks done; planner runs quality assessment
                    int is_subtask = strstr(tickets[t].description, "[parent:") != NULL;
                    role_accepts = (tkt_open && !is_subtask) ||
                                   (tkt_verified && !is_subtask) ||
                                   tkt_blocked               ||
                                   tkt_qa_ready;
                } else if (strcmp(a->role, "coder") == 0) {
                    int is_subtask = (strstr(tickets[t].description, "[parent:") != NULL);
                    // Fallback: unassigned Planned sub-tickets are also valid coder work.
                    // This handles the case where gh issue create silently dropped the
                    // assignee label because it didn't exist yet in the repo.
                    int unassigned_planned_sub = is_unassigned && is_subtask && tkt_planned;
                    // Also accept re-opened sub-tickets (user manually reset to Open)
                    // Also accept Review tickets when explicitly delegated by planner.
                    role_accepts = (is_delegated || unassigned_planned_sub) && (tkt_planned || tkt_rework || tkt_review || (tkt_open && is_subtask));
                    // Review gate: don't pick up new work while a prior submission is still
                    // under review. Exception: the same ticket returned as Rework (the coder
                    // must be able to act on reviewer feedback for its own submission).
                    if (role_accepts && a->pending_review_ticket[0] != '\0') {
                        int is_own_rework = (strcmp(tickets[t].uuid, a->pending_review_ticket) == 0);
                        if (!is_own_rework) role_accepts = 0;
                    }
                } else if (strcmp(a->role, "reviewer") == 0) {
                    // Reviewer only acts when there is actual work to review: a ticket in
                    // Review status, either unassigned (coder submitted normally) or directly
                    // delegated by the planner. Planned/Rework tickets are never picked up —
                    // those statuses mean work is not yet ready for review, and accepting them
                    // would activate the reviewer before the coder has done anything.
                    int assigned_to_other = !is_delegated && assignee_known;
                    role_accepts = tkt_review && !assigned_to_other;
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
                    if (!deps_satisfied(tickets, tkt_count, tickets[t].description)) {
                        role_accepts = 0;
                        // Rate-limit dep-gate log: one entry per ticket per 30 ticks (~60s).
                        static char dep_gate_logged_uuid[100][41];
                        static int  dep_gate_logged_tick[100];
                        static int  dep_gate_slots = 0;
                        int slot = -1;
                        for (int s = 0; s < dep_gate_slots; s++) {
                            if (strncmp(dep_gate_logged_uuid[s], tickets[t].uuid, 40) == 0) {
                                slot = s; break;
                            }
                        }
                        if (slot < 0 && dep_gate_slots < 100) {
                            slot = dep_gate_slots++;
                            strncpy(dep_gate_logged_uuid[slot], tickets[t].uuid, 40);
                            dep_gate_logged_tick[slot] = tick_counter - 30;
                        }
                        if (slot >= 0 && tick_counter - dep_gate_logged_tick[slot] >= 30) {
                            dep_gate_logged_tick[slot] = tick_counter;
                            char dep_log[256];
                            snprintf(dep_log, sizeof(dep_log),
                                     "[dep-gate] %s skipping ticket %.10s — dependency not Done",
                                     a->name, tickets[t].uuid);
                            log_message(dep_log);
                        }
                    }
                }

                if (!role_accepts && !is_recovery) continue;

                // Normalize assignee to this agent's hash and mark as In Progress.
                if (strcmp(tickets[t].assignee, a->hash) != 0) {
                    g_backend->ticket_assign(g_backend, tickets[t].uuid, a->hash);
                    char note[256];
                    snprintf(note, sizeof(note), "Assigned to %s (%s) by orchestrator routing.",
                             a->name, a->role);
                    g_backend->ticket_add_note(g_backend, tickets[t].uuid, note);
                }
                if (!tkt_progress) {
                    g_backend->ticket_set_status(g_backend, tickets[t].uuid, "In Progress");
                    char note[256];
                    snprintf(note, sizeof(note),
                             "Status set to In Progress. Agent %s (%s) started work.",
                             a->name, a->role);
                    g_backend->ticket_add_note(g_backend, tickets[t].uuid, note);
                }

                // Update the in-memory snapshot so other agents in this same tick
                // don't see this ticket as available and claim it concurrently.
                strncpy(tickets[t].assignee, a->hash,      sizeof(tickets[t].assignee) - 1);
                strncpy(tickets[t].status,   "In Progress", sizeof(tickets[t].status)   - 1);

                strncpy(a->current_ticket, tickets[t].uuid, sizeof(a->current_ticket) - 1);
                a->state = AGENT_STATE_IN_PROGRESS;
                a->resolving_block = tkt_blocked;
                a->doing_review    = tkt_review && (strcmp(a->role, "reviewer") != 0);
                a->doing_qa        = tkt_qa_ready;
                a->doing_phase2    = is_recovery ? saved_doing_phase2 : tkt_verified;
                ticket_steps[i] = -1;
                warm_up_ticks[i] = 0;

                char log[256];
                snprintf(log, sizeof(log), "Ticket %s → %s (%s). Waiting for CLI...",
                         tickets[t].uuid, a->name, a->role);
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
                char final_notes[2048]  = {0};
                int still_active = 0;
                for (int t = 0; t < tkt_count; t++) {
                    if (strcmp(tickets[t].uuid, a->current_ticket) == 0) {
                        // Status-only: the assignee label can lag behind the API call that
                        // set it, causing a false positive if we also check the assignee.
                        // An agent is active as long as the ticket STATUS is "In Progress".
                        // Exception: reviewer is also active while status is "Review" because
                        // the coder's pane may re-run its submit commands concurrently after
                        // handoff, overwriting the orchestrator's "In Progress" back to "Review".
                        // The reviewer completes only when status transitions to Done/Rework/etc.
                        still_active = (strcasecmp(tickets[t].status, "In Progress") == 0) ||
                                       (strcmp(a->role, "reviewer") == 0 &&
                                        strcasecmp(tickets[t].status, "Review") == 0);
                        strncpy(final_status, tickets[t].status,        sizeof(final_status) - 1);
                        strncpy(final_notes,  tickets[t].reviewer_notes, sizeof(final_notes)  - 1);
                        break;
                    }
                }
                if (!still_active) {
                    // Settling guard: "Open"/empty and "In Progress" are not valid handoff
                    // states. "Open" appears transiently between the remove-label and
                    // add-label of a status change. "In Progress" appears when the assignee
                    // label hasn't propagated yet but status propagation also failed.
                    // Require the state to persist 3+ ticks before treating as completion.
                    int final_is_transitional = (final_status[0] == '\0' ||
                                                  strcasecmp(final_status, "Open") == 0 ||
                                                  strcasecmp(final_status, "In Progress") == 0);
                    if (final_is_transitional) {
                        if (++open_settling_ticks[i] < 3)
                            continue;
                    } else {
                        open_settling_ticks[i] = 0;
                    }

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
                        g_backend->ticket_assign(g_backend, a->current_ticket, "");
                        for (int t = 0; t < tkt_count; t++) {
                            if (strcmp(tickets[t].uuid, a->current_ticket) == 0) {
                                tickets[t].assignee[0] = '\0';
                                break;
                            }
                        }
                        strncpy(a->pending_review_ticket, a->current_ticket,
                                sizeof(a->pending_review_ticket) - 1);
                        a->pending_review_ticket[sizeof(a->pending_review_ticket) - 1] = '\0';
                        log_message("[review] Cleared assignee — ticket submitted for review");
                    }

                    if (strcasecmp(final_status, "Rework") == 0 &&
                        strcmp(a->role, "reviewer") == 0) {
                        for (int j = 0; j < agent_count; j++) {
                            if (j == i) continue;
                            if (strcmp(agents[j].pending_review_ticket, a->current_ticket) == 0) {
                                g_backend->ticket_assign(g_backend, a->current_ticket, agents[j].hash);
                                for (int t = 0; t < tkt_count; t++) {
                                    if (strcmp(tickets[t].uuid, a->current_ticket) == 0) {
                                        strncpy(tickets[t].assignee, agents[j].hash,
                                                sizeof(tickets[t].assignee) - 1);
                                        break;
                                    }
                                }
                                char rework_log[256];
                                snprintf(rework_log, sizeof(rework_log),
                                         "[review] Rework: re-assigned ticket back to %s (%s)",
                                         agents[j].name, agents[j].role);
                                log_message(rework_log);
                                break;
                            }
                        }
                    }

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
                    g_backend->ticket_append_log(g_backend, a->current_ticket, a->name, wiki_msg);

                    // Preserve phase2 flag so is_recovery re-dispatch restores it if
                    // the cache hasn't propagated the planner's status change yet.
                    saved_doing_phase2 = a->doing_phase2;
                    a->state = AGENT_STATE_OPEN;
                    a->doing_review  = 0;
                    a->doing_qa      = 0;
                    a->doing_phase2  = 0;
                    strcpy(a->current_ticket, "None");
                    ticket_steps[i] = 0;
                    warm_up_ticks[i] = 0;
                    pane_idle_ticks[i] = 0;
                    last_pane_hash[i] = 0;
                    open_settling_ticks[i] = 0;
                    // Route immediately in this same tick instead of waiting for the next one.
                    goto route_open;
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
                    char branch_name[64] = {0};
                    if (g_backend->type == VCS_GITHUB) {
                        int _n = (a->current_ticket[0] == '#') ? atoi(a->current_ticket + 1)
                                                               : atoi(a->current_ticket);
                        snprintf(branch_name, sizeof(branch_name), "mei/issue-%d", _n);
                    } else {
                        snprintf(branch_name, sizeof(branch_name), "tkt-%.8s", a->current_ticket);
                    }

                    // Pre-compute parent feature branch (GitHub only, needed before branch creation).
                    // Sub-issues branch from the parent feature branch so all their changes
                    // accumulate there and are eventually PR'd to main as a unit.
                    char pre_parent_branch[64] = {0};
                    if (g_backend->type == VCS_GITHUB) {
                        for (int t = 0; t < tkt_count; t++) {
                            if (strcmp(tickets[t].uuid, a->current_ticket) != 0) continue;
                            const char *pp2 = strstr(tickets[t].description, "[parent:");
                            if (pp2) {
                                const char *s2 = pp2 + 8, *e2 = strchr(s2, ']');
                                if (e2 && (size_t)(e2 - s2) < 64) {
                                    char puid2[72] = {0};
                                    strncpy(puid2, s2, (size_t)(e2 - s2));
                                    int _pn = (puid2[0] == '#') ? atoi(puid2 + 1) : atoi(puid2);
                                    snprintf(pre_parent_branch, sizeof(pre_parent_branch),
                                             "mei/issue-%d", _pn);
                                }
                            }
                            break;
                        }
                    }

                    // For implementation roles, create the ticket branch in the
                    // repository and switch the workspace to it before sending the
                    // PULSE. The agent only needs to sync to the branch (fossil update
                    // or git checkout) — it never creates branches itself. This prevents
                    // ghost branches caused by agents committing to trunk.
                    if (strcmp(a->role, "coder") == 0 ||
                        strcmp(a->role, "researcher") == 0) {
                        char workspace[256];
                        snprintf(workspace, sizeof(workspace), "/tmp/workspaces/%s", a->name);
                        // Sub-issue on GitHub: branch from parent feature branch so each
                        // sub-issue's changes accumulate on the parent branch.
                        if (pre_parent_branch[0])
                            g_backend->branch_switch(g_backend, workspace, pre_parent_branch);
                        g_backend->branch_create(g_backend, workspace, branch_name);
                        g_backend->branch_switch(g_backend, workspace, branch_name);
                        char branch_log[128];
                        snprintf(branch_log, sizeof(branch_log),
                                 "[branch] Created/switched workspace %s → %s (base: %s)",
                                 a->name, branch_name,
                                 pre_parent_branch[0] ? pre_parent_branch : "main");
                        log_message(branch_log);
                    } else {
                        char workspace[256];
                        snprintf(workspace, sizeof(workspace), "/tmp/workspaces/%s", a->name);
                        // Planner in Phase 2: create the parent feature branch now so coders
                        // can branch from it. branch_create is idempotent — safe to re-call.
                        if (g_backend->type == VCS_GITHUB && a->doing_phase2)
                            g_backend->branch_create(g_backend, workspace, branch_name);
                        const char *default_branch = (g_backend->type == VCS_GITHUB) ? "main" : "trunk";
                        g_backend->branch_switch(g_backend, workspace, default_branch);
                    }
                    char upd_log[128];
                    snprintf(upd_log, sizeof(upd_log),
                             "[sync] Workspace updated for %s before PULSE", a->name);
                    log_message(upd_log);

                    // Fetch ticket info to build PULSE
                    VCSTicket tkt_info;
                    memset(&tkt_info, 0, sizeof(tkt_info));
                    strncpy(tkt_info.uuid, a->current_ticket, sizeof(tkt_info.uuid) - 1);
                    for (int t = 0; t < tkt_count; t++) {
                        if (strcmp(tickets[t].uuid, a->current_ticket) == 0) {
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
                                if (strcmp(tickets[t2].uuid,
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

                    // When building the reviewer PULSE, we need the hash of the coder
                    // who submitted THIS specific ticket, not just the first coder in the
                    // list. Scan pending_review_ticket to find the original submitter.
                    char rework_coder_hash[128] = {0};
                    for (int j = 0; j < agent_count; j++) {
                        if (strcmp(agents[j].pending_review_ticket, a->current_ticket) == 0) {
                            strncpy(rework_coder_hash, agents[j].hash, sizeof(rework_coder_hash) - 1);
                            break;
                        }
                    }
                    if (!rework_coder_hash[0])
                        strncpy(rework_coder_hash, coder_hash, sizeof(rework_coder_hash) - 1);

                    // Point 2/3: collect sub-tickets (comment contains [parent:<uuid>])
                    // and dependency references ([depends:<uuid>]) for richer context.
                    char parent_tag[72];
                    snprintf(parent_tag, sizeof(parent_tag), "[parent:%s]", a->current_ticket);
                    static char subtasks_ctx[MEI_TEXT_BUFFER_SIZE];
                    static char dep_ctx[MEI_TEXT_BUFFER_SIZE];
                    memset(subtasks_ctx, 0, sizeof(subtasks_ctx));
                    memset(dep_ctx, 0, sizeof(dep_ctx));
                    for (int t2 = 0; t2 < tkt_count; t2++) {
                        if (strstr(tickets[t2].description, parent_tag)) {
                            char sub[768];
                            snprintf(sub, sizeof(sub),
                                     "  [sub] %s | %s | status: %s\n"
                                     "        desc: %.500s\n",
                                     tickets[t2].uuid, tickets[t2].title,
                                     tickets[t2].status, tickets[t2].description);
                            strncat(subtasks_ctx, sub,
                                    sizeof(subtasks_ctx) - strlen(subtasks_ctx) - 1);
                        }
                        char dep_tag[72];
                        snprintf(dep_tag, sizeof(dep_tag), "[depends:%s]", a->current_ticket);
                        if (strstr(tickets[t2].description, dep_tag)) {
                            char dep[768];
                            snprintf(dep, sizeof(dep),
                                     "  [dep] %s | %s | status: %s\n"
                                     "        desc: %.500s\n",
                                     tickets[t2].uuid, tickets[t2].title,
                                     tickets[t2].status, tickets[t2].description);
                            strncat(dep_ctx, dep, sizeof(dep_ctx) - strlen(dep_ctx) - 1);
                        }
                    }

                    // Get the ticket description. Agent-created tickets populate
                    // description directly; web-UI tickets may leave it empty and
                    // store it as icomment in the creation artifact (Fossil-specific).
                    char tkt_description[2048] = {0};
                    if (tkt_info.description[0]) {
                        strncpy(tkt_description, tkt_info.description, sizeof(tkt_description) - 1);
                    } else {
                        g_backend->ticket_read_initial_description(
                            g_backend, a->current_ticket, tkt_description, sizeof(tkt_description));
                    }

                    char human_remarks[2048] = {0};
                    g_backend->ticket_read_human_remarks(g_backend, a->current_ticket,
                                                          human_remarks, sizeof(human_remarks));

                    char tkt_full[8192];
                    snprintf(tkt_full, sizeof(tkt_full),
                             "uuid:     %s\ntitle:    %s\nstatus:   %s\nassignee: %s\n\n"
                             "--- DESCRIPTION ---\n%s\n\n"
                             "--- REVIEWER NOTES ---\n%s\n\n"
                             "--- HUMAN REMARKS (requirements/feedback added via web UI) ---\n%s",
                             tkt_info.uuid, tkt_info.title, tkt_info.status,
                             tkt_info.assignee,
                             tkt_description[0] ? tkt_description : "(no description in ticket)",
                             tkt_info.reviewer_notes[0] ? tkt_info.reviewer_notes : "(none)",
                             human_remarks[0] ? human_remarks : "(none)");

                    char discussion_log[4096] = {0};
                    g_backend->ticket_read_log(g_backend, a->current_ticket,
                                               discussion_log, sizeof(discussion_log));

                    // Parent ticket context: when this is a sub-ticket ([parent:UUID] in description)
                    char parent_uuid[72] = {0};
                    const char *pp = strstr(tkt_info.description, "[parent:");
                    if (pp) {
                        const char *start = pp + 8;
                        const char *end   = strchr(start, ']');
                        if (end && (size_t)(end - start) < sizeof(parent_uuid) - 1)
                            strncpy(parent_uuid, start, end - start);
                    }
                    char parent_ctx[4096] = {0};
                    if (parent_uuid[0]) {
                        VCSTicket *parent_tkt = NULL;
                        for (int t2 = 0; t2 < tkt_count; t2++) {
                            if (strncmp(tickets[t2].uuid, parent_uuid,
                                        strlen(parent_uuid)) == 0) {
                                parent_tkt = &tickets[t2];
                                break;
                            }
                        }
                        char parent_desc[2048] = {0};
                        if (parent_tkt && parent_tkt->description[0]) {
                            strncpy(parent_desc, parent_tkt->description, sizeof(parent_desc) - 1);
                        } else {
                            g_backend->ticket_read_initial_description(
                                g_backend, parent_uuid, parent_desc, sizeof(parent_desc));
                        }
                        char parent_wiki[2048] = {0};
                        g_backend->ticket_read_log(g_backend, parent_uuid, parent_wiki, sizeof(parent_wiki));
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
                    // an audit log entry.  For Fossil: uses wiki pages in the open checkout.
                    // For GitHub: posts a tagged issue comment.
                    // IMPORTANT: use ./ prefix so any temp file is created inside the workspace
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

                    // ── Backend-specific command strings for PULSE messages ──────────────────
                    int _is_gh = (g_backend->type == VCS_GITHUB);
                    char _tkt_addr[32];   // numeric issue# for GitHub, UUID for Fossil

                    // Parent feature branch name (GitHub only): sub-issues merge here;
                    // planner QA inspects here; the final PR targets main from here.
                    char parent_branch_name[64] = {0};
                    if (_is_gh && parent_uuid[0]) {
                        int _pn = (parent_uuid[0] == '#') ? atoi(parent_uuid + 1) : atoi(parent_uuid);
                        snprintf(parent_branch_name, sizeof(parent_branch_name), "mei/issue-%d", _pn);
                    }
                    if (_is_gh) {
                        int _n = (a->current_ticket[0] == '#') ? atoi(a->current_ticket + 1)
                                                               : atoi(a->current_ticket);
                        snprintf(_tkt_addr, sizeof(_tkt_addr), "%d", _n);
                    } else {
                        strncpy(_tkt_addr, a->current_ticket, sizeof(_tkt_addr) - 1);
                    }

                    // Prefix for status-change commands (GitHub removes all status:* labels
                    // then adds the new one).
                    static const char _gh_removes[] =
                        "--remove-label 'status:Open' "
                        "--remove-label 'status:Planned' "
                        "--remove-label 'status:In Progress' "
                        "--remove-label 'status:Review' "
                        "--remove-label 'status:Rework' "
                        "--remove-label 'status:Blocked' "
                        "--remove-label 'status:Done' "
                        "--remove-label 'status:QA Ready' "
                        "--remove-label 'status:Pending Approval' "
                        "--remove-label 'status:Delegated' "
                        "--remove-label 'status:Verified' "
                        "--remove-label 'status:closed'";

                    // Helper macro: build a status-set command into `buf`.
                    // Single gh issue edit call — all --remove-label and --add-label flags
                    // are sent in one API request, so the transition is atomic on GitHub.
                    // Missing labels are silently ignored by gh (exit 0), so the split
                    // two-command form is unnecessary and creates a transient "Open" window.
#define _CMD_STATUS(buf, status) do { \
    if (_is_gh) \
        snprintf(buf, sizeof(buf), \
                 "gh issue edit %s -R '%s' %s --add-label 'status:%s' > /dev/null 2>&1", \
                 _tkt_addr, g_backend->repo_id, _gh_removes, status); \
    else \
        snprintf(buf, sizeof(buf), \
                 "fossil ticket set %s status \"%s\"", a->current_ticket, status); \
} while (0)

                    char _cmd_set_pa[768],       _cmd_set_planned[768],   _cmd_set_delegated[768];
                    char _cmd_set_done[1024],     _cmd_set_review[768],    _cmd_set_blocked[768];
                    char _cmd_set_rework[768];
                    _CMD_STATUS(_cmd_set_pa,        "Pending Approval");
                    _CMD_STATUS(_cmd_set_planned,   "Planned");
                    _CMD_STATUS(_cmd_set_delegated, "Delegated");
                    _CMD_STATUS(_cmd_set_done,      "Done");
                    // For GitHub: also close the issue so it disappears from the open list.
                    if (_is_gh) {
                        char _close_suffix[256];
                        snprintf(_close_suffix, sizeof(_close_suffix),
                                 "\n     gh issue close %s -R '%s' > /dev/null 2>&1",
                                 _tkt_addr, g_backend->repo_id);
                        strncat(_cmd_set_done, _close_suffix,
                                sizeof(_cmd_set_done) - strlen(_cmd_set_done) - 1);
                    }
                    _CMD_STATUS(_cmd_set_review,    "Review");
                    _CMD_STATUS(_cmd_set_blocked,   "Blocked");
                    _CMD_STATUS(_cmd_set_rework,    "Rework");
#undef _CMD_STATUS

                    // Set assignee: Fossil = private_contact; GitHub = assignee:<hash> label
                    // For GitHub, must create the label before adding it — use hash twice (%s %s).
                    // For Fossil, single %s (the extra arg is ignored by snprintf).
                    char _cmd_set_assignee_fmt[896];
                    if (_is_gh)
                        snprintf(_cmd_set_assignee_fmt, sizeof(_cmd_set_assignee_fmt),
                                 "gh label create 'assignee:%%s' -R '%s' --color '0075CA' --force > /dev/null 2>&1\n"
                                 "gh issue edit %s -R '%s' --remove-label 'assignee:%s' 2>/dev/null || true\n"
                                 "gh issue edit %s -R '%s' --add-label 'assignee:%%s'",
                                 g_backend->repo_id, _tkt_addr, g_backend->repo_id, a->hash,
                                 _tkt_addr, g_backend->repo_id);
                    else
                        snprintf(_cmd_set_assignee_fmt, sizeof(_cmd_set_assignee_fmt),
                                 "fossil ticket set %s private_contact \"%%s\"", a->current_ticket);

                    // Reviewer notes: GitHub = [review] comment; Fossil = reviewer_notes field
                    char _cmd_set_notes_fmt[512];   // use with snprintf(_cmd_set_notes_fmt, note_text)
                    if (_is_gh)
                        snprintf(_cmd_set_notes_fmt, sizeof(_cmd_set_notes_fmt),
                                 "gh issue comment %s -R '%s' --body '[review] %%s'",
                                 _tkt_addr, g_backend->repo_id);
                    else
                        snprintf(_cmd_set_notes_fmt, sizeof(_cmd_set_notes_fmt),
                                 "fossil ticket set %s reviewer_notes \"%%s\"", a->current_ticket);

                    // Build concrete reviewer-notes commands (used in coder + reviewer PULSE)
                    char _cmd_notes_coder[768], _cmd_notes_reviewer[768];
                    snprintf(_cmd_notes_coder,    sizeof(_cmd_notes_coder),
                             _cmd_set_notes_fmt, "REJECTION: <summary of issues>");
                    snprintf(_cmd_notes_reviewer, sizeof(_cmd_notes_reviewer),
                             _cmd_set_notes_fmt, "REJECTED: <exact issues found>");
                    (void)_cmd_notes_reviewer;

                    // Workspace sync
                    char _cmd_sync_trunk[192], _cmd_sync_branch[256];
                    if (_is_gh) {
                        if (a->doing_qa) {
                            // QA: planner inspects the parent feature branch where all
                            // sub-issue merges landed, not main.
                            snprintf(_cmd_sync_trunk, sizeof(_cmd_sync_trunk),
                                     "git fetch origin && git checkout '%s' && git pull origin '%s'",
                                     branch_name, branch_name);
                        } else {
                            strncpy(_cmd_sync_trunk, "git checkout main && git pull origin main",
                                    sizeof(_cmd_sync_trunk) - 1);
                        }
                        // Always fetch + reset --hard so the reviewer gets the latest push,
                        // not a stale local copy from a previous review cycle.
                        snprintf(_cmd_sync_branch, sizeof(_cmd_sync_branch),
                                 "git fetch origin && "
                                 "(git checkout '%s' || git checkout -b '%s' origin/'%s') && "
                                 "git reset --hard origin/'%s'",
                                 branch_name, branch_name, branch_name, branch_name);
                    } else {
                        strncpy(_cmd_sync_trunk, "fossil update trunk", sizeof(_cmd_sync_trunk) - 1);
                        snprintf(_cmd_sync_branch, sizeof(_cmd_sync_branch),
                                 "fossil update %s", branch_name);
                    }

                    // Commit command (coder uses this as a template)
                    char _cmd_commit_example[256];
                    if (_is_gh)
                        snprintf(_cmd_commit_example, sizeof(_cmd_commit_example),
                                 "git add -A && git commit -m \"Implement %s: <summary>\" "
                                 "&& git push -u origin HEAD",
                                 _tkt_addr);
                    else
                        snprintf(_cmd_commit_example, sizeof(_cmd_commit_example),
                                 "fossil commit -m \"Implement %s: <summary>\"",
                                 a->current_ticket);

                    // Post-commit verification
                    char _cmd_branch_verify[128], _cmd_ws_status[64], _cmd_diff[96];
                    if (_is_gh) {
                        strncpy(_cmd_branch_verify, "git branch --show-current",
                                sizeof(_cmd_branch_verify) - 1);
                        strncpy(_cmd_ws_status, "git status | head -3",
                                sizeof(_cmd_ws_status) - 1);
                        // Sub-issue: diff against parent feature branch; top-level: diff main
                        if (parent_branch_name[0])
                            snprintf(_cmd_diff, sizeof(_cmd_diff),
                                     "git diff '%s'...HEAD", parent_branch_name);
                        else
                            strncpy(_cmd_diff, "git diff main...HEAD", sizeof(_cmd_diff) - 1);
                    } else {
                        strncpy(_cmd_branch_verify, "fossil info | grep tags",
                                sizeof(_cmd_branch_verify) - 1);
                        strncpy(_cmd_ws_status, "fossil status | head -3",
                                sizeof(_cmd_ws_status) - 1);
                        strncpy(_cmd_diff, "fossil diff --from trunk",
                                sizeof(_cmd_diff) - 1);
                    }

                    // Reviewer merge flow.
                    // For sub-issues: merge into the parent feature branch (not main).
                    // The parent branch accumulates all sub-issue merges and is eventually
                    // PR'd to main by the planner after QA.
                    const char *_merge_base = parent_branch_name[0] ? parent_branch_name : "main";
                    char _cmd_merge[512], _cmd_after_merge[256];
                    if (_is_gh) {
                        if (parent_branch_name[0]) {
                            snprintf(_cmd_merge, sizeof(_cmd_merge),
                                     "git fetch origin && git checkout '%s' && git pull origin '%s' && \\\n"
                                     "  git merge --no-ff '%s' -m \"Merge %s: %s\" && git push origin '%s'",
                                     parent_branch_name, parent_branch_name,
                                     branch_name, branch_name, tkt_info.title,
                                     parent_branch_name);
                        } else {
                            snprintf(_cmd_merge, sizeof(_cmd_merge),
                                     "git checkout main && git pull origin main && \\\n"
                                     "  git merge --no-ff '%s' -m \"Merge %s: %s\" "
                                     "&& git push origin main",
                                     branch_name, branch_name, tkt_info.title);
                        }
                        strncpy(_cmd_after_merge, "(already pushed in merge step above)",
                                sizeof(_cmd_after_merge) - 1);
                    } else {
                        snprintf(_cmd_merge, sizeof(_cmd_merge),
                                 "fossil update trunk\nfossil merge %s", branch_name);
                        snprintf(_cmd_after_merge, sizeof(_cmd_after_merge),
                                 "fossil commit -m \"Merge %s: %s\"\n"
                                 "fossil tag add closed %s tip",
                                 branch_name, tkt_info.title, branch_name);
                    }
                    (void)_merge_base;

                    // Sub-ticket creation example (planner Phase 2)
                    char _cmd_create_sub[768];
                    if (_is_gh)
                        snprintf(_cmd_create_sub, sizeof(_cmd_create_sub),
                                 "# Ensure the assignee label exists first (required before --label can use it)\n"
                                 "gh label create 'assignee:<full agent hash from AVAILABLE AGENTS>' -R '%s' --color '0075CA' --force > /dev/null 2>&1\n"
                                 "gh issue create -R '%s' \\\n"
                                 "  --title \"<sub-task title>\" \\\n"
                                 "  --body \"[parent:%s] <sub-task description>\" \\\n"
                                 "  --label 'status:Planned' \\\n"
                                 "  --label 'assignee:<full agent hash from AVAILABLE AGENTS>'",
                                 g_backend->repo_id, g_backend->repo_id, a->current_ticket);
                    else
                        snprintf(_cmd_create_sub, sizeof(_cmd_create_sub),
                                 "fossil ticket add title \"<sub-task title>\" \\\n"
                                 "  comment \"[parent:%s] <sub-task description>\" \\\n"
                                 "  status \"Planned\" \\\n"
                                 "  private_contact \"<full agent hash from AVAILABLE AGENTS>\"",
                                 a->current_ticket);

                    // PR creation command: planner runs this after QA approval (GitHub only).
                    // Creates a PR from the parent feature branch to main for human review.
                    char _cmd_create_pr[512] = {0};
                    char _qa_pr_step[700]    = {0};
                    if (_is_gh && a->doing_qa) {
                        snprintf(_cmd_create_pr, sizeof(_cmd_create_pr),
                                 "gh pr create -R '%s' --base main --head '%s' \\\n"
                                 "  --title \"[MEI] %s\" \\\n"
                                 "  --body \"All sub-tasks completed and QA passed. "
                                 "Human review and merge to main required.\"",
                                 g_backend->repo_id, branch_name, tkt_info.title);
                        snprintf(_qa_pr_step, sizeof(_qa_pr_step),
                                 "   Then open a Pull Request to main for human review:\n"
                                 "     %s\n"
                                 "   !! The PR must be reviewed and merged by a human.\n"
                                 "   !! The orchestrator will NOT monitor or merge the PR.\n",
                                 _cmd_create_pr);
                    }

                    // Dependency sequencing note
                    char _dep_seq_note[512];
                    if (_is_gh)
                        snprintf(_dep_seq_note, sizeof(_dep_seq_note),
                                 "SEQUENCING — if sub-task B must wait for A, add to B's --body:\n"
                                 "  [depends:#<number printed by gh for A, e.g. #42>]\n"
                                 "  Use the exact issue number returned by `gh issue create`.");
                    else
                        strncpy(_dep_seq_note,
                                "SEQUENCING — if sub-task B must wait for A, add to B's comment:\n"
                                "  [depends:<UUID printed by fossil for A>]\n"
                                "  Use the exact 40-char hex UUID. NEVER use placeholders.",
                                sizeof(_dep_seq_note) - 1);

                    // Workspace type label
                    const char *_ws_type = _is_gh ? "git clone" : "Fossil checkout";

                    // Phase 1 plan-writing instruction
                    char _cmd_write_plan[512];
                    if (_is_gh)
                        snprintf(_cmd_write_plan, sizeof(_cmd_write_plan),
                                 "gh issue comment %s -R '%s' --body \"\n"
                                 "## Proposed Plan\n"
                                 "\n"
                                 "### Sub-task 1: <title>\n"
                                 "- Assigned to: <agent name — from AVAILABLE AGENTS below>\n"
                                 "- Description: <what exactly must be done>\n"
                                 "- Depends on: <none, or Sub-task N>\n"
                                 "[repeat for each sub-task]\n"
                                 "\n"
                                 "### Execution Order\n"
                                 "<sequential vs parallel rationale>\n"
                                 "\n"
                                 "### Risks and Success Criteria\n"
                                 "<key risks and how to measure success>\"",
                                 _tkt_addr, g_backend->repo_id);
                    else
                        snprintf(_cmd_write_plan, sizeof(_cmd_write_plan),
                                 "fossil ticket set %s comment \"[original description]\n"
                                 "\n"
                                 "---\n"
                                 "## Proposed Plan\n"
                                 "\n"
                                 "### Sub-task 1: <title>\n"
                                 "- Assigned to: <agent name — from AVAILABLE AGENTS below>\n"
                                 "- Description: <what exactly must be done>\n"
                                 "- Depends on: <none, or Sub-task N>\n"
                                 "[repeat for each sub-task]\n"
                                 "\n"
                                 "### Execution Order\n"
                                 "<sequential vs parallel rationale>\n"
                                 "\n"
                                 "### Risks and Success Criteria\n"
                                 "<key risks and how to measure success>\"",
                                 a->current_ticket);

                    // Approval stop note
                    const char *_approval_stop = _is_gh
                        ? "!! STOP. The human will read the plan in the GitHub issue comments.\n"
                          "!! To approve: change the issue label to status:Verified.\n"
                          "!! To reject: change the label back to status:Open (you will re-plan).\n"
                          "!! Your task is complete when the label is 'status:Pending Approval'.\n"
                        : "!! STOP. The human will read the plan in the Fossil web UI.\n"
                          "!! To approve: set status to 'Verified' — you will create sub-tickets.\n"
                          "!! To request changes: set status back to 'Open' — you will re-plan.\n"
                          "!! Your task is complete when status is 'Pending Approval'.\n";

                    // For GitHub, replace the Fossil wiki_cmd with a GitHub issue comment.
                    if (_is_gh) {
                        snprintf(wiki_cmd, sizeof(wiki_cmd),
                                 "gh issue comment %s -R '%s' \\\n"
                                 "  --body \"[log] $(date -u +%%Y-%%m-%%dT%%H:%%M:%%SZ) | %s | <notes here>\"",
                                 _tkt_addr, g_backend->repo_id, a->name);
                    }
                    // (For Fossil, wiki_cmd was already set correctly above)

                    // Phase detection: a->doing_phase2 is set when the ticket was
                    // "Verified" at routing time (human approved the plan).
                    // Otherwise the ticket was "Open" (fresh) → Phase 1.
                    char planner_task[4096] = {0};
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
                                 "     %s\n"
                                 "   Do NOT change the assignee — the original agent picks it up.\n"
                                 "5. If truly unresolvable without human input, leave status as \"Blocked\"\n"
                                 "   and document \"ESCALATION: <exact reason>\" in the wiki.\n"
                                 "!! DO NOT create sub-tickets. DO NOT reassign the ticket.\n",
                                 wiki_page, wiki_cmd,
                                 _cmd_set_planned);
                    } else if (a->doing_qa) {
                        snprintf(planner_task, sizeof(planner_task),
                                 "ALL SUB-TASKS COMPLETED — QUALITY ASSESSMENT REQUIRED\n\n"
                                 "Every sub-ticket for this parent task has been delivered and approved\n"
                                 "by the reviewer. Your job now is to evaluate the overall quality of\n"
                                 "the complete deliverable before closing the task.\n\n"
                                 "STEP 1 — REVIEW all sub-tickets listed in EXISTING SUB-TICKETS above.\n"
                                 "   For each sub-ticket, read its audit log to understand what was built\n"
                                 "   and what the reviewer found. Check the discussion history for issues.\n\n"
                                 "STEP 2 — INSPECT the actual deliverables in your workspace:\n"
                                 "     %s\n"
                                 "   Read the files that were committed. Run the build if applicable.\n"
                                 "   Verify the deliverables satisfy the ORIGINAL ticket description.\n\n"
                                 "STEP 3 — DOCUMENT your quality assessment in wiki page \"%s\":\n"
                                 "%s\n"
                                 "   Write a section titled '## Quality Assessment' containing:\n"
                                 "   - Overall verdict: APPROVED or NEEDS IMPROVEMENT\n"
                                 "   - Per sub-task: what was delivered and whether it is acceptable\n"
                                 "   - Any gaps, integration issues, or technical debt introduced\n"
                                 "   - If NEEDS IMPROVEMENT: exact issues and what sub-tasks must fix them\n\n"
                                 "STEP 4a — If ALL deliverables are satisfactory:\n"
                                 "     %s\n"
                                 "%s"
                                 "   This closes the parent issue permanently.\n\n"
                                 "STEP 4b — If quality needs improvement, create fix sub-tickets:\n"
                                 "   REUSE existing work — do not re-implement what is already correct.\n"
                                 "   Only create sub-tickets for the specific gaps found.\n"
                                 "   Run once per fix sub-task:\n"
                                 "     %s\n"
                                 "   Then restore monitoring so the orchestrator tracks the new sub-tasks:\n"
                                 "     %s\n"
                                 "   The orchestrator will run another quality assessment when the\n"
                                 "   new sub-tasks are all Done.\n"
                                 "!! Do NOT set status to 'Done' when creating fix sub-tickets.\n"
                                 "!! Do NOT create review sub-tickets — review is automatic.\n",
                                 _cmd_sync_trunk,
                                 wiki_page, wiki_cmd,
                                 _cmd_set_done, _qa_pr_step,
                                 _cmd_create_sub,
                                 _cmd_set_delegated);
                    } else if (subtasks_ctx[0]) {
                        snprintf(planner_task, sizeof(planner_task),
                                 "*** SUB-TICKETS ALREADY EXIST — DO NOT CREATE MORE ***\n"
                                 "Creating additional sub-tickets would produce duplicates. Your task:\n\n"
                                 "1. Review the EXISTING SUB-TICKETS listed above to understand current state.\n"
                                 "2. If planning notes are absent from the audit log for \"%s\", add them now:\n"
                                 "%s\n"
                                 "   Write why this decomposition was chosen, key risks, and success criteria.\n"
                                 "3. Set the parent ticket to Delegated so the orchestrator can monitor\n"
                                 "   sub-task completion and trigger a quality assessment when all are done:\n"
                                 "     %s\n"
                                 "   Do NOT set it to Done — quality assessment happens automatically\n"
                                 "   after all sub-tickets are finished.\n",
                                 wiki_page, wiki_cmd,
                                 _cmd_set_delegated);
                    } else if (a->doing_phase2) {
                        snprintf(planner_task, sizeof(planner_task),
                                 "THE HUMAN HAS APPROVED YOUR PLAN — CREATE SUB-TICKETS NOW.\n"
                                 "Read the '--- DISCUSSION HISTORY ---' section above for your plan.\n"
                                 "Run every command immediately without narrating.\n\n"
                                 "STEP 1 — CREATE SUB-TICKETS from the plan:\n"
                                 "For each sub-task run:\n"
                                 "  %s\n\n"
                                 "YOUR OWN HASH IS %s — NEVER assign a sub-ticket to yourself.\n"
                                 "Executors: coder, researcher. NEVER create a review sub-ticket —\n"
                                 "review is automatic when a coder sets a ticket to 'Review'.\n\n"
                                 "%s\n\n"
                                 "STEP 2 — DOCUMENT in the audit log for \"%s\":\n"
                                 "%s\n"
                                 "  - Which sub-tasks were created and assigned to which agents\n\n"
                                 "STEP 3 — SET parent to Delegated:\n"
                                 "     %s\n",
                                 _cmd_create_sub, a->hash,
                                 _dep_seq_note,
                                 wiki_page, wiki_cmd,
                                 _cmd_set_delegated);
                    } else {
                        snprintf(planner_task, sizeof(planner_task),
                                 "PHASE 1 — WRITE YOUR PROPOSED PLAN INTO THE TICKET (NO SUB-TICKETS YET)\n\n"
                                 "!! BEFORE PLANNING: read the '--- HUMAN REMARKS ---' section in the\n"
                                 "!! TICKET above. Every entry there is a requirement or feedback the human\n"
                                 "!! added via the web UI. You MUST incorporate ALL of them into the plan.\n"
                                 "!! If '--- HUMAN REMARKS ---' is not '(none)', your plan MUST address\n"
                                 "!! each remark with at least one dedicated sub-task.\n\n"
                                 "Design the decomposition (including all human remarks), then write\n"
                                 "your plan directly into the ticket so the human can review it.\n\n"
                                 "STEP 1 — WRITE THE PLAN using ONE command (literal newlines OK):\n\n"
                                 "     %s\n\n"
                                 "  YOUR OWN HASH IS %s — NEVER assign a sub-ticket to yourself.\n"
                                 "  NEVER plan a review sub-ticket — review is automatic.\n"
                                 "  Avoid double-quote characters inside the plan text.\n\n"
                                 "STEP 2 — SUBMIT for human review:\n"
                                 "     %s\n\n"
                                 "%s",
                                 _cmd_write_plan, a->hash,
                                 _cmd_set_pa,
                                 _approval_stop);
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
                            "3. DOCUMENT the decision in the audit log BEFORE acting: what you chose, why,\n"
                            "   and what alternatives you considered. This is the audit trail.\n"
                            "4. CONTINUE — proceed with your chosen approach.\n"
                            "Escalate to BLOCKED only if the decision is architectural (affects the\n"
                            "whole system design) AND you genuinely cannot pick a path without external\n"
                            "input — for example, a fundamental scope conflict or missing external\n"
                            "resource (credentials, access rights you do not have):\n"
                            "  Set the ticket status to 'Blocked' (use the appropriate command for your backend)\n"
                            "  (document question in audit log — the planner will read it and unblock you)\n"
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
                        // Set intent, state and next_action based on planner phase
                        if (a->doing_qa) {
                            strcpy(pmsg.intent, "Quality Assessment — Evaluate All Deliverables");
                            strcpy(pmsg.current_state, "Quality Assessment");
                            strcpy(pmsg.next_action,
                                   "Review deliverables, assess quality — mark Done if satisfied or create fix sub-tickets and set to Delegated");
                        } else if (a->doing_phase2) {
                            strcpy(pmsg.intent, "Execute Approved Plan — Create Sub-Tickets Now");
                            strcpy(pmsg.current_state, "Executing Approved Plan");
                            strcpy(pmsg.next_action,
                                   "Create sub-tickets from plan in discussion history, set parent to Delegated");
                        } else if (subtasks_ctx[0]) {
                            strcpy(pmsg.intent, "Sub-Tickets Exist — Document and Delegate");
                            strcpy(pmsg.current_state, "Delegating");
                            strcpy(pmsg.next_action,
                                   "Document decomposition in wiki, set parent to Delegated for quality monitoring");
                        } else {
                            strcpy(pmsg.intent, "Propose Plan — Write to Ticket, Await Human Approval");
                            strcpy(pmsg.current_state, "Planning (Phase 1 — Awaiting Approval)");
                            strcpy(pmsg.next_action,
                                   "Write plan into ticket comment, set status to Pending Approval");
                        }
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
                      }
                    } else if (strcmp(a->role, "coder") == 0) {
                        // Build concrete assignee command for handing off to reviewer
                        char _coder_assign_reviewer[768];
                        snprintf(_coder_assign_reviewer, sizeof(_coder_assign_reviewer),
                                 _cmd_set_assignee_fmt, reviewer_hash, reviewer_hash);

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
                                 "Your working directory /tmp/workspaces/%s IS a valid %s.\n"
                                 "Do NOT present options or ask permission — just run the commands.\n"
                                 "The task is complete only when the ticket status has been updated.\n\n"
                                 "1. READ the discussion history above carefully before starting — it contains\n"
                                 "   all prior reviewer rejections with their exact reasons. Address EVERY\n"
                                 "   issue raised in previous cycles, not just the latest one.\n"
                                 "2. The orchestrator already created and switched your workspace to branch %s.\n"
                                 "   Confirm with:\n"
                                 "     %s\n"
                                 "   The output MUST show branch '%s'.\n"
                                 "   else, run `%s` to correct it before writing any code.\n"
                                 "   !! Never commit to the main branch — that bypasses review entirely.\n"
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
                                 "     %s\n"
                                 "   After committing, verify the commit landed on the right branch:\n"
                                 "     %s\n"
                                 "   If it shows the main branch, your commit went to the wrong place — STOP\n"
                                 "   and set the ticket to Blocked with an explanation.\n"
                                 "7. DOCUMENT your implementation in the audit log for \"%s\" — MANDATORY\n"
                                 "   before submitting. The reviewer and future agents will read this:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Approach taken and why (not just what, but why this design)\n"
                                 "   - Key decisions and alternatives you considered and discarded\n"
                                 "   - Exact commands used to build and test, and their output/result\n"
                                 "   - Known limitations, assumptions, or technical debt introduced\n"
                                 "   - If this is a rework: what specifically changed from the previous attempt\n"
                                 "8. Submit for review only AFTER documenting:\n"
                                 "     %s\n"
                                 "     %s\n"
                                 "Reviewer hash: %s",
                                 persona_section,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first attempt)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 tkt_info.reviewer_notes[0] ? tkt_info.reviewer_notes : "(none - first attempt)",
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 a->name, _ws_type,
                                 branch_name,
                                 _cmd_ws_status,
                                 branch_name,
                                 _cmd_sync_branch,
                                 _cmd_commit_example,
                                 _cmd_branch_verify,
                                 wiki_page, wiki_cmd,
                                 _cmd_set_review,
                                 _coder_assign_reviewer,
                                 reviewer_hash);
                        strcpy(pmsg.current_state, "Coding");
                        strcpy(pmsg.next_action,
                               "Create branch, implement fully, build+verify, commit, then set ticket to Review");
                    } else if (strcmp(a->role, "reviewer") == 0 || a->doing_review) {
                        // Build concrete assignee command for returning ticket to coder
                        char _reviewer_assign_coder[768];
                        snprintf(_reviewer_assign_coder, sizeof(_reviewer_assign_coder),
                                 _cmd_set_assignee_fmt, rework_coder_hash, rework_coder_hash);

                        strcpy(pmsg.intent, "Review, Verify, and Merge or Reject");
                        snprintf(pmsg.context, sizeof(pmsg.context),
                                 "%s"
                                 "=== TICKET ===\n"
                                 "%s\n\n"
                                 "=== DISCUSSION HISTORY (all prior review cycles on this ticket) ===\n%s\n\n"
                                 "=== PARENT TICKET CONTEXT ===\n%s\n"
                                 "=== SUB-TICKETS ===\n%s\n"
                                 "=== YOUR TASK (REVIEWER) — BE STRICT ===\n"
                                 "!! EXECUTE EVERY STEP BELOW IN ORDER — no skipping, no narrating. !!\n"
                                 "1. READ the discussion history above — if there were prior rejections, verify\n"
                                 "   that EVERY previously reported issue has been fully addressed.\n"
                                 "2. Switch to the implementation branch (run these commands NOW):\n"
                                 "     %s\n"
                                 "   Then verify you are on the correct branch:\n"
                                 "     %s\n"
                                 "   The output MUST show '%s'.\n"
                                 "   If NOT — do not proceed with the review. Set the ticket to Blocked\n"
                                 "   with reason: \"Cannot checkout branch %s\".\n"
                                 "3. Verify the coder's work exists:\n"
                                 "     %s\n"
                                 "   If the diff is EMPTY, run: git log --oneline %s..HEAD\n"
                                 "   No commits = nothing to review → set ticket to Rework:\n"
                                 "   \"No code found on branch %s — coder did not push changes.\"\n"
                                 "4. Read EVERY file that was changed (from the diff above).\n"
                                 "5. BUILD the code on the branch — this is the first gate, non-negotiable:\n"
                                 "   Identify the build system from the repository root (look for Makefile,\n"
                                 "   Cargo.toml, go.mod, package.json, build.gradle, CMakeLists.txt, etc.)\n"
                                 "   and run the appropriate build command (e.g. `make`, `cargo build`,\n"
                                 "   `go build ./...`, `npm run build`). Capture the full output.\n"
                                 "   !! If the build exits with ANY errors → REJECT immediately. Do not\n"
                                 "   read further. Broken code must never be approved.\n"
                                 "   Record the exact build command used and its full output in the audit log.\n"
                                 "6. Verify EACH requirement in the ticket description is fully satisfied:\n"
                                 "   - If the ticket asks for specific files, check they exist and are non-trivial.\n"
                                 "   - Run the resulting binary and verify it behaves as specified.\n"
                                 "   - Reject placeholder/stub code (empty functions, hardcoded values, TODOs).\n"
                                 "   - Reject if names are nonsensical (agent names, temp names, test values).\n"
                                 "7. DOCUMENT your full review findings in the audit log for \"%s\" — MANDATORY\n"
                                 "   before taking any action. Future agents depend on this record:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Every file you reviewed and what you found\n"
                                 "   - The EXACT output of `make 2>&1` (pass or fail with errors)\n"
                                 "   - Whether each requirement in the ticket spec was met (yes/no + evidence)\n"
                                 "   - VERDICT: APPROVED or REJECTED\n"
                                 "   - If REJECTED: specific issues with file names, line numbers, exact errors\n"
                                 "8. If ALL requirements met AND build passed — merge into '%s':\n"
                                 "     %s\n"
                                 "   Then re-run the same build command you used in step 5 to verify the\n"
                                 "   merged result still compiles cleanly.\n"
                                 "   !! If the build fails after the merge, the branch introduced\n"
                                 "   incompatibilities. Undo the merge, then REJECT the ticket with the\n"
                                 "   exact build error as the rejection reason.\n"
                                 "   If the merged build succeeds, finalize and close:\n"
                                 "     %s\n"
                                 "     %s\n"
                                 "9. If ANYTHING is incomplete or wrong — AFTER documenting in the audit log:\n"
                                 "   Record the summary rejection reason (coder will read this):\n"
                                 "     %s\n"
                                 "   Return for rework:\n"
                                 "     %s\n"
                                 "     %s\n"
                                 "Coder hash (for rework): %s\n"
                                 "REMEMBER: approving bad code harms the project. When in doubt, reject.",
                                 persona_section,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first review)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 subtasks_ctx[0] ? subtasks_ctx : "  (none)\n",
                                 _cmd_sync_branch,
                                 _cmd_branch_verify,
                                 branch_name,
                                 branch_name,
                                 _cmd_diff,
                                 _merge_base,   // git log --oneline <base>..HEAD
                                 branch_name,
                                 wiki_page, wiki_cmd,
                                 _merge_base,   // "merge into '<base>'"
                                 _cmd_merge,
                                 _cmd_after_merge,
                                 _cmd_set_done,
                                 _cmd_notes_coder,
                                 _cmd_set_rework,
                                 _reviewer_assign_coder,
                                 rework_coder_hash);
                        strcpy(pmsg.current_state, "Reviewing");
                        strcpy(pmsg.next_action,
                               "Read all changes, build/run code, verify every requirement — merge only if fully satisfied, otherwise Rework");
                    } else {
                        // Build concrete assignee command for handing off to reviewer
                        char _researcher_assign_reviewer[768];
                        snprintf(_researcher_assign_reviewer, sizeof(_researcher_assign_reviewer),
                                 _cmd_set_assignee_fmt, reviewer_hash, reviewer_hash);

                        // Build researcher commit command
                        char _cmd_research_commit[256];
                        if (_is_gh)
                            snprintf(_cmd_research_commit, sizeof(_cmd_research_commit),
                                     "git add -A && git commit -m \"Research %s: <summary>\" "
                                     "&& git push -u origin HEAD",
                                     _tkt_addr);
                        else
                            snprintf(_cmd_research_commit, sizeof(_cmd_research_commit),
                                     "fossil commit -m \"Research %s: %s\"",
                                     a->current_ticket, tkt_info.title);

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
                                 "Run all commands inline. The task is complete only when the\n"
                                 "ticket status has been updated.\n\n"
                                 "1. READ the ticket description and discussion history carefully.\n"
                                 "   Understand exactly what deliverables are required.\n"
                                 "2. RESEARCH thoroughly — use all available tools:\n"
                                 "   - Read files in the repository (%s first)\n"
                                 "   - Search documentation, run commands, inspect data\n"
                                 "   - Gather ALL information the ticket asks for\n"
                                 "3. PRODUCE the required deliverables as files in your workspace:\n"
                                 "   - Write markdown documents, Python scripts, data files as needed\n"
                                 "   - Every deliverable must be non-trivial and complete\n"
                                 "   - Use meaningful names derived from the ticket title\n"
                                 "4. COMMIT your deliverables:\n"
                                 "     %s\n"
                                 "5. DOCUMENT your findings in the audit log for \"%s\" — MANDATORY:\n"
                                 "%s\n"
                                 "   Write a markdown section with:\n"
                                 "   - Summary of what was researched and key findings\n"
                                 "   - Every source consulted and what was learned from each\n"
                                 "   - Files produced: name, purpose, and key contents\n"
                                 "   - Open questions or limitations in the findings\n"
                                 "6. Submit for review AFTER documenting:\n"
                                 "     %s\n"
                                 "     %s\n"
                                 "Reviewer hash: %s",
                                 persona_section,
                                 tkt_full,
                                 discussion_log[0] ? discussion_log : "  (no history yet — this is the first attempt)\n",
                                 parent_ctx[0] ? parent_ctx : "  (none — this is a top-level ticket)\n",
                                 dep_ctx[0] ? dep_ctx : "  (none)\n",
                                 _cmd_sync_trunk,
                                 _cmd_research_commit,
                                 wiki_page, wiki_cmd,
                                 _cmd_set_review,
                                 _researcher_assign_reviewer,
                                 reviewer_hash);
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
                    g_backend->ticket_append_log(g_backend, a->current_ticket, a->name, wiki_msg);
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
                g_backend->ticket_set_status(g_backend, a->current_ticket, "Blocked");
                char note[256];
                snprintf(note, sizeof(note),
                         "BLOCKED: agent %s stalled after %d ticks (~%d min). Manual intervention required.",
                         a->name, MAX_STEPS_PER_TICKET, (MAX_STEPS_PER_TICKET * TICK_INTERVAL_MS) / 60000);
                g_backend->ticket_add_note(g_backend, a->current_ticket, note);
                char wiki_msg[256];
                snprintf(wiki_msg, sizeof(wiki_msg),
                         "(%s) **STALLED** after %d ticks (~%d min) without completing. Ticket set to Blocked — human intervention required.",
                         a->role, MAX_STEPS_PER_TICKET, (MAX_STEPS_PER_TICKET * TICK_INTERVAL_MS) / 60000);
                g_backend->ticket_append_log(g_backend, a->current_ticket, a->name, wiki_msg);
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
                // before executing VCS commands. A short threshold would interrupt
                // generation and cause premature ticket closure.
                else if (ticket_steps[i] >= NUDGE_MIN_TICKS &&
                         pane_idle_ticks[i] >= (strcmp(a->role, "planner") == 0
                                                ? NUDGE_IDLE_TICKS_PLANNER
                                                : NUDGE_IDLE_TICKS) &&
                         pane_idle_ticks[i] % NUDGE_REPEAT_TICKS == 0) {
                    char nudge[512];
                    int _nudge_gh = (g_backend && g_backend->type == VCS_GITHUB);
                    if (strcmp(a->role, "planner") == 0) {
                        if (_nudge_gh)
                            snprintf(nudge, sizeof(nudge),
                                     "Issue %s is still In Progress. You have already done your analysis.\n"
                                     "Now EXECUTE — run the gh/git commands from your task instructions.\n"
                                     "Do NOT explain further. Run every command now, one by one.\n"
                                     "The task ends only when the GitHub issue label has been updated\n"
                                     "(to 'status:Pending Approval', 'status:Delegated', 'status:Done',\n"
                                     "or 'status:Planned' depending on the phase — see YOUR TASK above).",
                                     a->current_ticket);
                        else
                            snprintf(nudge, sizeof(nudge),
                                     "Ticket %s is still In Progress. You have already done your analysis.\n"
                                     "Now EXECUTE — run the fossil commands from your task instructions.\n"
                                     "Do NOT explain further. Run every command now, one by one.\n"
                                     "The task ends only when the Fossil ticket status has been updated\n"
                                     "(to 'Pending Approval', 'Delegated', 'Done', or 'Planned' depending\n"
                                     "on which phase you are in — see YOUR TASK above).",
                                     a->current_ticket);
                    } else {
                        if (_nudge_gh)
                            snprintf(nudge, sizeof(nudge),
                                     "Issue %s is still In Progress on GitHub — your task is NOT complete.\n"
                                     "Your working directory is a valid git checkout. Execute NOW:\n"
                                     "1. Run all pending gh/git commands (commit, push, gh issue edit --add-label).\n"
                                     "2. Do NOT ask for confirmation — just execute every step immediately.\n"
                                     "3. The task ends only when the GitHub issue label has been updated.",
                                     a->current_ticket);
                        else
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
                    if (strncmp(tickets[t].uuid, a->current_ticket,
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

    // Orphaned ticket recovery: a ticket "In Progress" with no agent owning it is a
    // deadlock. This can happen if a false-positive still_active=0 reset an agent but
    // left the ticket status unchanged. Detect and reset immediately.
    for (int t = 0; t < tkt_count; t++) {
        if (strcasecmp(tickets[t].status, "In Progress") != 0) continue;

        int is_owned = 0;
        for (int i = 0; i < agent_count; i++) {
            if (agents[i].state == AGENT_STATE_IN_PROGRESS &&
                strcmp(agents[i].current_ticket, tickets[t].uuid) == 0) {
                is_owned = 1;
                break;
            }
        }
        if (is_owned) continue;

        // Orphaned. Determine recovery state: if any agent submitted this ticket for
        // review (pending_review_ticket is set), reset to Review so the reviewer picks
        // it up. Otherwise reset to Planned so the coder retries from scratch.
        const char *recovery_status = "Planned";
        for (int i = 0; i < agent_count; i++) {
            if (strcmp(agents[i].pending_review_ticket, tickets[t].uuid) == 0) {
                recovery_status = "Review";
                break;
            }
        }

        char orphan_log[256];
        snprintf(orphan_log, sizeof(orphan_log),
                 "[orphan] Ticket %.10s In Progress but no agent owns it — resetting to %s",
                 tickets[t].uuid, recovery_status);
        log_message(orphan_log);

        g_backend->ticket_assign(g_backend, tickets[t].uuid, "");
        g_backend->ticket_set_status(g_backend, tickets[t].uuid, recovery_status);
        strncpy(tickets[t].status,   recovery_status, sizeof(tickets[t].status) - 1);
        tickets[t].assignee[0] = '\0';
    }

    // QA promotion: when all sub-tickets of a "Delegated" parent ticket are Done,
    // promote the parent to "QA Ready" so the planner can assess overall quality.
    // Rate-limit: one log entry per promotion (status change prevents re-entry).
    for (int t = 0; t < tkt_count; t++) {
        if (strcasecmp(tickets[t].status, "Delegated") != 0) continue;

        char parent_tag[72];
        snprintf(parent_tag, sizeof(parent_tag), "[parent:%s]", tickets[t].uuid);

        int sub_count     = 0;
        int pending_count = 0;
        for (int t2 = 0; t2 < tkt_count; t2++) {
            if (t2 == t) continue;
            if (!strstr(tickets[t2].description, parent_tag)) continue;
            sub_count++;
            if (strcasecmp(tickets[t2].status, "Done")   != 0 &&
                strcasecmp(tickets[t2].status, "closed") != 0) {
                pending_count++;
            }
        }

        if (sub_count > 0 && pending_count == 0) {
            g_backend->ticket_set_status(g_backend, tickets[t].uuid, "QA Ready");
            strncpy(tickets[t].status, "QA Ready", sizeof(tickets[t].status) - 1);
            char qa_log[256];
            snprintf(qa_log, sizeof(qa_log),
                     "[qa] All %d sub-tickets done for parent %.10s — promoted to QA Ready",
                     sub_count, tickets[t].uuid);
            log_message(qa_log);
            g_backend->ticket_append_log(g_backend, tickets[t].uuid, "orchestrator",
                                         "All sub-tasks completed and approved. "
                                         "Parent ticket promoted to **QA Ready** — "
                                         "planner will assess overall quality before closing.");
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
