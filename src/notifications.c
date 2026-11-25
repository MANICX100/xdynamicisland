#include "notifications.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void resolve_history_path(char *buf, size_t len) {
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] != '\0') {
        snprintf(buf, len, "%s/xdynamicisland/notifications.tsv", xdg);
    } else if (home && home[0] != '\0') {
        snprintf(buf, len, "%s/.local/share/xdynamicisland/notifications.tsv", home);
    } else {
        snprintf(buf, len, "/tmp/xdynamicisland-notifications.tsv");
    }
}

static void mkdir_parent(const char *path) {
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

/* Strip newlines/tabs so TSV persistence stays 1-line-per-notification */
static void sanitize_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            if (j > 0 && dst[j - 1] != ' ') dst[j++] = ' ';
        } else if (c >= 0x20) {
            dst[j++] = (char)c;
        }
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
}

void xdi_notif_init_store_only(xdi_notif_store_t *store, int max_items, const char *history_path) {
    memset(store, 0, sizeof(*store));
    if (max_items < 5) max_items = 5;
    if (max_items > XDI_NOTIF_MAX_CAP) max_items = XDI_NOTIF_MAX_CAP;
    store->max_items = max_items;
    store->next_seq = 1;
    if (history_path && history_path[0]) {
        store->persist = true;
        snprintf(store->history_path, sizeof(store->history_path), "%s", history_path);
    }
}

static void parse_notify_message(xdi_notif_store_t *store, DBusMessage *msg) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) return;

    const char *app_name = "";
    uint32_t replaces_id = 0;
    const char *app_icon = "";
    const char *summary = "";
    const char *body = "";
    int urgency = 1;

    /* 1. s app_name */
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &app_name);
    }
    if (!dbus_message_iter_next(&iter)) return;

    /* 2. u replaces_id */
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&iter, &replaces_id);
    }
    if (!dbus_message_iter_next(&iter)) return;

    /* 3. s app_icon */
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &app_icon);
    }
    if (!dbus_message_iter_next(&iter)) return;

    /* 4. s summary */
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &summary);
    }
    if (!dbus_message_iter_next(&iter)) return;

    /* 5. s body */
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &body);
    }

    /* 6. as actions (skip) */
    if (dbus_message_iter_next(&iter)) {
        /* 7. a{sv} hints -> look for "urgency" */
        if (dbus_message_iter_next(&iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr;
            dbus_message_iter_recurse(&iter, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&arr, &entry);
                if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                    const char *k = NULL;
                    dbus_message_iter_get_basic(&entry, &k);
                    if (k && strcmp(k, "urgency") == 0 && dbus_message_iter_next(&entry) &&
                        dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var;
                        dbus_message_iter_recurse(&entry, &var);
                        int t = dbus_message_iter_get_arg_type(&var);
                        if (t == DBUS_TYPE_BYTE) {
                            unsigned char u = 1;
                            dbus_message_iter_get_basic(&var, &u);
                            urgency = (int)u;
                        } else if (t == DBUS_TYPE_INT32) {
                            int32_t u = 1;
                            dbus_message_iter_get_basic(&var, &u);
                            urgency = (int)u;
                        } else if (t == DBUS_TYPE_UINT32) {
                            uint32_t u = 1;
                            dbus_message_iter_get_basic(&var, &u);
                            urgency = (int)u;
                        }
                    }
                }
                dbus_message_iter_next(&arr);
            }
        }
    }

    if ((summary && summary[0]) || (body && body[0])) {
        xdi_notif_push(store, replaces_id,
                       (app_name && app_name[0]) ? app_name : "Notification",
                       app_icon, summary, body, urgency, time(NULL));
    }
}

static DBusHandlerResult monitor_filter(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)conn;
    xdi_notif_store_t *store = (xdi_notif_store_t *)user_data;
    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
        dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify")) {
        parse_notify_message(store, msg);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static bool attach_dbus_monitor(xdi_notif_store_t *store) {
    DBusError err;
    dbus_error_init(&err);

    /* Private connection required because BecomeMonitor converts the connection into read-only monitor mode */
    DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    /* Prefer modern BecomeMonitor (works on both dbus-broker & dbus-daemon) */
    DBusMessage *req = dbus_message_new_method_call(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus.Monitoring",
        "BecomeMonitor"
    );
    bool ok = false;
    if (req) {
        DBusMessageIter args, arr;
        dbus_message_iter_init_append(req, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &arr);
        const char *rule = "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &rule);
        dbus_message_iter_close_container(&args, &arr);
        uint32_t flags = 0;
        dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &flags);

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, req, 1500, &err);
        dbus_message_unref(req);
        if (reply) {
            dbus_message_unref(reply);
            ok = true;
        } else {
            dbus_error_free(&err);
        }
    }

    /* Fallback to legacy eavesdrop match rule for older dbus-daemon setups */
    if (!ok) {
        dbus_bus_add_match(conn,
                           "eavesdrop='true',type='method_call',interface='org.freedesktop.Notifications',member='Notify'",
                           &err);
        if (!dbus_error_is_set(&err)) {
            ok = true;
        } else {
            dbus_error_free(&err);
        }
    }

    if (!ok) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return false;
    }

    dbus_connection_add_filter(conn, monitor_filter, store, NULL);
    store->monitor_conn = conn;
    return true;
}

bool xdi_notif_init(xdi_notif_store_t *store, int max_items, bool persist) {
    char path[512] = {0};
    if (persist) {
        resolve_history_path(path, sizeof(path));
    }
    xdi_notif_init_store_only(store, max_items, persist ? path : NULL);
    if (persist) {
        xdi_notif_load(store);
        /* Loaded historical items shouldn't trigger fresh toast banners on startup */
        store->has_new_toast = false;
    }
    return attach_dbus_monitor(store);
}

void xdi_notif_destroy(xdi_notif_store_t *store) {
    if (!store) return;
    if (store->persist) {
        xdi_notif_save(store);
    }
    if (store->monitor_conn) {
        dbus_connection_close(store->monitor_conn);
        dbus_connection_unref(store->monitor_conn);
        store->monitor_conn = NULL;
    }
}

int xdi_notif_get_fd(xdi_notif_store_t *store) {
    if (!store || !store->monitor_conn) return -1;
    int fd = -1;
    if (dbus_connection_get_unix_fd(store->monitor_conn, &fd)) {
        return fd;
    }
    return -1;
}

void xdi_notif_poll(xdi_notif_store_t *store) {
    if (!store || !store->monitor_conn) return;
    dbus_connection_read_write(store->monitor_conn, 0);
    while (dbus_connection_dispatch(store->monitor_conn) == DBUS_DISPATCH_DATA_REMAINS) {
        /* Drain all queued notifications */
    }
}

static void recompute_unread(xdi_notif_store_t *store) {
    int u = 0;
    for (int i = 0; i < store->count; i++) {
        if (!store->items[i].read) u++;
    }
    store->unread_count = u;
}

void xdi_notif_push(xdi_notif_store_t *store, uint32_t replaces_id,
                    const char *app_name, const char *app_icon,
                    const char *summary, const char *body,
                    int urgency, time_t ts) {
    if (!store) return;

    xdi_notification_t item;
    memset(&item, 0, sizeof(item));
    item.seq_id = store->next_seq++;
    item.dbus_id = replaces_id;
    item.timestamp = (ts > 0) ? ts : time(NULL);
    sanitize_text(item.app_name, sizeof(item.app_name), app_name ? app_name : "System");
    sanitize_text(item.app_icon, sizeof(item.app_icon), app_icon ? app_icon : "");
    sanitize_text(item.summary, sizeof(item.summary), summary ? summary : "");
    sanitize_text(item.body, sizeof(item.body), body ? body : "");
    item.urgency = urgency;
    item.read = false;

    /* If replaces_id matches an existing entry, remove old copy so updated one moves to top */
    if (replaces_id != 0) {
        for (int i = 0; i < store->count; i++) {
            if (store->items[i].dbus_id == replaces_id) {
                memmove(&store->items[i], &store->items[i + 1],
                        (size_t)(store->count - i - 1) * sizeof(xdi_notification_t));
                store->count--;
                break;
            }
        }
    }

    int limit = store->max_items;
    if (limit > XDI_NOTIF_MAX_CAP) limit = XDI_NOTIF_MAX_CAP;
    if (store->count >= limit) {
        store->count = limit - 1;
    }

    if (store->count > 0) {
        memmove(&store->items[1], &store->items[0],
                (size_t)store->count * sizeof(xdi_notification_t));
    }
    store->items[0] = item;
    store->count++;

    store->latest = item;
    store->has_new_toast = true;
    recompute_unread(store);

    if (store->persist && store->history_path[0]) {
        xdi_notif_save(store);
    }
}

bool xdi_notif_remove_at(xdi_notif_store_t *store, int index) {
    if (!store || index < 0 || index >= store->count) return false;
    memmove(&store->items[index], &store->items[index + 1],
            (size_t)(store->count - index - 1) * sizeof(xdi_notification_t));
    store->count--;
    recompute_unread(store);
    if (store->persist && store->history_path[0]) {
        xdi_notif_save(store);
    }
    return true;
}

void xdi_notif_mark_all_read(xdi_notif_store_t *store) {
    if (!store) return;
    for (int i = 0; i < store->count; i++) {
        store->items[i].read = true;
    }
    store->unread_count = 0;
}

void xdi_notif_clear(xdi_notif_store_t *store) {
    if (!store) return;
    store->count = 0;
    store->unread_count = 0;
    store->has_new_toast = false;
    if (store->persist && store->history_path[0]) {
        xdi_notif_save(store);
    }
}

bool xdi_notif_save(const xdi_notif_store_t *store) {
    if (!store || !store->history_path[0]) return false;
    mkdir_parent(store->history_path);
    FILE *fp = fopen(store->history_path, "w");
    if (!fp) return false;
    for (int i = 0; i < store->count; i++) {
        const xdi_notification_t *n = &store->items[i];
        fprintf(fp, "%lld\t%u\t%d\t%d\t%s\t%s\t%s\n",
                (long long)n->timestamp, n->dbus_id, n->urgency, n->read ? 1 : 0,
                n->app_name, n->summary, n->body);
    }
    fclose(fp);
    return true;
}

bool xdi_notif_load(xdi_notif_store_t *store) {
    if (!store || !store->history_path[0]) return false;
    FILE *fp = fopen(store->history_path, "r");
    if (!fp) return false;

    store->count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp) && store->count < store->max_items) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        char *cols[7] = {0};
        char *p = line;
        for (int c = 0; c < 7; c++) {
            cols[c] = p;
            if (c < 6) {
                char *tab = strchr(p, '\t');
                if (!tab) {
                    cols[c] = NULL;
                    break;
                }
                *tab = '\0';
                p = tab + 1;
            }
        }
        if (!cols[5]) continue;

        xdi_notification_t *n = &store->items[store->count];
        memset(n, 0, sizeof(*n));
        n->seq_id = store->next_seq++;
        n->timestamp = (time_t)atoll(cols[0]);
        n->dbus_id = (uint32_t)strtoul(cols[1], NULL, 10);
        n->urgency = atoi(cols[2]);
        n->read = (atoi(cols[3]) != 0);
        snprintf(n->app_name, sizeof(n->app_name), "%s", cols[4]);
        snprintf(n->summary, sizeof(n->summary), "%s", cols[5]);
        snprintf(n->body, sizeof(n->body), "%s", cols[6] ? cols[6] : "");
        store->count++;
    }
    fclose(fp);
    recompute_unread(store);
    return true;
}

void xdi_notif_format_time(time_t ts, time_t now, char *buf, size_t len) {
    if (!buf || len == 0) return;
    long diff = (long)(now - ts);
    if (diff < 60) {
        snprintf(buf, len, "now");
    } else if (diff < 3600) {
        snprintf(buf, len, "%ldm", diff / 60);
    } else if (diff < 86400) {
        snprintf(buf, len, "%ldh", diff / 3600);
    } else {
        struct tm tm_val;
        localtime_r(&ts, &tm_val);
        strftime(buf, len, "%b %d", &tm_val);
    }
}
