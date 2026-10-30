# Vendored dependencies

| Library | Version | File | SHA-256 |
|---|---|---|---|
| miniaudio | 0.11.25 | `miniaudio.h` | `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89` |

Source: <https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h>

Rules:

- Do not upgrade without testing audio playback and measuring the binary (AGENTS.md 1.5).
- After an upgrade, rerun the size check and record the new number in this file.
- Only `src/audio/device.c` defines `MINIAUDIO_IMPLEMENTATION`.
