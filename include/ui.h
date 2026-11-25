#ifndef XDI_UI_H
#define XDI_UI_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>

#include "art.h"
#include "audio_viz.h"
#include "config.h"
#include "media.h"
#include "notifications.h"
#include "obstacles.h"
#include "spring.h"

typedef enum {
    XDI_MODE_HIDDEN = 0,
    XDI_MODE_IDLE,
    XDI_MODE_COMPACT_MEDIA,
    XDI_MODE_EXPANDED_MEDIA,
    XDI_MODE_TOAST_NOTIF,
    XDI_MODE_TOAST_VOLUME,
    XDI_MODE_NOTIF_HISTORY
} xdi_mode_t;

typedef enum {
    XDI_HIT_NONE = 0,
    XDI_HIT_PREV,
    XDI_HIT_PLAY_PAUSE,
    XDI_HIT_NEXT,
    XDI_HIT_SEEK_BAR,
    XDI_HIT_VOLUME_MUTE,
    XDI_HIT_VOLUME_BAR,
    XDI_HIT_NOTIF_BTN,
    XDI_HIT_SETTINGS_BTN,
    XDI_HIT_PLAYER_PREV,
    XDI_HIT_PLAYER_NEXT,
    XDI_HIT_CLEAR_NOTIFS,
    XDI_HIT_CLOSE_NOTIFS,
    XDI_HIT_TOAST_ACTION_BASE = 60,   /* 60 + action_idx */
    XDI_HIT_NOTIF_DISMISS_BASE = 100, /* 100 + row_idx */
    XDI_HIT_NOTIF_ACTION_BASE = 200   /* 200 + row_idx * XDI_NOTIF_MAX_ACTIONS + action_idx */
} xdi_hit_id_t;

typedef struct {
    double x, y, w, h;
    int id;
} xdi_hit_rect_t;

#define XDI_MAX_HIT_RECTS 96

typedef struct {
    Display *dpy;
    int screen;
    Window root;
    Window win;
    Visual *visual;
    Colormap colormap;
    int depth;
    bool composited;
    bool mapped;
    bool window_is_override;
    Atom atom_cm;
    Atom atom_wm_type;
    Atom atom_wm_state;
    Atom atom_state_above;
    Atom atom_state_sticky;
    Atom atom_state_skip_taskbar;
    Atom atom_state_skip_pager;
    Atom atom_motif_hints;
    Atom atom_net_active;
    Atom atom_net_clients;
    Atom atom_net_stacking;
    Atom atom_net_workarea;

    /* Monitor geometry */
    xdi_rect_t monitor;

    /* Double-buffered Cairo rendering */
    int surf_w;
    int surf_h;
    int win_x;
    int win_y;
    int shape_w;
    int shape_h;
    int shape_r;
    cairo_surface_t *xlib_surf;
    cairo_surface_t *back_surf;

    /* Animation springs */
    xdi_spring_t sp_w;
    xdi_spring_t sp_h;
    xdi_spring_t sp_r;
    xdi_spring_t sp_x;
    xdi_spring_t sp_y;
    xdi_spring_t sp_expand;    /* 0 = compact, 1 = expanded */
    double art_angle;          /* Vinyl rotation radians */

    /* UI State */
    xdi_mode_t mode;
    bool hovered;
    bool pinned_open;
    int hover_hit_id;
    int64_t hover_leave_ms;
    int64_t toast_until_ms;
    int64_t last_frame_ms;
    int64_t last_render_ms;
    int64_t last_obstacle_check_ms;
    bool obstacles_dirty;
    bool needs_redraw;
    bool open_settings_requested;

    /* Notification history scroll state */
    double notif_scroll;
    int notif_scroll_max;

    /* Obstacle tracking */
    xdi_obstacle_result_t obstacles;

    /* Interactive hit regions built during draw */
    xdi_hit_rect_t hits[XDI_MAX_HIT_RECTS];
    int hit_count;

    /* Subsystems (borrowed pointers) */
    xdi_config_t *cfg;
    xdi_media_t *media;
    xdi_notif_store_t *notifs;
    xdi_audio_viz_t *viz;
    xdi_art_loader_t *art;
} xdi_ui_t;

bool xdi_ui_init(xdi_ui_t *ui, Display *dpy, xdi_config_t *cfg,
                 xdi_media_t *media, xdi_notif_store_t *notifs,
                 xdi_audio_viz_t *viz, xdi_art_loader_t *art);

void xdi_ui_destroy(xdi_ui_t *ui);

/* Refresh monitor geometry via XRandR. */
void xdi_ui_update_monitor(xdi_ui_t *ui);

/* Re-apply config changes (always_on_top, position, accent, etc.) live. */
void xdi_ui_apply_config(xdi_ui_t *ui, Window settings_win);

/* Handle a single XEvent if belonging to the island or Root obstacle notifications. */
void xdi_ui_handle_event(xdi_ui_t *ui, const XEvent *ev, int64_t now_ms);

/* Trigger temporary toasts. */
void xdi_ui_show_volume_toast(xdi_ui_t *ui, int64_t now_ms);
void xdi_ui_show_notif_toast(xdi_ui_t *ui, int64_t now_ms);

/* Step state machine, springs, obstacle checks, and redraw if needed.
 * Returns recommended poll timeout in milliseconds (e.g. 16ms animating, 450ms idle). */
int xdi_ui_tick(xdi_ui_t *ui, int64_t now_ms, Window settings_win);

#endif /* XDI_UI_H */
