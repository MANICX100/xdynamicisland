#include "obstacles.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

bool xdi_is_launcher_name(const char *name) {
    if (!name || !*name) return false;
    static const char *const launchers[] = {
        "dmenu", "rofi", "bemenu", "gmrun", "ulauncher",
        "albert", "kupfer", "tofi", "fuzzel", "jgmenu",
        "xmenu", "skippy", NULL
    };
    for (int i = 0; launchers[i]; i++) {
        if (contains_ci(name, launchers[i])) return true;
    }
    return false;
}

xdi_obstacle_result_t xdi_solve_obstacles(const xdi_window_info_t *wins, int count,
                                          xdi_rect_t monitor,
                                          int island_x, int island_w,
                                          bool avoid_obstacles, bool avoid_panels) {
    xdi_obstacle_result_t res;
    memset(&res, 0, sizeof(res));
    res.top_blocker = None;
    res.bottom_blocker = None;

    if (!wins || count <= 0 || (!avoid_obstacles && !avoid_panels)) {
        return res;
    }

    int isl_left = island_x;
    int isl_right = island_x + island_w;
    int mon_top = monitor.y;
    int mon_bottom = monitor.y + monitor.height;
    int max_band_h = monitor.height / 2;

    for (int i = 0; i < count; i++) {
        const xdi_window_info_t *w = &wins[i];

        /* Ignore tiny invisible 1x1 helper windows and fullscreen windows */
        if (w->rect.width < 40 || w->rect.height < 8) continue;
        if (w->rect.height >= monitor.height - 20 && !w->is_launcher) continue;

        /* Horizontal overlap check with the island's footprint */
        int wx0 = w->rect.x;
        int wx1 = w->rect.x + w->rect.width;
        bool horiz_overlap = (wx1 > isl_left - 8) && (wx0 < isl_right + 8);

        /* 1. Top EWMH strut (panels/bars like polybar, xfce4-panel, icewm top bar) */
        if (avoid_panels && w->strut_top > 0 && w->strut_top < max_band_h) {
            if (w->strut_top > res.top_clearance) {
                res.top_clearance = w->strut_top;
                res.top_blocker = w->win;
            }
        }
        if (avoid_panels && w->strut_bottom > 0 && w->strut_bottom < max_band_h) {
            if (w->strut_bottom > res.bottom_clearance) {
                res.bottom_clearance = w->strut_bottom;
                res.bottom_blocker = w->win;
            }
        }

        if (!horiz_overlap) continue;

        /* 2. Launchers (dmenu, rofi, etc.) & override-redirect top/bottom overlays */
        bool top_candidate = false;
        bool bottom_candidate = false;

        if (avoid_obstacles && (w->is_launcher || w->override_redirect)) {
            /* Top overlay: starts near top of monitor (e.g. dmenu at y == monitor.y) */
            if (w->rect.y >= mon_top - 4 && w->rect.y <= mon_top + 120 &&
                w->rect.height <= max_band_h) {
                /* For unnamed override-redirect windows, require wide bar-like shape
                   so tiny tooltips or cursor popups don't shove the island down. */
                if (w->is_launcher || w->rect.width >= monitor.width / 3) {
                    top_candidate = true;
                }
            }
            /* Bottom overlay (e.g. dmenu -b) */
            int w_bottom = w->rect.y + w->rect.height;
            if (w_bottom >= mon_bottom - 120 && w_bottom <= mon_bottom + 4 &&
                w->rect.height <= max_band_h) {
                if (w->is_launcher || w->rect.width >= monitor.width / 3) {
                    bottom_candidate = true;
                }
            }
        }

        /* 3. EWMH dock panels without struts */
        if (avoid_panels && w->is_dock && w->rect.height <= 120) {
            if (w->rect.y >= mon_top - 4 && w->rect.y <= mon_top + 60) {
                top_candidate = true;
            }
            int w_bottom = w->rect.y + w->rect.height;
            if (w_bottom >= mon_bottom - 60 && w_bottom <= mon_bottom + 4) {
                bottom_candidate = true;
            }
        }

        if (top_candidate) {
            int clearance = (w->rect.y + w->rect.height) - mon_top;
            if (clearance > res.top_clearance) {
                res.top_clearance = clearance;
                res.top_blocker = w->win;
            }
        }
        if (bottom_candidate) {
            int clearance = mon_bottom - w->rect.y;
            if (clearance > res.bottom_clearance) {
                res.bottom_clearance = clearance;
                res.bottom_blocker = w->win;
            }
        }
    }

    return res;
}

/* Temporary silent X11 error handler while probing rapidly-closing transient windows */
static int silent_x_error(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    return 0;
}

static bool window_has_atom(Display *dpy, Window w, Atom prop, Atom target) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    bool found = false;
    if (XGetWindowProperty(dpy, w, prop, 0, 16, False, XA_ATOM,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < nitems; i++) {
            if (atoms[i] == target) {
                found = true;
                break;
            }
        }
        XFree(data);
    }
    return found;
}

static void read_struts(Display *dpy, Window w, Atom atom_strut_partial, Atom atom_strut,
                        xdi_rect_t monitor, int *out_top, int *out_bottom) {
    *out_top = 0;
    *out_bottom = 0;
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, atom_strut_partial, 0, 12, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems >= 12) {
        unsigned long *s = (unsigned long *)data;
        /* s[2]=top, s[8]=top_start_x, s[9]=top_end_x */
        if (s[2] > 0 && (int)s[9] >= monitor.x && (int)s[8] <= monitor.x + monitor.width) {
            int rel = (int)s[2] - monitor.y;
            if (rel > 0) *out_top = rel;
        }
        /* s[3]=bottom, s[10]=bottom_start_x, s[11]=bottom_end_x */
        if (s[3] > 0 && (int)s[11] >= monitor.x && (int)s[10] <= monitor.x + monitor.width) {
            *out_bottom = (int)s[3];
        }
        XFree(data);
        return;
    }
    if (data) XFree(data);

    if (XGetWindowProperty(dpy, w, atom_strut, 0, 4, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems >= 4) {
        unsigned long *s = (unsigned long *)data;
        if (s[2] > 0) *out_top = (int)s[2];
        if (s[3] > 0) *out_bottom = (int)s[3];
        XFree(data);
    }
}

xdi_obstacle_result_t xdi_detect_obstacles(Display *dpy, Window root,
                                           Window ignore_win1, Window ignore_win2,
                                           xdi_rect_t monitor,
                                           int island_x, int island_w,
                                           bool avoid_obstacles, bool avoid_panels) {
    xdi_obstacle_result_t empty;
    memset(&empty, 0, sizeof(empty));
    empty.top_blocker = None;
    empty.bottom_blocker = None;

    if (!dpy || (!avoid_obstacles && !avoid_panels)) return empty;

    int (*old_handler)(Display *, XErrorEvent *) = XSetErrorHandler(silent_x_error);

    Window r_ret = None, p_ret = None;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, root, &r_ret, &p_ret, &children, &nchildren) || !children || nchildren == 0) {
        XSetErrorHandler(old_handler);
        return empty;
    }

    Atom atom_wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom atom_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    Atom atom_strut_p = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    Atom atom_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    Atom atom_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

    xdi_window_info_t *infos = calloc(nchildren, sizeof(xdi_window_info_t));
    int count = 0;

    if (infos) {
        for (unsigned int i = 0; i < nchildren; i++) {
            Window w = children[i];
            if (w == ignore_win1 || w == ignore_win2) continue;

            XWindowAttributes attr;
            if (!XGetWindowAttributes(dpy, w, &attr)) continue;
            if (attr.map_state != IsViewable || attr.class == InputOnly) continue;

            /* Fast spatial pre-filter: only inspect windows near top or bottom of monitor */
            bool near_top = (attr.y <= monitor.y + 160);
            bool near_bot = (attr.y + attr.height >= monitor.y + monitor.height - 160);
            if (!near_top && !near_bot && !attr.override_redirect) continue;

            xdi_window_info_t info;
            memset(&info, 0, sizeof(info));
            info.win = w;
            info.rect.x = attr.x;
            info.rect.y = attr.y;
            info.rect.width = attr.width;
            info.rect.height = attr.height;
            info.override_redirect = (attr.override_redirect != 0);

            XClassHint ch;
            if (XGetClassHint(dpy, w, &ch)) {
                if ((ch.res_name && xdi_is_launcher_name(ch.res_name)) ||
                    (ch.res_class && xdi_is_launcher_name(ch.res_class))) {
                    info.is_launcher = true;
                }
                if (ch.res_name) XFree(ch.res_name);
                if (ch.res_class) XFree(ch.res_class);
            }

            if (!info.is_launcher) {
                Atom actual_type = None;
                int actual_fmt = 0;
                unsigned long nitems = 0, bytes_after = 0;
                unsigned char *name = NULL;
                if (XGetWindowProperty(dpy, w, atom_wm_name, 0, 64, False, AnyPropertyType,
                                       &actual_type, &actual_fmt, &nitems, &bytes_after, &name) == Success &&
                    name) {
                    if (xdi_is_launcher_name((const char *)name)) {
                        info.is_launcher = true;
                    }
                    XFree(name);
                }
            }

            if (avoid_panels) {
                info.is_dock = window_has_atom(dpy, w, atom_wtype, atom_dock);
                read_struts(dpy, w, atom_strut_p, atom_strut, monitor,
                            &info.strut_top, &info.strut_bottom);
            }

            infos[count++] = info;
        }
    }

    XFree(children);
    XSync(dpy, False);
    XSetErrorHandler(old_handler);

    xdi_obstacle_result_t res = xdi_solve_obstacles(infos, count, monitor,
                                                    island_x, island_w,
                                                    avoid_obstacles, avoid_panels);
    free(infos);
    return res;
}
