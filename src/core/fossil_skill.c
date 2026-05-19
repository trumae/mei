#include "core/fossil_skill.h"
#include "core/vcs_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

// ─── Internal repo path ───────────────────────────────────────────────────────

static char global_repo_path[1024] = {0};

static bool run_cmd(const char *cmd) {
    char full_cmd[1024];
    snprintf(full_cmd, sizeof(full_cmd), "%s >> /tmp/fossil_err.log 2>&1", cmd);
    for (int attempt = 0; attempt <= 30; attempt++) {
        if (attempt > 0) sleep(1);
        if (system(full_cmd) == 0) return true;
    }
    return false;
}

// ─── Public: repo initialisation (used by cmd_new before a backend exists) ───

bool fossil_init(const char *repo_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fossil init %s", repo_path);
    return run_cmd(cmd);
}

// ─── Static ticket helpers ───────────────────────────────────────────────────

static int is_valid_fossil_uuid(const char *s) {
    if (!s) return 0;
    size_t len = strlen(s);
    if (len < 10 || len > 64) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int impl_ticket_list(VCSBackend *b, VCSTicket *tickets, int max_tickets) {
    (void)b;
    if (!global_repo_path[0]) return 0;

    char ensure_cols[512];
    snprintf(ensure_cols, sizeof(ensure_cols),
             "printf 'ALTER TABLE ticket ADD COLUMN changelog TEXT;\\n"
             "ALTER TABLE ticket ADD COLUMN reviewer_notes TEXT;\\n' "
             "| fossil sqlite -R %s >/dev/null 2>&1",
             global_repo_path);
    system(ensure_cols);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "printf \".mode list\\n"
             "PRAGMA busy_timeout = 5000;\\n"
             "SELECT tkt_uuid || '|' || coalesce(title,'') || '|' || "
             "coalesce(status,'') || '|' || coalesce(private_contact,'') || '|' || "
             "replace(replace(coalesce(comment,''),char(10),char(1)),char(13),'') || '|' || "
             "replace(replace(coalesce(reviewer_notes,''),char(10),char(1)),char(13),'') "
             "FROM ticket WHERE status != 'Closed' AND status != 'done';\\n\" "
             "| fossil sqlite -R %s 2>/dev/null",
             global_repo_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    int count = 0;
    char line[MEI_TEXT_BUFFER_SIZE + 2048];
    while (fgets(line, sizeof(line), fp) && count < max_tickets) {
        line[strcspn(line, "\n")] = 0;

        char *fields[6];
        char *ptr = line;
        for (int i = 0; i < 6; i++) {
            fields[i] = ptr;
            char *sep = strchr(ptr, '|');
            if (sep) {
                *sep = '\0';
                ptr = sep + 1;
            } else {
                if (i < 5) ptr = ptr + strlen(ptr);
            }
        }

        char *uuid           = fields[0];
        char *title          = fields[1];
        char *status         = fields[2];
        char *assignee       = fields[3];
        char *comment        = fields[4];
        char *reviewer_notes = fields[5];

        if (is_valid_fossil_uuid(uuid)) {
            strncpy(tickets[count].uuid,           uuid,     sizeof(tickets[count].uuid) - 1);
            strncpy(tickets[count].title,          title   ? title   : "", sizeof(tickets[count].title) - 1);
            strncpy(tickets[count].status,         status  ? status  : "Open", sizeof(tickets[count].status) - 1);
            strncpy(tickets[count].assignee,       assignee? assignee: "", sizeof(tickets[count].assignee) - 1);
            char *f;
            f = tickets[count].description;
            strncpy(f, comment ? comment : "", sizeof(tickets[count].description) - 1);
            for (char *p = f; *p; p++) if ((unsigned char)*p == 1) *p = '\n';
            f = tickets[count].reviewer_notes;
            strncpy(f, reviewer_notes ? reviewer_notes : "", sizeof(tickets[count].reviewer_notes) - 1);
            for (char *p = f; *p; p++) if ((unsigned char)*p == 1) *p = '\n';
            count++;
        } else if (uuid && uuid[0] != '\0') {
            FILE *err = fopen("/tmp/fossil_err.log", "a");
            if (err) {
                fprintf(err, "[fossil ticket_list] rejected malformed uuid: %.80s\n", uuid);
                fclose(err);
            }
        }
    }

    pclose(fp);
    return count;
}

static bool impl_ticket_assign(VCSBackend *b, const char *ticket_id, const char *agent_name) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !agent_name) return false;

    char tmp_sql[64] = "/tmp/mei_sql_XXXXXX";
    int fd = mkstemp(tmp_sql);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_sql); return false; }
    fprintf(f, "PRAGMA busy_timeout = 5000;\n");
    fprintf(f, "UPDATE ticket SET private_contact = '%s' WHERE tkt_uuid LIKE '%s%%';\n",
            agent_name, ticket_id);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fossil sqlite -R %s < %s >> /tmp/fossil_err.log 2>&1",
             global_repo_path, tmp_sql);
    int res = system(cmd);
    unlink(tmp_sql);
    return (res == 0);
}

static bool impl_ticket_set_status(VCSBackend *b, const char *ticket_id, const char *status) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !status) return false;

    char tmp_sql[64] = "/tmp/mei_sql_XXXXXX";
    int fd = mkstemp(tmp_sql);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_sql); return false; }
    fprintf(f, "PRAGMA busy_timeout = 5000;\n");
    fprintf(f, "UPDATE ticket SET status = '%s' WHERE tkt_uuid LIKE '%s%%';\n",
            status, ticket_id);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fossil sqlite -R %s < %s >> /tmp/fossil_err.log 2>&1",
             global_repo_path, tmp_sql);
    int res = system(cmd);
    unlink(tmp_sql);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "fossil ticket_set_status: ticket=%.10s status=%s result=%d",
             ticket_id, status, res);
    log_message(log_msg);
    return (res == 0);
}

static bool impl_ticket_set_reviewer_notes(VCSBackend *b, const char *ticket_id,
                                            const char *notes) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !notes) return false;

    char tmp_sql[64] = "/tmp/mei_sql_XXXXXX";
    int fd = mkstemp(tmp_sql);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_sql); return false; }
    fprintf(f, "PRAGMA busy_timeout = 5000;\n");
    fprintf(f, "UPDATE ticket SET reviewer_notes = '");
    for (const char *p = notes; *p; p++) {
        if (*p == '\'') fputc('\'', f);
        fputc(*p, f);
    }
    fprintf(f, "' WHERE tkt_uuid LIKE '%s%%';\n", ticket_id);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fossil sqlite -R %s < %s >> /tmp/fossil_err.log 2>&1",
             global_repo_path, tmp_sql);
    int res = system(cmd);
    unlink(tmp_sql);
    return (res == 0);
}

static bool impl_ticket_add_note(VCSBackend *b, const char *ticket_id, const char *note) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !note) return false;

    char ensure_col[512];
    snprintf(ensure_col, sizeof(ensure_col),
             "printf 'ALTER TABLE ticket ADD COLUMN changelog TEXT;\\n' "
             "| fossil sqlite -R %s >/dev/null 2>&1",
             global_repo_path);
    system(ensure_col);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char tmp_sql[64] = "/tmp/mei_sql_XXXXXX";
    int fd = mkstemp(tmp_sql);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_sql); return false; }

    fprintf(f, "PRAGMA busy_timeout = 5000;\n");
    fprintf(f, "UPDATE ticket SET changelog = '[MEI %04d-%02d-%02d %02d:%02d] ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min);
    for (const char *p = note; *p; p++) {
        if (*p == '\'') fputc('\'', f);
        fputc(*p, f);
    }
    fprintf(f, "' WHERE tkt_uuid LIKE '%s%%';\n", ticket_id);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fossil sqlite -R %s < %s >> /tmp/fossil_err.log 2>&1",
             global_repo_path, tmp_sql);
    int res = system(cmd);
    unlink(tmp_sql);
    return (res == 0);
}

// ─── Audit log (wiki) ─────────────────────────────────────────────────────────

static bool impl_ticket_append_log(VCSBackend *b, const char *ticket_id,
                                    const char *agent, const char *message) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !agent || !message) return false;

    char page_name[32];
    snprintf(page_name, sizeof(page_name), "ticket-%.10s", ticket_id);

    char tmp_path[64] = "/tmp/mei_wiki_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return false;
    close(fd);

    char export_cmd[1024];
    snprintf(export_cmd, sizeof(export_cmd),
             "fossil wiki export \"%s\" %s -R %s >/dev/null 2>&1",
             page_name, tmp_path, global_repo_path);
    system(export_cmd);

    // Normalise literal \n sequences written by agents
    {
        char norm_path[64] = "/tmp/mei_wiki_norm_XXXXXX";
        int nfd = mkstemp(norm_path);
        if (nfd >= 0) {
            FILE *rf = fopen(tmp_path, "r");
            FILE *wf = fdopen(nfd, "w");
            if (rf && wf) {
                int c;
                while ((c = fgetc(rf)) != EOF) {
                    if (c == '\\') {
                        int nx = fgetc(rf);
                        if (nx == 'n')      fputc('\n', wf);
                        else if (nx == 't') fputc('\t', wf);
                        else { fputc(c, wf); if (nx != EOF) fputc(nx, wf); }
                    } else {
                        fputc(c, wf);
                    }
                }
                fclose(rf); fclose(wf);
                rename(norm_path, tmp_path);
            } else {
                if (rf) fclose(rf);
                if (wf) fclose(wf); else if (nfd >= 0) close(nfd);
                unlink(norm_path);
            }
        }
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    FILE *f = fopen(tmp_path, "a");
    if (!f) { unlink(tmp_path); return false; }
    fprintf(f, "\n**[%04d-%02d-%02d %02d:%02d] %s:** %s\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min,
            agent, message);
    fclose(f);

    char commit_cmd[1024];
    snprintf(commit_cmd, sizeof(commit_cmd),
             "fossil wiki commit \"%s\" %s --mimetype text/x-markdown -R %s >/dev/null 2>&1 "
             "|| fossil wiki create \"%s\" %s --mimetype text/x-markdown -R %s >/dev/null 2>&1",
             page_name, tmp_path, global_repo_path,
             page_name, tmp_path, global_repo_path);
    system(commit_cmd);

    unlink(tmp_path);
    return true;
}

static int impl_ticket_read_log(VCSBackend *b, const char *ticket_id,
                                 char *buffer, size_t max_size) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !buffer || max_size == 0) return 0;

    buffer[0] = '\0';
    char page_name[32];
    snprintf(page_name, sizeof(page_name), "ticket-%.10s", ticket_id);

    char tmp_path[64] = "/tmp/mei_wiki_read_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return 0;
    close(fd);

    char export_cmd[1024];
    snprintf(export_cmd, sizeof(export_cmd),
             "fossil wiki export \"%s\" %s -R %s >/dev/null 2>&1",
             page_name, tmp_path, global_repo_path);
    system(export_cmd);

    FILE *f = fopen(tmp_path, "r");
    if (!f) { unlink(tmp_path); return 0; }

    size_t total = fread(buffer, 1, max_size - 1, f);
    buffer[total] = '\0';
    fclose(f);
    unlink(tmp_path);
    return (int)total;
}

static int impl_ticket_read_human_remarks(VCSBackend *b, const char *ticket_id,
                                           char *buffer, size_t max_size) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !buffer || max_size == 0) return 0;
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "fossil ticket history %.40s -R %s 2>/dev/null"
             " | grep -B2 -A3 'icomment:'",
             ticket_id, global_repo_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    size_t total = 0;
    int c;
    while (total < max_size - 1 && (c = fgetc(fp)) != EOF)
        buffer[total++] = (char)c;
    buffer[total] = '\0';
    pclose(fp);
    return (int)total;
}

static int impl_ticket_read_initial_description(VCSBackend *b, const char *ticket_id,
                                                 char *buffer, size_t max_size) {
    (void)b;
    if (!global_repo_path[0] || !ticket_id || !buffer || max_size == 0) return 0;

    char uuid10[11] = {0};
    strncpy(uuid10, ticket_id, 10);

    char get_hash_cmd[512];
    snprintf(get_hash_cmd, sizeof(get_hash_cmd),
             "fossil sqlite -R %s "
             "\"SELECT b.uuid FROM event e JOIN blob b ON b.rid=e.objid "
             "WHERE e.type='t' AND e.comment LIKE ('%%%s%%') "
             "ORDER BY e.mtime ASC LIMIT 1;\" 2>/dev/null",
             global_repo_path, uuid10);

    FILE *fp = popen(get_hash_cmd, "r");
    if (!fp) return 0;

    char artifact_hash[128] = {0};
    if (fgets(artifact_hash, sizeof(artifact_hash), fp))
        artifact_hash[strcspn(artifact_hash, "\n\r")] = 0;
    pclose(fp);

    if (!artifact_hash[0]) return 0;

    char art_cmd[256];
    snprintf(art_cmd, sizeof(art_cmd),
             "fossil artifact %s -R %s 2>/dev/null", artifact_hash, global_repo_path);

    FILE *art_fp = popen(art_cmd, "r");
    if (!art_fp) return 0;

    int found = 0;
    char line[MEI_TEXT_BUFFER_SIZE];
    while (fgets(line, sizeof(line), art_fp)) {
        if (strncmp(line, "J icomment ", 11) != 0) continue;
        const char *src = line + 11;
        size_t out = 0;
        while (*src && *src != '\n' && out < max_size - 1) {
            if (src[0] == '\\' && src[1] == 's') {
                buffer[out++] = ' ';
                src += 2;
            } else {
                buffer[out++] = *src++;
            }
        }
        buffer[out] = '\0';
        found = (int)out;
        break;
    }
    pclose(art_fp);
    return found;
}

// ─── Workspace / VCS ─────────────────────────────────────────────────────────

static bool impl_workspace_init(VCSBackend *b, const char *workspace_path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s && cd %s && "
             "if [ ! -f .fslckout ]; then fossil open %s > /dev/null 2>&1; fi",
             workspace_path, workspace_path, b->repo_id);
    return (system(cmd) == 0);
}

static bool impl_branch_create(VCSBackend *b, const char *workspace_path,
                                const char *branch_name) {
    (void)b;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "cd %s && fossil branch new %s trunk >/dev/null 2>&1 || true",
             workspace_path, branch_name);
    system(cmd);
    return true;
}

static bool impl_branch_switch(VCSBackend *b, const char *workspace_path,
                                const char *branch_name) {
    (void)b;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "cd %s && fossil update %s >/dev/null 2>&1",
             workspace_path, branch_name);
    return (system(cmd) == 0);
}

static bool impl_commit(VCSBackend *b, const char *workspace_path, const char *message) {
    (void)b;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "cd %s && fossil commit -m \"%s\"", workspace_path, message);
    return run_cmd(cmd);
}

// ─── Agent definition files ──────────────────────────────────────────────────

static int impl_list_agent_files(VCSBackend *b, const char *dir,
                                  char out[][256], int max) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "fossil ls -r trunk -R %s 2>/dev/null", b->repo_id);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    int count = 0;
    size_t dlen = strlen(dir);
    char line[512];
    while (fgets(line, sizeof(line), fp) && count < max) {
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, dir, dlen) == 0 && line[dlen] == '/') {
            const char *ext = strrchr(line, '.');
            if (ext && strcmp(ext, ".md") == 0) {
                strncpy(out[count], line + dlen + 1, 255);
                count++;
            }
        }
    }
    pclose(fp);
    return count;
}

static int impl_read_agent_file(VCSBackend *b, const char *dir, const char *filename,
                                 char *buf, size_t max) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "fossil cat \"%s\" -r trunk -R %s 2>/dev/null", path, b->repo_id);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    size_t total = fread(buf, 1, max - 1, fp);
    buf[total] = '\0';
    pclose(fp);
    return (int)total;
}

// ─── Factory ─────────────────────────────────────────────────────────────────

VCSBackend *fossil_backend_create(const char *repo_path) {
    VCSBackend *b = calloc(1, sizeof(VCSBackend));
    if (!b) return NULL;

    strncpy(global_repo_path, repo_path, sizeof(global_repo_path) - 1);
    strncpy(b->repo_id,       repo_path, sizeof(b->repo_id) - 1);
    b->type = VCS_FOSSIL;

    b->ticket_list                   = impl_ticket_list;
    b->ticket_assign                 = impl_ticket_assign;
    b->ticket_set_status             = impl_ticket_set_status;
    b->ticket_set_reviewer_notes     = impl_ticket_set_reviewer_notes;
    b->ticket_add_note               = impl_ticket_add_note;
    b->ticket_append_log             = impl_ticket_append_log;
    b->ticket_read_log               = impl_ticket_read_log;
    b->ticket_read_human_remarks     = impl_ticket_read_human_remarks;
    b->ticket_read_initial_description = impl_ticket_read_initial_description;
    b->workspace_init                = impl_workspace_init;
    b->branch_create                 = impl_branch_create;
    b->branch_switch                 = impl_branch_switch;
    b->commit                        = impl_commit;
    b->list_agent_files              = impl_list_agent_files;
    b->read_agent_file               = impl_read_agent_file;

    return b;
}
