# tinyAmbient — Agent Context

**State:** M8 complete; v1 is done. CMake build, layer headers, size gate, resource gate, CI skeleton, formatting gate, PCG32, white/pink/brown noise with calibrated gains, `--render` to raw float32 or WAV, miniaudio device and callback, full command parser, engine command handling, INI config with atomic writes, portable mode, status output, parameter atomics, 20 ms play/pause/volume fades, `fade_ms` noise crossfades, the stop-after timer with its 20 s fade-out, the device restart backoff, `--foreground`, IPC on both platforms (Unix socket on Linux, per-user named pipe on Windows), single-instance enforcement, the detached background daemon, command forwarding, daemon-side persistence, and the corresponding CLI, config, engine, session, status, and IPC tests. Windows runs the same daemon, IPC, and test suite as Linux. v1.0.1 lowers the saved default volume to 20 and opens each session over 1 s. v1.0.2 makes `play` cancel a running stop fade, fixes the `--foreground` ordering (quit reaches the daemon; the single-instance address is taken before anything is persisted), and gives a second Ctrl+C the default terminate action.
**Reference branch:** `main`.

| Document | Answers | Authority |
|---|---|---|
| `docs/PRD.md` | What is built: scope, CLI, config, acceptance criteria, milestones | Wins on scope and behavior |
| `docs/ARCHITECTURE.md` | Per-component detail: internal state, filters, buffers, errors | Implementation detail |
| `AGENTS.md` (this file) | How it is built: constraints, layers, build, working rules | — |

If the PRD and this file disagree, the PRD wins and this file is fixed in the same diff.

## TL;DR

- **C11 + miniaudio.** One binary per OS, no bundled runtime, no GUI toolkit.
- **Linux (Fedora KDE) and Windows 10/11.** macOS is out of v1.
- **Three noise types:** white, pink, brown. 48 kHz, float32, stereo interleaved.
- **CLI + daemon** with single-instance enforcement. No tray, no autostart, no network.
- **`core/` has no OS APIs.** Audio callback does no allocation, locking, or I/O.
- **Parameters travel through atomics**, read once per buffer.
- **Headless tests:** `--render` plus FFT slope analysis.
- **Binary size is a requirement**, measured at every milestone.

---

## 1. Hard constraints

1. The audio callback does not allocate, block, log, or perform I/O.
2. Parameters cross the thread boundary through integer atomics: `_Atomic int` for type, playing, volume_milli, and mono, plus the one-shot requests (`fade_in_req`, `fade_out_ms`, `stop_cancel_req`). `volume_milli` avoids `_Atomic float`. The stop-after deadline is main-thread state and needs no atomic.
3. `core/` includes only `libc`, `libm`, and C11 headers (`stdint`, `stdbool`, `stddef`, `stdatomic`). No OS headers.
4. Allowed external dependencies: libc/libm; ALSA and Pulse loaded with `dlopen`; a pinned, vendored miniaudio. Anything else needs written justification and a measured size delta.
5. `miniaudio` is not upgraded without testing audio and measuring the binary.
6. Binary size is checked every milestone (`ls -l`, `size`). Linux limit: **<= 150 KB**. Windows limit: **<= 300 KB**.
7. No network sockets in any mode.
8. `-Wall -Wextra -Wpedantic` produces zero warnings in own code.
9. Documentation, comments, and commit messages are written in English.

---

## 2. Architecture

```
src/
  main.c                 # argv -> command_parse -> engine/IPC; wiring only
  core/                  # pure C11, zero OS calls
    noise.c  noise.h     # RNG, filters, per-type gain
    engine.c engine.h    # state, fades, stop-after
    config.c config.h    # INI parse/format, no I/O
    command.c command.h  # single parser for argv and IPC
    status.c status.h    # the six-line status formatter
    text.c text.h        # case-folding and bounded message formatting
  audio/
    device.c             # only file that includes miniaudio
  platform/
    paths.c  paths.h     # per-OS config directories, IPC address
    config_file.c .h     # load, atomic save (temp + fsync + rename)
    run.c                # daemon loop, detach, command dispatch
    ipc.c    ipc.h       # Unix socket / named pipe
  ui/
    cli.c                # flags, output, exit codes
tests/
  test_noise.c
  test_config.c
  test_command.c
  test_engine.c
  test_status.c
  play_check.c           # device harness, runs under the null backend
  session_check.c        # run-loop harness: stop-after, quit (POSIX only)
  ipc_check.cmake        # daemon, forwarding, single instance (POSIX)
  ipc_check_win.cmake    # the same contract over a named pipe (Windows)
  resource_check.cmake   # peak RSS and CPU budget (Linux, Release)
  check_spectrum.py      # slopes/RMS over raw or WAV
third_party/
  miniaudio.h
docs/
  PRD.md  ARCHITECTURE.md
```

Tests grow per milestone; the list above is the M8 target.

### Layer rules

| Layer | May depend on |
|---|---|
| `core/` | libc, libm, C11 headers |
| `audio/` | `core/`, miniaudio |
| `platform/` | libc and OS APIs (the only layer that uses them), `core/`, `audio/` |
| `ui/` | `core/`, `platform/` |
| `main.c` | all layers; no DSP, no UI logic |

### Boundary interfaces

```c
/* core/engine.h */
typedef struct wn_engine wn_engine;

wn_engine *wn_engine_create(const wn_config *cfg);
void       wn_engine_destroy(wn_engine *e);
int        wn_engine_command(wn_engine *e, const char *line);
int        wn_engine_apply_cmd(wn_engine *e, const wn_cmd *cmd);
void       wn_engine_render(wn_engine *e, float *out, unsigned frames);
int        wn_engine_is_playing(const wn_engine *e);

/* platform.h */
int  wn_platform_run(wn_engine *e, wn_session *session);
void wn_platform_request_quit(void);
int  wn_platform_ipc_send(const char *line, char *reply, size_t cap);
int  wn_platform_detach(int *ready_fd, char *err, size_t err_cap);
```

`wn_session` carries the mutable persisted configuration, the timer settings,
the readiness pipe, and the listening socket, so the loop can apply and save a
forwarded setting without reaching back into `ui/`.

### Data flow

```
argv --+
       +--> command_parse --> IPC line --> daemon loop --> engine atomics
       |                                     |                 |
       |                                     +--> config save  v
       |                                                   audio callback
       +--> foreground ----------------------------> daemon loop

client --connect--> $XDG_RUNTIME_DIR/tinyambient.sock
  absent  --> start daemon (fork, bind, readiness pipe)
  present --> send one line, print the reply, exit

engine_render:
  read atomics once per buffer
  apply fade and volume
  generate samples per channel
  write interleaved stereo, with soft clip
```

---

## 3. DSP

- 48 kHz, float32, stereo interleaved. Two independent generators by default; `--mono` generates one and duplicates it.
- **PCG32** RNG, one state per channel, explicit seed. Never `rand()`.
- Gains calibrated to **-18 dBFS RMS**, per type, never by ear.

| Type | Method | Target slope |
|---|---|---|
| `white` | uniform `[-1,1)` | 0 +/- 0.5 dB/oct |
| `pink` | Paul Kellet, 7 poles | -3 +/- 0.5 dB/oct |
| `brown` | integrator with leak (`a ~= 0.9995`) + DC blocker | -6 +/- 1.0 dB/oct |

Rules:

- Slope measured in third-octave bands from 40 Hz to 12 kHz.
- No `sin`, `exp`, or `log` per sample; coefficients are computed at init.
- Fades are linear and frame-based: 20 ms for play, pause, volume, and device resume; 1 s for the opening ramp at session start; `fade_ms` for a type crossfade; 20 s at stop-after expiry. A linear ramp needs no transcendental function and snaps to its exact target.
- Soft clip is a safety net; the gain should prevent clipping in the first place.
- Denormals: generation is skipped while the envelope is at zero, and fades snap to their exact target, so filters never process denormal input and the envelope never lingers near it. No flush-to-zero setup is needed.
- Blue, gray, and green are out of v1. Do not implement them.

---

## 4. Build and size budget

### Base flags (GCC/Clang)

```
-Os -flto=auto -ffunction-sections -fdata-sections
-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
-Wl,--gc-sections -s
```

Keep an `-O0 -g` build for debugging: `-flto -s` hides symbols.

### miniaudio

Define before `MINIAUDIO_IMPLEMENTATION`:

```c
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_MP3
#define MA_NO_FLAC
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
```

Runtime linking of ALSA/Pulse is the default; `ldd` must not list `libasound` or `libpulse`.

### Reference measurements (Fedora 44, GCC 16.2, x86_64)

| Variant | Size |
|---|---|
| DSP core (object) | 1.2 KB |
| CLI + DSP, libc only | 11 KB |
| CLI + miniaudio, no backends | 65 KB |
| CLI + miniaudio + ALSA + Pulse | **98 KB** |
| ALSA only / Pulse only | 77 / 81 KB |
| Windows + `/MT` | 292 KB measured (299,520 bytes), WASAPI only |

A regression above ~20 KB means something large slipped in.

### Milestone measurements

Recorded per AGENTS.md rule 6. Enforced by the `size-check` target and by the `size` ctest entry.

| Milestone | Variant | Size | Budget |
|---|---|---|---|
| M0 | Linux, GCC 16.2.1, `-Os -flto -s`, miniaudio compiled but unreferenced | 10 KB | 150 KB |
| M1 | M0 + PCG32, white noise, `--render`, parser, tests | 11 KB | 150 KB |
| M2 | M1 + pink and brown generators, `--noise`, `check_spectrum.py` | 11 KB | 150 KB |
| M3 | M2 + WAV container and its header checks | 11 KB | 150 KB |
| M4 | M3 + miniaudio device and callback (product binary, unreachable until M7) | 15 KB | 150 KB |
| M4 | `play_check` harness, which actually opens the device | 91 KB | — |
| M5 | M4 + full parser, engine commands, INI config, atomic writes, portable mode, status | 23 KB | 150 KB |
| M6 | M5 + parameter atomics, fades, crossfades, stop-after timer, device restart backoff, `--foreground` | 108 KB | 150 KB |
| M7 | M6 + Unix socket, single instance, detached daemon, forwarding, daemon-side persistence | 116 KB | 150 KB |
| M8 | M7 + Windows named pipe, per-user ACL, detached daemon, Windows IPC test | 116 KB Linux | 150 KB |
| M8 | Windows 10/11, MSVC 19, `/MT /O1 /GL`, WASAPI only | 292 KB (299,520 bytes) | 300 KB |
| v1.0.0 | M8 source, built by CI on Ubuntu 24.04 with GCC 13 | 126 KB (129,400 bytes) | 150 KB |
| v1.0.1 | v1.0.0 + opening ramp, default volume 20; Fedora 44, GCC 16.2.1 | 116 KB (118,472 bytes) | 150 KB |
| v1.0.2 | v1.0.1 + stop-fade cancel, `--foreground` ordering, `SA_RESETHAND`; Fedora 44, GCC 16.2.1 | 116 KB (118,544 bytes) | 150 KB |

M2 note: the file size did not move, but `.text` grew from 4785 to 5709 bytes (+924). The ELF header and section alignment swallowed the difference at this size.

M5 note: the file grew from 15,248 to 23,608 bytes (+8,360). `.text` is 15,529 bytes, `.data` 800, `.bss` 48. The cost is the full command parser, the INI parser/formatter, paths, the atomic writer, and the CLI status/persistence paths. The number is still an order of magnitude under the budget.

M6 note: the file grew from 23,608 to 110,120 bytes (+84.5 KB). This is the milestone where the product binary starts calling `wn_audio_start`, so miniaudio and the ALSA/Pulse backends stop being garbage-collected; the M4 probe already measured about 91 KB of `.text` for that path. The run loop, the fades, and `--foreground` add roughly 10 KB on top. `.text` is 101,307 bytes, `.data` 1,228, `.bss` 4,896. `ldd` still lists neither `libasound` nor `libpulse`.

M7 note: the file grew from 110,120 to 118,472 bytes (+8,352). `.text` is 108,037 bytes, `.data` 1,380, `.bss` 4,896. The cost is the socket server and client, the flock-based single-instance guard, the detach path with its readiness pipe, and the daemon-side command dispatch and persistence. `fork`, `poll`, and the socket calls are libc, so nothing new is linked; `ldd` still lists neither `libasound` nor `libpulse`.

M8 note: the Linux file did not move (118,472 bytes); `.text` grew by 180 bytes to 108,217 because the Windows branch is preprocessed out and only the `reply_and_release` path is new. The first Windows build measured **318,976 bytes (311 KB)**, 12 KB over budget: the named pipe, the SID/DACL code, the detach path, and `advapi32` were enough to cross a line the M7 binary was already close to. Compiling out DirectSound and WinMM (`MA_NO_DSOUND`, `MA_NO_WINMM`) brought it to **299,520 bytes (292 KB)**, 8 KB under. Windows 10/11 needs WASAPI; the two legacy backends cost more than the whole IPC layer. The Windows job prints the size before the gate so the number is in the log.

v1.0.0 note: the published Linux archive measures **129,400 bytes (126 KB)**. It is built by CI on Ubuntu 24.04 with GCC 13, not by the Fedora 44 toolchain the milestone rows record, and the 11 KB gap is compiler and linker version rather than code. The Windows payload is unchanged at 299,520 bytes. Both stay under the gate; the README publishes the archive size and names the local Fedora figure next to it, so a reader who downloads the binary sees the number that describes what they got.

v1.0.1 note: the file did not move (118,472 bytes); `.text` grew by 89 bytes to 108,306 for the opening-fade setter and its one call site. A 10 s window on the same machine measured 6.8 MB RSS steady, 9.0 MB peak, and 1.00% of one core, inside the published band. The Windows payload is unchanged.

v1.0.2 note: the file grew by 72 bytes to 118,544; `.text` is 108,696 (+390) and `.data` 1,396 (+16). The cost is the third one-shot atomic and its consume branch in the render loop (the stop-fade cancel), the foreground `quit`/bind reorder in the CLI, and `sigaction` with `SA_RESETHAND` in the run loop. The Windows payload is unchanged: the edits are platform-neutral and one branch is Windows-only but smaller than the `signal()` calls it replaces.

M0 note: nothing calls into miniaudio yet, so `--gc-sections` removes it entirely. A probe that references `ma_context_init` with the ALSA and PulseAudio backends measures **81 KB**, which is the realistic figure the binary will reach at M4 once the device is actually opened.

### Resource measurements

Recorded per rule 6. Enforced by the `resource` test on Linux, in Release only.

| Variant | RSS steady / peak | CPU | Budget |
|---|---|---|---|
| Fedora 44, PipeWire, 10 s window | 6.7 MB / 8.6 MB | 0.8-1.1% of one core | not gated |
| Null backend, what CI measures | 2.4 MB / 2.4 MB | 0.8-1.1% of one core | 4 MB peak, 2.0% |

The null backend loads no audio library, so its RSS is about 4 MB below the real
device. CI can only measure that path, so the gate lives there and the published
figures come from `scripts/measure.sh` on a machine with a sound card.

Both budgets are 1.7x to 2x the measurement. RSS could be tighter; CPU cannot,
because the clock tick rounds a 10 s average to 0.1% and a shared runner adds
load the code does not control. The regressions worth catching are algorithmic
and move these numbers by an order of magnitude, not by a fifth. Raising a
budget needs the measurement that justifies it, in the same diff (rule 10).

### Formatting

`clang-format` **23.1.3**, rules in `.clang-format`, enforced in CI by `scripts/check-format.sh`.

- The version is pinned in three places: this line, `scripts/check-format.sh`, and the CI job. Formatting output changes between major releases, so a mismatch produces drift that no one caused.
- The config encodes the code already in this repository. It is not imported from an external style guide; where an external rule conflicted with the existing headers, the existing headers won.
- `third_party/` is never formatted. miniaudio is vendored verbatim.
- Local use: `scripts/check-format.sh --write` to format, `scripts/check-format.sh` to verify.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `TA_SANITIZE` | `OFF` | ASan + UBSan on own code. `audio/` stays uninstrumented (AGENTS 1.5). The `size` and `resource` ctest entries are skipped in this configuration: the sanitizer runtime is part of the binary and inflates both measurements, so measuring them does not test the shipped artifact. |
| `TA_REQUIRE_SPECTRUM` | `ON` | Fail configuration when Python3 is missing. A silently skipped FR-02 gate is worse than a missing one. |

### Per-OS notes

- **Linux:** link dynamically against libc, load audio backends with `dlopen`. Do not attempt static musl plus ALSA.
- **Windows:** `/MT` to avoid the VC++ Redistributable; `/O1 /GL /Gy /OPT:REF /OPT:ICF`. Own code needs `/std:c11 /experimental:c11atomics`: MSVC refuses `<stdatomic.h>` without the explicit atomic opt-in, and `CMAKE_C_STANDARD` alone does not add it.
- **CI:** Ubuntu + Windows matrix, plus a third job that builds with ASan and UBSan (`-DTA_SANITIZE=ON`). Linux installs `alsa-lib-devel`/`libasound2-dev` and `libpulse-dev`. No runner has an audio device, so `audio/` is exercised through miniaudio's null backend (`TINYAMBIENT_AUDIO=null`) and everything else through offline renders. The run-loop test (`tests/session_check.c`) is POSIX-only: it needs a second thread to observe a loop that must keep running, and MSVC has no C11 threads. The IPC contract has one harness per platform: `tests/ipc_check.cmake` inspects `/proc`, socket modes, and wall-clock latency; `tests/ipc_check_win.cmake` asserts the same forwarded-command behaviour over a private named pipe.
- **Windows IPC:** named pipes at `\\.\pipe\tinyambient-<sid>` with a DACL that grants the current user only. `CreateNamedPipe` with `nMaxInstances = 1` is the single-instance guard, so there is no lock file. The daemon is a detached copy of the executable (`DETACHED_PROCESS`) that re-runs the same arguments behind an internal `--daemon-child <event-handle>` switch; the parent waits on the inherited event, which the child signals once the device is up. `TINYAMBIENT_IPC` overrides the pipe name so the tests never touch a real session.

- **Sanitizers locally:** Fedora ships the sanitizer runtimes separately. `sudo dnf install libasan libubsan`, then configure with `-DTA_SANITIZE=ON`. Without those packages the build links but the binary cannot run.

---

## 5. Pitfalls

- **`dlopen` without fallback:** try ALSA and Pulse; report a clear error when neither exists.
- **Corrupted config:** atomic write (temp file + `fsync` + `rename` + directory `fsync`) in the same directory.
- **Portable mode without write permission:** fail with a clear message; never write elsewhere.
- **Hotplug:** a device removed mid-playback is reinitialized with backoff on the main thread, preserving state.
- **Clipping:** volume 100 and a crossfade must not sum above 1.0.
- **Denormals:** long fades and silence produce denormal values.
- **Strings:** always `snprintf`; never `strcpy`/`strcat`.
- **Contaminated core:** if `core/` includes an OS header, the architecture has failed.
- **CMake + Objective-C:** irrelevant in v1; do not reintroduce `.m` files.
- **Windows console:** the console subsystem stays. The CLI is the only entry point in v1 (no tray, no autostart, no shortcut), and a GUI-subsystem binary would need `AttachConsole` to print `--status` or `--help` at all. The daemon is started with `DETACHED_PROCESS`, so a background start allocates no console. A double-click from Explorer flashes the parent's console window for the duration of the start; that is the accepted trade.
- **`--status` with no daemon:** query IPC first; on `WN_IPC_ABSENT`, read config and report `playing=0` and `stop_after=off`, exit 0. Never start a daemon for status.
- **Single instance:** take the `flock` on the lock file before unlinking a stale socket or binding. Unlinking before the lock is held cuts a live daemon off from its clients.
- **Socket cleanup on quit:** stop accepting, unlink, and release the lock before sending the final reply, so a client that sees `ok` never observes a stale socket and a start issued right after `quit` is not refused while the old daemon is still stopping the device.
- **Client races with quit:** a connection can be accepted while the daemon is leaving; retry a connection that closes without a reply, then treat a missing socket as "start a daemon".
- **Forwarded errors:** the daemon replies `error <exit-code> <message>`; the client prints the message and exits with that code, so a failed daemon-side config write stays exit 1 and an invalid command stays exit 2.
- **Windows IPC:** the pipe instance is the connection, so the quit reply must be written and flushed before the instance is destroyed. `wn_ipc_server_reply_and_release` owns that ordering per platform; do not call `wn_ipc_server_release` before the reply on Windows, or the client sees a broken pipe instead of `ok`.
- **Windows daemon start:** the child re-runs the parent's arguments. Everything it does must be deterministic from those arguments plus the saved configuration, or the two paths diverge. `--render` never reaches this path, so the arguments are flags, noise names, and integers that need no command-line quoting.
- **Windows handle inheritance:** `CreateProcess` with `bInheritHandles` hands over every inheritable handle, including the caller's stdout. A daemon that keeps that pipe open makes `execute_process`, a shell pipeline, or any reader waiting for EOF block until the daemon exits. Pass `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` with only the readiness event.

---

## 6. Working rules

1. One milestone at a time, in PRD order. M8 closes v1; anything beyond it needs a PRD change first.
2. Small diffs; build and test after each one.
3. Do not add dependencies or widen scope (tray, network, presets, hotkeys, GUI) without asking.
4. Prefer fixed buffers, `snprintf`, and explicit bounds checks.
5. ASan/UBSan cover own code; miniaudio is excluded from those builds.
6. Measure the binary every milestone and record the number, together with its memory and CPU footprint (`scripts/measure.sh`).
7. Do not commit binaries; `.gitignore` already covers them.
8. Do not copy the reference PoC blindly: it has known weak spots (brown noise leak, gain calibration).
9. Explain the *why* in a few lines before writing code. This is a project to learn systems programming concepts. The developer has 3 yoe as a Javascript and ABAP Developer (SAPUI5 Consultant).
10. When a decision changes, update this file or the PRD in the same diff.
