#ifndef XDI_ART_H
#define XDI_ART_H

#include <cairo/cairo.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char current_url[512];
    char pending_url[512];
    char queued_url[512];
    cairo_surface_t *surface; /* Owned ARGB32 Cairo surface of decoded album art (or NULL) */
    uint32_t dominant_rgb;    /* Dominant vibrant color extracted from art */
    bool has_dominant;

    /* Background fetch/decode thread so network/disk I/O never stalls 60fps UI */
    pthread_mutex_t lock;
    pthread_t worker;
    bool worker_active;
    bool worker_joinable;
    bool ready;
    unsigned char *decoded_rgba;
    int decoded_w;
    int decoded_h;
    char cache_dir[512];
} xdi_art_loader_t;

void xdi_art_init(xdi_art_loader_t *art);
void xdi_art_destroy(xdi_art_loader_t *art);

/* Request loading an album art URL (file:// or https://). Non-blocking. */
void xdi_art_request(xdi_art_loader_t *art, const char *url);

/* Poll from main thread to promote finished decode into Cairo surface. Returns true if updated. */
bool xdi_art_poll(xdi_art_loader_t *art);

#endif /* XDI_ART_H */
