# tinyAmbient — white, pink, and brown noise from your terminal

<p align="center">
  <img src="docs/logo.svg" width="120" alt="tinyAmbient logo">
</p>

**Background noise for your workday, without the browser tab.**

tinyAmbient is a command-line white, pink, and brown noise generator for Linux
(x86_64) and Windows 10/11 (x86_64). It runs as one ~120 KB binary: no browser
tab, no account, no ads, no network connection.

![tinyAmbient in a terminal](docs/demo.gif)

*A 15 second tour of the CLI.*

## Try it

```sh
tinyambient --noise pink    # start playing; the command returns at once
tinyambient --status        # see what is running
tinyambient --volume 35     # change it from any terminal
tinyambient --quit          # stop
```

The first command starts a small background process. Everything after it talks
to that process, so the session survives between terminals and shells. A new
session uses pink noise at volume 20 and opens over a second, so it never lands
at full level unannounced.

## What you get

- White, pink, and brown noise, generated in-process.
- One command to start; control it from any terminal afterwards.
- Remembers your noise type, volume, and mono/stereo setting.
- Stops on its own after a time you choose.
- Roughly 126 KB on Linux and 293 KB on Windows. No installer, no runtime, no
  dependencies beyond the audio stack your desktop already ships.

## Why this exists

If you want background noise while working, the usual options are a 10-hour
YouTube loop, a streaming playlist, a paid desktop app, or a physical fan. Each
one costs something: a browser tab holding memory, an account, a subscription,
or a piece of hardware.

tinyAmbient is the lean version of that. It sits at about 7 MB of memory and
around 1% of one CPU core, holds a steady volume, and stays out of the way.

## How light is it?

Real numbers from v1.0.0 on Fedora 44 with PipeWire. Binary size is from the
published Linux archive, which is built on the CI runner:

| | On a desktop | On CI (no sound card) |
|---|---|---|
| Binary size | 126 KB | 126 KB |
| Memory in use | 6.7 MB | 2.4 MB |
| Memory at peak | 8.6 MB | 2.4 MB |
| CPU while playing | 0.8-1.1% of a core | 0.8-1.1% of a core |
| Threads | 3 | 3 |

CI has no sound card, so the audio library never loads there and the memory
figure is lower. Size, memory, and CPU are checked on every build: a change that
makes tinyAmbient heavier fails instead of shipping.

The archive measures 129,400 bytes. The same source built here with Fedora's GCC
16.2 comes out at 116 KB, so compiler and linker version decide a good part of
the file size at this scale. Neither number is close to the 150 KB budget.

From a source checkout, `scripts/measure.sh` measures your own machine.

## Install

Download the archive for your platform from the
[latest release](https://github.com/joaopmatos/tinyAmbient/releases/latest),
unpack it, and put the binary on your `PATH`.

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

No separate runtime is required. You can also skip the `PATH` step and run the
executable with its full path.

The background process runs from `%LOCALAPPDATA%\tinyAmbient\daemon\`, not from
the folder you installed. That keeps the installed binary idle, so
`winget upgrade` can replace it while noise is playing. A running daemon keeps
the version it started with; `tinyambient --quit` and start again to pick up an
upgrade. Run `tinyambient --quit` before uninstalling too: the package manager
removes the binary, and the daemon would otherwise keep playing with no command
left to reach it. With `--portable` the daemon runs from the unpacked folder
instead, because its `config.ini` lives there; stop it before replacing those
files.

### Run without installing

You can run the binary straight from the folder you extracted:

```sh
./tinyambient --noise pink
```

On Windows:

```powershell
.\tinyambient.exe --noise pink
```

Settings still go to the usual per-user location unless you pass `--portable`.

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

Commands work with or without the leading `--`: `tinyambient noise brown` and
`tinyambient --noise brown` do the same thing. `tinyambient --help` prints the
full list.

## Status

`--status` prints a short, fixed list of lines:

```text
version=1.0.5
playing=1
noise=pink
volume=20
mono=0
stop_after=off
```

`playing=1` means audio is running, `mono=0` means stereo, and `stop_after=off`
means no timer is set. While a timer is active, the value is the number of
seconds left. The format is stable, so scripts can read it.

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
volume=20
mono=false
fade_ms=250
```

| Key | Default | What it does |
|---|---|---|
| `noise` | `pink` | `white`, `pink`, or `brown`. |
| `volume` | `20` | 0-100. |
| `mono` | `false` | `true` sends one channel to both ears. |
| `fade_ms` | `250` | Crossfade length when the noise changes, 0-2000 ms. |

## Coming from something else

| What you do today | The tinyAmbient equivalent |
|---|---|
| A 10-hour loop on YouTube | `tinyambient --noise brown --stop-after 600` |
| A noise playlist on a streaming service | `tinyambient --noise pink --volume 30` |
| A fan or a sound machine | `tinyambient --noise white` |

tinyAmbient ships three synthetic generators. It has no rain, fan, or nature
recordings, and no per-ear balance beyond stereo or mono.

## Render to a file

You can write the noise to a file instead of playing it:

```sh
tinyambient --render noise.wav --noise brown --seconds 10 --seed 42
```

A `.wav` path gets a standard WAV header. Any other path writes raw 32-bit
float samples. The render defaults are `--noise white --volume 100 --seconds
10 --seed 1 --stereo`; `--mono` duplicates one channel. The same seed and
arguments always produce the same file.

## Build from source

You will need CMake 3.20+, a C11 compiler, Ninja, and Python 3 for the test
suite.

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
- `scripts/measure.sh` - the memory and CPU numbers above, on your hardware.

## FAQ

### Is there a white, pink, or brown noise generator for the command line?

Yes. tinyAmbient generates all three in-process and switches between them from
the same command:

```sh
tinyambient --noise white
tinyambient --noise pink
tinyambient --noise brown
```

### Does it work offline?

Completely. tinyAmbient opens no network sockets. The only thing it needs is the
audio stack your desktop already runs.

### Do I need to install anything on Windows?

No. Extract `tinyambient.exe` and run it. Adding the folder to `PATH` is optional
and only saves typing the full path.

### Can it stop on its own after a set time?

Yes. `tinyambient --stop-after 90` starts a 90 minute timer, `--stop-after off`
cancels it, and the range is 1 to 10080 minutes. An expired timer fades out over
20 seconds instead of cutting off, and `play` during that fade cancels it.

### Why is it only 120 KB?

Because it does one thing. There is no GUI, no tray icon, no decoder, no network
client, and no bundled runtime. Size and memory are gated on every build, so the
binary fails CI instead of quietly growing.

## License

MIT. See [LICENSE](LICENSE).
