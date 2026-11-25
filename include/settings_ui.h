#ifndef XDI_SETTINGS_UI_H
#define XDI_SETTINGS_UI_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdbool.h>

#include "config.h"

#define XDI_SETTINGS_W 400
#define XDI_SETTINGS_H 520

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    Atom atom_wm_delete;
    cairo_surface_t *xlib_surf;
    cairo_surface_t *back_surf;
    bool open;
    bool dirty;
    int hover_row;
    xdi_config_t *cfg;
    char config_path[512];
} xdi_settings_ui_t;

void xdi_settings_init(xdi_settings_ui_t *s, Display *dpy, xdi_config_t *cfg, const char *config_path);
void xdi_settings_destroy(xdi_settings_ui_t *s);
void xdi_settings_open(xdi_settings_ui_t *s);
void xdi_settings_close(xdi_settings_ui_t *s);

/* Handle XEvent for settings window. Returns true if configuration was modified. */
bool xdi_settings_handle_event(xdi_settings_ui_t *s, const XEvent *ev);

/* Render if dirty. */
void xdi_settings_render(xdi_settings_ui_t *s);

#endif /* XDI_SETTINGS_UI_H */
