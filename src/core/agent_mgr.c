#include "core/agent_mgr.h"
#include "core/fossil_skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int agent_mgr_load_all(Agent *agents) {
    int count = 0;
    const char *repo_path = fossil_get_repo_path();
    
    if (!repo_path || strlen(repo_path) == 0) {
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "fossil ls -r trunk -R %s 2>/dev/null", repo_path);
    
    FILE *ls_fp = popen(cmd, "r");
    if (ls_fp) {
        char filename[512];
        while (fgets(filename, sizeof(filename), ls_fp) && count < MAX_AGENTS) {
            // Remove newline
            filename[strcspn(filename, "\n")] = 0;
            
            // Check if file is in .agents/ and ends with .md
            if (strncmp(filename, ".agents/", 8) == 0) {
                const char *ext = strrchr(filename, '.');
                if (ext && strcmp(ext, ".md") == 0) {
                    
                    // Now read the file contents using fossil cat
                    char cat_cmd[2048];
                    snprintf(cat_cmd, sizeof(cat_cmd), "fossil cat \"%s\" -r trunk -R %s 2>/dev/null", filename, repo_path);
                    
                    FILE *cat_fp = popen(cat_cmd, "r");
                    if (cat_fp) {
                        char line[2048];
                        Agent *a = &agents[count];
                        memset(a, 0, sizeof(Agent));
                        
                        a->state = AGENT_STATE_OPEN;
                        a->last_heartbeat = 0;
                        strcpy(a->current_ticket, "None");

                        while (fgets(line, sizeof(line), cat_fp)) {
                            char key[64];
                            char value[1024];
                            if (sscanf(line, "%63[^:]: %[^\n]", key, value) == 2) {
                                if (strcmp(key, "name") == 0) {
                                    strncpy(a->name, value, sizeof(a->name)-1);
                                } else if (strcmp(key, "role") == 0) {
                                    strncpy(a->role, value, sizeof(a->role)-1);
                                } else if (strcmp(key, "cli") == 0) {
                                    strncpy(a->cli, value, sizeof(a->cli)-1);
                                } else if (strcmp(key, "cmd") == 0) {
                                    strncpy(a->cmd, value, sizeof(a->cmd)-1);
                                } else if (strcmp(key, "description") == 0) {
                                    strncpy(a->description, value, sizeof(a->description)-1);
                                } else if (strcmp(key, "capabilities") == 0) {
                                    strncpy(a->capabilities, value, sizeof(a->capabilities)-1);
                                }
                            }
                        }
                        pclose(cat_fp);
                        
                        // Compute SHA1 hash of the agent name for private_contact matching
                        // Use cut instead of awk to avoid env issues in popen
                        char hash_cmd[256];
                        snprintf(hash_cmd, sizeof(hash_cmd), "printf '%%s' \"%s\" | shasum | cut -d' ' -f1", a->name);
                        FILE *hash_fp = popen(hash_cmd, "r");
                        if (hash_fp) {
                            if (fgets(a->hash, sizeof(a->hash), hash_fp)) {
                                a->hash[strcspn(a->hash, "\n \t")] = 0; // strip newline and trailing spaces
                            }
                            pclose(hash_fp);
                        }
                        
                        if (strlen(a->name) > 0) {
                            count++;
                        }
                    }
                }
            }
        }
        pclose(ls_fp);
    }
    
    return count;
}
