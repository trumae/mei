# MEI - Multi-Agent Environment Integrator

MEI is a C99-based multi-agent orchestrator that uses **Fossil** as the single source of truth and **Tmux** for agent process isolation.

## Build Commands
- Build project: `make`
- Clean build: `make clean`

## Running MEI
- Launch with a Fossil repository: `./bin/orchestrator_ui <path_to_repo.fossil>`
- Launch with workspace cleanup: `./bin/orchestrator_ui <path_to_repo.fossil> --clean`

## Testing
- Setup manual test project: `./tests/setup_test_project.sh`
- Run automated E2E integration test: `./tests/run_integration_test.sh`

## Architecture Principles
1. **Single Source of Truth**: ALL state lives in the Fossil repository (Tickets, Wiki, etc.).
2. **Isolation**: Every agent has a dedicated workspace (`/tmp/workspaces/<agent_name>`) with its own `fossil open`.
3. **Communication**: Orchestrator communicates with agents via the **PULSE Protocol** (JSON/XML envelopes injected into Tmux buffers).
4. **Agent Discovery**: Agents are defined in `/.agents/*.md` files within the Fossil repository (trunk).

## Coding Guidelines
- **C99 Standard**: Use `std=c99` for all core logic.
- **Ncurses UI**: Main interaction loop must handle non-blocking input and trigger the `orchestrator_tick`.
- **Fossil Integration**: Always use the `-R <repo>` flag for out-of-tree Fossil commands to ensure consistency.
- **No Local State**: The orchestrator binary should be stateless; use `orchestrator_tick` to sync state from Fossil.
- **Graceful Shutdown**: Always ensure `orchestrator_shutdown` is called to kill the `mei` tmux session.

## Ticket Mapping
- **Assignee**: Mapped to the `private_contact` field in Fossil tickets.
- **States**: `AGENT_STATE_OPEN` (Idle), `AGENT_STATE_IN_PROGRESS`, `AGENT_STATE_BLOCKED`, `AGENT_STATE_PAUSED`.
- **Step Limits**: Enforce `MAX_STEPS_PER_TICKET` (default: 50) to prevent agent loops.
