#include "instance.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

bool xdi_instance_parse_pid(const char *str, pid_t *out_pid) {
    if (!str || !out_pid) return false;
    while (*str && isspace((unsigned char)*str)) str++;
    if (!isdigit((unsigned char)*str)) return false;
    char *end = NULL;
    long v = strtol(str, &end, 10);
    if (end == str || v <= 1 || v > 4194304) return false;
    *out_pid = (pid_t)v;
    return true;
}

bool xdi_instance_comm_matches(const char *comm) {
    if (!comm) return false;
    /* /proc/<pid>/comm is truncated to 15 chars ("xdynamicisland" is 14 chars) */
    return strncmp(comm, "xdynamicisland", 14) == 0;
}

static bool pid_is_xdynamicisland(pid_t pid, uid_t my_uid) {
    if (pid <= 1 || pid == getpid()) return false;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_uid != my_uid) return false;

    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char comm[64] = {0};
    bool ok = (fgets(comm, sizeof(comm), fp) != NULL) && xdi_instance_comm_matches(comm);
    fclose(fp);
    return ok;
}

static void terminate_pid_cleanly(pid_t pid) {
    if (kill(pid, SIGTERM) != 0) return;

    /* Wait up to 600ms for graceful shutdown (saves history + destroys X11 window) */
    for (int i = 0; i < 60; i++) {
        if (kill(pid, 0) != 0 && errno == ESRCH) return;
        sleep_ms(10);
    }

    /* Escalate to SIGKILL if hung */
    kill(pid, SIGKILL);
    for (int i = 0; i < 20; i++) {
        if (kill(pid, 0) != 0 && errno == ESRCH) return;
        sleep_ms(10);
    }
}

static void kill_other_instances_via_proc(uid_t my_uid) {
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *ent;
    pid_t me = getpid();
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        pid_t pid = 0;
        if (!xdi_instance_parse_pid(ent->d_name, &pid) || pid == me) continue;
        if (pid_is_xdynamicisland(pid, my_uid)) {
            terminate_pid_cleanly(pid);
        }
    }
    closedir(dir);
}

static void resolve_pid_path(char *buf, size_t len, const char *custom_path) {
    if (custom_path && custom_path[0]) {
        snprintf(buf, len, "%s", custom_path);
        return;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] && access(runtime, W_OK) == 0) {
        snprintf(buf, len, "%s/xdynamicisland.pid", runtime);
    } else {
        snprintf(buf, len, "/tmp/xdynamicisland-%lu.pid", (unsigned long)getuid());
    }
}

bool xdi_instance_acquire(xdi_instance_t *inst, const char *custom_pid_path) {
    if (!inst) return false;
    memset(inst, 0, sizeof(*inst));
    inst->lock_fd = -1;

    uid_t my_uid = getuid();
    resolve_pid_path(inst->pid_path, sizeof(inst->pid_path), custom_pid_path);

    /* 1. Fast path: check PID recorded in lock file */
    FILE *rfp = fopen(inst->pid_path, "r");
    if (rfp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), rfp)) {
            pid_t old_pid = 0;
            if (xdi_instance_parse_pid(buf, &old_pid) &&
                pid_is_xdynamicisland(old_pid, my_uid)) {
                terminate_pid_cleanly(old_pid);
            }
        }
        fclose(rfp);
    }

    /* 2. Fallback scan: also replace instances started without a PID file (e.g. v1.0.0) */
    if (!custom_pid_path) {
        kill_other_instances_via_proc(my_uid);
    }

    /* 3. Acquire exclusive advisory lock and record our PID */
    int fd = open(inst->pid_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return false;

    for (int tries = 0; tries < 20; tries++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) break;
        sleep_ms(15);
    }

    if (ftruncate(fd, 0) == 0) {
        lseek(fd, 0, SEEK_SET);
        char pid_buf[32];
        int n = snprintf(pid_buf, sizeof(pid_buf), "%ld\n", (long)getpid());
        if (n > 0) {
            ssize_t wr = write(fd, pid_buf, (size_t)n);
            (void)wr;
        }
    }

    inst->lock_fd = fd;
    return true;
}

void xdi_instance_release(xdi_instance_t *inst) {
    if (!inst) return;
    if (inst->lock_fd >= 0) {
        /* Only unlink if PID file still belongs to us */
        FILE *fp = fopen(inst->pid_path, "r");
        if (fp) {
            char buf[64] = {0};
            pid_t recorded = 0;
            if (fgets(buf, sizeof(buf), fp) &&
                xdi_instance_parse_pid(buf, &recorded) &&
                recorded == getpid()) {
                unlink(inst->pid_path);
            }
            fclose(fp);
        }
        flock(inst->lock_fd, LOCK_UN);
        close(inst->lock_fd);
        inst->lock_fd = -1;
    }
}
