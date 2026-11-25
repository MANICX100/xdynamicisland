#include "media.h"
#include "mpv.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool xdi_parse_wpctl_volume(const char *output, double *out_vol, bool *out_muted) {
    if (!output || !out_vol || !out_muted) return false;
    const char *p = strstr(output, "Volume:");
    if (!p) return false;
    p += 7;
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p || v < 0.0 || v > 5.0) return false;
    *out_vol = v;
    *out_muted = (strstr(output, "[MUTED]") != NULL);
    return true;
}

static bool run_cmd_capture(const char *const argv[], char *buf, size_t len) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);
    size_t total = 0;
    while (total + 1 < len) {
        ssize_t n = read(fds[0], buf + total, len - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void run_cmd_detached(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            FILE *devnull = fopen("/dev/null", "w");
            if (devnull) {
                dup2(fileno(devnull), STDOUT_FILENO);
                dup2(fileno(devnull), STDERR_FILENO);
                fclose(devnull);
            }
            execvp(argv[0], (char *const *)argv);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

static void refresh_wpctl_volume(xdi_media_t *m, bool notify_change) {
    const char *const argv[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL};
    char out[128] = {0};
    double vol = 0.0;
    bool muted = false;
    if (run_cmd_capture(argv, out, sizeof(out)) &&
        xdi_parse_wpctl_volume(out, &vol, &muted)) {
        if (notify_change && m->volume.available &&
            (fabs(vol - m->volume.volume) > 0.005 || muted != m->volume.muted)) {
            m->volume_changed = true;
        }
        m->volume.volume = vol;
        m->volume.muted = muted;
        m->volume.available = true;
    }
}

static int find_player(const xdi_media_t *m, const char *bus_name) {
    for (int i = 0; i < m->player_count; i++) {
        if (strcmp(m->players[i].bus_name, bus_name) == 0) return i;
    }
    return -1;
}

static xdi_playback_status_t parse_playback_status(const char *s) {
    if (!s) return XDI_PLAYBACK_STOPPED;
    if (strcmp(s, "Playing") == 0) return XDI_PLAYBACK_PLAYING;
    if (strcmp(s, "Paused") == 0)  return XDI_PLAYBACK_PAUSED;
    return XDI_PLAYBACK_STOPPED;
}

static void parse_metadata_dict(DBusMessageIter *dict, xdi_player_t *p) {
    p->title[0] = '\0';
    p->artist[0] = '\0';
    p->album[0] = '\0';
    p->art_url[0] = '\0';
    p->length_us = 0;

    if (dbus_message_iter_get_arg_type(dict) != DBUS_TYPE_ARRAY) return;
    DBusMessageIter entry_iter;
    dbus_message_iter_recurse(dict, &entry_iter);
    while (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv;
        dbus_message_iter_recurse(&entry_iter, &kv);
        if (dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_STRING) {
            const char *key = NULL;
            dbus_message_iter_get_basic(&kv, &key);
            if (key && dbus_message_iter_next(&kv) &&
                dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_VARIANT) {
                DBusMessageIter var;
                dbus_message_iter_recurse(&kv, &var);
                int vtype = dbus_message_iter_get_arg_type(&var);

                if (strcmp(key, "xesam:title") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    if (s) snprintf(p->title, sizeof(p->title), "%s", s);
                } else if (strcmp(key, "xesam:album") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    if (s) snprintf(p->album, sizeof(p->album), "%s", s);
                } else if (strcmp(key, "mpris:artUrl") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    if (s) snprintf(p->art_url, sizeof(p->art_url), "%s", s);
                } else if (strcmp(key, "mpris:length") == 0) {
                    if (vtype == DBUS_TYPE_INT64) {
                        int64_t v = 0;
                        dbus_message_iter_get_basic(&var, &v);
                        p->length_us = v;
                    } else if (vtype == DBUS_TYPE_UINT64) {
                        uint64_t v = 0;
                        dbus_message_iter_get_basic(&var, &v);
                        p->length_us = (int64_t)v;
                    }
                } else if (strcmp(key, "xesam:artist") == 0) {
                    if (vtype == DBUS_TYPE_ARRAY) {
                        DBusMessageIter arr;
                        dbus_message_iter_recurse(&var, &arr);
                        size_t pos = 0;
                        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                            const char *a = NULL;
                            dbus_message_iter_get_basic(&arr, &a);
                            if (a && a[0]) {
                                if (pos > 0 && pos + 2 < sizeof(p->artist)) {
                                    p->artist[pos++] = ',';
                                    p->artist[pos++] = ' ';
                                }
                                int n = snprintf(p->artist + pos, sizeof(p->artist) - pos, "%s", a);
                                if (n > 0) pos += (size_t)n;
                                if (pos >= sizeof(p->artist) - 1) break;
                            }
                            dbus_message_iter_next(&arr);
                        }
                    } else if (vtype == DBUS_TYPE_STRING) {
                        const char *a = NULL;
                        dbus_message_iter_get_basic(&var, &a);
                        if (a) snprintf(p->artist, sizeof(p->artist), "%s", a);
                    }
                }
            }
        }
        dbus_message_iter_next(&entry_iter);
    }
}

static void fetch_dbus_string_prop(DBusConnection *conn, const char *dest, const char *path,
                                   const char *iface, const char *prop, char *out, size_t len) {
    DBusMessage *msg = dbus_message_new_method_call(dest, path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 300, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter var;
        dbus_message_iter_recurse(&iter, &var);
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
            const char *s = NULL;
            dbus_message_iter_get_basic(&var, &s);
            if (s) snprintf(out, len, "%s", s);
        }
    }
    dbus_message_unref(reply);
}

static void fetch_player_all_props(xdi_media_t *m, xdi_player_t *p) {
    if (!m->conn || p->is_mpv_native) return;
    p->is_kdeconnect = (strstr(p->bus_name, "kdeconnect") != NULL);

    /* Identity from org.mpris.MediaPlayer2 */
    p->identity[0] = '\0';
    fetch_dbus_string_prop(m->conn, p->bus_name, "/org/mpris/MediaPlayer2",
                           "org.mpris.MediaPlayer2", "Identity",
                           p->identity, sizeof(p->identity));
    if (p->identity[0] == '\0') {
        const char *short_name = p->bus_name + strlen("org.mpris.MediaPlayer2.");
        snprintf(p->identity, sizeof(p->identity), "%s", short_name);
        char *dot = strchr(p->identity, '.');
        if (dot) *dot = '\0';
        if (p->identity[0]) p->identity[0] = (char)toupper((unsigned char)p->identity[0]);
    }
    if (p->is_kdeconnect && m->kde_device_name[0]) {
        snprintf(p->identity, sizeof(p->identity), "KDE Connect · %.45s", m->kde_device_name);
    } else if (p->is_kdeconnect) {
        snprintf(p->identity, sizeof(p->identity), "KDE Connect");
    }

    /* GetAll on org.mpris.MediaPlayer2.Player */
    DBusMessage *msg = dbus_message_new_method_call(p->bus_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return;
    const char *iface = "org.mpris.MediaPlayer2.Player";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(m->conn, msg, 400, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }

    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr;
        dbus_message_iter_recurse(&iter, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter kv;
            dbus_message_iter_recurse(&arr, &kv);
            if (dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_STRING) {
                const char *key = NULL;
                dbus_message_iter_get_basic(&kv, &key);
                if (key && dbus_message_iter_next(&kv) &&
                    dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_VARIANT) {
                    DBusMessageIter var;
                    dbus_message_iter_recurse(&kv, &var);
                    int t = dbus_message_iter_get_arg_type(&var);
                    if (strcmp(key, "PlaybackStatus") == 0 && t == DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&var, &s);
                        p->status = parse_playback_status(s);
                    } else if (strcmp(key, "Metadata") == 0 && t == DBUS_TYPE_ARRAY) {
                        parse_metadata_dict(&var, p);
                    } else if (strcmp(key, "Position") == 0 && t == DBUS_TYPE_INT64) {
                        int64_t pos = 0;
                        dbus_message_iter_get_basic(&var, &pos);
                        p->position_us = pos;
                        p->pos_timestamp_ms = monotonic_ms();
                    } else if (strcmp(key, "CanSeek") == 0 && t == DBUS_TYPE_BOOLEAN) {
                        dbus_bool_t b = FALSE;
                        dbus_message_iter_get_basic(&var, &b);
                        p->can_seek = (b != FALSE);
                    } else if (strcmp(key, "CanControl") == 0 && t == DBUS_TYPE_BOOLEAN) {
                        dbus_bool_t b = FALSE;
                        dbus_message_iter_get_basic(&var, &b);
                        p->can_control = (b != FALSE);
                    }
                }
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
}

static void fetch_player_position(xdi_media_t *m, xdi_player_t *p) {
    if (!m->conn || !p || p->is_mpv_native) return;
    DBusMessage *msg = dbus_message_new_method_call(p->bus_name, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return;
    const char *iface = "org.mpris.MediaPlayer2.Player";
    const char *prop = "Position";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(m->conn, msg, 150, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter var;
        dbus_message_iter_recurse(&iter, &var);
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_INT64) {
            int64_t pos = 0;
            dbus_message_iter_get_basic(&var, &pos);
            p->position_us = pos;
            p->pos_timestamp_ms = monotonic_ms();
        }
    }
    dbus_message_unref(reply);
}

static void probe_kdeconnect_device(xdi_media_t *m) {
    if (!m->conn) return;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.kde.kdeconnect", "/modules/kdeconnect",
        "org.kde.kdeconnect.daemon", "devices");
    if (!msg) return;
    dbus_bool_t only_reachable = TRUE;
    dbus_bool_t only_paired = TRUE;
    dbus_message_append_args(msg,
                             DBUS_TYPE_BOOLEAN, &only_reachable,
                             DBUS_TYPE_BOOLEAN, &only_paired,
                             DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(m->conn, msg, 250, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }
    DBusMessageIter iter, arr;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &arr);
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            const char *dev_id = NULL;
            dbus_message_iter_get_basic(&arr, &dev_id);
            if (dev_id && dev_id[0]) {
                char path[256];
                snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s", dev_id);
                fetch_dbus_string_prop(m->conn, "org.kde.kdeconnect", path,
                                       "org.kde.kdeconnect.device", "name",
                                       m->kde_device_name, sizeof(m->kde_device_name));
            }
        }
    }
    dbus_message_unref(reply);
}

int xdi_select_best_player(const xdi_player_t *players, int count, int current_idx, bool manual) {
    if (!players || count <= 0) return -1;
    if (manual && current_idx >= 0 && current_idx < count) {
        if (players[current_idx].status == XDI_PLAYBACK_PLAYING ||
            players[current_idx].title[0] != '\0') {
            return current_idx;
        }
    }
    if (current_idx >= 0 && current_idx < count &&
        players[current_idx].status == XDI_PLAYBACK_PLAYING &&
        players[current_idx].title[0] != '\0') {
        return current_idx;
    }
    for (int i = 0; i < count; i++) {
        if (players[i].status == XDI_PLAYBACK_PLAYING && players[i].title[0] != '\0') return i;
    }
    for (int i = 0; i < count; i++) {
        if (players[i].status == XDI_PLAYBACK_PLAYING) return i;
    }
    if (current_idx >= 0 && current_idx < count &&
        players[current_idx].status == XDI_PLAYBACK_PAUSED &&
        players[current_idx].title[0] != '\0') {
        return current_idx;
    }
    for (int i = 0; i < count; i++) {
        if (players[i].status == XDI_PLAYBACK_PAUSED && players[i].title[0] != '\0') return i;
    }
    for (int i = 0; i < count; i++) {
        if (players[i].title[0] != '\0') return i;
    }
    return 0;
}

static void add_or_update_player(xdi_media_t *m, const char *bus_name) {
    int idx = find_player(m, bus_name);
    if (idx < 0) {
        if (m->player_count >= XDI_MAX_PLAYERS) return;
        idx = m->player_count++;
        memset(&m->players[idx], 0, sizeof(m->players[idx]));
        snprintf(m->players[idx].bus_name, sizeof(m->players[idx].bus_name), "%s", bus_name);
    }
    fetch_player_all_props(m, &m->players[idx]);
    m->active_index = xdi_select_best_player(m->players, m->player_count,
                                             m->active_index, m->manual_selection);
    m->state_changed = true;
}

static void remove_player(xdi_media_t *m, const char *bus_name) {
    int idx = find_player(m, bus_name);
    if (idx < 0) return;
    memmove(&m->players[idx], &m->players[idx + 1],
            (size_t)(m->player_count - idx - 1) * sizeof(xdi_player_t));
    m->player_count--;
    if (m->active_index == idx) {
        m->manual_selection = false;
        m->active_index = -1;
    } else if (m->active_index > idx) {
        m->active_index--;
    }
    m->active_index = xdi_select_best_player(m->players, m->player_count,
                                             m->active_index, m->manual_selection);
    m->state_changed = true;
}

static bool has_mpris_mpv(const xdi_media_t *m) {
    for (int i = 0; i < m->player_count; i++) {
        if (!m->players[i].is_mpv_native &&
            strstr(m->players[i].bus_name, "mpv") != NULL) {
            return true;
        }
    }
    return false;
}

static void sync_native_mpv(xdi_media_t *m, int64_t now_ms) {
    if (!m->mpv) return;
    xdi_mpv_poll(m->mpv, now_ms, has_mpris_mpv(m));
    if (!m->mpv->state_changed) return;
    m->mpv->state_changed = false;

    static const char *const native_bus = "xdi.native.mpv";
    if (!m->mpv->active) {
        remove_player(m, native_bus);
        return;
    }

    int idx = find_player(m, native_bus);
    if (idx < 0) {
        if (m->player_count >= XDI_MAX_PLAYERS) return;
        idx = m->player_count++;
    }
    m->players[idx] = m->mpv->player;
    m->players[idx].is_mpv_native = true;
    m->active_index = xdi_select_best_player(m->players, m->player_count,
                                             m->active_index, m->manual_selection);
    m->state_changed = true;
}

static void scan_existing_players(xdi_media_t *m) {
    if (!m->conn) return;
    DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.DBus",
                                                    "/org/freedesktop/DBus",
                                                    "org.freedesktop.DBus", "ListNames");
    if (!msg) return;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(m->conn, msg, 1000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }
    DBusMessageIter iter, arr;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            const char *name = NULL;
            dbus_message_iter_get_basic(&arr, &name);
            if (name && strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
                add_or_update_player(m, name);
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
}

static DBusHandlerResult mpris_filter(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)conn;
    xdi_media_t *m = (xdi_media_t *)user_data;

    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner,
                                  DBUS_TYPE_INVALID)) {
            if (name && strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
                if (new_owner && new_owner[0] != '\0') {
                    add_or_update_player(m, name);
                } else {
                    remove_player(m, name);
                }
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        const char *path = dbus_message_get_path(msg);
        if (!path || strcmp(path, "/org/mpris/MediaPlayer2") != 0) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        for (int i = 0; i < m->player_count; i++) {
            if (!m->players[i].is_mpv_native) {
                fetch_player_all_props(m, &m->players[i]);
            }
        }
        m->active_index = xdi_select_best_player(m->players, m->player_count,
                                                 m->active_index, m->manual_selection);
        m->state_changed = true;
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_is_signal(msg, "org.mpris.MediaPlayer2.Player", "Seeked")) {
        int64_t pos = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_INT64, &pos, DBUS_TYPE_INVALID)) {
            if (m->active_index >= 0 && m->active_index < m->player_count &&
                !m->players[m->active_index].is_mpv_native) {
                m->players[m->active_index].position_us = pos;
                m->players[m->active_index].pos_timestamp_ms = monotonic_ms();
                m->state_changed = true;
            }
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool xdi_media_init(xdi_media_t *m) {
    memset(m, 0, sizeof(*m));
    m->active_index = -1;
    m->kde_battery = -1;

    DBusError err;
    dbus_error_init(&err);
    m->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!m->conn || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        m->conn = NULL;
    } else {
        dbus_connection_set_exit_on_disconnect(m->conn, FALSE);
        dbus_bus_add_match(m->conn,
            "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'",
            NULL);
        dbus_bus_add_match(m->conn,
            "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/org/mpris/MediaPlayer2'",
            NULL);
        dbus_bus_add_match(m->conn,
            "type='signal',interface='org.mpris.MediaPlayer2.Player',member='Seeked'",
            NULL);
        dbus_connection_add_filter(m->conn, mpris_filter, m, NULL);

        probe_kdeconnect_device(m);
        scan_existing_players(m);
    }

    refresh_wpctl_volume(m, false);
    m->last_vol_poll_ms = monotonic_ms();
    m->last_pos_poll_ms = m->last_vol_poll_ms;
    return m->conn != NULL || m->volume.available;
}

void xdi_media_attach_mpv(xdi_media_t *m, struct xdi_mpv_s *mpv) {
    if (!m) return;
    m->mpv = mpv;
    sync_native_mpv(m, monotonic_ms());
}

void xdi_media_destroy(xdi_media_t *m) {
    if (!m) return;
    if (m->conn) {
        dbus_connection_close(m->conn);
        dbus_connection_unref(m->conn);
        m->conn = NULL;
    }
    m->mpv = NULL;
}

int xdi_media_get_fd(xdi_media_t *m) {
    if (!m || !m->conn) return -1;
    int fd = -1;
    if (dbus_connection_get_unix_fd(m->conn, &fd)) return fd;
    return -1;
}

void xdi_media_poll(xdi_media_t *m, int64_t now_ms) {
    if (!m) return;
    if (m->conn) {
        dbus_connection_read_write(m->conn, 0);
        while (dbus_connection_dispatch(m->conn) == DBUS_DISPATCH_DATA_REMAINS) {
            /* Drain all queued MPRIS + KDE Connect signals */
        }
    }

    sync_native_mpv(m, now_ms);

    /* Periodically poll wpctl volume (every 900ms) so external hardware volume keys trigger OSD toast */
    if (now_ms - m->last_vol_poll_ms >= 900) {
        m->last_vol_poll_ms = now_ms;
        refresh_wpctl_volume(m, true);
    }

    /* Refresh playback position every 3 seconds when playing so seek bar stays exact */
    if (now_ms - m->last_pos_poll_ms >= 3000) {
        m->last_pos_poll_ms = now_ms;
        if (m->active_index >= 0 && m->active_index < m->player_count &&
            m->players[m->active_index].status == XDI_PLAYBACK_PLAYING &&
            !m->players[m->active_index].is_mpv_native) {
            fetch_player_position(m, &m->players[m->active_index]);
        }
    }
}

const xdi_player_t *xdi_media_active(const xdi_media_t *m) {
    if (!m || m->active_index < 0 || m->active_index >= m->player_count) return NULL;
    return &m->players[m->active_index];
}

int64_t xdi_media_current_position_us(const xdi_player_t *p, int64_t now_ms) {
    if (!p) return 0;
    int64_t pos = p->position_us;
    if (p->status == XDI_PLAYBACK_PLAYING && p->pos_timestamp_ms > 0 && now_ms > p->pos_timestamp_ms) {
        pos += (now_ms - p->pos_timestamp_ms) * 1000;
    }
    if (pos < 0) pos = 0;
    if (p->length_us > 0 && pos > p->length_us) pos = p->length_us;
    return pos;
}

static void send_mpris_method(xdi_media_t *m, const char *method) {
    if (!m || !m->conn || m->active_index < 0 || m->active_index >= m->player_count) return;
    const char *dest = m->players[m->active_index].bus_name;
    DBusMessage *msg = dbus_message_new_method_call(dest, "/org/mpris/MediaPlayer2",
                                                    "org.mpris.MediaPlayer2.Player", method);
    if (!msg) return;
    dbus_connection_send(m->conn, msg, NULL);
    dbus_connection_flush(m->conn);
    dbus_message_unref(msg);
}

void xdi_media_play_pause(xdi_media_t *m) {
    if (!m || m->active_index < 0 || m->active_index >= m->player_count) return;
    if (m->players[m->active_index].is_mpv_native && m->mpv) {
        int64_t now = monotonic_ms();
        xdi_mpv_play_pause(m->mpv, now);
        sync_native_mpv(m, now);
        return;
    }
    send_mpris_method(m, "PlayPause");
}

void xdi_media_next(xdi_media_t *m) {
    if (!m || m->active_index < 0 || m->active_index >= m->player_count) return;
    if (m->players[m->active_index].is_mpv_native && m->mpv) {
        xdi_mpv_next(m->mpv);
        return;
    }
    send_mpris_method(m, "Next");
}

void xdi_media_prev(xdi_media_t *m) {
    if (!m || m->active_index < 0 || m->active_index >= m->player_count) return;
    if (m->players[m->active_index].is_mpv_native && m->mpv) {
        xdi_mpv_prev(m->mpv);
        return;
    }
    send_mpris_method(m, "Previous");
}

void xdi_media_seek_fraction(xdi_media_t *m, double frac) {
    if (!m || m->active_index < 0 || m->active_index >= m->player_count) return;
    xdi_player_t *p = &m->players[m->active_index];
    if (p->is_mpv_native && m->mpv) {
        int64_t now = monotonic_ms();
        xdi_mpv_seek_fraction(m->mpv, frac, now);
        sync_native_mpv(m, now);
        return;
    }
    if (!m->conn || p->length_us <= 0) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    int64_t target_us = (int64_t)(frac * (double)p->length_us);
    int64_t cur_us = xdi_media_current_position_us(p, monotonic_ms());
    int64_t delta_us = target_us - cur_us;

    DBusMessage *msg = dbus_message_new_method_call(p->bus_name, "/org/mpris/MediaPlayer2",
                                                    "org.mpris.MediaPlayer2.Player", "Seek");
    if (!msg) return;
    dbus_message_append_args(msg, DBUS_TYPE_INT64, &delta_us, DBUS_TYPE_INVALID);
    dbus_connection_send(m->conn, msg, NULL);
    dbus_connection_flush(m->conn);
    dbus_message_unref(msg);

    p->position_us = target_us;
    p->pos_timestamp_ms = monotonic_ms();
    m->state_changed = true;
}

void xdi_media_cycle_player(xdi_media_t *m, int delta) {
    if (!m || m->player_count <= 1) return;
    int next = (m->active_index + delta) % m->player_count;
    if (next < 0) next += m->player_count;
    m->active_index = next;
    m->manual_selection = true;
    m->state_changed = true;
}

void xdi_media_volume_delta(xdi_media_t *m, int delta_percent) {
    if (!m) return;
    char arg[32];
    if (delta_percent >= 0) {
        snprintf(arg, sizeof(arg), "%d%%+", delta_percent);
    } else {
        snprintf(arg, sizeof(arg), "%d%%-", -delta_percent);
    }
    const char *const argv[] = {"wpctl", "set-volume", "-l", "1.25", "@DEFAULT_AUDIO_SINK@", arg, NULL};
    run_cmd_detached(argv);

    double nv = m->volume.volume + (double)delta_percent / 100.0;
    if (nv < 0.0) nv = 0.0;
    if (nv > 1.25) nv = 1.25;
    m->volume.volume = nv;
    m->volume.available = true;
    m->volume_changed = true;
    m->last_vol_poll_ms = monotonic_ms() - 700;
}

void xdi_media_volume_set(xdi_media_t *m, double fraction) {
    if (!m) return;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    char arg[32];
    snprintf(arg, sizeof(arg), "%.2f", fraction);
    const char *const argv[] = {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", arg, NULL};
    run_cmd_detached(argv);

    m->volume.volume = fraction;
    m->volume.available = true;
    m->volume_changed = true;
    m->last_vol_poll_ms = monotonic_ms() - 700;
}

void xdi_media_toggle_mute(xdi_media_t *m) {
    if (!m) return;
    const char *const argv[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL};
    run_cmd_detached(argv);
    m->volume.muted = !m->volume.muted;
    m->volume.available = true;
    m->volume_changed = true;
    m->last_vol_poll_ms = monotonic_ms() - 700;
}
