<div align="center">

# xdynamicisland

**An ultra-fast, desktop-environment-independent Dynamic Island for Linux X11**

*Inspired by Apple's Dynamic Island and [linux_dynamic_island](https://github.com/basskibo/linux_dynamic_island) — built from scratch in pure C11 with zero DE, GTK, or Qt dependencies.*

[![CI Build & Tests](https://github.com/MANICX100/xdynamicisland/actions/workflows/ci.yml/badge.svg)](https://github.com/MANICX100/xdynamicisland/actions/workflows/ci.yml)
[![Release Pipeline](https://github.com/MANICX100/xdynamicisland/actions/workflows/release.yml/badge.svg)](https://github.com/MANICX100/xdynamicisland/actions/workflows/release.yml)
[![Latest Release](https://img.shields.io/github/v/release/MANICX100/xdynamicisland?include_prereleases&style=flat-square&color=FF6A00)](https://github.com/MANICX100/xdynamicisland/releases/latest)
[![GitHub Downloads](https://img.shields.io/github/downloads/MANICX100/xdynamicisland/total?style=flat-square&color=1DB954)](https://github.com/MANICX100/xdynamicisland/releases)
[![License: MIT](https://img.shields.io/github/license/MANICX100/xdynamicisland?style=flat-square&color=00D4FF)](LICENSE)

[![Language: C11](https://img.shields.io/badge/Language-C11-blue?style=flat-square&logo=c)](Makefile)
[![Platform: Linux X11](https://img.shields.io/badge/Platform-Linux%20X11-black?style=flat-square&logo=linux)](README.md)
[![Unit Tests](https://img.shields.io/badge/Unit%20Tests-9%2F9%20Passing-brightgreen?style=flat-square)](tests/test_all.c)
[![Sanitizers](https://img.shields.io/badge/ASan%20%2B%20UBSan-Verified-brightgreen?style=flat-square)](.github/workflows/ci.yml)
[![Static Analysis](https://img.shields.io/badge/Static%20Analysis-cppcheck-brightgreen?style=flat-square)](.github/workflows/ci.yml)
[![Binary Size](https://img.shields.io/badge/Stripped%20Binary-~176%20KB-blueviolet?style=flat-square)](#performance--footprint)
[![Private Memory](https://img.shields.io/badge/Private%20RAM-~3.6%20MB-informational?style=flat-square)](#performance--footprint)
[![DE Independent](https://img.shields.io/badge/DE%20Dependencies-None-success?style=flat-square)](#features)

</div>

---

## One-Line Install

Download the prebuilt portable `x86_64` binary directly to `/usr/local/bin/xdynamicisland` using **`doas`** *(default — see [how to install OpenDoas](https://github.com/Duncaen/OpenDoas#installation) or the [Arch Wiki `doas` guide](https://wiki.archlinux.org/title/Doas))*:

```bash
doas curl -fsSL https://github.com/MANICX100/xdynamicisland/releases/latest/download/xdynamicisland-linux-x86_64 -o /usr/local/bin/xdynamicisland && doas chmod +x /usr/local/bin/xdynamicisland
```

**Alternative using `sudo`:**

```bash
sudo curl -fsSL https://github.com/MANICX100/xdynamicisland/releases/latest/download/xdynamicisland-linux-x86_64 -o /usr/local/bin/xdynamicisland && sudo chmod +x /usr/local/bin/xdynamicisland
```

Or use the checksum-verifying installer script *(automatically uses `doas` if available, falling back to `sudo`)*:

```bash
curl -fsSL https://raw.githubusercontent.com/MANICX100/xdynamicisland/main/install.sh | sh
```

Then launch it anywhere (or add `xdynamicisland &` to your `~/.xinitrc`, IceWM startup, i3/bspwm/dwm/openbox autostart):

```bash
xdynamicisland &
```

---

## Features

- **100% Desktop-Environment Independent** — Works on **IceWM, dwm, i3, bspwm, Openbox, AwesomeWM, Fluxbox, XFCE, MATE, KDE/X11, GNOME/X11**, or bare X.Org.
- **Compositor & Non-Compositor Support** — Automatically uses 32-bit ARGB transparency + soft multi-layered drop shadows when a compositor (`picom`, `xcompmgr`, etc.) is running, and seamlessly switches to crisp `XShape` rounded-pill masking when **no compositor** is active (zero ugly black square corners!).
- **Fluid 60fps iOS Spring Physics** — Critically tuned damped harmonic springs ($\zeta = 0.78$, response $0.40\,\text{s}$) give authentic Apple Dynamic Island expansion, collapse, toast morphing, and position gliding.
- **Sexy Live Playback Waveforms (6 Styles)** — Real-time 256-point Radix-2 FFT audio spectrum captured live from PipeWire/PulseAudio (`@DEFAULT_MONITOR@` via optional `dlopen`), with automatic organic multi-harmonic animation for remote playback (such as phone audio via **KDE Connect**):
  - `wave` — Dual-layered glowing harmonic waveform with soft bloom & album-art-blended gradient *(default)*
  - `bars` — Classic iOS Dynamic Island dancing rounded pill equalizer
  - `ribbon` — Mirrored liquid audio ribbon
  - `spectrum` — High-resolution multi-band FFT analyzer
  - `segments` — Retro illuminated LED meter
  - `solid` — Smooth cosine-interpolated filled wave
- **Complete Media & Audio Control (`MPRIS2` + `KDE Connect` + `wpctl`)**:
  - Controls **Spotify, Chromium, Firefox, Vivaldi, mpv, VLC**, and **paired phones via KDE Connect** (automatically shows your paired phone's name and cover art!).
  - Click the source badge to switch between multiple active players (e.g., PC browser ↔ KDE Connect phone).
  - Interactive **scrub/progress bar**, Play/Pause, Next, Previous, and async cached album art (PNG & JPEG) with optional vinyl spin and dominant-color ambient glow.
  - Native **WirePlumber / PipeWire (`wpctl`)** volume & mute control — scroll anywhere on the island or click the inline volume slider/mute button, with automatic iOS-style Volume OSD toasts!
- **Notification History via `notification-daemon`**:
  - Non-invasively listens to `org.freedesktop.Notifications.Notify` over D-Bus (`BecomeMonitor`, compatible with both `dbus-broker` and `dbus-daemon`) — works right alongside `notification-daemon`, `dunst`, or `xfce4-notifyd` without replacing or conflicting with your existing daemon!
  - Live toast pop-ups on new notifications + full scrollable **Notification History drawer** with relative timestamps, unread badge, individual dismiss, Clear All, and persistent history across restarts.
- **Smart `Always On Top` + `dmenu` / Overlay Awareness**:
  - Configurable **Always On Top** setting so the island floats above all regular windows (or yields to normal windows when disabled).
  - Event-driven X11 `SubstructureNotify` obstacle detector watches for **`dmenu`, `rofi`, `bemenu`, `gmrun`, override-redirect launchers, and EWMH panels** — the instant `dmenu` is summoned (single-line, vertical `-l`, or bottom `-b`), `xdynamicisland` smoothly spring-glides right below (or above) `dmenu` so it is **never obscured** and **never obscures `dmenu`**!
- **Built-in Native Settings Window** — Zero GTK/Qt bloat! Click the gear icon (`⚙`), right-click the island, or run `xdynamicisland --settings` for instant live-applied settings saved to `~/.config/xdynamicisland/config.ini`.

---

## Performance & Footprint

Designed strictly in priority order: **1. Performance → 2. Memory Footprint → 3. Binary Size**, while preserving silky visual polish.

| Metric | Measured Result | How It's Achieved |
| :--- | :--- | :--- |
| **Idle CPU Usage** | **`~0.0%`** | Pure event-driven `poll()` multiplexing X11 + D-Bus fds; audio thread stops automatically when paused/remote |
| **Active Animation** | **Locked 60 FPS** | Zero-GC C11 (`-O3 -flto`), double-buffered Cairo rendering, sub-stepped spring integrator |
| **Private RAM (`Private_Dirty`)** | **`~3.6 MB`** | No GTK, no Qt, no Electron, no Python/JS runtime; album art downscaled to ≤160×160 |
| **Proportional Set Size (`PSS`)** | **`~4.5 MB`** | Direct `libX11` + `libcairo` + `libpangocairo` + `libdbus-1` |
| **Stripped Binary Size** | **`~176 KB`** | `-flto -ffunction-sections -fdata-sections -Wl,--gc-sections`, embedded `STBI_ONLY_JPEG/PNG`, optional `dlopen` for PulseAudio |

---

## Controls & Interactions

| Interaction | Action |
| :--- | :--- |
| **Hover over Island** | Smoothly expands into the Media Control Card (or Quick Actions when idle) |
| **Mouse Wheel Up / Down** | Adjusts system volume via `wpctl` (or scrolls the Notification History drawer when open) |
| **Left-Click Controls** | Play/Pause, Previous, Next, Seek Bar, Volume Bar, Mute Toggle |
| **Click Source Label (`1/2`)** | Cycles between active MPRIS2 players (e.g., Chromium ↔ KDE Connect) |
| **Click Bell Icon (`🔔`)** | Opens the scrollable `notification-daemon` Notification History drawer |
| **Click Gear Icon (`⚙`) / Right-Click** | Opens the live native Settings window (right-click also dismisses active toasts) |

---

## CLI Options

```text
Usage: xdynamicisland [OPTIONS]

Options:
  -c, --config <path>     Load/save config at custom path
  -s, --settings          Open settings window on startup
  -p, --position <pos>    top-center|top-left|top-right|bottom-center|bottom-left|bottom-right
  -e, --eq-style <style>  wave|bars|ribbon|spectrum|segments|solid
      --always-on-top     Keep island above all normal windows (default)
      --no-always-on-top  Allow normal windows to cover the island
      --avoid-dmenu       Reposition below dmenu/rofi when summoned (default)
      --no-avoid-dmenu    Disable obstacle repositioning
  -v, --version           Print version and exit
  -h, --help              Show this help message
```

---

## Configuration

Settings are automatically saved to `~/.config/xdynamicisland/config.ini`:

```ini
[island]
always_on_top = true
avoid_obstacles = true
avoid_panels = true
hide_when_idle = false
spin_art = true
real_audio_viz = true
show_clock_idle = true
persist_notifications = true
position = top-center
eq_style = wave
art_shape = circle
monitor_index = -1
margin = 10
obstacle_gap = 8
toast_duration_ms = 4500
max_history = 64
volume_step = 5
accent_color = #FF6A00
opacity = 0.96
font_family = Sans
```

Notification history is persisted in `~/.local/share/xdynamicisland/notifications.tsv` and remote album artwork is cached in `~/.cache/xdynamicisland/art/`.

---

## Building from Source

### Runtime & Build Dependencies

Only standard Linux X11 desktop libraries are required *(replace `doas` with `sudo` if preferred)*:

```bash
# Debian / Ubuntu
doas apt install build-essential pkg-config libx11-dev libxext-dev libxrandr-dev \
                 libcairo2-dev libpango1.0-dev libdbus-1-dev

# Fedora
doas dnf install gcc make pkg-config libX11-devel libXext-devel libXrandr-devel \
                 cairo-devel pango-devel dbus-devel

# Arch Linux
doas pacman -S base-devel pkgconf libx11 libxext libxrandr cairo pango dbus
```

Optional runtime tools automatically detected if present:
- `wpctl` (WirePlumber / PipeWire) for volume & mute controls
- `libpulse-simple.so.0` (PipeWire-Pulse / PulseAudio) for live FFT audio waveform capture
- `kdeconnectd` for remote phone media control & notifications
- `notification-daemon` (or any `org.freedesktop.Notifications` daemon) for notification history
- `curl` for downloading remote HTTPS album artwork

### Build, Test & Install

```bash
git clone https://github.com/MANICX100/xdynamicisland.git
cd xdynamicisland

make release       # Build -O3 -flto stripped binary (~176 KB)
make test          # Run unit test suite
make sanitize      # Run tests with ASan + UBSan (requires clang or libasan)
doas make install  # Install to /usr/local/bin/xdynamicisland (or: sudo make install)
```

---

## Architecture

```text
src/
├── main.c           # Single poll() event loop (X11 fd + MPRIS D-Bus fd + Notification D-Bus fd)
├── ui.c             # Dynamic Island X11 window (ARGB + XShape fallback), Cairo/Pango renderer, waveforms
├── settings_ui.c    # Native lightweight X11 + Cairo Settings window
├── spring.c         # Sub-stepped damped harmonic spring physics engine
├── media.c          # MPRIS2 D-Bus client (Spotify/browsers/KDE Connect) + wpctl volume backend
├── audio_viz.c      # 256-pt Radix-2 FFT + optional dlopen(libpulse-simple) + harmonic fallback
├── notifications.c  # Non-invasive D-Bus BecomeMonitor listener for notification-daemon + history store
├── obstacles.c      # SubstructureNotify + EWMH obstacle solver (dmenu, rofi, top/bottom bars)
├── art.c            # Background threaded album art fetcher/decoder (embedded stb_image) + color extractor
└── config.c         # INI configuration parser/writer + glossy accent gradient math
```

---

## License

Released under the [MIT License](LICENSE).
