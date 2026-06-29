/**
 * @file uxplaylib.h
 * @brief UxPlayLib - AirPlay Mirroring & Audio Server Library
 * @version 1.0.0
 *
 * Pure C API for embedding an AirPlay receiver into any application.
 * Designed for DLL export on Windows (MinGW-w64) and easy P/Invoke
 * integration with C# / WinUI 3.
 *
 * Typical usage:
 * @code
 *   uxplay_t srv;
 *   uxplay_create(&srv);
 *   uxplay_config_t cfg = uxplay_default_config();
 *   cfg.server_name = "Living Room";
 *   uxplay_configure(srv, &cfg);
 *   uxplay_set_event_callback(srv, my_handler, NULL);
 *   uxplay_start(srv);          // non-blocking
 *   // ... GUI event loop ...
 *   uxplay_stop(srv);
 *   uxplay_destroy(srv);
 * @endcode
 *
 * @copyright GPLv3 - based on UxPlay by FDH2 / antimof
 */

#ifndef UXPLAYLIB_H
#define UXPLAYLIB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * DLL Export / Import
 * =================================================================*/

#ifdef _WIN32
  #ifdef UXPLAY_BUILDING_DLL
    #define UXPLAY_API __declspec(dllexport)
  #else
    #define UXPLAY_API __declspec(dllimport)
  #endif
#else
  #define UXPLAY_API __attribute__((visibility("default")))
#endif

/* ===================================================================
 * Version
 * =================================================================*/

#define UXPLAYLIB_VERSION_MAJOR  1
#define UXPLAYLIB_VERSION_MINOR  0
#define UXPLAYLIB_VERSION_PATCH  0
#define UXPLAYLIB_VERSION_STRING "1.0.0"

/* ===================================================================
 * Opaque Handle
 * =================================================================*/

/** Opaque server instance handle. */
typedef struct uxplay_instance_s *uxplay_t;

/* ===================================================================
 * Enumerations
 * =================================================================*/

/** Server lifecycle state. */
typedef enum uxplay_state_e {
    UXPLAY_STATE_IDLE     = 0, /**< Created but not started.           */
    UXPLAY_STATE_STARTING = 1, /**< Initialization in progress.        */
    UXPLAY_STATE_RUNNING  = 2, /**< Accepting AirPlay connections.     */
    UXPLAY_STATE_STOPPING = 3, /**< Graceful shutdown in progress.     */
    UXPLAY_STATE_ERROR    = 4  /**< Unrecoverable error; must destroy. */
} uxplay_state_t;

/** Video flip / rotation applied before rendering. */
typedef enum uxplay_videoflip_e {
    UXPLAY_FLIP_NONE   = 0,
    UXPLAY_FLIP_LEFT   = 1,  /**< 90 deg counter-clockwise */
    UXPLAY_FLIP_RIGHT  = 2,  /**< 90 deg clockwise         */
    UXPLAY_FLIP_INVERT = 3,  /**< 180 deg                  */
    UXPLAY_FLIP_VFLIP  = 4,  /**< Mirror vertically        */
    UXPLAY_FLIP_HFLIP  = 5   /**< Mirror horizontally      */
} uxplay_videoflip_t;

/** Log severity. */
typedef enum uxplay_log_level_e {
    UXPLAY_LOG_ERROR   = 3,
    UXPLAY_LOG_WARNING = 4,
    UXPLAY_LOG_INFO    = 5,
    UXPLAY_LOG_DEBUG   = 6,
    UXPLAY_LOG_VERBOSE = 7
} uxplay_log_level_t;

/** Client access-control mode. */
typedef enum uxplay_access_control_e {
    UXPLAY_ACCESS_FREE     = 0, /**< No authentication.          */
    UXPLAY_ACCESS_PIN      = 1, /**< One-time on-screen PIN.     */
    UXPLAY_ACCESS_PASSWORD = 2  /**< Persistent password.        */
} uxplay_access_control_t;

/** Event types delivered through the event callback. */
typedef enum uxplay_event_type_e {
    UXPLAY_EVENT_STATE_CHANGED       =  0,
    UXPLAY_EVENT_CLIENT_CONNECTED    =  1,
    UXPLAY_EVENT_CLIENT_DISCONNECTED =  2,
    UXPLAY_EVENT_DISPLAY_PIN         =  3,
    UXPLAY_EVENT_MIRROR_STARTED      =  4,
    UXPLAY_EVENT_MIRROR_STOPPED      =  5,
    UXPLAY_EVENT_AUDIO_STARTED       =  6,
    UXPLAY_EVENT_AUDIO_STOPPED       =  7,
    UXPLAY_EVENT_AUDIO_METADATA      =  8,
    UXPLAY_EVENT_VIDEO_SIZE_CHANGED  =  9,
    UXPLAY_EVENT_ERROR               = 10
} uxplay_event_type_t;

/** Error codes returned by API functions. */
typedef enum uxplay_error_e {
    UXPLAY_OK                    =  0,
    UXPLAY_ERR_INVALID_ARGUMENT  = -1,
    UXPLAY_ERR_ALREADY_RUNNING   = -2,
    UXPLAY_ERR_NOT_RUNNING       = -3,
    UXPLAY_ERR_GSTREAMER_INIT    = -4,
    UXPLAY_ERR_RAOP_INIT         = -5,
    UXPLAY_ERR_DNSSD_INIT        = -6,
    UXPLAY_ERR_NETWORK           = -7,
    UXPLAY_ERR_OUT_OF_MEMORY     = -8,
    UXPLAY_ERR_INTERNAL          = -9
} uxplay_error_t;

/* ===================================================================
 * Configuration
 * =================================================================*/

/**
 * Server configuration.
 *
 * Obtain with uxplay_default_config(), change the fields you care about,
 * then pass to uxplay_configure().  String pointers are copied internally
 * so the caller may free them after configure() returns.
 */
typedef struct uxplay_config_s {
    /* ---- Identity ---- */
    const char *server_name;     /**< Bonjour display name (default "UxPlay").     */
    const char *mac_address;     /**< "AA:BB:CC:DD:EE:FF" or NULL for auto-detect. */
    bool        append_hostname; /**< Append @hostname to server_name.             */

    /* ---- Display ---- */
    uint16_t width;              /**< Pixels wide  (0 = 1920).                     */
    uint16_t height;             /**< Pixels high  (0 = 1080).                     */
    uint16_t refresh_rate;       /**< Hz            (0 = 60).                      */
    uint16_t max_fps;            /**< Client FPS cap(0 = 30).                      */

    /* ---- Video rendering ---- */
    const char *videosink;       /**< GStreamer sink  (NULL = platform default).    */
    const char *video_decoder;   /**< GStreamer decoder  (NULL = "decodebin").      */
    const char *video_converter; /**< GStreamer converter(NULL = "videoconvert").   */
    const char *video_parser;    /**< GStreamer parser   (NULL = "h264parse").      */
    uxplay_videoflip_t videoflip;
    bool fullscreen;
    bool h265_support;           /**< Accept 4K H.265 streams.                     */
    bool video_sync;             /**< Timestamp-based A/V sync (default true).     */
    bool bt709_fix;              /**< Apply BT.709 colorimetry fix.                */
    bool use_video;              /**< Enable video rendering (default true).       */
    bool nofreeze;               /**< Don't freeze last frame on disconnect.       */

    /* ---- Audio rendering ---- */
    const char *audiosink;       /**< GStreamer sink (NULL = "autoaudiosink").      */
    bool  audio_sync;            /**< Sync audio-only playback (~2 s latency).     */
    bool  use_audio;             /**< Enable audio rendering (default true).       */
    double initial_volume;       /**< Client initial volume 0.0 = max (0 dB).     */
    double db_low;               /**< Low  dB bound (default -30.0).               */
    double db_high;              /**< High dB bound (default   0.0).               */

    /* ---- Network ---- */
    uint16_t tcp_ports[3];       /**< Fixed TCP ports (0 = auto).                  */
    uint16_t udp_ports[3];       /**< Fixed UDP ports (0 = auto).                  */

    /* ---- Security ---- */
    uxplay_access_control_t access_control;
    const char *password;
    const char *keyfile;
    bool  registration_list;

    /* ---- Miscellaneous ---- */
    uxplay_log_level_t log_level;
    bool  coverart_display;
    const char *coverart_filename;
    bool  hls_support;
    const char *lang;            /**< HLS language prefs, e.g. "en:fr:es".         */
    bool  nohold;                /**< Let new client kick existing one.             */
    bool  taper_volume;          /**< Use tapered volume curve (default false).     */
    bool  srgb_fix;              /**< Full-range [0-255] color (default true).      */
    double audio_latency;        /**< Audio latency in sec (0 = auto 0.25).         */
    int   reset_timeout;         /**< Silent reset after N sec (0 = never).         */
    bool  keep_window;           /**< Keep video window on disconnect.              */
    bool  force_software_decoder;/**< Force software h264 decoding.                 */
    const char *metadata_filename; /**< Write metadata text to this file.           */
    const char *record_filename; /**< Record media to MP4 file (NULL = no).        */
    bool  overscanned;           /**< Display overscan mode (-o).                   */
    bool  restrict_clients;      /**< Restrict to allowed clients (-restrict).      */
    bool  show_fps_data;         /**< Show FPS performance data in log (-FPSdata).  */
} uxplay_config_t;

/* ===================================================================
 * Event Payloads
 * =================================================================*/

typedef struct uxplay_client_info_s {
    const char *device_id;
    const char *device_model;
    const char *device_name;
} uxplay_client_info_t;

typedef struct uxplay_audio_meta_s {
    const char *artist;
    const char *title;
    const char *album;
} uxplay_audio_meta_t;

typedef struct uxplay_video_size_s {
    float width_source;   /**< Original source width.   */
    float height_source;  /**< Original source height.  */
    float width;          /**< Rendered width.           */
    float height;         /**< Rendered height.          */
} uxplay_video_size_t;

/**
 * Event data delivered through the event callback.
 *
 * Use the `type` field to determine which union member is valid:
 *   - STATE_CHANGED       → state
 *   - CLIENT_CONNECTED    → client
 *   - CLIENT_DISCONNECTED → (no payload)
 *   - DISPLAY_PIN         → pin
 *   - MIRROR_STARTED      → (no payload)
 *   - MIRROR_STOPPED      → (no payload)
 *   - AUDIO_STARTED       → (no payload)
 *   - AUDIO_STOPPED       → (no payload)
 *   - AUDIO_METADATA      → audio_meta
 *   - VIDEO_SIZE_CHANGED  → video_size
 *   - ERROR               → error_msg
 */
typedef struct uxplay_event_data_s {
    uxplay_event_type_t type;
    union {
        uxplay_state_t       state;
        uxplay_client_info_t client;
        const char          *pin;
        uxplay_audio_meta_t  audio_meta;
        uxplay_video_size_t  video_size;
        const char          *error_msg;
    };
} uxplay_event_data_t;

/* ===================================================================
 * Callback Types
 * =================================================================*/

/**
 * Event callback — called from internal threads when a server event
 * occurs.  Keep the handler fast and thread-safe.
 *
 * @param user_data  Opaque pointer passed to uxplay_set_event_callback().
 * @param event      Event data; valid only for the duration of the call.
 */
typedef void (*uxplay_event_callback_t)(void *user_data,
                                        const uxplay_event_data_t *event);

/**
 * Log callback — receives formatted log messages from the server.
 *
 * @param user_data  Opaque pointer passed to uxplay_set_log_callback().
 * @param level      Severity level of the message.
 * @param message    Null-terminated log string; valid only for the duration
 *                   of the call.
 */
typedef void (*uxplay_log_callback_t)(void *user_data,
                                      uxplay_log_level_t level,
                                      const char *message);

/* ===================================================================
 * Lifecycle API
 * =================================================================*/

/**
 * Return a configuration struct pre-filled with sensible defaults.
 * Modify the returned struct and pass it to uxplay_configure().
 */
UXPLAY_API uxplay_config_t uxplay_default_config(void);

/**
 * Return the library version string (e.g. "1.0.0").
 */
UXPLAY_API const char* uxplay_version(void);

/**
 * Return a human-readable string for the given error code.
 */
UXPLAY_API const char* uxplay_error_string(uxplay_error_t error);

/**
 * Create a new server instance.
 *
 * @param[out] out_handle  Receives the opaque handle on success.
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_create(uxplay_t *out_handle);

/**
 * Apply configuration to a server instance.
 * Must be called before uxplay_start(). String pointers in the config
 * struct are copied internally — the caller may free them after this
 * call returns.
 *
 * @param handle  Server instance.
 * @param config  Configuration to apply.
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_configure(uxplay_t handle,
                                           const uxplay_config_t *config);

/**
 * Register an event callback.  Only one callback per instance;
 * passing NULL removes the current one.
 *
 * @param handle     Server instance.
 * @param callback   Event handler function, or NULL.
 * @param user_data  Opaque pointer forwarded to the callback.
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_set_event_callback(uxplay_t handle,
                                                     uxplay_event_callback_t callback,
                                                     void *user_data);

/**
 * Register a log callback.  Only one callback per instance;
 * passing NULL removes the current one.
 *
 * @param handle     Server instance.
 * @param callback   Log handler function, or NULL.
 * @param user_data  Opaque pointer forwarded to the callback.
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_set_log_callback(uxplay_t handle,
                                                    uxplay_log_callback_t callback,
                                                    void *user_data);

/**
 * Start the AirPlay server.  This call is non-blocking — the server
 * runs on a background thread.  Use the event callback or
 * uxplay_get_state() to monitor progress.
 *
 * @param handle  Server instance (must be configured first).
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_start(uxplay_t handle);

/**
 * Stop the server gracefully.  Blocks until the background thread
 * has finished.
 *
 * @param handle  Server instance.
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_stop(uxplay_t handle);

/**
 * Destroy a server instance and free all associated resources.
 * If the server is still running, it will be stopped first.
 *
 * @param handle  Server instance (may be NULL).
 */
UXPLAY_API void uxplay_destroy(uxplay_t handle);

/* ===================================================================
 * Runtime API
 * =================================================================*/

/**
 * Query the current server state.
 *
 * @param handle  Server instance.
 * @return Current state, or UXPLAY_STATE_ERROR if the handle is NULL.
 */
UXPLAY_API uxplay_state_t uxplay_get_state(uxplay_t handle);

/**
 * Set the playback volume on the currently connected client.
 *
 * @param handle  Server instance.
 * @param volume  Volume in dB (0.0 = max, negative = quieter).
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_set_volume(uxplay_t handle, double volume);

/**
 * Return the number of currently connected AirPlay clients.
 *
 * @param handle  Server instance.
 * @return Connection count, or 0 if the handle is NULL.
 */
UXPLAY_API int uxplay_get_connection_count(uxplay_t handle);

/**
 * Disconnect all currently connected clients.
 *
 * @param handle  Server instance.
 * @return UXPLAY_OK or an error code.
 */
UXPLAY_API uxplay_error_t uxplay_disconnect_clients(uxplay_t handle);

#ifdef __cplusplus
}
#endif

#endif /* UXPLAYLIB_H */
