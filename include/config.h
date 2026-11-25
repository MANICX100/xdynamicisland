#ifndef XDI_CONFIG_H
#define XDI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XDI_VERSION "1.0.0"

typedef enum {
    XDI_POS_TOP_CENTER = 0,
    XDI_POS_TOP_LEFT,
    XDI_POS_TOP_RIGHT,
    XDI_POS_BOTTOM_CENTER,
    XDI_POS_BOTTOM_LEFT,
    XDI_POS_BOTTOM_RIGHT,
    XDI_POS_COUNT
} xdi_position_t;

typedef enum {
    XDI_EQ_WAVE = 0,
    XDI_EQ_BARS,
    XDI_EQ_RIBBON,
    XDI_EQ_SPECTRUM,
    XDI_EQ_SEGMENTS,
    XDI_EQ_SOLID,
    XDI_EQ_COUNT
} xdi_eq_style_t;

typedef enum {
    XDI_ART_CIRCLE = 0,
    XDI_ART_ROUNDED,
    XDI_ART_SQUARE,
    XDI_ART_COUNT
} xdi_art_shape_t;

typedef struct {
    bool always_on_top;        /* Appear above all normal windows */
    bool avoid_obstacles;      /* Reposition below dmenu / rofi / top overlays */
    bool avoid_panels;         /* Also respect EWMH dock panels / struts */
    bool hide_when_idle;       /* Hide island when no media or notifications active */
    bool spin_art;             /* Slowly rotate circular album art while playing */
    bool real_audio_viz;       /* Capture live PipeWire/Pulse monitor for waveform */
    bool show_clock_idle;      /* Show clock in idle pill */
    bool persist_notifications;/* Persist notification history across restarts */

    xdi_position_t position;
    xdi_eq_style_t eq_style;
    xdi_art_shape_t art_shape;

    int monitor_index;         /* -1 = primary/auto, >=0 = explicit XRandR index */
    int margin;                /* Margin from screen edge (px) */
    int obstacle_gap;          /* Gap between obstacle (e.g. dmenu) and island (px) */
    int toast_duration_ms;     /* Notification toast duration (ms) */
    int max_history;           /* Max notifications retained in history */
    int volume_step;           /* Volume increment per scroll tick (%) */

    uint32_t accent_rgb;       /* 0xRRGGBB accent color */
    double opacity;            /* Background opacity when composited (0.5..1.0) */
    char font_family[64];      /* Preferred Pango font family */
} xdi_config_t;

/* Populate default configuration values. */
void xdi_config_defaults(xdi_config_t *cfg);

/* Resolve default config file path (~/.config/xdynamicisland/config.ini). */
void xdi_config_default_path(char *buf, size_t len);

/* Load configuration from INI file. Returns true if file existed and was parsed. */
bool xdi_config_load(xdi_config_t *cfg, const char *path);

/* Save configuration to INI file (creating parent directories). */
bool xdi_config_save(const xdi_config_t *cfg, const char *path);

/* Parse a single key=value pair into cfg. Returns true if recognized. */
bool xdi_config_set_kv(xdi_config_t *cfg, const char *key, const char *val);

/* Enum <-> string conversions. */
const char *xdi_position_to_str(xdi_position_t pos);
xdi_position_t xdi_position_from_str(const char *str);
const char *xdi_eq_style_to_str(xdi_eq_style_t style);
xdi_eq_style_t xdi_eq_style_from_str(const char *str);
const char *xdi_art_shape_to_str(xdi_art_shape_t shape);
xdi_art_shape_t xdi_art_shape_from_str(const char *str);

/* Parse hex color "#RRGGBB" or "RRGGBB". Returns true on success. */
bool xdi_parse_hex_rgb(const char *str, uint32_t *out_rgb);

/* Derive a lighter/warmer highlight RGB from an accent color for glossy gradients. */
void xdi_accent_gradient(uint32_t rgb, double top_rgb[3], double bot_rgb[3]);

#endif /* XDI_CONFIG_H */
