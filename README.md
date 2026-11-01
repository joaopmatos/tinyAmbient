# tinyAmbient

**Background noise for your workday, without the browser tab.**

tinyAmbient plays white, pink, or brown noise while you work. It starts
quickly, keeps playing in the background, and stays out of the way. No
account, no ads, no network connection.

It runs on Linux (x86_64) and Windows 10/11 (x86_64).

## My motivation
Let's imagine you want to listen to some background noise while working. What are your main options?
- Youtube video
- Spotify
- Blanket
- Other apps?

None of these satisfied my desire for a *lean*, *resource efficient*, zero dependencies application. The implementation that's been done runs on *extremely* low resources: under 10 MB of RAM, under 1% of one CPU core, and a <150 KB Linux / <300 KB Windows binary.

This application only needs a local installation and you're off to the races! It efficiently gets out of the way.

## What you get

- White, pink, and brown noise.
- One command to start; control it from any terminal.
- Keeps playing between commands.
- Remembers your noise type, volume, and mono/stereo setting.
- Stops on its own after a time you choose.
- No account, no telemetry, no network access.

## Install

tinyAmbient is a single binary. Download the archive for your platform from
the [latest release](https://github.com/joaopmatos/tinyAmbient/releases/latest),
unpack it, and put the binary on your `PATH`. There is no installer and no
runtime to add.

### Linux

1. Download `tinyambient-<version>-linux-x86_64.tar.gz`.
2. Unpack it:

   ```sh
   tar -xzf tinyambient-*-linux-x86_64.tar.gz
   ```

3. Install the binary. `~/.local/bin` works for a single user:

   ```sh
   mkdir -p ~/.local/bin
   install -m 755 tinyambient ~/.local/bin/
   ```

   Use `sudo install -m 755 tinyambient /usr/local/bin/` for all users.

4. Check that it runs:

   ```sh
   tinyambient --version
   ```

5. Start it:

   ```sh
   tinyambient --noise pink
   ```

If `~/.local/bin` is not on your `PATH`, add it in your shell profile or use
`/usr/local/bin` instead. tinyAmbient uses ALSA or PulseAudio/PipeWire, which
are already present on most desktop Linux systems. There is nothing else to
install.

### Windows

1. Download `tinyambient-<version>-windows-x86_64.zip`.
2. Extract `tinyambient.exe` to a folder, for example `C:\Tools\tinyAmbient`.
3. Add that folder to your user `PATH`:
   - Settings → System → About → Advanced system settings → Environment
     Variables → User variables → `Path` → Edit → New → `C:\Tools\tinyAmbient`.
   - Or from PowerShell:

     ```powershell
     $dir = "C:\Tools\tinyAmbient"
     $path = [Environment]::GetEnvironmentVariable("Path", "User")
     [Environment]::SetEnvironmentVariable("Path", "$path;$dir", "User")
     ```

4. Open a new terminal and check that it runs:

   ```powershell
   tinyambient --version
   ```

5. Start it:

   ```powershell
   tinyambient --noise pink
   ```

No separate runtime is required. You can also skip the `PATH` step and run
the executable with its full path.

### Verify the download (optional)

Each release ships a `SHA256SUMS` file next to the archives. Download it and
the archive you need into the same folder, then compare:

```sh
sha256sum -c SHA256SUMS --ignore-missing
```

On Windows:

```powershell
Get-FileHash .\tinyambient-<version>-windows-x86_64.zip -Algorithm SHA256
```

The printed hash must match the line for that file in `SHA256SUMS`.

### Run without installing

You can run the binary from the folder you extracted:

```sh
./tinyambient --noise pink
```

On Windows:

```powershell
.\tinyambient.exe --noise pink
```

Settings still go to the usual per-user location unless you pass
`--portable`.

## Quick start

```sh
tinyambient --noise brown --volume 25   # start with brown noise at 25%
tinyambient --status                    # see what is playing
tinyambient --volume 40                 # change the volume
tinyambient --stop-after 90             # stop after 90 minutes
tinyambient --toggle                    # pause or resume
tinyambient --quit                      # stop now
```

The first command starts a small background process. Later commands talk to
it and return immediately, so you can keep the same session running from any
terminal.

## Commands

| Command | Effect |
|---|---|
| `play`, `pause`, `toggle` | Control playback. |
| `noise white\|pink\|brown` | Choose the noise type. |
| `volume 0-100` | Set the volume. |
| `stop-after 1-10080\|off` | Stop after N minutes, or cancel the timer. |
| `status` | Show the current state. |
| `quit` | Stop the background process. |
| `--portable` | Keep `config.ini` next to the executable. |
| `--foreground` | Keep the process attached to the terminal. |

You can write commands with or without the leading `--`: `tinyambient noise
brown` and `tinyambient --noise brown` do the same thing.

## Status

`--status` prints a short, fixed list of lines:

```text
version=1.0.0
playing=1
noise=pink
volume=40
mono=0
stop_after=off
```

`playing=1` means audio is running, `mono=0` means stereo, and
`stop_after=off` means no timer is set. While a timer is active, the value is
the number of seconds left. The format is stable, so scripts can read it.

## Configuration

Settings live in a small text file:

- Linux: `$XDG_CONFIG_HOME/tinyambient/config.ini`, or
  `~/.config/tinyambient/config.ini`.
- Windows: `%APPDATA%\tinyAmbient\config.ini`.

You can edit it by hand. Unknown keys and comments are kept when tinyAmbient
rewrites the file.

```ini
[general]
noise=pink
volume=40
mono=false
fade_ms=250
```

| Key | Default | What it does |
|---|---|---|
| `noise` | `pink` | `white`, `pink`, or `brown`. |
| `volume` | `40` | 0-100. |
| `mono` | `false` | `true` sends one channel to both ears. |
| `fade_ms` | `250` | Crossfade length when the noise changes, 0-2000 ms. |

## Advanced: render to a file

You can also write the noise to a file instead of playing it:

```sh
tinyambient --render noise.wav --noise brown --seconds 10 --seed 42
```

A `.wav` path gets a standard WAV header. Any other path writes raw 32-bit
float samples. `--seconds`, `--seed`, `--noise`, `--volume`, `--mono`, and
`--stereo` apply to the render.

## Build from source

Prefer to build it yourself? You will need CMake 3.20+, a C11 compiler,
Ninja, and Python 3 for the test suite.

Linux:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows (Visual Studio 2022 Build Tools):

```powershell
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build enforces the size budget: 150 KB on Linux and 300 KB on Windows.

## Documentation

- `docs/PRD.md` - scope, behavior, acceptance criteria.
- `docs/ARCHITECTURE.md` - internal design.
- `AGENTS.md` - constraints, layers, build, size budget.

## License

MIT. See [LICENSE](LICENSE).
