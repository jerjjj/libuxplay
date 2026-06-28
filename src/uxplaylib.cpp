/**
 * UxPlayLib — Thin wrapper around the UxPlay core (uxplay_core.cpp).
 *
 * This file converts the uxplay_config_t struct into command-line
 * arguments and delegates to start_uxplay() / stop_uxplay(), which
 * contain the proven, upstream UxPlay logic.
 *
 * License: GNU General Public License v3.0
 */

#ifndef UXPLAY_BUILDING_DLL
#define UXPLAY_BUILDING_DLL
#endif

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "../include/uxplaylib.h"
#include "uxplay_api.h"

/* We also need raop.h for runtime controls (volume, disconnect) */
#include "../lib/raop.h"

/* ========================================================================
 * Active instance pointer (single-instance; for bridge callbacks)
 * ======================================================================== */

static uxplay_instance_s *g_active_inst = nullptr;

/* ========================================================================
 * Internal instance structure
 * ======================================================================== */

struct uxplay_instance_s {
    std::atomic<uxplay_state_t> state{UXPLAY_STATE_IDLE};
    uxplay_config_t             cfg{};

    /* Owned copies of config strings */
    std::string str_server_name;
    std::string str_mac_address;
    std::string str_videosink;
    std::string str_video_decoder;
    std::string str_video_converter;
    std::string str_video_parser;
    std::string str_audiosink;
    std::string str_password;
    std::string str_keyfile;
    std::string str_coverart_filename;
    std::string str_lang;
    std::string str_metadata_filename;
    std::string str_record_filename;

    /* Callbacks (currently informational — not wired into core) */
    uxplay_event_callback_t event_cb  = nullptr;
    void                   *event_ud  = nullptr;
    uxplay_log_callback_t   log_cb    = nullptr;
    void                   *log_ud    = nullptr;

    /* Background thread */
    std::thread server_thread;
};

/* ========================================================================
 * Bridge: forward core events to user's event callback
 * ======================================================================== */

static void bridge_core_event(const uxplay_core_event_t *evt, void *ud) {
    auto *inst = static_cast<uxplay_instance_s *>(ud);
    if (!inst || !inst->event_cb) return;

    uxplay_event_data_t out{};
    switch (evt->type) {
    case UXPLAY_CORE_EVT_CLIENT_CONNECTED:
        out.type = UXPLAY_EVENT_CLIENT_CONNECTED;
        out.client.device_id    = evt->client.device_id;
        out.client.device_model = evt->client.model;
        out.client.device_name  = evt->client.name;
        break;
    case UXPLAY_CORE_EVT_CLIENT_DISCONNECTED:
        out.type = UXPLAY_EVENT_CLIENT_DISCONNECTED;
        break;
    case UXPLAY_CORE_EVT_DISPLAY_PIN:
        out.type = UXPLAY_EVENT_DISPLAY_PIN;
        out.pin  = evt->pin;
        break;
    case UXPLAY_CORE_EVT_MIRROR_STARTED:
        out.type = UXPLAY_EVENT_MIRROR_STARTED;
        break;
    case UXPLAY_CORE_EVT_MIRROR_STOPPED:
        out.type = UXPLAY_EVENT_MIRROR_STOPPED;
        break;
    case UXPLAY_CORE_EVT_AUDIO_STARTED:
        out.type = UXPLAY_EVENT_AUDIO_STARTED;
        break;
    case UXPLAY_CORE_EVT_VIDEO_SIZE:
        out.type = UXPLAY_EVENT_VIDEO_SIZE_CHANGED;
        out.video_size.width_source  = evt->video_size.ws;
        out.video_size.height_source = evt->video_size.hs;
        out.video_size.width         = evt->video_size.w;
        out.video_size.height        = evt->video_size.h;
        break;
    case UXPLAY_CORE_EVT_AUDIO_META:
        out.type = UXPLAY_EVENT_AUDIO_METADATA;
        out.audio_meta.artist = evt->meta.artist;
        out.audio_meta.title  = evt->meta.title;
        out.audio_meta.album  = evt->meta.album;
        break;
    case UXPLAY_CORE_EVT_ERROR:
        out.type = UXPLAY_EVENT_ERROR;
        out.error_msg = evt->error_msg;
        break;
    default:
        return;  /* unknown event, don't forward */
    }
    inst->event_cb(inst->event_ud, &out);
}

static void bridge_core_log(int level, const char *msg, void *ud) {
    auto *inst = static_cast<uxplay_instance_s *>(ud);
    if (!inst || !inst->log_cb) return;

    uxplay_log_level_t lv;
    if      (level <= 3) lv = UXPLAY_LOG_ERROR;
    else if (level == 4) lv = UXPLAY_LOG_WARNING;
    else if (level == 5) lv = UXPLAY_LOG_INFO;
    else if (level == 6) lv = UXPLAY_LOG_DEBUG;
    else                 lv = UXPLAY_LOG_VERBOSE;
    inst->log_cb(inst->log_ud, lv, msg);
}

/* ========================================================================
 * Config → argv conversion
 * ======================================================================== */

static std::vector<std::string> config_to_args(const uxplay_config_t *c) {
    std::vector<std::string> a;
    a.push_back("uxplay");                    /* argv[0] = program name */

    /* ---- Identity ---- */
    if (c->server_name && c->server_name[0]) {
        a.push_back("-n");
        a.push_back(c->server_name);
    }
    if (!c->append_hostname) {
        a.push_back("-nh");
    }
    if (c->mac_address && c->mac_address[0]) {
        a.push_back("-m");
        a.push_back(c->mac_address);
    }

    /* ---- Display ---- */
    {
        unsigned w = c->width  ? c->width  : 1920;
        unsigned h = c->height ? c->height : 1080;
        std::string res = std::to_string(w) + "x" + std::to_string(h);
        if (c->refresh_rate) {
            res += "@" + std::to_string(c->refresh_rate);
        }
        a.push_back("-s");
        a.push_back(res);
    }
    if (c->max_fps) {
        a.push_back("-fps");
        a.push_back(std::to_string(c->max_fps));
    }

    /* ---- Video ---- */
    if (!c->use_video) {
        a.push_back("-vs");
        a.push_back("0");
    } else {
        if (c->videosink && c->videosink[0]) {
            a.push_back("-vs");
            a.push_back(c->videosink);
        }
        if (c->video_decoder && c->video_decoder[0]) {
            a.push_back("-vd");
            a.push_back(c->video_decoder);
        }
        if (c->video_converter && c->video_converter[0]) {
            a.push_back("-vc");
            a.push_back(c->video_converter);
        }
        if (c->video_parser && c->video_parser[0]) {
            a.push_back("-vp");
            a.push_back(c->video_parser);
        }
        if (c->videoflip != UXPLAY_FLIP_NONE) {
            /* Map to upstream -f (flip) / -r (rotate) */
            switch (c->videoflip) {
            case UXPLAY_FLIP_HFLIP:  a.push_back("-f"); a.push_back("H"); break;
            case UXPLAY_FLIP_VFLIP:  a.push_back("-f"); a.push_back("V"); break;
            case UXPLAY_FLIP_INVERT: a.push_back("-f"); a.push_back("I"); break;
            case UXPLAY_FLIP_RIGHT:  a.push_back("-r"); a.push_back("R"); break;
            case UXPLAY_FLIP_LEFT:   a.push_back("-r"); a.push_back("L"); break;
            default: break;
            }
        }
        if (c->fullscreen) {
            a.push_back("-fs");
        }
        if (c->h265_support) {
            a.push_back("-h265");
        }
        if (!c->video_sync) {
            a.push_back("-vsync");
            a.push_back("no");
        }
        if (c->bt709_fix) {
            a.push_back("-bt709");
        }
    }

    /* ---- Audio ---- */
    if (!c->use_audio) {
        a.push_back("-as");
        a.push_back("0");
    } else {
        if (c->audiosink && c->audiosink[0]) {
            a.push_back("-as");
            a.push_back(c->audiosink);
        }
        if (c->audio_sync) {
            a.push_back("-async");
        }
    }
    if (c->db_low != -30.0 || c->db_high != 0.0) {
        a.push_back("-db");
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f:%.1f", c->db_low, c->db_high);
        a.push_back(buf);
    }
    if (c->initial_volume != 0.0) {
        a.push_back("-vol");
        /* Convert dB to 0.0-1.0 fraction matching upstream formula */
        double frac;
        if (c->initial_volume <= -144.0)      frac = 0.0;
        else if (c->initial_volume >= 0.0)    frac = 1.0;
        else                                  frac = (c->initial_volume + 144.0) / 144.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.3f", frac);
        a.push_back(buf);
    }

    /* ---- Network ---- */
    if (c->tcp_ports[0]) {
        a.push_back("-p");
        a.push_back("tcp");
        std::string ports = std::to_string(c->tcp_ports[0]);
        if (c->tcp_ports[1]) ports += "," + std::to_string(c->tcp_ports[1]);
        if (c->tcp_ports[2]) ports += "," + std::to_string(c->tcp_ports[2]);
        a.push_back(ports);
    }
    if (c->udp_ports[0]) {
        a.push_back("-p");
        a.push_back("udp");
        std::string ports = std::to_string(c->udp_ports[0]);
        if (c->udp_ports[1]) ports += "," + std::to_string(c->udp_ports[1]);
        if (c->udp_ports[2]) ports += "," + std::to_string(c->udp_ports[2]);
        a.push_back(ports);
    }

    /* ---- Security ---- */
    if (c->access_control == UXPLAY_ACCESS_PIN) {
        a.push_back("-pin");
    } else if (c->access_control == UXPLAY_ACCESS_PASSWORD) {
        if (c->password && c->password[0]) {
            a.push_back("-pw");
            a.push_back(c->password);
        }
    }
    if (c->keyfile && c->keyfile[0]) {
        a.push_back("-key");
        a.push_back(c->keyfile);
    }
    if (c->registration_list) {
        a.push_back("-reg");
    }

    /* ---- Miscellaneous ---- */
    if (c->log_level <= UXPLAY_LOG_DEBUG) {
        a.push_back("-d");
    }
    if (c->nohold) {
        a.push_back("-nohold");
    }
    if (c->nofreeze) {
        a.push_back("-nofreeze");
    }
    if (c->keep_window) {
        a.push_back("-nc");
    }
    if (c->force_software_decoder) {
        a.push_back("-avdec");
    }
    if (c->taper_volume) {
        a.push_back("-taper");
    }
    if (!c->srgb_fix) {
        a.push_back("-srgb");
        a.push_back("no");
    }
    if (c->audio_latency > 0.0) {
        a.push_back("-al");
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", c->audio_latency);
        a.push_back(buf);
    }
    if (c->reset_timeout > 0) {
        a.push_back("-reset");
        a.push_back(std::to_string(c->reset_timeout));
    }
    if (c->metadata_filename && c->metadata_filename[0]) {
        a.push_back("-md");
        a.push_back(c->metadata_filename);
    }
    if (c->record_filename && c->record_filename[0]) {
        a.push_back("-mp4");
        a.push_back(c->record_filename);
    }
    if (c->hls_support) {
        a.push_back("-hls");
    }
    if (c->lang && c->lang[0]) {
        a.push_back("-lang");
        a.push_back(c->lang);
    }
    if (c->coverart_display) {
        if (c->coverart_filename && c->coverart_filename[0]) {
            a.push_back("-ca");
            a.push_back(c->coverart_filename);
        } else {
            a.push_back("-ca");
            a.push_back(".");
        }
    }

    return a;
}

/* ========================================================================
 * DllMain — Windows socket init
 * ======================================================================== */

#ifdef _WIN32
static bool g_winsock_ok = false;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        WSADATA wsa;
        g_winsock_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_winsock_ok) WSACleanup();
    }
    return TRUE;
}
#endif

/* ========================================================================
 * Public API — Lifecycle
 * ======================================================================== */

UXPLAY_API uxplay_config_t uxplay_default_config(void) {
    uxplay_config_t c{};
    c.server_name      = "UxPlay";
    c.append_hostname  = true;
    c.width            = 0;
    c.height           = 0;
    c.refresh_rate     = 0;
    c.max_fps          = 0;
    c.videoflip        = UXPLAY_FLIP_NONE;
    c.fullscreen       = false;
    c.h265_support     = false;
    c.video_sync       = true;
    c.bt709_fix        = false;
    c.use_video        = true;
    c.nofreeze         = false;
    c.audio_sync       = false;
    c.use_audio        = true;
    c.initial_volume   = 0.0;
    c.db_low           = -30.0;
    c.db_high          = 0.0;
    c.access_control   = UXPLAY_ACCESS_FREE;
    c.log_level        = UXPLAY_LOG_INFO;
    c.nohold           = false;
    c.coverart_display = false;
    c.hls_support      = false;
    c.taper_volume          = false;
    c.srgb_fix              = true;
    c.audio_latency         = 0.0;
    c.reset_timeout         = 0;
    c.keep_window           = false;
    c.force_software_decoder = false;
    return c;
}

UXPLAY_API const char *uxplay_version(void) {
    return UXPLAYLIB_VERSION_STRING;
}

UXPLAY_API const char *uxplay_error_string(uxplay_error_t error) {
    switch (error) {
    case UXPLAY_OK:                   return "OK";
    case UXPLAY_ERR_INVALID_ARGUMENT: return "Invalid argument";
    case UXPLAY_ERR_ALREADY_RUNNING:  return "Already running";
    case UXPLAY_ERR_NOT_RUNNING:      return "Not running";
    case UXPLAY_ERR_GSTREAMER_INIT:   return "GStreamer init failed";
    case UXPLAY_ERR_RAOP_INIT:        return "RAOP init failed";
    case UXPLAY_ERR_DNSSD_INIT:       return "DNS-SD init failed";
    case UXPLAY_ERR_NETWORK:          return "Network error";
    case UXPLAY_ERR_OUT_OF_MEMORY:    return "Out of memory";
    case UXPLAY_ERR_INTERNAL:         return "Internal error";
    default:                          return "Unknown error";
    }
}

UXPLAY_API uxplay_error_t uxplay_create(uxplay_t *out_handle) {
    if (!out_handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    auto *inst = new (std::nothrow) uxplay_instance_s;
    if (!inst) return UXPLAY_ERR_OUT_OF_MEMORY;
    inst->cfg = uxplay_default_config();
    *out_handle = inst;
    return UXPLAY_OK;
}

static void copy_string(std::string &dst, const char *src) {
    if (src) dst = src; else dst.clear();
}

UXPLAY_API uxplay_error_t uxplay_configure(uxplay_t handle,
                                            const uxplay_config_t *config) {
    if (!handle || !config) return UXPLAY_ERR_INVALID_ARGUMENT;
    if (handle->state.load() == UXPLAY_STATE_RUNNING)
        return UXPLAY_ERR_ALREADY_RUNNING;

    /* Deep-copy strings into the instance */
    copy_string(handle->str_server_name,      config->server_name);
    copy_string(handle->str_mac_address,       config->mac_address);
    copy_string(handle->str_videosink,         config->videosink);
    copy_string(handle->str_video_decoder,     config->video_decoder);
    copy_string(handle->str_video_converter,   config->video_converter);
    copy_string(handle->str_video_parser,      config->video_parser);
    copy_string(handle->str_audiosink,         config->audiosink);
    copy_string(handle->str_password,          config->password);
    copy_string(handle->str_keyfile,           config->keyfile);
    copy_string(handle->str_coverart_filename, config->coverart_filename);
    copy_string(handle->str_lang,              config->lang);
    copy_string(handle->str_metadata_filename, config->metadata_filename);
    copy_string(handle->str_record_filename,   config->record_filename);

    /* Copy the whole struct, then fix the string pointers */
    handle->cfg = *config;
    handle->cfg.server_name      = handle->str_server_name.c_str();
    handle->cfg.mac_address      = handle->str_mac_address.empty() ? nullptr : handle->str_mac_address.c_str();
    handle->cfg.videosink        = handle->str_videosink.empty() ? nullptr : handle->str_videosink.c_str();
    handle->cfg.video_decoder    = handle->str_video_decoder.empty() ? nullptr : handle->str_video_decoder.c_str();
    handle->cfg.video_converter  = handle->str_video_converter.empty() ? nullptr : handle->str_video_converter.c_str();
    handle->cfg.video_parser     = handle->str_video_parser.empty() ? nullptr : handle->str_video_parser.c_str();
    handle->cfg.audiosink        = handle->str_audiosink.empty() ? nullptr : handle->str_audiosink.c_str();
    handle->cfg.password         = handle->str_password.empty() ? nullptr : handle->str_password.c_str();
    handle->cfg.keyfile          = handle->str_keyfile.empty() ? nullptr : handle->str_keyfile.c_str();
    handle->cfg.coverart_filename= handle->str_coverart_filename.empty() ? nullptr : handle->str_coverart_filename.c_str();
    handle->cfg.lang             = handle->str_lang.empty() ? nullptr : handle->str_lang.c_str();
    handle->cfg.metadata_filename = handle->str_metadata_filename.empty() ? nullptr : handle->str_metadata_filename.c_str();
    handle->cfg.record_filename  = handle->str_record_filename.empty() ? nullptr : handle->str_record_filename.c_str();

    return UXPLAY_OK;
}

UXPLAY_API uxplay_error_t uxplay_set_event_callback(uxplay_t handle,
                                                     uxplay_event_callback_t callback,
                                                     void *user_data) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    handle->event_cb = callback;
    handle->event_ud = user_data;
    return UXPLAY_OK;
}

UXPLAY_API uxplay_error_t uxplay_set_log_callback(uxplay_t handle,
                                                    uxplay_log_callback_t callback,
                                                    void *user_data) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    handle->log_cb = callback;
    handle->log_ud = user_data;
    return UXPLAY_OK;
}

UXPLAY_API uxplay_error_t uxplay_start(uxplay_t handle) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;

    uxplay_state_t expected = UXPLAY_STATE_IDLE;
    if (!handle->state.compare_exchange_strong(expected, UXPLAY_STATE_STARTING))
        return UXPLAY_ERR_ALREADY_RUNNING;

    /* Convert configuration to command-line arguments */
    auto args = config_to_args(&handle->cfg);

    /* Diagnostic: log the generated argument list via log_cb */
    if (handle->log_cb) {
        std::string dbg = "[libuxplay] args:";
        for (auto &s : args) { dbg += " "; dbg += s; }
        handle->log_cb(handle->log_ud, UXPLAY_LOG_INFO, dbg.c_str());
    }

    /* Install core callbacks BEFORE launching the thread */
    uxplay_core_set_callbacks(bridge_core_event, bridge_core_log, handle);
    g_active_inst = handle;

    /* Launch the core in a background thread */
    handle->server_thread = std::thread([handle, args = std::move(args)]() {
        /* Build argv (pointers into the captured strings) */
        std::vector<char *> argv;
        argv.reserve(args.size());
        for (auto &s : args)
            argv.push_back(const_cast<char *>(s.c_str()));

        handle->state.store(UXPLAY_STATE_RUNNING);

        /* This blocks until the server shuts down */
        start_uxplay(static_cast<int>(argv.size()), argv.data());

        /* Reset core globals so server can be restarted */
        uxplay_core_reset_state();
        g_active_inst = nullptr;
        handle->state.store(UXPLAY_STATE_IDLE);
    });

    return UXPLAY_OK;
}

UXPLAY_API uxplay_error_t uxplay_stop(uxplay_t handle) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;

    uxplay_state_t st = handle->state.load();
    if (st != UXPLAY_STATE_RUNNING && st != UXPLAY_STATE_STARTING)
        return UXPLAY_ERR_NOT_RUNNING;

    handle->state.store(UXPLAY_STATE_STOPPING);

    /* Ask the core to shut down (signals the GLib main loop to quit) */
    stop_uxplay();

    /* Wait for the background thread to finish */
    if (handle->server_thread.joinable())
        handle->server_thread.join();

    handle->state.store(UXPLAY_STATE_IDLE);
    return UXPLAY_OK;
}

UXPLAY_API void uxplay_destroy(uxplay_t handle) {
    if (!handle) return;

    /* Stop if still running */
    if (handle->state.load() == UXPLAY_STATE_RUNNING ||
        handle->state.load() == UXPLAY_STATE_STARTING) {
        uxplay_stop(handle);
    }
    if (handle->server_thread.joinable())
        handle->server_thread.join();

    delete handle;
}

/* ========================================================================
 * Public API — Runtime
 * ======================================================================== */

UXPLAY_API uxplay_state_t uxplay_get_state(uxplay_t handle) {
    if (!handle) return UXPLAY_STATE_ERROR;
    return handle->state.load();
}

UXPLAY_API uxplay_error_t uxplay_set_volume(uxplay_t handle, double volume) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    /* AirPlay volume is controlled by the client device.
       The server receives volume changes through callbacks.
       Server-initiated volume control is not part of the protocol. */
    (void)volume;
    return UXPLAY_OK;
}

UXPLAY_API int uxplay_get_connection_count(uxplay_t handle) {
    if (!handle) return 0;
    return (int)uxplay_core_get_connections();
}

UXPLAY_API uxplay_error_t uxplay_disconnect_clients(uxplay_t handle) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    if (handle->state.load() != UXPLAY_STATE_RUNNING)
        return UXPLAY_ERR_NOT_RUNNING;
    raop_t *r = (raop_t *)uxplay_core_get_raop();
    if (r) raop_remove_known_connections(r);
    return UXPLAY_OK;
}
