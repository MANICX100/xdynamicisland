#include "notifications.h"

#include <ctype.h>
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

static bool contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

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

static void recompute_unread(xdi_notif_store_t *store) {
    int u = 0;
    for (int i = 0; i < store->count; i++) {
        if (!store->items[i].read) u++;
    }
    store->unread_count = u;
}

static void sync_latest_if_matching(xdi_notif_store_t *store, const xdi_notification_t *item) {
    if (store->latest.seq_id == item->seq_id ||
        (item->kde_public_id[0] && strcmp(store->latest.kde_public_id, item->kde_public_id) == 0)) {
        store->latest = *item;
    }
}

/* Bind parsed actions & synthesize Reply/Dismiss actions for KDE Connect notifications */
static void bind_kdeconnect_actions(xdi_notification_t *n) {
    if (!n->is_kdeconnect || !n->kde_dev_id[0] || !n->kde_public_id[0]) return;

    bool has_reply = false;
    bool has_dismiss = false;

    for (int i = 0; i < n->action_count; i++) {
        if (strcasecmp(n->actions[i].key, "inline-reply") == 0 ||
            strcasecmp(n->actions[i].key, "reply") == 0 ||
            strcasecmp(n->actions[i].label, "Reply") == 0 ||
            contains_ci(n->actions[i].label, "Reply")) {
            if (n->kde_reply_id[0]) {
                n->actions[i].kind = XDI_ACT_KDE_REPLY;
                snprintf(n->actions[i].label, sizeof(n->actions[i].label), "Reply");
                has_reply = true;
            } else if (n->kde_internal_id[0]) {
                n->actions[i].kind = XDI_ACT_KDE_ACTION;
            }
        } else if (strcasecmp(n->actions[i].label, "Dismiss") == 0 && n->kde_dismissable) {
            n->actions[i].kind = XDI_ACT_KDE_DISMISS;
            has_dismiss = true;
        } else if (n->kde_internal_id[0]) {
            n->actions[i].kind = XDI_ACT_KDE_ACTION;
        }
    }

    if (n->kde_reply_id[0] && !has_reply && n->action_count < XDI_NOTIF_MAX_ACTIONS) {
        xdi_notif_action_t *a = &n->actions[n->action_count++];
        snprintf(a->key, sizeof(a->key), "reply");
        snprintf(a->label, sizeof(a->label), "Reply");
        a->kind = XDI_ACT_KDE_REPLY;
    }
    if (n->kde_dismissable && !has_dismiss && n->action_count < XDI_NOTIF_MAX_ACTIONS) {
        xdi_notif_action_t *a = &n->actions[n->action_count++];
        snprintf(a->key, sizeof(a->key), "dismiss");
        snprintf(a->label, sizeof(a->label), "Dismiss");
        a->kind = XDI_ACT_KDE_DISMISS;
    }
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

/* ---------- D-Bus Notify Parsing (including `as actions` and KDE Connect hints) ---------- */

static void parse_notify_message(xdi_notif_store_t *store, DBusMessage *msg) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) return;

    const char *app_name = "";
    uint32_t replaces_id = 0;
    const char *app_icon = "";
    const char *summary = "";
    const char *body = "";
    int urgency = 1;
    bool is_kde_hint = false;
    xdi_notif_action_t actions[XDI_NOTIF_MAX_ACTIONS];
    int action_count = 0;
    memset(actions, 0, sizeof(actions));

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

    /* 6. as actions -> [key0, label0, key1, label1, ...] */
    if (dbus_message_iter_next(&iter)) {
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr;
            dbus_message_iter_recurse(&iter, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING &&
                   action_count < XDI_NOTIF_MAX_ACTIONS) {
                const char *akey = NULL, *alabel = NULL;
                dbus_message_iter_get_basic(&arr, &akey);
                if (!dbus_message_iter_next(&arr)) break;
                if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&arr, &alabel);
                }
                dbus_message_iter_next(&arr);

                if (akey && alabel && alabel[0]) {
                    /* Skip unlabeled or generic "default" click actions unless it's Reply */
                    if (strcmp(akey, "default") == 0 && !contains_ci(alabel, "reply")) {
                        continue;
                    }
                    sanitize_text(actions[action_count].key, sizeof(actions[action_count].key), akey);
                    sanitize_text(actions[action_count].label, sizeof(actions[action_count].label), alabel);
                    actions[action_count].kind = XDI_ACT_NONE;
                    action_count++;
                }
            }
        }

        /* 7. a{sv} hints -> look for "urgency", "desktop-entry", "x-kde-*" */
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
                    if (k && dbus_message_iter_next(&entry) &&
                        dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var;
                        dbus_message_iter_recurse(&entry, &var);
                        int t = dbus_message_iter_get_arg_type(&var);
                        if (strcmp(k, "urgency") == 0) {
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
                        } else if (strcmp(k, "desktop-entry") == 0 && t == DBUS_TYPE_STRING) {
                            const char *de = NULL;
                            dbus_message_iter_get_basic(&var, &de);
                            if (de && contains_ci(de, "kdeconnect")) is_kde_hint = true;
                        } else if (strncmp(k, "x-kde-reply", 11) == 0) {
                            is_kde_hint = true;
                        }
                    }
                }
                dbus_message_iter_next(&arr);
            }
        }
    }

    if (app_name && contains_ci(app_name, "kdeconnect")) is_kde_hint = true;
    if (app_name && contains_ci(app_name, "kde connect")) is_kde_hint = true;

    if ((summary && summary[0]) || (body && body[0])) {
        xdi_notif_push_with_actions(store, replaces_id,
                                    (app_name && app_name[0]) ? app_name : "Notification",
                                    app_icon, summary, body, urgency, time(NULL),
                                    is_kde_hint, actions, action_count);
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

    DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

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

/* ---------- KDE Connect D-Bus Notification Plugin Integration ---------- */

static void fetch_kde_notif_details(xdi_notif_store_t *store, const char *dev_id,
                                    const char *public_id, bool allow_toast) {
    if (!store->ctrl_conn || !dev_id || !dev_id[0] || !public_id || !public_id[0]) return;

    char path[256];
    snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s/notifications/%s", dev_id, public_id);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.kde.kdeconnect", path, "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return;
    const char *iface = "org.kde.kdeconnect.device.notifications.notification";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(store->ctrl_conn, msg, 250, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }

    char internal_id[128] = {0};
    char app_name[64] = {0};
    char title[192] = {0};
    char text[384] = {0};
    char ticker[256] = {0};
    char reply_id[64] = {0};
    bool dismissable = false;
    bool silent = !allow_toast;

    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr;
        dbus_message_iter_recurse(&iter, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter kv;
            dbus_message_iter_recurse(&arr, &kv);
            if (dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_STRING) {
                const char *k = NULL;
                dbus_message_iter_get_basic(&kv, &k);
                if (k && dbus_message_iter_next(&kv) &&
                    dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_VARIANT) {
                    DBusMessageIter var;
                    dbus_message_iter_recurse(&kv, &var);
                    int t = dbus_message_iter_get_arg_type(&var);
                    if (t == DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&var, &s);
                        if (s) {
                            if (strcmp(k, "internalId") == 0) snprintf(internal_id, sizeof(internal_id), "%s", s);
                            else if (strcmp(k, "appName") == 0) snprintf(app_name, sizeof(app_name), "%s", s);
                            else if (strcmp(k, "title") == 0) snprintf(title, sizeof(title), "%s", s);
                            else if (strcmp(k, "text") == 0) snprintf(text, sizeof(text), "%s", s);
                            else if (strcmp(k, "ticker") == 0) snprintf(ticker, sizeof(ticker), "%s", s);
                            else if (strcmp(k, "replyId") == 0) snprintf(reply_id, sizeof(reply_id), "%s", s);
                        }
                    } else if (t == DBUS_TYPE_BOOLEAN) {
                        dbus_bool_t b = FALSE;
                        dbus_message_iter_get_basic(&var, &b);
                        if (strcmp(k, "dismissable") == 0) dismissable = (b != FALSE);
                        else if (strcmp(k, "silent") == 0 && b != FALSE) silent = true;
                    }
                }
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);

    if (title[0] == '\0' && ticker[0] != '\0') {
        snprintf(title, sizeof(title), "%s", ticker);
    }
    xdi_notif_merge_kdeconnect(store, dev_id, public_id, internal_id,
                               app_name[0] ? app_name : "KDE Connect",
                               title, text, reply_id, dismissable, silent, time(NULL));
}

static bool extract_dev_id_from_path(const char *path, char *out, size_t len) {
    static const char prefix[] = "/modules/kdeconnect/devices/";
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *s = path + sizeof(prefix) - 1;
    const char *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    if (n == 0 || n >= len) return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

static DBusHandlerResult kdeconnect_notif_filter(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)conn;
    xdi_notif_store_t *store = (xdi_notif_store_t *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *iface = dbus_message_get_interface(msg);
    if (!iface || strcmp(iface, "org.kde.kdeconnect.device.notifications") != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *member = dbus_message_get_member(msg);
    const char *path = dbus_message_get_path(msg);
    if (!member || !path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    char dev_id[64] = {0};
    if (!extract_dev_id_from_path(path, dev_id, sizeof(dev_id))) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(member, "notificationPosted") == 0 || strcmp(member, "notificationUpdated") == 0) {
        const char *pub_id = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pub_id, DBUS_TYPE_INVALID) &&
            pub_id && pub_id[0]) {
            fetch_kde_notif_details(store, dev_id, pub_id, strcmp(member, "notificationPosted") == 0);
        }
    } else if (strcmp(member, "notificationRemoved") == 0) {
        const char *pub_id = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pub_id, DBUS_TYPE_INVALID) &&
            pub_id && pub_id[0]) {
            xdi_notif_kde_removed(store, pub_id);
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void sync_active_kdeconnect_notifications(xdi_notif_store_t *store) {
    if (!store->ctrl_conn) return;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.kde.kdeconnect", "/modules/kdeconnect",
        "org.kde.kdeconnect.daemon", "devices");
    if (!msg) return;
    dbus_bool_t only_reachable = TRUE, only_paired = TRUE;
    dbus_message_append_args(msg,
                             DBUS_TYPE_BOOLEAN, &only_reachable,
                             DBUS_TYPE_BOOLEAN, &only_paired,
                             DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(store->ctrl_conn, msg, 250, &err);
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
            const char *dev_id = NULL;
            dbus_message_iter_get_basic(&arr, &dev_id);
            if (dev_id && dev_id[0]) {
                char path[256];
                snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s/notifications", dev_id);
                DBusMessage *nmsg = dbus_message_new_method_call(
                    "org.kde.kdeconnect", path,
                    "org.kde.kdeconnect.device.notifications", "activeNotifications");
                if (nmsg) {
                    DBusError nerr;
                    dbus_error_init(&nerr);
                    DBusMessage *nrep = dbus_connection_send_with_reply_and_block(
                        store->ctrl_conn, nmsg, 250, &nerr);
                    dbus_message_unref(nmsg);
                    if (nrep) {
                        DBusMessageIter ni, narr;
                        if (dbus_message_iter_init(nrep, &ni) &&
                            dbus_message_iter_get_arg_type(&ni) == DBUS_TYPE_ARRAY) {
                            dbus_message_iter_recurse(&ni, &narr);
                            while (dbus_message_iter_get_arg_type(&narr) == DBUS_TYPE_STRING) {
                                const char *pub_id = NULL;
                                dbus_message_iter_get_basic(&narr, &pub_id);
                                if (pub_id && pub_id[0]) {
                                    fetch_kde_notif_details(store, dev_id, pub_id, false);
                                }
                                dbus_message_iter_next(&narr);
                            }
                        }
                        dbus_message_unref(nrep);
                    } else {
                        dbus_error_free(&nerr);
                    }
                }
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
}

void xdi_notif_attach_kdeconnect(xdi_notif_store_t *store, DBusConnection *ctrl_conn) {
    if (!store || !ctrl_conn) return;
    store->ctrl_conn = ctrl_conn;
    dbus_bus_add_match(ctrl_conn,
        "type='signal',interface='org.kde.kdeconnect.device.notifications'", NULL);
    dbus_connection_add_filter(ctrl_conn, kdeconnect_notif_filter, store, NULL);
    sync_active_kdeconnect_notifications(store);
    store->has_new_toast = false;
}

/* ---------- Public Lifecycle & Store Operations ---------- */

bool xdi_notif_init(xdi_notif_store_t *store, int max_items, bool persist) {
    char path[512] = {0};
    if (persist) {
        resolve_history_path(path, sizeof(path));
    }
    xdi_notif_init_store_only(store, max_items, persist ? path : NULL);
    if (persist) {
        xdi_notif_load(store);
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
    store->ctrl_conn = NULL;
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

void xdi_notif_push_with_actions(xdi_notif_store_t *store, uint32_t replaces_id,
                                 const char *app_name, const char *app_icon,
                                 const char *summary, const char *body,
                                 int urgency, time_t ts, bool is_kde_hint,
                                 const xdi_notif_action_t *actions, int action_count) {
    if (!store) return;
    time_t now_ts = (ts > 0) ? ts : time(NULL);

    char clean_app[64], clean_sum[192], clean_body[384];
    sanitize_text(clean_app, sizeof(clean_app), app_name ? app_name : "System");
    sanitize_text(clean_sum, sizeof(clean_sum), summary ? summary : "");
    sanitize_text(clean_body, sizeof(clean_body), body ? body : "");

    /* Check if this Notify matches a KDE Connect notificationPosted that arrived milliseconds earlier */
    if (is_kde_hint) {
        for (int i = 0; i < store->count && i < 6; i++) {
            xdi_notification_t *ex = &store->items[i];
            if (ex->is_kdeconnect && labs((long)(now_ts - ex->timestamp)) <= 5 &&
                ((clean_body[0] && contains_ci(clean_body, ex->body)) ||
                 (clean_sum[0] && (contains_ci(clean_sum, ex->summary) || contains_ci(clean_sum, ex->app_name))))) {
                ex->dbus_id = replaces_id;
                ex->urgency = urgency;
                if (actions && action_count > 0) {
                    int cap = (action_count > XDI_NOTIF_MAX_ACTIONS) ? XDI_NOTIF_MAX_ACTIONS : action_count;
                    memcpy(ex->actions, actions, (size_t)cap * sizeof(xdi_notif_action_t));
                    ex->action_count = cap;
                    bind_kdeconnect_actions(ex);
                }
                store->latest = *ex;
                store->has_new_toast = true;
                store->changed = true;
                return;
            }
        }
    }

    xdi_notification_t item;
    memset(&item, 0, sizeof(item));
    item.seq_id = store->next_seq++;
    item.dbus_id = replaces_id;
    item.timestamp = now_ts;
    snprintf(item.app_name, sizeof(item.app_name), "%s", clean_app);
    sanitize_text(item.app_icon, sizeof(item.app_icon), app_icon ? app_icon : "");
    snprintf(item.summary, sizeof(item.summary), "%s", clean_sum);
    snprintf(item.body, sizeof(item.body), "%s", clean_body);
    item.urgency = urgency;
    item.read = false;
    item.is_kdeconnect = is_kde_hint;

    if (actions && action_count > 0) {
        int cap = (action_count > XDI_NOTIF_MAX_ACTIONS) ? XDI_NOTIF_MAX_ACTIONS : action_count;
        memcpy(item.actions, actions, (size_t)cap * sizeof(xdi_notif_action_t));
        item.action_count = cap;
    }

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
    store->changed = true;
    recompute_unread(store);

    if (store->persist && store->history_path[0]) {
        xdi_notif_save(store);
    }
}

void xdi_notif_push(xdi_notif_store_t *store, uint32_t replaces_id,
                    const char *app_name, const char *app_icon,
                    const char *summary, const char *body,
                    int urgency, time_t ts) {
    xdi_notif_push_with_actions(store, replaces_id, app_name, app_icon,
                                summary, body, urgency, ts, false, NULL, 0);
}

void xdi_notif_merge_kdeconnect(xdi_notif_store_t *store,
                                const char *dev_id, const char *public_id,
                                const char *internal_id, const char *app_name,
                                const char *title, const char *text,
                                const char *reply_id, bool dismissable,
                                bool silent, time_t ts) {
    if (!store || !dev_id || !public_id) return;
    time_t now_ts = (ts > 0) ? ts : time(NULL);

    char clean_app[64], clean_title[192], clean_text[384];
    sanitize_text(clean_app, sizeof(clean_app), app_name ? app_name : "KDE Connect");
    sanitize_text(clean_title, sizeof(clean_title), title ? title : "");
    sanitize_text(clean_text, sizeof(clean_text), text ? text : "");

    /* 1. Try matching existing entry by public_id or recent correlated Notify call */
    int match = -1;
    for (int i = 0; i < store->count; i++) {
        const xdi_notification_t *ex = &store->items[i];
        if (ex->kde_public_id[0] && strcmp(ex->kde_public_id, public_id) == 0) {
            match = i;
            break;
        }
        if (i < 6 && labs((long)(now_ts - ex->timestamp)) <= 5 &&
            (ex->is_kdeconnect || contains_ci(ex->app_name, "kde")) &&
            ((clean_text[0] && contains_ci(ex->body, clean_text)) ||
             (clean_title[0] && (contains_ci(ex->summary, clean_title) || contains_ci(ex->body, clean_title))) ||
             (clean_app[0] && contains_ci(ex->summary, clean_app)))) {
            match = i;
            break;
        }
    }

    if (match >= 0) {
        xdi_notification_t *n = &store->items[match];
        n->is_kdeconnect = true;
        n->kde_dismissable = dismissable;
        snprintf(n->kde_dev_id, sizeof(n->kde_dev_id), "%s", dev_id);
        snprintf(n->kde_public_id, sizeof(n->kde_public_id), "%s", public_id);
        if (internal_id && internal_id[0]) {
            snprintf(n->kde_internal_id, sizeof(n->kde_internal_id), "%s", internal_id);
        }
        if (reply_id) {
            snprintf(n->kde_reply_id, sizeof(n->kde_reply_id), "%s", reply_id);
        }
        bind_kdeconnect_actions(n);
        sync_latest_if_matching(store, n);
        store->changed = true;
        return;
    }

    /* 2. Insert as a new KDE Connect notification entry */
    xdi_notification_t item;
    memset(&item, 0, sizeof(item));
    item.seq_id = store->next_seq++;
    item.timestamp = now_ts;
    snprintf(item.app_name, sizeof(item.app_name), "%s", clean_app);
    snprintf(item.summary, sizeof(item.summary), "%s", clean_title);
    snprintf(item.body, sizeof(item.body), "%s", clean_text);
    item.urgency = 1;
    item.read = silent;
    item.is_kdeconnect = true;
    item.kde_dismissable = dismissable;
    snprintf(item.kde_dev_id, sizeof(item.kde_dev_id), "%s", dev_id);
    snprintf(item.kde_public_id, sizeof(item.kde_public_id), "%s", public_id);
    if (internal_id) snprintf(item.kde_internal_id, sizeof(item.kde_internal_id), "%s", internal_id);
    if (reply_id) snprintf(item.kde_reply_id, sizeof(item.kde_reply_id), "%s", reply_id);

    bind_kdeconnect_actions(&item);

    int limit = store->max_items;
    if (limit > XDI_NOTIF_MAX_CAP) limit = XDI_NOTIF_MAX_CAP;
    if (store->count >= limit) store->count = limit - 1;
    if (store->count > 0) {
        memmove(&store->items[1], &store->items[0],
                (size_t)store->count * sizeof(xdi_notification_t));
    }
    store->items[0] = item;
    store->count++;

    if (!silent) {
        store->latest = item;
        store->has_new_toast = true;
    }
    store->changed = true;
    recompute_unread(store);
    if (store->persist && store->history_path[0]) {
        xdi_notif_save(store);
    }
}

void xdi_notif_kde_removed(xdi_notif_store_t *store, const char *public_id) {
    if (!store || !public_id || !public_id[0]) return;
    for (int i = 0; i < store->count; i++) {
        if (strcmp(store->items[i].kde_public_id, public_id) == 0) {
            store->items[i].action_count = 0;
            store->items[i].kde_dismissable = false;
            store->items[i].kde_reply_id[0] = '\0';
            store->items[i].kde_public_id[0] = '\0';
            store->changed = true;
            sync_latest_if_matching(store, &store->items[i]);
            break;
        }
    }
}

static bool dispatch_action_on_item(xdi_notif_store_t *store, xdi_notification_t *n, int action_index,
                                    bool *should_remove) {
    if (!n || action_index < 0 || action_index >= n->action_count) return false;
    *should_remove = false;

    const xdi_notif_action_t *act = &n->actions[action_index];
    if (act->kind == XDI_ACT_NONE) return false;

    if (!store->ctrl_conn || !n->kde_dev_id[0]) {
        return false;
    }

    if (act->kind == XDI_ACT_KDE_REPLY && n->kde_public_id[0]) {
        char path[256];
        snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s/notifications/%s",
                 n->kde_dev_id, n->kde_public_id);
        DBusMessage *msg = dbus_message_new_method_call(
            "org.kde.kdeconnect", path,
            "org.kde.kdeconnect.device.notifications.notification", "reply");
        if (!msg) return false;
        dbus_connection_send(store->ctrl_conn, msg, NULL);
        dbus_connection_flush(store->ctrl_conn);
        dbus_message_unref(msg);
        return true;
    }

    if (act->kind == XDI_ACT_KDE_DISMISS && n->kde_public_id[0]) {
        char path[256];
        snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s/notifications/%s",
                 n->kde_dev_id, n->kde_public_id);
        DBusMessage *msg = dbus_message_new_method_call(
            "org.kde.kdeconnect", path,
            "org.kde.kdeconnect.device.notifications.notification", "dismiss");
        if (!msg) return false;
        dbus_connection_send(store->ctrl_conn, msg, NULL);
        dbus_connection_flush(store->ctrl_conn);
        dbus_message_unref(msg);
        *should_remove = true;
        return true;
    }

    if (act->kind == XDI_ACT_KDE_ACTION && n->kde_internal_id[0]) {
        char path[256];
        snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s/notifications", n->kde_dev_id);
        DBusMessage *msg = dbus_message_new_method_call(
            "org.kde.kdeconnect", path,
            "org.kde.kdeconnect.device.notifications", "sendAction");
        if (!msg) return false;
        const char *key = n->kde_internal_id;
        const char *action = act->label;
        dbus_message_append_args(msg,
                                 DBUS_TYPE_STRING, &key,
                                 DBUS_TYPE_STRING, &action,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(store->ctrl_conn, msg, NULL);
        dbus_connection_flush(store->ctrl_conn);
        dbus_message_unref(msg);
        return true;
    }

    return false;
}

bool xdi_notif_invoke_action(xdi_notif_store_t *store, int item_index, int action_index) {
    if (!store || item_index < 0 || item_index >= store->count) return false;
    bool remove_item = false;
    bool ok = dispatch_action_on_item(store, &store->items[item_index], action_index, &remove_item);
    if (ok && remove_item) {
        /* Avoid double-dismissing in xdi_notif_remove_at */
        store->items[item_index].kde_dismissable = false;
        xdi_notif_remove_at(store, item_index);
    }
    return ok;
}

bool xdi_notif_invoke_latest_action(xdi_notif_store_t *store, int action_index) {
    if (!store) return false;
    for (int i = 0; i < store->count; i++) {
        if (store->items[i].seq_id == store->latest.seq_id) {
            return xdi_notif_invoke_action(store, i, action_index);
        }
    }
    bool remove_item = false;
    return dispatch_action_on_item(store, &store->latest, action_index, &remove_item);
}

bool xdi_notif_remove_at(xdi_notif_store_t *store, int index) {
    if (!store || index < 0 || index >= store->count) return false;

    /* If removing a dismissable KDE Connect notification, dismiss it on the phone too */
    xdi_notification_t *n = &store->items[index];
    if (store->ctrl_conn && n->is_kdeconnect && n->kde_dismissable &&
        n->kde_dev_id[0] && n->kde_public_id[0]) {
        char path[256];
        snprintf(path, sizeof(path), "/modules/kdeconnect/devices/%s/notifications/%s",
                 n->kde_dev_id, n->kde_public_id);
        DBusMessage *msg = dbus_message_new_method_call(
            "org.kde.kdeconnect", path,
            "org.kde.kdeconnect.device.notifications.notification", "dismiss");
        if (msg) {
            dbus_connection_send(store->ctrl_conn, msg, NULL);
            dbus_connection_flush(store->ctrl_conn);
            dbus_message_unref(msg);
        }
    }

    memmove(&store->items[index], &store->items[index + 1],
            (size_t)(store->count - index - 1) * sizeof(xdi_notification_t));
    store->count--;
    recompute_unread(store);
    store->changed = true;
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
    store->changed = true;
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
