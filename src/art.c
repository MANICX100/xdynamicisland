#include "art.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wshadow"
#include "../third_party/stb_image.h"
#pragma GCC diagnostic pop

static void ensure_cache_dir(char *buf, size_t len) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) {
        snprintf(buf, len, "%s/xdynamicisland/art", xdg);
    } else if (home && home[0]) {
        snprintf(buf, len, "%s/.cache/xdynamicisland/art", home);
    } else {
        snprintf(buf, len, "/tmp/xdynamicisland-art");
    }

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", buf);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static uint64_t fnv1a_64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void url_decode_file_path(const char *uri, char *out, size_t len) {
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) p += 7;
    size_t j = 0;
    while (*p && j + 1 < len) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            out[j++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = '\0';
}

static bool download_url_to_file(const char *url, const char *dest) {
    struct stat st;
    if (stat(dest, &st) == 0 && st.st_size > 128) return true;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDOUT_FILENO);
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }
        execlp("curl", "curl", "-fsSL", "--max-time", "5", "--max-filesize", "4194304",
               "-o", dest, url, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool extract_vibrant_color(const unsigned char *rgba, int w, int h, uint32_t *out_rgb) {
    if (!rgba || w <= 0 || h <= 0) return false;
    double sum_r = 0, sum_g = 0, sum_b = 0, weight_sum = 0;
    int step_x = (w > 32) ? w / 32 : 1;
    int step_y = (h > 32) ? h / 32 : 1;

    for (int y = 0; y < h; y += step_y) {
        for (int x = 0; x < w; x += step_x) {
            const unsigned char *px = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            double r = px[0] / 255.0;
            double g = px[1] / 255.0;
            double b = px[2] / 255.0;
            double mx = fmax(r, fmax(g, b));
            double mn = fmin(r, fmin(g, b));
            double sat = (mx > 0.01) ? (mx - mn) / mx : 0.0;
            double lum = 0.299 * r + 0.587 * g + 0.114 * b;
            if (lum < 0.18 || lum > 0.88) continue;
            double weight = sat * sat * (1.0 - fabs(lum - 0.55));
            if (weight > 0.05) {
                sum_r += r * weight;
                sum_g += g * weight;
                sum_b += b * weight;
                weight_sum += weight;
            }
        }
    }
    if (weight_sum < 0.1) return false;
    int rr = (int)fmin(255.0, (sum_r / weight_sum) * 255.0);
    int gg = (int)fmin(255.0, (sum_g / weight_sum) * 255.0);
    int bb = (int)fmin(255.0, (sum_b / weight_sum) * 255.0);
    *out_rgb = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
    return true;
}

static void *art_worker_fn(void *arg) {
    xdi_art_loader_t *art = (xdi_art_loader_t *)arg;

    pthread_mutex_lock(&art->lock);
    char url[512];
    snprintf(url, sizeof(url), "%s", art->pending_url);
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s", art->cache_dir);
    pthread_mutex_unlock(&art->lock);

    char path[640] = {0};
    bool have_file = false;

    if (strncmp(url, "file://", 7) == 0) {
        url_decode_file_path(url, path, sizeof(path));
        have_file = true;
    } else if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        uint64_t h = fnv1a_64(url);
        snprintf(path, sizeof(path), "%s/%016llx.img", cache_dir, (unsigned long long)h);
        have_file = download_url_to_file(url, path);
    } else if (url[0] == '/') {
        snprintf(path, sizeof(path), "%s", url);
        have_file = true;
    }

    unsigned char *rgba = NULL;
    int w = 0, h = 0, ch = 0;
    if (have_file && path[0]) {
        rgba = stbi_load(path, &w, &h, &ch, 4);
    }

    pthread_mutex_lock(&art->lock);
    if (art->decoded_rgba) {
        stbi_image_free(art->decoded_rgba);
        art->decoded_rgba = NULL;
    }
    art->decoded_rgba = rgba;
    art->decoded_w = w;
    art->decoded_h = h;
    art->ready = true;
    art->worker_active = false;
    pthread_mutex_unlock(&art->lock);

    return NULL;
}

static void spawn_worker(xdi_art_loader_t *art, const char *url) {
    snprintf(art->pending_url, sizeof(art->pending_url), "%s", url);
    art->ready = false;
    art->worker_active = true;
    if (pthread_create(&art->worker, NULL, art_worker_fn, art) == 0) {
        art->worker_joinable = true;
    } else {
        art->worker_active = false;
    }
}

void xdi_art_init(xdi_art_loader_t *art) {
    memset(art, 0, sizeof(*art));
    pthread_mutex_init(&art->lock, NULL);
    ensure_cache_dir(art->cache_dir, sizeof(art->cache_dir));
}

void xdi_art_destroy(xdi_art_loader_t *art) {
    if (!art) return;
    if (art->worker_joinable) {
        pthread_join(art->worker, NULL);
        art->worker_joinable = false;
    }
    if (art->decoded_rgba) {
        stbi_image_free(art->decoded_rgba);
        art->decoded_rgba = NULL;
    }
    if (art->surface) {
        cairo_surface_destroy(art->surface);
        art->surface = NULL;
    }
    pthread_mutex_destroy(&art->lock);
}

void xdi_art_request(xdi_art_loader_t *art, const char *url) {
    if (!art) return;
    const char *u = url ? url : "";
    if (strcmp(u, art->current_url) == 0) return;

    snprintf(art->current_url, sizeof(art->current_url), "%s", u);

    if (u[0] == '\0') {
        if (art->surface) {
            cairo_surface_destroy(art->surface);
            art->surface = NULL;
        }
        art->has_dominant = false;
        art->queued_url[0] = '\0';
        return;
    }

    pthread_mutex_lock(&art->lock);
    bool active = art->worker_active;
    pthread_mutex_unlock(&art->lock);

    if (active) {
        snprintf(art->queued_url, sizeof(art->queued_url), "%s", u);
        return;
    }

    if (art->worker_joinable) {
        pthread_join(art->worker, NULL);
        art->worker_joinable = false;
    }

    art->queued_url[0] = '\0';
    spawn_worker(art, u);
}

bool xdi_art_poll(xdi_art_loader_t *art) {
    if (!art) return false;
    pthread_mutex_lock(&art->lock);
    bool ready = art->ready;
    unsigned char *rgba = NULL;
    int w = 0, h = 0;
    if (ready) {
        rgba = art->decoded_rgba;
        w = art->decoded_w;
        h = art->decoded_h;
        art->decoded_rgba = NULL;
        art->ready = false;
    }
    pthread_mutex_unlock(&art->lock);

    if (!ready) return false;

    if (art->worker_joinable) {
        pthread_join(art->worker, NULL);
        art->worker_joinable = false;
    }

    if (art->surface) {
        cairo_surface_destroy(art->surface);
        art->surface = NULL;
    }
    art->has_dominant = false;

    if (rgba && w > 0 && h > 0) {
        uint32_t dom = 0;
        if (extract_vibrant_color(rgba, w, h, &dom)) {
            art->dominant_rgb = dom;
            art->has_dominant = true;
        }

        int target = 160;
        int dw = w, dh = h;
        if (dw > target || dh > target) {
            if (dw >= dh) {
                dh = (dh * target) / dw;
                dw = target;
            } else {
                dw = (dw * target) / dh;
                dh = target;
            }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
        }

        cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        unsigned char *dst_data = cairo_image_surface_get_data(src);
        int stride = cairo_image_surface_get_stride(src);

        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(dst_data + y * stride);
            const unsigned char *srow = rgba + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; x++) {
                uint32_t r = srow[x * 4 + 0];
                uint32_t g = srow[x * 4 + 1];
                uint32_t b = srow[x * 4 + 2];
                uint32_t a = srow[x * 4 + 3];
                if (a < 255) {
                    r = (r * a) / 255;
                    g = (g * a) / 255;
                    b = (b * a) / 255;
                }
                row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(src);
        stbi_image_free(rgba);

        if (dw != w || dh != h) {
            cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dw, dh);
            cairo_t *cr = cairo_create(scaled);
            cairo_scale(cr, (double)dw / (double)w, (double)dh / (double)h);
            cairo_set_source_surface(cr, src, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_destroy(cr);
            cairo_surface_destroy(src);
            art->surface = scaled;
        } else {
            art->surface = src;
        }
    } else if (rgba) {
        stbi_image_free(rgba);
    }

    if (art->queued_url[0]) {
        char next_url[512];
        snprintf(next_url, sizeof(next_url), "%s", art->queued_url);
        art->queued_url[0] = '\0';
        spawn_worker(art, next_url);
    }

    return true;
}
