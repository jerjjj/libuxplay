/**
 * UxPlayLib - AirPlay Mirroring & Audio Server Library for Windows
 * Based on UxPlay (GPLv3) by FDH2 / antimof
 *
 * Library implementation — wraps the core UxPlay server logic into
 * a clean C API suitable for DLL export and GUI integration.
 *
 * License: GNU General Public License v3.0
 */

#ifndef UXPLAY_BUILDING_DLL
#define UXPLAY_BUILDING_DLL
#endif

#include <stddef.h>
#include <cstring>
#include <ctype.h>
#include <string>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <stdarg.h>
#include <math.h>
#include <inttypes.h>
#include <mutex>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <glib.h>
#include <unordered_map>
#include <winsock2.h>
#include <iphlpapi.h>
#include <pthread.h>
#include <windows.h>
#else
#include <csignal>
#include <glib-unix.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "../include/uxplaylib.h"
#include "../lib/raop.h"
#include "../lib/stream.h"
#include "../lib/logger.h"
#include "../lib/crypto.h"
#include "../renderers/video_renderer.h"
#include "../renderers/audio_renderer.h"
#include "../renderers/mux_renderer.h"

/*============================================================================
 * Internal Constants
 *============================================================================*/

#define LIB_VERSION       "1.73.6"
#define SECOND_IN_USECS   1000000
#define SECOND_IN_NSECS   1000000000UL
#define DEFAULT_NAME      "UxPlay"
#define LOWEST_ALLOWED_PORT  1024
#define HIGHEST_PORT         65535
#define MISSED_FEEDBACK_LIMIT 15
#define MAX_VIDEO_RENDERERS  5
#define MAX_AUDIO_RENDERERS  3

/*============================================================================
 * Internal Instance Structure
 *============================================================================*/

struct uxplay_instance_s {
    /* --- State --- */
    std::atomic<uxplay_state_t>  state{UXPLAY_STATE_IDLE};
    std::mutex                   mutex;

    /* --- Configuration (copied from user) --- */
    uxplay_config_t              cfg;
    /* Owned string copies (config pointers refer into these) */
    std::string  str_server_name;
    std::string  str_mac_address;
    std::string  str_videosink;
    std::string  str_videosink_options;
    std::string  str_video_decoder;
    std::string  str_video_converter;
    std::string  str_video_parser;
    std::string  str_audiosink;
    std::string  str_password;
    std::string  str_keyfile;
    std::string  str_coverart_filename;
    std::string  str_lang;

    /* --- Callbacks --- */
    uxplay_event_callback_t   event_cb      = nullptr;
    void                     *event_ud      = nullptr;
    uxplay_log_callback_t     log_cb        = nullptr;
    void                     *log_ud        = nullptr;

    /* --- Server objects --- */
    dnssd_t   *dnssd          = nullptr;
    raop_t    *raop           = nullptr;
    logger_t  *render_logger  = nullptr;

    /* --- GLib main loop & thread --- */
    GMainLoop  *main_loop     = nullptr;
    std::thread server_thread;

    /* --- Runtime state --- */
    unsigned int  open_connections = 0;
    uint64_t      remote_clock_offset = 0;
    bool          relaunch_video    = false;
    bool          reset_loop        = false;
    bool          reset_httpd       = false;
    bool          preserve_connections = false;
    bool          close_window      = true;
    bool          full_video_reset  = true;
    int           n_video_renderers = 0;
    int           n_audio_renderers = 0;
    unsigned char compression_type  = 0;
    unsigned char audio_type        = 0;
    guint         missed_feedback   = 0;
    bool          monitor_progress  = false;
    uint32_t      rtptime           = 0;
    uint32_t      rtptime_start     = 0;
    uint32_t      rtptime_end       = 0;
    int64_t       audio_delay_alac  = 0;
    int64_t       audio_delay_aac   = 0;

    /* Video flip arrays */
    videoflip_t   videoflip[2] = { NONE, NONE };

    /* Network ports */
    unsigned short raop_port    = 0;
    unsigned short airplay_port = 0;
    unsigned short display[5]   = {0};
    unsigned short tcp_ports[3] = {0};
    unsigned short udp_ports[3] = {0};

    /* Metadata */
    std::string  artist;
    std::string  track_title;
    std::string  track_album;

    /* Allowed / blocked clients */
    std::vector<std::string> allowed_clients;
    std::vector<std::string> blocked_clients;
    bool restrict_clients = false;
};

/*============================================================================
 * Single active instance pointer (for raop callbacks that receive void *cls)
 * The raop callback system passes cls = raop_callbacks_t::cls which we set
 * to the uxplay_instance_s pointer.
 *============================================================================*/

static uxplay_instance_s *g_inst = nullptr;

/*============================================================================
 * Internal logging
 *============================================================================*/

static void lib_log(uxplay_instance_s *inst, uxplay_log_level_t level, const char *format, ...) {
    char buf[2048];
    va_list va;
    va_start(va, format);
    vsnprintf(buf, sizeof(buf), format, va);
    va_end(va);

    if (inst && inst->log_cb) {
        inst->log_cb(inst->log_ud, level, buf);
    }
}

#define LOGI(...) lib_log(g_inst, UXPLAY_LOG_INFO, __VA_ARGS__)
#define LOGW(...) lib_log(g_inst, UXPLAY_LOG_WARNING, __VA_ARGS__)
#define LOGE(...) lib_log(g_inst, UXPLAY_LOG_ERROR, __VA_ARGS__)
#define LOGD(...) lib_log(g_inst, UXPLAY_LOG_DEBUG, __VA_ARGS__)

/*============================================================================
 * Internal event dispatch
 *============================================================================*/

static void emit_event(uxplay_instance_s *inst, const uxplay_event_data_t *evt) {
    if (inst && inst->event_cb) {
        inst->event_cb(inst->event_ud, evt);
    }
}

static void emit_state_change(uxplay_instance_s *inst, uxplay_state_t state) {
    inst->state.store(state);
    uxplay_event_data_t evt{};
    evt.type  = UXPLAY_EVENT_STATE_CHANGED;
    evt.state = state;
    emit_event(inst, &evt);
}

/*============================================================================
 * Network helpers (adapted from uxplay.cpp)
 *============================================================================*/

static std::string find_mac_address() {
    std::string mac;
    char str[3];
#ifdef _WIN32
    ULONG buflen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
    if (!addresses) return mac;
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
        if (!addresses) return mac;
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES addr = addresses; addr; addr = addr->Next) {
            if (addr->PhysicalAddressLength != 6
                || (addr->IfType != 6 && addr->IfType != 71)
                || addr->OperStatus != 1) continue;
            mac.clear();
            for (int i = 0; i < 6; i++) {
                snprintf(str, sizeof(str), "%02x", (int)addr->PhysicalAddress[i]);
                mac += str;
                if (i < 5) mac += ":";
            }
            break;
        }
    }
    free(addresses);
#else
    struct ifaddrs *ifap;
    if (getifaddrs(&ifap) == 0) {
        for (struct ifaddrs *p = ifap; p; p = p->ifa_next) {
            if (!p->ifa_addr) continue;
#ifdef __linux__
            if (p->ifa_addr->sa_family != AF_PACKET) continue;
            struct sockaddr_ll *s = (struct sockaddr_ll*) p->ifa_addr;
            unsigned char octet[6];
            int non_null = 0;
            for (int i = 0; i < 6; i++)
                if ((octet[i] = s->sll_addr[i]) != 0) non_null++;
#else
            if (p->ifa_addr->sa_family != AF_LINK) continue;
            unsigned char *ptr = (unsigned char *) LLADDR((struct sockaddr_dl *) p->ifa_addr);
            unsigned char octet[6];
            int non_null = 0;
            for (int i = 0; i < 6; i++)
                if ((octet[i] = *ptr++) != 0) non_null++;
#endif
            if (non_null) {
                mac.clear();
                for (int i = 0; i < 6; i++) {
                    snprintf(str, sizeof(str), "%02x", octet[i]);
                    mac += str;
                    if (i < 5) mac += ":";
                }
                break;
            }
        }
        freeifaddrs(ifap);
    }
#endif
    return mac;
}

static std::string random_mac_address() {
    char str[4];
    unsigned char random_bytes[6];
    get_random_bytes(random_bytes, sizeof(random_bytes));
    random_bytes[0] = (random_bytes[0] & ~0x01) | 0x02;
    snprintf(str, 3, "%02x", random_bytes[0]);
    std::string mac(str);
    for (int i = 1; i < 6; i++) {
        snprintf(str, 4, ":%02x", random_bytes[i]);
        mac += str;
    }
    return mac;
}

static int parse_hw_addr(const std::string &s, std::vector<char> &hw) {
    for (int i = 0; i < (int)s.length(); i += 3)
        hw.push_back((char) strtol(s.substr(i, 2).c_str(), NULL, 16));
    return 0;
}

static void append_hostname(std::string &name) {
    char hostname[256] = {0};
#ifdef _WIN32
    DWORD len = sizeof(hostname);
    if (GetComputerNameA(hostname, &len) && strlen(hostname)) {
        name += "@";
        name += hostname;
    }
#else
    if (gethostname(hostname, sizeof(hostname)) == 0 && strlen(hostname)) {
        name += "@";
        name += hostname;
    }
#endif
}

/*============================================================================
 * DMAP metadata parser (simplified from uxplay.cpp)
 *============================================================================*/

static int parse_dmap_header(const unsigned char *data, char *tag, int *len) {
    if (!data) return -1;
    memcpy(tag, data, 4);
    tag[4] = '\0';
    *len = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    if (strcmp(tag, "mlit") != 0) return -1;
    return 0;
}

/*============================================================================
 * RAOP Callbacks (adapted from uxplay.cpp, using g_inst)
 *============================================================================*/

extern "C" {

static void lib_log_callback(void *cls, int level, const char *msg) {
    if (!g_inst) return;
    uxplay_log_level_t lvl;
    switch (level) {
        case 0: case 1: case 2: case 3: lvl = UXPLAY_LOG_ERROR; break;
        case 4: lvl = UXPLAY_LOG_WARNING; break;
        case 5: lvl = UXPLAY_LOG_INFO; break;
        default: lvl = UXPLAY_LOG_DEBUG; break;
    }
    lib_log(g_inst, lvl, "%s", msg);
}

static void cb_conn_init(void *cls) {
    if (!g_inst) return;
    g_inst->open_connections++;
    LOGD("Open connections: %u", g_inst->open_connections);

    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_CLIENT_CONNECTED;
    emit_event(g_inst, &evt);
}

static void cb_conn_destroy(void *cls) {
    if (!g_inst) return;
    g_inst->open_connections--;
    LOGD("Open connections: %u", g_inst->open_connections);

    if (g_inst->open_connections == 0) {
        g_inst->remote_clock_offset = 0;
        if (g_inst->cfg.use_audio) {
            audio_renderer_stop();
        }
    }

    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_CLIENT_DISCONNECTED;
    emit_event(g_inst, &evt);
}

static void cb_conn_feedback(void *cls) {
    if (!g_inst) return;
    g_inst->missed_feedback = 0;
}

static void cb_conn_reset(void *cls, int reason) {
    if (!g_inst) return;
    switch (reason) {
        case 1: LOGI("*** ERROR lost connection with client (network problem?)"); break;
        case 2: LOGI("*** ERROR Unsupported HLS streaming source"); break;
        default: break;
    }
    if (!g_inst->cfg.nofreeze) {
        g_inst->close_window = false;
    }
    g_inst->reset_httpd = true;
    g_inst->relaunch_video = true;
    g_inst->reset_loop = true;
}

static void cb_conn_teardown(void *cls, bool *teardown_96, bool *teardown_110) {
    /* not used in current implementation */
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    if (!g_inst) return;
    LOGI("connection request from %s (%s) deviceID=%s", name, model, deviceid);
    *admit = true;

    if (g_inst->restrict_clients) {
        *admit = false;
        for (auto &id : g_inst->allowed_clients) {
            if (id == deviceid) { *admit = true; break; }
        }
    }
    for (auto &id : g_inst->blocked_clients) {
        if (id == deviceid) { *admit = false; break; }
    }

    if (*admit && g_inst->cfg.use_video) {
        video_renderer_set_device_model(model, name);
    }

    /* Emit event with client info */
    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_CLIENT_CONNECTED;
    evt.client.device_id = deviceid;
    evt.client.device_model = model;
    evt.client.device_name = name;
    emit_event(g_inst, &evt);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    if (!g_inst) return;
    if (g_inst->cfg.use_audio) {
        if (!g_inst->remote_clock_offset) {
            uint64_t local_time = (data->ntp_time_local ? data->ntp_time_local : get_local_time());
            g_inst->remote_clock_offset = local_time - data->ntp_time_remote;
        }
        data->ntp_time_remote += g_inst->remote_clock_offset;
        switch (data->ct) {
            case 2:
                g_inst->rtptime = data->rtp_time;
                if (g_inst->audio_delay_alac)
                    data->ntp_time_remote = (uint64_t)((int64_t)data->ntp_time_remote + g_inst->audio_delay_alac);
                break;
            case 4: case 8:
                g_inst->monitor_progress = false;
                if (g_inst->audio_delay_aac)
                    data->ntp_time_remote = (uint64_t)((int64_t)data->ntp_time_remote + g_inst->audio_delay_aac);
                break;
            default: break;
        }
        audio_renderer_render_buffer(data->data, &data->data_len, &data->seqnum, &data->ntp_time_remote);
    }
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    if (!g_inst) return;
    if (g_inst->cfg.use_video) {
        if (!g_inst->remote_clock_offset) {
            uint64_t local_time = (data->ntp_time_local ? data->ntp_time_local : get_local_time());
            g_inst->remote_clock_offset = local_time - data->ntp_time_remote;
        }
        int count = 0;
        uint64_t pts_mismatch = 0;
        do {
            data->ntp_time_remote += g_inst->remote_clock_offset;
            pts_mismatch = video_renderer_render_buffer(data->data, &data->data_len, &data->nal_count, &data->ntp_time_remote);
            if (pts_mismatch) {
                g_inst->remote_clock_offset += pts_mismatch;
            }
            count++;
        } while (pts_mismatch && count < 10);
    }
}

static void cb_video_pause(void *cls) {
    if (g_inst && g_inst->cfg.use_video) video_renderer_pause();
}

static void cb_video_resume(void *cls) {
    if (g_inst && g_inst->cfg.use_video) video_renderer_resume();
}

static void cb_audio_flush(void *cls) {
    if (g_inst && g_inst->cfg.use_audio) audio_renderer_flush();
}

static void cb_video_flush(void *cls) {
    if (g_inst && g_inst->cfg.use_video) video_renderer_flush();
}

static double cb_audio_set_client_volume(void *cls) {
    return g_inst ? g_inst->cfg.initial_volume : 0.0;
}

static void cb_audio_set_volume(void *cls, float volume) {
    if (!g_inst || !g_inst->cfg.use_audio) return;
    double db_low  = g_inst->cfg.db_low;
    double db_high = g_inst->cfg.db_high;
    double frac, gst_volume;

    if (volume == -144.0f) {
        frac = 0.0;
    } else if (volume < -30.0f || volume > 0.0f) {
        frac = (volume < -30.0f) ? 0.0 : 1.0;
    } else if (volume == -30.0f) {
        frac = 0.0;
    } else if (volume == 0.0f) {
        frac = 1.0;
    } else {
        frac = (double)((30.0f + volume) / 30.0f);
        if (frac > 1.0) frac = 1.0;
    }

    if (frac == 0.0) {
        gst_volume = 0.0;
    } else {
        double db = db_low + (db_high - db_low) * frac;
        gst_volume = pow(10.0, 0.05 * db);
    }
    audio_renderer_set_volume(gst_volume);
}

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf,
                                 bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    if (!g_inst) return;
    LOGI("ct=%d spf=%d usingScreen=%d isMedia=%d audioFormat=0x%lx",
         *ct, *spf, *usingScreen, *isMedia, (unsigned long)*audioFormat);

    unsigned char type;
    switch (*ct) {
        case 2:  type = 0x20; break;
        case 8:  type = 0x80; break;
        default: type = 0x10; break;
    }
    g_inst->audio_type = type;

    if (g_inst->cfg.use_audio) {
        audio_renderer_start(ct);
    }

    /* Emit audio started event */
    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_AUDIO_STARTED;
    emit_event(g_inst, &evt);
}

static void cb_video_report_size(void *cls, float *ws, float *hs, float *w, float *h) {
    if (!g_inst) return;
    if (g_inst->cfg.use_video) {
        video_renderer_size(ws, hs, w, h);
    }
    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_VIDEO_SIZE_CHANGED;
    evt.video_size.width_source  = *ws;
    evt.video_size.height_source = *hs;
    evt.video_size.width         = *w;
    evt.video_size.height        = *h;
    emit_event(g_inst, &evt);
}

static void cb_audio_set_coverart(void *cls, const void *buffer, int buflen) {
    if (!g_inst) return;
    if (buffer && g_inst->cfg.coverart_display) {
        video_renderer_choose_codec(true, false);
        video_renderer_display_jpeg(buffer, &buflen);
    }
}

static void cb_audio_stop_coverart_rendering(void *cls) {
    /* Simplified: no-op for now */
}

static void cb_audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    if (!g_inst) return;
    g_inst->rtptime_start = *start;
    g_inst->rtptime       = *curr;
    g_inst->rtptime_end   = *end;
}

static void cb_audio_set_metadata(void *cls, const void *buffer, int buflen) {
    if (!g_inst) return;
    const unsigned char *metadata = (const unsigned char *)buffer;
    char dmap_tag[5] = {0};
    int datalen;

    if (buflen < 8) return;
    if (parse_dmap_header(metadata, dmap_tag, &datalen)) return;

    /* Parse DMAP items to extract artist, title, album */
    int offset = 8;
    std::string artist_str, title_str, album_str;

    while (offset + 8 <= datalen + 8) {
        char tag[5] = {0};
        memcpy(tag, metadata + offset, 4);
        int item_len = (metadata[offset+4] << 24) | (metadata[offset+5] << 16) |
                       (metadata[offset+6] << 8) | metadata[offset+7];
        offset += 8;
        if (offset + item_len > buflen) break;

        if (strcmp(tag, "asar") == 0)
            artist_str.assign((const char*)(metadata + offset), item_len);
        else if (strcmp(tag, "minm") == 0)
            title_str.assign((const char*)(metadata + offset), item_len);
        else if (strcmp(tag, "asal") == 0)
            album_str.assign((const char*)(metadata + offset), item_len);

        offset += item_len;
    }

    g_inst->artist      = artist_str;
    g_inst->track_title = title_str;
    g_inst->track_album = album_str;

    if (!title_str.empty()) {
        LOGI("Now Playing: %s - %s (%s)", artist_str.c_str(), title_str.c_str(), album_str.c_str());
    }

    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_AUDIO_METADATA;
    evt.audio_meta.artist = g_inst->artist.c_str();
    evt.audio_meta.title  = g_inst->track_title.c_str();
    evt.audio_meta.album  = g_inst->track_album.c_str();
    emit_event(g_inst, &evt);
}

static void cb_display_pin(void *cls, char *pin) {
    if (!g_inst) return;
    LOGI("PIN code: %s", pin);
    uxplay_event_data_t evt{};
    evt.type = UXPLAY_EVENT_DISPLAY_PIN;
    evt.pin  = pin;
    emit_event(g_inst, &evt);
}

static const char* cb_passwd(void *cls, int *len) {
    if (!g_inst) { *len = 0; return NULL; }
    if (g_inst->cfg.access_control == UXPLAY_ACCESS_PASSWORD && g_inst->cfg.password) {
        *len = (int)strlen(g_inst->cfg.password);
        return g_inst->cfg.password;
    }
    *len = 0;
    return NULL;
}

static void cb_register_client(void *cls, const char *device_id, const char *client_pk, const char *client_name) {
    if (!g_inst) return;
    LOGI("Registered client: %s (%s)", client_name, device_id);
}

static bool cb_check_register(void *cls, const char *client_pk) {
    return true;  /* simplified: always allow */
}

static void cb_export_dacp(void *cls, const char *active_remote, const char *dacp_id) {
    /* No-op for library */
}

static void cb_video_reset(void *cls, reset_type_t type) {
    if (!g_inst) return;
    switch (type) {
        case RESET_TYPE_NOHOLD:
        case RESET_TYPE_HLS_EOS:
            if (g_inst->cfg.use_video) {
                video_renderer_stop();
                video_renderer_destroy();
                video_renderer_init(g_inst->render_logger, g_inst->str_server_name.c_str(),
                                    g_inst->videoflip, g_inst->str_video_parser.c_str(), "",
                                    g_inst->str_video_decoder.c_str(), g_inst->str_video_converter.c_str(),
                                    g_inst->str_videosink.c_str(), g_inst->str_videosink_options.c_str(),
                                    g_inst->cfg.fullscreen, g_inst->cfg.video_sync,
                                    g_inst->cfg.h265_support, g_inst->cfg.coverart_display, 3, NULL);
                video_renderer_start();
                g_inst->close_window = false;
            }
            g_inst->preserve_connections = false;
            g_inst->remote_clock_offset = 0;
            g_inst->relaunch_video = true;
            break;
        case RESET_TYPE_RTP_TO_HLS_TEARDOWN:
            g_inst->preserve_connections = true;
            /* fall through */
        case RESET_TYPE_RTP_SHUTDOWN:
            if (g_inst->cfg.use_video) video_renderer_stop();
            g_inst->remote_clock_offset = 0;
            g_inst->relaunch_video = true;
            break;
        case RESET_TYPE_HLS_SHUTDOWN:
            if (g_inst->cfg.use_video) video_renderer_stop();
            g_inst->preserve_connections = true;
            g_inst->remote_clock_offset = 0;
            g_inst->relaunch_video = true;
            break;
        case RESET_TYPE_ON_VIDEO_PLAY:
            break;
        default:
            break;
    }
    g_inst->reset_loop = true;
}

static int cb_video_set_codec(void *cls, video_codec_t codec) {
    if (!g_inst || !g_inst->cfg.use_video) return 0;
    bool is_h265 = (codec == VIDEO_CODEC_H265);
    return video_renderer_choose_codec(false, is_h265);
}

static void cb_mirror_video_running(void *cls, bool is_running) {
    if (!g_inst) return;
    uxplay_event_data_t evt{};
    evt.type = is_running ? UXPLAY_EVENT_MIRROR_STARTED : UXPLAY_EVENT_MIRROR_STOPPED;
    emit_event(g_inst, &evt);
}

static void cb_on_video_play(void *cls, const char *location, const float start_position) {
    /* HLS support: simplified for now */
}

static void cb_on_video_scrub(void *cls, const float position) {
    if (g_inst && g_inst->cfg.use_video) video_renderer_seek(position);
}

static void cb_on_video_rate(void *cls, const float rate) {
    if (!g_inst) return;
    if (rate == 0.0f && g_inst->cfg.use_video) video_renderer_pause();
    else if (g_inst->cfg.use_video) video_renderer_resume();
}

static void cb_on_video_stop(void *cls) {
    if (g_inst && g_inst->cfg.use_video) video_renderer_pause();
}

static void cb_on_video_acquire_playback_info(void *cls, playback_info_t *info) {
    if (!g_inst || !g_inst->cfg.use_video) return;
    double dur, pos, ss, sd;
    float rate;
    bool be, bf;
    if (video_get_playback_info(&dur, &pos, &ss, &sd, &rate, &be, &bf)) {
        info->duration = dur;
        info->position = pos;
        info->rate     = rate;
        info->ready_to_play = true;
        info->playback_buffer_empty = be;
        info->playback_buffer_full  = bf;
    }
}

static float cb_on_video_playlist_remove(void *cls) {
    return 0.0f;
}

} /* extern "C" */

/*============================================================================
 * GLib MainLoop callbacks
 *============================================================================*/

static gboolean feedback_callback(gpointer loop) {
    if (!g_inst) return FALSE;
    if (g_inst->open_connections > 0) {
        g_inst->missed_feedback++;
        if (g_inst->missed_feedback > MISSED_FEEDBACK_LIMIT) {
            LOGI("no client heartbeat for > %d seconds: resetting", MISSED_FEEDBACK_LIMIT);
            g_inst->reset_httpd = true;
            g_inst->relaunch_video = true;
            g_inst->reset_loop = true;
        }
    }
    return TRUE;
}

static gboolean reset_callback(gpointer loop) {
    if (!g_inst) return FALSE;
    if (g_inst->reset_loop) {
        g_inst->reset_loop = false;
        g_main_loop_quit((GMainLoop *)loop);
    }
    return TRUE;
}

static gboolean progress_callback(gpointer loop) {
    /* progress reporting - simplified */
    return TRUE;
}

/*============================================================================
 * Internal server lifecycle (adapted from uxplay.cpp)
 *============================================================================*/

static int start_dnssd_internal(uxplay_instance_s *inst, const std::vector<char> &hw_addr,
                                 const std::string &name) {
    int err;
    unsigned char pin_pw = 0;
    if (inst->cfg.access_control == UXPLAY_ACCESS_PIN) pin_pw = 1;
    else if (inst->cfg.access_control == UXPLAY_ACCESS_PASSWORD) pin_pw = 2;

    inst->dnssd = dnssd_init(name.c_str(), (int)name.length(),
                              hw_addr.data(), (int)hw_addr.size(), &err, pin_pw);
    if (err) {
        LOGE("Could not initialize dnssd library: error %d", err);
        return -1;
    }

    /* Set standard AirPlay features */
    dnssd_set_airplay_features(inst->dnssd, 0, 0);
    dnssd_set_airplay_features(inst->dnssd, 1, 1);
    dnssd_set_airplay_features(inst->dnssd, 2, 1);
    dnssd_set_airplay_features(inst->dnssd, 3, 0);
    dnssd_set_airplay_features(inst->dnssd, 4, 0);
    dnssd_set_airplay_features(inst->dnssd, 5, 1);
    dnssd_set_airplay_features(inst->dnssd, 6, 1);
    dnssd_set_airplay_features(inst->dnssd, 7, 1);
    dnssd_set_airplay_features(inst->dnssd, 8, 0);
    dnssd_set_airplay_features(inst->dnssd, 9, 1);
    dnssd_set_airplay_features(inst->dnssd, 10, 1);
    dnssd_set_airplay_features(inst->dnssd, 11, 1);
    dnssd_set_airplay_features(inst->dnssd, 12, 1);
    dnssd_set_airplay_features(inst->dnssd, 13, 1);
    dnssd_set_airplay_features(inst->dnssd, 14, 1);
    dnssd_set_airplay_features(inst->dnssd, 15, 1);
    dnssd_set_airplay_features(inst->dnssd, 16, 1);
    dnssd_set_airplay_features(inst->dnssd, 17, 1);
    dnssd_set_airplay_features(inst->dnssd, 18, 1);
    dnssd_set_airplay_features(inst->dnssd, 19, 0);
    dnssd_set_airplay_features(inst->dnssd, 20, 1);
    dnssd_set_airplay_features(inst->dnssd, 22, 1);
    dnssd_set_airplay_features(inst->dnssd, 23, 1);
    dnssd_set_airplay_features(inst->dnssd, 25, 1);
    dnssd_set_airplay_features(inst->dnssd, 26, 1);
    dnssd_set_airplay_features(inst->dnssd, 27, 1);
    dnssd_set_airplay_features(inst->dnssd, 30, 1);

    if (inst->cfg.h265_support) {
        dnssd_set_airplay_features(inst->dnssd, 0, 1);
    }

    return 0;
}

static int register_dnssd_internal(uxplay_instance_s *inst) {
    int err = dnssd_register_raop(inst->dnssd, inst->raop_port);
    if (err) {
        LOGE("dnssd_register_raop failed: %d", err);
        return -1;
    }
    err = dnssd_register_airplay(inst->dnssd, inst->airplay_port);
    if (err) {
        LOGE("dnssd_register_airplay failed: %d", err);
        return -1;
    }
    return 0;
}

static void stop_dnssd_internal(uxplay_instance_s *inst) {
    if (inst->dnssd) {
        dnssd_unregister_raop(inst->dnssd);
        dnssd_unregister_airplay(inst->dnssd);
        dnssd_destroy(inst->dnssd);
        inst->dnssd = nullptr;
    }
}

static int start_raop_server_internal(uxplay_instance_s *inst) {
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));

    cbs.cls                           = inst;
    cbs.conn_init                     = cb_conn_init;
    cbs.conn_destroy                  = cb_conn_destroy;
    cbs.conn_reset                    = cb_conn_reset;
    cbs.conn_feedback                 = cb_conn_feedback;
    cbs.audio_process                 = cb_audio_process;
    cbs.video_process                 = cb_video_process;
    cbs.audio_flush                   = cb_audio_flush;
    cbs.video_flush                   = cb_video_flush;
    cbs.video_pause                   = cb_video_pause;
    cbs.video_resume                  = cb_video_resume;
    cbs.audio_set_client_volume       = cb_audio_set_client_volume;
    cbs.audio_set_volume              = cb_audio_set_volume;
    cbs.audio_get_format              = cb_audio_get_format;
    cbs.video_report_size             = cb_video_report_size;
    cbs.audio_set_metadata            = cb_audio_set_metadata;
    cbs.audio_set_coverart            = cb_audio_set_coverart;
    cbs.audio_stop_coverart_rendering = cb_audio_stop_coverart_rendering;
    cbs.audio_set_progress            = cb_audio_set_progress;
    cbs.report_client_request         = cb_report_client_request;
    cbs.display_pin                   = cb_display_pin;
    cbs.register_client               = cb_register_client;
    cbs.check_register                = cb_check_register;
    cbs.passwd                        = cb_passwd;
    cbs.export_dacp                   = cb_export_dacp;
    cbs.video_reset                   = cb_video_reset;
    cbs.video_set_codec               = cb_video_set_codec;
    cbs.mirror_video_running          = cb_mirror_video_running;
    cbs.on_video_play                 = cb_on_video_play;
    cbs.on_video_scrub                = cb_on_video_scrub;
    cbs.on_video_rate                 = cb_on_video_rate;
    cbs.on_video_stop                 = cb_on_video_stop;
    cbs.on_video_playlist_remove      = cb_on_video_playlist_remove;
    cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;

    inst->raop = raop_init(&cbs);
    if (!inst->raop) {
        LOGE("Error initializing raop!");
        return -1;
    }

    int log_level = LOGGER_INFO;
    if (inst->cfg.log_level <= UXPLAY_LOG_DEBUG) log_level = LOGGER_DEBUG;
    raop_set_log_callback(inst->raop, lib_log_callback, NULL);
    raop_set_log_level(inst->raop, log_level);

    int nohold = inst->cfg.nohold ? 1 : 0;
    const char *keyfile = inst->str_keyfile.empty() ? "" : inst->str_keyfile.c_str();
    if (raop_init2(inst->raop, nohold, inst->str_mac_address.c_str(), keyfile)) {
        LOGE("Error initializing raop (2)!");
        free(inst->raop);
        inst->raop = nullptr;
        return -1;
    }

    /* Set display parameters */
    if (inst->display[0]) raop_set_plist(inst->raop, "width", (int)inst->display[0]);
    if (inst->display[1]) raop_set_plist(inst->raop, "height", (int)inst->display[1]);
    if (inst->display[2]) raop_set_plist(inst->raop, "refreshRate", (int)inst->display[2]);
    if (inst->display[3]) raop_set_plist(inst->raop, "maxFPS", (int)inst->display[3]);

    if (inst->cfg.hls_support) raop_set_plist(inst->raop, "hls", 1);

    /* Network port selection */
    raop_set_tcp_ports(inst->raop, inst->tcp_ports);
    raop_set_udp_ports(inst->raop, inst->udp_ports);

    inst->raop_port = raop_get_port(inst->raop);
    raop_start_httpd(inst->raop, &inst->raop_port);
    raop_set_port(inst->raop, inst->raop_port);
    inst->airplay_port = inst->raop_port;

    if (inst->dnssd) {
        raop_set_dnssd(inst->raop, inst->dnssd);
    } else {
        LOGE("raop_set failed: dnssd not initialized");
        return -2;
    }

    if (!inst->str_lang.empty()) {
        raop_set_lang(inst->raop, inst->str_lang.c_str());
    }

    return 0;
}

static void stop_raop_server_internal(uxplay_instance_s *inst) {
    if (inst->raop) {
        raop_destroy(inst->raop);
        inst->raop = nullptr;
    }
}

/*============================================================================
 * Internal main loop (runs in background thread)
 *============================================================================*/

static void run_main_loop(uxplay_instance_s *inst) {
    guint gst_video_bus_watch_id[MAX_VIDEO_RENDERERS] = {0};
    guint gst_audio_bus_watch_id[MAX_AUDIO_RENDERERS] = {0};

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    inst->main_loop = loop;

    inst->relaunch_video = false;
    inst->monitor_progress = false;
    inst->reset_loop = false;
    inst->reset_httpd = false;
    inst->preserve_connections = false;
    inst->n_video_renderers = 0;
    inst->n_audio_renderers = 0;

    if (inst->cfg.use_video) {
        inst->n_video_renderers = 1;
        inst->relaunch_video = true;
        if (inst->cfg.h265_support) inst->n_video_renderers++;
        if (inst->cfg.coverart_display) inst->n_video_renderers++;
        g_assert(inst->n_video_renderers <= MAX_VIDEO_RENDERERS);
        for (int i = 0; i < inst->n_video_renderers; i++) {
            gst_video_bus_watch_id[i] = (guint)video_renderer_listen((void*)loop, i);
        }
    }

    if (inst->cfg.use_audio) {
        inst->rtptime_start = 0;
        inst->rtptime_end = 0;
        inst->monitor_progress = true;
        inst->n_audio_renderers = 2;
        g_assert(inst->n_audio_renderers <= MAX_AUDIO_RENDERERS);
        for (int i = 0; i < inst->n_audio_renderers; i++) {
            gst_audio_bus_watch_id[i] = (guint)audio_renderer_listen((void*)loop, i);
        }
    }

    inst->missed_feedback = 0;
    guint feedback_id = g_timeout_add_seconds(1, (GSourceFunc)feedback_callback, (gpointer)loop);
    guint reset_id    = g_timeout_add(100, (GSourceFunc)reset_callback, (gpointer)loop);

    g_main_loop_run(loop);

    /* Cleanup GLib sources */
    for (int i = 0; i < inst->n_video_renderers; i++) {
        if (gst_video_bus_watch_id[i] > 0) g_source_remove(gst_video_bus_watch_id[i]);
    }
    for (int i = 0; i < inst->n_audio_renderers; i++) {
        if (gst_audio_bus_watch_id[i] > 0) g_source_remove(gst_audio_bus_watch_id[i]);
    }
    if (reset_id > 0) g_source_remove(reset_id);
    if (feedback_id > 0) g_source_remove(feedback_id);

    g_main_loop_unref(loop);
    inst->main_loop = nullptr;
}

static void server_thread_func(uxplay_instance_s *inst) {
    emit_state_change(inst, UXPLAY_STATE_RUNNING);

reconnect:
    inst->compression_type = 0;
    inst->close_window = true;

    run_main_loop(inst);

    if (inst->relaunch_video && inst->state.load() == UXPLAY_STATE_RUNNING) {
        if (inst->reset_httpd) {
            raop_stop_httpd(inst->raop);
        }
        if (inst->cfg.use_audio) {
            audio_renderer_stop();
        }
        if (inst->cfg.use_video && (inst->close_window || inst->preserve_connections || inst->full_video_reset)) {
            video_renderer_destroy();
            if (!inst->preserve_connections) {
                raop_remove_known_connections(inst->raop);
            }
            video_renderer_init(inst->render_logger, inst->str_server_name.c_str(),
                                inst->videoflip, inst->str_video_parser.c_str(), "",
                                inst->str_video_decoder.c_str(), inst->str_video_converter.c_str(),
                                inst->str_videosink.c_str(), inst->str_videosink_options.c_str(),
                                inst->cfg.fullscreen, inst->cfg.video_sync,
                                inst->cfg.h265_support, inst->cfg.coverart_display, 3, NULL);
            inst->full_video_reset = false;
            video_renderer_start();
        }
        if (inst->reset_httpd) {
            unsigned short port = raop_get_port(inst->raop);
            raop_start_httpd(inst->raop, &port);
            raop_set_port(inst->raop, port);
        }
        goto reconnect;
    }

    /* Normal shutdown path */
    LOGI("Stopping RAOP Server...");
    stop_raop_server_internal(inst);
    stop_dnssd_internal(inst);

    /* Cleanup renderers */
    if (inst->cfg.use_audio) audio_renderer_destroy();
    if (inst->cfg.use_video) video_renderer_destroy();
    if (inst->render_logger) {
        logger_destroy(inst->render_logger);
        inst->render_logger = nullptr;
    }

    emit_state_change(inst, UXPLAY_STATE_IDLE);
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

UXPLAY_API uxplay_config_t uxplay_default_config(void) {
    uxplay_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.server_name     = DEFAULT_NAME;
    cfg.append_hostname = true;
    cfg.width           = 1920;
    cfg.height          = 1080;
    cfg.refresh_rate    = 60;
    cfg.max_fps         = 30;

    /* Video defaults */
    cfg.videosink       = "autovideosink";
    cfg.video_decoder   = "decodebin";
    cfg.video_converter = "videoconvert";
    cfg.video_parser    = "h264parse";
    cfg.videoflip       = UXPLAY_FLIP_NONE;
    cfg.fullscreen      = false;
    cfg.h265_support    = false;
    cfg.video_sync      = true;
    cfg.bt709_fix       = false;
    cfg.use_video       = true;
    cfg.nofreeze        = false;

    /* Audio defaults */
    cfg.audiosink       = "autoaudiosink";
    cfg.audio_sync      = false;
    cfg.use_audio       = true;
    cfg.initial_volume  = 0.0;
    cfg.db_low          = -30.0;
    cfg.db_high         = 0.0;

    /* Security */
    cfg.access_control  = UXPLAY_ACCESS_FREE;

    /* Misc */
    cfg.log_level       = UXPLAY_LOG_INFO;
    cfg.nohold          = true;

    return cfg;
}

UXPLAY_API const char* uxplay_version(void) {
    return UXPLAYLIB_VERSION_STRING " (based on UxPlay " LIB_VERSION ")";
}

UXPLAY_API const char* uxplay_error_string(uxplay_error_t error) {
    switch (error) {
        case UXPLAY_OK:                   return "Success";
        case UXPLAY_ERR_INVALID_ARGUMENT: return "Invalid argument";
        case UXPLAY_ERR_ALREADY_RUNNING:  return "Server is already running";
        case UXPLAY_ERR_NOT_RUNNING:      return "Server is not running";
        case UXPLAY_ERR_GSTREAMER_INIT:   return "GStreamer initialization failed";
        case UXPLAY_ERR_RAOP_INIT:        return "RAOP server initialization failed";
        case UXPLAY_ERR_DNSSD_INIT:       return "DNS-SD service discovery initialization failed";
        case UXPLAY_ERR_NETWORK:          return "Network error";
        case UXPLAY_ERR_OUT_OF_MEMORY:    return "Out of memory";
        case UXPLAY_ERR_INTERNAL:         return "Internal error";
        default:                          return "Unknown error";
    }
}

UXPLAY_API uxplay_error_t uxplay_create(uxplay_t *out_handle) {
    if (!out_handle) return UXPLAY_ERR_INVALID_ARGUMENT;

    auto *inst = new (std::nothrow) uxplay_instance_s();
    if (!inst) return UXPLAY_ERR_OUT_OF_MEMORY;

    /* Apply defaults */
    inst->cfg = uxplay_default_config();

    *out_handle = inst;
    return UXPLAY_OK;
}

UXPLAY_API uxplay_error_t uxplay_configure(uxplay_t handle, const uxplay_config_t *config) {
    if (!handle || !config) return UXPLAY_ERR_INVALID_ARGUMENT;
    auto *inst = handle;

    std::lock_guard<std::mutex> lock(inst->mutex);
    if (inst->state.load() != UXPLAY_STATE_IDLE) return UXPLAY_ERR_ALREADY_RUNNING;

    /* Deep-copy strings */
    inst->str_server_name      = config->server_name ? config->server_name : DEFAULT_NAME;
    inst->str_mac_address      = config->mac_address ? config->mac_address : "";
    inst->str_videosink        = config->videosink ? config->videosink : "autovideosink";
    inst->str_videosink_options= config->videosink_options ? config->videosink_options : "";
    inst->str_video_decoder    = config->video_decoder ? config->video_decoder : "decodebin";
    inst->str_video_converter  = config->video_converter ? config->video_converter : "videoconvert";
    inst->str_video_parser     = config->video_parser ? config->video_parser : "h264parse";
    inst->str_audiosink        = config->audiosink ? config->audiosink : "autoaudiosink";
    inst->str_password         = config->password ? config->password : "";
    inst->str_keyfile          = config->keyfile ? config->keyfile : "";
    inst->str_coverart_filename= config->coverart_filename ? config->coverart_filename : "";
    inst->str_lang             = config->lang ? config->lang : "";

    /* Copy config and fix up internal pointers */
    inst->cfg = *config;
    inst->cfg.server_name      = inst->str_server_name.c_str();
    inst->cfg.mac_address      = inst->str_mac_address.c_str();
    inst->cfg.videosink        = inst->str_videosink.c_str();
    inst->cfg.videosink_options= inst->str_videosink_options.c_str();
    inst->cfg.video_decoder    = inst->str_video_decoder.c_str();
    inst->cfg.video_converter  = inst->str_video_converter.c_str();
    inst->cfg.video_parser     = inst->str_video_parser.c_str();
    inst->cfg.audiosink        = inst->str_audiosink.c_str();
    inst->cfg.password         = inst->str_password.c_str();
    inst->cfg.keyfile          = inst->str_keyfile.c_str();
    inst->cfg.coverart_filename= inst->str_coverart_filename.c_str();
    inst->cfg.lang             = inst->str_lang.c_str();

    /* Map videoflip */
    inst->videoflip[0] = (videoflip_t)config->videoflip;
    inst->videoflip[1] = NONE;

    /* Copy port arrays */
    memcpy(inst->tcp_ports, config->tcp_ports, sizeof(inst->tcp_ports));
    memcpy(inst->udp_ports, config->udp_ports, sizeof(inst->udp_ports));

    /* Display parameters */
    inst->display[0] = config->width  ? config->width  : 1920;
    inst->display[1] = config->height ? config->height : 1080;
    inst->display[2] = config->refresh_rate ? config->refresh_rate : 60;
    inst->display[3] = config->max_fps ? config->max_fps : 30;
    inst->display[4] = 0;

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
    auto *inst = handle;

    std::lock_guard<std::mutex> lock(inst->mutex);
    if (inst->state.load() != UXPLAY_STATE_IDLE) return UXPLAY_ERR_ALREADY_RUNNING;

    g_inst = inst;
    emit_state_change(inst, UXPLAY_STATE_STARTING);

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    LOGI("UxPlayLib %s starting...", uxplay_version());

    /* --- Prepare video sink options for Windows --- */
#ifdef _WIN32
    /* Default to d3d11videosink on Windows if user chose autovideosink */
    if (inst->str_videosink == "autovideosink") {
        /* autovideosink is fine on Windows with GStreamer */
    }
    /* Apply fullscreen toggle mode for d3d11videosink */
    if (inst->str_videosink == "d3d11videosink" && inst->str_videosink_options.empty()) {
        if (inst->cfg.fullscreen) {
            inst->str_videosink_options =
                " fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_PROPERTY fullscreen=TRUE ";
        } else {
            inst->str_videosink_options =
                " fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER ";
        }
        inst->cfg.videosink_options = inst->str_videosink_options.c_str();
    }
#endif

    /* Apply BT.709 fix if requested */
    if (inst->cfg.bt709_fix && inst->cfg.use_video) {
        inst->str_video_parser += " ! capssetter caps=\"video/x-h264, colorimetry=bt709\"";
        inst->cfg.video_parser = inst->str_video_parser.c_str();
    }

    /* Append hostname if requested */
    if (inst->cfg.append_hostname) {
        append_hostname(inst->str_server_name);
        inst->cfg.server_name = inst->str_server_name.c_str();
    }

    /* Initialize GStreamer */
    if (!gstreamer_init()) {
        LOGE("GStreamer initialization failed");
        emit_state_change(inst, UXPLAY_STATE_ERROR);
        g_inst = nullptr;
        return UXPLAY_ERR_GSTREAMER_INIT;
    }

    /* Initialize render logger */
    inst->render_logger = logger_init();
    int log_level = (inst->cfg.log_level <= UXPLAY_LOG_DEBUG) ? LOGGER_DEBUG : LOGGER_INFO;
    logger_set_callback(inst->render_logger, lib_log_callback, NULL);
    logger_set_level(inst->render_logger, log_level);

    /* Initialize audio renderer */
    if (inst->cfg.use_audio) {
        audio_renderer_init(inst->render_logger, inst->str_audiosink.c_str(),
                            &inst->cfg.audio_sync, &inst->cfg.video_sync, "");
    }

    /* Initialize video renderer */
    if (inst->cfg.use_video) {
        video_renderer_init(inst->render_logger, inst->str_server_name.c_str(),
                            inst->videoflip, inst->str_video_parser.c_str(), "",
                            inst->str_video_decoder.c_str(), inst->str_video_converter.c_str(),
                            inst->str_videosink.c_str(), inst->str_videosink_options.c_str(),
                            inst->cfg.fullscreen, inst->cfg.video_sync,
                            inst->cfg.h265_support, inst->cfg.coverart_display, 3, NULL);
        video_renderer_start();
    }

    /* Determine MAC address */
    if (inst->str_mac_address.empty()) {
        inst->str_mac_address = find_mac_address();
        if (inst->str_mac_address.empty()) {
            inst->str_mac_address = random_mac_address();
            LOGI("Using random MAC: %s", inst->str_mac_address.c_str());
        } else {
            LOGI("Using system MAC: %s", inst->str_mac_address.c_str());
        }
    }

    /* Parse MAC to hw_addr vector */
    std::vector<char> hw_addr;
    parse_hw_addr(inst->str_mac_address, hw_addr);

    /* Set default display resolution */
    if (!inst->display[0] && !inst->display[1]) {
        if (inst->cfg.h265_support) {
            inst->display[0] = 3840;
            inst->display[1] = 2160;
        } else {
            inst->display[0] = 1920;
            inst->display[1] = 1080;
        }
    }

    /* Start DNS-SD */
    if (start_dnssd_internal(inst, hw_addr, inst->str_server_name) != 0) {
        LOGE("Failed to initialize DNS-SD");
        if (inst->cfg.use_audio) audio_renderer_destroy();
        if (inst->cfg.use_video) video_renderer_destroy();
        logger_destroy(inst->render_logger);
        inst->render_logger = nullptr;
        emit_state_change(inst, UXPLAY_STATE_ERROR);
        g_inst = nullptr;
        return UXPLAY_ERR_DNSSD_INIT;
    }

    /* Start RAOP server */
    if (start_raop_server_internal(inst) != 0) {
        LOGE("Failed to start RAOP server");
        stop_dnssd_internal(inst);
        if (inst->cfg.use_audio) audio_renderer_destroy();
        if (inst->cfg.use_video) video_renderer_destroy();
        logger_destroy(inst->render_logger);
        inst->render_logger = nullptr;
        emit_state_change(inst, UXPLAY_STATE_ERROR);
        g_inst = nullptr;
        return UXPLAY_ERR_RAOP_INIT;
    }

    /* Register DNS-SD */
    if (register_dnssd_internal(inst) != 0) {
        LOGE("Failed to register DNS-SD services");
        stop_raop_server_internal(inst);
        stop_dnssd_internal(inst);
        if (inst->cfg.use_audio) audio_renderer_destroy();
        if (inst->cfg.use_video) video_renderer_destroy();
        logger_destroy(inst->render_logger);
        inst->render_logger = nullptr;
        emit_state_change(inst, UXPLAY_STATE_ERROR);
        g_inst = nullptr;
        return UXPLAY_ERR_DNSSD_INIT;
    }

    LOGI("AirPlay server '%s' started on port %u", inst->str_server_name.c_str(), inst->raop_port);

    /* Launch the server main loop in a background thread */
    inst->server_thread = std::thread(server_thread_func, inst);

    return UXPLAY_OK;
}

UXPLAY_API uxplay_error_t uxplay_stop(uxplay_t handle) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    auto *inst = handle;

    uxplay_state_t current = inst->state.load();
    if (current != UXPLAY_STATE_RUNNING && current != UXPLAY_STATE_STARTING) {
        return UXPLAY_ERR_NOT_RUNNING;
    }

    emit_state_change(inst, UXPLAY_STATE_STOPPING);

    /* Signal the main loop to quit */
    inst->relaunch_video = false;
    inst->reset_loop = true;

    if (inst->main_loop) {
        g_main_loop_quit(inst->main_loop);
    }

    /* Wait for thread to finish */
    if (inst->server_thread.joinable()) {
        inst->server_thread.join();
    }

    g_inst = nullptr;
    return UXPLAY_OK;
}

UXPLAY_API void uxplay_destroy(uxplay_t handle) {
    if (!handle) return;

    /* Stop if running */
    if (handle->state.load() == UXPLAY_STATE_RUNNING ||
        handle->state.load() == UXPLAY_STATE_STARTING) {
        uxplay_stop(handle);
    }

    delete handle;
}

UXPLAY_API uxplay_state_t uxplay_get_state(uxplay_t handle) {
    if (!handle) return UXPLAY_STATE_IDLE;
    return handle->state.load();
}

UXPLAY_API uxplay_error_t uxplay_set_volume(uxplay_t handle, double volume) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    if (handle->state.load() != UXPLAY_STATE_RUNNING) return UXPLAY_ERR_NOT_RUNNING;

    /* Convert [0,1] linear to GStreamer volume */
    if (volume <= 0.0) {
        audio_renderer_set_volume(0.0);
    } else if (volume >= 1.0) {
        audio_renderer_set_volume(1.0);
    } else {
        double db = handle->cfg.db_low + (handle->cfg.db_high - handle->cfg.db_low) * volume;
        double gst_vol = pow(10.0, 0.05 * db);
        audio_renderer_set_volume(gst_vol);
    }
    return UXPLAY_OK;
}

UXPLAY_API int uxplay_get_connection_count(uxplay_t handle) {
    if (!handle) return 0;
    return (int)handle->open_connections;
}

UXPLAY_API uxplay_error_t uxplay_disconnect_clients(uxplay_t handle) {
    if (!handle) return UXPLAY_ERR_INVALID_ARGUMENT;
    if (handle->state.load() != UXPLAY_STATE_RUNNING) return UXPLAY_ERR_NOT_RUNNING;

    /* Trigger a reset which disconnects clients */
    handle->reset_httpd = true;
    handle->relaunch_video = true;
    handle->reset_loop = true;

    return UXPLAY_OK;
}

/*============================================================================
 * DllMain for Windows
 *============================================================================*/

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            break;
        }
        case DLL_PROCESS_DETACH:
            WSACleanup();
            break;
    }
    return TRUE;
}
#endif
