#ifndef FOSSIL_SKILL_H
#define FOSSIL_SKILL_H

#include <stdbool.h>
#include <stddef.h>

// Set the global fossil repository path to use for operations
void fossil_set_repo_path(const char *path);

// Get the current global fossil repository path
const char *fossil_get_repo_path();

#include "config.h"

typedef struct {
    char tkt_uuid[64];
    char title[128];
    char status[64];
    char assignee[64]; // Mapped to private_contact in Fossil
    char comment[MEI_TEXT_BUFFER_SIZE];
    char reviewer_notes[4096]; // Rejection feedback written by reviewer agent
} FossilTicket;

// Initialize the fossil repository if not exists (Mock or Real)
bool fossil_init(const char *repo_path);

// Fetch raw ticket list string
int fossil_ticket_list(char *buffer, size_t max_size);

// Fetch parsed tickets. Returns number of tickets read.
int fossil_ticket_list_parsed(FossilTicket *tickets, int max_tickets);

// Create a new ticket
bool fossil_ticket_create(const char *title, const char *description);

// Assign a ticket to an agent
bool fossil_ticket_assign(const char *ticket_id, const char *agent_name);

// Update ticket status
bool fossil_ticket_set_status(const char *ticket_id, const char *status);

// Update the reviewer_notes field on a ticket (used for plan approval markers)
bool fossil_ticket_set_reviewer_notes(const char *ticket_id, const char *notes);

// Commit changes in a specific workspace
bool fossil_commit(const char *workspace, const char *message);

// Append a timestamped entry to the wiki page for this ticket (ticket-<uuid8>).
// Creates the page on first call.  Failures are silent — wiki logging is best-effort.
bool fossil_wiki_append_log(const char *ticket_id, const char *agent, const char *message);

// Append an orchestrator note to the ticket's changelog field.
// Used to explain WHY the orchestrator made a change (assignment, status transitions, etc.).
// The note is stored in the 'changelog' field; all prior values are preserved in
// `fossil ticket history <uuid>` even though only the latest is visible inline.
bool fossil_ticket_add_note(const char *ticket_id, const char *note);

// Read the accumulated wiki log for a ticket (ticket-<uuid10> page) into buffer.
// Returns the number of bytes read, or 0 if the page doesn't exist yet.
int fossil_ticket_read_wiki_log(const char *ticket_id, char *buffer, size_t max_size);

// Read the icomment field from the ticket creation artifact.
// Fossil web UI stores the initial description as "J icomment" in the artifact
// instead of the ticket table's comment column. Returns bytes written, 0 on failure.
int fossil_ticket_read_icomment_from_artifact(const char *ticket_id, char *buffer, size_t max_size);

// Read the full change history for a ticket via `fossil ticket history`.
// Includes all human remarks (icomment changes) and field changes.
// Returns bytes written, 0 on failure.
int fossil_ticket_read_history(const char *ticket_id, char *buffer, size_t max_size);

// Extract only the icomment (human remarks typed in the Fossil web UI) entries
// from the ticket history.  Much smaller than the full history — always fits in
// the PULSE regardless of how long the history has grown.
// Returns bytes written, 0 on failure.
int fossil_ticket_read_human_remarks(const char *ticket_id, char *buffer, size_t max_size);

#endif // FOSSIL_SKILL_H
