#ifndef FOSSIL_SKILL_H
#define FOSSIL_SKILL_H

#include <stdbool.h>
#include <stddef.h>

// Set the global fossil repository path to use for operations
void fossil_set_repo_path(const char *path);

// Get the current global fossil repository path
const char *fossil_get_repo_path();

// Initialize the fossil repository if not exists (Mock or Real)
bool fossil_init(const char *repo_path);

// Fetch the list of tickets. In a real scenario, this parses fossil ticket show.
// Writes the raw string into buffer.
int fossil_ticket_list(char *buffer, size_t max_size);

// Create a new ticket
bool fossil_ticket_create(const char *title, const char *description);

// Assign a ticket to an agent
bool fossil_ticket_assign(const char *ticket_id, const char *agent_name);

// Update ticket status
bool fossil_ticket_set_status(const char *ticket_id, const char *status);

// Commit changes in a specific workspace
bool fossil_commit(const char *workspace, const char *message);

#endif // FOSSIL_SKILL_H
