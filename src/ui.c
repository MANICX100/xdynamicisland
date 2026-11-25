#include "ui.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SHADOW_PAD_COMPOSITED 12

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} motif_wm_hints_t;

/* ---------- Geometry & Cairo Vector Primitives ---------- */

static void path_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (w <= 0.0 || h <= 0.0) return;
    double max_r = fmin(w, h) * 0.5;
    if (r > max_r) r = max_r;
    if (r < 0.0) r = 0.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI * 0.5);
    cairo_arc(cr, x + r, y + h - r, r, M_PI * 0.5, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

static void add_hit(xdi_ui_t *ui, int id, double x, double y, double w, double h) {
    if (ui->hit_count >= XDI_MAX_HIT_RECTS) return;
    ui->hits[ui->hit_count++] = (xdi_hit_rect_t){x, y, w, h, id};
}

static int find_hit(const xdi_ui_t *ui, double x, double y) {
    for (int i = ui->hit_count - 1; i >= 0; i--) {
        const xdi_hit_rect_t *r = &ui->hits[i];
        if (x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h) {
            return r->id;
        }
    }
    return XDI_HIT_NONE;
}

static void draw_text(cairo_t *cr, const char *font_family, double pt_size, bool bold,
                      double r, double g, double b, double a,
                      double x, double y, double max_w, PangoAlignment align,
                      const char *text) {
    if (!text || !text[0]) return;
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, font_family && font_family[0] ? font_family : "Sans");
    pango_font_description_set_size(desc, (int)(pt_size * PANGO_SCALE));
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, align);
    if (max_w > 0.0) {
        pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
    }
    pango_layout_set_text(layout, text, -1);

    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void format_mmss(int64_t us, char *buf, size_t len) {
    if (us < 0) us = 0;
    int64_t secs = us / 1000000;
    int mins = (int)(secs / 60);
    int rem = (int)(secs % 60);
    snprintf(buf, len, "%d:%02d", mins, rem);
}

/* ---------- Crisp Vector Icons (Zero external SVG/icon theme dependency!) ---------- */

static void draw_icon_play(cairo_t *cr, double cx, double cy, double sz) {
    double r = sz * 0.48;
    cairo_move_to(cr, cx - r * 0.65, cy - r * 0.9);
    cairo_line_to(cr, cx + r * 0.95, cy);
    cairo_line_to(cr, cx - r * 0.65, cy + r * 0.9);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_icon_pause(cairo_t *cr, double cx, double cy, double sz) {
    double bw = sz * 0.24;
    double bh = sz * 0.82;
    double gap = sz * 0.16;
    path_rounded_rect(cr, cx - gap * 0.5 - bw, cy - bh * 0.5, bw, bh, bw * 0.45);
    path_rounded_rect(cr, cx + gap * 0.5, cy - bh * 0.5, bw, bh, bw * 0.45);
    cairo_fill(cr);
}

static void draw_icon_prev(cairo_t *cr, double cx, double cy, double sz) {
    double h = sz * 0.68;
    double w = sz * 0.52;
    /* Vertical bar */
    path_rounded_rect(cr, cx - w * 0.88, cy - h * 0.5, sz * 0.15, h, 1.5);
    cairo_fill(cr);
    /* Left-pointing triangle */
    cairo_move_to(cr, cx - w * 0.55, cy);
    cairo_line_to(cr, cx + w * 0.55, cy - h * 0.5);
    cairo_line_to(cr, cx + w * 0.55, cy + h * 0.5);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_icon_next(cairo_t *cr, double cx, double cy, double sz) {
    double h = sz * 0.68;
    double w = sz * 0.52;
    /* Right-pointing triangle */
    cairo_move_to(cr, cx + w * 0.55, cy);
    cairo_line_to(cr, cx - w * 0.55, cy - h * 0.5);
    cairo_line_to(cr, cx - w * 0.55, cy + h * 0.5);
    cairo_close_path(cr);
    cairo_fill(cr);
    /* Vertical bar */
    path_rounded_rect(cr, cx + w * 0.72, cy - h * 0.5, sz * 0.15, h, 1.5);
    cairo_fill(cr);
}

static void draw_icon_bell(cairo_t *cr, double cx, double cy, double sz) {
    double r = sz * 0.34;
    cairo_save(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_arc(cr, cx, cy - sz * 0.08, r, M_PI, 2.0 * M_PI);
    cairo_line_to(cr, cx + r * 1.25, cy + sz * 0.28);
    cairo_line_to(cr, cx - r * 1.25, cy + sz * 0.28);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy + sz * 0.42, sz * 0.10, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_icon_gear(cairo_t *cr, double cx, double cy, double sz) {
    double r_out = sz * 0.42;
    double r_in = sz * 0.28;
    cairo_save(cr);
    cairo_set_line_width(cr, 1.7);
    for (int i = 0; i < 8; i++) {
        double a = i * (M_PI / 4.0);
        cairo_move_to(cr, cx + cos(a) * r_in, cy + sin(a) * r_in);
        cairo_line_to(cr, cx + cos(a) * r_out, cy + sin(a) * r_out);
    }
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, sz * 0.22, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_icon_speaker(cairo_t *cr, double cx, double cy, double sz, double vol, bool muted) {
    double s = sz * 0.45;
    cairo_save(cr);
    cairo_move_to(cr, cx - s, cy - s * 0.35);
    cairo_line_to(cr, cx - s * 0.45, cy - s * 0.35);
    cairo_line_to(cr, cx + s * 0.05, cy - s * 0.8);
    cairo_line_to(cr, cx + s * 0.05, cy + s * 0.8);
    cairo_line_to(cr, cx - s * 0.45, cy + s * 0.35);
    cairo_line_to(cr, cx - s, cy + s * 0.35);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (muted || vol < 0.01) {
        cairo_move_to(cr, cx + s * 0.35, cy - s * 0.4);
        cairo_line_to(cr, cx + s * 0.95, cy + s * 0.4);
        cairo_move_to(cr, cx + s * 0.95, cy - s * 0.4);
        cairo_line_to(cr, cx + s * 0.35, cy + s * 0.4);
        cairo_stroke(cr);
    } else {
        cairo_arc(cr, cx + s * 0.15, cy, s * 0.45, -M_PI * 0.32, M_PI * 0.32);
        cairo_stroke(cr);
        if (vol > 0.45) {
            cairo_arc(cr, cx + s * 0.15, cy, s * 0.82, -M_PI * 0.34, M_PI * 0.34);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

static void draw_icon_phone(cairo_t *cr, double cx, double cy, double sz) {
    double w = sz * 0.56;
    double h = sz * 0.9;
    cairo_save(cr);
    cairo_set_line_width(cr, 1.3);
    path_rounded_rect(cr, cx - w * 0.5, cy - h * 0.5, w, h, 2.2);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy + h * 0.32, 0.9, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_icon_music_note(cairo_t *cr, double cx, double cy, double sz) {
    double s = sz * 0.42;
    cairo_save(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - s * 0.25, cy + s * 0.55);
    cairo_line_to(cr, cx - s * 0.25, cy - s * 0.65);
    cairo_line_to(cr, cx + s * 0.65, cy - s * 0.85);
    cairo_line_to(cr, cx + s * 0.65, cy + s * 0.35);
    cairo_stroke(cr);
    cairo_arc(cr, cx - s * 0.48, cy + s * 0.55, s * 0.28, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, cx + s * 0.42, cy + s * 0.35, s * 0.28, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* ---------- Sexy Live Playback Waveform Renderer (6 Styles) ---------- */

static void draw_waveform(cairo_t *cr, const xdi_ui_t *ui,
                          double x, double y, double w, double h, bool playing) {
    uint32_t accent = ui->cfg->accent_rgb;
    if (ui->art->has_dominant) {
        /* Subtle 40% blend toward album art's vibrant dominant hue for iOS magic! */
        int ar = (accent >> 16) & 0xFF, ag = (accent >> 8) & 0xFF, ab = accent & 0xFF;
        int dr = (ui->art->dominant_rgb >> 16) & 0xFF;
        int dg = (ui->art->dominant_rgb >> 8) & 0xFF;
        int db = ui->art->dominant_rgb & 0xFF;
        int mr = (ar * 6 + dr * 4) / 10;
        int mg = (ag * 6 + dg * 4) / 10;
        int mb = (ab * 6 + db * 4) / 10;
        accent = ((uint32_t)mr << 16) | ((uint32_t)mg << 8) | (uint32_t)mb;
    }

    double top[3], bot[3];
    xdi_accent_gradient(accent, top, bot);

    cairo_pattern_t *grad = cairo_pattern_create_linear(x, y, x + w, y + h);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, top[0], top[1], top[2], playing ? 1.0 : 0.45);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, bot[0], bot[1], bot[2], playing ? 1.0 : 0.45);

    cairo_save(cr);

    switch (ui->cfg->eq_style) {
    case XDI_EQ_WAVE: {
        /* Dual-layered glowing silky harmonic waveform */
        double mid_y = y + h * 0.5;
        double max_amp = h * 0.44;
        double base_amp = fmax(0.10, ui->viz->rms * 0.95 + 0.15 * xdi_audio_viz_sample(ui->viz, 0.5));
        if (!playing) base_amp *= 0.25;

        /* Secondary ghost harmonic wave */
        cairo_new_path(cr);
        for (int i = 0; i <= (int)w; i++) {
            double u = (double)i / w;
            double taper = sin(M_PI * u);
            double local = 0.45 + 0.55 * xdi_audio_viz_sample(ui->viz, u);
            double dy = max_amp * base_amp * local * taper *
                        sin(u * 4.2 * M_PI - ui->viz->phase * 1.15 + 1.1) * 0.72;
            if (i == 0) cairo_move_to(cr, x + i, mid_y + dy);
            else cairo_line_to(cr, x + i, mid_y + dy);
        }
        cairo_set_source_rgba(cr, top[0], top[1], top[2], playing ? 0.32 : 0.14);
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke(cr);

        /* Soft bloom glow under primary wave */
        cairo_new_path(cr);
        for (int i = 0; i <= (int)w; i++) {
            double u = (double)i / w;
            double taper = pow(sin(M_PI * u), 0.85);
            double local = 0.40 + 0.65 * xdi_audio_viz_sample(ui->viz, u);
            double dy = max_amp * base_amp * local * taper *
                        (0.75 * sin(u * 3.4 * M_PI + ui->viz->phase * 1.35) +
                         0.25 * sin(u * 6.8 * M_PI - ui->viz->phase * 0.9));
            if (i == 0) cairo_move_to(cr, x + i, mid_y + dy);
            else cairo_line_to(cr, x + i, mid_y + dy);
        }
        if (playing) {
            cairo_set_source_rgba(cr, bot[0], bot[1], bot[2], 0.28);
            cairo_set_line_width(cr, 5.2);
            cairo_stroke_preserve(cr);
        }
        /* Crisp primary foreground wave */
        cairo_set_source(cr, grad);
        cairo_set_line_width(cr, 2.2);
        cairo_stroke(cr);
        break;
    }

    case XDI_EQ_RIBBON: {
        /* Mirrored liquid audio ribbon rippling continuously */
        double mid_y = y + h * 0.5;
        cairo_new_path(cr);
        for (int i = 0; i <= (int)w; i++) {
            double u = (double)i / w;
            double taper = sin(M_PI * u);
            double amp = (1.2 + xdi_audio_viz_sample(ui->viz, u) * (h * 0.46)) * taper *
                         (0.76 + 0.24 * sin(u * 2.8 * M_PI + ui->viz->phase));
            if (i == 0) cairo_move_to(cr, x + i, mid_y - amp);
            else cairo_line_to(cr, x + i, mid_y - amp);
        }
        for (int i = (int)w; i >= 0; i--) {
            double u = (double)i / w;
            double taper = sin(M_PI * u);
            double amp = (1.2 + xdi_audio_viz_sample(ui->viz, u) * (h * 0.46)) * taper *
                         (0.76 + 0.24 * sin(u * 2.8 * M_PI + ui->viz->phase));
            cairo_line_to(cr, x + i, mid_y + amp);
        }
        cairo_close_path(cr);
        cairo_set_source(cr, grad);
        cairo_fill(cr);
        break;
    }

    case XDI_EQ_SPECTRUM: {
        int nb = (w >= 40.0) ? 10 : 8;
        double gap = 2.0;
        double bw = (w - gap * (nb - 1)) / (double)nb;
        if (bw < 1.8) bw = 1.8;
        cairo_set_source(cr, grad);
        for (int i = 0; i < nb; i++) {
            double u = (double)i / (double)(nb - 1);
            double lvl = xdi_audio_viz_sample(ui->viz, u);
            double bh = 2.5 + lvl * (h - 3.0) * (1.0 - 0.04 * i);
            path_rounded_rect(cr, x + i * (bw + gap), y + h - bh, bw, bh, bw * 0.5);
        }
        cairo_fill(cr);
        break;
    }

    case XDI_EQ_SEGMENTS: {
        int cols = (w >= 40.0) ? 6 : 5;
        int rows = 5;
        double gap_x = 2.5, gap_y = 1.6;
        double bw = (w - gap_x * (cols - 1)) / (double)cols;
        double bh = (h - gap_y * (rows - 1)) / (double)rows;
        for (int c = 0; c < cols; c++) {
            double lvl = xdi_audio_viz_sample(ui->viz, (double)c / (double)(cols - 1));
            int lit = (int)ceil(lvl * rows);
            if (lit < 1) lit = 1;
            for (int r = 0; r < rows; r++) {
                double bx = x + c * (bw + gap_x);
                double by = y + h - (r + 1) * bh - r * gap_y;
                path_rounded_rect(cr, bx, by, bw, bh, 1.2);
                if (r < lit) {
                    cairo_set_source(cr, grad);
                } else {
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
                }
                cairo_fill(cr);
            }
        }
        break;
    }

    case XDI_EQ_SOLID: {
        cairo_new_path(cr);
        cairo_move_to(cr, x, y + h);
        for (int i = 0; i <= (int)w; i++) {
            double u = (double)i / w;
            double taper = sqrt(fmax(0.0, sin(M_PI * u)));
            double lvl = xdi_audio_viz_sample(ui->viz, u);
            double py = y + h - 1.5 - lvl * (h - 2.5) * taper;
            cairo_line_to(cr, x + i, py);
        }
        cairo_line_to(cr, x + w, y + h);
        cairo_close_path(cr);
        cairo_set_source(cr, grad);
        cairo_fill(cr);
        break;
    }

    case XDI_EQ_BARS:
    default: {
        int nb = (w >= 40.0) ? 6 : 5;
        double gap = 3.0;
        double bw = (w - gap * (nb - 1)) / (double)nb;
        if (bw > 4.2) bw = 4.2;
        double total_w = nb * bw + (nb - 1) * gap;
        double sx = x + (w - total_w) * 0.5;
        cairo_set_source(cr, grad);
        for (int i = 0; i < nb; i++) {
            double u = (double)i / (double)(nb - 1);
            double lvl = xdi_audio_viz_sample(ui->viz, u);
            double bh = 3.5 + lvl * (h - 3.5);
            path_rounded_rect(cr, sx + i * (bw + gap), y + (h - bh) * 0.5, bw, bh, bw * 0.5);
        }
        cairo_fill(cr);
        break;
    }
    }

    cairo_restore(cr);
    cairo_pattern_destroy(grad);
}

/* ---------- Album Art Renderer (Circle / Rounded / Square + Optional Vinyl Spin) ---------- */

static void draw_album_art(cairo_t *cr, const xdi_ui_t *ui,
                           double x, double y, double sz, bool compact) {
    double rad;
    switch (ui->cfg->art_shape) {
        case XDI_ART_SQUARE:  rad = compact ? 3.0 : 4.0; break;
        case XDI_ART_ROUNDED: rad = sz * 0.24; break;
        case XDI_ART_CIRCLE:
        default:              rad = compact ? sz * 0.5 : sz * 0.24; break;
    }

    cairo_save(cr);
    path_rounded_rect(cr, x, y, sz, sz, rad);
    cairo_clip(cr);

    if (ui->art->surface) {
        int sw = cairo_image_surface_get_width(ui->art->surface);
        int sh = cairo_image_surface_get_height(ui->art->surface);
        if (sw > 0 && sh > 0) {
            double scale = fmax(sz / (double)sw, sz / (double)sh);
            cairo_translate(cr, x + sz * 0.5, y + sz * 0.5);
            if (ui->cfg->spin_art && compact && ui->cfg->art_shape == XDI_ART_CIRCLE) {
                cairo_rotate(cr, ui->art_angle);
            }
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, ui->art->surface, -sw * 0.5, -sh * 0.5);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
        }
    } else {
        /* Stylish dark gradient vinyl/music fallback */
        double top[3], bot[3];
        xdi_accent_gradient(ui->cfg->accent_rgb, top, bot);
        cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + sz, y + sz);
        cairo_pattern_add_color_stop_rgba(g, 0.0, top[0] * 0.55, top[1] * 0.55, top[2] * 0.55, 1.0);
        cairo_pattern_add_color_stop_rgba(g, 1.0, bot[0] * 0.35, bot[1] * 0.35, bot[2] * 0.35, 1.0);
        cairo_set_source(cr, g);
        cairo_rectangle(cr, x, y, sz, sz);
        cairo_fill(cr);
        cairo_pattern_destroy(g);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        draw_icon_music_note(cr, x + sz * 0.5, y + sz * 0.5, sz * 0.62);
    }
    cairo_restore(cr);

    /* Subtle rim highlight around album art */
    cairo_save(cr);
    path_rounded_rect(cr, x + 0.5, y + 0.5, sz - 1.0, sz - 1.0, rad);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* ---------- X11 Window Creation, Monitor, Compositor & Shape Mask ---------- */

static int shadow_pad(const xdi_ui_t *ui) {
    return ui->composited ? SHADOW_PAD_COMPOSITED : 0;
}

static bool detect_compositor(xdi_ui_t *ui) {
    return XGetSelectionOwner(ui->dpy, ui->atom_cm) != None;
}

static Visual *find_argb_visual(Display *dpy, int screen, int *out_depth) {
    XVisualInfo tpl;
    tpl.screen = screen;
    tpl.depth = 32;
    tpl.class = TrueColor;
    int n = 0;
    XVisualInfo *vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tpl, &n);
    if (vi && n > 0) {
        Visual *v = vi[0].visual;
        *out_depth = 32;
        XFree(vi);
        return v;
    }
    if (vi) XFree(vi);
    *out_depth = DefaultDepth(dpy, screen);
    return DefaultVisual(dpy, screen);
}

static void apply_ewmh_hints(xdi_ui_t *ui) {
    /* Class hint for identification */
    XClassHint ch = {.res_name = "xdynamicisland", .res_class = "xdynamicisland"};
    XSetClassHint(ui->dpy, ui->win, &ch);
    XStoreName(ui->dpy, ui->win, "xdynamicisland");

    /* Borderless Motif hints (matters when always_on_top == false and WM manages window) */
    motif_wm_hints_t mhints = {2, 0, 0, 0, 0};
    XChangeProperty(ui->dpy, ui->win, ui->atom_motif_hints, ui->atom_motif_hints,
                    32, PropModeReplace, (unsigned char *)&mhints, 5);

    Atom wtype = XInternAtom(ui->dpy,
                             ui->cfg->always_on_top ? "_NET_WM_WINDOW_TYPE_DOCK" : "_NET_WM_WINDOW_TYPE_UTILITY",
                             False);
    XChangeProperty(ui->dpy, ui->win, ui->atom_wm_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&wtype, 1);

    Atom states[4];
    int ns = 0;
    states[ns++] = ui->atom_state_sticky;
    states[ns++] = ui->atom_state_skip_taskbar;
    states[ns++] = ui->atom_state_skip_pager;
    if (ui->cfg->always_on_top) {
        states[ns++] = ui->atom_state_above;
    }
    XChangeProperty(ui->dpy, ui->win, ui->atom_wm_state, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)states, ns);
}

static void destroy_window_surfaces(xdi_ui_t *ui) {
    if (ui->xlib_surf) {
        cairo_surface_destroy(ui->xlib_surf);
        ui->xlib_surf = NULL;
    }
    if (ui->back_surf) {
        cairo_surface_destroy(ui->back_surf);
        ui->back_surf = NULL;
    }
    ui->surf_w = 0;
    ui->surf_h = 0;
}

static bool create_island_window(xdi_ui_t *ui) {
    if (ui->win != None) {
        destroy_window_surfaces(ui);
        XDestroyWindow(ui->dpy, ui->win);
        ui->win = None;
        ui->mapped = false;
    }

    ui->composited = detect_compositor(ui);
    if (ui->composited) {
        ui->visual = find_argb_visual(ui->dpy, ui->screen, &ui->depth);
        if (ui->depth == 32) {
            ui->colormap = XCreateColormap(ui->dpy, ui->root, ui->visual, AllocNone);
        } else {
            ui->colormap = DefaultColormap(ui->dpy, ui->screen);
        }
    } else {
        ui->depth = DefaultDepth(ui->dpy, ui->screen);
        ui->visual = DefaultVisual(ui->dpy, ui->screen);
        ui->colormap = DefaultColormap(ui->dpy, ui->screen);
    }

    ui->window_is_override = ui->cfg->always_on_top;

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = ui->colormap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.override_redirect = ui->window_is_override ? True : False;
    swa.event_mask = ExposureMask | EnterWindowMask | LeaveWindowMask |
                     PointerMotionMask | ButtonPressMask | ButtonReleaseMask |
                     StructureNotifyMask;

    unsigned long mask = CWColormap | CWBackPixel | CWBorderPixel |
                         CWOverrideRedirect | CWEventMask;

    int pad = shadow_pad(ui);
    int w = (int)round(ui->sp_w.value) + pad * 2;
    int h = (int)round(ui->sp_h.value) + pad * 2;
    if (w < 40) w = 150;
    if (h < 24) h = 36;

    ui->win = XCreateWindow(ui->dpy, ui->root, ui->win_x, ui->win_y,
                            (unsigned int)w, (unsigned int)h, 0,
                            ui->depth, InputOutput, ui->visual, mask, &swa);
    if (ui->win == None) return false;

    apply_ewmh_hints(ui);
    return true;
}

static void apply_non_composited_shape(xdi_ui_t *ui, int w, int h, double radius) {
    if (ui->composited || ui->win == None || w <= 0 || h <= 0) return;

    Pixmap mask = XCreatePixmap(ui->dpy, ui->win, (unsigned int)w, (unsigned int)h, 1);
    if (!mask) return;

    GC gc = XCreateGC(ui->dpy, mask, 0, NULL);
    XSetForeground(ui->dpy, gc, 0);
    XFillRectangle(ui->dpy, mask, gc, 0, 0, (unsigned int)w, (unsigned int)h);

    /* Draw exact rounded pill in 1-bit shape bitmap */
    int r = (int)round(radius);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    int d = r * 2;

    XSetForeground(ui->dpy, gc, 1);
    if (w > d) {
        XFillRectangle(ui->dpy, mask, gc, r, 0, (unsigned int)(w - d), (unsigned int)h);
    }
    if (h > d) {
        XFillRectangle(ui->dpy, mask, gc, 0, r, (unsigned int)w, (unsigned int)(h - d));
    }
    if (d > 0) {
        XFillArc(ui->dpy, mask, gc, 0, 0, (unsigned int)d, (unsigned int)d, 90 * 64, 90 * 64);
        XFillArc(ui->dpy, mask, gc, w - d, 0, (unsigned int)d, (unsigned int)d, 0, 90 * 64);
        XFillArc(ui->dpy, mask, gc, 0, h - d, (unsigned int)d, (unsigned int)d, 180 * 64, 90 * 64);
        XFillArc(ui->dpy, mask, gc, w - d, h - d, (unsigned int)d, (unsigned int)d, 270 * 64, 90 * 64);
    }

    XShapeCombineMask(ui->dpy, ui->win, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(ui->dpy, gc);
    XFreePixmap(ui->dpy, mask);
}

void xdi_ui_update_monitor(xdi_ui_t *ui) {
    ui->monitor.x = 0;
    ui->monitor.y = 0;
    ui->monitor.width = DisplayWidth(ui->dpy, ui->screen);
    ui->monitor.height = DisplayHeight(ui->dpy, ui->screen);

    int rr_event = 0, rr_error = 0;
    if (!XRRQueryExtension(ui->dpy, &rr_event, &rr_error)) return;

    int nmon = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(ui->dpy, ui->root, True, &nmon);
    if (mons && nmon > 0) {
        int chosen = 0;
        if (ui->cfg->monitor_index >= 0 && ui->cfg->monitor_index < nmon) {
            chosen = ui->cfg->monitor_index;
        } else {
            for (int i = 0; i < nmon; i++) {
                if (mons[i].primary) {
                    chosen = i;
                    break;
                }
            }
        }
        ui->monitor.x = mons[chosen].x;
        ui->monitor.y = mons[chosen].y;
        ui->monitor.width = mons[chosen].width;
        ui->monitor.height = mons[chosen].height;
        XRRFreeMonitors(mons);
    }
}

/* ---------- State Machine & Target Geometry Computation ---------- */

static void compute_target_geometry(xdi_ui_t *ui, int64_t now_ms,
                                    double *tw, double *th, double *tr,
                                    double *tx, double *ty) {
    const xdi_player_t *p = xdi_media_active(ui->media);
    bool has_media = (p != NULL && (p->status == XDI_PLAYBACK_PLAYING || p->title[0] != '\0'));

    /* Mode transitions */
    if (ui->mode == XDI_MODE_TOAST_NOTIF || ui->mode == XDI_MODE_TOAST_VOLUME) {
        if (now_ms >= ui->toast_until_ms) {
            ui->mode = has_media ? XDI_MODE_COMPACT_MEDIA : XDI_MODE_IDLE;
        }
    }

    if (ui->mode != XDI_MODE_NOTIF_HISTORY &&
        ui->mode != XDI_MODE_TOAST_NOTIF &&
        ui->mode != XDI_MODE_TOAST_VOLUME) {
        if (ui->hovered || ui->pinned_open) {
            if (has_media) {
                ui->mode = XDI_MODE_EXPANDED_MEDIA;
            } else {
                ui->mode = XDI_MODE_IDLE;
            }
        } else {
            if (has_media) {
                ui->mode = XDI_MODE_COMPACT_MEDIA;
            } else {
                ui->mode = ui->cfg->hide_when_idle ? XDI_MODE_HIDDEN : XDI_MODE_IDLE;
            }
        }
    }

    switch (ui->mode) {
        case XDI_MODE_HIDDEN:
            *tw = 100.0; *th = 24.0; *tr = 12.0;
            break;
        case XDI_MODE_IDLE:
            if (ui->hovered) {
                *tw = 236.0; *th = 38.0; *tr = 19.0;
            } else {
                *tw = (ui->notifs->unread_count > 0) ? 168.0 : 142.0;
                *th = 34.0; *tr = 17.0;
            }
            break;
        case XDI_MODE_COMPACT_MEDIA:
            *tw = 274.0; *th = 38.0; *tr = 19.0;
            break;
        case XDI_MODE_EXPANDED_MEDIA:
            *tw = 392.0; *th = 158.0; *tr = 28.0;
            break;
        case XDI_MODE_TOAST_NOTIF:
            *tw = 368.0; *th = 68.0; *tr = 24.0;
            break;
        case XDI_MODE_TOAST_VOLUME:
            *tw = 244.0; *th = 40.0; *tr = 20.0;
            break;
        case XDI_MODE_NOTIF_HISTORY:
            *tw = 400.0;
            *th = (ui->notifs->count == 0) ? 132.0 : fmin(330.0, 68.0 + ui->notifs->count * 62.0);
            *tr = 26.0;
            break;
    }

    /* Horizontal alignment on monitor */
    int m = ui->cfg->margin;
    switch (ui->cfg->position) {
        case XDI_POS_TOP_LEFT:
        case XDI_POS_BOTTOM_LEFT:
            *tx = (double)(ui->monitor.x + m);
            break;
        case XDI_POS_TOP_RIGHT:
        case XDI_POS_BOTTOM_RIGHT:
            *tx = (double)(ui->monitor.x + ui->monitor.width - m) - *tw;
            break;
        case XDI_POS_TOP_CENTER:
        case XDI_POS_BOTTOM_CENTER:
        default:
            *tx = (double)ui->monitor.x + ((double)ui->monitor.width - *tw) * 0.5;
            break;
    }

    /* Vertical alignment + obstacle clearance (dmenu, rofi, top/bottom panels) */
    bool is_bottom = (ui->cfg->position == XDI_POS_BOTTOM_CENTER ||
                      ui->cfg->position == XDI_POS_BOTTOM_LEFT ||
                      ui->cfg->position == XDI_POS_BOTTOM_RIGHT);
    if (!is_bottom) {
        double base_y = (double)(ui->monitor.y + m);
        if (ui->obstacles.top_clearance > 0) {
            double clear_y = (double)(ui->monitor.y + ui->obstacles.top_clearance + ui->cfg->obstacle_gap);
            if (clear_y > base_y) base_y = clear_y;
        }
        *ty = base_y;
    } else {
        double base_y = (double)(ui->monitor.y + ui->monitor.height - m) - *th;
        if (ui->obstacles.bottom_clearance > 0) {
            double clear_y = (double)(ui->monitor.y + ui->monitor.height -
                                      ui->obstacles.bottom_clearance - ui->cfg->obstacle_gap) - *th;
            if (clear_y < base_y) base_y = clear_y;
        }
        *ty = base_y;
    }
}

/* ---------- Full Cairo Frame Renderer ---------- */

static void draw_circle_button(cairo_t *cr, xdi_ui_t *ui, int hit_id,
                               double cx, double cy, double r, bool accent_bg) {
    bool hov = (ui->hover_hit_id == hit_id);
    add_hit(ui, hit_id, cx - r, cy - r, r * 2.0, r * 2.0);
    if (accent_bg) {
        double top[3], bot[3];
        xdi_accent_gradient(ui->cfg->accent_rgb, top, bot);
        cairo_pattern_t *g = cairo_pattern_create_linear(cx, cy - r, cx, cy + r);
        cairo_pattern_add_color_stop_rgba(g, 0.0, top[0], top[1], top[2], hov ? 1.0 : 0.92);
        cairo_pattern_add_color_stop_rgba(g, 1.0, bot[0], bot[1], bot[2], hov ? 1.0 : 0.92);
        cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    } else if (hov) {
        cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
        cairo_fill(cr);
    }
}

static void render_frame(xdi_ui_t *ui, int64_t now_ms) {
    if (!ui->win || !ui->mapped) return;

    int pad = shadow_pad(ui);
    int pill_w = (int)round(ui->sp_w.value);
    int pill_h = (int)round(ui->sp_h.value);
    if (pill_w < 60) pill_w = 60;
    if (pill_h < 24) pill_h = 24;
    double rad = ui->sp_r.value;
    if (rad > pill_h * 0.5) rad = pill_h * 0.5;

    int total_w = pill_w + pad * 2;
    int total_h = pill_h + pad * 2;
    int win_x = (int)round(ui->sp_x.value) - pad;
    int win_y = (int)round(ui->sp_y.value) - pad;

    if (total_w != ui->surf_w || total_h != ui->surf_h ||
        win_x != ui->win_x || win_y != ui->win_y) {
        XMoveResizeWindow(ui->dpy, ui->win, win_x, win_y,
                          (unsigned int)total_w, (unsigned int)total_h);
        ui->win_x = win_x;
        ui->win_y = win_y;
    }

    if (total_w != ui->surf_w || total_h != ui->surf_h || !ui->xlib_surf || !ui->back_surf) {
        destroy_window_surfaces(ui);
        ui->xlib_surf = cairo_xlib_surface_create(ui->dpy, ui->win, ui->visual, total_w, total_h);
        ui->back_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, total_w, total_h);
        ui->surf_w = total_w;
        ui->surf_h = total_h;
    }

    if (!ui->composited) {
        apply_non_composited_shape(ui, total_w, total_h, rad);
    }

    cairo_t *cr = cairo_create(ui->back_surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    ui->hit_count = 0;

    double px = (double)pad;
    double py = (double)pad;
    double pw = (double)pill_w;
    double ph = (double)pill_h;

    /* 1. Soft multi-layered drop shadow when composited */
    if (ui->composited) {
        for (int i = 3; i >= 1; i--) {
            double spread = (double)i * 2.6;
            path_rounded_rect(cr, px - spread * 0.4, py + spread * 0.65,
                              pw + spread * 0.8, ph + spread * 0.5, rad + spread * 0.4);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.14 / (double)i);
            cairo_fill(cr);
        }
    }

    /* 2. Obsidian Dynamic Island Pill Body */
    path_rounded_rect(cr, px, py, pw, ph, rad);
    double bg_alpha = ui->composited ? ui->cfg->opacity : 1.0;
    cairo_pattern_t *bg = cairo_pattern_create_linear(px, py, px, py + ph);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.035, 0.035, 0.042, bg_alpha);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.005, 0.005, 0.008, bg_alpha);
    cairo_set_source(cr, bg);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    /* Clip all interior contents cleanly inside the rounded pill */
    cairo_save(cr);
    path_rounded_rect(cr, px, py, pw, ph, rad);
    cairo_clip(cr);

    /* Subtle ambient radial accent glow when media is playing or toast active */
    const xdi_player_t *player = xdi_media_active(ui->media);
    bool playing = (player && player->status == XDI_PLAYBACK_PLAYING);
    if (playing || ui->mode == XDI_MODE_TOAST_NOTIF) {
        uint32_t glow_rgb = (ui->art->has_dominant && ui->mode != XDI_MODE_TOAST_NOTIF)
                                ? ui->art->dominant_rgb
                                : ui->cfg->accent_rgb;
        double gr = ((glow_rgb >> 16) & 0xFF) / 255.0;
        double gg = ((glow_rgb >> 8) & 0xFF) / 255.0;
        double gb = (glow_rgb & 0xFF) / 255.0;
        cairo_pattern_t *rad_g = cairo_pattern_create_radial(
            px + pw * 0.82, py + ph * 0.35, 4.0,
            px + pw * 0.82, py + ph * 0.35, pw * 0.55);
        cairo_pattern_add_color_stop_rgba(rad_g, 0.0, gr, gg, gb, 0.16);
        cairo_pattern_add_color_stop_rgba(rad_g, 1.0, gr, gg, gb, 0.0);
        cairo_set_source(cr, rad_g);
        cairo_rectangle(cr, px, py, pw, ph);
        cairo_fill(cr);
        cairo_pattern_destroy(rad_g);
    }

    const char *font = ui->cfg->font_family;

    /* 3. Mode-Specific Content */
    if (ui->mode == XDI_MODE_IDLE) {
        /* Sleek Idle Pill: Clock + Notification badge + Hover quick buttons */
        char time_str[16] = "";
        if (ui->cfg->show_clock_idle) {
            time_t now_t = time(NULL);
            struct tm tm_now;
            localtime_r(&now_t, &tm_now);
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);
        }

        /* Subtle living status indicator dot */
        double top[3], bot[3];
        xdi_accent_gradient(ui->cfg->accent_rgb, top, bot);
        cairo_arc(cr, px + 18.0, py + ph * 0.5, 3.5, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, top[0], top[1], top[2], 0.9);
        cairo_fill(cr);

        if (time_str[0]) {
            draw_text(cr, font, 10.0, true, 1.0, 1.0, 1.0, 0.94,
                      px + 30.0, py + (ph - 16.0) * 0.5, 64.0, PANGO_ALIGN_LEFT, time_str);
        }

        /* Notification bell button with unread badge */
        double bell_cx = px + pw - (ui->hovered ? 52.0 : 24.0);
        double bell_cy = py + ph * 0.5;
        draw_circle_button(cr, ui, XDI_HIT_NOTIF_BTN, bell_cx, bell_cy, 13.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ui->notifs->unread_count > 0 ? 0.98 : 0.65);
        draw_icon_bell(cr, bell_cx, bell_cy, 13.0);

        if (ui->notifs->unread_count > 0) {
            cairo_arc(cr, bell_cx + 6.0, bell_cy - 5.5, 4.2, 0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.24, 0.28, 1.0);
            cairo_fill(cr);
        }

        if (ui->hovered) {
            /* Settings gear icon appears on hover */
            double gear_cx = px + pw - 22.0;
            draw_circle_button(cr, ui, XDI_HIT_SETTINGS_BTN, gear_cx, bell_cy, 13.0, false);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
            draw_icon_gear(cr, gear_cx, bell_cy, 13.5);

            /* Volume mute quick button */
            double spk_cx = px + pw - 82.0;
            draw_circle_button(cr, ui, XDI_HIT_VOLUME_MUTE, spk_cx, bell_cy, 13.0, false);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ui->media->volume.muted ? 0.45 : 0.85);
            draw_icon_speaker(cr, spk_cx, bell_cy, 13.0, ui->media->volume.volume, ui->media->volume.muted);
        }
    } else if (ui->mode == XDI_MODE_COMPACT_MEDIA && player) {
        /* Compact Media Pill: Album Art + Title + Sexy Waveform */
        double art_sz = 26.0;
        draw_album_art(cr, ui, px + 6.0, py + (ph - art_sz) * 0.5, art_sz, true);

        double wf_w = 42.0;
        double wf_h = 18.0;
        double wf_x = px + pw - wf_w - 14.0;
        double wf_y = py + (ph - wf_h) * 0.5;
        draw_waveform(cr, ui, wf_x, wf_y, wf_w, wf_h, playing);

        double tx = px + 6.0 + art_sz + 10.0;
        double tw = wf_x - tx - 10.0;
        if (player->is_kdeconnect) {
            cairo_set_source_rgba(cr, 0.65, 0.82, 1.0, 0.85);
            draw_icon_phone(cr, tx + 4.5, py + ph * 0.5, 11.0);
            tx += 13.0;
            tw -= 13.0;
        }
        const char *label = player->title[0] ? player->title : player->identity;
        draw_text(cr, font, 9.5, true, 1.0, 1.0, 1.0, 0.96,
                  tx, py + (ph - 15.0) * 0.5, fmax(20.0, tw), PANGO_ALIGN_LEFT, label);

        /* Tiny red dot if unread notifications exist */
        if (ui->notifs->unread_count > 0) {
            cairo_arc(cr, px + pw - 8.0, py + 9.0, 3.2, 0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.26, 0.30, 0.95);
            cairo_fill(cr);
        }
    } else if (ui->mode == XDI_MODE_EXPANDED_MEDIA && player) {
        /* Expanded Media Card */
        double art_sz = 56.0;
        double art_x = px + 16.0;
        double art_y = py + 15.0;
        draw_album_art(cr, ui, art_x, art_y, art_sz, false);

        /* Top-right utility buttons (Settings + Notification Bell + Waveform) */
        double gear_cx = px + pw - 22.0;
        double bell_cx = px + pw - 50.0;
        double util_cy = py + 24.0;
        draw_circle_button(cr, ui, XDI_HIT_SETTINGS_BTN, gear_cx, util_cy, 12.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.78);
        draw_icon_gear(cr, gear_cx, util_cy, 13.0);

        draw_circle_button(cr, ui, XDI_HIT_NOTIF_BTN, bell_cx, util_cy, 12.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ui->notifs->unread_count > 0 ? 1.0 : 0.78);
        draw_icon_bell(cr, bell_cx, util_cy, 13.0);
        if (ui->notifs->unread_count > 0) {
            cairo_arc(cr, bell_cx + 5.5, util_cy - 5.0, 3.8, 0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.25, 0.28, 1.0);
            cairo_fill(cr);
        }

        /* Taller waveform right under utility buttons */
        draw_waveform(cr, ui, px + pw - 64.0, py + 43.0, 48.0, 24.0, playing);

        /* Track Metadata */
        double mx = art_x + art_sz + 13.0;
        double mw = (px + pw - 74.0) - mx;

        /* Source badge + multi-player switcher */
        double sx = mx;
        if (player->is_kdeconnect) {
            cairo_set_source_rgba(cr, 0.55, 0.80, 1.0, 0.92);
            draw_icon_phone(cr, sx + 4.5, py + 21.0, 11.0);
            sx += 13.0;
        }
        char src_buf[96];
        if (ui->media->player_count > 1) {
            snprintf(src_buf, sizeof(src_buf), "%s (%d/%d)",
                     player->identity, ui->media->active_index + 1, ui->media->player_count);
            add_hit(ui, XDI_HIT_PLAYER_NEXT, mx, py + 13.0, fmin(mw, 180.0), 17.0);
        } else {
            snprintf(src_buf, sizeof(src_buf), "%s", player->identity);
        }
        draw_text(cr, font, 7.8, true, 0.65, 0.67, 0.72, 0.95,
                  sx, py + 15.0, mw - (sx - mx), PANGO_ALIGN_LEFT, src_buf);

        const char *title = player->title[0] ? player->title : "Unknown Track";
        draw_text(cr, font, 10.5, true, 1.0, 1.0, 1.0, 0.98,
                  mx, py + 31.0, mw, PANGO_ALIGN_LEFT, title);

        const char *artist = player->artist[0] ? player->artist :
                             (player->album[0] ? player->album : (playing ? "Playing" : "Paused"));
        draw_text(cr, font, 8.8, false, 0.66, 0.67, 0.71, 0.92,
                  mx, py + 50.0, mw, PANGO_ALIGN_LEFT, artist);

        /* Progress / Scrub Bar */
        double bar_y = py + 86.0;
        double bar_x = px + 50.0;
        double bar_w = pw - 100.0;
        int64_t pos_us = xdi_media_current_position_us(player, now_ms);
        double frac = (player->length_us > 0) ? (double)pos_us / (double)player->length_us : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;

        char t_cur[16], t_tot[16];
        format_mmss(pos_us, t_cur, sizeof(t_cur));
        if (player->length_us > 0) {
            format_mmss(player->length_us, t_tot, sizeof(t_tot));
        } else {
            snprintf(t_tot, sizeof(t_tot), "--:--");
        }
        draw_text(cr, font, 7.8, false, 0.62, 0.63, 0.68, 0.9,
                  px + 14.0, bar_y - 4.0, 34.0, PANGO_ALIGN_LEFT, t_cur);
        draw_text(cr, font, 7.8, false, 0.62, 0.63, 0.68, 0.9,
                  px + pw - 46.0, bar_y - 4.0, 34.0, PANGO_ALIGN_RIGHT, t_tot);

        add_hit(ui, XDI_HIT_SEEK_BAR, bar_x - 4.0, bar_y - 7.0, bar_w + 8.0, 18.0);
        path_rounded_rect(cr, bar_x, bar_y, bar_w, 4.5, 2.25);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
        cairo_fill(cr);

        if (frac > 0.005) {
            double top[3], bot[3];
            xdi_accent_gradient(ui->cfg->accent_rgb, top, bot);
            cairo_pattern_t *pg = cairo_pattern_create_linear(bar_x, bar_y, bar_x + bar_w * frac, bar_y);
            cairo_pattern_add_color_stop_rgba(pg, 0.0, bot[0], bot[1], bot[2], 1.0);
            cairo_pattern_add_color_stop_rgba(pg, 1.0, top[0], top[1], top[2], 1.0);
            path_rounded_rect(cr, bar_x, bar_y, fmax(5.0, bar_w * frac), 4.5, 2.25);
            cairo_set_source(cr, pg);
            cairo_fill(cr);
            cairo_pattern_destroy(pg);

            if (ui->hover_hit_id == XDI_HIT_SEEK_BAR) {
                cairo_arc(cr, bar_x + bar_w * frac, bar_y + 2.25, 5.0, 0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
                cairo_fill(cr);
            }
        }

        /* Bottom Control Deck: Prev / PlayPause / Next + wpctl Volume Slider */
        double ctrl_cy = py + 126.0;
        double center_x = px + pw * 0.38;

        draw_circle_button(cr, ui, XDI_HIT_PREV, center_x - 50.0, ctrl_cy, 17.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        draw_icon_prev(cr, center_x - 50.0, ctrl_cy, 16.0);

        draw_circle_button(cr, ui, XDI_HIT_PLAY_PAUSE, center_x, ctrl_cy, 20.0, true);
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.98);
        if (playing) draw_icon_pause(cr, center_x, ctrl_cy, 17.0);
        else         draw_icon_play(cr, center_x + 1.0, ctrl_cy, 17.0);

        draw_circle_button(cr, ui, XDI_HIT_NEXT, center_x + 50.0, ctrl_cy, 17.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        draw_icon_next(cr, center_x + 50.0, ctrl_cy, 16.0);

        /* Right-side wpctl Volume Control */
        double vol_spk_x = px + pw - 116.0;
        draw_circle_button(cr, ui, XDI_HIT_VOLUME_MUTE, vol_spk_x, ctrl_cy, 14.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ui->media->volume.muted ? 0.45 : 0.9);
        draw_icon_speaker(cr, vol_spk_x, ctrl_cy, 14.0, ui->media->volume.volume, ui->media->volume.muted);

        double vbar_x = vol_spk_x + 18.0;
        double vbar_w = 78.0;
        add_hit(ui, XDI_HIT_VOLUME_BAR, vbar_x - 4.0, ctrl_cy - 10.0, vbar_w + 8.0, 20.0);
        path_rounded_rect(cr, vbar_x, ctrl_cy - 2.5, vbar_w, 5.0, 2.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
        cairo_fill(cr);

        double vf = ui->media->volume.muted ? 0.0 : fmin(1.0, ui->media->volume.volume);
        if (vf > 0.01) {
            path_rounded_rect(cr, vbar_x, ctrl_cy - 2.5, fmax(5.0, vbar_w * vf), 5.0, 2.5);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
            cairo_fill(cr);
        }
    } else if (ui->mode == XDI_MODE_TOAST_NOTIF) {
        /* Live Notification Toast Banner */
        const xdi_notification_t *n = &ui->notifs->latest;
        double badge_r = 18.0;
        double bx = px + 26.0;
        double by = py + ph * 0.5;
        double top[3], bot[3];
        xdi_accent_gradient(n->urgency >= 2 ? 0xFF3B30 : ui->cfg->accent_rgb, top, bot);
        cairo_pattern_t *g = cairo_pattern_create_linear(bx, by - badge_r, bx, by + badge_r);
        cairo_pattern_add_color_stop_rgba(g, 0.0, top[0], top[1], top[2], 1.0);
        cairo_pattern_add_color_stop_rgba(g, 1.0, bot[0], bot[1], bot[2], 1.0);
        cairo_arc(cr, bx, by, badge_r, 0, 2.0 * M_PI);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.98);
        draw_icon_bell(cr, bx, by, 16.0);

        double tx = px + 56.0;
        double tw = pw - 74.0;
        char header[260];
        snprintf(header, sizeof(header), "%s  ·  %s", n->app_name, n->summary);
        draw_text(cr, font, 9.5, true, 1.0, 1.0, 1.0, 0.98,
                  tx, py + 14.0, tw, PANGO_ALIGN_LEFT, header);
        if (n->body[0]) {
            draw_text(cr, font, 8.6, false, 0.72, 0.74, 0.78, 0.92,
                      tx, py + 35.0, tw, PANGO_ALIGN_LEFT, n->body);
        }
        add_hit(ui, XDI_HIT_NOTIF_BTN, px, py, pw, ph);
    } else if (ui->mode == XDI_MODE_TOAST_VOLUME) {
        /* Inline wpctl Volume OSD Toast */
        double cy = py + ph * 0.5;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ui->media->volume.muted ? 0.5 : 0.95);
        draw_icon_speaker(cr, px + 24.0, cy, 16.0, ui->media->volume.volume, ui->media->volume.muted);

        double bx = px + 44.0;
        double bw = pw - 96.0;
        path_rounded_rect(cr, bx, cy - 3.5, bw, 7.0, 3.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.2);
        cairo_fill(cr);

        double vf = ui->media->volume.muted ? 0.0 : fmin(1.0, ui->media->volume.volume);
        if (vf > 0.01) {
            double top[3], bot[3];
            xdi_accent_gradient(ui->cfg->accent_rgb, top, bot);
            cairo_pattern_t *g = cairo_pattern_create_linear(bx, cy, bx + bw * vf, cy);
            cairo_pattern_add_color_stop_rgba(g, 0.0, bot[0], bot[1], bot[2], 1.0);
            cairo_pattern_add_color_stop_rgba(g, 1.0, top[0], top[1], top[2], 1.0);
            path_rounded_rect(cr, bx, cy - 3.5, fmax(7.0, bw * vf), 7.0, 3.5);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
        }

        char pct[16];
        if (ui->media->volume.muted) snprintf(pct, sizeof(pct), "Mute");
        else snprintf(pct, sizeof(pct), "%d%%", (int)round(ui->media->volume.volume * 100.0));
        draw_text(cr, font, 9.0, true, 1.0, 1.0, 1.0, 0.94,
                  px + pw - 46.0, cy - 8.0, 36.0, PANGO_ALIGN_RIGHT, pct);
    } else if (ui->mode == XDI_MODE_NOTIF_HISTORY) {
        /* Scrollable Notification History Drawer (from notification-daemon) */
        double hdr_y = py + 14.0;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
        draw_icon_bell(cr, px + 24.0, hdr_y + 9.0, 14.0);

        char title[64];
        snprintf(title, sizeof(title), "Notifications (%d)", ui->notifs->count);
        draw_text(cr, font, 10.0, true, 1.0, 1.0, 1.0, 0.98,
                  px + 40.0, hdr_y + 1.0, 190.0, PANGO_ALIGN_LEFT, title);

        /* Clear All button */
        if (ui->notifs->count > 0) {
            double cx = px + pw - 96.0;
            double cy = hdr_y - 1.0;
            bool hov = (ui->hover_hit_id == XDI_HIT_CLEAR_NOTIFS);
            add_hit(ui, XDI_HIT_CLEAR_NOTIFS, cx, cy, 56.0, 22.0);
            path_rounded_rect(cr, cx, cy, 56.0, 22.0, 11.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hov ? 0.22 : 0.11);
            cairo_fill(cr);
            draw_text(cr, font, 8.2, true, 1.0, 1.0, 1.0, 0.92,
                      cx, cy + 4.0, 56.0, PANGO_ALIGN_CENTER, "Clear");
        }

        /* Close button */
        double close_cx = px + pw - 22.0;
        double close_cy = hdr_y + 10.0;
        draw_circle_button(cr, ui, XDI_HIT_CLOSE_NOTIFS, close_cx, close_cy, 11.0, false);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.86);
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, close_cx - 4.0, close_cy - 4.0);
        cairo_line_to(cr, close_cx + 4.0, close_cy + 4.0);
        cairo_move_to(cr, close_cx + 4.0, close_cy - 4.0);
        cairo_line_to(cr, close_cx - 4.0, close_cy + 4.0);
        cairo_stroke(cr);

        /* Divider line */
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, px + 14.0, py + 44.0);
        cairo_line_to(cr, px + pw - 14.0, py + 44.0);
        cairo_stroke(cr);

        double list_top = py + 50.0;
        double list_h = ph - 60.0;

        if (ui->notifs->count == 0) {
            draw_text(cr, font, 9.2, false, 0.62, 0.64, 0.69, 0.9,
                      px + 18.0, list_top + 16.0, pw - 36.0, PANGO_ALIGN_CENTER,
                      "No notifications yet");
            draw_text(cr, font, 7.8, false, 0.48, 0.50, 0.56, 0.85,
                      px + 18.0, list_top + 38.0, pw - 36.0, PANGO_ALIGN_CENTER,
                      "Monitoring notification-daemon via D-Bus");
        } else {
            double row_h = 56.0;
            double gap = 6.0;
            double content_h = ui->notifs->count * (row_h + gap);
            ui->notif_scroll_max = (content_h > list_h) ? (int)(content_h - list_h) : 0;
            if (ui->notif_scroll < 0.0) ui->notif_scroll = 0.0;
            if (ui->notif_scroll > ui->notif_scroll_max) ui->notif_scroll = ui->notif_scroll_max;

            cairo_save(cr);
            cairo_rectangle(cr, px + 10.0, list_top, pw - 20.0, list_h);
            cairo_clip(cr);

            time_t now_t = time(NULL);
            for (int i = 0; i < ui->notifs->count; i++) {
                double ry = list_top + i * (row_h + gap) - ui->notif_scroll;
                if (ry + row_h < list_top || ry > list_top + list_h) continue;

                const xdi_notification_t *n = &ui->notifs->items[i];
                double rx = px + 12.0;
                double rw = pw - 24.0;
                path_rounded_rect(cr, rx, ry, rw, row_h, 13.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.07);
                cairo_fill(cr);

                /* Urgency accent dot */
                double top[3], bot[3];
                xdi_accent_gradient(n->urgency >= 2 ? 0xFF3B30 : ui->cfg->accent_rgb, top, bot);
                cairo_arc(cr, rx + 12.0, ry + 15.0, 3.2, 0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, top[0], top[1], top[2], 0.95);
                cairo_fill(cr);

                draw_text(cr, font, 8.0, true, 0.70, 0.73, 0.79, 0.95,
                          rx + 22.0, ry + 8.0, rw - 110.0, PANGO_ALIGN_LEFT, n->app_name);

                char tbuf[24];
                xdi_notif_format_time(n->timestamp, now_t, tbuf, sizeof(tbuf));
                draw_text(cr, font, 7.6, false, 0.55, 0.57, 0.62, 0.9,
                          rx + rw - 78.0, ry + 8.0, 46.0, PANGO_ALIGN_RIGHT, tbuf);

                /* Per-notification dismiss 'x' */
                if (i < 24) {
                    int hid = XDI_HIT_NOTIF_DISMISS_BASE + i;
                    double dx = rx + rw - 17.0;
                    double dy = ry + 15.0;
                    draw_circle_button(cr, ui, hid, dx, dy, 9.0, false);
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.62);
                    cairo_set_line_width(cr, 1.3);
                    cairo_move_to(cr, dx - 3.0, dy - 3.0);
                    cairo_line_to(cr, dx + 3.0, dy + 3.0);
                    cairo_move_to(cr, dx + 3.0, dy - 3.0);
                    cairo_line_to(cr, dx - 3.0, dy + 3.0);
                    cairo_stroke(cr);
                }

                draw_text(cr, font, 9.0, true, 1.0, 1.0, 1.0, 0.96,
                          rx + 12.0, ry + 22.0, rw - 28.0, PANGO_ALIGN_LEFT,
                          n->summary[0] ? n->summary : "(No title)");
                if (n->body[0]) {
                    draw_text(cr, font, 8.0, false, 0.66, 0.68, 0.73, 0.88,
                              rx + 12.0, ry + 38.0, rw - 24.0, PANGO_ALIGN_LEFT, n->body);
                }
            }
            cairo_restore(cr);
        }
    }

    cairo_restore(cr); /* End interior clip */

    /* 4. Crisp Specular Hardware Rim Highlight */
    path_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1.0, ph - 1.0, rad);
    cairo_pattern_t *rim = cairo_pattern_create_linear(px, py, px, py + ph);
    cairo_pattern_add_color_stop_rgba(rim, 0.0, 1.0, 1.0, 1.0, 0.20);
    cairo_pattern_add_color_stop_rgba(rim, 0.45, 1.0, 1.0, 1.0, 0.07);
    cairo_pattern_add_color_stop_rgba(rim, 1.0, 1.0, 1.0, 1.0, 0.12);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);

    cairo_destroy(cr);

    /* Blit back buffer to X11 window surface atomically (zero flicker!) */
    cairo_t *xcr = cairo_create(ui->xlib_surf);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(xcr, ui->back_surf, 0, 0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    cairo_surface_flush(ui->xlib_surf);
    XFlush(ui->dpy);
}

/* ---------- Public Lifecycle, Event Handling, and Tick ---------- */

bool xdi_ui_init(xdi_ui_t *ui, Display *dpy, xdi_config_t *cfg,
                 xdi_media_t *media, xdi_notif_store_t *notifs,
                 xdi_audio_viz_t *viz, xdi_art_loader_t *art) {
    memset(ui, 0, sizeof(*ui));
    ui->dpy = dpy;
    ui->screen = DefaultScreen(dpy);
    ui->root = RootWindow(dpy, ui->screen);
    ui->win = None;
    ui->cfg = cfg;
    ui->media = media;
    ui->notifs = notifs;
    ui->viz = viz;
    ui->art = art;

    char cm_name[32];
    snprintf(cm_name, sizeof(cm_name), "_NET_WM_CM_S%d", ui->screen);
    ui->atom_cm = XInternAtom(dpy, cm_name, False);
    ui->atom_wm_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    ui->atom_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    ui->atom_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    ui->atom_state_sticky = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    ui->atom_state_skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    ui->atom_state_skip_pager = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    ui->atom_motif_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    ui->atom_net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

    /* Listen to Root SubstructureNotify for instant zero-poll dmenu / rofi detection! */
    XSelectInput(dpy, ui->root, SubstructureNotifyMask | PropertyChangeMask);
    XRRSelectInput(dpy, ui->root, RRScreenChangeNotifyMask);

    xdi_ui_update_monitor(ui);

    xdi_spring_init(&ui->sp_w, 142.0, 0.40, 0.78);
    xdi_spring_init(&ui->sp_h, 34.0, 0.40, 0.78);
    xdi_spring_init(&ui->sp_r, 17.0, 0.40, 0.78);
    xdi_spring_init(&ui->sp_x, ui->monitor.x + (ui->monitor.width - 142.0) * 0.5, 0.36, 0.82);
    xdi_spring_init(&ui->sp_y, ui->monitor.y + cfg->margin, 0.32, 0.82);
    xdi_spring_init(&ui->sp_expand, 0.0, 0.38, 0.80);

    ui->mode = XDI_MODE_IDLE;
    ui->obstacles_dirty = true;
    ui->needs_redraw = true;

    if (!create_island_window(ui)) return false;

    /* Initial geometry snap */
    double tw, th, tr, tx, ty;
    compute_target_geometry(ui, 0, &tw, &th, &tr, &tx, &ty);
    xdi_spring_snap(&ui->sp_w, tw);
    xdi_spring_snap(&ui->sp_h, th);
    xdi_spring_snap(&ui->sp_r, tr);
    xdi_spring_snap(&ui->sp_x, tx);
    xdi_spring_snap(&ui->sp_y, ty);

    if (ui->mode != XDI_MODE_HIDDEN) {
        XMapWindow(ui->dpy, ui->win);
        if (ui->cfg->always_on_top) XRaiseWindow(ui->dpy, ui->win);
        ui->mapped = true;
    }

    return true;
}

void xdi_ui_destroy(xdi_ui_t *ui) {
    if (!ui) return;
    destroy_window_surfaces(ui);
    if (ui->win != None && ui->dpy) {
        XDestroyWindow(ui->dpy, ui->win);
        ui->win = None;
    }
    if (ui->dpy && ui->depth == 32 && ui->colormap != DefaultColormap(ui->dpy, ui->screen)) {
        XFreeColormap(ui->dpy, ui->colormap);
    }
}

void xdi_ui_apply_config(xdi_ui_t *ui, Window settings_win) {
    xdi_ui_update_monitor(ui);
    xdi_audio_viz_set_live_enabled(ui->viz, ui->cfg->real_audio_viz);

    bool comp = detect_compositor(ui);
    if (ui->window_is_override != ui->cfg->always_on_top || comp != ui->composited) {
        bool was_mapped = ui->mapped;
        create_island_window(ui);
        if (was_mapped && ui->mode != XDI_MODE_HIDDEN) {
            XMapWindow(ui->dpy, ui->win);
            if (ui->cfg->always_on_top) XRaiseWindow(ui->dpy, ui->win);
            ui->mapped = true;
        }
    } else {
        apply_ewmh_hints(ui);
    }

    ui->obstacles = xdi_detect_obstacles(ui->dpy, ui->root, ui->win, settings_win,
                                         ui->monitor,
                                         (int)round(ui->sp_x.target),
                                         (int)round(ui->sp_w.target),
                                         ui->cfg->avoid_obstacles,
                                         ui->cfg->avoid_panels);
    ui->needs_redraw = true;
}

void xdi_ui_show_volume_toast(xdi_ui_t *ui, int64_t now_ms) {
    if (ui->mode == XDI_MODE_EXPANDED_MEDIA || ui->mode == XDI_MODE_NOTIF_HISTORY) {
        ui->needs_redraw = true;
        return;
    }
    ui->mode = XDI_MODE_TOAST_VOLUME;
    ui->toast_until_ms = now_ms + 2200;
    ui->needs_redraw = true;
}

void xdi_ui_show_notif_toast(xdi_ui_t *ui, int64_t now_ms) {
    if (ui->mode == XDI_MODE_NOTIF_HISTORY) {
        ui->needs_redraw = true;
        return;
    }
    ui->mode = XDI_MODE_TOAST_NOTIF;
    ui->toast_until_ms = now_ms + ui->cfg->toast_duration_ms;
    ui->needs_redraw = true;
}

static void handle_click(xdi_ui_t *ui, int button, double x, double y, int64_t now_ms) {
    /* Scroll wheel: scroll notification history or adjust wpctl volume */
    if (button == 4 || button == 5) {
        if (ui->mode == XDI_MODE_NOTIF_HISTORY) {
            ui->notif_scroll += (button == 4) ? -32.0 : 32.0;
            if (ui->notif_scroll < 0.0) ui->notif_scroll = 0.0;
            if (ui->notif_scroll > ui->notif_scroll_max) ui->notif_scroll = ui->notif_scroll_max;
            ui->needs_redraw = true;
        } else {
            int delta = (button == 4) ? ui->cfg->volume_step : -ui->cfg->volume_step;
            xdi_media_volume_delta(ui->media, delta);
            xdi_ui_show_volume_toast(ui, now_ms);
        }
        return;
    }

    /* Right-click toggles Settings (or dismisses toast) */
    if (button == 3) {
        if (ui->mode == XDI_MODE_TOAST_NOTIF || ui->mode == XDI_MODE_TOAST_VOLUME) {
            ui->toast_until_ms = now_ms;
            ui->needs_redraw = true;
        } else if (ui->mode == XDI_MODE_NOTIF_HISTORY) {
            ui->mode = XDI_MODE_IDLE;
            ui->pinned_open = false;
            ui->needs_redraw = true;
        } else {
            ui->open_settings_requested = true;
        }
        return;
    }

    if (button != 1) return;

    int hit = find_hit(ui, x, y);
    switch (hit) {
        case XDI_HIT_PREV:
            xdi_media_prev(ui->media);
            break;
        case XDI_HIT_PLAY_PAUSE:
            xdi_media_play_pause(ui->media);
            break;
        case XDI_HIT_NEXT:
            xdi_media_next(ui->media);
            break;
        case XDI_HIT_PLAYER_NEXT:
            xdi_media_cycle_player(ui->media, 1);
            break;
        case XDI_HIT_SEEK_BAR: {
            for (int i = 0; i < ui->hit_count; i++) {
                if (ui->hits[i].id == XDI_HIT_SEEK_BAR) {
                    double frac = (x - ui->hits[i].x) / ui->hits[i].w;
                    xdi_media_seek_fraction(ui->media, frac);
                    break;
                }
            }
            break;
        }
        case XDI_HIT_VOLUME_MUTE:
            xdi_media_toggle_mute(ui->media);
            ui->needs_redraw = true;
            break;
        case XDI_HIT_VOLUME_BAR: {
            for (int i = 0; i < ui->hit_count; i++) {
                if (ui->hits[i].id == XDI_HIT_VOLUME_BAR) {
                    double frac = (x - ui->hits[i].x) / ui->hits[i].w;
                    xdi_media_volume_set(ui->media, frac);
                    ui->needs_redraw = true;
                    break;
                }
            }
            break;
        }
        case XDI_HIT_NOTIF_BTN:
            ui->mode = XDI_MODE_NOTIF_HISTORY;
            ui->pinned_open = true;
            ui->notif_scroll = 0.0;
            xdi_notif_mark_all_read(ui->notifs);
            ui->needs_redraw = true;
            break;
        case XDI_HIT_SETTINGS_BTN:
            ui->open_settings_requested = true;
            break;
        case XDI_HIT_CLEAR_NOTIFS:
            xdi_notif_clear(ui->notifs);
            ui->needs_redraw = true;
            break;
        case XDI_HIT_CLOSE_NOTIFS:
            ui->mode = XDI_MODE_IDLE;
            ui->pinned_open = false;
            ui->needs_redraw = true;
            break;
        default:
            if (hit >= XDI_HIT_NOTIF_DISMISS_BASE) {
                int idx = hit - XDI_HIT_NOTIF_DISMISS_BASE;
                xdi_notif_remove_at(ui->notifs, idx);
                ui->needs_redraw = true;
            } else if (ui->mode == XDI_MODE_COMPACT_MEDIA) {
                ui->pinned_open = true;
                ui->mode = XDI_MODE_EXPANDED_MEDIA;
                ui->needs_redraw = true;
            } else if (ui->mode == XDI_MODE_IDLE) {
                ui->mode = XDI_MODE_NOTIF_HISTORY;
                ui->pinned_open = true;
                xdi_notif_mark_all_read(ui->notifs);
                ui->needs_redraw = true;
            }
            break;
    }
}

void xdi_ui_handle_event(xdi_ui_t *ui, const XEvent *ev, int64_t now_ms) {
    /* Root window SubstructureNotify: instant notification when dmenu/rofi/bars map or close! */
    if (ev->xany.window == ui->root) {
        switch (ev->type) {
            case MapNotify:
            case UnmapNotify:
            case ConfigureNotify:
            case DestroyNotify:
                if (ev->xmap.window != ui->win) {
                    ui->obstacles_dirty = true;
                }
                break;
            case PropertyNotify:
                ui->obstacles_dirty = true;
                break;
            default:
                /* XRandR screen change */
                xdi_ui_update_monitor(ui);
                ui->obstacles_dirty = true;
                ui->needs_redraw = true;
                break;
        }
        return;
    }

    if (ev->xany.window != ui->win) return;

    switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) ui->needs_redraw = true;
            break;
        case EnterNotify:
            ui->hovered = true;
            ui->hover_leave_ms = 0;
            ui->needs_redraw = true;
            break;
        case LeaveNotify:
            if (ev->xcrossing.mode == NotifyNormal && ev->xcrossing.detail != NotifyInferior) {
                ui->hover_leave_ms = now_ms;
                ui->hover_hit_id = XDI_HIT_NONE;
            }
            break;
        case MotionNotify: {
            int hit = find_hit(ui, (double)ev->xmotion.x, (double)ev->xmotion.y);
            if (hit != ui->hover_hit_id) {
                ui->hover_hit_id = hit;
                ui->needs_redraw = true;
            }
            break;
        }
        case ButtonPress:
            handle_click(ui, (int)ev->xbutton.button,
                         (double)ev->xbutton.x, (double)ev->xbutton.y, now_ms);
            break;
        default:
            break;
    }
}

int xdi_ui_tick(xdi_ui_t *ui, int64_t now_ms, Window settings_win) {
    double dt = (ui->last_frame_ms > 0) ? (double)(now_ms - ui->last_frame_ms) / 1000.0 : 0.016;
    if (dt < 0.001) dt = 0.001;
    if (dt > 0.08) dt = 0.08;
    ui->last_frame_ms = now_ms;

    /* Graceful 260ms hover-leave debounce so moving mouse across buttons doesn't flicker */
    if (ui->hover_leave_ms > 0 && (now_ms - ui->hover_leave_ms) >= 260) {
        ui->hovered = false;
        ui->hover_leave_ms = 0;
        if (ui->mode == XDI_MODE_EXPANDED_MEDIA) {
            ui->pinned_open = false;
        }
        ui->needs_redraw = true;
    }

    /* Check for media / volume / notification events */
    if (ui->media->state_changed) {
        ui->media->state_changed = false;
        const xdi_player_t *p = xdi_media_active(ui->media);
        xdi_art_request(ui->art, p ? p->art_url : "");
        ui->needs_redraw = true;
    }
    if (ui->media->volume_changed) {
        ui->media->volume_changed = false;
        xdi_ui_show_volume_toast(ui, now_ms);
    }
    if (ui->notifs->has_new_toast) {
        ui->notifs->has_new_toast = false;
        xdi_ui_show_notif_toast(ui, now_ms);
    }
    if (xdi_art_poll(ui->art)) {
        ui->needs_redraw = true;
    }

    /* Refresh dmenu / rofi / top-panel obstacles whenever root tree changes (or periodically) */
    if (ui->obstacles_dirty || (now_ms - ui->last_obstacle_check_ms) >= 600) {
        ui->obstacles_dirty = false;
        ui->last_obstacle_check_ms = now_ms;

        xdi_obstacle_result_t prev = ui->obstacles;
        ui->obstacles = xdi_detect_obstacles(ui->dpy, ui->root, ui->win, settings_win,
                                             ui->monitor,
                                             (int)round(ui->sp_x.target),
                                             (int)round(ui->sp_w.target),
                                             ui->cfg->avoid_obstacles,
                                             ui->cfg->avoid_panels);
        if (ui->obstacles.top_clearance != prev.top_clearance ||
            ui->obstacles.bottom_clearance != prev.bottom_clearance) {
            ui->needs_redraw = true;
        }

        /* Stacking order maintenance: stay above normal windows if always_on_top,
         * while yielding cleanly below active dmenu/launcher blockers */
        if (ui->mapped && ui->cfg->always_on_top) {
            Window blocker = ui->obstacles.top_blocker != None
                                 ? ui->obstacles.top_blocker
                                 : ui->obstacles.bottom_blocker;
            if (blocker != None && ui->cfg->avoid_obstacles) {
                XWindowChanges wc;
                wc.sibling = blocker;
                wc.stack_mode = Below;
                XConfigureWindow(ui->dpy, ui->win, CWSibling | CWStackMode, &wc);
            } else {
                XRaiseWindow(ui->dpy, ui->win);
            }
        }
    }

    /* Recompute target dimensions & position */
    double tw, th, tr, tx, ty;
    compute_target_geometry(ui, now_ms, &tw, &th, &tr, &tx, &ty);

    /* Map / unmap for hide_when_idle */
    if (ui->mode == XDI_MODE_HIDDEN) {
        if (ui->mapped) {
            XUnmapWindow(ui->dpy, ui->win);
            ui->mapped = false;
            XFlush(ui->dpy);
        }
        return 350;
    } else if (!ui->mapped) {
        xdi_spring_snap(&ui->sp_x, tx);
        xdi_spring_snap(&ui->sp_y, ty);
        XMapWindow(ui->dpy, ui->win);
        if (ui->cfg->always_on_top) XRaiseWindow(ui->dpy, ui->win);
        ui->mapped = true;
        ui->needs_redraw = true;
    }

    xdi_spring_set_target(&ui->sp_w, tw);
    xdi_spring_set_target(&ui->sp_h, th);
    xdi_spring_set_target(&ui->sp_r, tr);
    xdi_spring_set_target(&ui->sp_x, tx);
    xdi_spring_set_target(&ui->sp_y, ty);

    bool animating = false;
    animating |= xdi_spring_step(&ui->sp_w, dt);
    animating |= xdi_spring_step(&ui->sp_h, dt);
    animating |= xdi_spring_step(&ui->sp_r, dt);
    animating |= xdi_spring_step(&ui->sp_x, dt);
    animating |= xdi_spring_step(&ui->sp_y, dt);

    const xdi_player_t *player = xdi_media_active(ui->media);
    bool playing = (player && player->status == XDI_PLAYBACK_PLAYING);
    bool is_remote = (player && player->is_kdeconnect);

    bool viz_moving = xdi_audio_viz_step(ui->viz, playing, is_remote, dt, now_ms);
    if (viz_moving && (ui->mode == XDI_MODE_COMPACT_MEDIA || ui->mode == XDI_MODE_EXPANDED_MEDIA)) {
        animating = true;
    }

    if (playing && ui->cfg->spin_art && ui->mode == XDI_MODE_COMPACT_MEDIA) {
        ui->art_angle += dt * 0.65;
        if (ui->art_angle > 2.0 * M_PI) ui->art_angle -= 2.0 * M_PI;
        animating = true;
    }

    /* Update clock minute / toast expiration / seek bar */
    static int last_minute = -1;
    if (ui->mode == XDI_MODE_IDLE && ui->cfg->show_clock_idle) {
        time_t t = time(NULL);
        int m = (int)(t / 60);
        if (m != last_minute) {
            last_minute = m;
            ui->needs_redraw = true;
        }
    }

    if (animating || ui->needs_redraw) {
        ui->needs_redraw = false;
        render_frame(ui, now_ms);
    }

    if (animating) return 16; /* Smooth 60fps */
    if (ui->hover_leave_ms > 0) return 40;
    if (ui->mode == XDI_MODE_TOAST_NOTIF || ui->mode == XDI_MODE_TOAST_VOLUME) {
        int rem = (int)(ui->toast_until_ms - now_ms);
        return (rem > 20 && rem < 250) ? rem : 100;
    }
    return 450; /* Low-power idle sleep (0.0% CPU!) */
}
