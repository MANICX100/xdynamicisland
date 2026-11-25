#ifndef XDI_MEDIA_H
#define XDI_MEDIA_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdint.h>

#define XDI_MAX_PLAYERS 12

struct xdi_mpv_s;

typedef enum {
    XDI_PLAYBACK_STOPPED = 0,
    XDI_PLAYBACK_PAUSED,
    XDI_PLAYBACK_PLAYING
} xdi_playback_status_t;

typedef struct {
    char bus_name[128];
    char identity[64];
    char title[256];
    char artist[256];
    char album[128];
    char art_url[512];
    xdi_playback_status_t status;
    int64_t length_us;
    int64_t position_us;
    int64_t pos_timestamp_ms; /* Monotonic ms when position_us was sampled */
    bool is_kdeconnect;
    bool is_mpv_native;
    bool can_seek;
    bool can_control;
} xdi_player_t;

typedef struct {
    double volume;       /* 0.0 .. 1.5 */
    bool muted;
    bool available;
} xdi_volume_t;

typedef struct {
    DBusConnection *conn;
    struct xdi_mpv_s *mpv; /* Optional native mpv backend (IPC + stock X11) */
    xdi_player_t players[XDI_MAX_PLAYERS];
    int player_count;
    int active_index;
    bool manual_selection;
    xdi_volume_t volume;
    bool state_changed;
    bool volume_changed;
    int64_t last_vol_poll_ms;
    int64_t last_pos_poll_ms;
    char kde_device_name[64];
    int kde_battery;     /* -1 if unknown */
} xdi_media_t;

/* Initialize MPRIS2 D-Bus connection + initial wpctl volume query. */
bool xdi_media_init(xdi_media_t *m);

/* Attach optional native mpv backend (handles mpv without mpv-mpris). */
void xdi_media_attach_mpv(xdi_media_t *m, struct xdi_mpv_s *mpv);

/* Shutdown and release resources. */
void xdi_media_destroy(xdi_media_t *m);

/* Get file descriptor for poll() integration (-1 if unavailable). */
int xdi_media_get_fd(xdi_media_t *m);

/* Dispatch pending D-Bus messages, native mpv updates, and periodic volume/position refresh. */
void xdi_media_poll(xdi_media_t *m, int64_t now_ms);

/* Return currently active player (or NULL if none). */
const xdi_player_t *xdi_media_active(const xdi_media_t *m);

/* Compute estimated live playback position in microseconds. */
int64_t xdi_media_current_position_us(const xdi_player_t *p, int64_t now_ms);

/* Media controls on the active player (MPRIS2, KDE Connect, or native mpv). */
void xdi_media_play_pause(xdi_media_t *m);
void xdi_media_next(xdi_media_t *m);
void xdi_media_prev(xdi_media_t *m);
void xdi_media_seek_fraction(xdi_media_t *m, double frac);

/* Cycle between detected players (e.g. Chromium <-> mpv <-> KDE Connect). */
void xdi_media_cycle_player(xdi_media_t *m, int delta);

/* Volume control via wpctl (@DEFAULT_AUDIO_SINK@) with PipeWire/WirePlumber. */
void xdi_media_volume_delta(xdi_media_t *m, int delta_percent);
void xdi_media_volume_set(xdi_media_t *m, double fraction);
void xdi_media_toggle_mute(xdi_media_t *m);

/* Pure helper functions (exposed for unit testing). */
bool xdi_parse_wpctl_volume(const char *output, double *out_vol, bool *out_muted);
int xdi_select_best_player(const xdi_player_t *players, int count, int current_idx, bool manual);

#endif /* XDI_MEDIA_H */
