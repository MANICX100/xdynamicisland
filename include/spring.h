#ifndef XDI_SPRING_H
#define XDI_SPRING_H

#include <stdbool.h>

/*
 * Critically/lightly underdamped harmonic spring matching iOS Dynamic Island
 * feel (response ~0.42s, damping ratio ~0.78 -> crisp ~2% natural overshoot).
 */
typedef struct {
    double value;
    double target;
    double velocity;
    double stiffness; /* omega^2 */
    double damping;   /* 2 * zeta * omega */
} xdi_spring_t;

/* Initialize spring at rest at `initial` with given response (s) and zeta. */
void xdi_spring_init(xdi_spring_t *s, double initial, double response_sec, double zeta);

/* Set a new target value without resetting velocity (smooth retargeting). */
void xdi_spring_set_target(xdi_spring_t *s, double target);

/* Snap immediately to target with zero velocity. */
void xdi_spring_snap(xdi_spring_t *s, double value);

/* Advance spring by `dt` seconds (clamped for stability). Returns true if still moving. */
bool xdi_spring_step(xdi_spring_t *s, double dt);

/* Returns true if spring is effectively settled at target. */
bool xdi_spring_settled(const xdi_spring_t *s);

/* Linear interpolation helper. */
static inline double xdi_lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

#endif /* XDI_SPRING_H */
