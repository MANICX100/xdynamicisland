#include "mpv.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void close_ipc(xdi_mpv_t *mpv) {
    if (mpv->ipc_fd >= 0) {
        close(mpv->ipc_fd);
        mpv->ipc_fd = -1;
    }
    mpv->using_ipc = false;
    mpv->ipc_path[0] = '\0';
    mpv->rx_len = 0;
}

static void reset_player(xdi_mpv_t *mpv) {
    memset(&mpv->player, 0, sizeof(mpv->player));
    snprintf(mpv->player.bus_name, sizeof(mpv->player.bus_name), "xdi.native.mpv");
    snprintf(mpv->player.identity, sizeof(mpv->player.identity), "mpv");
    mpv->player.can_control = true;
}

void xdi_mpv_init(xdi_mpv_t *mpv, Display *dpy, Window root) {
    memset(mpv, 0, sizeof(*mpv));
    mpv->dpy = dpy;
    mpv->root = root;
    mpv->ipc_fd = -1;
    mpv->mpv_win = None;
    if (dpy) {
        mpv->atom_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
        mpv->atom_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        mpv->atom_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        mpv->atom_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    }
    reset_player(mpv);
}

void xdi_mpv_destroy(xdi_mpv_t *mpv) {
    if (!mpv) return;
    close_ipc(mpv);
}

int xdi_mpv_get_fd(const xdi_mpv_t *mpv) {
    return (mpv && mpv->ipc_fd >= 0) ? mpv->ipc_fd : -1;
}

/* ---------- Pure Helpers (Unit-Testable) ---------- */

void xdi_mpv_clean_window_title(const char *raw_title, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!raw_title || !raw_title[0]) return;

    snprintf(out, out_len, "%s", raw_title);
    size_t len = strlen(out);

    /* Strip trailing " - mpv" */
    if (len > 6 && strcmp(out + len - 6, " - mpv") == 0) {
        out[len - 6] = '\0';
        len -= 6;
    }

    /* Trim whitespace */
    while (len > 0 && isspace((unsigned char)out[len - 1])) {
        out[--len] = '\0';
    }
}

bool xdi_mpv_extract_ipc_arg(const char *cmdline, size_t cmdline_len, char *out_path, size_t out_len) {
    if (!cmdline || cmdline_len == 0 || !out_path || out_len == 0) return false;
    size_t i = 0;
    while (i < cmdline_len) {
        const char *arg = cmdline + i;
        size_t alen = strnlen(arg, cmdline_len - i);
        if (strncmp(arg, "--input-ipc-server=", 19) == 0 && alen > 19) {
            snprintf(out_path, out_len, "%.*s", (int)(alen - 19), arg + 19);
            return out_path[0] != '\0';
        }
        if (strcmp(arg, "--input-ipc-server") == 0 && i + alen + 1 < cmdline_len) {
            const char *next = cmdline + i + alen + 1;
            size_t nlen = strnlen(next, cmdline_len - (i + alen + 1));
            if (nlen > 0) {
                snprintf(out_path, out_len, "%.*s", (int)nlen, next);
                return out_path[0] != '\0';
            }
        }
        i += alen + 1;
    }
    return false;
}

/* Minimal zero-dependency JSON field extractors for mpv IPC replies/events */
static bool json_find_string(const char *json, const char *key, char *out, size_t out_len) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (isspace((unsigned char)*p) || *p == ':')) p++;
    if (*p != '"') return false;
    p++;
    size_t j = 0;
    while (*p && *p != '"' && j + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[j++] = ' '; break;
                case 't': out[j++] = ' '; break;
                case '"': out[j++] = '"'; break;
                case '\\': out[j++] = '\\'; break;
                default: out[j++] = *p; break;
            }
            p++;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = '\0';
    return true;
}

static bool json_find_bool(const char *json, const char *key, bool *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (isspace((unsigned char)*p) || *p == ':')) p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool json_find_double(const char *json, const char *key, double *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (isspace((unsigned char)*p) || *p == ':')) p++;
    if (*p == 'n' || *p == '"' || *p == '\0') return false;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

bool xdi_mpv_parse_ipc_line(xdi_player_t *p, const char *line, int64_t now_ms) {
    if (!p || !line || !line[0]) return false;

    /* Handle observe_property events: {"event":"property-change","id":N,"name":"...","data":...} */
    char event[48] = {0};
    if (json_find_string(line, "event", event, sizeof(event))) {
        if (strcmp(event, "property-change") != 0) return false;
        char name[64] = {0};
        if (!json_find_string(line, "name", name, sizeof(name))) return false;

        if (strcmp(name, "pause") == 0) {
            bool paused = false;
            if (json_find_bool(line, "data", &paused)) {
                p->status = paused ? XDI_PLAYBACK_PAUSED : XDI_PLAYBACK_PLAYING;
                p->pos_timestamp_ms = now_ms;
                return true;
            }
        } else if (strcmp(name, "media-title") == 0) {
            char title[256] = {0};
            if (json_find_string(line, "data", title, sizeof(title)) && title[0]) {
                snprintf(p->title, sizeof(p->title), "%s", title);
                return true;
            }
        } else if (strcmp(name, "filename") == 0) {
            char fname[256] = {0};
            if (p->title[0] == '\0' && json_find_string(line, "data", fname, sizeof(fname)) && fname[0]) {
                snprintf(p->title, sizeof(p->title), "%s", fname);
                return true;
            }
        } else if (strcmp(name, "metadata/by-key/artist") == 0) {
            char artist[256] = {0};
            if (json_find_string(line, "data", artist, sizeof(artist))) {
                snprintf(p->artist, sizeof(p->artist), "%s", artist);
                return true;
            }
        } else if (strcmp(name, "metadata/by-key/album") == 0) {
            char album[128] = {0};
            if (json_find_string(line, "data", album, sizeof(album))) {
                snprintf(p->album, sizeof(p->album), "%s", album);
                return true;
            }
        } else if (strcmp(name, "duration") == 0) {
            double dur = 0.0;
            if (json_find_double(line, "data", &dur) && dur >= 0.0) {
                p->length_us = (int64_t)(dur * 1000000.0);
                p->can_seek = (p->length_us > 0);
                return true;
            }
        } else if (strcmp(name, "time-pos") == 0) {
            double pos = 0.0;
            if (json_find_double(line, "data", &pos) && pos >= 0.0) {
                p->position_us = (int64_t)(pos * 1000000.0);
                p->pos_timestamp_ms = now_ms;
                return true;
            }
        }
        return false;
    }

    /* Handle get_property time-pos reply: {"data":42.5,"request_id":7,"error":"success"} */
    double req_id = 0.0;
    if (json_find_double(line, "request_id", &req_id) && (int)req_id == 7) {
        double pos = 0.0;
        if (json_find_double(line, "data", &pos) && pos >= 0.0) {
            p->position_us = (int64_t)(pos * 1000000.0);
            p->pos_timestamp_ms = now_ms;
            return true;
        }
    }
    return false;
}

/* ---------- JSON IPC Socket Connection ---------- */

static void ipc_send_raw(xdi_mpv_t *mpv, const char *cmd) {
    if (mpv->ipc_fd < 0 || !cmd) return;
    size_t len = strlen(cmd);
    ssize_t wr = send(mpv->ipc_fd, cmd, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (wr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close_ipc(mpv);
    }
}

static bool try_connect_ipc(xdi_mpv_t *mpv, const char *sock_path) {
    if (!sock_path || !sock_path[0]) return false;
    if (strlen(sock_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    mpv->ipc_fd = fd;
    mpv->using_ipc = true;
    mpv->rx_len = 0;
    snprintf(mpv->ipc_path, sizeof(mpv->ipc_path), "%s", sock_path);
    reset_player(mpv);
    mpv->player.status = XDI_PLAYBACK_PLAYING;

    /* Subscribe to low-frequency metadata/state properties (zero per-frame IPC spam!)
     * and query time-pos once + every 3s (interpolated locally between updates). */
    static const char *const init_cmds =
        "{\"command\":[\"observe_property\",1,\"pause\"]}\n"
        "{\"command\":[\"observe_property\",2,\"media-title\"]}\n"
        "{\"command\":[\"observe_property\",3,\"filename\"]}\n"
        "{\"command\":[\"observe_property\",4,\"metadata/by-key/artist\"]}\n"
        "{\"command\":[\"observe_property\",5,\"metadata/by-key/album\"]}\n"
        "{\"command\":[\"observe_property\",6,\"duration\"]}\n"
        "{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":7}\n";
    ipc_send_raw(mpv, init_cmds);
    mpv->last_pos_req_ms = 0;
    return true;
}

static void drain_ipc(xdi_mpv_t *mpv, int64_t now_ms) {
    if (mpv->ipc_fd < 0) return;

    while (1) {
        if (mpv->rx_len >= sizeof(mpv->rx_buf) - 1) {
            mpv->rx_len = 0;
        }
        ssize_t n = recv(mpv->ipc_fd, mpv->rx_buf + mpv->rx_len,
                         sizeof(mpv->rx_buf) - 1 - mpv->rx_len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_ipc(mpv);
            mpv->active = false;
            mpv->state_changed = true;
            return;
        }
        if (n == 0) {
            /* mpv closed IPC socket */
            close_ipc(mpv);
            mpv->active = false;
            mpv->state_changed = true;
            return;
        }
        mpv->rx_len += (size_t)n;
        mpv->rx_buf[mpv->rx_len] = '\0';

        char *start = mpv->rx_buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (xdi_mpv_parse_ipc_line(&mpv->player, start, now_ms)) {
                mpv->active = true;
                mpv->state_changed = true;
            } else if (strstr(start, "\"playback-restart\"") || strstr(start, "\"seek\"")) {
                ipc_send_raw(mpv, "{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":7}\n");
                mpv->last_pos_req_ms = now_ms;
            }
            start = nl + 1;
        }
        size_t rem = strlen(start);
        if (rem > 0 && start != mpv->rx_buf) {
            memmove(mpv->rx_buf, start, rem);
        }
        mpv->rx_len = rem;
    }
}

/* ---------- X11 Window Discovery + /proc Inspection for Stock mpv ---------- */

static bool read_proc_cpu_ticks(pid_t pid, uint64_t *out_ticks) {
    if (pid <= 1) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return false;

    /* Fields after last ')' in /proc/<pid>/stat: state(3) ... utime(14) stime(15) */
    char *rp = strrchr(buf, ')');
    if (!rp) return false;
    unsigned long long utime = 0, stime = 0;
    int field = 3;
    char *p = rp + 1;
    while (*p && field <= 15) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (field == 14) utime = strtoull(p, &p, 10);
        else if (field == 15) stime = strtoull(p, &p, 10);
        else { while (*p && !isspace((unsigned char)*p)) p++; }
        field++;
    }
    *out_ticks = (uint64_t)(utime + stime);
    return true;
}

static bool probe_mpv_pid_ipc_socket(pid_t pid, char *out_path, size_t out_len) {
    if (pid <= 1) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return false;
    return xdi_mpv_extract_ipc_arg(buf, (size_t)n, out_path, out_len);
}

static bool read_window_title(const xdi_mpv_t *mpv, Window w, char *out, size_t len) {
    Display *dpy = mpv->dpy;
    Atom net_wm_name = mpv->atom_wm_name;
    Atom utf8 = mpv->atom_utf8;
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, net_wm_name, 0, 128, False, utf8,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems > 0) {
        snprintf(out, len, "%.*s", (int)nitems, (const char *)data);
        XFree(data);
        return true;
    }
    if (data) XFree(data);

    char *wm_name = NULL;
    if (XFetchName(dpy, w, &wm_name) && wm_name) {
        snprintf(out, len, "%s", wm_name);
        XFree(wm_name);
        return true;
    }
    return false;
}

static pid_t read_window_pid(const xdi_mpv_t *mpv, Window w) {
    Display *dpy = mpv->dpy;
    Atom net_wm_pid = mpv->atom_wm_pid;
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    pid_t pid = 0;
    if (XGetWindowProperty(dpy, w, net_wm_pid, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems >= 1) {
        pid = (pid_t)(*(unsigned long *)data);
        XFree(data);
    }
    return pid;
}

static void scan_for_mpv(xdi_mpv_t *mpv, int64_t now_ms) {
    /* 1. Try well-known mpv IPC sockets first (fastest, works even for audio-only/headless mpv) */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char rt_sock[256] = {0};
    if (runtime && runtime[0]) {
        snprintf(rt_sock, sizeof(rt_sock), "%s/mpv.sock", runtime);
    }
    const char *const common_sockets[] = {
        "/tmp/mpvsocket",
        "/tmp/mpv.sock",
        "/tmp/mpv-socket",
        rt_sock[0] ? rt_sock : NULL,
        NULL
    };
    for (int i = 0; common_sockets[i]; i++) {
        if (access(common_sockets[i], R_OK | W_OK) == 0) {
            if (try_connect_ipc(mpv, common_sockets[i])) {
                mpv->active = true;
                mpv->state_changed = true;
                return;
            }
        }
    }

    /* 2. Inspect X11 _NET_CLIENT_LIST for any mpv window */
    if (!mpv->dpy) return;
    Atom client_list = mpv->atom_client_list;
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    Window found_win = None;
    pid_t found_pid = 0;
    char raw_title[256] = {0};

    if (XGetWindowProperty(mpv->dpy, mpv->root, client_list, 0, 256, False, XA_WINDOW,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data) {
        Window *wins = (Window *)data;
        for (unsigned long i = 0; i < nitems; i++) {
            XClassHint ch;
            if (XGetClassHint(mpv->dpy, wins[i], &ch)) {
                bool is_mpv = (ch.res_class && strcasecmp(ch.res_class, "mpv") == 0) ||
                              (ch.res_name && strcasecmp(ch.res_name, "mpv") == 0);
                if (ch.res_name) XFree(ch.res_name);
                if (ch.res_class) XFree(ch.res_class);
                if (is_mpv) {
                    found_win = wins[i];
                    found_pid = read_window_pid(mpv, found_win);
                    read_window_title(mpv, found_win, raw_title, sizeof(raw_title));
                    break;
                }
            }
        }
        XFree(data);
    }

    if (found_win == None) {
        if (mpv->active && !mpv->using_ipc) {
            mpv->active = false;
            mpv->mpv_win = None;
            mpv->mpv_pid = 0;
            mpv->state_changed = true;
        }
        return;
    }

    /* Check if this mpv process was started with a custom --input-ipc-server path */
    char custom_sock[256] = {0};
    if (found_pid > 1 && probe_mpv_pid_ipc_socket(found_pid, custom_sock, sizeof(custom_sock))) {
        if (try_connect_ipc(mpv, custom_sock)) {
            mpv->mpv_win = found_win;
            mpv->mpv_pid = found_pid;
            mpv->active = true;
            mpv->state_changed = true;
            return;
        }
    }

    /* Stock zero-config mpv window mode */
    char clean_title[256];
    xdi_mpv_clean_window_title(raw_title, clean_title, sizeof(clean_title));
    if (clean_title[0] == '\0') snprintf(clean_title, sizeof(clean_title), "mpv");

    bool is_new = (!mpv->active || mpv->mpv_win != found_win);
    if (is_new) {
        reset_player(mpv);
        mpv->mpv_win = found_win;
        mpv->mpv_pid = found_pid;
        mpv->player.status = XDI_PLAYBACK_PLAYING;
        mpv->player.can_seek = false;
        mpv->zero_tick_streak = 0;
        read_proc_cpu_ticks(found_pid, &mpv->prev_cpu_ticks);
        mpv->active = true;
        mpv->state_changed = true;
    }

    if (strcmp(mpv->player.title, clean_title) != 0) {
        snprintf(mpv->player.title, sizeof(mpv->player.title), "%s", clean_title);
        mpv->state_changed = true;
    }

    /* Lightweight playing/paused inference via /proc/<pid>/stat CPU ticks (~5us, zero forks) */
    if (found_pid > 1 && now_ms >= mpv->manual_toggle_until_ms) {
        uint64_t ticks = 0;
        if (read_proc_cpu_ticks(found_pid, &ticks)) {
            uint64_t delta = (ticks >= mpv->prev_cpu_ticks) ? (ticks - mpv->prev_cpu_ticks) : 0;
            mpv->prev_cpu_ticks = ticks;
            xdi_playback_status_t next_status = mpv->player.status;
            if (delta >= 2) {
                mpv->zero_tick_streak = 0;
                next_status = XDI_PLAYBACK_PLAYING;
            } else {
                mpv->zero_tick_streak++;
                if (mpv->zero_tick_streak >= 2) {
                    next_status = XDI_PLAYBACK_PAUSED;
                }
            }
            if (next_status != mpv->player.status) {
                mpv->player.status = next_status;
                mpv->state_changed = true;
            }
        }
    }
}

void xdi_mpv_poll(xdi_mpv_t *mpv, int64_t now_ms, bool mpris_has_mpv) {
    if (!mpv) return;
    if (mpris_has_mpv) {
        if (mpv->active || mpv->ipc_fd >= 0) {
            close_ipc(mpv);
            mpv->active = false;
            mpv->mpv_win = None;
            mpv->state_changed = true;
        }
        return;
    }

    if (mpv->ipc_fd >= 0) {
        drain_ipc(mpv, now_ms);
        if (mpv->ipc_fd >= 0 && mpv->player.status == XDI_PLAYBACK_PLAYING &&
            (now_ms - mpv->last_pos_req_ms) >= 3000) {
            mpv->last_pos_req_ms = now_ms;
            ipc_send_raw(mpv, "{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":7}\n");
        }
        return;
    }

    if (now_ms - mpv->last_scan_ms >= 1100 || mpv->last_scan_ms == 0) {
        mpv->last_scan_ms = now_ms;
        scan_for_mpv(mpv, now_ms);
    }
}

/* ---------- X11 Synthetic Key Fallback for Stock mpv ---------- */

static void send_x11_key(Display *dpy, Window root, Window target, KeySym ks, unsigned int state) {
    if (!dpy || target == None) return;
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (kc == 0) return;

    XKeyEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.display = dpy;
    ev.window = target;
    ev.root = root;
    ev.subwindow = None;
    ev.time = CurrentTime;
    ev.same_screen = True;
    ev.keycode = kc;
    ev.state = state;

    ev.type = KeyPress;
    XSendEvent(dpy, target, False, KeyPressMask, (XEvent *)&ev);
    ev.type = KeyRelease;
    XSendEvent(dpy, target, False, KeyReleaseMask, (XEvent *)&ev);
    XFlush(dpy);
}

void xdi_mpv_play_pause(xdi_mpv_t *mpv, int64_t now_ms) {
    if (!mpv || !mpv->active) return;
    if (mpv->using_ipc && mpv->ipc_fd >= 0) {
        ipc_send_raw(mpv, "{\"command\":[\"cycle\",\"pause\"]}\n");
    } else if (mpv->mpv_win != None) {
        send_x11_key(mpv->dpy, mpv->root, mpv->mpv_win, XK_space, 0);
        mpv->player.status = (mpv->player.status == XDI_PLAYBACK_PLAYING)
                                 ? XDI_PLAYBACK_PAUSED
                                 : XDI_PLAYBACK_PLAYING;
        mpv->manual_toggle_until_ms = now_ms + 2600;
        mpv->zero_tick_streak = 0;
        if (mpv->mpv_pid > 1) read_proc_cpu_ticks(mpv->mpv_pid, &mpv->prev_cpu_ticks);
        mpv->state_changed = true;
    }
}

void xdi_mpv_next(xdi_mpv_t *mpv) {
    if (!mpv || !mpv->active) return;
    if (mpv->using_ipc && mpv->ipc_fd >= 0) {
        ipc_send_raw(mpv, "{\"command\":[\"playlist-next\"]}\n");
    } else if (mpv->mpv_win != None) {
        send_x11_key(mpv->dpy, mpv->root, mpv->mpv_win, XK_greater, ShiftMask);
    }
}

void xdi_mpv_prev(xdi_mpv_t *mpv) {
    if (!mpv || !mpv->active) return;
    if (mpv->using_ipc && mpv->ipc_fd >= 0) {
        ipc_send_raw(mpv, "{\"command\":[\"playlist-prev\"]}\n");
    } else if (mpv->mpv_win != None) {
        send_x11_key(mpv->dpy, mpv->root, mpv->mpv_win, XK_less, ShiftMask);
    }
}

void xdi_mpv_seek_fraction(xdi_mpv_t *mpv, double frac, int64_t now_ms) {
    if (!mpv || !mpv->active || !mpv->using_ipc || mpv->ipc_fd < 0) return;
    if (mpv->player.length_us <= 0) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    double target_sec = ((double)mpv->player.length_us / 1000000.0) * frac;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"command\":[\"seek\",%.3f,\"absolute\"]}\n", target_sec);
    ipc_send_raw(mpv, cmd);

    mpv->player.position_us = (int64_t)(target_sec * 1000000.0);
    mpv->player.pos_timestamp_ms = now_ms;
    mpv->state_changed = true;
}
