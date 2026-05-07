#include "cmd/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_PATH      "/tmp/mei.log"
#define DEFAULT_LINES 50

#define ANSI_RESET   "\033[0m"
#define ANSI_DIM     "\033[2m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"

static void print_line(const char *line) {
    const char *col = ANSI_RESET;
    if (strstr(line, "[!]")    || strstr(line, "BLOCKED") || strstr(line, "FAILED"))
        col = ANSI_RED;
    else if (strstr(line, "[PULSE]"))
        col = ANSI_CYAN;
    else if (strstr(line, "[done]") || strstr(line, "[ok]"))
        col = ANSI_GREEN;
    else if (strstr(line, "[sync]") || strstr(line, "[init]") ||
             strstr(line, "[wait]") || strstr(line, "[trust]") || strstr(line, "[perm]"))
        col = ANSI_YELLOW;

    // Dim the timestamp "[HH:MM:SS]", then colorize the rest
    const char *bracket_close = strchr(line, ']');
    if (bracket_close) {
        printf("%s%.*s%s%s%s%s",
               ANSI_DIM,
               (int)(bracket_close - line + 1), line,
               ANSI_RESET,
               col,
               bracket_close + 1,
               ANSI_RESET);
    } else {
        printf("%s%s%s", col, line, ANSI_RESET);
    }
}

int cmd_log(int argc, char *argv[]) {
    int follow  = 0;
    int n_lines = DEFAULT_LINES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--follow") == 0 || strcmp(argv[i], "-f") == 0)
            follow = 1;
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            n_lines = atoi(argv[++i]);
    }

    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        fprintf(stderr, "Log not found: %s  (run 'mei run' first)\n", LOG_PATH);
        return 1;
    }

    // Count total lines, then rewind and skip to (total - n_lines)
    int total = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) total++;

    rewind(f);
    int skip = (total > n_lines) ? total - n_lines : 0;
    for (int i = 0; i < skip; i++) fgets(buf, sizeof(buf), f);

    // Print the tail
    long pos = ftell(f);
    while (fgets(buf, sizeof(buf), f)) {
        print_line(buf);
        pos = ftell(f);
    }

    if (!follow) {
        fclose(f);
        return 0;
    }

    // Follow mode: poll for new lines every 200ms
    fflush(stdout);
    while (1) {
        if (fgets(buf, sizeof(buf), f)) {
            print_line(buf);
            fflush(stdout);
            pos = ftell(f);
        } else {
            fseek(f, pos, SEEK_SET);
            usleep(200000);
        }
    }

    fclose(f);
    return 0;
}
