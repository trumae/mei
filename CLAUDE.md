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
- Intent flags on Agent struct: `resolving_block` (handling Blocked), `doing_review` (non-reviewer executing a planner-assigned review task)
- `deps_satisfied` treats both `"Done"` and `"closed"` as terminal — agents sometimes set ticket status to `"closed"` instead of `"Done"`

## Review Routing
- Any role (coder, researcher, etc.) accepts a `Review`-status ticket when `is_delegated` (planner explicitly assigned it).
- When a non-reviewer agent finishes and sets its ticket to `Review` (normal work submission), the orchestrator clears `private_contact` so the reviewer picks it up as an unassigned ticket.
- Reviewer only picks up Review tickets that are unassigned or directly delegated to it; if any known agent is assignee, reviewer skips.
- Non-reviewer acting as reviewer receives the same PULSE template as the reviewer role (`a->doing_review` triggers this).

## Agent Decision & Unblocking Protocol
Executor agents (coder, reviewer, researcher) receive a **DECISION PROTOCOL** section in every
PULSE that instructs them to decide autonomously, document the choice in the wiki, and continue.
They escalate to `BLOCKED` only for genuine architectural blockers (scope conflicts, missing
external credentials) — not for ordinary ambiguity.

When a ticket is set to `Blocked`, the planner picks it up automatically:
1. **Planner routing** (`orchestrator.c:role_accepts`): planner accepts `Open` (non-sub-task) OR
   any `Blocked` ticket. The flag `Agent.resolving_block` carries this intent to the PULSE phase.
2. **Planner PULSE** (`orchestrator.c` IN_PROGRESS build): if `resolving_block`, sends
   `"Resolve Blocked Ticket"` intent instead of the normal decomposition task. Task instructs the
   planner to identify the blocker from the discussion history, decide, document in wiki, and reset
   ticket status to `"Planned"` (keeping the original `private_contact`).
3. **Auto-unblock** (`orchestrator.c` BLOCKED agent handling): on each tick, if the blocked
   agent's ticket status is no longer `"Blocked"` in Fossil (planner resolved it), the agent is
   reset to `AGENT_STATE_OPEN` and picks up its ticket on the next routing cycle.

Stall-timeout blocks (agent exceeded `MAX_STEPS_PER_TICKET`) also route to the planner.
If the planner cannot resolve it, it leaves status `"Blocked"` with `ESCALATION: <reason>` in
the wiki, requiring human intervention.
