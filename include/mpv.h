#ifndef XDI_MPV_H
#define XDI_MPV_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "media.h"

/*
 * Native mpv integration supporting:
 *   1. mpv JSON IPC socket (--input-ipc-server / standard socket paths)
 *      -> zero-poll event-driven observe_property (exact title, artist, album,
 *         pause state, duration, seek, play/pause, next, prev + 3s time-pos sync).
 *   2. Zero-config stock mpv on X11 (no mpv-mpris, no input-ipc-server)
 *      -> detects mpv X11 window via _NET_CLIENT_LIST + WM_CLASS("mpv"),
 *         reads media title from _NET_WM_NAME, infers play/pause via lightweight
 *         /proc/<pid>/stat CPU activity delta, and controls play/pause/next/prev
 *         via X11 XSendEvent (space / > / <).
 */
typedef struct xdi_mpv_s {
    Display *dpy;
    Window root;
    Atom atom_client_list;
    Atom atom_wm_name;
    Atom atom_utf8;
    Atom atom_wm_pid;

    /* Active mpv state */
    bool active;
    bool using_ipc;
    xdi_player_t player;
    bool state_changed;

    /* JSON IPC socket state */
    int ipc_fd;
    char ipc_path[256];
    char rx_buf[4096];
    size_t rx_len;
    int64_t last_pos_req_ms;

    /* Stock X11 fallback state */
    Window mpv_win;
    pid_t mpv_pid;
    uint64_t prev_cpu_ticks;
    int zero_tick_streak;
    int64_t manual_toggle_until_ms;
    int64_t last_scan_ms;
} xdi_mpv_t;

void xdi_mpv_init(xdi_mpv_t *mpv, Display *dpy, Window root);
void xdi_mpv_destroy(xdi_mpv_t *mpv);

/* Pollable fd for mpv JSON IPC socket (-1 if not connected). */
int xdi_mpv_get_fd(const xdi_mpv_t *mpv);

/* Poll IPC events and periodic stock-mpv window discovery.
 * If `mpris_has_mpv` is true, native backend stays quiet to avoid duplicates. */
void xdi_mpv_poll(xdi_mpv_t *mpv, int64_t now_ms, bool mpris_has_mpv);

/* Playback controls */
void xdi_mpv_play_pause(xdi_mpv_t *mpv, int64_t now_ms);
void xdi_mpv_next(xdi_mpv_t *mpv);
void xdi_mpv_prev(xdi_mpv_t *mpv);
void xdi_mpv_seek_fraction(xdi_mpv_t *mpv, double frac, int64_t now_ms);

/* Pure parsing helpers exposed for unit testing */
void xdi_mpv_clean_window_title(const char *raw_title, char *out, size_t out_len);
bool xdi_mpv_extract_ipc_arg(const char *cmdline, size_t cmdline_len, char *out_path, size_t out_len);
bool xdi_mpv_parse_ipc_line(xdi_player_t *p, const char *line, int64_t now_ms);

#endif /* XDI_MPV_H */
