#include "cmd/new.h"
#include "core/vcs_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ──────────────────────────────────────────────
// Agent scaffold templates — Fossil (fossil-* capabilities)
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

// ──────────────────────────────────────────────
// Agent scaffold templates — GitHub (git-* capabilities)
static const char *GH_PLANNER_MD =
    "name: planner\n"
    "role: planner\n"
    "cli: claude\n"
    "cmd: claude --dangerously-skip-permissions\n"
    "capabilities: [planning, git-read, architecture]\n"
    "description: Planning agent. Analyzes open GitHub issues and decomposes them into\n"
    "  concrete sub-tasks assigned to executor agents. Never self-assigns tickets.\n"
    "  Edit this description to define your planner's persona and domain expertise.\n";

static const char *GH_CODER_MD =
    "name: coder\n"
    "role: coder\n"
    "cli: claude\n"
    "cmd: claude --dangerously-skip-permissions\n"
    "capabilities: [coding, git-commit, debugging]\n"
    "description: Coding agent. Picks up Planned issues, implements the required\n"
    "  changes on a dedicated branch, and submits for review. Edit this description\n"
    "  to define your coder's tech stack and preferred implementation style.\n";

static const char *GH_REVIEWER_MD =
    "name: reviewer\n"
    "role: reviewer\n"
    "cli: claude\n"
    "cmd: claude --dangerously-skip-permissions\n"
    "capabilities: [code-review, qa, git-read]\n"
    "description: Review agent. Validates that coder deliverables meet the issue\n"
    "  requirements. Approves (Done) or rejects with detailed feedback (Rework).\n"
    "  Edit this description to define your quality standards.\n";

// All MEI status labels to create in a GitHub repo.
static const char *MEI_LABELS[][2] = {
    { "status:Open",             "FBCA04" },
    { "status:Planned",          "0075CA" },
    { "status:In Progress",      "0E8A16" },
    { "status:Review",           "7057FF" },
    { "status:Rework",           "E4E669" },
    { "status:Blocked",          "EE0701" },
    { "status:Done",             "CCCCCC" },
    { "status:QA Ready",         "95D0A4" },
    { "status:Pending Approval", "FFD700" },
    { "status:Delegated",        "00AABB" },
    { "status:Verified",         "006B75" },
    { "status:closed",           "CCCCCC" },
    { NULL, NULL }
};

static int cmd_new_github(const char *owner_repo) {
    printf("Setting up MEI on github.com/%s...\n\n", owner_repo);
    char cmd[2048];
    int rc = 0;

    // Step 1: verify repo access
    printf("  [1/4] Verifying repository access...    ");
    fflush(stdout);
    snprintf(cmd, sizeof(cmd),
             "gh api repos/%s > /dev/null 2>&1", owner_repo);
    rc = system(cmd);
    if (rc != 0) {
        printf("FAILED\n");
        fprintf(stderr, "Error: cannot access '%s'. Check gh auth and repo name.\n", owner_repo);
        return 1;
    }
    printf("ok\n");

    // Step 2: create status labels
    printf("  [2/4] Creating MEI labels...             ");
    fflush(stdout);
    for (int i = 0; MEI_LABELS[i][0]; i++) {
        snprintf(cmd, sizeof(cmd),
                 "gh label create '%s' -R '%s' --color '%s' --force > /dev/null 2>&1",
                 MEI_LABELS[i][0], owner_repo, MEI_LABELS[i][1]);
        system(cmd);  // --force upserts, so ignore individual failures
    }
    printf("ok\n");

    // Step 3: clone (or init for empty repos) and scaffold .mei/
    printf("  [3/4] Creating agent scaffolds...        ");
    fflush(stdout);
    char tmpdir[] = "/tmp/mei_gh_XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }

    // gh repo clone respects the user's configured git protocol (SSH or HTTPS)
    snprintf(cmd, sizeof(cmd),
             "gh repo clone '%s' '%s' -- --quiet > /dev/null 2>&1",
             owner_repo, tmpdir);
    if (system(cmd) != 0) {
        // Repo has no commits yet — ask gh for the preferred clone URL, then init locally
        char origin_url[512] = "";
        char url_cmd[256];
        snprintf(url_cmd, sizeof(url_cmd),
                 "gh api repos/%s --jq '.ssh_url' 2>/dev/null", owner_repo);
        FILE *fp = popen(url_cmd, "r");
        if (fp) {
            if (fgets(origin_url, sizeof(origin_url), fp)) {
                char *nl = strchr(origin_url, '\n');
                if (nl) *nl = '\0';
            }
            pclose(fp);
        }
        if (!origin_url[0])
            snprintf(origin_url, sizeof(origin_url),
                     "https://github.com/%s.git", owner_repo);

        snprintf(cmd, sizeof(cmd),
                 "rm -rf '%s' && git init '%s' > /dev/null 2>&1 && "
                 "git -C '%s' remote add origin '%s' > /dev/null 2>&1",
                 tmpdir, tmpdir, tmpdir, origin_url);
        rc = system(cmd);
        if (rc != 0) { printf("FAILED\n"); goto gh_cleanup; }
    }

    char mei_dir[512];
    snprintf(mei_dir, sizeof(mei_dir), "%s/.mei", tmpdir);
    mkdir(mei_dir, 0755);

    char fpath[600];
    snprintf(fpath, sizeof(fpath), "%s/planner.md",  mei_dir);
    if (write_file(fpath, GH_PLANNER_MD)  != 0) { printf("FAILED (planner.md)\n");  rc = 1; goto gh_cleanup; }
    snprintf(fpath, sizeof(fpath), "%s/coder.md",    mei_dir);
    if (write_file(fpath, GH_CODER_MD)    != 0) { printf("FAILED (coder.md)\n");    rc = 1; goto gh_cleanup; }
    snprintf(fpath, sizeof(fpath), "%s/reviewer.md", mei_dir);
    if (write_file(fpath, GH_REVIEWER_MD) != 0) { printf("FAILED (reviewer.md)\n"); rc = 1; goto gh_cleanup; }
    printf("ok\n");

    // Step 4: commit and push
    // Commit is best-effort: if .mei/ is already up to date (re-run), nothing to commit is fine.
    // Push is the definitive check.
    printf("  [4/4] Committing and pushing scaffolds... ");
    fflush(stdout);
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git add .mei/ && "
             "(git diff --cached --quiet || "
             " git commit -m 'Initial MEI scaffold: agent definitions' > /dev/null 2>&1)",
             tmpdir);
    system(cmd);  // tolerate "nothing to commit"

    snprintf(cmd, sizeof(cmd),
             "git -C '%s' push -u origin HEAD > /dev/null 2>&1", tmpdir);
    rc = system(cmd);
    if (rc != 0) { printf("FAILED\n"); goto gh_cleanup; }
    printf("ok\n");

    printf("\nRepository ready: github.com/%s\n\n", owner_repo);
    printf("Next steps:\n");
    printf("  1. Customize agents (set cli, cmd, and persona description):\n");
    printf("       git clone https://github.com/%s && cd %s\n", owner_repo,
           strrchr(owner_repo, '/') ? strrchr(owner_repo, '/') + 1 : owner_repo);
    printf("       $EDITOR .mei/planner.md .mei/coder.md .mei/reviewer.md\n");
    printf("       git commit -am \"Customize agents\" && git push\n\n");
    printf("  2. Create your first issue on GitHub (set label status:Open).\n\n");
    printf("  3. Run the orchestrator:\n");
    printf("       mei github run %s\n\n", owner_repo);

gh_cleanup:
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    system(cmd);
    return (rc == 0) ? 0 : 1;
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
        fprintf(stderr, "Usage:\n"
                "  mei fossil new <name> [--dir <path>]   Create a Fossil repo\n"
                "  mei github new <owner/repo>             Set up MEI on existing GitHub repo\n");
        return 1;
    }

    // GitHub path: backend is GitHub OR name contains '/'
    if ((g_backend && g_backend->type == VCS_GITHUB) || strchr(name, '/'))
        return cmd_new_github(name);

    // Fossil path: validate name (no path separators or spaces)
    for (const char *p = name; *p; p++) {
        if (*p == ' ' || *p == '\\') {
            fprintf(stderr, "Error: name must not contain spaces or backslashes.\n");
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
    snprintf(agents_dir, sizeof(agents_dir), "%s/.mei", tmpdir);
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
             "cd \"%s\" && fossil add .mei/ > /dev/null 2>&1 && "
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
    printf("       $EDITOR .mei/planner.md .mei/coder.md .mei/reviewer.md\n");
    printf("       fossil commit -m \"Customize agents\"\n");
    printf("       fossil close\n\n");
    printf("  2. Create your first ticket:\n");
    printf("       fossil ticket add title \"My first task\" -R %s\n\n", abs_fossil);
    printf("  3. Run the orchestrator:\n");
    printf("       mei fossil run %s\n\n", abs_fossil);

cleanup:
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
    if (rc != 0) fprintf(stderr, "\nFailed. Partial output may remain at %s\n", fossil_path);
    return (rc == 0) ? 0 : 1;
}
