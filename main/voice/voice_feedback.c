#include "voice_feedback.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE     feedback_lock = portMUX_INITIALIZER_UNLOCKED;
static voice_feedback_t current_feedback;
static void (*feedback_observer)(void);

void
voice_feedback_set_observer(void (*observer)(void))
{
    portENTER_CRITICAL(&feedback_lock);
    feedback_observer = observer;
    portEXIT_CRITICAL(&feedback_lock);
}

bool
voice_feedback_publish(voice_state_t state, const char *activation_code)
{
    if (state < VOICE_STATE_DISABLED || state > VOICE_STATE_ERROR) {
        return false;
    }
    voice_feedback_t next = {.state = state};
    if (state == VOICE_STATE_ACTIVATION) {
        if (activation_code == NULL) {
            return false;
        }
        size_t length = strnlen(activation_code, sizeof(next.activation_code));
        if (length == 0 || length > VOICE_ACTIVATION_CODE_MAX) {
            return false;
        }
        for (size_t i = 0; i < length; i++) {
            if ((unsigned char)activation_code[i] <= 0x20 || (unsigned char)activation_code[i] > 0x7e) {
                return false;
            }
        }
        memcpy(next.activation_code, activation_code, length);
    } else if (activation_code != NULL) {
        return false;
    }
    void (*observer)(void) = NULL;
    bool changed = false;
    portENTER_CRITICAL(&feedback_lock);
    if (current_feedback.state != VOICE_STATE_ACTIVATION
        || (state != VOICE_STATE_CONNECTING && state != VOICE_STATE_ERROR && state != VOICE_STATE_OFFLINE))
    {
        changed =
            current_feedback.state != next.state || strcmp(current_feedback.activation_code, next.activation_code) != 0;
        current_feedback = next;
        observer = feedback_observer;
    }
    portEXIT_CRITICAL(&feedback_lock);
    if (changed && observer != NULL) {
        observer();
    }
    return true;
}

voice_feedback_t
voice_feedback_get(void)
{
    portENTER_CRITICAL(&feedback_lock);
    voice_feedback_t copy = current_feedback;
    portEXIT_CRITICAL(&feedback_lock);
    return copy;
}
