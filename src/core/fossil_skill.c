#include "core/fossil_skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

static char global_repo_path[1024] = {0};

void fossil_set_repo_path(const char *path) {
    if (path) {
        strncpy(global_repo_path, path, sizeof(global_repo_path) - 1);
    }
}

const char *fossil_get_repo_path() {
    return global_repo_path;
}

static bool run_cmd(const char *cmd) {
    char full_cmd[1024];
    snprintf(full_cmd, sizeof(full_cmd), "%s >> /tmp/fossil_err.log 2>&1", cmd);
    for (int attempt = 0; attempt <= 30; attempt++) {
        if (attempt > 0) sleep(1);
        if (system(full_cmd) == 0) return true;
    }
    return false;
}

bool fossil_init(const char *repo_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fossil init %s", repo_path);
    return run_cmd(cmd);
}

int fossil_ticket_list(char *buffer, size_t max_size) {
    char cmd[1024];
    if (global_repo_path[0]) {
        snprintf(cmd, sizeof(cmd), "fossil ticket show 0 -R %s 2>/dev/null", global_repo_path);
    } else {
        snprintf(cmd, sizeof(cmd), "fossil ticket show 0 2>/dev/null");
    }
    
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }

    size_t total_read = 0;
    size_t bytes_read;
    while ((bytes_read = fread(buffer + total_read, 1, max_size - total_read - 1, fp)) > 0) {
        total_read += bytes_read;
        if (total_read >= max_size - 1) {
            break;
        }
    }
    
    buffer[total_read] = '\0';
    pclose(fp);
    
    return total_read;
}

int fossil_ticket_list_parsed(FossilTicket *tickets, int max_tickets) {
    if (!global_repo_path[0]) return 0;

    // Ensure custom columns exist (no-op after first run; all output suppressed).
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
             "SELECT tkt_uuid || '|' || coalesce(title,'') || '|' || "
             "coalesce(status,'') || '|' || coalesce(private_contact,'') || '|' || "
             "coalesce(comment,'') || '|' || coalesce(reviewer_notes,'') "
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

        if (uuid && strlen(uuid) > 0) {
            strncpy(tickets[count].tkt_uuid,        uuid,           sizeof(tickets[count].tkt_uuid) - 1);
            strncpy(tickets[count].title,            title          ? title          : "", sizeof(tickets[count].title) - 1);
            strncpy(tickets[count].status,           status         ? status         : "Open", sizeof(tickets[count].status) - 1);
            strncpy(tickets[count].assignee,         assignee       ? assignee       : "", sizeof(tickets[count].assignee) - 1);
            strncpy(tickets[count].comment,          comment        ? comment        : "", sizeof(tickets[count].comment) - 1);
            strncpy(tickets[count].reviewer_notes,   reviewer_notes ? reviewer_notes : "", sizeof(tickets[count].reviewer_notes) - 1);
            count++;
        }
    }

    pclose(fp);
    return count;
}

bool fossil_ticket_create(const char *title, const char *description) {
    char cmd[2048];
    if (global_repo_path[0]) {
        snprintf(cmd, sizeof(cmd), "fossil ticket add title \"%s\" comment \"%s\" -R %s", title, description, global_repo_path);
    } else {
        snprintf(cmd, sizeof(cmd), "fossil ticket add title \"%s\" comment \"%s\"", title, description);
    }
    return run_cmd(cmd);
}

bool fossil_ticket_assign(const char *ticket_id, const char *agent_name) {
    char cmd[512];
    if (global_repo_path[0]) {
        snprintf(cmd, sizeof(cmd), "fossil ticket set %s private_contact \"%s\" -R %s", ticket_id, agent_name, global_repo_path);
    } else {
        snprintf(cmd, sizeof(cmd), "fossil ticket set %s private_contact \"%s\"", ticket_id, agent_name);
    }
    return run_cmd(cmd);
}

bool fossil_ticket_set_status(const char *ticket_id, const char *status) {
    char cmd[512];
    if (global_repo_path[0]) {
        snprintf(cmd, sizeof(cmd), "fossil ticket set %s status \"%s\" -R %s", ticket_id, status, global_repo_path);
    } else {
        snprintf(cmd, sizeof(cmd), "fossil ticket set %s status \"%s\"", ticket_id, status);
    }
    bool res = run_cmd(cmd);
    char log[1024];
    snprintf(log, sizeof(log), "fossil_ticket_set_status: cmd='%s', result=%d", cmd, res);
    log_message(log);
    return res;
}

bool fossil_commit(const char *workspace, const char *message) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cd %s && fossil commit -m \"%s\"", workspace, message);
    return run_cmd(cmd);
}

bool fossil_ticket_add_note(const char *ticket_id, const char *note) {
    if (!global_repo_path[0] || !ticket_id || !note) return false;

    // Ensure the changelog column exists; ALTER TABLE is a no-op if already present.
    char ensure_col[512];
    snprintf(ensure_col, sizeof(ensure_col),
             "printf 'ALTER TABLE ticket ADD COLUMN changelog TEXT;\\n' "
             "| fossil sqlite -R %s >/dev/null 2>&1",
             global_repo_path);
    system(ensure_col);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "fossil ticket set %s changelog \"[MEI %04d-%02d-%02d %02d:%02d] %s\" -R %s",
             ticket_id,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min,
             note, global_repo_path);
    return run_cmd(cmd);
}

int fossil_ticket_read_wiki_log(const char *ticket_id, char *buffer, size_t max_size) {
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

bool fossil_wiki_append_log(const char *ticket_id, const char *agent, const char *message) {
    if (!global_repo_path[0] || !ticket_id || !agent || !message) return false;

    // Wiki page name: "ticket-" + first 10 hex chars of UUID (safe for Fossil wiki names)
    char page_name[32];
    snprintf(page_name, sizeof(page_name), "ticket-%.10s", ticket_id);

    // Create a temp file to hold the accumulated wiki content
    char tmp_path[64] = "/tmp/mei_wiki_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return false;
    close(fd);

    // Try to export the existing page content into the temp file.
    // If the page doesn't exist yet fossil wiki export fails; temp file stays empty.
    char export_cmd[1024];
    snprintf(export_cmd, sizeof(export_cmd),
             "fossil wiki export \"%s\" %s -R %s >/dev/null 2>&1",
             page_name, tmp_path, global_repo_path);
    system(export_cmd);

    // Append the new timestamped entry (Fossil wiki / Markdown bold syntax)
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    FILE *f = fopen(tmp_path, "a");
    if (!f) { unlink(tmp_path); return false; }
    fprintf(f, "\n**[%04d-%02d-%02d %02d:%02d] %s:** %s\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min,
            agent, message);
    fclose(f);

    // Commit the updated page; fall back to create if the page doesn't exist yet.
    // system() used directly so our >/dev/null redirects are not broken by run_cmd's suffix.
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
