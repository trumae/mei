#include "core/tmux_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tmux_session_exists(const char *agent_name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux has-session -t agent:%s 2>/dev/null", agent_name);
    int status = system(cmd);
    return (status == 0);
}

bool tmux_spawn_agent(const char *agent_name, const char *cli_command, const char *workspace) {
    if (tmux_session_exists(agent_name)) {
        return true; // Already running
    }
    
    char cmd[1024];
    // Start detached session with the agent name in the given workspace.
    // We start 'sh' and then send the command to ensure we have a shell.
    snprintf(cmd, sizeof(cmd), "cd %s && tmux new-session -d -s agent:%s", workspace, agent_name);
    
    if (system(cmd) != 0) {
        return false;
    }

    // Send the CLI command if provided
    if (cli_command && strlen(cli_command) > 0) {
        snprintf(cmd, sizeof(cmd), "tmux send-keys -t agent:%s \"%s\" C-m", agent_name, cli_command);
        system(cmd);
    }

    return true;
}

bool tmux_kill_agent(const char *agent_name) {
    if (!tmux_session_exists(agent_name)) {
        return true;
    }
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux kill-session -t agent:%s", agent_name);
    return (system(cmd) == 0);
}

bool tmux_send_pulse(const char *agent_name, const char *pulse_payload) {
    if (!tmux_session_exists(agent_name)) {
        return false;
    }

    // Escape quotes and special characters could be complex here, assuming sanitized payload or simple text
    // A robust implementation would write the payload to a temp file and send `cat /tmp/file` or use `load-buffer`
    char cmd[4096];
    
    // Using load-buffer and paste-buffer is safer for multiline / complex payloads in tmux
    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/pulse_%s.txt", agent_name);
    
    FILE *f = fopen(tmp_file, "w");
    if (!f) return false;
    fprintf(f, "%s", pulse_payload);
    fclose(f);

    snprintf(cmd, sizeof(cmd), "tmux load-buffer %s && tmux paste-buffer -t agent:%s && tmux send-keys -t agent:%s C-m", tmp_file, agent_name, agent_name);
    int res = system(cmd);
    
    remove(tmp_file);
    return (res == 0);
}

int tmux_capture_output(const char *agent_name, char *buffer, size_t max_size) {
    if (!tmux_session_exists(agent_name)) {
        return -1;
    }

    char cmd[256];
    // -p prints to stdout
    snprintf(cmd, sizeof(cmd), "tmux capture-pane -p -t agent:%s", agent_name);
    
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
