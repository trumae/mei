#ifndef PULSE_H
#define PULSE_H

#include <stddef.h>

// A structure representing the PULSE envelope
typedef struct {
    char intent[128];
    char context[256];
    char current_state[256];
    char next_action[256];
} PulseMessage;

// Formats a PulseMessage into a string buffer ready to be sent to an agent.
// Returns the length of the formatted string or -1 on error.
int pulse_format(const PulseMessage *msg, char *buffer, size_t max_size);

// Parses a raw string into a PulseMessage structure.
// Returns 1 on success, 0 on failure.
int pulse_parse(const char *raw_data, PulseMessage *msg);

#endif // PULSE_H
