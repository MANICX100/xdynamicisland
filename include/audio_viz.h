#ifndef XDI_AUDIO_VIZ_H
#define XDI_AUDIO_VIZ_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define XDI_VIZ_BANDS 16

typedef struct {
    /* Smoothed normalized band energies (0.0 .. 1.0) for rendering */
    double bands[XDI_VIZ_BANDS];
    /* Raw target energies updated from live FFT or procedural harmonic generator */
    double targets[XDI_VIZ_BANDS];
    double rms;
    double phase;
    double harmonic_timer;
    bool using_live_audio;
    bool live_signal_active;

    /* Optional background capture thread via dlopen(libpulse-simple.so.0) */
    bool capture_enabled;
    bool thread_running;
    bool stop_requested;
    pthread_t thread;
    pthread_mutex_t lock;
    double live_bands[XDI_VIZ_BANDS];
    double live_rms;
    int64_t last_live_signal_ms;
    uint32_t rng_state;
} xdi_audio_viz_t;

/* Initialize visualizer state (does not open audio until playing). */
void xdi_audio_viz_init(xdi_audio_viz_t *viz, bool enable_live_capture);

/* Stop capture thread and release resources. */
void xdi_audio_viz_destroy(xdi_audio_viz_t *viz);

/* Enable or disable live PipeWire/Pulse monitor capture dynamically. */
void xdi_audio_viz_set_live_enabled(xdi_audio_viz_t *viz, bool enabled);

/* Update visualizer each frame. Returns true if visually changing (needs repaint). */
bool xdi_audio_viz_step(xdi_audio_viz_t *viz, bool is_playing, bool is_remote,
                        double dt, int64_t now_ms);

/* Sample smooth cosine-interpolated amplitude at x in [0, 1]. */
double xdi_audio_viz_sample(const xdi_audio_viz_t *viz, double x);

/* Pure FFT band extractor exposed for unit tests (expects N = 256 mono float samples). */
void xdi_audio_viz_compute_fft_bands(const float *samples, int n,
                                     double out_bands[XDI_VIZ_BANDS], double *out_rms);

#endif /* XDI_AUDIO_VIZ_H */
