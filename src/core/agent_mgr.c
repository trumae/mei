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
    snprintf(cmd, sizeof(cmd), "fossil ls -R %s 2>/dev/null", repo_path);
    
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
                    snprintf(cat_cmd, sizeof(cat_cmd), "fossil cat \"%s\" -R %s 2>/dev/null", filename, repo_path);
                    
                    FILE *cat_fp = popen(cat_cmd, "r");
                    if (cat_fp) {
                        char line[256];
                        Agent *a = &agents[count];
                        memset(a, 0, sizeof(Agent));
                        
                        a->state = AGENT_STATE_OPEN;
                        a->last_heartbeat = 0;
                        strcpy(a->current_ticket, "None");

                        while (fgets(line, sizeof(line), cat_fp)) {
                            char key[64];
                            char value[192];
                            if (sscanf(line, "%63[^:]: %[^\n]", key, value) == 2) {
                                if (strcmp(key, "name") == 0) {
                                    strncpy(a->name, value, sizeof(a->name)-1);
                                } else if (strcmp(key, "role") == 0) {
                                    strncpy(a->role, value, sizeof(a->role)-1);
                                }
                            }
                        }
                        pclose(cat_fp);
                        
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
