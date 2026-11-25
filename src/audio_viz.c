#include "audio_viz.h"

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define XDI_FFT_N 256

/* Minimal PulseAudio simple ABI structs/enums for zero-dependency dlopen */
typedef enum {
    XDI_PA_STREAM_RECORD = 2
} xdi_pa_stream_direction_t;

typedef enum {
    XDI_PA_SAMPLE_FLOAT32LE = 5
} xdi_pa_sample_format_t;

typedef struct {
    xdi_pa_sample_format_t format;
    uint32_t rate;
    uint8_t channels;
} xdi_pa_sample_spec_t;

typedef struct {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
} xdi_pa_buffer_attr_t;

typedef void *(*pa_simple_new_fn)(const char *, const char *, int, const char *,
                                  const char *, const xdi_pa_sample_spec_t *,
                                  const void *, const xdi_pa_buffer_attr_t *, int *);
typedef int (*pa_simple_read_fn)(void *, void *, size_t, int *);
typedef void (*pa_simple_free_fn)(void *);

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Iterative Cooley-Tukey Radix-2 FFT (power-of-two n <= 256) */
static void fft_radix2(float *re, float *im, int n) {
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
        int m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * (float)M_PI / (float)len;
        float wlen_r = cosf(angle);
        float wlen_i = sinf(angle);
        int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int k = 0; k < half; k++) {
                int u = i + k;
                int v = u + half;
                float vr = re[v] * wr - im[v] * wi;
                float vi = re[v] * wi + im[v] * wr;
                re[v] = re[u] - vr;
                im[v] = im[u] - vi;
                re[u] += vr;
                im[u] += vi;
                float nwr = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = nwr;
            }
        }
    }
}

void xdi_audio_viz_compute_fft_bands(const float *samples, int n,
                                     double out_bands[XDI_VIZ_BANDS], double *out_rms) {
    if (!samples || n < 32 || !out_bands) return;
    int fft_n = (n >= XDI_FFT_N) ? XDI_FFT_N : 64;
    float re[XDI_FFT_N];
    float im[XDI_FFT_N];

    double sum_sq = 0.0;
    for (int i = 0; i < fft_n; i++) {
        float s = samples[i];
        sum_sq += (double)s * (double)s;
        /* Hann window to eliminate spectral leakage */
        float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(fft_n - 1)));
        re[i] = s * w;
        im[i] = 0.0f;
    }

    double rms = sqrt(sum_sq / (double)fft_n);
    if (out_rms) *out_rms = fmin(1.0, rms * 3.8);

    fft_radix2(re, im, fft_n);

    /* Logarithmically spaced bin ranges across [1 .. fft_n/2 - 1] */
    int max_bin = fft_n / 2;
    for (int b = 0; b < XDI_VIZ_BANDS; b++) {
        double f0 = pow((double)(max_bin - 1), (double)b / (double)XDI_VIZ_BANDS);
        double f1 = pow((double)(max_bin - 1), (double)(b + 1) / (double)XDI_VIZ_BANDS);
        int bin0 = (int)floor(f0);
        int bin1 = (int)ceil(f1);
        if (bin0 < 1) bin0 = 1;
        if (bin1 <= bin0) bin1 = bin0 + 1;
        if (bin1 > max_bin) bin1 = max_bin;

        double peak = 0.0;
        for (int k = bin0; k < bin1; k++) {
            double mag = sqrt((double)re[k] * (double)re[k] + (double)im[k] * (double)im[k]);
            mag /= ((double)fft_n * 0.25);
            if (mag > peak) peak = mag;
        }

        /* Slight perceptual treble boost + logarithmic dynamic range compression */
        double boost = 1.0 + 0.06 * (double)b;
        double level = log1p(peak * 9.0 * boost) / log1p(9.0);
        if (level < 0.0) level = 0.0;
        if (level > 1.0) level = 1.0;
        out_bands[b] = level;
    }
}

static void *capture_thread_fn(void *arg) {
    xdi_audio_viz_t *viz = (xdi_audio_viz_t *)arg;

    void *lib = dlopen("libpulse-simple.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) {
        pthread_mutex_lock(&viz->lock);
        viz->thread_running = false;
        pthread_mutex_unlock(&viz->lock);
        return NULL;
    }

    pa_simple_new_fn fn_new = NULL;
    pa_simple_read_fn fn_read = NULL;
    pa_simple_free_fn fn_free = NULL;
    *(void **)(&fn_new) = dlsym(lib, "pa_simple_new");
    *(void **)(&fn_read) = dlsym(lib, "pa_simple_read");
    *(void **)(&fn_free) = dlsym(lib, "pa_simple_free");

    if (!fn_new || !fn_read || !fn_free) {
        dlclose(lib);
        pthread_mutex_lock(&viz->lock);
        viz->thread_running = false;
        pthread_mutex_unlock(&viz->lock);
        return NULL;
    }

    xdi_pa_sample_spec_t ss = {
        .format = XDI_PA_SAMPLE_FLOAT32LE,
        .rate = 22050,
        .channels = 1
    };
    xdi_pa_buffer_attr_t attr = {
        .maxlength = (uint32_t)-1,
        .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
        .fragsize = XDI_FFT_N * sizeof(float)
    };

    int err = 0;
    void *pa = fn_new(NULL, "xdynamicisland", XDI_PA_STREAM_RECORD,
                      "@DEFAULT_MONITOR@", "waveform", &ss, NULL, &attr, &err);
    if (!pa) {
        dlclose(lib);
        pthread_mutex_lock(&viz->lock);
        viz->thread_running = false;
        pthread_mutex_unlock(&viz->lock);
        return NULL;
    }

    float buf[XDI_FFT_N];
    double bands[XDI_VIZ_BANDS];

    while (1) {
        pthread_mutex_lock(&viz->lock);
        bool stop = viz->stop_requested;
        pthread_mutex_unlock(&viz->lock);
        if (stop) break;

        if (fn_read(pa, buf, sizeof(buf), &err) < 0) {
            break;
        }

        double rms = 0.0;
        xdi_audio_viz_compute_fft_bands(buf, XDI_FFT_N, bands, &rms);
        int64_t now = monotonic_ms();

        pthread_mutex_lock(&viz->lock);
        memcpy(viz->live_bands, bands, sizeof(bands));
        viz->live_rms = rms;
        if (rms > 0.008) {
            viz->last_live_signal_ms = now;
        }
        pthread_mutex_unlock(&viz->lock);
    }

    fn_free(pa);
    dlclose(lib);

    pthread_mutex_lock(&viz->lock);
    viz->thread_running = false;
    pthread_mutex_unlock(&viz->lock);
    return NULL;
}

static void start_capture_if_needed(xdi_audio_viz_t *viz) {
    if (!viz->capture_enabled) return;
    pthread_mutex_lock(&viz->lock);
    bool running = viz->thread_running;
    if (!running) {
        viz->stop_requested = false;
        viz->thread_running = true;
        if (pthread_create(&viz->thread, NULL, capture_thread_fn, viz) != 0) {
            viz->thread_running = false;
        }
    }
    pthread_mutex_unlock(&viz->lock);
}

static void stop_capture_if_running(xdi_audio_viz_t *viz) {
    pthread_mutex_lock(&viz->lock);
    bool running = viz->thread_running;
    viz->stop_requested = true;
    pthread_mutex_unlock(&viz->lock);
    if (running) {
        pthread_join(viz->thread, NULL);
    }
}

void xdi_audio_viz_init(xdi_audio_viz_t *viz, bool enable_live_capture) {
    memset(viz, 0, sizeof(*viz));
    pthread_mutex_init(&viz->lock, NULL);
    viz->capture_enabled = enable_live_capture;
    viz->rng_state = 0x9E3779B9u;
}

void xdi_audio_viz_destroy(xdi_audio_viz_t *viz) {
    stop_capture_if_running(viz);
    pthread_mutex_destroy(&viz->lock);
}

void xdi_audio_viz_set_live_enabled(xdi_audio_viz_t *viz, bool enabled) {
    viz->capture_enabled = enabled;
    if (!enabled) {
        stop_capture_if_running(viz);
    }
}

bool xdi_audio_viz_step(xdi_audio_viz_t *viz, bool is_playing, bool is_remote,
                        double dt, int64_t now_ms) {
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;

    if (is_playing && !is_remote && viz->capture_enabled) {
        start_capture_if_needed(viz);
    } else if (!is_playing || is_remote) {
        /* Release audio monitor immediately when paused/remote to save CPU/battery */
        pthread_mutex_lock(&viz->lock);
        bool running = viz->thread_running;
        pthread_mutex_unlock(&viz->lock);
        if (running) {
            stop_capture_if_running(viz);
        }
    }

    bool live_ok = false;
    if (is_playing && !is_remote && viz->capture_enabled) {
        pthread_mutex_lock(&viz->lock);
        if (viz->thread_running && (now_ms - viz->last_live_signal_ms) < 700) {
            live_ok = true;
            for (int i = 0; i < XDI_VIZ_BANDS; i++) {
                viz->targets[i] = viz->live_bands[i];
            }
            viz->rms = viz->rms * 0.7 + viz->live_rms * 0.3;
        }
        pthread_mutex_unlock(&viz->lock);
    }
    viz->using_live_audio = live_ok;
    viz->live_signal_active = live_ok;

    if (is_playing) {
        viz->phase += dt * 5.6;
        if (!live_ok) {
            /* Rich organic multi-harmonic fallback (ideal for KDE Connect phone audio) */
            viz->harmonic_timer += dt;
            if (viz->harmonic_timer >= 0.16) {
                viz->harmonic_timer = 0.0;
                double pulse = 0.55 + 0.45 * sin(viz->phase * 0.65);
                for (int i = 0; i < XDI_VIZ_BANDS; i++) {
                    double r = (double)(xorshift32(&viz->rng_state) & 0xFFFFu) / 65535.0;
                    double env = sin(M_PI * ((double)i + 0.5) / (double)XDI_VIZ_BANDS);
                    double wave = 0.5 + 0.5 * sin(viz->phase * 1.1 + (double)i * 0.62);
                    double val = (0.18 + 0.62 * r * wave) * (0.45 + 0.55 * env) * (0.7 + 0.3 * pulse);
                    if (val > 1.0) val = 1.0;
                    viz->targets[i] = val;
                }
            }
            double sum = 0.0;
            for (int i = 0; i < XDI_VIZ_BANDS; i++) sum += viz->targets[i];
            viz->rms = viz->rms * 0.8 + (sum / (double)XDI_VIZ_BANDS) * 0.2;
        }
    } else {
        for (int i = 0; i < XDI_VIZ_BANDS; i++) {
            viz->targets[i] = 0.0;
        }
        viz->rms *= exp(-dt * 9.0);
        if (viz->rms < 0.002) viz->rms = 0.0;
    }

    /* Fast attack, smooth decay easing for silky iOS-style motion */
    double attack = 1.0 - exp(-dt * 18.0);
    double decay = 1.0 - exp(-dt * 9.5);
    bool moving = is_playing;

    for (int i = 0; i < XDI_VIZ_BANDS; i++) {
        double k = (viz->targets[i] > viz->bands[i]) ? attack : decay;
        double next = viz->bands[i] + (viz->targets[i] - viz->bands[i]) * k;
        if (fabs(next - viz->bands[i]) > 0.0015 || (!is_playing && next > 0.002)) {
            moving = true;
        }
        if (!is_playing && next < 0.002) next = 0.0;
        viz->bands[i] = next;
    }

    return moving;
}

double xdi_audio_viz_sample(const xdi_audio_viz_t *viz, double x) {
    if (x <= 0.0) return viz->bands[0];
    if (x >= 1.0) return viz->bands[XDI_VIZ_BANDS - 1];
    double t = x * (double)(XDI_VIZ_BANDS - 1);
    int i = (int)floor(t);
    if (i >= XDI_VIZ_BANDS - 1) return viz->bands[XDI_VIZ_BANDS - 1];
    double f = (1.0 - cos((t - (double)i) * M_PI)) * 0.5;
    return viz->bands[i] * (1.0 - f) + viz->bands[i + 1] * f;
}
