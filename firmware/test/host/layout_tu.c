// A second translation unit that includes <limits.h> FIRST, the way ui.c
// ends up doing through FreeRTOS/LVGL headers on the device. It exists to
// prove the shared structs have ONE layout whatever the include order: the
// original header guarded its own NAME_MAX with #ifndef, <limits.h> defines
// NAME_MAX as 255, and cable_agent_t / cable_question_t silently differed
// between cable_client.c and ui.c.
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#ifndef NAME_MAX
#error "this libc's <limits.h> has no NAME_MAX: the test would prove nothing"
#endif

#include <stddef.h>

#include "cable_client.h"

size_t layout_agent_size(void) { return sizeof(cable_agent_t); }
size_t layout_question_size(void) { return sizeof(cable_question_t); }
size_t layout_notif_size(void) { return sizeof(cable_notif_t); }
