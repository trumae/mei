#ifndef FOSSIL_SKILL_H
#define FOSSIL_SKILL_H

#include <stdbool.h>
#include <stddef.h>

// Set the global fossil repository path to use for operations
void fossil_set_repo_path(const char *path);

// Get the current global fossil repository path
const char *fossil_get_repo_path();

typedef struct {
    char tkt_uuid[64];
    char title[128];
    char status[64];
    char assignee[64]; // Mapped to private_contact in Fossil
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

// Commit changes in a specific workspace
bool fossil_commit(const char *workspace, const char *message);

#endif // FOSSIL_SKILL_H
