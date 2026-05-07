#include "cmd/new.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Agent scaffold templates.
// description: MUST be the last field — the parser accumulates all subsequent lines.
static const char *PLANNER_MD =
    "name: planner\n"
    "role: planner\n"
    "cli: claude\n"
    "cmd: claude --dangerously-skip-permissions\n"
    "capabilities: [planning, fossil-read, architecture]\n"
    "description: Planning agent. Analyzes open tickets and decomposes them into\n"
    "  concrete sub-tasks assigned to executor agents. Never self-assigns tickets.\n"
    "  Edit this description to define your planner's persona and domain expertise.\n";

static const char *CODER_MD =
    "name: coder\n"
    "role: coder\n"
    "cli: claude\n"
    "cmd: claude --dangerously-skip-permissions\n"
    "capabilities: [coding, fossil-commit, debugging]\n"
    "description: Coding agent. Picks up Planned tickets, implements the required\n"
    "  changes on a dedicated branch, and submits for review. Edit this description\n"
    "  to define your coder's tech stack and preferred implementation style.\n";

static const char *REVIEWER_MD =
    "name: reviewer\n"
    "role: reviewer\n"
    "cli: claude\n"
    "cmd: claude --dangerously-skip-permissions\n"
    "capabilities: [code-review, qa, fossil-read]\n"
    "description: Review agent. Validates that coder deliverables meet the ticket\n"
    "  requirements. Approves by merging into trunk (Done) or rejects with detailed\n"
    "  feedback (Rework). Edit this description to define your quality standards.\n";

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

int cmd_new(int argc, char *argv[]) {
    const char *name    = NULL;
    const char *out_dir = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
            out_dir = argv[++i];
        else if (!name)
            name = argv[i];
    }

    if (!name) {
        fprintf(stderr, "Usage: mei new <name> [--dir <path>]\n");
        return 1;
    }

    // Name must not contain spaces or path separators
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == ' ' || *p == '\\') {
            fprintf(stderr, "Error: name must not contain spaces or path separators.\n");
            return 1;
        }
    }

    // Build fossil file path
    char fossil_path[1024];
    snprintf(fossil_path, sizeof(fossil_path), "%s/%s.fossil", out_dir, name);

    struct stat st;
    if (stat(fossil_path, &st) == 0) {
        fprintf(stderr, "Error: '%s' already exists.\n", fossil_path);
        return 1;
    }

    // Temporary checkout directory
    char tmpdir[] = "/tmp/mei_new_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }

    printf("Creating %s...\n\n", fossil_path);

    char cmd[2048];
    int  rc = 0;

    // Step 1: init
    printf("  [1/4] Initializing fossil repository...  ");
    fflush(stdout);
    snprintf(cmd, sizeof(cmd), "fossil init \"%s\" > /dev/null 2>&1", fossil_path);
    rc = system(cmd);
    if (rc != 0) { printf("FAILED\n"); goto cleanup; }
    printf("ok\n");

    // Resolve to absolute path for fossil open
    char abs_fossil[1024];
    if (!realpath(fossil_path, abs_fossil)) {
        fprintf(stderr, "Error: could not resolve '%s'\n", fossil_path);
        rc = 1; goto cleanup;
    }

    // Step 2: open + scaffold
    printf("  [2/4] Creating agent scaffolds...        ");
    fflush(stdout);
    snprintf(cmd, sizeof(cmd),
             "cd \"%s\" && fossil open \"%s\" > /dev/null 2>&1", tmpdir, abs_fossil);
    rc = system(cmd);
    if (rc != 0) { printf("FAILED\n"); goto cleanup; }

    char agents_dir[1200];
    snprintf(agents_dir, sizeof(agents_dir), "%s/.agents", tmpdir);
    mkdir(agents_dir, 0755);

    char path[1300];
    snprintf(path, sizeof(path), "%s/planner.md",  agents_dir);
    if (write_file(path, PLANNER_MD)  != 0) { printf("FAILED (planner.md)\n");  rc = 1; goto cleanup; }
    snprintf(path, sizeof(path), "%s/coder.md",    agents_dir);
    if (write_file(path, CODER_MD)    != 0) { printf("FAILED (coder.md)\n");    rc = 1; goto cleanup; }
    snprintf(path, sizeof(path), "%s/reviewer.md", agents_dir);
    if (write_file(path, REVIEWER_MD) != 0) { printf("FAILED (reviewer.md)\n"); rc = 1; goto cleanup; }
    printf("ok\n");

    // Step 3: add + commit
    printf("  [3/4] Committing initial definitions...  ");
    fflush(stdout);
    snprintf(cmd, sizeof(cmd),
             "cd \"%s\" && fossil add .agents/ > /dev/null 2>&1 && "
             "fossil commit -m \"Initial scaffold: MEI agent definitions\" > /dev/null 2>&1",
             tmpdir);
    rc = system(cmd);
    if (rc != 0) { printf("FAILED\n"); goto cleanup; }
    printf("ok\n");

    // Step 4: close checkout
    printf("  [4/4] Cleaning up...                     ");
    fflush(stdout);
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && fossil close > /dev/null 2>&1", tmpdir);
    system(cmd);
    printf("ok\n");

    printf("\nRepository ready: %s\n\n", abs_fossil);
    printf("Next steps:\n");
    printf("  1. Customize agents (set cli, cmd, and persona description):\n");
    printf("       fossil open %s\n", abs_fossil);
    printf("       $EDITOR .agents/planner.md .agents/coder.md .agents/reviewer.md\n");
    printf("       fossil commit -m \"Customize agents\"\n");
    printf("       fossil close\n\n");
    printf("  2. Create your first ticket:\n");
    printf("       fossil ticket add title \"My first task\" -R %s\n\n", abs_fossil);
    printf("  3. Run the orchestrator:\n");
    printf("       mei run %s\n\n", abs_fossil);

cleanup:
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
    if (rc != 0) fprintf(stderr, "\nFailed. Partial output may remain at %s\n", fossil_path);
    return (rc == 0) ? 0 : 1;
}
