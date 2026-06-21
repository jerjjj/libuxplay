# libuxplay

An AirPlay mirroring and audio receiver library with a pure C API, built on top of [UxPlay](https://github.com/FDH2/UxPlay). Designed for embedding into desktop applications on Windows (MSYS2 / MinGW-w64), with first-class support for C# P/Invoke and WinUI 3 integration.

The library wraps the full upstream UxPlay server logic (protocol handling, GStreamer pipelines, DNS-SD registration) behind a thin C facade. Configuration is done through a struct instead of command-line arguments, and the server runs on a background thread so your GUI stays responsive.

## Features

- **Pure C API** -- 14 exported functions, opaque handle design, no C++ symbols leaked
- **Non-blocking** -- `uxplay_start()` runs the server on a background thread
- **Event-driven** -- a single callback delivers client connect/disconnect, mirroring state, audio metadata, video size changes, PIN display, and errors
- **Log routing** -- all internal log output can be captured through a callback instead of going to stdout
- **Restartable** -- global state is fully reset after `uxplay_stop()` returns; you can call `uxplay_start()` again without restarting the process
- **Configurable** -- resolution, FPS cap, GStreamer pipeline elements, access control (open / PIN / password), volume range, and more
- **DLL-ready** -- `__declspec(dllexport)` on Windows, `visibility("default")` on Linux/macOS
- **C# bindings included** -- ready-to-use P/Invoke wrapper for .NET / WinUI 3 projects

## Prerequisites

The library is developed and tested on **MSYS2 UCRT64** (Windows). Install the required packages:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-libplist \
  mingw-w64-ucrt-x86_64-glib2 \
  mingw-w64-ucrt-x86_64-gstreamer \
  mingw-w64-ucrt-x86_64-gst-plugins-base \
  mingw-w64-ucrt-x86_64-gst-plugins-good \
  mingw-w64-ucrt-x86_64-gst-plugins-bad
```

You also need the **Bonjour SDK** (Apple) for DNS-SD service discovery on Windows. Set the environment variable `BONJOUR_SDK_HOME` if it is not installed at the default path `C:\Program Files\Bonjour SDK`.

### GStreamer video sinks (Windows)

| Sink | Notes |
|------|-------|
| `d3d11videosink` | Recommended. Hardware-accelerated, low latency. |
| `glimagesink` | OpenGL-based fallback. |
| `autovideosink` | Auto-selection (default). |

For the best experience on Windows, set `cfg.videosink = "d3d11videosink"` in your configuration.

## Building

```bash
mkdir build && cd build
cmake .. -G Ninja
cmake --build .
```

Build outputs:

| File | Description |
|------|-------------|
| `libuxplaylib.dll` | Shared library |
| `libuxplaylib.dll.a` | Import library (for linking at compile time) |
| `test_uxplaylib.exe` | Example program |

To build a static library instead:

```bash
cmake .. -G Ninja -DBUILD_SHARED_LIBS=OFF
```

## Quick Start (C)

The full lifecycle of a server takes six steps: **create -- configure -- set callbacks -- start -- (run) -- stop -- destroy**.

```c
#include "uxplaylib.h"
#include <stdio.h>

/* Event callback (called from a background thread) */
static void on_event(void *ud, const uxplay_event_data_t *ev) {
    switch (ev->type) {
    case UXPLAY_EVENT_CLIENT_CONNECTED:
        printf("Client: %s (%s)\n",
               ev->client.device_name,
               ev->client.device_model);
        break;
    case UXPLAY_EVENT_MIRROR_STARTED:
        printf("Screen mirroring started\n");
        break;
    case UXPLAY_EVENT_AUDIO_METADATA:
        printf("Now playing: %s - %s\n",
               ev->audio_meta.artist,
               ev->audio_meta.title);
        break;
    default:
        break;
    }
}

/* Log callback (optional -- captures all internal log output) */
static void on_log(void *ud, uxplay_log_level_t level, const char *msg) {
    const char *tag;
    switch (level) {
    case UXPLAY_LOG_ERROR:   tag = "ERR"; break;
    case UXPLAY_LOG_WARNING: tag = "WRN"; break;
    case UXPLAY_LOG_INFO:    tag = "INF"; break;
    case UXPLAY_LOG_DEBUG:   tag = "DBG"; break;
    default:                 tag = "   "; break;
    }
    printf("[%s] %s\n", tag, msg);
}

int main(void) {
    uxplay_t server;
    uxplay_error_t err;

    /* 1. Create */
    err = uxplay_create(&server);
    if (err != UXPLAY_OK) {
        fprintf(stderr, "create failed: %s\n", uxplay_error_string(err));
        return 1;
    }

    /* 2. Configure */
    uxplay_config_t cfg = uxplay_default_config();
    cfg.server_name = "Living Room";
    cfg.use_video   = true;
    cfg.use_audio   = true;
    cfg.log_level   = UXPLAY_LOG_INFO;

    err = uxplay_configure(server, &cfg);
    if (err != UXPLAY_OK) {
        uxplay_destroy(server);
        return 1;
    }

    /* 3. Set callbacks */
    uxplay_set_event_callback(server, on_event, NULL);
    uxplay_set_log_callback(server, on_log, NULL);

    /* 4. Start (non-blocking) */
    err = uxplay_start(server);
    if (err != UXPLAY_OK) {
        uxplay_destroy(server);
        return 1;
    }

    /* 5. Your application loop -- press Enter to quit */
    printf("Server running. Press Enter to stop.\n");
    getchar();

    /* 6. Stop and destroy */
    uxplay_stop(server);
    uxplay_destroy(server);
    return 0;
}
```

Build and run:

```bash
gcc -o myserver myserver.c -I/path/to/include -L/path/to/build -luxplaylib
PATH="/path/to/build:$PATH" ./myserver
```

Then open an iPhone or Mac on the same network, tap Screen Mirroring or AirPlay, and select "Living Room".

The server can be stopped and restarted without recreating the instance. After `uxplay_stop()` returns, call `uxplay_start()` again to begin accepting connections with the same (or new) configuration.

## API Reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `uxplay_create(uxplay_t *out)` | Allocate a new server instance. |
| `uxplay_configure(handle, &cfg)` | Apply a configuration struct. Call before `start`. String pointers are copied internally. |
| `uxplay_set_event_callback(handle, cb, ud)` | Register an event callback (one per instance; pass `NULL` to remove). |
| `uxplay_set_log_callback(handle, cb, ud)` | Register a log callback. When set, all internal log output is routed through it instead of going to stdout. |
| `uxplay_start(handle)` | Start the server on a background thread (non-blocking). |
| `uxplay_stop(handle)` | Stop the server. Blocks until the background thread finishes. The instance can be started again after this call returns. |
| `uxplay_destroy(handle)` | Free all resources. Stops the server first if it is still running. |

### Runtime Control

| Function | Description |
|----------|-------------|
| `uxplay_get_state(handle)` | Returns the current `uxplay_state_t` (IDLE, STARTING, RUNNING, STOPPING, ERROR). |
| `uxplay_set_volume(handle, dB)` | Reserved. AirPlay volume is controlled by the client device; the server receives volume changes through internal callbacks. |
| `uxplay_get_connection_count(handle)` | Number of currently connected AirPlay clients. |
| `uxplay_disconnect_clients(handle)` | Force-disconnect all clients. |

### Utilities

| Function | Description |
|----------|-------------|
| `uxplay_default_config()` | Returns a `uxplay_config_t` pre-filled with sensible defaults. |
| `uxplay_version()` | Returns the library version string. |
| `uxplay_error_string(err)` | Returns a human-readable description for an error code. |

All functions that can fail return `uxplay_error_t`. Check against `UXPLAY_OK` (0); use `uxplay_error_string()` to get a message suitable for logging.

## Configuration

Call `uxplay_default_config()` to get a struct with defaults, then override only the fields you need:

```c
uxplay_config_t cfg = uxplay_default_config();
```

### Identity

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `server_name` | `const char*` | `"UxPlay"` | Name shown on iOS/macOS AirPlay device list. |
| `mac_address` | `const char*` | `NULL` (auto) | Fixed MAC address in `"AA:BB:CC:DD:EE:FF"` format. `NULL` auto-detects from the first network adapter. |
| `append_hostname` | `bool` | `true` | Append `@hostname` to the server name. |

### Display

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `width` | `uint16_t` | `0` (1920) | Advertised display width in pixels. |
| `height` | `uint16_t` | `0` (1080) | Advertised display height in pixels. |
| `refresh_rate` | `uint16_t` | `0` (60) | Display refresh rate in Hz. |
| `max_fps` | `uint16_t` | `0` (30) | FPS cap sent to the client. |

### Video Rendering

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `videosink` | `const char*` | `NULL` (auto) | GStreamer video sink element name. |
| `video_decoder` | `const char*` | `NULL` | GStreamer decoder (default: `"decodebin"`). |
| `video_converter` | `const char*` | `NULL` | GStreamer converter (default: `"videoconvert"`). |
| `video_parser` | `const char*` | `NULL` | GStreamer parser (default: `"h264parse"`). |
| `videoflip` | `uxplay_videoflip_t` | `NONE` | Rotation or mirror transform. |
| `fullscreen` | `bool` | `false` | Launch the video window in fullscreen. |
| `h265_support` | `bool` | `false` | Accept H.265 / HEVC streams (enables 4K). |
| `video_sync` | `bool` | `true` | Timestamp-based A/V synchronization. |
| `use_video` | `bool` | `true` | Enable video rendering. Set `false` for audio-only mode. |

### Audio Rendering

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `audiosink` | `const char*` | `NULL` (auto) | GStreamer audio sink element name. |
| `audio_sync` | `bool` | `false` | Synchronize audio-only playback (adds ~2 s latency). |
| `use_audio` | `bool` | `true` | Enable audio rendering. |
| `initial_volume` | `double` | `0.0` | Initial volume in dB. `0.0` = maximum (0 dB). |
| `db_low` | `double` | `-30.0` | Lowest volume in dB. |
| `db_high` | `double` | `0.0` | Highest volume in dB. |

### Network

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `tcp_ports[3]` | `uint16_t[3]` | `{0,0,0}` | Fixed TCP ports for the three internal servers. `0` = OS-assigned. |
| `udp_ports[3]` | `uint16_t[3]` | `{0,0,0}` | Fixed UDP ports. `0` = OS-assigned. |

### Security

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `access_control` | `uxplay_access_control_t` | `FREE` | `FREE` = open, `PIN` = one-time on-screen PIN, `PASSWORD` = persistent password. |
| `password` | `const char*` | `NULL` | Password string (used when `access_control == PASSWORD`). |
| `keyfile` | `const char*` | `NULL` | Path to a persistent key file for pairing. |

### Miscellaneous

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `log_level` | `uxplay_log_level_t` | `INFO` | Minimum severity for log output. |
| `nohold` | `bool` | `false` | Allow a new client to kick the existing one. |
| `nofreeze` | `bool` | `false` | Do not freeze the last frame when the client disconnects. |
| `coverart_display` | `bool` | `false` | Display cover art during audio-only playback. |

## Event Handling

Register a callback with `uxplay_set_event_callback()`. The callback is invoked from internal threads, so keep it fast and thread-safe. The `uxplay_event_data_t` struct carries a `type` field that tells you which union member to read:

| Event Type | Payload | Description |
|------------|---------|-------------|
| `CLIENT_CONNECTED` | `event->client` | A device connected. Fields: `device_id`, `device_model`, `device_name`. |
| `CLIENT_DISCONNECTED` | (none) | The client disconnected. |
| `DISPLAY_PIN` | `event->pin` | Show this PIN code to the user for pairing. |
| `MIRROR_STARTED` | (none) | Screen mirroring has begun. |
| `MIRROR_STOPPED` | (none) | Screen mirroring has ended. |
| `AUDIO_METADATA` | `event->audio_meta` | Now-playing info changed. Fields: `artist`, `title`, `album`. |
| `VIDEO_SIZE_CHANGED` | `event->video_size` | Video resolution changed. Fields: `width`, `height`, `width_source`, `height_source`. |
| `ERROR` | `event->error_msg` | An unrecoverable error occurred. |

The following event types are defined in the header but not yet fired by the current implementation: `STATE_CHANGED`, `AUDIO_STARTED`, `AUDIO_STOPPED`. They are reserved for future use.

Example:

```c
static void on_event(void *user_data, const uxplay_event_data_t *event) {
    switch (event->type) {
    case UXPLAY_EVENT_CLIENT_CONNECTED:
        printf("Connected: %s (%s)\n",
               event->client.device_name,
               event->client.device_model);
        break;
    case UXPLAY_EVENT_AUDIO_METADATA:
        printf("Now playing: %s - %s [%s]\n",
               event->audio_meta.artist,
               event->audio_meta.title,
               event->audio_meta.album);
        break;
    case UXPLAY_EVENT_ERROR:
        fprintf(stderr, "Error: %s\n", event->error_msg);
        break;
    default:
        break;
    }
}
```

## Log Callback

Register with `uxplay_set_log_callback()` to capture all internal log messages. When a log callback is set, the library routes every log line through it instead of printing to stdout. This is useful for integrating with your application's own logging framework.

```c
static void on_log(void *ud, uxplay_log_level_t level, const char *msg) {
    const char *tag;
    switch (level) {
    case UXPLAY_LOG_ERROR:   tag = "ERR"; break;
    case UXPLAY_LOG_WARNING: tag = "WRN"; break;
    case UXPLAY_LOG_INFO:    tag = "INF"; break;
    case UXPLAY_LOG_DEBUG:   tag = "DBG"; break;
    default:                 tag = "   "; break;
    }
    printf("[%s] %s\n", tag, msg);
}

uxplay_set_log_callback(server, on_log, NULL);
```

At startup the library logs the full argument list it passes to the internal server core as an `INFO`-level message prefixed with `[libuxplay] args:`. This is useful for diagnosing configuration issues.

## C# / WinUI 3 Integration

A complete P/Invoke binding is provided in `examples/UxPlayInterop.cs`. Copy it into your .NET project along with `libuxplaylib.dll`.

```csharp
using UxPlay.Interop;

// Create and configure
var server = UxPlayLib.Create();
var config = UxPlayLib.DefaultConfig();
config.ServerName = "My AirPlay";
config.UseVideo = true;
config.UseAudio = true;
UxPlayLib.Configure(server, ref config);

// Start
UxPlayLib.Start(server);

// ... run your WinUI 3 event loop ...

// Cleanup
UxPlayLib.Stop(server);
UxPlayLib.Destroy(server);
config.Dispose();  // frees native string allocations
```

When distributing, make sure the following DLLs are alongside your executable:

- `libuxplaylib.dll`
- GStreamer runtime DLLs (or add the GStreamer `bin` directory to `PATH`)
- `dnssd.dll` from the Bonjour SDK

## Error Handling

Every API function that can fail returns `uxplay_error_t`. Always check the return value:

```c
uxplay_error_t err = uxplay_start(server);
if (err != UXPLAY_OK) {
    fprintf(stderr, "Failed to start: %s\n", uxplay_error_string(err));
    uxplay_destroy(server);
    return 1;
}
```

| Error Code | Meaning |
|------------|---------|
| `UXPLAY_OK` | Success. |
| `UXPLAY_ERR_INVALID_ARGUMENT` | NULL handle or invalid config value. |
| `UXPLAY_ERR_ALREADY_RUNNING` | `start()` called on a server that is already running. |
| `UXPLAY_ERR_NOT_RUNNING` | `stop()` called on a server that is not running. |
| `UXPLAY_ERR_GSTREAMER_INIT` | GStreamer initialization failed. Check that GStreamer is installed and the `GST_PLUGIN_PATH` is set. |
| `UXPLAY_ERR_RAOP_INIT` | RAOP protocol initialization failed. |
| `UXPLAY_ERR_DNSSD_INIT` | DNS-SD (Bonjour) service registration failed. Make sure the Bonjour service is running. |
| `UXPLAY_ERR_NETWORK` | Network error (port binding, etc.). |
| `UXPLAY_ERR_OUT_OF_MEMORY` | Memory allocation failed. |
| `UXPLAY_ERR_INTERNAL` | Unexpected internal error. |

## Troubleshooting

**Server starts but the device does not appear on iPhone/Mac**

- Verify that both machines are on the same local network (same subnet).
- Check that the Windows Firewall is not blocking incoming connections. Add exceptions for the executable or temporarily disable the firewall to test.
- Confirm that the Bonjour service (`mDNSResponder`) is running: open `services.msc` and look for "Bonjour Service".

**`UXPLAY_ERR_GSTREAMER_INIT` on startup**

- Make sure GStreamer is installed. In MSYS2: `pacman -S mingw-w64-ucrt-x86_64-gstreamer mingw-w64-ucrt-x86_64-gst-plugins-base`.
- If GStreamer is installed outside MSYS2, set `GST_PLUGIN_PATH` to point to the plugins directory.

**Black screen or no video output**

- Try setting `cfg.videosink = "d3d11videosink"` for hardware-accelerated rendering on Windows.
- If that does not work, fall back to `"glimagesink"` or `"autovideosink"`.
- Check GStreamer debug output by setting the environment variable `GST_DEBUG=3`.

**Audio plays but video does not**

- The H.264 decoder may be missing. Install the "bad" plugins: `pacman -S mingw-w64-ucrt-x86_64-gst-plugins-bad`.
- Alternatively, try `cfg.video_decoder = "avdec_h264"` to force a specific software decoder.

**High latency or stuttering**

- Set `cfg.video_sync = false` to disable timestamp-based synchronization (reduces latency at the cost of occasional frame drops).
- Use `d3d11videosink` instead of software-based sinks.
- Lower `cfg.max_fps` (e.g. to 24) if the machine cannot keep up with 30 fps.

**PIN code is never displayed**

- Set `cfg.access_control = UXPLAY_ACCESS_PIN` and handle the `UXPLAY_EVENT_DISPLAY_PIN` event in your callback. The PIN string is in `event->pin`.

**Closing the GStreamer video window**

- In library mode, closing the GStreamer video window causes the server to shut down cleanly. The host application can restart the server by calling `uxplay_start()` again. The reconnect/relaunch behavior present in standalone UxPlay is intentionally disabled because GStreamer pipeline teardown on Windows is not safe in that code path.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).

Based on [UxPlay](https://github.com/FDH2/UxPlay) by FDH2 / antimof. The `lib/llhttp` directory is licensed under the MIT License. The `lib/playfair` directory carries its own license (see `lib/playfair/LICENSE.md`).
