# tinyAmbient — Product Requirements Document

**Version:** 1.0
**Date:** 2026-10-29
**Status:** Approved for v1 implementation
**Companion documents:** `AGENTS.md` (constraints, layers, toolchain, size budget) and `docs/ARCHITECTURE.md` (per-component implementation detail).

This document defines product scope and observable behavior. `AGENTS.md` defines how the product is built. Where the two disagree, this document wins on scope and behavior, and `AGENTS.md` must be updated.

## TL;DR

tinyAmbient v1 is a background ambient-noise player: one small executable per OS, driven by a CLI, with a single-instance daemon. It targets Linux (Fedora KDE) and Windows 10/11, generates white, pink, and brown noise, and persists state in an INI file. No tray, no autostart, no network, no GUI toolkit.

## 1. Summary

- One binary per OS, at most 150 KB on Linux and 300 KB on Windows.
- Controlled by a CLI; a daemon keeps audio running between invocations.
- Three noise types: white, pink, brown.
- Persistent configuration in a plain INI file.
- Offline render mode for headless DSP verification.

## 2. Problem and context

Continuous background noise is used for sustained concentration and masking office noise. The available options carry costs that matter on a working machine:

| Option | Cost |
|---|---|
| Browser tab (web generators, video) | Hundreds of MB RSS, network traffic, tab management |
| Electron applications | 150–300 MB RSS, auto-update surface |
| Streaming services | Network dependency, account, ads |

The requirement is a process that starts instantly, stays under 10 MB RSS, uses under 1% of one core, creates no network sockets, and is controllable from a terminal or a script.

Two machines drive v1: a personal Fedora KDE desktop and a Windows 10/11 work laptop. macOS is out of scope.

The motivation for this project is personal: at home, I like to work on a Fedora KDE environment (Desktop and Laptop) and, at work, I use a Windows machine (oh, the pain!). This aims, first and foremost, at solving the need for a tool to play ambient noise with *very little* resource consumption.

## 3. Users and scenarios

Single user: a developer working on two machines, preferring keyboard and terminal over GUI.

| ID | Scenario | Expected behavior |
|---|---|---|
| S1 | User starts work and runs `tinyambient` | Daemon starts, plays saved noise at saved volume; the first sample is audible within 200 ms and the level swells over 1 s instead of arriving as a step |
| S2 | User joins a call and runs `tinyambient toggle` | Audio pauses with a 20 ms fade |
| S3 | User runs `tinyambient --volume 25` | Volume changes with a fade; value persists for the next session |
| S4 | User wants the noise to stop when the work session ends and runs `tinyambient --stop-after 50` | At the 50-minute mark, playback fades out over 20 s and pauses |
| S5 | User checks state from a script with `tinyambient --status` | Fixed-format `key=value` lines on stdout, exit code 0 |
| S6 | User switches from pink to brown with `tinyambient --noise brown` | 250 ms crossfade, no click, value persists |
| S7 | User validates DSP without a sound card | `tinyambient --render out.wav --seconds 10 --noise brown --seed 42` produces a deterministic file |

## 4. Goals and success metrics

| Goal | Metric | Gate |
|---|---|---|
| Small binary | Linux stripped size | <= 150 KB |
| | Windows binary, static CRT | <= 300 KB |
| Fast start | Exec to first audible sample | < 200 ms |
| Low CPU | Average CPU, 48 kHz stereo, 2019+ x86_64 | <= 1% of one core |
| Low memory | Resident set size | < 10 MB |
| No network | Outbound sockets in any mode | 0 |
| No clicks | Audible artifacts on start, stop, volume change, noise change, timer expiry | 0 |
| Determinism | Same seed and parameters produce identical output | Byte-identical |
| Reliable state | Config survives process kill during write | Never truncated or corrupt |

## 5. Scope

### In scope (v1)

- Platforms: Linux (Fedora KDE, x86_64) and Windows 10/11 (x86_64).
- Noise types: white, pink, brown.
- CLI with a single-instance daemon.
- Playback control, volume, mono/stereo, session auto-stop timer.
- Persistent INI configuration with atomic writes.
- Portable mode.
- Offline render to raw float32 or WAV.
- Audio device failure recovery.

### Out of scope (v1)

- Autostart integration of any kind. Launch-on-login is left to OS tooling managed by the user (systemd user unit, Task Scheduler, desktop launcher). No installer hooks.
- Tray icon on any platform.
- macOS.
- Sleep-aid and bedtime use cases.
- Blue, gray, and green noise. Their curves are undefined.
- Noise layering, presets, equalization.
- File playback, decoders, playlists.
- Network, telemetry, auto-update.
- Global hotkeys.
- Audio device selection from the CLI.
- GUI toolkit of any kind.

## 6. Functional requirements

**FR-01 Playback control.** The CLI accepts `play`, `pause`, and `toggle`. Each applies to the running daemon with a 20 ms volume fade. Resulting state is reported by `--status` and lasts for the life of the daemon.

**FR-02 Noise generation.** Types `white`, `pink`, `brown`. Output is 48 kHz, float32, stereo interleaved. Measured slope over 40 Hz–12 kHz in third-octave bands: white 0 +/- 0.5 dB/oct, pink -3 +/- 0.5 dB/oct, brown -6 +/- 1.0 dB/oct. RMS target -18 dBFS +/- 0.5 dB. Peak stays within [-1.0, 1.0].

**FR-03 Volume.** Integer 0–100 mapped to linear gain, applied with a 20 ms fade. Volume `0` produces silence while `playing=1`. Values outside 0–100 are a usage error.

**FR-04 Noise switching.** Changing type crossfades over `fade_ms` (default 250). No discontinuity, no clipping, no gap.

**FR-05 Session auto-stop.** `--stop-after <minutes>` starts a countdown from the moment the command is applied; minutes are an integer from 1 to 10080 (one week). `--stop-after off` cancels it. At expiry, playback fades out over 20 s and pauses; the daemon stays alive. `play`, `toggle`, or a volume change during the fade cancels the pause; a resume that reaches the engine in the same buffer as the expiry loses, because the timer is authoritative. The timer bounds a work session; it is session state and is never written to configuration.

**FR-06 Config persistence.** Commands that change a persisted setting (`noise`, `volume`, `mono`, `stereo`) apply to the daemon and write configuration. Playback commands (`play`, `pause`, `toggle`) and `stop-after` change session state only and never write. Writes are atomic: temporary file in the target directory, `fsync`, `rename`. `--status`, `--render`, `--help`, and `--version` never write.

**FR-07 Single instance.** One daemon per user session. A second invocation forwards its command over IPC and exits. Linux: Unix socket at `$XDG_RUNTIME_DIR/tinyambient.sock`, mode `0600`. Windows: named pipe at `\\.\pipe\tinyambient-<sid>` restricted to the current user.

**FR-08 Status output.** `--status` prints exactly six lines on stdout, in this order, with no spaces around `=`, each terminated by a newline:

```
version=1.0.1
playing=1
noise=pink
volume=20
mono=0
stop_after=off
```

`stop_after` is either `off` or an integer count of remaining seconds. With no daemon running, `--status` reads configuration, reports `playing=0` and `stop_after=off`, and exits 0.

**FR-09 No-argument start.** `tinyambient` with no arguments starts the daemon if absent, plays the saved noise at the saved volume, and exits 0. First sample is audible within 200 ms. A session start opens from silence over 1 s instead of the 20 ms used for playback changes, so a first run is not dropped into a steady level. `--render` keeps the 20 ms fade, so offline output is unchanged.

**FR-10 Offline render.** `--render <file>` writes 48 kHz stereo float32 without opening an audio device and without contacting the daemon. A `.wav` extension produces a WAV container; any other extension produces raw little-endian float32 interleaved. `--seconds N` defaults to 10 and accepts 1–3600. `--seed N` defaults to 1. `--noise`, `--volume`, and `--mono`/`--stereo` select the same parameters a daemon would use; the render defaults are white, volume 100, and stereo. Byte count is `48000 * 2 * 4 * N`.

**FR-11 Portable mode.** `--portable` reads and writes `config.ini` next to the executable instead of the per-OS configuration directory.

**FR-12 Foreground mode.** `--foreground` keeps the daemon attached to the terminal and logs lifecycle events to stderr. The process does not detach.

**FR-13 Mono and stereo.** Default is independent per-channel generators. `--mono` generates one channel and duplicates it. `--stereo` restores the default. The choice persists.

**FR-14 Exit codes and diagnostics.** Exit codes: `0` success, `1` runtime failure (audio device, config write, render I/O), `2` usage error, `3` IPC failure. Errors print to stderr prefixed with `tinyambient: `.

**FR-15 Version and help.** `--version` prints `tinyambient 1.0.0` and exits 0. `--help` prints usage and exits 0.

**FR-16 Device failure recovery.** When the audio device stops or is removed, the daemon retries initialization with backoff (100 ms, 500 ms, 2 s, then every 5 s). Parameters and playback state are preserved. Resuming produces no click.

## 7. CLI contract

```
tinyambient [command] [options]

Commands
  play | pause | toggle
  noise <white|pink|brown>
  volume <0-100>
  stop-after <1-10080|off>
  status
  quit

Options
  --noise <type>
  --volume <0-100>
  --play | --pause | --toggle
  --stop-after <minutes|off>
  --status
  --quit
  --mono | --stereo
  --render <file> [--seconds N] [--seed N] [--noise T] [--volume N]
                   [--mono | --stereo]
  --portable
  --foreground
  --version
  --help
```

Rules:

1. Bare words and long flags are equivalent. `tinyambient noise pink` and `tinyambient --noise pink` behave identically.
2. Multiple content flags in one invocation apply in the order written.
3. `--render` implies offline mode. Combining `--render` with playback, timer, status, quit, portable, or foreground commands is a usage error (exit 2). `--noise`, `--volume`, and `--mono`/`--stereo` act as render parameters in this mode.
4. Combining `--status` with content flags is a usage error (exit 2).
5. Unknown flags, unknown noise names, and out-of-range values exit 2 with a one-line message.
6. `--help` and `--version` take precedence over all other arguments.
7. Content commands are forwarded to the daemon when it exists and executed by a new daemon when it does not. Both paths produce identical results.
8. A `--render` path beginning with `-` is a usage error (exit 2), like a flag-like value for any other option.

## 8. Configuration file

Location: `$XDG_CONFIG_HOME/tinyambient/config.ini` (Linux, fallback `~/.config/tinyambient/config.ini`), `%APPDATA%\tinyAmbient\config.ini` (Windows). `--portable` overrides both.

```ini
[general]
noise=pink
volume=20
mono=false
fade_ms=250
```

| Key | Type | Default | Range |
|---|---|---|---|
| `noise` | enum | `pink` | `white`, `pink`, `brown` |
| `volume` | integer | `20` | 0–100 |
| `mono` | boolean | `false` | `true`, `false` |
| `fade_ms` | integer | `250` | 0–2000 |

Unknown keys and comments are ignored and preserved on rewrite. Malformed values fall back to defaults with a warning on stderr.

## 9. Non-functional requirements

**NFR-01 Size.** Linux stripped binary <= 150 KB. Windows binary with static CRT <= 300 KB. CI fails on breach.

**NFR-02 Audio thread.** The render callback performs no allocation, no locking, no logging, no syscalls. Parameters are read once per buffer from atomics.

**NFR-03 Layer purity.** `core/` includes only `libc`, `libm`, and C11 standard headers. No OS headers.

**NFR-04 Privacy.** No network sockets are created in any mode. IPC artifacts are readable only by the owning user.

**NFR-05 Build hygiene.** Own code compiles warning-free with `-Wall -Wextra -Wpedantic`. ASan and UBSan builds are clean. miniaudio is excluded from sanitizer builds.

**NFR-06 Determinism.** DSP output depends only on seed, noise type, channel index, and sample count. No wall-clock or platform-dependent RNG.

**NFR-07 Denormals.** Filter states never enter denormal range during long fades or silence.

## 10. Acceptance criteria

| FR | Test |
|---|---|
| FR-01 | Headless: `play`, `pause`, `toggle` through IPC produce the expected `status` transitions |
| FR-02 | `check_spectrum.py` on rendered files: slope and RMS within tolerance; peak <= 1.0 |
| FR-03 | Volume 0 renders exactly 0.0; volume 100 matches calibrated RMS; volume 101 exits 2 |
| FR-04 | Render with a type change: no sample outside [-1, 1], no consecutive-sample step above 0.25 |
| FR-05 | Stop-after of 1 minute on a test build ends with `playing=0`, `stop_after=off`, daemon alive |
| FR-06 | Kill during a write leaves a valid, parseable config |
| FR-07 | Second invocation returns in under 100 ms and does not create a second daemon |
| FR-08 | Output matches the six-line format exactly, including order and newlines |
| FR-09 | Fresh config, no daemon: single invocation produces audio and exits 0 |
| FR-10 | File size equals `384000 * seconds`; two renders with the same seed are byte-identical |
| FR-11 | `--portable` creates `config.ini` beside the executable and ignores XDG/AppData |
| FR-12 | `--foreground` keeps stdout/stderr attached and exits on `quit` |
| FR-13 | Mono render has identical left and right channels; stereo render differs |
| FR-14 | Each failure class returns its documented exit code and a `tinyambient: ` message |
| FR-15 | `--version` prints one line and exits 0; `--help` exits 0 |
| FR-16 | Removing the default device mid-playback resumes without a click after the device returns |
| NFR-01 | CI size check on the Linux binary; manual check on Windows |
| NFR-04 | `strace` on Linux shows no `socket`/`connect` calls to network families |

## 11. Milestones

| Milestone | Deliverable | Exit criterion |
|---|---|---|
| M0 | CMake, directory structure, CI skeleton | Builds on Linux and Windows |
| M1 | RNG, white noise, `--render` | Deterministic white render, size measured |
| M2 | Pink and brown noise, spectrum analysis | Slopes within tolerance |
| M3 | WAV writer, headless tests | Byte-count and determinism tests pass |
| M4 | miniaudio playback and callback | Continuous playback, no clicks |
| M5 | CLI, engine, config | FR-01, FR-03, FR-06, FR-11, FR-13, FR-14, FR-15 pass |
| M6 | Atomics, fades, auto-stop timer | FR-04, FR-05, FR-16 pass |
| M7 | IPC, single instance | FR-07, FR-08, FR-09 pass |
| M8 | Windows parity | All FRs pass on Windows 10/11 |

M0–M7 constitute the Linux release. M8 closes v1. v1.0.1 is a patch on M8: the
saved default drops to volume 20 and a session start fades in over 1 s.

## 12. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| miniaudio grows between versions | Size gate breach | Pin the version, measure every milestone |
| ALSA/Pulse absent on target system | No audio | Fall back to the other backend, report a clear error |
| Windows console subsystem choice | Console flashes on background start | Resolved at M8: console subsystem, with `DETACHED_PROCESS` for the daemon. A GUI subsystem would need `AttachConsole` before any `--status` output |
| Float filter drift across platforms | Spectrum test failures on Windows | Resolved at M8: the spectrum gate runs and passes on the Windows runner |
| Scope creep toward tray or autostart | Delayed release | Both are explicitly out of scope in section 5 |
| Config path collisions in portable mode | Shared state between copies | Portable mode requires a writable executable directory; fail with a clear error otherwise |

## 13. Decision log

| # | Decision | State |
|---|---|---|
| D-01 | Autostart removed from all versions; left to OS tooling | Decided |
| D-02 | Tray icon out of v1 | Decided |
| D-03 | macOS out of v1 | Decided |
| D-04 | v1 ships white, pink, brown only | Decided |
| D-05 | No arguments starts the daemon and plays | Decided |
| D-06 | Setting commands (`noise`, `volume`, `mono`, `stereo`) persist automatically; playback and timer changes are session state | Decided |
| D-07 | Auto-stop expiry pauses; daemon stays alive | Decided |
| D-08 | Volume 0 is silence with `playing=1` | Decided |
| D-09 | `status` uses a fixed six-line `key=value` format | Decided |
| D-10 | `--seed` added to the CLI for deterministic renders | Decided |
| D-11 | `--mono`/`--stereo` added to the CLI | Decided |
| D-12 | `--device` dropped from v1 | Decided |
| D-13 | `sleep_minutes` dropped from config; timer is session state | Decided |
| D-14 | Default stereo uses independent per-channel generators | Decided |
| D-15 | Repository license: MIT | Decided |
| D-16 | Windows IPC and daemon startup land at M8: named pipes with a per-user DACL, and a detached copy of the executable instead of a fork | Decided |
| D-17 | A session starts at volume 20 and opens over 1 s (v1.0.1); `--render` keeps the 20 ms fade | Decided |

## 14. Open questions

1. Exact curves and filters for gray and green noise (v2).
2. Whether the tray returns in v2, and on which platform first.
3. Windows console subsystem for background starts (resolved at M8).
4. Repository license (resolved: MIT).
