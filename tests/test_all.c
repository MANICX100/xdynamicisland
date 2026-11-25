#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_viz.h"
#include "config.h"
#include "media.h"
#include "notifications.h"
#include "obstacles.h"
#include "spring.h"

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s (%d) != %s (%d)\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        g_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_NEAR(a, b, eps) do { \
    double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (eps)) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s (%.6f) != %s (%.6f) +/- %.6f\n", __FILE__, __LINE__, #a, _a, #b, _b, (eps)); \
        g_failed++; \
        return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    int before = g_failed; \
    fn(); \
    if (g_failed == before) { \
        printf("  [PASS] %s\n", #fn); \
        g_passed++; \
    } \
} while (0)

/* ---------- Spring Physics Tests ---------- */

static void test_spring_convergence_and_overshoot(void) {
    xdi_spring_t s;
    xdi_spring_init(&s, 0.0, 0.40, 0.78);
    xdi_spring_set_target(&s, 200.0);

    double max_val = 0.0;
    bool moving = true;
    for (int i = 0; i < 200; i++) {
        moving = xdi_spring_step(&s, 0.016);
        if (s.value > max_val) max_val = s.value;
    }
    ASSERT_TRUE(!moving);
    ASSERT_NEAR(s.value, 200.0, 0.01);
    /* Underdamped zeta=0.78 produces ~2% natural iOS overshoot */
    ASSERT_TRUE(max_val > 201.0 && max_val < 215.0);
}

static void test_spring_snap_and_clamp(void) {
    xdi_spring_t s;
    xdi_spring_init(&s, 10.0, 0.40, 0.80);
    xdi_spring_set_target(&s, 100.0);
    /* Huge dt hiccup (2.0s) must stay numerically stable */
    xdi_spring_step(&s, 2.0);
    ASSERT_TRUE(isfinite(s.value) && s.value > 10.0 && s.value < 200.0);

    xdi_spring_snap(&s, 42.0);
    ASSERT_NEAR(s.value, 42.0, 1e-6);
    ASSERT_TRUE(xdi_spring_settled(&s));
}

/* ---------- Configuration Tests ---------- */

static void test_config_roundtrip(void) {
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/xdi_test_cfg_%d.ini", (int)getpid());

    xdi_config_t a;
    xdi_config_defaults(&a);
    ASSERT_TRUE(a.always_on_top);
    ASSERT_TRUE(a.avoid_obstacles);
    ASSERT_EQ_INT(a.eq_style, XDI_EQ_WAVE);

    a.always_on_top = false;
    a.avoid_obstacles = true;
    a.position = XDI_POS_BOTTOM_RIGHT;
    a.eq_style = XDI_EQ_RIBBON;
    a.art_shape = XDI_ART_ROUNDED;
    a.margin = 18;
    a.obstacle_gap = 12;
    a.accent_rgb = 0x00D4FF;
    a.opacity = 0.88;

    ASSERT_TRUE(xdi_config_save(&a, tmp_path));

    xdi_config_t b;
    xdi_config_defaults(&b);
    ASSERT_TRUE(xdi_config_load(&b, tmp_path));
    unlink(tmp_path);

    ASSERT_TRUE(b.always_on_top == false);
    ASSERT_TRUE(b.avoid_obstacles == true);
    ASSERT_EQ_INT(b.position, XDI_POS_BOTTOM_RIGHT);
    ASSERT_EQ_INT(b.eq_style, XDI_EQ_RIBBON);
    ASSERT_EQ_INT(b.art_shape, XDI_ART_ROUNDED);
    ASSERT_EQ_INT(b.margin, 18);
    ASSERT_EQ_INT(b.obstacle_gap, 12);
    ASSERT_EQ_INT((int)b.accent_rgb, 0x00D4FF);
    ASSERT_NEAR(b.opacity, 0.88, 0.01);
}

static void test_config_hex_and_gradient(void) {
    uint32_t rgb = 0;
    ASSERT_TRUE(xdi_parse_hex_rgb("#FF6A00", &rgb));
    ASSERT_EQ_INT((int)rgb, 0xFF6A00);
    ASSERT_TRUE(xdi_parse_hex_rgb("1db954", &rgb));
    ASSERT_EQ_INT((int)rgb, 0x1DB954);
    ASSERT_TRUE(!xdi_parse_hex_rgb("#GGG000", &rgb));
    ASSERT_TRUE(!xdi_parse_hex_rgb("#123", &rgb));

    double top[3], bot[3];
    xdi_accent_gradient(0xFF6A00, top, bot);
    /* Highlight top color must be lighter than base bottom color */
    double lum_top = 0.299 * top[0] + 0.587 * top[1] + 0.114 * top[2];
    double lum_bot = 0.299 * bot[0] + 0.587 * bot[1] + 0.114 * bot[2];
    ASSERT_TRUE(lum_top > lum_bot);
}

/* ---------- Notification History Tests ---------- */

static void test_notifications_store_and_persistence(void) {
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/xdi_test_notifs_%d.tsv", (int)getpid());

    xdi_notif_store_t st;
    xdi_notif_init_store_only(&st, 5, tmp_path);

    xdi_notif_push(&st, 10, "Discord", "", "Alice", "Hello\tWorld\nLine2", 1, 1700000000);
    xdi_notif_push(&st, 11, "KDE Connect", "", "SMS", "Check out xdynamicisland", 2, 1700000060);
    ASSERT_EQ_INT(st.count, 2);
    ASSERT_EQ_INT(st.unread_count, 2);
    ASSERT_TRUE(strcmp(st.items[0].app_name, "KDE Connect") == 0);
    /* Tabs/newlines sanitized for clean TSV persistence */
    ASSERT_TRUE(strcmp(st.items[1].body, "Hello World Line2") == 0);

    /* replaces_id=10 should replace old Discord item and move it to top */
    xdi_notif_push(&st, 10, "Discord", "", "Alice", "Edited message", 1, 1700000120);
    ASSERT_EQ_INT(st.count, 2);
    ASSERT_TRUE(strcmp(st.items[0].body, "Edited message") == 0);

    /* Capacity capping at 5 */
    for (int i = 0; i < 8; i++) {
        xdi_notif_push(&st, 0, "App", "", "Title", "Body", 1, 1700000200 + i);
    }
    ASSERT_EQ_INT(st.count, 5);

    /* Mark read & reload from disk */
    xdi_notif_mark_all_read(&st);
    ASSERT_EQ_INT(st.unread_count, 0);
    ASSERT_TRUE(xdi_notif_save(&st));

    xdi_notif_store_t st2;
    xdi_notif_init_store_only(&st2, 5, tmp_path);
    ASSERT_TRUE(xdi_notif_load(&st2));
    unlink(tmp_path);

    ASSERT_EQ_INT(st2.count, 5);
    ASSERT_EQ_INT(st2.unread_count, 0);
    ASSERT_TRUE(xdi_notif_remove_at(&st2, 0));
    ASSERT_EQ_INT(st2.count, 4);

    char tbuf[32];
    xdi_notif_format_time(1000, 1020, tbuf, sizeof(tbuf));
    ASSERT_TRUE(strcmp(tbuf, "now") == 0);
    xdi_notif_format_time(1000, 1300, tbuf, sizeof(tbuf));
    ASSERT_TRUE(strcmp(tbuf, "5m") == 0);
    xdi_notif_format_time(1000, 8200, tbuf, sizeof(tbuf));
    ASSERT_TRUE(strcmp(tbuf, "2h") == 0);
}

/* ---------- Obstacle / dmenu Detection Tests ---------- */

static void test_dmenu_top_and_bottom_repositioning(void) {
    xdi_rect_t mon = {0, 0, 1920, 1080};
    int isl_x = 820, isl_w = 280;

    /* Standard single-line dmenu at top (override_redirect, full width, 24px tall) */
    xdi_window_info_t dmenu_top = {
        .rect = {0, 0, 1920, 24},
        .override_redirect = true,
        .is_dock = false,
        .is_launcher = true,
        .win = 1001
    };
    xdi_obstacle_result_t r1 = xdi_solve_obstacles(&dmenu_top, 1, mon, isl_x, isl_w, true, true);
    ASSERT_EQ_INT(r1.top_clearance, 24);
    ASSERT_TRUE(r1.top_blocker == 1001);

    /* Vertical dmenu -l 10 (220px tall) */
    xdi_window_info_t dmenu_list = {
        .rect = {0, 0, 1920, 220},
        .override_redirect = true,
        .is_launcher = true,
        .win = 1002
    };
    xdi_obstacle_result_t r2 = xdi_solve_obstacles(&dmenu_list, 1, mon, isl_x, isl_w, true, true);
    ASSERT_EQ_INT(r2.top_clearance, 220);

    /* Stock unpatched dmenu with NO WM_CLASS (only override_redirect + wide top bar) */
    xdi_window_info_t anon_dmenu = {
        .rect = {0, 0, 1920, 26},
        .override_redirect = true,
        .is_launcher = false,
        .win = 1003
    };
    xdi_obstacle_result_t r3 = xdi_solve_obstacles(&anon_dmenu, 1, mon, isl_x, isl_w, true, true);
    ASSERT_EQ_INT(r3.top_clearance, 26);

    /* Tiny tooltip popup near top must NOT push the island */
    xdi_window_info_t tooltip = {
        .rect = {850, 10, 120, 22},
        .override_redirect = true,
        .is_launcher = false,
        .win = 1004
    };
    xdi_obstacle_result_t r4 = xdi_solve_obstacles(&tooltip, 1, mon, isl_x, isl_w, true, true);
    ASSERT_EQ_INT(r4.top_clearance, 0);

    /* Bottom dmenu -b */
    xdi_window_info_t dmenu_bot = {
        .rect = {0, 1054, 1920, 26},
        .override_redirect = true,
        .is_launcher = true,
        .win = 1005
    };
    xdi_obstacle_result_t r5 = xdi_solve_obstacles(&dmenu_bot, 1, mon, isl_x, isl_w, true, true);
    ASSERT_EQ_INT(r5.bottom_clearance, 26);
    ASSERT_TRUE(r5.bottom_blocker == 1005);

    /* Disabled avoid_obstacles ignores dmenu */
    xdi_obstacle_result_t r6 = xdi_solve_obstacles(&dmenu_top, 1, mon, isl_x, isl_w, false, false);
    ASSERT_EQ_INT(r6.top_clearance, 0);

    /* Launcher name helper */
    ASSERT_TRUE(xdi_is_launcher_name("dmenu"));
    ASSERT_TRUE(xdi_is_launcher_name("Rofi"));
    ASSERT_TRUE(!xdi_is_launcher_name("firefox"));
}

/* ---------- Media & wpctl Parsing Tests ---------- */

static void test_wpctl_volume_parser(void) {
    double vol = 0.0;
    bool muted = false;

    ASSERT_TRUE(xdi_parse_wpctl_volume("Volume: 0.65\n", &vol, &muted));
    ASSERT_NEAR(vol, 0.65, 1e-4);
    ASSERT_TRUE(!muted);

    ASSERT_TRUE(xdi_parse_wpctl_volume("Volume: 0.42 [MUTED]\n", &vol, &muted));
    ASSERT_NEAR(vol, 0.42, 1e-4);
    ASSERT_TRUE(muted);

    ASSERT_TRUE(xdi_parse_wpctl_volume("Volume: 1.25", &vol, &muted));
    ASSERT_NEAR(vol, 1.25, 1e-4);

    ASSERT_TRUE(!xdi_parse_wpctl_volume("error: sink not found", &vol, &muted));
}

static void test_player_prioritization(void) {
    xdi_player_t ps[3];
    memset(ps, 0, sizeof(ps));
    snprintf(ps[0].title, sizeof(ps[0].title), "Paused Browser Tab");
    ps[0].status = XDI_PLAYBACK_PAUSED;

    snprintf(ps[1].title, sizeof(ps[1].title), "Phone Song");
    ps[1].status = XDI_PLAYBACK_PLAYING;
    ps[1].is_kdeconnect = true;

    snprintf(ps[2].title, sizeof(ps[2].title), "Stopped VLC");
    ps[2].status = XDI_PLAYBACK_STOPPED;

    /* Automatically prefers Playing player (KDE Connect) */
    ASSERT_EQ_INT(xdi_select_best_player(ps, 3, 0, false), 1);

    /* Manual selection sticks when player has title */
    ASSERT_EQ_INT(xdi_select_best_player(ps, 3, 0, true), 0);
}

/* ---------- Audio FFT & Waveform Tests ---------- */

static void test_audio_fft_and_waveform(void) {
    float sine[256];
    for (int i = 0; i < 256; i++) {
        /* 1.5 kHz sine at 22050 Hz */
        sine[i] = 0.5f * sinf(2.0f * 3.14159265f * 1500.0f * (float)i / 22050.0f);
    }
    double bands[XDI_VIZ_BANDS];
    double rms = 0.0;
    xdi_audio_viz_compute_fft_bands(sine, 256, bands, &rms);
    ASSERT_TRUE(rms > 0.3);

    double max_band = 0.0;
    for (int i = 0; i < XDI_VIZ_BANDS; i++) {
        ASSERT_TRUE(bands[i] >= 0.0 && bands[i] <= 1.0);
        if (bands[i] > max_band) max_band = bands[i];
    }
    ASSERT_TRUE(max_band > 0.4);

    /* Silence produces near-zero RMS and bands */
    float silence[256];
    memset(silence, 0, sizeof(silence));
    xdi_audio_viz_compute_fft_bands(silence, 256, bands, &rms);
    ASSERT_NEAR(rms, 0.0, 1e-5);

    /* Visualizer stepping in remote harmonic mode settles cleanly when paused */
    xdi_audio_viz_t viz;
    xdi_audio_viz_init(&viz, false);
    for (int i = 0; i < 30; i++) {
        xdi_audio_viz_step(&viz, true, true, 0.016, i * 16);
    }
    ASSERT_TRUE(xdi_audio_viz_sample(&viz, 0.5) > 0.05);

    for (int i = 0; i < 120; i++) {
        xdi_audio_viz_step(&viz, false, true, 0.016, 1000 + i * 16);
    }
    ASSERT_NEAR(xdi_audio_viz_sample(&viz, 0.5), 0.0, 0.01);
    xdi_audio_viz_destroy(&viz);
}

int main(void) {
    printf("Running xdynamicisland unit tests...\n");
    RUN_TEST(test_spring_convergence_and_overshoot);
    RUN_TEST(test_spring_snap_and_clamp);
    RUN_TEST(test_config_roundtrip);
    RUN_TEST(test_config_hex_and_gradient);
    RUN_TEST(test_notifications_store_and_persistence);
    RUN_TEST(test_dmenu_top_and_bottom_repositioning);
    RUN_TEST(test_wpctl_volume_parser);
    RUN_TEST(test_player_prioritization);
    RUN_TEST(test_audio_fft_and_waveform);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
