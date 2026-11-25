#ifndef XDI_OBSTACLES_H
#define XDI_OBSTACLES_H

#include <X11/Xlib.h>
#include <stdbool.h>

typedef struct {
    int x;
    int y;
    int width;
    int height;
} xdi_rect_t;

typedef struct {
    xdi_rect_t rect;
    bool override_redirect;
    bool is_dock;
    bool is_launcher;     /* Matches dmenu, rofi, ulauncher, bemenu, etc. */
    int strut_top;
    int strut_bottom;
    Window win;
} xdi_window_info_t;

typedef struct {
    int top_clearance;    /* Bottom edge (relative to monitor.y) of highest obstructing top window */
    int bottom_clearance; /* Top clearance (relative to monitor bottom) of bottom obstructing window */
    Window top_blocker;   /* Window currently blocking top edge (e.g. dmenu), or None */
    Window bottom_blocker;/* Window currently blocking bottom edge (e.g. dmenu -b), or None */
} xdi_obstacle_result_t;

/* Query all mapped X11 top-level windows and compute clearance for dmenu/rofi/panels. */
xdi_obstacle_result_t xdi_detect_obstacles(Display *dpy, Window root,
                                           Window ignore_win1, Window ignore_win2,
                                           xdi_rect_t monitor,
                                           int island_x, int island_w,
                                           bool avoid_obstacles, bool avoid_panels);

/* Pure geometry solver (exposed for deterministic unit testing without an X server). */
xdi_obstacle_result_t xdi_solve_obstacles(const xdi_window_info_t *wins, int count,
                                          xdi_rect_t monitor,
                                          int island_x, int island_w,
                                          bool avoid_obstacles, bool avoid_panels);

/* Check if a WM_CLASS or window title matches known top launchers (dmenu, rofi, etc.). */
bool xdi_is_launcher_name(const char *name);

#endif /* XDI_OBSTACLES_H */
