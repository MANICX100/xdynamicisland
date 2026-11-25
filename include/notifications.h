#ifndef XDI_NOTIFICATIONS_H
#define XDI_NOTIFICATIONS_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XDI_NOTIF_MAX_CAP 128

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
} xdi_notification_t;

typedef struct {
    DBusConnection *monitor_conn; /* Dedicated D-Bus BecomeMonitor connection */
    xdi_notification_t items[XDI_NOTIF_MAX_CAP];
    int count;             /* Stored items (newest at index 0) */
    int max_items;
    int unread_count;
    uint64_t next_seq;
    bool has_new_toast;
    xdi_notification_t latest;
    bool persist;
    char history_path[512];
} xdi_notif_store_t;

/* Initialize notification history store & attach non-invasive D-Bus monitor for notification-daemon. */
bool xdi_notif_init(xdi_notif_store_t *store, int max_items, bool persist);

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

/* Remove a single notification by index (0 = newest). */
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
