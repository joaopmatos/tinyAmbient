# tinyAmbient — Implementation Architecture

Per-component detail. Scope and behavior come from `docs/PRD.md`; layer rules and the size budget come from `AGENTS.md`. This document answers "how does this file work internally".

## 1. Thread model

v1 has two threads.

| Thread | Responsibility | Allowed to |
|---|---|---|
| Main | daemon loop, IPC accept/poll, device restart with backoff, config writes | allocate, block, do I/O, log |
| Audio | miniaudio callback, `wn_engine_render` only | read atomics, write the buffer, nothing else |

No UI thread in v1. No locks between the two: all communication goes through atomics.

### State ownership

| State | Writer | Reader |
|---|---|---|
| `noise_type` | main | audio |
| `playing` | main | audio |
| `volume_milli` | main | audio |
| `stop_at_ms` (monotonic) | main | main |
| filter phases, RNG state | audio | audio |
| config on disk | main | main |

The audio thread never reads the stop-after deadline. The main thread checks it and clears `playing` once the fade completes.

---

## 2. `core/command.c`

Single parser for argv and IPC lines. It knows nothing about sockets, files, or `stdout`.

```c
typedef enum {
    WN_CMD_NONE, WN_CMD_PLAY, WN_CMD_PAUSE, WN_CMD_TOGGLE,
    WN_CMD_NOISE, WN_CMD_VOLUME, WN_CMD_STOP_AFTER,
    WN_CMD_MONO, WN_CMD_STEREO, WN_CMD_STATUS,
    WN_CMD_RENDER, WN_CMD_PORTABLE, WN_CMD_FOREGROUND,
    WN_CMD_VERSION, WN_CMD_HELP, WN_CMD_QUIT
} wn_cmd_kind;

typedef struct {
    wn_cmd_kind kind;
    int         ivalue;   /* volume, minutes, seed, seconds */
    const char *svalue;   /* noise type, render path */
} wn_cmd;
```

Contract:

- Returns commands in written order, or an error with an invalid `kind` and a message.
- Treats bare words and long flags as equivalent.
- Rejects the combinations listed in PRD section 7 (render with daemon commands, status with content commands).
- Validates syntax plus the ranges it can check locally: `volume` and `noise`.

The mapping to IPC text (`noise pink`, `volume 40`) and the inverse parser live here, so there is only one semantics.

---

## 3. `core/config.c`

INI parser and formatter. No `fopen`, no paths, no `stat`. Takes and returns strings.

```c
typedef struct {
    int noise;         /* wn_noise_type */
    int volume;        /* 0..100 */
    int mono;          /* 0/1 */
    int fade_ms;       /* 0..2000 */
} wn_config;

void wn_config_defaults(wn_config *cfg);
int  wn_config_parse(const char *text, size_t len, wn_config *out,
                     char *warn, size_t warn_cap);
int  wn_config_format(const wn_config *cfg, const char *original,
                      char *out, size_t cap);
```

- Keys under `[general]`: `noise`, `volume`, `mono`, `fade_ms`.
- Unknown keys survive a rewrite.
- Malformed values fall back to defaults with a warning; they never abort startup.
- `fade_ms` is clamped to 0-2000 on read.

The atomic write (temp file + `fsync` + `rename`) lives in the layer that owns I/O, not here.

---

## 4. `core/noise.c`

```c
typedef struct {
    uint64_t rng;          /* PCG32, one per channel */
    int      type;
    float    b0..b6;       /* Kellet, pink */
    float    brown, dc_x, dc_y;
    float    gain;
} wn_noise;

void  wn_noise_init(wn_noise *n, int type, uint64_t seed);
void  wn_noise_set_type(wn_noise *n, int type);   /* resets filters, keeps RNG */
void  wn_noise_render(wn_noise *n, float *out, unsigned frames, unsigned channels);
float wn_noise_gain_for(int type);                /* calibrated, fixed table */
```

- PCG32 with an explicit seed; one state per channel to decorrelate them.
- White: uniform over `[-1,1)`.
- Pink: Paul Kellet, 7 poles.
- Brown: leaky integrator (`a ~= 0.9995`) followed by a DC blocker. A higher leak flattens the slope (-4.87 dB/oct in the PoC); do not repeat that.
- Gains come from a table calibrated with `check_spectrum.py`, not from invented constants.
- `wn_noise_render` does not change `type` itself: the engine orchestrates type changes so it can crossfade.
- No `sin`/`exp`/`log` per sample.

---

## 5. `core/engine.c`

Owns observable state and applies fades. It is the only thing the callback calls.

Internal state:

- two `wn_noise` instances (left/right), or one when `mono` is set;
- current and target gain (linear volume) with a 20 ms ramp;
- crossfade pair: old and new type, progress in samples, duration `fade_ms`;
- `playing` and the stop-after deadline.

Behavior:

- Reads atomics once at the start of each buffer, never mid-sample-loop.
- `volume 0` produces silence with `playing=1`; generation continues.
- A type change crossfades over `fade_ms`, with no gap and no discontinuity.
- Stop-after fade-out lasts 20 s; at the end `playing=0` and the daemon keeps running.
- Startup and quit use a short fade to avoid pops.
- The output buffer is always fully written, including during silence.

---

## 6. `audio/device.c`

The only file that includes `miniaudio.h` with `MINIAUDIO_IMPLEMENTATION`.

- `ma_device` with `f32`, 2 channels, 48 kHz (`ma_device_config_init(ma_device_type_playback)`).
- Period of 512-1024 frames.
- Callback: `ma_device *` to `wn_engine *` via `pUserData`; calls `wn_engine_render` and returns.
- No allocation, locks, logging, or syscalls inside the callback.
- The notification callback (device stopped or removed) only sets `_Atomic int restart_requested`; the main thread observes it and reinitializes.
- Backoff: 100 ms, 500 ms, 2 s, then 5 s.
- Parameters and `playing` survive a restart; resuming must not click.

---

## 7. `platform/paths.c`

Resolves the config directory. The only place that touches `getenv`, `HOME`, or `APPDATA`.

| OS | Path |
|---|---|
| Linux | `$XDG_CONFIG_HOME/tinyambient/config.ini`, fallback `~/.config/tinyambient/config.ini` |
| Windows | `%APPDATA%\tinyAmbient\config.ini` |

- `--portable` replaces this with `config.ini` next to the executable.
- Returns fixed buffers built with `snprintf`; explicit error if the path does not fit.
- In portable mode, verify the directory is writable and fail with a clear message otherwise.
- Create the parent directory if missing, mode `0700`.

---

## 8. `platform/ipc.c`

| OS | Address | Permissions |
|---|---|---|
| Linux | `$XDG_RUNTIME_DIR/tinyambient.sock` (fallback `/tmp/tinyambient-<uid>.sock`) | `0600` |
| Windows | `\\.\pipe\tinyambient-<sid>` | Current-user ACL |

Startup sequence for any invocation:

1. try to connect to the socket/pipe;
2. if it exists, send the command, read the reply, print it, exit;
3. if it does not exist, become the daemon and create the socket.

Details:

- One command per line, newline-terminated; replies are `ok` or `key=value` lines.
- Fixed line limit (e.g. 256 bytes); longer commands are a usage error.
- An orphaned socket (dead process) is removed before creating a new one.
- IPC failure returns exit code 3.
- `--foreground` keeps the process attached to the terminal and logs lifecycle events to `stderr`.

---

## 9. `ui/cli.c`

Turns `argv` into `wn_cmd` through `core/command.c` and formats output. Flags and precedence rules live in PRD section 7; do not duplicate them here.

- `--status` prints exactly the six lines from PRD section 8, in order.
- `--help` and `--version` take precedence over everything.
- Errors go to `stderr` prefixed with `tinyambient: `.
- Exit codes: 0 success, 1 runtime failure, 2 usage error, 3 IPC failure.

---

## 10. `main.c` and `--render`

`main.c` only wires things together:

```
parse argv -> config load -> decide mode
  +- render   -> in-memory engine -> writer -> exit
  +- status   -> IPC if a daemon exists, else config -> exit
  +- content  -> IPC if a daemon exists, else start daemon -> exit
  +- no args  -> start daemon if absent, then exit
```

`--render` starts no daemon, opens no device, and writes no config. It uses the same `wn_engine_render` with an explicit seed so output matches playback.

WAV writer: 44-byte header, `float32`, 2 channels, 48 kHz. Any other extension produces raw little-endian interleaved float32. Expected data size: `384000 * seconds` bytes.

---

## 11. Invariants to test

1. Same seed and parameters produce byte-identical output.
2. No sample outside `[-1.0, 1.0]`.
3. A type change produces no step above 0.25 between consecutive samples.
4. Slopes and RMS stay within the tolerances of PRD section 6, FR-02.
5. A config write killed mid-flight leaves a parseable file.
6. A second invocation creates no second daemon and returns in under 100 ms.
7. `--status` follows the format and values of FR-08, with and without a daemon.
