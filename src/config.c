#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void xdi_config_defaults(xdi_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->always_on_top = true;
    cfg->avoid_obstacles = true;
    cfg->avoid_panels = true;
    cfg->hide_when_idle = false;
    cfg->spin_art = true;
    cfg->real_audio_viz = true;
    cfg->show_clock_idle = true;
    cfg->persist_notifications = true;

    cfg->position = XDI_POS_TOP_CENTER;
    cfg->eq_style = XDI_EQ_WAVE;
    cfg->art_shape = XDI_ART_CIRCLE;

    cfg->monitor_index = -1;
    cfg->margin = 10;
    cfg->obstacle_gap = 8;
    cfg->toast_duration_ms = 4500;
    cfg->max_history = 64;
    cfg->volume_step = 5;

    cfg->accent_rgb = 0xFF6A00; /* Warm Apple-inspired amber-orange */
    cfg->opacity = 0.96;
    snprintf(cfg->font_family, sizeof(cfg->font_family), "%s", "Sans");
}

void xdi_config_default_path(char *buf, size_t len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] != '\0') {
        snprintf(buf, len, "%s/xdynamicisland/config.ini", xdg);
    } else if (home && home[0] != '\0') {
        snprintf(buf, len, "%s/.config/xdynamicisland/config.ini", home);
    } else {
        snprintf(buf, len, "/tmp/xdynamicisland.ini");
    }
}

static bool parse_bool(const char *val) {
    if (!val) return false;
    return strcmp(val, "1") == 0 ||
           strcasecmp(val, "true") == 0 ||
           strcasecmp(val, "yes") == 0 ||
           strcasecmp(val, "on") == 0;
}

const char *xdi_position_to_str(xdi_position_t pos) {
    switch (pos) {
        case XDI_POS_TOP_CENTER:    return "top-center";
        case XDI_POS_TOP_LEFT:      return "top-left";
        case XDI_POS_TOP_RIGHT:     return "top-right";
        case XDI_POS_BOTTOM_CENTER: return "bottom-center";
        case XDI_POS_BOTTOM_LEFT:   return "bottom-left";
        case XDI_POS_BOTTOM_RIGHT:  return "bottom-right";
        default:                    return "top-center";
    }
}

xdi_position_t xdi_position_from_str(const char *str) {
    if (!str) return XDI_POS_TOP_CENTER;
    if (strcasecmp(str, "top-left") == 0)      return XDI_POS_TOP_LEFT;
    if (strcasecmp(str, "top-right") == 0)     return XDI_POS_TOP_RIGHT;
    if (strcasecmp(str, "bottom-center") == 0) return XDI_POS_BOTTOM_CENTER;
    if (strcasecmp(str, "bottom-left") == 0)   return XDI_POS_BOTTOM_LEFT;
    if (strcasecmp(str, "bottom-right") == 0)  return XDI_POS_BOTTOM_RIGHT;
    return XDI_POS_TOP_CENTER;
}

const char *xdi_eq_style_to_str(xdi_eq_style_t style) {
    switch (style) {
        case XDI_EQ_WAVE:     return "wave";
        case XDI_EQ_BARS:     return "bars";
        case XDI_EQ_RIBBON:   return "ribbon";
        case XDI_EQ_SPECTRUM: return "spectrum";
        case XDI_EQ_SEGMENTS: return "segments";
        case XDI_EQ_SOLID:    return "solid";
        default:              return "wave";
    }
}

xdi_eq_style_t xdi_eq_style_from_str(const char *str) {
    if (!str) return XDI_EQ_WAVE;
    if (strcasecmp(str, "bars") == 0)     return XDI_EQ_BARS;
    if (strcasecmp(str, "ribbon") == 0)   return XDI_EQ_RIBBON;
    if (strcasecmp(str, "spectrum") == 0) return XDI_EQ_SPECTRUM;
    if (strcasecmp(str, "segments") == 0) return XDI_EQ_SEGMENTS;
    if (strcasecmp(str, "solid") == 0)    return XDI_EQ_SOLID;
    return XDI_EQ_WAVE;
}

const char *xdi_art_shape_to_str(xdi_art_shape_t shape) {
    switch (shape) {
        case XDI_ART_CIRCLE:  return "circle";
        case XDI_ART_ROUNDED: return "rounded";
        case XDI_ART_SQUARE:  return "square";
        default:              return "circle";
    }
}

xdi_art_shape_t xdi_art_shape_from_str(const char *str) {
    if (!str) return XDI_ART_CIRCLE;
    if (strcasecmp(str, "rounded") == 0) return XDI_ART_ROUNDED;
    if (strcasecmp(str, "square") == 0)  return XDI_ART_SQUARE;
    return XDI_ART_CIRCLE;
}

bool xdi_parse_hex_rgb(const char *str, uint32_t *out_rgb) {
    if (!str || !out_rgb) return false;
    if (*str == '#') str++;
    if (strlen(str) != 6) return false;
    uint32_t val = 0;
    for (int i = 0; i < 6; i++) {
        char c = str[i];
        uint32_t nib;
        if (c >= '0' && c <= '9')      nib = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') nib = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nib = (uint32_t)(c - 'A' + 10);
        else return false;
        val = (val << 4) | nib;
    }
    *out_rgb = val;
    return true;
}

void xdi_accent_gradient(uint32_t rgb, double top_rgb[3], double bot_rgb[3]) {
    double r = ((rgb >> 16) & 0xFF) / 255.0;
    double g = ((rgb >> 8) & 0xFF) / 255.0;
    double b = (rgb & 0xFF) / 255.0;
    bot_rgb[0] = r;
    bot_rgb[1] = g;
    bot_rgb[2] = b;

    /* Convert RGB -> HSL, shift hue slightly warmer (+16 deg) and lift lightness */
    double mx = fmax(r, fmax(g, b));
    double mn = fmin(r, fmin(g, b));
    double d = mx - mn;
    double l = (mx + mn) * 0.5;
    double s = (d > 1e-6) ? d / (1.0 - fabs(2.0 * l - 1.0)) : 0.0;
    double h = 0.0;
    if (d > 1e-6) {
        if (mx == r)      h = fmod((g - b) / d, 6.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else              h = (r - g) / d + 4.0;
        h *= 60.0;
        if (h < 0.0) h += 360.0;
    }

    double hue = fmod(h + 18.0, 360.0);
    double light = fmin(l + 0.16, 0.92);
    double a = s * fmin(light, 1.0 - light);

    const double offsets[3] = {0.0, 8.0, 4.0};
    for (int i = 0; i < 3; i++) {
        double k = fmod(offsets[i] + hue / 30.0, 12.0);
        double f = light - a * fmax(-1.0, fmin(k - 3.0, fmin(9.0 - k, 1.0)));
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
        top_rgb[i] = f;
    }
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool xdi_config_set_kv(xdi_config_t *cfg, const char *key, const char *val) {
    if (!cfg || !key || !val) return false;
    if (strcmp(key, "always_on_top") == 0) {
        cfg->always_on_top = parse_bool(val);
    } else if (strcmp(key, "avoid_obstacles") == 0) {
        cfg->avoid_obstacles = parse_bool(val);
    } else if (strcmp(key, "avoid_panels") == 0) {
        cfg->avoid_panels = parse_bool(val);
    } else if (strcmp(key, "hide_when_idle") == 0) {
        cfg->hide_when_idle = parse_bool(val);
    } else if (strcmp(key, "spin_art") == 0) {
        cfg->spin_art = parse_bool(val);
    } else if (strcmp(key, "real_audio_viz") == 0) {
        cfg->real_audio_viz = parse_bool(val);
    } else if (strcmp(key, "show_clock_idle") == 0) {
        cfg->show_clock_idle = parse_bool(val);
    } else if (strcmp(key, "persist_notifications") == 0) {
        cfg->persist_notifications = parse_bool(val);
    } else if (strcmp(key, "position") == 0) {
        cfg->position = xdi_position_from_str(val);
    } else if (strcmp(key, "eq_style") == 0) {
        cfg->eq_style = xdi_eq_style_from_str(val);
    } else if (strcmp(key, "art_shape") == 0) {
        cfg->art_shape = xdi_art_shape_from_str(val);
    } else if (strcmp(key, "monitor_index") == 0) {
        cfg->monitor_index = clamp_int(atoi(val), -1, 16);
    } else if (strcmp(key, "margin") == 0) {
        cfg->margin = clamp_int(atoi(val), 0, 120);
    } else if (strcmp(key, "obstacle_gap") == 0) {
        cfg->obstacle_gap = clamp_int(atoi(val), 0, 64);
    } else if (strcmp(key, "toast_duration_ms") == 0) {
        cfg->toast_duration_ms = clamp_int(atoi(val), 1000, 15000);
    } else if (strcmp(key, "max_history") == 0) {
        cfg->max_history = clamp_int(atoi(val), 5, 128);
    } else if (strcmp(key, "volume_step") == 0) {
        cfg->volume_step = clamp_int(atoi(val), 1, 25);
    } else if (strcmp(key, "accent_color") == 0) {
        uint32_t rgb = 0;
        if (xdi_parse_hex_rgb(val, &rgb)) cfg->accent_rgb = rgb;
    } else if (strcmp(key, "opacity") == 0) {
        double op = atof(val);
        if (op < 0.5) op = 0.5;
        if (op > 1.0) op = 1.0;
        cfg->opacity = op;
    } else if (strcmp(key, "font_family") == 0) {
        if (val[0] != '\0') {
            snprintf(cfg->font_family, sizeof(cfg->font_family), "%s", val);
        }
    } else {
        return false;
    }
    return true;
}

bool xdi_config_load(xdi_config_t *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_ws(line);
        if (*s == '\0' || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_ws(s);
        char *val = trim_ws(eq + 1);
        xdi_config_set_kv(cfg, key, val);
    }
    fclose(fp);
    return true;
}

static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

bool xdi_config_save(const xdi_config_t *cfg, const char *path) {
    if (!cfg || !path) return false;
    mkdir_p(path);
    FILE *fp = fopen(path, "w");
    if (!fp) return false;

    fprintf(fp, "# xdynamicisland configuration\n");
    fprintf(fp, "[island]\n");
    fprintf(fp, "always_on_top = %s\n", cfg->always_on_top ? "true" : "false");
    fprintf(fp, "avoid_obstacles = %s\n", cfg->avoid_obstacles ? "true" : "false");
    fprintf(fp, "avoid_panels = %s\n", cfg->avoid_panels ? "true" : "false");
    fprintf(fp, "hide_when_idle = %s\n", cfg->hide_when_idle ? "true" : "false");
    fprintf(fp, "spin_art = %s\n", cfg->spin_art ? "true" : "false");
    fprintf(fp, "real_audio_viz = %s\n", cfg->real_audio_viz ? "true" : "false");
    fprintf(fp, "show_clock_idle = %s\n", cfg->show_clock_idle ? "true" : "false");
    fprintf(fp, "persist_notifications = %s\n", cfg->persist_notifications ? "true" : "false");
    fprintf(fp, "position = %s\n", xdi_position_to_str(cfg->position));
    fprintf(fp, "eq_style = %s\n", xdi_eq_style_to_str(cfg->eq_style));
    fprintf(fp, "art_shape = %s\n", xdi_art_shape_to_str(cfg->art_shape));
    fprintf(fp, "monitor_index = %d\n", cfg->monitor_index);
    fprintf(fp, "margin = %d\n", cfg->margin);
    fprintf(fp, "obstacle_gap = %d\n", cfg->obstacle_gap);
    fprintf(fp, "toast_duration_ms = %d\n", cfg->toast_duration_ms);
    fprintf(fp, "max_history = %d\n", cfg->max_history);
    fprintf(fp, "volume_step = %d\n", cfg->volume_step);
    fprintf(fp, "accent_color = #%06X\n", cfg->accent_rgb & 0xFFFFFFu);
    fprintf(fp, "opacity = %.2f\n", cfg->opacity);
    fprintf(fp, "font_family = %s\n", cfg->font_family);

    fclose(fp);
    return true;
}
