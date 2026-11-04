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
| `mono` | main | audio |
| `fade_in_req`, `fade_out_ms`, `stop_cancel_req` | main | audio (consumed with an exchange) |
| `stop_at_ms` (monotonic) | main | main |
| filter phases, RNG state, fade envelopes | audio | audio |
| config on disk | main | main |

The audio thread never reads the stop-after deadline. The main thread checks it
and requests a stop fade; the engine clears `playing` itself when the fade lands,
so the daemon observes the pause without inspecting the envelope.

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
  `--status` also rejects `--foreground`, because that would start a session;
  `--portable` stays legal next to `--status`, since it only selects which file
  is read.
- Validates syntax plus the ranges it can check locally: `volume` 0–100,
  `stop-after` 1–10080 or `off`, `seconds` 1–3600, `seed` 0–2147483647, and
  the three noise names.
- Repeating a single-valued option (`--noise`, `--volume`, `--stop-after`,
  `--render`, `--seconds`, `--seed`) is an error. Play/pause/toggle and
  mono/stereo may appear together; the last one wins, as rule 2 requires.
- A `--render` value starting with `-` is rejected. Every other value option
  rejects flag-like values through validation; render holds the same line
  instead of silently writing a file named `--help`.
- `wn_cmd_takes_value` reports which command names consume the next token.
  `ui/cli.c` uses it for the rule-6 prescan so the value-skipping rules cannot
  drift from the parser.
- `wn_cmd_parse_line` splits a mutable buffer in place. `svalue` may point into
  that buffer, so the caller keeps it alive while applying the commands.

The mapping to IPC text (`noise pink`, `volume 40`) and the inverse parser live
here, so there is only one semantics. `wn_engine_command` uses the line parser
rather than re-implementing a command subset. `wn_cmd_format_ipc_line` is the
other direction: it formats parsed commands back into one line, skipping the
local-only flags (`--portable`, `--foreground`, `--help`, `--version`) and the
render options, which the parser already forbids next to daemon commands.

`wn_engine_command` returns 0 on success and non-zero for an unknown or invalid command, so the IPC layer can answer with an error instead of `ok`. `wn_engine_apply_cmd` applies one already-parsed command, which is what the daemon uses after validating a whole forwarded line.

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
- Unknown keys and comments survive a rewrite. Lines before the first section
  header are emitted first, above `[general]` (the parser ignores keys there,
  so they stay inert). Comments and unknown keys under `[general]` are emitted
  after the four canonical keys, before any other section, so they cannot drift
  into the section that follows them. Other sections are preserved last, with
  their headers, comments, and keys. Blank and malformed lines are normalized
  away.
- Malformed values fall back to the default for that key with a warning; they
  never abort startup. The first warning is kept.
- `volume` is range-checked. `fade_ms` is clamped to 0-2000 on read.
- `wn_config_format` uses `snprintf` semantics: it returns the byte count the
  full text needs, so a value `>= cap` signals truncation. With `out == NULL`
  and `cap == 0` it only measures.

The atomic write (temp file + `fsync` + `rename` + directory `fsync`) lives in
the layer that owns I/O, not here; see `platform/config_file.c` below.

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

Owns observable state and applies the fades. It is the only thing the callback
calls.

Cross-thread state (AGENTS.md 1.2), all `_Atomic int` and read once per buffer:

- `type` — requested noise type. `wn_engine_noise` reports it immediately, so
  status shows the destination of a crossfade, not its midpoint.
- `playing`.
- `volume_milli` — 0..1000, per-mille of full scale, so `_Atomic float` is never
  needed. The value is always a multiple of ten, so the 0..100 reading is exact.
- `mono` — a parameter like the others; a plain `int` shared with the audio
  thread would be a data race.
- `fade_in_req` / `fade_out_ms` / `stop_cancel_req` — one-shot requests the audio
  thread consumes with an atomic exchange. `wn_engine_fade_in` is the
  device-resume path (FR-16); `wn_engine_fade_out` is the session timer's expiry
  (FR-05); `stop_cancel_req` is set by `play`/`toggle` so a resume cancels a
  pending auto-pause even though the envelope target has not changed.

Audio-thread-only state: two `wn_noise` generators (left/right), a second pair
that holds the outgoing noise during a crossfade, and two independent,
piecewise-linear progressions:

1. **Gain envelope.** One envelope covers play, pause, and volume: the target is
   `playing ? volume_milli : 0`, and any change restarts a linear fade from the
   current value over `WN_FADE_MS` (20 ms). Linear ramps need no `sin`/`exp`, and
   the envelope snaps to its exact target when the fade lands, so it never
   lingers in denormal range. The engine starts at zero and fades in, which is
   the "no clicks on start" goal; `wn_engine_fade_in` re-triggers the same ramp
   after a device restart. Session setup replaces the length of that first ramp
   with `WN_OPENING_FADE_MS` (1 s) through `wn_engine_set_opening_fade`, so a
   fresh daemon swells into the saved level instead of stepping into it; the
   setter only touches the envelope before the audio thread exists. `--render`
   never calls it, so offline output keeps the 20 ms fade and stays identical.
   A stop fade is the same envelope with a long duration; when it lands the
   audio thread stores `playing = 0`, so the daemon observes the pause without
   inspecting the envelope. A playing or volume change during the stop fade
   cancels it: `play` and `toggle` raise the one-shot `stop_cancel_req`, and a
   target change is detected directly, because either means the user is still
   working. Expiry is authoritative when both land in the same buffer: the
   fade-out request is consumed first, so the session still pauses.
2. **Noise crossfade.** On a type change the audio thread copies the current
   generators to `fade_gen`, then calls `wn_noise_set_type` on the live ones
   (filters reset, RNG preserved). The copy continues the old sequence from
   exactly the next sample, so the mix `old + (new - old) * t` starts on the
   audible sample and not on a step. Both sides advance during the fade, `t`
   reaches 1 over `fade_ms`, and the mix stays within `[-1, 1]` because it
   interpolates two bounded signals. A second change during a running crossfade
   is deferred until it ends, so the mix always has one continuous path. While
   the envelope is at zero the switch is instant: a crossfade nobody hears only
   costs CPU.

Other behavior:

- `volume 0` produces exact silence with `playing=1`; a paused or muted engine
  skips generation, writes a fully zeroed buffer, and still advances its
  bookkeeping and the frame counter.
- Mono duplicates the mixed left sample, so both channels stay bit-identical
  (FR-13).
- `wn_engine_command` parses a line with `wn_cmd_parse_line` and applies every
  command only after the whole line is known to be supported, so an invalid tail
  cannot leave the engine half-applied. It accepts `noise`, `volume`, `play`,
  `pause`, `toggle`, `mono`, and `stereo`. Status, quit, and stop-after belong to
  the daemon loop, not the engine. `wn_engine_apply_cmd` is the same application
  step for commands the daemon has already parsed and validated.
- The output buffer is always fully written, including during silence.
- `wn_engine_noise`, `wn_engine_volume`, `wn_engine_is_mono`, and
  `wn_engine_is_playing` expose the state for status output. The stop-after
  deadline stays on the main thread and is not engine state.

`wn_engine_set_seed` reseeds both generators (streams stay 0 and 1). Only
`--render` uses it, because there the output must depend on the seed alone
(NFR-06). Playback uses the fixed default seed: noise needs no variety. It is
the one function that touches the generators from outside the audio thread, and
it runs before playback starts.

Volume maps linearly, `0..100 -> 0.0..1.0`, multiplied by the calibrated type
gain. Mono renders one generator and duplicates the sample, so both channels
stay bit-identical (FR-13).

### `core/status.c`

Formats the six lines of FR-08 from plain values: playing, noise type, volume,
mono, and remaining timer seconds (`< 0` means `off`). The daemon and the
no-daemon fallback in `ui/cli.c` both call it, so the two forms cannot drift.
The version string comes from `TA_VERSION`, defined `PUBLIC` on `ta_core` by
CMake so the daemon reports the same value as `--version`.

### `core/text.c`

Two helpers every layer uses on command text: `wn_text_equal_ci` (ASCII-only
case folding, so a locale cannot change how a noise name or a `.wav` extension
compares) and `wn_text_set`/`wn_text_vset` (bounded `vsnprintf` into a caller
buffer). They live in `core/` because `ui/` may depend on `core/` but not the
reverse, and they exist so the parser, the config reader, and the CLI cannot
grow three subtly different copies of the same comparison or the same
message-formatting guard.

---

## 6. `audio/device.c`

The only file that includes `miniaudio.h` with `MINIAUDIO_IMPLEMENTATION`.

- `ma_device` with `f32`, 2 channels, 48 kHz (`ma_device_config_init(ma_device_type_playback)`).
- Period of 512 frames.
- Callback: `ma_device *` to `wn_engine *` via `pUserData`; calls `wn_engine_render` and returns.
- No allocation, locks, logging, or syscalls inside the callback.
- The device and the forced context are static, never heap-allocated: a fixed address cannot be freed under a running callback.
- `notificationCallback` runs on a miniaudio-owned thread and only stores `_Atomic int restart_requested`, for `stopped`, `rerouted`, and `interruption_began`. A `stopping` guard (`_Atomic`, relaxed — only the value matters, and a plain int would be a cross-thread data race) keeps our own `ma_device_uninit` from arming it.
- `wn_audio_needs_restart` exposes the flag. The backoff loop lives in
  `platform/run.c` (M6): 100 ms, 500 ms, 2 s, then every 5 s, retried for as
  long as the daemon runs.
- Parameters and `playing` survive a restart, and a resumed device receives
  `wn_engine_fade_in`, so restarting is click-free (FR-16).
- From M6 the shipped binary calls `wn_audio_start` from the run loop, so
  miniaudio and the ALSA/Pulse backends are part of the product binary for the
  first time. `tests/play_check.c` remains the direct harness for the device
  path.
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
- `wn_paths_ipc_address` resolves `$XDG_RUNTIME_DIR/tinyambient.sock`, falling
  back to `$TMPDIR/tinyambient-<uid>/tinyambient.sock` (`/tmp` when `TMPDIR` is
  unset). `wn_paths_make_runtime_dir` creates that fallback directory with mode
  `0700` and rejects an existing one unless it is a directory owned by the
  current user, so a stale socket or a hostile pre-created path cannot redirect
  or block the daemon. With `XDG_RUNTIME_DIR` set the session manager owns the
  directory and nothing is created. The socket itself is still mode `0600`.
- On Windows it resolves `\\.\pipe\tinyambient-<sid>` from the user SID;
  `wn_paths_user_sid` returns the same SID for the pipe's DACL. `TINYAMBIENT_IPC`
  replaces the whole name, because Windows has no per-test runtime directory and
  the tests must not reach a real session.

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
  4. on POSIX, `fsync` the parent directory, so the rename itself is durable.
     `EINVAL` and `ENOTSUP` are ignored: a filesystem that cannot sync a
     directory should not fail an otherwise successful save;
  5. remove the temporary file on any failure.
- POSIX temporary files are created with mode `0600`, and the rename preserves
  that mode, so the configuration is never group- or world-readable.
- No temporary file survives a failed save.

---

## 9. `platform/run.c`

The daemon main loop: device supervision, the session timer, IPC command
dispatch, and quit.

- `wn_session` carries everything the loop mutates: the current persisted
  configuration and its original text, the config path, the portable flag, the
  initial timer, the readiness pipe, the verbose flag, and the listening socket.
  It is also what makes the loop testable without a socket (`tests/session_check.c`
  passes a zeroed session).
- Starts the device before entering the loop and then reports readiness to the
  process that forked it. A device missing at startup is a runtime failure (exit
  1, clear message); a device lost later is retried.
- Wakes every 50 ms to check the quit flag, the restart flag, the stop-after
  deadline, and IPC. When a socket is present, `poll` with the same 50 ms timeout
  is the sleep, so IPC and supervision share one cadence. That is what keeps a
  forwarded command inside the FR-07 budget of 100 ms.
- **Restart backoff (FR-16):** 100 ms, 500 ms, 2 s, then every 5 s, forever.
  The loop stops the dead device first, then reintroduces it with
  `wn_audio_start`; on success it calls `wn_engine_fade_in`, so the resume
  starts from silence. Engine state is untouched by any of this.
- **Stop-after (FR-05):** the deadline is monotonic milliseconds computed at
  start from the CLI value or replaced by a forwarded `stop-after`. At expiry the
  loop calls `wn_engine_fade_out`, the engine fades and pauses itself, and the
  loop clears the deadline immediately, so status reports `stop_after=off` from
  the moment the fade starts. The timer is session state; it is never written to
  configuration.
- **Command dispatch:** a forwarded line is parsed with the same
  `wn_cmd_parse_line`, validated as a whole, then persisted settings are saved
  once, and only after a successful save are the atomics written. A failed save
  therefore leaves the file and the audio state unchanged and replies
  `error 1 <message>`. Unsupported kinds reply `error 2 ...`.
- **Quit** stops accepting, unlinks the socket, and releases the lock before
  sending the final `ok`, so a client that sees the reply can neither observe a
  stale socket nor collide with this daemon while it stops the audio device.
- SIGINT, SIGTERM, and SIGPIPE handling: the first two set an atomic quit flag
  from the handler (`sigaction` with `SA_RESETHAND` on POSIX, `signal()` on
  Windows, plus a lock-free store in both cases, all async-signal-safe), so a
  second Ctrl+C restores the default terminate action for a wedged loop; the
  third is ignored so a vanished client cannot kill the daemon. Any thread may
  also call `wn_platform_request_quit`, which is how the test harness stops the
  loop.
- `stop_fade_ms` is a parameter so a test build can shorten the product's 20 s
  fade without a second code path.
- `wn_platform_detach` forks once, calls `setsid`, redirects the standard
  streams to `/dev/null`, and keeps a readiness pipe open. The parent polls the
  pipe for five seconds and exits 0 only after the child reports that the device
  is up; otherwise it kills the child and exits 1. A single fork is enough
  because the daemon never reopens a terminal.
- On Windows there is no fork. The parent creates an inheritable manual-reset
  event, starts a detached copy of itself (`DETACHED_PROCESS`) with
  `--daemon-child <handle>` in front of the original arguments, and waits on the
  event and the child's process handle for five seconds. The child signals the
  event once the device is up; if it exits first, the parent reports the same
  failure a POSIX parent sees on EOF. The child re-runs the arguments rather
  than inheriting memory, which is safe because every step of session setup is
  deterministic from the arguments and the saved configuration.

---

## 10. `platform/ipc.c`

| OS | Address | Permissions |
|---|---|---|
| Linux | `$XDG_RUNTIME_DIR/tinyambient.sock` (fallback `$TMPDIR/tinyambient-<uid>/tinyambient.sock` in a `0700` directory) | `0600` |
| Windows | `\\.\pipe\tinyambient-<sid>` | DACL grants the current user only |

Startup sequence for any invocation:

1. try to connect to the address;
2. if it answers, send the command, read the reply, print it, exit;
3. if it is absent, become the daemon.

Single instance:

- The daemon opens `<socket>.lock` and takes an exclusive non-blocking `flock`
  before it unlinks a stale socket and calls `bind`. A second start fails with
  "another daemon is already running" instead of stealing the path.
- `flock` is attached to the open file description, so it survives the fork into
  the daemon. A POSIX record lock would not: those belong to the process and are
  dropped when the parent exits.
- The socket path is recorded in the handle only after the lock is held. A
  failed start must never unlink a path that belongs to a live daemon; before
  the lock, `unlink` is not ours to call.
- The socket is chmodded to `0600` explicitly, so the umask cannot widen it.
- On quit, the listener is closed, the socket removed, and the lock released
  while the current client is still open; the last reply follows. The next start
  therefore sees a clean slate, and a start that races the teardown cannot be
  refused while this daemon is still stopping the audio device.

Protocol:

- One command per line, newline-terminated, at most `WN_LINE_MAX` (256) bytes.
- Replies are `ok`, the six `key=value` status lines, or
  `error <exit-code> <message>`. The client prints non-`ok` replies to stdout,
  prints an error message to stderr, and exits with the code in the reply. A
  failed daemon-side config write therefore stays exit 1 (FR-14) and an invalid
  command stays exit 2.
- The accepted client has a 200 ms receive timeout, so a connection that stalls
  cannot block the loop. The client has a 2 s send/receive timeout.
- A connection that closes without a reply is retried twice with a 20 ms pause
  and then treated as absent. This covers the window in which a `quit` and a
  fresh start overlap: the second invocation must see either the old daemon or a
  clean "not there", never a spurious failure.

Windows: the same contract over a byte-mode named pipe. Differences that matter:

- `CreateNamedPipe` with `nMaxInstances = 1` is the single-instance guard; a
  second daemon fails with `ERROR_ACCESS_DENIED`, which maps to the same
  "another daemon is already running" message. There is no lock file and no
  stale path to clean up: the pipe disappears when its last handle closes.
- The pipe is created with `FILE_FLAG_OVERLAPPED`, so the daemon can wait for a
  client for one tick without blocking the timer or the device supervision.
  `ConnectNamedPipe` stays pending across ticks; `ERROR_PIPE_CONNECTED` covers
  the client that arrived between creation and the call.
- The DACL grants `GENERIC_ALL` to the current user SID and nothing else, which
  is the named-pipe equivalent of the socket's `0600`. The ACL is sized from
  `GetLengthSid` rather than a fixed buffer: accounts with many subauthorities
  would otherwise fail the pipe creation on a sizing problem, not a permissions
  one.
- The instance is the connection, so a reply must be written and flushed before
  the instance is disconnected; `wn_ipc_server_reply_and_release` keeps that
  ordering per platform. The client reads until the newline and treats a broken
  pipe with no bytes as "the daemon was leaving" and retries.
- The client maps `ERROR_PIPE_BUSY` (a live daemon between clients) to
  `WaitNamedPipe` plus a retry, and `ERROR_FILE_NOT_FOUND` to `WN_IPC_ABSENT`.

---

## 11. `ui/cli.c`

Turns `argv` into `wn_cmd` through `core/command.c` and formats output. Flags and
precedence rules live in PRD section 7; do not duplicate them here.

- A prescan honours `--help` and `--version` before the full parse, so they win
  over an otherwise invalid argument list. The prescan skips the value of a
  value-taking option (`--noise help` is a bad noise name, not a help request).
  The skip list is `wn_cmd_takes_value` from `core/command.c`, so it cannot
  drift from the parser. A value is skipped only for a command the parser
  recognizes; after an unrecognized token, help and version still win, exactly
  as they do after `--bogus` (rule 6).
- `--status` queries the daemon first and prints its six lines. With no daemon
  it prints the no-daemon form from the same `core/status.c` formatter:
  `playing=0`, `stop_after=off`, and the persisted settings. It never starts a
  daemon (FR-08).
- Content commands are formatted with `wn_cmd_format_ipc_line` and forwarded.
  `ok` is silent; status is not forwardable here because the parser rejects it
  next to content commands. A daemon error reply is printed and mapped to its
  exit code. If no daemon answers, the invocation starts one (rule 7).
- With no forwardable command (a bare start, or `--portable` alone), the client
  only checks whether a daemon already answers. If one does, the invocation is a
  no-op; otherwise it starts the daemon. `quit` with nothing running exits 0
  instead of starting a daemon just to stop it.
- `--foreground` takes the single-instance address before anything else, so a
  second foreground start fails with exit 3 instead of persisting settings the
  running daemon never sees; `quit` is forwarded to a live daemon first, and is
  a no-op when none runs. With the address owned, it prepares the session
  exactly like the background path, then runs `wn_platform_run` without forking
  until SIGINT, SIGTERM, or a forwarded `quit`.
- The background path prepares the same state in the client before forking, so
  the daemon starts where a foreground run would: the setting commands are
  applied and saved once, the engine is created, the playback commands are
  applied, and the timer is armed. The parent waits for the readiness byte, so a
  first start with no audio device still exits 1 with a message.
- Windows cannot fork, so the same preparation runs twice: once in the parent
  (which then exits) and once in the detached child, which re-parses the
  original arguments behind `--daemon-child`. The repetition is safe because
  the steps are deterministic; `toggle` is the only one that depends on prior
  state, and both copies start from the saved configuration.
- Setting commands (`noise`, `volume`, `mono`, `stereo`) persist. When the daemon
  applies them, it writes the file itself; playback and timer commands never
  write. `--portable` changes the path, nothing else. With a daemon already
  running, `--portable --status` reports that daemon's state: the flag selects a
  file, and a live daemon has already chosen its own.
- `--render` neither reads nor writes configuration and never contacts a daemon.
- Errors go to `stderr` prefixed with `tinyambient: `.
- Exit codes: 0 success, 1 runtime failure, 2 usage error, 3 IPC failure.

---

## 12. `main.c` and `--render`

`main.c` only wires things together:

```
parse argv -> config load -> decide mode
  +- render     -> in-memory engine -> writer -> exit
  +- status     -> IPC if a daemon exists, else config -> exit
  +- foreground -> config + engine -> wn_platform_run -> exit
  +- content    -> IPC if a daemon exists, else start daemon -> exit
  +- no args    -> start daemon if absent, then exit
```

All four non-render modes are live as of M7. `--foreground` runs the same loop
without detaching.

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

## 13. Invariants to test

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
13. The first sample after an engine start or a device restart is exactly zero;
    a stop fade ends with `playing=0` and exact silence.
14. A forwarded setting is persisted by the daemon; a forwarded playback or
    timer command never writes the file.
15. A daemon-side invalid command replies `error 2 ...`; a failed config write
    replies `error 1 ...` and changes neither the file nor the engine.
16. `quit` removes the socket before the client sees the reply, and the daemon
    process is gone shortly after.
17. Two starts in a row leave exactly one daemon.
