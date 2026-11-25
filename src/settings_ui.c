#include "settings_ui.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const uint32_t ACCENT_PRESETS[] = {
    0xFF6A00, /* Sunset Orange */
    0xFF2D55, /* Apple Rose */
    0x00D4FF, /* Neon Cyan */
    0x1DB954, /* Spotify Green */
    0xAF52DE, /* Electric Violet */
    0xFFFFFF  /* Pure White */
};
#define NUM_ACCENTS 6

enum {
    ROW_ALWAYS_ON_TOP = 0,
    ROW_AVOID_OBSTACLES,
    ROW_AVOID_PANELS,
    ROW_EQ_STYLE,
    ROW_POSITION,
    ROW_ACCENT,
    ROW_REAL_AUDIO,
    ROW_SPIN_ART,
    ROW_HIDE_IDLE,
    ROW_MARGIN,
    ROW_OBSTACLE_GAP,
    ROW_COUNT
};

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    double mr = fmin(w, h) * 0.5;
    if (r > mr) r = mr;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI * 0.5);
    cairo_arc(cr, x + r, y + h - r, r, M_PI * 0.5, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

static void draw_label(cairo_t *cr, const char *font, double pt, bool bold,
                       double r, double g, double b, double a,
                       double x, double y, double w, PangoAlignment align, const char *txt) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, font && font[0] ? font : "Sans");
    pango_font_description_set_size(desc, (int)(pt * PANGO_SCALE));
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
    pango_layout_set_alignment(layout, align);
    pango_layout_set_text(layout, txt, -1);

    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void draw_toggle(cairo_t *cr, double x, double y, bool on, uint32_t accent) {
    double w = 38.0, h = 21.0;
    rounded_rect(cr, x, y, w, h, h * 0.5);
    if (on) {
        double r = ((accent >> 16) & 0xFF) / 255.0;
        double g = ((accent >> 8) & 0xFF) / 255.0;
        double b = (accent & 0xFF) / 255.0;
        cairo_set_source_rgba(cr, r, g, b, 0.95);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20);
    }
    cairo_fill(cr);

    double kx = on ? (x + w - h * 0.5) : (x + h * 0.5);
    cairo_arc(cr, kx, y + h * 0.5, h * 0.38, 0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.98);
    cairo_fill(cr);
}

void xdi_settings_init(xdi_settings_ui_t *s, Display *dpy, xdi_config_t *cfg, const char *config_path) {
    memset(s, 0, sizeof(*s));
    s->dpy = dpy;
    s->screen = DefaultScreen(dpy);
    s->win = None;
    s->cfg = cfg;
    s->hover_row = -1;
    s->atom_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    snprintf(s->config_path, sizeof(s->config_path), "%s", config_path);
}

void xdi_settings_close(xdi_settings_ui_t *s) {
    if (!s || !s->open) return;
    if (s->xlib_surf) { cairo_surface_destroy(s->xlib_surf); s->xlib_surf = NULL; }
    if (s->back_surf) { cairo_surface_destroy(s->back_surf); s->back_surf = NULL; }
    if (s->win != None) {
        XDestroyWindow(s->dpy, s->win);
        s->win = None;
    }
    s->open = false;
    XFlush(s->dpy);
}

void xdi_settings_destroy(xdi_settings_ui_t *s) {
    xdi_settings_close(s);
}

void xdi_settings_open(xdi_settings_ui_t *s) {
    if (!s) return;
    if (s->open && s->win != None) {
        XRaiseWindow(s->dpy, s->win);
        XFlush(s->dpy);
        return;
    }

    Window root = RootWindow(s->dpy, s->screen);
    int sw = DisplayWidth(s->dpy, s->screen);
    int sh = DisplayHeight(s->dpy, s->screen);
    int wx = (sw - XDI_SETTINGS_W) / 2;
    int wy = (sh - XDI_SETTINGS_H) / 2;

    s->win = XCreateSimpleWindow(s->dpy, root, wx, wy,
                                 XDI_SETTINGS_W, XDI_SETTINGS_H, 0, 0, 0x101014);
    XStoreName(s->dpy, s->win, "xdynamicisland Settings");
    XClassHint ch = {.res_name = "xdynamicisland-settings", .res_class = "xdynamicisland"};
    XSetClassHint(s->dpy, s->win, &ch);

    XSizeHints *shints = XAllocSizeHints();
    if (shints) {
        shints->flags = PMinSize | PMaxSize;
        shints->min_width = shints->max_width = XDI_SETTINGS_W;
        shints->min_height = shints->max_height = XDI_SETTINGS_H;
        XSetWMNormalHints(s->dpy, s->win, shints);
        XFree(shints);
    }

    XSetWMProtocols(s->dpy, s->win, &s->atom_wm_delete, 1);
    XSelectInput(s->dpy, s->win,
                 ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask | KeyPressMask);

    s->xlib_surf = cairo_xlib_surface_create(s->dpy, s->win,
                                             DefaultVisual(s->dpy, s->screen),
                                             XDI_SETTINGS_W, XDI_SETTINGS_H);
    s->back_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, XDI_SETTINGS_W, XDI_SETTINGS_H);

    XMapWindow(s->dpy, s->win);
    XRaiseWindow(s->dpy, s->win);
    s->open = true;
    s->dirty = true;
    XFlush(s->dpy);
}

static int row_at_y(int y) {
    int start_y = 66;
    int row_h = 38;
    if (y < start_y) return -1;
    int r = (y - start_y) / row_h;
    if (r < 0 || r >= ROW_COUNT) return -1;
    return r;
}

bool xdi_settings_handle_event(xdi_settings_ui_t *s, const XEvent *ev) {
    if (!s || !s->open || ev->xany.window != s->win) return false;

    if (ev->type == ClientMessage && (Atom)ev->xclient.data.l[0] == s->atom_wm_delete) {
        xdi_settings_close(s);
        return false;
    }
    if (ev->type == Expose && ev->xexpose.count == 0) {
        s->dirty = true;
        return false;
    }
    if (ev->type == LeaveNotify) {
        s->hover_row = -1;
        s->dirty = true;
        return false;
    }
    if (ev->type == MotionNotify) {
        int r = row_at_y(ev->xmotion.y);
        if (r != s->hover_row) {
            s->hover_row = r;
            s->dirty = true;
        }
        return false;
    }
    if (ev->type == KeyPress) {
        KeySym ks = XLookupKeysym((XKeyEvent *)&ev->xkey, 0);
        if (ks == XK_Escape) {
            xdi_settings_close(s);
        }
        return false;
    }
    if (ev->type != ButtonPress) return false;

    int row = row_at_y(ev->xbutton.y);
    if (row < 0) return false;

    int dir = (ev->xbutton.button == 3 || ev->xbutton.button == 5) ? -1 : 1;
    bool changed = true;

    switch (row) {
        case ROW_ALWAYS_ON_TOP:
            s->cfg->always_on_top = !s->cfg->always_on_top;
            break;
        case ROW_AVOID_OBSTACLES:
            s->cfg->avoid_obstacles = !s->cfg->avoid_obstacles;
            break;
        case ROW_AVOID_PANELS:
            s->cfg->avoid_panels = !s->cfg->avoid_panels;
            break;
        case ROW_EQ_STYLE:
            s->cfg->eq_style = (xdi_eq_style_t)(((int)s->cfg->eq_style + dir + XDI_EQ_COUNT) % XDI_EQ_COUNT);
            break;
        case ROW_POSITION:
            s->cfg->position = (xdi_position_t)(((int)s->cfg->position + dir + XDI_POS_COUNT) % XDI_POS_COUNT);
            break;
        case ROW_ACCENT: {
            /* Check if clicking specific color swatch */
            double sx0 = XDI_SETTINGS_W - 22.0 - NUM_ACCENTS * 24.0;
            bool picked = false;
            for (int i = 0; i < NUM_ACCENTS; i++) {
                double cx = sx0 + i * 24.0 + 10.0;
                if (fabs((double)ev->xbutton.x - cx) <= 11.0) {
                    s->cfg->accent_rgb = ACCENT_PRESETS[i];
                    picked = true;
                    break;
                }
            }
            if (!picked) {
                int cur = 0;
                for (int i = 0; i < NUM_ACCENTS; i++) {
                    if (ACCENT_PRESETS[i] == s->cfg->accent_rgb) cur = i;
                }
                s->cfg->accent_rgb = ACCENT_PRESETS[(cur + dir + NUM_ACCENTS) % NUM_ACCENTS];
            }
            break;
        }
        case ROW_REAL_AUDIO:
            s->cfg->real_audio_viz = !s->cfg->real_audio_viz;
            break;
        case ROW_SPIN_ART:
            s->cfg->spin_art = !s->cfg->spin_art;
            break;
        case ROW_HIDE_IDLE:
            s->cfg->hide_when_idle = !s->cfg->hide_when_idle;
            break;
        case ROW_MARGIN:
            s->cfg->margin += (dir > 0 ? 4 : -4);
            if (s->cfg->margin > 40) s->cfg->margin = 0;
            if (s->cfg->margin < 0) s->cfg->margin = 40;
            break;
        case ROW_OBSTACLE_GAP:
            s->cfg->obstacle_gap += (dir > 0 ? 2 : -2);
            if (s->cfg->obstacle_gap > 24) s->cfg->obstacle_gap = 0;
            if (s->cfg->obstacle_gap < 0) s->cfg->obstacle_gap = 24;
            break;
        default:
            changed = false;
            break;
    }

    if (changed) {
        xdi_config_save(s->cfg, s->config_path);
        s->dirty = true;
    }
    return changed;
}

void xdi_settings_render(xdi_settings_ui_t *s) {
    if (!s || !s->open || !s->dirty || !s->back_surf || !s->xlib_surf) return;
    s->dirty = false;

    cairo_t *cr = cairo_create(s->back_surf);

    /* Dark obsidian background */
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, XDI_SETTINGS_H);
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.08, 0.08, 0.10);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.04, 0.04, 0.055);
    cairo_set_source(cr, bg);
    cairo_rectangle(cr, 0, 0, XDI_SETTINGS_W, XDI_SETTINGS_H);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    const char *font = s->cfg->font_family;
    draw_label(cr, font, 12.5, true, 1.0, 1.0, 1.0, 0.98,
               20, 16, XDI_SETTINGS_W - 40, PANGO_ALIGN_LEFT, "Dynamic Island Settings");
    draw_label(cr, font, 8.0, false, 0.58, 0.60, 0.66, 0.9,
               20, 39, XDI_SETTINGS_W - 40, PANGO_ALIGN_LEFT,
               "Click any row to toggle or cycle · Saved automatically");

    static const char *const labels[ROW_COUNT] = {
        "Always On Top",
        "Auto-Reposition Below dmenu",
        "Respect Desktop Panels",
        "Waveform Visualizer Style",
        "Screen Position",
        "Accent Color",
        "Live PipeWire/Pulse FFT Audio",
        "Spin Compact Album Art",
        "Hide Island When Idle",
        "Screen Edge Margin",
        "Gap Below dmenu / Overlays"
    };

    int start_y = 66;
    int row_h = 38;
    for (int i = 0; i < ROW_COUNT; i++) {
        double ry = (double)(start_y + i * row_h);
        rounded_rect(cr, 14.0, ry + 2.0, XDI_SETTINGS_W - 28.0, row_h - 4.0, 10.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (i == s->hover_row) ? 0.11 : 0.05);
        cairo_fill(cr);

        draw_label(cr, font, 9.2, false, 0.94, 0.95, 0.98, 0.96,
                   26.0, ry + 11.0, 220.0, PANGO_ALIGN_LEFT, labels[i]);

        double rx = XDI_SETTINGS_W - 62.0;
        double ty = ry + (row_h - 21.0) * 0.5;
        char val_buf[48] = {0};

        switch (i) {
            case ROW_ALWAYS_ON_TOP:
                draw_toggle(cr, rx, ty, s->cfg->always_on_top, s->cfg->accent_rgb);
                break;
            case ROW_AVOID_OBSTACLES:
                draw_toggle(cr, rx, ty, s->cfg->avoid_obstacles, s->cfg->accent_rgb);
                break;
            case ROW_AVOID_PANELS:
                draw_toggle(cr, rx, ty, s->cfg->avoid_panels, s->cfg->accent_rgb);
                break;
            case ROW_REAL_AUDIO:
                draw_toggle(cr, rx, ty, s->cfg->real_audio_viz, s->cfg->accent_rgb);
                break;
            case ROW_SPIN_ART:
                draw_toggle(cr, rx, ty, s->cfg->spin_art, s->cfg->accent_rgb);
                break;
            case ROW_HIDE_IDLE:
                draw_toggle(cr, rx, ty, s->cfg->hide_when_idle, s->cfg->accent_rgb);
                break;
            case ROW_EQ_STYLE:
                snprintf(val_buf, sizeof(val_buf), "%s", xdi_eq_style_to_str(s->cfg->eq_style));
                break;
            case ROW_POSITION:
                snprintf(val_buf, sizeof(val_buf), "%s", xdi_position_to_str(s->cfg->position));
                break;
            case ROW_MARGIN:
                snprintf(val_buf, sizeof(val_buf), "%d px", s->cfg->margin);
                break;
            case ROW_OBSTACLE_GAP:
                snprintf(val_buf, sizeof(val_buf), "%d px", s->cfg->obstacle_gap);
                break;
            case ROW_ACCENT: {
                double sx0 = XDI_SETTINGS_W - 22.0 - NUM_ACCENTS * 24.0;
                for (int c = 0; c < NUM_ACCENTS; c++) {
                    double cx = sx0 + c * 24.0 + 10.0;
                    double cy = ry + row_h * 0.5;
                    uint32_t rgb = ACCENT_PRESETS[c];
                    cairo_arc(cr, cx, cy, 8.0, 0, 2.0 * M_PI);
                    cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.0,
                                         ((rgb >> 8) & 0xFF) / 255.0,
                                         (rgb & 0xFF) / 255.0);
                    cairo_fill(cr);
                    if (rgb == s->cfg->accent_rgb) {
                        cairo_arc(cr, cx, cy, 10.2, 0, 2.0 * M_PI);
                        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
                        cairo_set_line_width(cr, 1.6);
                        cairo_stroke(cr);
                    }
                }
                break;
            }
        }

        if (val_buf[0]) {
            double pill_w = 104.0;
            double px = XDI_SETTINGS_W - pill_w - 22.0;
            rounded_rect(cr, px, ry + 8.0, pill_w, 22.0, 11.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13);
            cairo_fill(cr);
            draw_label(cr, font, 8.5, true, 1.0, 1.0, 1.0, 0.95,
                       px, ry + 12.0, pill_w, PANGO_ALIGN_CENTER, val_buf);
        }
    }

    draw_label(cr, font, 7.6, false, 0.45, 0.47, 0.53, 0.9,
               20.0, XDI_SETTINGS_H - 24.0, XDI_SETTINGS_W - 40.0, PANGO_ALIGN_CENTER,
               "xdynamicisland v" XDI_VERSION " · Press Esc to close");

    cairo_destroy(cr);

    cairo_t *xcr = cairo_create(s->xlib_surf);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(xcr, s->back_surf, 0, 0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    cairo_surface_flush(s->xlib_surf);
    XFlush(s->dpy);
}
