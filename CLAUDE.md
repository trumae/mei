# MEI - Multi-Agent Environment Integrator

C99 multi-agent orchestrator using **Fossil SCM** as the single source of truth and **Tmux** for agent process isolation.

## Commands
- Build: `make` / Clean: `make clean` / Install: `make install`
- Run: `./bin/orchestrator_ui <path_to_repo.fossil>` (add `--clean` to wipe workspaces)
- Test setup: `./tests/setup_test_project.sh`
- E2E integration test: `./tests/run_integration_test.sh`

## Source Layout
- `src/main.c`, `src/agent.c`, `src/ui.c` — entry point and top-level glue
- `src/core/orchestrator.c` — tick loop, state sync from Fossil
- `src/core/agent_mgr.c` — agent lifecycle management
- `src/core/tmux_mgr.c` — Tmux session/pane control
- `src/core/pulse.c` — PULSE Protocol encode/decode
- `src/core/fossil_skill.c` — Fossil CLI wrappers

## Architecture Invariants
- **No local state**: orchestrator binary is stateless; all state lives in Fossil (tickets, wiki).
- **Out-of-tree Fossil**: always use `-R <repo>` flag; never `cd` into a checkout.
- **Workspaces**: each agent gets `/tmp/workspaces/<agent_name>` with its own `fossil open`.
- **Agent discovery**: agents defined in `/.agents/*.md` files in the Fossil trunk.
- **Shutdown**: always call `orchestrator_shutdown` to kill the `mei` tmux session.
- **UI loop**: ncurses input must be non-blocking and trigger `orchestrator_tick` each iteration.

## Fossil Ticket Mapping
- Assignee → `private_contact` field
- States: `AGENT_STATE_OPEN`, `AGENT_STATE_IN_PROGRESS`, `AGENT_STATE_BLOCKED`, `AGENT_STATE_PAUSED`
- Step limit: `MAX_STEPS_PER_TICKET` = 50 (prevents infinite agent loops)
- Intent flags on `Agent` struct: `resolving_block`, `doing_review`, `doing_qa`, `doing_phase2`
- `deps_satisfied` treats both `"Done"` and `"closed"` as terminal — agents sometimes set `"closed"` instead of `"Done"`

## Parent Ticket Lifecycle (Plan Approval + QA Loop)
Status flow for top-level tickets:

`Open` → *(planner Phase 1: writes plan)* → `Pending Approval` → *(human approves in Fossil UI)* → `Verified` → *(planner Phase 2: creates sub-tickets)* → `Delegated` → *(all sub-tickets Done)* → `QA Ready` → *(planner QA)* → `Done`

- Human can reject by setting `Pending Approval` back to `Open` — planner re-plans.
- If QA fails, planner creates fix sub-tickets and resets parent to `Delegated` (loop repeats).
- No agent ever picks up a `Pending Approval` ticket.

## Review Routing
- Non-reviewer submits work by setting ticket to `Review`; orchestrator clears `private_contact` so the reviewer picks it up as unassigned.
- Reviewer skips `Review` tickets assigned to any known agent; only picks up unassigned or planner-delegated ones.
- Any executor role accepts a `Review` ticket when the planner explicitly delegates it (`is_delegated`).

## Agent Decision & Unblocking Protocol
Executors decide autonomously and document choices in the wiki. They escalate to `BLOCKED` only for genuine architectural blockers (scope conflicts, missing external credentials) — never for ordinary ambiguity.

- Planner auto-picks up any `Blocked` ticket and resolves it; resets status to `Planned` keeping the original assignee.
- Stall-timeout (agent exceeded `MAX_STEPS_PER_TICKET`) also routes to the planner.
- If the planner cannot resolve, it writes `ESCALATION: <reason>` in the wiki and leaves the ticket `Blocked` for human intervention.
