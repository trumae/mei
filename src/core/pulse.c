#include "core/pulse.h"
#include <stdio.h>
#include <string.h>

int pulse_format(const PulseMessage *msg, char *buffer, size_t max_size) {
    if (!msg || !buffer || max_size == 0) return -1;

    // Plain markdown — no [PULSE] envelope.
    // Envelope tags were interpreted as empty metadata by LLM backends (GPT-4.1,
    // Claude), causing "message is empty" responses.  Plain markdown headings
    // are unambiguous instructions to any model.
    int written = snprintf(buffer, max_size,
        "# %s\n\n"
        "%s\n\n"
        "**Current State:** %s\n"
        "**Your next action:** %s\n",
        msg->intent,
        msg->context,
        msg->current_state,
        msg->next_action
    );

    if (written < 0 || (size_t)written >= max_size) {
        return -1;
    }

    return written;
}

int pulse_parse(const char *raw_data, PulseMessage *msg) {
    if (!raw_data || !msg) return 0;

    // Simple parser for the format above
    const char *start = strstr(raw_data, "[PULSE]");
    const char *end = strstr(raw_data, "[/PULSE]");

    if (!start || !end || start >= end) {
        return 0; // Invalid envelope
    }

    // Reset msg
    memset(msg, 0, sizeof(PulseMessage));

    // Simple line by line extraction (not robust against newlines within fields, but serves the prototype)
    const char *intent_ptr = strstr(start, "Intent: ");
    const char *context_ptr = strstr(start, "Context: ");
    const char *state_ptr = strstr(start, "CurrentState: ");
    const char *action_ptr = strstr(start, "NextAction: ");

    if (intent_ptr) sscanf(intent_ptr, "Intent: %[^\n]", msg->intent);
    if (context_ptr) sscanf(context_ptr, "Context: %[^\n]", msg->context);
    if (state_ptr) sscanf(state_ptr, "CurrentState: %[^\n]", msg->current_state);
    if (action_ptr) sscanf(action_ptr, "NextAction: %[^\n]", msg->next_action);

    return 1;
}
