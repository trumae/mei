#include "core/fossil_skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char global_repo_path[1024] = {0};

void fossil_set_repo_path(const char *path) {
    if (path) {
        strncpy(global_repo_path, path, sizeof(global_repo_path) - 1);
    }
}

const char *fossil_get_repo_path() {
    return global_repo_path;
}

// Helper to run commands and check success
static bool run_cmd(const char *cmd) {
    int res = system(cmd);
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
    snprintf(cmd, sizeof(cmd), "echo \".mode list\nSELECT tkt_uuid || '|' || coalesce(title, '') || '|' || coalesce(status, '') || '|' || coalesce(private_contact, '') FROM ticket WHERE status != 'Closed' AND status != 'done';\" | fossil sqlite -R %s 2>/dev/null", global_repo_path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    
    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && count < max_tickets) {
        line[strcspn(line, "\n")] = 0; // Remove newline
        
        char *uuid = strtok(line, "|");
        char *title = strtok(NULL, "|");
        char *status = strtok(NULL, "|");
        char *assignee = strtok(NULL, "|");
        
        if (uuid) {
            strncpy(tickets[count].tkt_uuid, uuid, sizeof(tickets[count].tkt_uuid) - 1);
            strncpy(tickets[count].title, title ? title : "", sizeof(tickets[count].title) - 1);
            strncpy(tickets[count].status, status ? status : "Open", sizeof(tickets[count].status) - 1);
            strncpy(tickets[count].assignee, assignee ? assignee : "", sizeof(tickets[count].assignee) - 1);
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
    return run_cmd(cmd);
}

bool fossil_commit(const char *workspace, const char *message) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cd %s && fossil commit -m \"%s\"", workspace, message);
    return run_cmd(cmd);
}
