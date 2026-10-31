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
    WN_CMD_RENDER, WN_CMD_SECONDS, WN_CMD_SEED,
    WN_CMD_PORTABLE, WN_CMD_FOREGROUND,
    WN_CMD_VERSION, WN_CMD_HELP, WN_CMD_QUIT
} wn_cmd_kind;

typedef struct {
    wn_cmd_kind kind;
    int         ivalue;   /* volume, minutes, seed, seconds */
    const char *svalue;   /* noise type, render path */
} wn_cmd;
```

Contract:

- Returns commands in written order, or -1 with a message in `err`.
- Treats bare words and long flags as equivalent: `noise pink`, `--noise pink`,
  `play`, and `--play` all parse the same way.
- Rejects the combinations listed in PRD section 7 (render with daemon
  commands, status with content commands, seconds or seed without render).
- Validates syntax plus the ranges it can check locally: `volume` 0–100,
  `stop-after` 1–10080 or `off`, `seconds` 1–3600, `seed` 0–2147483647, and
  the three noise names.
- Repeating a single-valued option (`--noise`, `--volume`, `--stop-after`,
  `--render`, `--seconds`, `--seed`) is an error. Play/pause/toggle and
  mono/stereo may appear together; the last one wins, as rule 2 requires.
- `wn_cmd_parse_line` splits a mutable buffer in place. `svalue` may point into
  that buffer, so the caller keeps it alive while applying the commands.

The mapping to IPC text (`noise pink`, `volume 40`) and the inverse parser live
here, so there is only one semantics. `wn_engine_command` uses the line parser
rather than re-implementing a command subset.

`wn_engine_command` returns 0 on success and non-zero for an unknown or invalid command, so the IPC layer can answer with an error instead of `ok`.

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

void   wn_config_defaults(wn_config *cfg);
void   wn_config_parse(const char *text, size_t len, wn_config *out,
                       char *warn, size_t warn_cap);
size_t wn_config_format(const wn_config *cfg, const char *original,
                        char *out, size_t cap);
```

- Keys under `[general]`: `noise`, `volume`, `mono`, `fade_ms`.
- Parsing starts from defaults, so a missing key keeps its default. Section and
  key names are trimmed and compared case-insensitively; `#` and `;` start a
  comment line.
- Unknown keys survive a rewrite. Unknown keys under `[general]` are emitted
  after the four canonical keys, before any other section, so they cannot drift
  into the section that follows them. Unknown sections are preserved after the
  canonical block.
- Malformed values fall back to the default for that key with a warning; they
  never abort startup. The first warning is kept.
- `volume` is range-checked. `fade_ms` is clamped to 0-2000 on read.
- `wn_config_format` uses `snprintf` semantics: it returns the byte count the
  full text needs, so a value `>= cap` signals truncation. With `out == NULL`
  and `cap == 0` it only measures.

The atomic write (temp file + `fsync` + `rename`) lives in the layer that owns
I/O, not here; see `platform/config_file.c` below.

---

## 4. `core/noise.c`

```c
typedef struct {
    uint64_t state;         /* PCG32 state */
    uint64_t inc;           /* PCG32 stream; must be odd */
    int      type;
    float    pink_b[7];     /* Paul Kellet, 7 poles */
    float    brown;         /* leaky integrator */
    float    dc_x, dc_y;    /* DC blocker */
} wn_noise;

void  wn_noise_init(wn_noise *n, int type, uint64_t seed, uint64_t stream);
void  wn_noise_set_type(wn_noise *n, int type);   /* resets filters, keeps RNG */
float wn_noise_next(wn_noise *n);                 /* one sample */
float wn_noise_gain_for(int type);                /* calibrated, fixed table */
```

- PCG32 with an explicit seed; one generator per channel. Channels are
  decorrelated by **stream**, not by offsetting the seed: `stream = channel
  index`, `inc = (stream << 1) | 1`. Generators with different increments walk
  disjoint sequences, which offsetting a seed cannot guarantee.
- White: uniform over `[-1,1)`, taken as `(x >> 8)` — 24 bits, exactly the
  float32 significand width, so every conversion step is exact and the output
  distribution has no rounding bias.
- Pink: Paul Kellet, 7 poles.
- Brown: leaky integrator (`a ~= 0.9995`) followed by a DC blocker. A higher leak flattens the slope (-4.87 dB/oct in the PoC); do not repeat that.
- Gains come from a table calibrated with `check_spectrum.py`, not from invented constants. White is analytic: RMS of a uniform distribution is `1/sqrt(3)`, and -18 dBFS is `10^(-18/20)`, so the gain is `0.218052`. Pink and brown are measured over 10M samples on both streams: `0.071424` and `0.009741`. The C test `test_noise` proves the analytic value matches the ensemble measurement to five digits, which is the sanity check on the method.
- Brown's energy sits near DC, so any window shorter than about a minute lands within roughly +/-0.3 dB of the target. That is a property of `1/f^2` noise, not a calibration error; the spectrum test uses 20 s renders for that reason.
- `wn_noise_next` does not change `type` itself: the engine orchestrates type changes so it can crossfade.
- No `sin`/`exp`/`log` per sample.
- Filter state resets on a type change while the RNG state is preserved, so switching type never rewinds the sequence.

### Measuring the slope

`tests/check_spectrum.py` Welch-averages a Hann-windowed FFT, sums power into
third-octave bands, and fits band level in dB against `log2(frequency)`.

Band levels must be **power spectral density** (power divided by band width).
A third-octave band's width is proportional to its center frequency, so summing
raw band power reports +3 dB/oct for flat white noise. Dividing by the width is
what makes the PRD targets 0, -3, and -6 dB/oct rather than 3, 0, and -3.

---

## 5. `core/engine.c`

Owns observable state and, from M6, applies fades. It is the only thing the
callback calls.

Internal state at M5:

- two `wn_noise` instances (left/right), or one generator duplicated when
  `mono` is set;
- `type`, `volume`, `mono`, and `playing`;
- an atomic `frames_rendered` counter for diagnostics.

`wn_engine_command` parses a line with `wn_cmd_parse_line` and applies every
command only after the whole line is known to be supported, so an invalid tail
cannot leave the engine half-applied. It accepts `noise`, `volume`, `play`,
`pause`, `toggle`, `mono`, and `stereo`. Status, quit, and stop-after belong to
the daemon loop, not the engine.

Behavior at M5:

- Parameter changes are immediate. M6 turns the fields into atomics read once
  per buffer and adds the 20 ms volume fade and the `fade_ms` crossfade.
- `volume 0` produces silence with `playing=1`; generation continues, so filter
  state and RNG progress do not depend on the volume.
- A paused engine writes silence and does not generate. M6 replaces the abrupt
  stop with a fade.
- The output buffer is always fully written, including during silence.
- `wn_engine_noise`, `wn_engine_volume`, `wn_engine_is_mono`, and
  `wn_engine_is_playing` expose the state for status output. The stop-after
  deadline stays on the main thread and is not engine state.

`wn_engine_set_seed` reseeds both generators (streams stay 0 and 1). Only
`--render` uses it, because there the output must depend on the seed alone
(NFR-06). Playback uses the fixed default seed: noise needs no variety.

Volume maps linearly, `0..100 -> 0.0..1.0`, multiplied by the calibrated type
gain. Mono renders one generator and duplicates the sample, so both channels
stay bit-identical (FR-13).

---

## 6. `audio/device.c`

The only file that includes `miniaudio.h` with `MINIAUDIO_IMPLEMENTATION`.

- `ma_device` with `f32`, 2 channels, 48 kHz (`ma_device_config_init(ma_device_type_playback)`).
- Period of 512 frames.
- Callback: `ma_device *` to `wn_engine *` via `pUserData`; calls `wn_engine_render` and returns.
- No allocation, locks, logging, or syscalls inside the callback.
- The device and the forced context are static, never heap-allocated: a fixed address cannot be freed under a running callback.
- `notificationCallback` runs on a miniaudio-owned thread and only stores `_Atomic int restart_requested`, for `stopped`, `rerouted`, and `interruption_began`. A `stopping` guard keeps our own `ma_device_uninit` from arming it.
- `wn_audio_needs_restart` exposes the flag. The backoff loop (100 ms, 500 ms, 2 s, then every 5 s) belongs to `platform/` and lands in M7, because that is where the main loop is.
- Parameters and `playing` survive a restart; resuming must not click (fades land in M6).
- At M4 nothing in the shipped binary calls `wn_audio_start`, so `--gc-sections` still removes miniaudio entirely. `tests/play_check.c` is the harness that exercises the path, and the size it reports (about 86 KB of `.text`) is the real cost the binary will carry once M7 gives the device a caller.
- `TINYAMBIENT_AUDIO=null` forces miniaudio's null backend, which drives the callback in real time without hardware. CI sets it, so the audio path has automated coverage; it also separates a device fault from a DSP fault when debugging.

---

## 7. `platform/paths.c`

Resolves the config directory. The only place that touches `getenv`, `HOME`, or `APPDATA`.

| OS | Path |
|---|---|
| Linux | `$XDG_CONFIG_HOME/tinyambient/config.ini`, fallback `~/.config/tinyambient/config.ini` |
| Windows | `%APPDATA%\tinyAmbient\config.ini` |

- `--portable` replaces this with `config.ini` next to the executable. Linux
  resolves the executable through `/proc/self/exe`; Windows uses
  `GetModuleFileNameA`.
- Returns fixed buffers built with `snprintf`; explicit error if the path does
  not fit.
- Non-portable saves create missing parent directories with mode `0700`.
- Portable mode never creates the executable's directory. The atomic write then
  fails with a clear message when that directory is not writable.
- `wn_paths_ipc_address` lands with IPC in M7.

---

## 8. `platform/config_file.c`

File I/O for the INI configuration. The parser and formatter stay in `core/`;
this module owns the path-dependent parts.

- `wn_config_file_load` reads at most `WN_CONFIG_TEXT_MAX` (8192) bytes. A
  missing file is not an error: defaults are used and the original text is
  empty. A larger file is parsed up to the cap and reports a warning.
- Warnings about malformed values are passed through from `core/config.c`.
  They never abort startup.
- `wn_config_file_save` formats the configuration, preserving unknown keys and
  sections from the original text, then writes atomically:
  1. create a temporary file in the same directory (`mkstemp` on POSIX,
     `path.tmp.<pid>` on Windows);
  2. `fwrite`, `fflush`, `fsync` (`_commit` on Windows);
  3. `rename` (`MoveFileExA` with replace on Windows);
  4. remove the temporary file on any failure.
- POSIX temporary files are created with mode `0600`, and the rename preserves
  that mode, so the configuration is never group- or world-readable.
- No temporary file survives a failed save.

---

## 9. `platform/ipc.c`

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
- An error reply from the daemon makes the client print the message to `stderr` and exit 2.
- IPC failure returns exit code 3.
- `--foreground` keeps the process attached to the terminal and logs lifecycle events to `stderr`.

---

## 10. `ui/cli.c`

Turns `argv` into `wn_cmd` through `core/command.c` and formats output. Flags and
precedence rules live in PRD section 7; do not duplicate them here.

- A prescan honours `--help` and `--version` before the full parse, so they win
  over an otherwise invalid argument list. The prescan skips the value of a
  value-taking option (`--noise help` is a bad noise name, not a help request).
- `--status` prints exactly the six lines from PRD section 6, FR-08, in order,
  followed by one newline each. At M5 there is no daemon, so it always reports
  the no-daemon form: `playing=0`, `stop_after=off`, and the persisted settings.
  M7 inserts the IPC query ahead of this fallback.
- Setting commands (`noise`, `volume`, `mono`, `stereo`) load the file, apply in
  written order, and save once. `--portable` changes the path, nothing else.
- `--render` neither reads nor writes configuration.
- Playback and timer commands are session state. Until M7 gives them a daemon
  to talk to, they exit 1 with a diagnostic; they never write configuration.
- Errors go to `stderr` prefixed with `tinyambient: `.
- Exit codes: 0 success, 1 runtime failure, 2 usage error, 3 IPC failure.

---

## 11. `main.c` and `--render`

`main.c` only wires things together:

```
parse argv -> config load -> decide mode
  +- render   -> in-memory engine -> writer -> exit
  +- status   -> IPC if a daemon exists, else config -> exit
  +- content  -> IPC if a daemon exists, else start daemon -> exit
  +- no args  -> start daemon if absent, then exit
```

`--render` starts no daemon, opens no device, and reads and writes no config. It
uses the same `wn_engine_render` with an explicit seed so output matches
playback. Noise, volume, and mono are taken from the command line, never from
the saved configuration: white, 100, and stereo when the flags are absent. That
keeps a render reproducible from the arguments alone (NFR-06).

Block size is 512 frames, matching the planned device period, so the render loop and the audio callback have the same shape.

WAV writer: 44-byte header, `float32`, 2 channels, 48 kHz. Any other extension produces raw little-endian interleaved float32. Expected data size: `384000 * seconds` bytes.

The header is built in `ui/cli.c`, not `core/`: it is file I/O, and `core/` stays
I/O-free. WAVE_FORMAT_IEEE_FLOAT (3) means the payload is byte-identical to a raw
render of the same seed and type, with no sample conversion.

The data size is known before writing starts, so the header needs no seek-back.
At the 3600 s limit the data chunk is 1,382,400,000 bytes, which still fits the
32-bit RIFF size field; total file size is `384000 * seconds + 44`.

---

## 12. Invariants to test

1. Same seed and parameters produce byte-identical output.
2. No sample outside `[-1.0, 1.0]`.
3. A type change produces no step above 0.25 between consecutive samples.
4. Slopes and RMS stay within the tolerances of PRD section 6, FR-02.
5. A config write killed mid-flight leaves a parseable file.
6. A second invocation creates no second daemon and returns in under 100 ms.
7. `--status` follows the format and values of FR-08, with and without a daemon.
8. Malformed config values fall back to defaults with a warning and no abort.
9. Unknown keys and sections survive a rewrite.
10. `--status` and `--render` never write configuration; usage errors never
    rewrite it either.
11. `--portable` writes `config.ini` next to the executable and ignores
    `XDG_CONFIG_HOME`/`APPDATA`.
12. A saved config is mode `0600` and leaves no temporary file behind.
