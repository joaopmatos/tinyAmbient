# tinyAmbient — Agent Context

**State:** M0 complete. CMake build, layer headers, size gate, and CI skeleton are in place. No DSP, audio device, IPC, or CLI behavior yet.
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
2. Parameters cross the thread boundary through integer atomics: `_Atomic int` for type and playing, `_Atomic int volume_milli` (avoids `_Atomic float`). The stop-after deadline is main-thread state and needs no atomic.
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
  audio/
    device.c             # only file that includes miniaudio
  platform/
    paths.c  paths.h     # per-OS config directories
    ipc.c    ipc.h       # Unix socket / named pipe
  ui/
    cli.c                # flags, output, exit codes
tests/
  test_noise.c
  test_config.c
  test_command.c
  check_spectrum.py      # slopes/RMS over raw or WAV
third_party/
  miniaudio.h
docs/
  PRD.md  ARCHITECTURE.md
```

Tests grow per milestone; the list above is the M7 target.

### Layer rules

| Layer | May depend on |
|---|---|
| `core/` | libc, libm, C11 headers |
| `audio/` | `core/`, miniaudio |
| `platform/` | libc and OS APIs (the only layer that uses them) |
| `ui/` | `core/`, `platform/` |
| `main.c` | all layers; no DSP, no UI logic |

### Boundary interfaces

```c
/* core/engine.h */
typedef struct wn_engine wn_engine;

wn_engine *wn_engine_create(const wn_config *cfg);
void       wn_engine_destroy(wn_engine *e);
int        wn_engine_command(wn_engine *e, const char *line);
void       wn_engine_render(wn_engine *e, float *out, unsigned frames);
int        wn_engine_is_playing(const wn_engine *e);

/* platform.h */
int  wn_platform_run(wn_engine *e);
void wn_platform_request_quit(void);
int  wn_platform_ipc_send(const char *line, char *reply, size_t cap);
```

### Data flow

```
argv --+
       +--> command_parse --> engine_command --> atomics --> audio callback --> device
IPC  --+

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
- Soft clip is a safety net; the gain should prevent clipping in the first place.
- Denormals: flush-to-zero on x86 or a minimum DC offset to stay out of denormal range.
- Blue, gray, and green are out of v1. Do not implement them.

---

## 4. Build and size budget

### Base flags (GCC/Clang)

```
-Os -flto -ffunction-sections -fdata-sections
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
| Windows + `/MT` | ~200-250 KB (estimated) |

A regression above ~20 KB means something large slipped in.

### Milestone measurements

Recorded per AGENTS.md rule 6. Enforced by the `size-check` target and by the `size` ctest entry.

| Milestone | Variant | Size | Budget |
|---|---|---|---|
| M0 | Linux, GCC 16.2.1, `-Os -flto -s`, miniaudio compiled but unreferenced | 10 KB | 150 KB |

M0 note: nothing calls into miniaudio yet, so `--gc-sections` removes it entirely. A probe that references `ma_context_init` with the ALSA and PulseAudio backends measures **81 KB**, which is the realistic figure the binary will reach at M4 once the device is actually opened.

### Formatting

`clang-format` **23.1.3**, rules in `.clang-format`, enforced in CI by `scripts/check-format.sh`.

- The version is pinned in three places: this line, `scripts/check-format.sh`, and the CI job. Formatting output changes between major releases, so a mismatch produces drift that no one caused.
- The config encodes the code already in this repository. It is not imported from an external style guide; where an external rule conflicted with the existing headers, the existing headers won.
- `third_party/` is never formatted. miniaudio is vendored verbatim.
- Local use: `scripts/check-format.sh --write` to format, `scripts/check-format.sh` to verify.

### Per-OS notes

- **Linux:** link dynamically against libc, load audio backends with `dlopen`. Do not attempt static musl plus ALSA.
- **Windows:** `/MT` to avoid the VC++ Redistributable; `/O1 /GL /Gy /OPT:REF /OPT:ICF`.
- **CI:** Ubuntu + Windows matrix. Linux installs `alsa-lib-devel`/`libasound2-dev` and `libpulse-dev`. No audio device in CI: offline render only.

---

## 5. Pitfalls

- **`dlopen` without fallback:** try ALSA and Pulse; report a clear error when neither exists.
- **Corrupted config:** atomic write (temp file + `fsync` + `rename`) in the same directory.
- **Portable mode without write permission:** fail with a clear message; never write elsewhere.
- **Hotplug:** a device removed mid-playback is reinitialized with backoff on the main thread, preserving state.
- **Clipping:** volume 100 and a crossfade must not sum above 1.0.
- **Denormals:** long fades and silence produce denormal values.
- **Strings:** always `snprintf`; never `strcpy`/`strcat`.
- **Contaminated core:** if `core/` includes an OS header, the architecture has failed.
- **CMake + Objective-C:** irrelevant in v1; do not reintroduce `.m` files.
- **Windows console:** subsystem choice is deferred to M8 by the PRD.
- **`--status` with no daemon:** read config, report `playing=0` and `stop_after=off`, exit 0. Do not start a daemon.

---

## 6. Working rules

1. One milestone at a time, in PRD order. Do not jump to M8.
2. Small diffs; build and test after each one.
3. Do not add dependencies or widen scope (tray, network, presets, hotkeys, GUI) without asking.
4. Prefer fixed buffers, `snprintf`, and explicit bounds checks.
5. ASan/UBSan cover own code; miniaudio is excluded from those builds.
6. Measure the binary every milestone and record the number.
7. Do not commit binaries; `.gitignore` already covers them.
8. Do not copy the reference PoC blindly: it has known weak spots (brown noise leak, gain calibration).
9. Explain the *why* in a few lines before writing code. This is a project to learn systems programming concepts. The developer has 3 yoe as a Javascript and ABAP Developer (SAPUI5 Consultant).
10. When a decision changes, update this file or the PRD in the same diff.
