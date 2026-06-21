/**
 * test_uxplaylib.c — Minimal C test program for the UxPlay library (DLL).
 *
 * Build (MSYS2 / MinGW-w64):
 *   gcc -o test_uxplaylib test_uxplaylib.c -L../build -luxplaylib -I../include
 *
 * Run:
 *   PATH="../build:$PATH" ./test_uxplaylib
 *
 * Usage:
 *   Starts an AirPlay server named "UxPlayLib Test" and waits for
 *   connections. Press Enter to stop the server and exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uxplaylib.h"

/* ---- Callbacks ---- */

static void on_event(void *user_data, const uxplay_event_data_t *event) {
    switch (event->type) {
    case UXPLAY_EVENT_STATE_CHANGED:
        printf("[EVENT] State changed to: %d\n", event->state);
        break;
    case UXPLAY_EVENT_CLIENT_CONNECTED:
        if (event->client.device_name) {
            printf("[EVENT] Client connected: %s (%s) ID=%s\n",
                   event->client.device_name,
                   event->client.device_model ? event->client.device_model : "?",
                   event->client.device_id ? event->client.device_id : "?");
        } else {
            printf("[EVENT] Client connected\n");
        }
        break;
    case UXPLAY_EVENT_CLIENT_DISCONNECTED:
        printf("[EVENT] Client disconnected\n");
        break;
    case UXPLAY_EVENT_DISPLAY_PIN:
        printf("[EVENT] *** PIN CODE: %s ***\n", event->pin);
        break;
    case UXPLAY_EVENT_MIRROR_STARTED:
        printf("[EVENT] Screen mirroring started\n");
        break;
    case UXPLAY_EVENT_MIRROR_STOPPED:
        printf("[EVENT] Screen mirroring stopped\n");
        break;
    case UXPLAY_EVENT_AUDIO_STARTED:
        printf("[EVENT] Audio streaming started\n");
        break;
    case UXPLAY_EVENT_AUDIO_STOPPED:
        printf("[EVENT] Audio streaming stopped\n");
        break;
    case UXPLAY_EVENT_AUDIO_METADATA:
        printf("[EVENT] Now Playing: %s - %s (%s)\n",
               event->audio_meta.artist ? event->audio_meta.artist : "?",
               event->audio_meta.title  ? event->audio_meta.title  : "?",
               event->audio_meta.album  ? event->audio_meta.album  : "?");
        break;
    case UXPLAY_EVENT_VIDEO_SIZE_CHANGED:
        printf("[EVENT] Video size: %.0fx%.0f (source: %.0fx%.0f)\n",
               event->video_size.width, event->video_size.height,
               event->video_size.width_source, event->video_size.height_source);
        break;
    case UXPLAY_EVENT_ERROR:
        printf("[EVENT] Error: %s\n", event->error_msg ? event->error_msg : "unknown");
        break;
    default:
        printf("[EVENT] Unknown event: %d\n", event->type);
        break;
    }
}

static void on_log(void *user_data, uxplay_log_level_t level, const char *message) {
    const char *prefix;
    switch (level) {
    case UXPLAY_LOG_ERROR:   prefix = "ERROR";   break;
    case UXPLAY_LOG_WARNING: prefix = "WARN ";   break;
    case UXPLAY_LOG_INFO:    prefix = "INFO ";   break;
    case UXPLAY_LOG_DEBUG:   prefix = "DEBUG";   break;
    default:                 prefix = "     ";   break;
    }
    printf("[%s] %s\n", prefix, message);
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    uxplay_t server = NULL;
    uxplay_error_t err;

    printf("UxPlayLib Test - Version: %s\n", uxplay_version());
    printf("===========================================\n\n");

    /* 1. Create instance */
    err = uxplay_create(&server);
    if (err != UXPLAY_OK) {
        printf("Failed to create server: %s\n", uxplay_error_string(err));
        return 1;
    }

    /* 2. Configure */
    uxplay_config_t cfg = uxplay_default_config();
    cfg.server_name   = "UxPlayLib Test";
    cfg.use_video     = true;
    cfg.use_audio     = true;
    cfg.nohold        = true;
    cfg.log_level     = UXPLAY_LOG_INFO;

    /* Use d3d11videosink on Windows for best performance */
    /* cfg.videosink = "d3d11videosink"; */

    err = uxplay_configure(server, &cfg);
    if (err != UXPLAY_OK) {
        printf("Failed to configure server: %s\n", uxplay_error_string(err));
        uxplay_destroy(server);
        return 1;
    }

    /* 3. Set callbacks */
    uxplay_set_event_callback(server, on_event, NULL);
    uxplay_set_log_callback(server, on_log, NULL);

    /* 4. Start server */
    printf("Starting AirPlay server...\n");
    err = uxplay_start(server);
    if (err != UXPLAY_OK) {
        printf("Failed to start server: %s\n", uxplay_error_string(err));
        uxplay_destroy(server);
        return 1;
    }

    printf("\n*** Server is running! ***\n");
    printf("*** Look for \"%s\" on your iOS/macOS device ***\n", cfg.server_name);
    printf("*** Press Enter to stop the server... ***\n\n");

    /* 5. Wait for user input */
    getchar();

    /* 6. Stop & cleanup */
    printf("Stopping server...\n");
    uxplay_stop(server);
    uxplay_destroy(server);

    printf("Done.\n");
    return 0;
}
