# MEI Concepts

This document explains the core concepts behind MEI (Multi-Agent Environment Integrator) — what each part is, why it exists, and how the pieces fit together.

---

## 1. The Orchestrator

The orchestrator (`orchestrator_ui`) is the central process that governs all agents. It is **stateless**: it holds no authoritative state of its own. Every time it needs to know what is happening — which tickets exist, who is assigned to what, which tickets are blocked — it reads from Fossil. This means the binary can be killed and restarted at any time without losing information. The state lives in the repository, not in RAM.

At startup, the orchestrator:
1. Reads agent definitions from `/.agents/*.md` in the Fossil trunk.
2. Creates a workspace for each agent.
3. Opens the Fossil repository in each workspace.
4. Spawns a Tmux session per agent running their CLI command.
5. Syncs in-progress or blocked tickets back to the correct agents.

---

## 2. The Tick (Heartbeat)

MEI does not push work to agents continuously. Instead it operates on a **periodic heartbeat** called the tick, which fires every `TICK_INTERVAL_MS` (2 seconds). On each tick, `orchestrator_tick` is called and:

- Reads the full ticket list from Fossil.
- For each idle agent, checks if there is a ticket matching its role.
- For each working agent, checks if the CLI process is ready (warm-up phase) and, if so, sends a PULSE with the ticket context.
- Detects stalled agents (exceeded `MAX_STEPS_PER_TICKET`) and marks them blocked.

The tick model prevents uncontrolled agent loops: an agent only receives instructions when the orchestrator decides it is time.

---

## 3. Fossil SCM as the Single Source of Truth

**Fossil** is a distributed version control system with built-in ticket tracking and wiki. In MEI, Fossil is not just for code — it is the **database**. All persistent state lives there:

- **Tickets** — units of work (tasks, bugs, features)
- **Wiki pages** — per-ticket audit logs written by agents during execution
- **Trunk** — the `/.agents/*.md` files that define the agent pool
- **Commit history** — code changes made by coder agents

The orchestrator always uses `fossil -R <repo>` (out-of-tree mode) so it never needs to `cd` into a checkout. This keeps the orchestrator process location-independent.

### Key Ticket Fields

| Fossil field      | MEI meaning                                              |
|-------------------|----------------------------------------------------------|
| `title`           | Task description                                         |
| `status`          | Workflow state (Open, Planned, In Progress, Review, ...) |
| `private_contact` | Assigned agent — stored by name or Fossil hash           |
| `comment`         | Task body; `[parent:<uuid>]` marks a sub-ticket          |
| `changelog`       | Orchestrator notes explaining WHY a status changed       |
| `icomment`        | Initial description written via the Fossil web UI        |

---

## 4. Agents

An **agent** is any CLI process that can receive text input via Tmux and act on it. An agent is not a specific program — it is a role bound to a command. Examples include `claude`, `opencode`, or any shell script.

### Agent Definition Files

Agents are defined by Markdown files placed in the `/.agents/` directory inside the Fossil repository (checked into trunk). The orchestrator reads these files at startup.

**Format:**
```
name: planner-claude
role: planner
cli: claude
cmd: claude --dangerously-skip-permissions
capabilities: [planning, fossil-read, architecture]
description: Analyses raw tickets and creates structured action plans in the Wiki.
```

- `name` — unique identifier; used as the Tmux session name and workspace directory name. No spaces.
- `role` — controls which tickets this agent picks up (see Roles below).
- `cli` — the CLI program being run (informational).
- `cmd` — the full command spawned inside Tmux.
- `capabilities` — informational; used in PULSE context to tell the agent what it can do.
- `description` — the agent's identity prompt; included in every PULSE it receives.

---

## 5. Agent Roles and Ticket Routing

The orchestrator routes tickets to agents based on the `role` field. This is the core workflow logic:

| Role       | Picks up                                                                      |
|------------|-------------------------------------------------------------------------------|
| `planner`  | `Open` tickets that are not sub-tasks (no `[parent:]` marker in comment)      |
| `coder`    | `Planned` or `Rework` tickets explicitly delegated to it by a planner         |
| `reviewer` | `Review` tickets delegated to it                                               |
| other      | Unassigned `Open` tickets, or `Planned`/`Rework` tickets delegated to it      |

**Delegation** means the ticket's `private_contact` field matches the agent's name or Fossil hash. A planner signals delegation by assigning the ticket and setting its status.

**Recovery**: if the orchestrator restarts mid-task, any agent that has an `In Progress` ticket assigned to it in Fossil will resume that ticket automatically on the next tick.

---

## 6. Agent States

Each agent has an in-memory state that the UI reflects:

| State              | Meaning                                                            |
|--------------------|--------------------------------------------------------------------|
| `OPEN`             | Idle, waiting for a ticket to be routed                            |
| `IN_PROGRESS`      | Working on a ticket; receiving PULSE messages each tick            |
| `REVIEW`           | Ticket submitted for review (agent is idle)                        |
| `BLOCKED`          | Exceeded `MAX_STEPS_PER_TICKET` or manually blocked; needs human intervention |
| `PAUSED`           | Manually paused via the UI; Tmux session alive but no PULSEs sent  |
| `DONE`             | Ticket closed; agent returns to `OPEN` on next tick                |
| `OFFLINE`          | Tmux session not found                                             |

---

## 7. Workspaces

Each agent gets an isolated filesystem workspace at `/tmp/workspaces/<agent_name>`. At startup the orchestrator:

1. Creates the directory (`mkdir -p`).
2. Runs `fossil open <repo>` inside it if not already opened.

The agent CLI process runs inside this directory, so any `fossil commit` it makes is scoped to that checkout. Agents never share a workspace — this is the isolation guarantee.

---

## 8. Tmux Sessions

The orchestrator uses **Tmux** to run agent CLI processes as background sessions. Each agent maps to one Tmux session named after the agent's `name` field.

Key operations:

- `tmux_spawn_agent` — creates a new session running a wrapper script (`.mei_runner.sh`) that prevents the window from closing on error.
- `tmux_send_pulse` — injects a PULSE payload string into the session's pane followed by Enter, triggering the CLI to process it.
- `tmux_capture_output` — reads the visible pane buffer to detect whether the CLI is ready (warm-up detection).
- `tmux_kill_agent` — kills the session (used on shutdown or manual kill via UI).

The `attach` command in the UI (`a`) connects you directly to an agent's Tmux session for live inspection. Detach with `Ctrl+B, d`.

---

## 9. The PULSE Protocol

PULSE is the message format the orchestrator sends to agents. It is a structured envelope (JSON/XML) injected into the agent's Tmux pane. Its purpose is to give the agent everything it needs to take exactly one meaningful step on its current ticket.

**Fields:**

| Field           | Content                                                    |
|-----------------|------------------------------------------------------------|
| `intent`        | What the agent should do on this step                      |
| `context`       | Full ticket body, wiki log history, and agent capabilities |
| `current_state` | The ticket's current status in Fossil                      |
| `next_action`   | Explicit instruction for what to produce or change next    |

Agents receive one PULSE per tick while in `IN_PROGRESS` state. The orchestrator reads the wiki log to build the `context` field, so the agent always has a record of its own prior steps.

---

## 10. Warm-up Phase

When an agent first picks up a ticket, the orchestrator enters a **warm-up phase** before sending the first PULSE. This is necessary because CLI tools (especially TUI-based ones like `opencode`) take several seconds to fully initialize.

The orchestrator waits until either:
- The Tmux pane output contains a recognizable prompt (e.g. `>`), or
- `CLI_WARMUP_TIMEOUT_TICKS` (15 ticks × 2s = 30s) have elapsed as a safety timeout.

A minimum of `CLI_WARMUP_MIN_TICKS` (5 ticks = 10s) is always enforced before accepting any pane output as "ready", to prevent triggering on boot messages.

The warm-up state is represented by `ticket_steps[i] = -1`.

---

## 11. MAX_STEPS_PER_TICKET

To prevent infinite agent loops, each agent has a step counter that increments on every PULSE sent. When it reaches `MAX_STEPS_PER_TICKET` (1500 steps × 2s ≈ 50 minutes), the orchestrator:

1. Sets the agent's state to `BLOCKED`.
2. Updates the Fossil ticket status to `Blocked`.
3. Appends an explanatory note to the ticket's `changelog` field.
4. Stops sending PULSEs to that agent.

A human (or a reviewer agent) must then inspect the ticket, add context or new instructions, and manually reset the status to re-engage the agent.

---

## 12. Wiki Audit Log

Every significant agent action is recorded as a timestamped entry on a Fossil wiki page named `ticket-<uuid>`. This page is:

- **Written by agents** after each step (they are instructed to log their actions in the PULSE).
- **Read by the orchestrator** to build the `context` field of the next PULSE.
- **Readable by humans** via the Fossil web UI.

This gives every ticket a full, durable history of what was attempted, decided, and produced — independent of the orchestrator process.

---

## 13. The Trust Dialog

Some CLI tools (notably `claude`) display a workspace trust confirmation dialog on first launch in a new directory. The orchestrator detects this by scanning the captured pane output and sends a dismissal keystroke (Enter) automatically. Once dismissed, the state is recorded in `trust_accepted[i]` and never repeated for the lifetime of that process.

---

## Concept Map

```
Fossil Repository (.fossil)
  ├── Trunk: /.agents/*.md  ──────────────────► Agent definitions (name, role, cmd)
  ├── Tickets                                   ├── planner → routes Open tickets
  │     ├── status: Open/Planned/In Progress    ├── coder   → executes Planned tickets
  │     ├── private_contact: <agent hash>       └── reviewer → validates Review tickets
  │     └── changelog: orchestrator notes
  └── Wiki: ticket-<uuid>  ◄── agents write / orchestrator reads for PULSE context

Orchestrator (stateless binary)
  └── tick every 2s
        ├── read tickets from Fossil
        ├── route idle agents to matching tickets
        └── for each IN_PROGRESS agent:
              ├── capture Tmux pane (warm-up / ready check)
              ├── build PULSE from ticket + wiki log
              └── inject PULSE into Tmux session

Tmux Sessions (one per agent)
  └── /tmp/workspaces/<name>/  ← isolated fossil checkout
        └── <agent CLI process>  ← receives PULSE, commits to Fossil
```
