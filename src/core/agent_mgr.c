#include "core/agent_mgr.h"
#include "core/vcs_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Candidate directories scanned in order. First one that contains .md files wins.
// .mei/ is the current standard; .agents/ is the legacy fallback.
static const char *AGENT_DIRS[] = { ".mei", ".agents", NULL };

static void trim_trailing_whitespace(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

// Resolve which agent directory to use. Returns the dir string, or NULL if none found.
// Emits a warning to stderr if the legacy .agents/ fallback is used.
static const char *resolve_agent_dir(void) {
    for (int d = 0; AGENT_DIRS[d]; d++) {
        char files[1][256];
        int n = g_backend->list_agent_files(g_backend, AGENT_DIRS[d], files, 1);
        if (n > 0) {
            if (d > 0)
                fprintf(stderr, "[WARN] Using legacy %s/ — consider migrating to .mei/\n",
                        AGENT_DIRS[d]);
            return AGENT_DIRS[d];
        }
    }
    return NULL;
}

// Parse a single agent definition from a text buffer (one .md file content).
// Returns 1 if a valid agent was extracted, 0 otherwise.
static int parse_agent_from_buffer(const char *buf, Agent *a) {
    memset(a, 0, sizeof(Agent));
    a->state = AGENT_STATE_OPEN;
    strcpy(a->current_ticket, "None");

    int in_description = 0;
    const char *p = buf;

    while (*p) {
        // Find end of current line
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        char line[2048];
        size_t copy_len = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 2;
        strncpy(line, p, copy_len);
        line[copy_len] = '\0';

        if (in_description) {
            // Accumulate every subsequent line into description until EOF
            strncat(a->description, line,
                    sizeof(a->description) - strlen(a->description) - 1);
            strncat(a->description, "\n",
                    sizeof(a->description) - strlen(a->description) - 1);
        } else {
            char key[64], value[1024];
            if (sscanf(line, "%63[^:]: %[^\n]", key, value) == 2) {
                if (strcmp(key, "name") == 0) {
                    strncpy(a->name, value, sizeof(a->name) - 1);
                } else if (strcmp(key, "role") == 0) {
                    strncpy(a->role, value, sizeof(a->role) - 1);
                } else if (strcmp(key, "cli") == 0) {
                    strncpy(a->cli, value, sizeof(a->cli) - 1);
                } else if (strcmp(key, "cmd") == 0) {
                    strncpy(a->cmd, value, sizeof(a->cmd) - 1);
                } else if (strcmp(key, "capabilities") == 0) {
                    strncpy(a->capabilities, value, sizeof(a->capabilities) - 1);
                } else if (strcmp(key, "description") == 0) {
                    strncpy(a->description, value, sizeof(a->description) - 1);
                    strncat(a->description, "\n",
                            sizeof(a->description) - strlen(a->description) - 1);
                    in_description = 1;
                }
            }
        }

        p += line_len;
        if (*p == '\n') p++;
    }

    // Agent identifier used in assignee labels — use the name directly.
    // This makes GitHub labels human-readable ("assignee:coder" instead of
    // "assignee:77df854a..."). Names must be unique within a repo, which is
    // already enforced by having one .mei/<name>.md file per agent.
    if (a->name[0])
        strncpy(a->hash, a->name, sizeof(a->hash) - 1);

    // Strip trailing whitespace from all string fields
    trim_trailing_whitespace(a->name);
    trim_trailing_whitespace(a->role);
    trim_trailing_whitespace(a->cli);
    trim_trailing_whitespace(a->cmd);
    trim_trailing_whitespace(a->capabilities);
    trim_trailing_whitespace(a->description);

    return (strlen(a->name) > 0) ? 1 : 0;
}

int agent_mgr_load_all(Agent *agents) {
    if (!g_backend) return 0;

    const char *agent_dir = resolve_agent_dir();
    if (!agent_dir) return 0;

    char filenames[MAX_AGENTS][256];
    int file_count = g_backend->list_agent_files(g_backend, agent_dir, filenames, MAX_AGENTS);

    int count = 0;
    for (int f = 0; f < file_count && count < MAX_AGENTS; f++) {
        char content[MEI_TEXT_BUFFER_SIZE];
        int len = g_backend->read_agent_file(g_backend, agent_dir, filenames[f],
                                              content, sizeof(content));
        if (len <= 0) continue;

        if (parse_agent_from_buffer(content, &agents[count]))
            count++;
    }

    return count;
}
