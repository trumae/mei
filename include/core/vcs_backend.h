#ifndef VCS_BACKEND_H
#define VCS_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include "core/config.h"

typedef enum { VCS_FOSSIL, VCS_GITHUB } VCSType;

// Unified ticket representation across all backends.
// Fossil: uuid=tkt_uuid, description=comment field, assignee=private_contact.
// GitHub: uuid="#<number>", description=issue body, assignee=label "assignee:<hash>".
typedef struct {
    char uuid[64];
    char title[128];
    char status[64];
    char assignee[64];
    char description[2048];    // summary; full content via ticket_read_initial_description()
    char reviewer_notes[2048];
} VCSTicket;

typedef struct VCSBackend {
    // Ticket CRUD
    int  (*ticket_list)(struct VCSBackend *b, VCSTicket *out, int max);
    bool (*ticket_assign)(struct VCSBackend *b, const char *uuid, const char *agent_hash);
    bool (*ticket_set_status)(struct VCSBackend *b, const char *uuid, const char *status);
    bool (*ticket_set_reviewer_notes)(struct VCSBackend *b, const char *uuid, const char *notes);
    bool (*ticket_add_note)(struct VCSBackend *b, const char *uuid, const char *note);

    // Audit log: Fossil uses wiki pages; GitHub uses issue comments with prefixes.
    bool (*ticket_append_log)(struct VCSBackend *b, const char *uuid, const char *agent, const char *msg);
    int  (*ticket_read_log)(struct VCSBackend *b, const char *uuid, char *buf, size_t max);
    int  (*ticket_read_human_remarks)(struct VCSBackend *b, const char *uuid, char *buf, size_t max);
    // Fossil: reads icomment from creation artifact (web UI tickets).
    // GitHub: always returns 0 (description is already in VCSTicket.description).
    int  (*ticket_read_initial_description)(struct VCSBackend *b, const char *uuid, char *buf, size_t max);

    // Workspace / VCS operations
    bool (*workspace_init)(struct VCSBackend *b, const char *workspace_path);
    bool (*branch_create)(struct VCSBackend *b, const char *workspace_path, const char *branch_name);
    bool (*branch_switch)(struct VCSBackend *b, const char *workspace_path, const char *branch_name);
    bool (*commit)(struct VCSBackend *b, const char *workspace_path, const char *message);

    // Agent definition files (.mei/ dir with .md files)
    // list_agent_files: returns filenames (without dir prefix) in out[]. Returns count.
    // read_agent_file:  reads content of dir/filename into buf. Returns bytes read.
    int  (*list_agent_files)(struct VCSBackend *b, const char *dir, char out[][256], int max);
    int  (*read_agent_file)(struct VCSBackend *b, const char *dir, const char *filename,
                            char *buf, size_t max);

    VCSType type;
    char    repo_id[256];   // .fossil path (Fossil) or "owner/repo" (GitHub)
} VCSBackend;

// Global backend instance — set by vcs_backend_create() at startup.
extern VCSBackend *g_backend;

VCSBackend *vcs_backend_create(VCSType type, const char *repo_id);
void        vcs_backend_destroy(VCSBackend *b);

#endif // VCS_BACKEND_H
