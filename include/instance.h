#ifndef XDI_INSTANCE_H
#define XDI_INSTANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int lock_fd;
    char pid_path[256];
} xdi_instance_t;

/*
 * Ensure only one instance of xdynamicisland runs for the current user.
 * If an existing instance is detected (via PID file or /proc scan), sends
 * SIGTERM (falling back to SIGKILL after grace period) and replaces it.
 * Returns true on success.
 */
bool xdi_instance_acquire(xdi_instance_t *inst, const char *custom_pid_path);

/* Release lock file on clean exit. */
void xdi_instance_release(xdi_instance_t *inst);

/* Pure helpers exposed for unit testing. */
bool xdi_instance_parse_pid(const char *str, pid_t *out_pid);
bool xdi_instance_comm_matches(const char *comm);

#endif /* XDI_INSTANCE_H */
