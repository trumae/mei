# MEI — Multi-Agent Environment Integrator - in brazilian portuguese: Micro Empreendedor Individual :)

[![License](https://img.shields.io/badge/license-BSD%202--Clause-blue.svg)](LICENSE)

A stateless C99 orchestrator that coordinates autonomous AI agents using **Fossil SCM** as its single source of truth and **Tmux** for process isolation.

MEI treats your Fossil repository as the only authoritative state store. The orchestrator binary itself holds nothing in memory that matters — restart it at any time and every agent resumes exactly where it left off.

---

## Core Concepts

### Stateless Orchestration

The orchestrator runs a tight tick loop (every 2 seconds) that reads the current world state from Fossil, computes what needs to happen, and acts. No database, no daemon, no side-channel storage. Kill the process, restart it, and work continues seamlessly.

### Fossil as the Database

Tickets are tasks. Each ticket has a status, an assignee (`private_contact`), and a changelog (stored in a custom column). The orchestrator queries the live Fossil SQLite file directly and writes back via `fossil ticket change`. This gives you full Fossil UI access to everything the agents do — you can inspect, intervene, and approve through the normal Fossil web interface.

### Agents as CLI Processes

An agent is any CLI tool that reads from stdin and outputs to a terminal — Claude Code, OpenCode, a shell script, or a custom binary. MEI spawns each agent in its own Tmux window and communicates via the PULSE protocol.

### PULSE Protocol

When an agent is assigned a ticket, MEI sends a single PULSE message containing the full task context: ticket details, wiki history, parent ticket context, and role-specific instructions. The message is injected via Tmux bracketed-paste mode, so multi-line Markdown arrives safely. After the initial PULSE, the agent works autonomously. If the agent's pane goes idle for more than 60 seconds, MEI sends a lightweight nudge.

### Workspace Isolation

Each agent gets an independent Fossil checkout at `/tmp/workspaces/<agent-name>`. All Fossil operations use the `-R <repo>` out-of-tree flag, so agents never interfere with each other's working copy.

### Wiki as Audit Log

Every significant step is written to a `ticket-<uuid>` wiki page in Fossil. When building the next PULSE, MEI reads this history to provide the agent with full conversational context. The wiki is the memory.

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  MEI Orchestrator                │
│                                                 │
│  ┌──────────┐   tick()   ┌─────────────────┐   │
│  │ ncurses  │ ─────────► │  orchestrator.c  │   │
│  │   TUI    │            │  (routing logic) │   │
│  └──────────┘            └────────┬────────┘   │
│                                   │             │
│         ┌─────────────────────────┤             │
│         ▼                         ▼             │
│  ┌─────────────┐         ┌──────────────────┐  │
│  │ fossil_skill│         │    tmux_mgr.c    │  │
│  │  (ticket    │         │  (spawn / PULSE  │  │
│  │   CRUD)     │         │   / capture)     │  │
│  └──────┬──────┘         └────────┬─────────┘  │
└─────────┼───────────────────────  ┼ ───────────┘
          │                         │
          ▼                         ▼
   ┌─────────────┐          ┌──────────────┐
   │  repo.fossil│          │  Tmux windows│
   │  (tickets,  │          │  (one per    │
   │  wiki, blobs)│         │   agent CLI) │
   └─────────────┘          └──────────────┘
```

**Source layout:**

| File | Responsibility |
|------|---------------|
| `src/cmd/run.c` | `mei run` — launches the TUI and tick loop |
| `src/cmd/new.c` | `mei new` — initialises a fresh Fossil repo |
| `src/cmd/status.c` | `mei status` — prints agent states |
| `src/cmd/agents.c` | `mei agents` — lists loaded agent definitions |
| `src/cmd/log.c` | `mei log` — tails the orchestrator log |
| `src/core/orchestrator.c` | Tick loop, ticket routing, warm-up detection |
| `src/core/agent_mgr.c` | Loads `/.agents/*.md` definitions from Fossil trunk |
| `src/core/fossil_skill.c` | Fossil CLI wrappers (ticket CRUD, wiki, checkout) |
| `src/core/tmux_mgr.c` | Tmux session spawning and PULSE delivery |
| `src/core/pulse.c` | PULSE message formatting |
| `src/ui.c` | ncurses TUI (agent list, details, live log) |

---

## Ticket Lifecycle

Tickets follow a structured flow with a human approval gate and a QA loop:

```
Open
 └─► (planner writes plan) ─► Pending Approval
                                  └─► (human approves in Fossil UI) ─► Verified
                                  └─► (human rejects) ─────────────► Open  (re-plan)
                                        Verified
                                         └─► (planner creates sub-tickets) ─► Delegated
                                                Delegated
                                                 └─► (all sub-tickets Done) ─► QA Ready
                                                        QA Ready
                                                         └─► (planner QA passes) ─► Done
                                                         └─► (QA fails) ─────────► Delegated  (fix loop)
```

**Roles and what they pick up:**

| Role | Ticket conditions |
|------|-------------------|
| Planner | Unassigned `Open`, any `Verified`, any `Blocked`, any `QA Ready` |
| Coder | `Planned` / `Rework` / `Review` delegated to them |
| Reviewer | Unassigned `Review` tickets |
| Others | Unassigned `Open` or explicitly delegated `Planned` / `Rework` |

Agents are identified by the SHA-1 hash of their name stored in the `private_contact` field.

**Safety lock:** an agent that exceeds `MAX_STEPS_PER_TICKET` (1500 ticks ≈ 50 min) is automatically blocked and the ticket is routed to the planner for triage.

---

## Agent Definition Format

Agents are defined as Markdown files stored inside the Fossil repository at `/.agents/*.md`:

```yaml
name: coder-1
role: coder
cli: opencode
cmd: opencode --model big-pickle
capabilities: [coding, fossil-read, fossil-write]
description: |
  You are a senior software engineer working on a quantitative trading system.
  You receive tasks via PULSE messages and document every decision in the wiki.
  ...
```

Fields:

| Field | Description |
|-------|-------------|
| `name` | Unique agent identifier |
| `role` | `planner`, `coder`, `reviewer`, or any custom role |
| `cli` | Name of the CLI binary (used for warm-up detection) |
| `cmd` | Full command to spawn the agent process |
| `capabilities` | Comma-separated list of capabilities (informational) |
| `description` | Full persona / system prompt injected into the PULSE |

---

## Prerequisites

- **GCC** with C99 support
- **ncurses** (`libncurses-dev` on Debian/Ubuntu)
- **Fossil SCM** ≥ 2.18 (must be in `PATH`)
- **Tmux** ≥ 3.0 (must be in `PATH`)
- An AI CLI tool for your agents (e.g. [Claude Code](https://docs.anthropic.com/claude-code), [OpenCode](https://opencode.ai))

---

## Build & Install

```sh
# Build
make

# Install to ~/bin/mei
make install

# Clean build artifacts
make clean
```

The binary embeds the Fossil checkout hash at build time so you can always trace which revision is running.

---

## Usage

### Create a new project

```sh
mei new myproject --dir ./myproject
```

This initialises a Fossil repository, creates the `/.agents/` directory stub, and opens the default workspace.

### Add agent definitions

Adjust `*.md` files into `/.agents/` inside the Fossil trunk (see format above). Commit them.

### Run the orchestrator

```sh
mei run myproject.fossil
```

Add `--clean` to wipe all workspaces and start fresh.

### CLI reference

```
mei run <repo.fossil> [--clean]   Start the TUI and tick loop
mei new <name> --dir <path>       Initialise a new project repo
mei status <repo.fossil>          Print current agent states
mei agents <repo.fossil>          List loaded agent definitions
mei log [-f] [-n N]               Tail the orchestrator log
```

### TUI keyboard shortcuts

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate agent list |
| `p` | Pause selected agent |
| `r` | Resume selected agent |
| `k` | Kill selected agent |
| `a` | Attach to agent's Tmux window |
| `q` | Quit (runs clean shutdown) |

---

## Tests

```sh
# Create a test Fossil repo with example agents
./tests/setup_test_project.sh

# Run the end-to-end integration test
./tests/run_integration_test.sh
```

The integration test spawns a planner and a coder, validates ticket creation, and verifies that PULSE messages are delivered correctly.

---

## Design Philosophy

- **No local state.** The orchestrator is a pure function of Fossil state. You can kill and restart it at any point without losing work.
- **Out-of-tree Fossil.** Every Fossil operation uses `-R <repo>`. Agents never `cd` into a checkout.
- **One PULSE per assignment.** Agents are given everything they need upfront and trusted to work autonomously. Nudges are rare and lightweight.
- **Human in the loop.** The approval gate (`Pending Approval → Verified`) is intentional. Automated planning is never unconditionally trusted.
- **Agents decide, orchestrator routes.** Agents escalate to `BLOCKED` only for genuine blockers (missing credentials, scope conflicts). Ordinary ambiguity is resolved by the agent and documented in the wiki.

---

## Version

Current version: **0.1.0**

---

## License

BSD 2-Clause License. See [LICENSE](LICENSE) for the full text.
