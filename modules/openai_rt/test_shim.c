// test_shim.c: minimal globals / stubs for the test harness
#include "openai_rt.h"

/* Define the global instance so websocket.c, utils.c, etc. can link */
struct openai_rt g_oairt;

/* We’re not driving the audio event pump in the test; return nothing */
struct audio_event *audio_get_next_event(void) {
    return NULL;
}