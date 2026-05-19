#include "core/github_skill.h"
#include "core/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// All GitHub status labels recognized by MEI.
static const char *STATUS_LABELS[] = {
    "status:Open", "status:Planned", "status:In Progress",
    "status:Review", "status:Rework", "status:Blocked",
    "status:Done", "status:QA Ready", "status:Pending Approval",
    "status:Delegated", "status:Verified", "status:closed", NULL
};

// Per-session state cache to limit GitHub API calls.
typedef struct {
    VCSTicket tickets[200];
    int       ticket_count;
    int       ticks_since_poll;
    int       poll_interval_ticks;
    int       dirty;
} GithubStateCache;

static GithubStateCache g_cache;

// ──────────────────────────────────────────────
// Shell helpers
// ──────────────────────────────────────────────

// Run cmd, capture stdout into buf (null-terminated). Returns bytes captured.
static int run_capture(const char *cmd, char *buf, size_t max) {
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = '\0'; return 0; }
    size_t n = fread(buf, 1, max - 1, fp);
    buf[n] = '\0';
    pclose(fp);
    return (int)n;
}

// ──────────────────────────────────────────────
// TSV parsing
// ──────────────────────────────────────────────

// Unescape jq @tsv encoding (modifies in-place).
// \n → newline, \t → tab, \\ → backslash
static void tsv_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && *(r + 1)) {
            r++;
            switch (*r) {
                case 'n':  *w++ = '\n'; break;
                case 't':  *w++ = '\t'; break;
                case 'r':  *w++ = '\r'; break;
                case '\\': *w++ = '\\'; break;
                default:   *w++ = '\\'; *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Extract next tab-delimited field from *p. Advances *p past the delimiter.
// Returns 1 if a field was consumed, 0 if end of string.
static int tsv_field(const char **p, char *buf, size_t max) {
    if (!**p) return 0;
    size_t i = 0;
    while (**p && **p != '\t' && **p != '\n') {
        if (i < max - 1) buf[i++] = **p;
        (*p)++;
    }
    buf[i] = '\0';
    tsv_unescape(buf);
    if (**p == '\t' || **p == '\n') (*p)++;
    return 1;
}

// ──────────────────────────────────────────────
// Label helpers
// ──────────────────────────────────────────────

// Extract the value of the first label with given prefix from a comma-sep list.
static void label_extract(const char *labels_csv, const char *prefix,
                           char *out, size_t max) {
    size_t plen = strlen(prefix);
    const char *p = labels_csv;
    out[0] = '\0';
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > plen && strncmp(p, prefix, plen) == 0) {
            size_t vlen = len - plen;
            if (vlen > max - 1) vlen = max - 1;
            strncpy(out, p + plen, vlen);
            out[vlen] = '\0';
            return;
        }
        p += len;
        if (*p == ',') p++;
    }
}

// ──────────────────────────────────────────────
// Issue number extraction
// ──────────────────────────────────────────────

static int issue_number(const char *uuid) {
    return (uuid[0] == '#') ? atoi(uuid + 1) : atoi(uuid);
}

// ──────────────────────────────────────────────
// Cache
// ──────────────────────────────────────────────

// Fetch all issues from GitHub and populate the cache.
// poll_interval recalculated from distinct assignee count (proxy for agent count).
static void cache_poll(VCSBackend *b) {
    char cmd[1024];
    // Static: 256 KB avoids stack overflow for large issue lists
    static char buf[262144];

    // Fetch all issues (open + closed) as TSV: number, state, title, labels, body (truncated at 2000 chars)
    snprintf(cmd, sizeof(cmd),
             "gh issue list -R '%s' --state all --limit 200 "
             "--json number,title,body,labels,state "
             "--jq '[.[] | [(.number|tostring), .state, .title, "
             "(.labels|map(.name)|join(\",\")), (.body // \"\" | .[0:2000])]] | .[] | @tsv'",
             b->repo_id);

    int n = run_capture(cmd, buf, sizeof(buf));
    if (n <= 0) {
        // API call failed — keep existing cache, force retry on next tick
        g_cache.dirty = 1;
        return;
    }

    int count = 0;
    const char *p = buf;
    while (*p && count < 200) {
        VCSTicket *t = &g_cache.tickets[count];
        memset(t, 0, sizeof(*t));

        char num[16], state[16], title[128], labels[512], body[2048];

        if (!tsv_field(&p, num,    sizeof(num)))    break;
        if (!tsv_field(&p, state,  sizeof(state)))  break;
        if (!tsv_field(&p, title,  sizeof(title)))  break;
        if (!tsv_field(&p, labels, sizeof(labels))) break;
        if (!tsv_field(&p, body,   sizeof(body)))   break;

        snprintf(t->uuid, sizeof(t->uuid), "#%s", num);
        strncpy(t->title, title, sizeof(t->title) - 1);
        strncpy(t->description, body, sizeof(t->description) - 1);

        // Status: from status:* label, or infer from GitHub state
        char status_val[64];
        label_extract(labels, "status:", status_val, sizeof(status_val));
        if (status_val[0]) {
            strncpy(t->status, status_val, sizeof(t->status) - 1);
        } else if (strcasecmp(state, "CLOSED") == 0) {
            strncpy(t->status, "Done", sizeof(t->status) - 1);
        } else {
            strncpy(t->status, "Open", sizeof(t->status) - 1);
        }

        // Assignee: from assignee:* label
        label_extract(labels, "assignee:", t->assignee, sizeof(t->assignee));

        count++;

        // Skip to next line (tsv_field already advanced past the last field's newline)
    }
    g_cache.ticket_count = count;

    // Adaptive poll interval: count distinct non-empty assignees as agent proxy
    {
        int distinct = 0;
        char seen[32][64] = {0};
        for (int i = 0; i < count; i++) {
            if (!g_cache.tickets[i].assignee[0]) continue;
            int found = 0;
            for (int j = 0; j < distinct; j++) {
                if (strcmp(seen[j], g_cache.tickets[i].assignee) == 0) { found = 1; break; }
            }
            if (!found && distinct < 32)
                strncpy(seen[distinct++], g_cache.tickets[i].assignee, 63);
        }
        g_cache.poll_interval_ticks = distinct * 2;
        if (g_cache.poll_interval_ticks < 5) g_cache.poll_interval_ticks = 5;
    }

    g_cache.ticks_since_poll = 0;
    g_cache.dirty = 0;
}

// ──────────────────────────────────────────────
// VCSBackend implementations
// ──────────────────────────────────────────────

static int impl_ticket_list(VCSBackend *b, VCSTicket *out, int max) {
    if (g_cache.dirty || g_cache.ticks_since_poll >= g_cache.poll_interval_ticks)
        cache_poll(b);
    else
        g_cache.ticks_since_poll++;

    int n = g_cache.ticket_count < max ? g_cache.ticket_count : max;
    memcpy(out, g_cache.tickets, (size_t)n * sizeof(VCSTicket));
    return n;
}

static bool impl_ticket_assign(VCSBackend *b, const char *uuid, const char *agent_hash) {
    int num = issue_number(uuid);

    // Find old assignee label from cache so we can remove it
    char old_label[128] = "";
    for (int i = 0; i < g_cache.ticket_count; i++) {
        if (strcmp(g_cache.tickets[i].uuid, uuid) == 0 && g_cache.tickets[i].assignee[0]) {
            snprintf(old_label, sizeof(old_label), "assignee:%s", g_cache.tickets[i].assignee);
            break;
        }
    }

    char cmd[1024];
    int rc;

    if (agent_hash && agent_hash[0]) {
        // Labels must exist before gh can add them to an issue.
        // Create the assignee label on-demand (--force = idempotent upsert).
        snprintf(cmd, sizeof(cmd),
                 "gh label create 'assignee:%s' -R '%s' --color '0075CA' --force > /dev/null 2>&1",
                 agent_hash, b->repo_id);
        system(cmd);

        if (old_label[0])
            snprintf(cmd, sizeof(cmd),
                     "gh issue edit %d -R '%s' --remove-label '%s' --add-label 'assignee:%s' > /dev/null 2>&1",
                     num, b->repo_id, old_label, agent_hash);
        else
            snprintf(cmd, sizeof(cmd),
                     "gh issue edit %d -R '%s' --add-label 'assignee:%s' > /dev/null 2>&1",
                     num, b->repo_id, agent_hash);
    } else {
        // Empty hash = unassign: just remove the old assignee label if present
        if (!old_label[0]) { g_cache.dirty = 1; return true; }
        snprintf(cmd, sizeof(cmd),
                 "gh issue edit %d -R '%s' --remove-label '%s' > /dev/null 2>&1",
                 num, b->repo_id, old_label);
    }

    rc = system(cmd);
    g_cache.dirty = 1;
    return rc == 0;
}

static bool impl_ticket_set_status(VCSBackend *b, const char *uuid, const char *status) {
    int num = issue_number(uuid);

    // Build --remove-label flags for all known status labels
    char removes[2048] = "";
    for (int i = 0; STATUS_LABELS[i]; i++) {
        char part[128];
        snprintf(part, sizeof(part), " --remove-label '%s'", STATUS_LABELS[i]);
        strncat(removes, part, sizeof(removes) - strlen(removes) - 1);
    }

    // Single atomic gh call: remove all status labels AND add the new one in one
    // API request. gh silently ignores --remove-label for labels not present on the
    // issue (exit 0), so no two-step split is needed. A single call means there is
    // never a window where the issue has no status:* label.
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "gh issue edit %d -R '%s'%s --add-label 'status:%s' > /dev/null 2>&1",
             num, b->repo_id, removes, status);
    system(cmd);

    // Close or reopen based on status
    if (strcasecmp(status, "Done") == 0 || strcasecmp(status, "closed") == 0) {
        snprintf(cmd, sizeof(cmd),
                 "gh issue close %d -R '%s' > /dev/null 2>&1", num, b->repo_id);
        system(cmd);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "gh issue reopen %d -R '%s' > /dev/null 2>&1", num, b->repo_id);
        system(cmd);
    }

    g_cache.dirty = 1;
    return true;
}

static bool impl_ticket_set_reviewer_notes(VCSBackend *b, const char *uuid, const char *notes) {
    int num = issue_number(uuid);
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "gh issue comment %d -R '%s' --body '[review] %s' > /dev/null 2>&1",
             num, b->repo_id, notes);
    int rc = system(cmd);
    g_cache.dirty = 1;
    return rc == 0;
}

static bool impl_ticket_add_note(VCSBackend *b, const char *uuid, const char *note) {
    int num = issue_number(uuid);
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "gh issue comment %d -R '%s' --body '[orch] %s' > /dev/null 2>&1",
             num, b->repo_id, note);
    int rc = system(cmd);
    g_cache.dirty = 1;
    return rc == 0;
}

static bool impl_ticket_append_log(VCSBackend *b, const char *uuid,
                                    const char *agent, const char *msg) {
    int num = issue_number(uuid);
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "gh issue comment %d -R '%s' --body '[log] %s | %s | %s' > /dev/null 2>&1",
             num, b->repo_id, ts, agent, msg);
    int rc = system(cmd);
    g_cache.dirty = 1;
    return rc == 0;
}

static int impl_ticket_read_log(VCSBackend *b, const char *uuid, char *buf, size_t max) {
    int num = issue_number(uuid);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "gh issue view %d -R '%s' --json comments "
             "--jq '[.comments[] | select(.body | ltrimstr(\" \") | "
             "(startswith(\"[log]\") or startswith(\"[orch]\")))] | map(.body) | join(\"\\n\")'",
             num, b->repo_id);
    return run_capture(cmd, buf, max);
}

static int impl_ticket_read_human_remarks(VCSBackend *b, const char *uuid, char *buf, size_t max) {
    int num = issue_number(uuid);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "gh issue view %d -R '%s' --json comments "
             "--jq '[.comments[] | select(.body | ltrimstr(\" \") | "
             "(startswith(\"[log]\") or startswith(\"[orch]\") or startswith(\"[review]\")) | not)] "
             "| map(.body) | join(\"\\n---\\n\")'",
             num, b->repo_id);
    return run_capture(cmd, buf, max);
}

static int impl_ticket_read_initial_description(VCSBackend *b, const char *uuid,
                                                 char *buf, size_t max) {
    // Return body from cache if present
    for (int i = 0; i < g_cache.ticket_count; i++) {
        if (strcmp(g_cache.tickets[i].uuid, uuid) == 0) {
            size_t n = strlen(g_cache.tickets[i].description);
            if (n > max - 1) n = max - 1;
            memcpy(buf, g_cache.tickets[i].description, n);
            buf[n] = '\0';
            return (int)n;
        }
    }
    // Not in cache — fetch directly
    int num = issue_number(uuid);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "gh issue view %d -R '%s' --json body --jq '.body'",
             num, b->repo_id);
    return run_capture(cmd, buf, max);
}

static bool impl_workspace_init(VCSBackend *b, const char *workspace_path) {
    char cmd[1024];
    // Use gh repo clone so the user's configured protocol (SSH or HTTPS) is respected.
    snprintf(cmd, sizeof(cmd),
             "gh repo clone '%s' '%s' > /dev/null 2>&1",
             b->repo_id, workspace_path);
    return system(cmd) == 0;
}

static bool impl_branch_create(VCSBackend *b, const char *workspace_path,
                                const char *branch_name) {
    char cmd[1024];
    // First fetch to get the latest remote state, then try to create the branch.
    // If the branch already exists on the remote (e.g. from a prior session), check it
    // out directly rather than trying to create a divergent local branch.
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git fetch origin > /dev/null 2>&1 && "
             "("
             "  (git checkout -b '%s' > /dev/null 2>&1 && git push -u origin '%s' > /dev/null 2>&1)"
             "  || git checkout -B '%s' origin/'%s' > /dev/null 2>&1"
             ")",
             workspace_path, branch_name, branch_name, branch_name, branch_name);
    (void)b;
    return system(cmd) == 0;
}

static bool impl_branch_switch(VCSBackend *b, const char *workspace_path,
                                const char *branch_name) {
    (void)b;
    char cmd[1024];
    // Try local checkout first; fall back to fetching from origin if branch is remote-only.
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && (git checkout '%s' > /dev/null 2>&1"
             " || (git fetch origin > /dev/null 2>&1"
             "     && git checkout -b '%s' origin/'%s' > /dev/null 2>&1))",
             workspace_path, branch_name, branch_name, branch_name);
    return system(cmd) == 0;
}

static bool impl_commit(VCSBackend *b, const char *workspace_path, const char *message) {
    (void)b;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git add -A && git commit -m '%s' && git push -u origin HEAD > /dev/null 2>&1",
             workspace_path, message);
    return system(cmd) == 0;
}

static int impl_list_agent_files(VCSBackend *b, const char *dir,
                                  char out[][256], int max) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "gh api repos/%s/contents/%s --jq '[.[] | select(.name | endswith(\".md\")) | .name] | .[]'",
             b->repo_id, dir);
    char buf[8192];
    int n = run_capture(cmd, buf, sizeof(buf));
    if (n <= 0) return 0;

    int count = 0;
    const char *p = buf;
    while (*p && count < max) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0 && len < 255) {
            strncpy(out[count], p, len);
            out[count][len] = '\0';
            count++;
        }
        p += len;
        if (*p == '\n') p++;
    }
    return count;
}

static int impl_read_agent_file(VCSBackend *b, const char *dir,
                                 const char *filename, char *buf, size_t max) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "gh api repos/%s/contents/%s/%s --jq '.content' | tr -d '\\n' | base64 --decode",
             b->repo_id, dir, filename);
    return run_capture(cmd, buf, max);
}

// ──────────────────────────────────────────────
// Factory
// ──────────────────────────────────────────────

VCSBackend *github_backend_create(const char *owner_repo) {
    VCSBackend *b = calloc(1, sizeof(VCSBackend));
    if (!b) return NULL;

    b->type = VCS_GITHUB;
    strncpy(b->repo_id, owner_repo, sizeof(b->repo_id) - 1);

    b->ticket_list                  = impl_ticket_list;
    b->ticket_assign                = impl_ticket_assign;
    b->ticket_set_status            = impl_ticket_set_status;
    b->ticket_set_reviewer_notes    = impl_ticket_set_reviewer_notes;
    b->ticket_add_note              = impl_ticket_add_note;
    b->ticket_append_log            = impl_ticket_append_log;
    b->ticket_read_log              = impl_ticket_read_log;
    b->ticket_read_human_remarks    = impl_ticket_read_human_remarks;
    b->ticket_read_initial_description = impl_ticket_read_initial_description;
    b->workspace_init               = impl_workspace_init;
    b->branch_create                = impl_branch_create;
    b->branch_switch                = impl_branch_switch;
    b->commit                       = impl_commit;
    b->list_agent_files             = impl_list_agent_files;
    b->read_agent_file              = impl_read_agent_file;

    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.poll_interval_ticks = 10;

    return b;
}
