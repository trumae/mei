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
