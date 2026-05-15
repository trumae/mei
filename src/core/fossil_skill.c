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
    // Use char(1) (SOH) as a newline placeholder so multi-line comment/reviewer_notes
    // fields don't break the line-by-line fgets parser.  We restore \n after parsing.
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
            strncpy(tickets[count].tkt_uuid,        uuid,           sizeof(tickets[count].tkt_uuid) - 1);
            strncpy(tickets[count].title,            title          ? title          : "", sizeof(tickets[count].title) - 1);
            strncpy(tickets[count].status,           status         ? status         : "Open", sizeof(tickets[count].status) - 1);
            strncpy(tickets[count].assignee,         assignee       ? assignee       : "", sizeof(tickets[count].assignee) - 1);
            // Restore newlines from the SOH placeholder used in the SQL query
            char *f;
            f = tickets[count].comment;
            strncpy(f, comment ? comment : "", sizeof(tickets[count].comment) - 1);
            for (char *p = f; *p; p++) if ((unsigned char)*p == 1) *p = '\n';
            f = tickets[count].reviewer_notes;
            strncpy(f, reviewer_notes ? reviewer_notes : "", sizeof(tickets[count].reviewer_notes) - 1);
            for (char *p = f; *p; p++) if ((unsigned char)*p == 1) *p = '\n';
            count++;
        } else if (uuid && uuid[0] != '\0') {
            FILE *err = fopen("/tmp/fossil_err.log", "a");
            if (err) {
                fprintf(err, "[fossil_ticket_list_parsed] rejected malformed uuid: %.80s\n", uuid);
                fclose(err);
            }
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

bool fossil_ticket_set_status(const char *ticket_id, const char *status) {
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
             "fossil_ticket_set_status: ticket=%.10s status=%s result=%d",
             ticket_id, status, res);
    log_message(log_msg);
    return (res == 0);
}

bool fossil_ticket_set_reviewer_notes(const char *ticket_id, const char *notes) {
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

bool fossil_commit(const char *workspace, const char *message) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cd %s && fossil commit -m \"%s\"", workspace, message);
    return run_cmd(cmd);
}

bool fossil_ticket_add_note(const char *ticket_id, const char *note) {
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

// Read the icomment field from the ticket creation artifact.
// Fossil web UI stores the initial description as "J icomment VALUE" in the artifact,
// not in the ticket table's comment column. This function extracts it directly.
int fossil_ticket_read_icomment_from_artifact(const char *ticket_id,
                                               char *buffer, size_t max_size) {
    if (!global_repo_path[0] || !ticket_id || !buffer || max_size == 0) return 0;

    // Find the creation artifact hash: earliest 't'-type event for this ticket UUID.
    // Fossil truncates the UUID to 10 chars in event.comment (e.g. "7aa828bb74"),
    // so LIKE with the full 40-char UUID would never match.
    char uuid10[11] = {0};
    strncpy(uuid10, ticket_id, 10);

    // Use SQL as a CLI arg to fossil sqlite — avoids both the stdin/ncurses
    // interference issue and shell printf interpreting % in LIKE patterns.
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

    // Read the artifact and parse the "J icomment VALUE" line.
    // Fossil encodes spaces as \s in artifact field values.
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

int fossil_ticket_show_full(const char *ticket_id, char *buffer, size_t max_size) {
    if (!global_repo_path[0] || !ticket_id || !buffer || max_size == 0) return 0;

    FILE *dbg = fopen("/tmp/mei_tkt_debug.log", "a");

    // Step 1: write SQL to temp file
    char tmp_sql[64] = "/tmp/mei_tkt_sql_XXXXXX";
    int fd = mkstemp(tmp_sql);
    if (fd < 0) {
        if (dbg) { fprintf(dbg, "[show_full] mkstemp failed for %s\n", ticket_id); fclose(dbg); }
        return 0;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_sql); return 0; }

    fprintf(f,
        ".mode list\n"
        "SELECT "
        "'uuid:     ' || tkt_uuid         || char(10) ||"
        "'title:    ' || coalesce(title,'')            || char(10) ||"
        "'status:   ' || coalesce(status,'')           || char(10) ||"
        "'type:     ' || coalesce(type,'')             || char(10) ||"
        "'priority: ' || coalesce(priority,'')         || char(10) ||"
        "'severity: ' || coalesce(severity,'')         || char(10) ||"
        "'assignee: ' || coalesce(private_contact,'')  || char(10) ||"
        "char(10) || '--- DESCRIPTION ---'             || char(10) ||"
        "coalesce(nullif(comment,''), '(no description provided)') || char(10) ||"
        "char(10) || '--- REVIEWER NOTES ---'          || char(10) ||"
        "coalesce(nullif(reviewer_notes,''),'(none)')  || char(10) ||"
        "char(10) || '--- CHANGELOG ---'               || char(10) ||"
        "coalesce(nullif(changelog,''),'(none)')"
        " FROM ticket WHERE tkt_uuid LIKE '%s%%';\n",
        ticket_id);
    fclose(f);

    // Step 2: run fossil sqlite
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fossil sqlite -R %s < %s 2>/tmp/mei_tkt_sql_err.log",
             global_repo_path, tmp_sql);

    if (dbg) fprintf(dbg, "[show_full] cmd: %s\n", cmd);

    FILE *fp = popen(cmd, "r");
    unlink(tmp_sql);
    if (!fp) {
        if (dbg) { fprintf(dbg, "[show_full] popen failed\n"); fclose(dbg); }
        return 0;
    }

    size_t total = fread(buffer, 1, max_size - 1, fp);
    buffer[total] = '\0';
    pclose(fp);

    if (dbg) fprintf(dbg, "[show_full] SQL returned %zu bytes for ticket %.10s\n", total, ticket_id);

    // Step 3: if comment is empty, splice icomment from creation artifact
    char *sentinel = strstr(buffer, "(no description provided)");
    if (dbg) fprintf(dbg, "[show_full] sentinel found: %s\n", sentinel ? "YES" : "NO");

    if (sentinel) {
        char icomment[4096] = {0};
        int ic_len = fossil_ticket_read_icomment_from_artifact(ticket_id, icomment, sizeof(icomment));
        if (dbg) fprintf(dbg, "[show_full] icomment read: %d bytes: %.80s\n", ic_len, icomment);

        if (ic_len > 0) {
            size_t before       = (size_t)(sentinel - buffer);
            size_t sentinel_len = strlen("(no description provided)");
            size_t after        = total - before - sentinel_len;
            if (before + (size_t)ic_len + after < max_size - 1) {
                memmove(sentinel + ic_len, sentinel + sentinel_len, after + 1);
                memcpy(sentinel, icomment, ic_len);
                total = before + (size_t)ic_len + after;
                if (dbg) fprintf(dbg, "[show_full] splice OK, new total=%zu\n", total);
            } else {
                if (dbg) fprintf(dbg, "[show_full] splice skipped: would overflow\n");
            }
        }
    }

    if (dbg) fclose(dbg);
    return (int)total;
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

    // Normalize literal \n sequences written by agents into real newlines.
    // Agents sometimes produce escaped output (e.g. echo "line1\nline2") when
    // writing wiki content, which appears verbatim in the Fossil web UI.
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
                        if (nx == 'n')       fputc('\n', wf);
                        else if (nx == 't')  fputc('\t', wf);
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

// Read the human-facing change history for a ticket using `fossil ticket history`.
// Returns bytes written to buffer, 0 on failure.
int fossil_ticket_read_history(const char *ticket_id, char *buffer, size_t max_size) {
    if (!global_repo_path[0] || !ticket_id || !buffer || max_size == 0) return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fossil ticket history %.40s -R %s 2>/dev/null",
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

// Extract only the human-added icomment entries from ticket history.
// These are the remarks the human types in the Fossil web UI — they are stored
// as "Change icomment:" artifacts and are invisible in the ticket's comment column.
// The full history can be 10KB+; this function returns only the small subset that
// matters for plan feedback, so it always fits in the PULSE regardless of history size.
int fossil_ticket_read_human_remarks(const char *ticket_id, char *buffer, size_t max_size) {
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
