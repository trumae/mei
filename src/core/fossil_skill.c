#include "core/fossil_skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    int res = system(full_cmd);
    return (res == 0);
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
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "printf \".mode list\\nSELECT tkt_uuid || '|' || coalesce(title, '') || '|' || coalesce(status, '') || '|' || coalesce(private_contact, '') || '|' || coalesce(comment, '') FROM ticket WHERE status != 'Closed' AND status != 'done';\\n\" | fossil sqlite -R %s 2>/dev/null", global_repo_path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    
    int count = 0;
    char line[MEI_TEXT_BUFFER_SIZE + 2048];
    while (fgets(line, sizeof(line), fp) && count < max_tickets) {
        line[strcspn(line, "\n")] = 0; // Remove newline
        
        char *fields[5];
        char *ptr = line;
        for (int i = 0; i < 5; i++) {
            fields[i] = ptr;
            char *sep = strchr(ptr, '|');
            if (sep) {
                *sep = '\0';
                ptr = sep + 1;
            } else {
                // Last field or missing fields
                if (i < 4) ptr = ""; // Should not happen with well-formed output
            }
        }
        
        char *uuid = fields[0];
        char *title = fields[1];
        char *status = fields[2];
        char *assignee = fields[3];
        char *comment = fields[4];
        
        if (uuid && strlen(uuid) > 0) {
            strncpy(tickets[count].tkt_uuid, uuid, sizeof(tickets[count].tkt_uuid) - 1);
            strncpy(tickets[count].title, title ? title : "", sizeof(tickets[count].title) - 1);
            strncpy(tickets[count].status, status ? status : "Open", sizeof(tickets[count].status) - 1);
            strncpy(tickets[count].assignee, assignee ? assignee : "", sizeof(tickets[count].assignee) - 1);
            strncpy(tickets[count].comment, comment ? comment : "", sizeof(tickets[count].comment) - 1);
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
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    // Format: [MEI YYYY-MM-DD HH:MM] <note> — stored in 'changelog' field.
    // Notes are orchestrator-generated (no shell-special chars), so direct quoting is safe.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "fossil ticket set %s changelog \"[MEI %04d-%02d-%02d %02d:%02d] %s\" -R %s",
             ticket_id,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min,
             note, global_repo_path);
    return run_cmd(cmd);
}
