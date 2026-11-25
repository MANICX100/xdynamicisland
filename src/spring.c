#include "spring.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void xdi_spring_init(xdi_spring_t *s, double initial, double response_sec, double zeta) {
    if (response_sec < 0.05) response_sec = 0.05;
    if (zeta < 0.2) zeta = 0.2;
    double omega = (2.0 * M_PI) / response_sec;
    s->value = initial;
    s->target = initial;
    s->velocity = 0.0;
    s->stiffness = omega * omega;
    s->damping = 2.0 * zeta * omega;
}

void xdi_spring_set_target(xdi_spring_t *s, double target) {
    s->target = target;
}

void xdi_spring_snap(xdi_spring_t *s, double value) {
    s->value = value;
    s->target = value;
    s->velocity = 0.0;
}

bool xdi_spring_settled(const xdi_spring_t *s) {
    return fabs(s->value - s->target) < 0.08 && fabs(s->velocity) < 0.15;
}

bool xdi_spring_step(xdi_spring_t *s, double dt) {
    if (dt <= 0.0) return !xdi_spring_settled(s);
    if (dt > 0.05) dt = 0.05; /* Clamp large frame hiccups for numerical stability */

    /* Sub-step semi-implicit Euler (4 substeps) for unconditionally stable smooth curves */
    const int substeps = 4;
    double h = dt / (double)substeps;
    for (int i = 0; i < substeps; i++) {
        double disp = s->value - s->target;
        double accel = -s->stiffness * disp - s->damping * s->velocity;
        s->velocity += accel * h;
        s->value += s->velocity * h;
    }

    if (xdi_spring_settled(s)) {
        s->value = s->target;
        s->velocity = 0.0;
        return false;
    }
    return true;
}
