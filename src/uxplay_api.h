// uxplay_api.h — internal interface between uxplaylib wrapper and uxplay core
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Core entry points (from upstream UxPlay, lightly modified) ---- */
int  start_uxplay(int argc, char *argv[]);
void stop_uxplay(void);

/* ==================================================================
 * Extensions beyond leapbtw/libuxplay — callback & runtime hooks
 * ================================================================== */

/* Event types fired from inside uxplay_core.cpp */
#define UXPLAY_CORE_EVT_CLIENT_CONNECTED    1
#define UXPLAY_CORE_EVT_CLIENT_DISCONNECTED 2
#define UXPLAY_CORE_EVT_DISPLAY_PIN         3
#define UXPLAY_CORE_EVT_MIRROR_STARTED      4
#define UXPLAY_CORE_EVT_MIRROR_STOPPED      5
#define UXPLAY_CORE_EVT_AUDIO_STARTED       6
#define UXPLAY_CORE_EVT_VIDEO_SIZE          7
#define UXPLAY_CORE_EVT_AUDIO_META          8
#define UXPLAY_CORE_EVT_ERROR               9

/* Lightweight event payload passed to the event callback. */
typedef struct uxplay_core_event_s {
    int type;
    union {
        struct { const char *device_id; const char *model; const char *name; } client;
        const char *pin;
        const char *error_msg;
        struct { float ws; float hs; float w; float h; } video_size;
        struct { const char *artist; const char *title; const char *album; } meta;
    };
} uxplay_core_event_t;

/** Event callback — called from core threads. */
typedef void (*uxplay_core_event_fn)(const uxplay_core_event_t *evt, void *user_data);

/** Log callback — replaces printf-based log output. */
typedef void (*uxplay_core_log_fn)(int level, const char *msg, void *user_data);

/**
 * Install callbacks BEFORE calling start_uxplay().
 * Pass NULL to disable a callback. Thread-safe only before start.
 */
void uxplay_core_set_callbacks(uxplay_core_event_fn event_cb,
                               uxplay_core_log_fn   log_cb,
                               void                *user_data);

/**
 * Reset all global state inside uxplay_core.cpp to defaults.
 * Call AFTER start_uxplay() returns, before a subsequent start.
 */
void uxplay_core_reset_state(void);

/** Return the current open-connection count (thread-safe read). */
unsigned int uxplay_core_get_connections(void);

/** Return the internal raop_t* handle (or NULL if not started). */
void *uxplay_core_get_raop(void);

#ifdef __cplusplus
}
#endif
