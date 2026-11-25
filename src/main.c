#include <X11/Xlib.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "art.h"
#include "audio_viz.h"
#include "config.h"
#include "media.h"
#include "notifications.h"
#include "settings_ui.h"
#include "ui.h"

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* Resilient X11 error handler: transient BadWindow/BadDrawable errors can happen
 * when an ephemeral override-redirect window like dmenu closes mid-frame. */
static int x11_error_handler(Display *dpy, XErrorEvent *ev) {
    if (ev->error_code == BadWindow || ev->error_code == BadDrawable ||
        ev->error_code == BadMatch || ev->error_code == BadAccess) {
        return 0;
    }
    if (getenv("XDI_DEBUG")) {
        char msg[128];
        XGetErrorText(dpy, ev->error_code, msg, sizeof(msg));
        fprintf(stderr, "xdynamicisland: X11 error %d (%s) req=%d\n",
                ev->error_code, msg, ev->request_code);
    }
    return 0;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void print_usage(const char *prog) {
    printf("xdynamicisland v%s — DE-independent Dynamic Island for Linux X11\n\n"
           "Usage: %s [OPTIONS]\n\n"
           "Options:\n"
           "  -c, --config <path>     Load/save config at custom path\n"
           "  -s, --settings          Open settings window on startup\n"
           "  -p, --position <pos>    top-center|top-left|top-right|bottom-center|bottom-left|bottom-right\n"
           "  -e, --eq-style <style>  wave|bars|ribbon|spectrum|segments|solid\n"
           "      --always-on-top     Keep island above all normal windows (default)\n"
           "      --no-always-on-top  Allow normal windows to cover the island\n"
           "      --avoid-dmenu       Reposition below dmenu/rofi when summoned (default)\n"
           "      --no-avoid-dmenu    Disable obstacle repositioning\n"
           "  -v, --version           Print version and exit\n"
           "  -h, --help              Show this help message\n",
           XDI_VERSION, prog);
}

int main(int argc, char **argv) {
    char config_path[512];
    xdi_config_default_path(config_path, sizeof(config_path));

    xdi_config_t cfg;
    xdi_config_defaults(&cfg);

    /* First pass: locate custom config path or --help / --version */
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)) {
            print_usage(argv[0]);
            return 0;
        }
        if ((strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)) {
            printf("xdynamicisland %s\n", XDI_VERSION);
            return 0;
        }
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            snprintf(config_path, sizeof(config_path), "%s", argv[++i]);
        }
    }

    if (!xdi_config_load(&cfg, config_path)) {
        xdi_config_save(&cfg, config_path);
    }

    bool open_settings_now = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--settings") == 0) {
            open_settings_now = true;
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--position") == 0) && i + 1 < argc) {
            cfg.position = xdi_position_from_str(argv[++i]);
        } else if ((strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eq-style") == 0) && i + 1 < argc) {
            cfg.eq_style = xdi_eq_style_from_str(argv[++i]);
        } else if (strcmp(argv[i], "--always-on-top") == 0) {
            cfg.always_on_top = true;
        } else if (strcmp(argv[i], "--no-always-on-top") == 0) {
            cfg.always_on_top = false;
        } else if (strcmp(argv[i], "--avoid-dmenu") == 0) {
            cfg.avoid_obstacles = true;
        } else if (strcmp(argv[i], "--no-avoid-dmenu") == 0) {
            cfg.avoid_obstacles = false;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            i++;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xdynamicisland: failed to open X11 display (check $DISPLAY)\n");
        return 1;
    }
    XSetErrorHandler(x11_error_handler);

    xdi_media_t media;
    xdi_media_init(&media);

    xdi_notif_store_t notifs;
    xdi_notif_init(&notifs, cfg.max_history, cfg.persist_notifications);

    xdi_audio_viz_t viz;
    xdi_audio_viz_init(&viz, cfg.real_audio_viz);

    xdi_art_loader_t art;
    xdi_art_init(&art);

    const xdi_player_t *initial_player = xdi_media_active(&media);
    if (initial_player && initial_player->art_url[0]) {
        xdi_art_request(&art, initial_player->art_url);
    }

    xdi_ui_t ui;
    if (!xdi_ui_init(&ui, dpy, &cfg, &media, &notifs, &viz, &art)) {
        fprintf(stderr, "xdynamicisland: failed to initialize X11 window\n");
        xdi_art_destroy(&art);
        xdi_audio_viz_destroy(&viz);
        xdi_notif_destroy(&notifs);
        xdi_media_destroy(&media);
        XCloseDisplay(dpy);
        return 1;
    }

    xdi_settings_ui_t settings;
    xdi_settings_init(&settings, dpy, &cfg, config_path);
    if (open_settings_now) {
        xdi_settings_open(&settings);
    }

    int x11_fd = ConnectionNumber(dpy);

    while (g_running) {
        int64_t now_ms = monotonic_ms();

        /* Drain all queued X11 events */
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (settings.open && ev.xany.window == settings.win) {
                if (xdi_settings_handle_event(&settings, &ev)) {
                    xdi_ui_apply_config(&ui, settings.win);
                }
            } else {
                xdi_ui_handle_event(&ui, &ev, now_ms);
            }
        }

        if (ui.open_settings_requested) {
            ui.open_settings_requested = false;
            xdi_settings_open(&settings);
        }

        /* Drain D-Bus MPRIS2 + notification-daemon messages */
        xdi_media_poll(&media, now_ms);
        xdi_notif_poll(&notifs);

        /* Step animations and render */
        int wait_ms = xdi_ui_tick(&ui, now_ms, settings.open ? settings.win : None);
        if (settings.open) {
            xdi_settings_render(&settings);
        }

        /* Event-driven poll across X11 + MPRIS D-Bus + notification-daemon D-Bus */
        struct pollfd pfds[3];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){.fd = x11_fd, .events = POLLIN};

        int mfd = xdi_media_get_fd(&media);
        if (mfd >= 0) pfds[nfds++] = (struct pollfd){.fd = mfd, .events = POLLIN};

        int nfd = xdi_notif_get_fd(&notifs);
        if (nfd >= 0) pfds[nfds++] = (struct pollfd){.fd = nfd, .events = POLLIN};

        if (XPending(dpy) == 0) {
            poll(pfds, (nfds_t)nfds, wait_ms);
        }
    }

    xdi_settings_destroy(&settings);
    xdi_ui_destroy(&ui);
    xdi_art_destroy(&art);
    xdi_audio_viz_destroy(&viz);
    xdi_notif_destroy(&notifs);
    xdi_media_destroy(&media);
    XCloseDisplay(dpy);
    return 0;
}
