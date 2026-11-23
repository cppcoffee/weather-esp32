#ifndef __VOICE_FEEDBACK_H__
#define __VOICE_FEEDBACK_H__

#include <stdbool.h>

#define VOICE_ACTIVATION_CODE_MAX 16

typedef enum {
    VOICE_STATE_DISABLED,
    VOICE_STATE_WAIT_NETWORK,
    VOICE_STATE_OFFLINE,
    VOICE_STATE_CONNECTING,
    VOICE_STATE_NEED_SETUP,
    VOICE_STATE_ACTIVATION,
    VOICE_STATE_READY,
    VOICE_STATE_LISTENING,
    VOICE_STATE_FOLLOWUP,
    VOICE_STATE_WAITING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_ERROR,
} voice_state_t;

typedef struct {
    voice_state_t state;
    char          activation_code[VOICE_ACTIVATION_CODE_MAX + 1];
} voice_feedback_t;

// Copies the code before returning. Activation requires 1..16 printable ASCII
// characters without spaces; other states require NULL. Invalid input changes
// nothing. A pending code survives connection retries/outages/errors until
// another state replaces it.
bool             voice_feedback_publish(voice_state_t state, const char *activation_code);
voice_feedback_t voice_feedback_get(void);
// Optional callback invoked outside the critical section when the published
// snapshot changes.
void voice_feedback_set_observer(void (*observer)(void));

#endif
