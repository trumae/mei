#include "core/tmux_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMUX_SESSION "mei"

static void ensure_session_exists() {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux has-session -t %s 2>/dev/null", TMUX_SESSION);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "tmux new-session -d -s %s", TMUX_SESSION);
        if (system(cmd) != 0) {
            char err[4096];
            snprintf(err, sizeof(err), "echo 'Failed: %s' >> /tmp/tmux_err.log", cmd);
            system(err);
        }
    }
}

bool tmux_session_exists(const char *agent_name) {
    char cmd[512];
    // Check if the window exists in our session
    snprintf(cmd, sizeof(cmd), "tmux list-windows -t %s -F \"#W\" 2>/dev/null | grep -qx \"%s\"", TMUX_SESSION, agent_name);
    int status = system(cmd);
    return (status == 0);
}

bool tmux_spawn_agent(const char *agent_name, const char *cli_command, const char *workspace) {
    ensure_session_exists();

    if (tmux_session_exists(agent_name)) {
        return true; // Already running
    }
    
    char cmd[2048];
    // Create a new window in the existing session
    if (cli_command && strlen(cli_command) > 0) {
        snprintf(cmd, sizeof(cmd), "tmux new-window -d -t %s -n \"%s\" -c \"%s\" %s", TMUX_SESSION, agent_name, workspace, cli_command);
    } else {
        snprintf(cmd, sizeof(cmd), "tmux new-window -d -t %s -n \"%s\" -c \"%s\"", TMUX_SESSION, agent_name, workspace);
    }
    
    int res = system(cmd); if(res!=0) { char err[4096]; snprintf(err, sizeof(err), "echo 'Failed: %s' >> /tmp/tmux_err.log", cmd); system(err); } return res == 0;
}

bool tmux_kill_agent(const char *agent_name) {
    if (!tmux_session_exists(agent_name)) {
        return true;
    }
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux kill-window -t %s:\"%s\"", TMUX_SESSION, agent_name);
    int res = system(cmd); if(res!=0) { char err[4096]; snprintf(err, sizeof(err), "echo 'Failed: %s' >> /tmp/tmux_err.log", cmd); system(err); } return res == 0;
}

bool tmux_send_pulse(const char *agent_name, const char *pulse_payload) {
    if (!tmux_session_exists(agent_name)) {
        return false;
    }

    char tmp_payload[256];
    snprintf(tmp_payload, sizeof(tmp_payload), "/tmp/pulse_%s.txt", agent_name);
    FILE *f = fopen(tmp_payload, "w");
    if (!f) return false;
    fprintf(f, "%s", pulse_payload);
    fclose(f);

    char tmp_script[256];
    snprintf(tmp_script, sizeof(tmp_script), "/tmp/pulse_%s.sh", agent_name);
    FILE *sf = fopen(tmp_script, "w");
    if (!sf) { remove(tmp_payload); return false; }
    
    // Use tmux load-buffer + paste-buffer so that the entire PULSE lands as a
    // single chunk of text, not as N separate Enter-submitted messages.
    // paste-buffer triggers bracketed paste mode on the target pane (if the CLI
    // supports it, e.g. claude, opencode), so embedded newlines are treated as
    // literal newlines in the input field — not as submit actions.
    // A single Enter after the paste submits the complete PULSE as one message.
    fprintf(sf,
            "#!/bin/bash\n"
            "tmux load-buffer '%s'\n"
            "tmux paste-buffer -t '%s:%s'\n"
            "sleep 0.3\n"
            "tmux send-keys -t '%s:%s' Enter\n",
            tmp_payload,
            TMUX_SESSION, agent_name,
            TMUX_SESSION, agent_name);
    fclose(sf);

    system("chmod +x /tmp/pulse_*.sh");
    int res = system(tmp_script);
    
    remove(tmp_payload);
    remove(tmp_script);
    return (res == 0);
}

int tmux_capture_output(const char *agent_name, char *buffer, size_t max_size) {
    if (!tmux_session_exists(agent_name)) {
        return -1;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux capture-pane -p -t %s:\"%s\"", TMUX_SESSION, agent_name);
    
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

