#ifndef XDI_NOTIFICATIONS_H
#define XDI_NOTIFICATIONS_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XDI_NOTIF_MAX_CAP 64
#define XDI_NOTIF_MAX_ACTIONS 4

typedef enum {
    XDI_ACT_NONE = 0,
    XDI_ACT_KDE_ACTION,   /* org.kde.kdeconnect.device.notifications.sendAction(internalId, label) */
    XDI_ACT_KDE_REPLY,    /* org.kde.kdeconnect.device.notifications.notification.reply() */
    XDI_ACT_KDE_DISMISS   /* org.kde.kdeconnect.device.notifications.notification.dismiss() */
} xdi_notif_action_kind_t;

typedef struct {
    char key[48];
    char label[40];
    xdi_notif_action_kind_t kind;
} xdi_notif_action_t;

typedef struct {
    uint64_t seq_id;       /* Internal monotonically increasing ID */
    uint32_t dbus_id;      /* Notification replaces_id / daemon id */
    time_t timestamp;
    char app_name[64];
    char app_icon[128];
    char summary[192];
    char body[384];
    int urgency;           /* 0=low, 1=normal, 2=critical */
    bool read;

    /* Interactive actions (primarily KDE Connect Android actions, Reply, Dismiss) */
    xdi_notif_action_t actions[XDI_NOTIF_MAX_ACTIONS];
    int action_count;

    /* KDE Connect D-Bus notification binding */
    bool is_kdeconnect;
    bool kde_dismissable;
    char kde_dev_id[64];
    char kde_public_id[64];
    char kde_internal_id[128];
    char kde_reply_id[64];
} xdi_notification_t;

typedef struct {
    DBusConnection *monitor_conn; /* Dedicated D-Bus BecomeMonitor connection */
    DBusConnection *ctrl_conn;    /* Borrowed regular D-Bus connection for KDE Connect calls/signals */
    xdi_notification_t items[XDI_NOTIF_MAX_CAP];
    int count;                    /* Stored items (newest at index 0) */
    int max_items;
    int unread_count;
    uint64_t next_seq;
    bool has_new_toast;
    bool changed;
    xdi_notification_t latest;
    bool persist;
    char history_path[512];
} xdi_notif_store_t;

/* Initialize notification history store & attach non-invasive D-Bus monitor for notification-daemon. */
bool xdi_notif_init(xdi_notif_store_t *store, int max_items, bool persist);

/* Attach regular D-Bus session connection to receive KDE Connect notification signals & execute actions. */
void xdi_notif_attach_kdeconnect(xdi_notif_store_t *store, DBusConnection *ctrl_conn);

/* Pure store init without D-Bus connection (for unit testing). */
void xdi_notif_init_store_only(xdi_notif_store_t *store, int max_items, const char *history_path);

/* Cleanup D-Bus connection. */
void xdi_notif_destroy(xdi_notif_store_t *store);

/* Return pollable fd for D-Bus monitor connection (-1 if unavailable). */
int xdi_notif_get_fd(xdi_notif_store_t *store);

/* Drain and parse any incoming org.freedesktop.Notifications.Notify calls. */
void xdi_notif_poll(xdi_notif_store_t *store);

/* Push a notification into the store (handles replaces_id updates & capping). */
void xdi_notif_push(xdi_notif_store_t *store, uint32_t replaces_id,
                    const char *app_name, const char *app_icon,
                    const char *summary, const char *body,
                    int urgency, time_t ts);

/* Push with parsed Freedesktop action pairs [key0, label0, ...] (count = number of actions). */
void xdi_notif_push_with_actions(xdi_notif_store_t *store, uint32_t replaces_id,
                                 const char *app_name, const char *app_icon,
                                 const char *summary, const char *body,
                                 int urgency, time_t ts, bool is_kde_hint,
                                 const xdi_notif_action_t *actions, int action_count);

/* Merge/upsert a KDE Connect D-Bus notification object (correlates with recent Notify to avoid duplicates). */
void xdi_notif_merge_kdeconnect(xdi_notif_store_t *store,
                                const char *dev_id, const char *public_id,
                                const char *internal_id, const char *app_name,
                                const char *title, const char *text,
                                const char *reply_id, bool dismissable,
                                bool silent, time_t ts);

/* Mark KDE Connect notification actions removed when dismissed on phone. */
void xdi_notif_kde_removed(xdi_notif_store_t *store, const char *public_id);

/* Execute an interactive notification action (e.g. KDE Connect Reply / Dismiss / Android action).
 * Returns true if action was dispatched. */
bool xdi_notif_invoke_action(xdi_notif_store_t *store, int item_index, int action_index);
bool xdi_notif_invoke_latest_action(xdi_notif_store_t *store, int action_index);

/* Remove a single notification by index (0 = newest); also dismisses on KDE Connect if dismissable. */
bool xdi_notif_remove_at(xdi_notif_store_t *store, int index);

/* Mark all notifications as read. */
void xdi_notif_mark_all_read(xdi_notif_store_t *store);

/* Clear all notification history. */
void xdi_notif_clear(xdi_notif_store_t *store);

/* Save/load persistent notification history (TSV escaped). */
bool xdi_notif_save(const xdi_notif_store_t *store);
bool xdi_notif_load(xdi_notif_store_t *store);

/* Format relative timestamp ("now", "3m", "2h", "HH:MM"). */
void xdi_notif_format_time(time_t ts, time_t now, char *buf, size_t len);

#endif /* XDI_NOTIFICATIONS_H */
