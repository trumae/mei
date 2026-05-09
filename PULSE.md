# PULSE Protocol

PULSE is the communication protocol MEI uses to assign work to agent CLIs running inside Tmux panes. It defines what is sent, when it is sent, how it is delivered, and what the orchestrator does between sends.

---

## How it works (plain English)

When a ticket is assigned to an agent, the orchestrator does **not** immediately send anything. It first waits for the agent's CLI to finish loading — detected by watching the Tmux pane for a recognisable prompt, or by waiting a minimum number of ticks. Only once the CLI is ready does it send the PULSE: a single Markdown message that contains the full task context.

After that, the orchestrator goes silent. It polls Fossil every 2 seconds to see if the agent changed the ticket status. As long as the agent is working, nothing is sent. The agent finishes, updates the ticket in Fossil, and the orchestrator picks it up on the next tick — no further messages needed.

The only exception is the **nudge**: if the Tmux pane has been visually unchanged for 60 consecutive seconds while the ticket is still open, a short follow-up is sent to prompt the agent to continue. Nudges repeat every 120 seconds. Planners are exempt — they do long-running LLM generation that can appear idle for several minutes before producing output.

A typical ticket lifecycle looks like this:

```
t=0s     Ticket assigned to agent       → ticket_steps = -1 (warm-up)
t=2s     Tick: CLI not ready yet        → log "[wait]", skip
t=4s     Tick: CLI not ready yet        → log "[wait]", skip
t=6s     Tick: CLI ready                → PULSE sent (one-shot), ticket_steps = 0
         ── agent works autonomously ──
t=8s     Tick: steps=1, Fossil poll     → still "In Progress", pane changed → no action
t=10s    Tick: steps=2, Fossil poll     → still "In Progress", pane changed → no action
         ...
t=306s   Tick: pane frozen for 60s      → nudge sent (non-planner only)
         ...
t=Xs     Tick: Fossil poll              → ticket status changed to "Review" / "Done"
         → agent released, state = OPEN → next ticket
```

The key invariant: **one PULSE per ticket assignment**. Every subsequent tick is silent monitoring.

---

## Formal Specification

### 1. Definitions

| Term | Definition |
|------|------------|
| **Tick** | One execution of `orchestrator_tick()`, fired every `TICK_INTERVAL_MS` ms |
| **PULSE** | A formatted Markdown message carrying a complete task assignment |
| **Nudge** | A short follow-up message sent when a non-planner agent appears stalled |
| **Warm-up** | The phase between ticket assignment and first PULSE send (`ticket_steps == -1`) |
| **Active** | The phase between PULSE send and ticket completion (`ticket_steps >= 1`) |
| **Pane hash** | djb2 hash of the captured Tmux pane content, used for inactivity detection |

---

### 2. Timing parameters

| Constant | Value | Meaning |
|----------|-------|---------|
| `TICK_INTERVAL_MS` | 2000 ms | Orchestrator heartbeat period |
| `CLI_WARMUP_MIN_TICKS` | 5 ticks (10 s) | Minimum ticks before CLI is considered ready from any output |
| `CLI_WARMUP_TIMEOUT_TICKS` | 15 ticks (30 s) | Force CLI-ready after this many warm-up ticks regardless |
| `NUDGE_MIN_TICKS` | 15 ticks (30 s) | Minimum active ticks before any nudge is eligible |
| `NUDGE_IDLE_TICKS` | 30 ticks (60 s) | Pane must be unchanged for this many ticks to trigger a nudge |
| `NUDGE_REPEAT_TICKS` | 60 ticks (120 s) | Interval between repeated nudges while agent remains idle |

---

### 3. Agent state machine

```
              ticket assigned
OPEN ─────────────────────────────► WARM-UP  (ticket_steps = -1)
  ▲                                     │
  │                                     │ CLI ready
  │                                     ▼
  │                              PULSE sent once
  │                              (ticket_steps = 0)
  │                                     │
  │                                     │ next tick
  │                                     ▼
  │                               ACTIVE MONITORING
  │                               (ticket_steps 1..N)
  │                                     │
  │              ┌──────────────────────┤
  │              │ every tick           │
  │              │ Fossil poll          │ ticket status
  │              │ pane hash update     │ no longer "In Progress"
  │              │ nudge check          │
  │              └──────────────────────┤
  │                                     │
  └─────────────────────────────────────┘
           agent released, state = OPEN
```

State transitions use `ticket_steps[i]` as the discriminant:

| `ticket_steps` | Phase | Action taken each tick |
|---------------|-------|------------------------|
| `-1` | Warm-up | Capture pane, detect CLI ready. If ready: sync workspace, send PULSE, set `ticket_steps = 0`. |
| `0` | Post-PULSE | No completion check (too early). Increment to 1 on next tick. |
| `>= 1` | Active | Check Fossil for ticket completion. Update pane hash. Increment counter. Fire nudge if conditions met. |

---

### 4. CLI readiness detection

During warm-up, the orchestrator captures the agent's Tmux pane on every tick and applies the following logic in order:

1. **Trust dialog** — if the pane contains `"Quick safety check"` or `"I trust this folder"`, send `Enter` once, set `trust_accepted = 1`, and continue to the next tick. This dismisses the Claude Code workspace trust prompt before any PULSE is sent.

2. **Prompt detection** — the CLI is considered ready if any of the following appear in the pane output:
   - `"Ask anything"` / `"ask anything"` (OpenCode prompt)
   - `"❯"` (U+276F, common in Zsh/prompt frameworks)
   - `"> "` (generic REPL prompt)
   - `"$ "` (shell prompt)

3. **Minimum-ticks fallback** — if none of the above strings are found but `warm_up_ticks >= CLI_WARMUP_MIN_TICKS` and the pane has any output, the CLI is declared ready. This handles agents with non-standard prompts (bash scripts, custom CLIs).

4. **Hard timeout** — if `warm_up_ticks >= CLI_WARMUP_TIMEOUT_TICKS`, the CLI is declared ready unconditionally. This prevents an agent from being permanently stuck in warm-up.

Before the PULSE is sent, the workspace is synced with trunk:
```sh
cd /tmp/workspaces/<agent> && fossil update trunk &
```

---

### 5. Wire format

The PULSE is serialised as plain Markdown by `pulse_format()`:

```markdown
# <intent>

<context>

**Current State:** <current_state>
**Your next action:** <next_action>
```

No envelope tags or structured headers are used. An earlier design used `[PULSE]...[/PULSE]` XML-style tags, but LLM backends (including GPT-4.1 and Claude) treated the envelope as metadata and responded with "message is empty". Plain Markdown headings are unambiguous to any model.

#### Field limits

| Field | C type | Max bytes | Purpose |
|-------|--------|-----------|---------|
| `intent` | `char[128]` | 127 | One-line task title (becomes the Markdown `#` heading) |
| `context` | `char[65536]` | 65535 | Full task body: persona, ticket, history, instructions |
| `current_state` | `char[256]` | 255 | Agent's current state label (e.g. `"Planning"`) |
| `next_action` | `char[256]` | 255 | Imperative one-liner for the agent's immediate next step |

Total maximum serialised payload: `MEI_TEXT_BUFFER_SIZE + 512` bytes (~65 KB).

---

### 6. Delivery mechanism

PULSE delivery uses Tmux bracketed paste:

```c
// 1. Write payload to a temp file
FILE *f = fopen("/tmp/pulse_<agent>.txt", "w");
fprintf(f, "%s", payload);
fclose(f);

// 2. Load file into Tmux paste buffer
tmux load-buffer "/tmp/pulse_<agent>.txt"

// 3. Paste into agent pane using bracketed-paste mode (-p flag)
tmux paste-buffer -p -t mei:"<agent>"

// 4. Brief settling delay, then submit
sleep 0.5
tmux send-keys -t mei:"<agent>" Enter
```

The `-p` flag enables bracketed paste (`ESC[200~...ESC[201~`), which prevents TUI input widgets from auto-submitting on embedded newlines within the multi-line payload. Without it, a PULSE containing `\n` would be interpreted as multiple separate submissions.

The temp file is removed after delivery.

---

### 7. Nudge conditions

A nudge fires when **all** of the following are true on the same tick:

```
a->role != "planner"
AND ticket_steps[i] >= NUDGE_MIN_TICKS        (agent has been active long enough)
AND pane_idle_ticks[i] >= NUDGE_IDLE_TICKS    (pane has been visually static)
AND pane_idle_ticks[i] % NUDGE_REPEAT_TICKS == 0  (rate-limit repeat nudges)
```

**Planners are permanently exempt.** Planners perform deep reasoning that can hold the LLM in a silent generation loop for several minutes before producing terminal output. Nudging a planner mid-generation causes it to interrupt its reasoning, abandon sub-ticket creation, and prematurely close the parent ticket.

The nudge message is a short imperative reminder, not a full PULSE. It does not include ticket context or persona — only a directive to execute the pending Fossil commands and update the ticket status.

---

### 8. Pane inactivity tracking

The orchestrator captures the last `4096` bytes of each agent's Tmux pane every tick using `tmux capture-pane -p`. It applies a djb2 hash to the captured content:

```c
static unsigned long hash_pane(const char *s, int len) {
    unsigned long h = 5381;
    for (int i = 0; i < len; i++)
        h = ((h << 5) + h) ^ (unsigned char)s[i];
    return h;
}
```

If the hash equals the hash from the previous tick, `pane_idle_ticks[i]` is incremented. Any change resets it to zero. This approach is entirely CLI-agnostic — it detects inactivity based on visible output, not on CLI-specific strings.

---

### 9. Context sections included in a PULSE

The `context` field is composed of sections that vary by agent role. All roles receive:

- **Persona** — the agent's own description from its `.agents/<name>.md` file, up to 4096 chars.
- **Decision Protocol** (non-planners only) — standing instructions to decide autonomously, document in wiki, and never stall for input.
- **Ticket** — UUID, title, status, assignee, description, reviewer notes.
- **Discussion history** — accumulated wiki log for this ticket, up to 4096 bytes.
- **Parent ticket context** — if this is a sub-ticket, the parent's description and planning notes.

Additional sections by role:

| Role | Extra sections |
|------|---------------|
| `planner` | Available agent roster (names, roles, hashes, capabilities, truncated descriptions); existing sub-tickets |
| `coder` | Reviewer feedback; related sub-tickets; dependency context; branch name; wiki update shell snippet |
| `reviewer` | Related sub-tickets; branch name; merge/rework commands; wiki update shell snippet |
| `researcher` | Dependency context; branch name; wiki update shell snippet |

---

### 10. Relationship to Fossil state

The PULSE is a one-way push. The orchestrator never reads a response from the agent's terminal to determine completion. Completion is detected exclusively by polling Fossil:

```
ticket.status != "In Progress"   →  agent finished, release to OPEN
OR
ticket.assignee != agent.hash    →  ticket was reassigned externally
```

This makes the protocol robust against any CLI behaviour — crashes, hangs, or unexpected output do not affect orchestrator correctness, because Fossil is the single source of truth.
