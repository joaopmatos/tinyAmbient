#!/bin/sh
# Measures what the README claims: memory and CPU of the daemon while it plays.
#
#   scripts/measure.sh                  # real device, 10 s window
#   scripts/measure.sh --null           # no sound card
#   scripts/measure.sh --seconds 30     # longer window, finer CPU resolution
#   scripts/measure.sh --binary <path>  # a binary other than build/tinyambient
#
# CI runs the same measurement as `ctest -R resource`, which can only use the
# null backend because no runner has a sound card.
#
# The daemon gets private config and socket directories, so it cannot touch your
# real session. It plays at volume 1 to stay quiet; the work is the same at any
# volume, because the envelope only scales samples that are generated anyway.

set -eu

BIN="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)/build/tinyambient"
BACKEND="device"
WINDOW=10

while [ $# -gt 0 ]; do
    case "$1" in
        --null) BACKEND="null" ;;
        --seconds)
            WINDOW="${2:-}"
            shift
            ;;
        --binary)
            BIN="${2:-}"
            shift
            ;;
        -h | --help)
            awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
            exit 0
            ;;
        *)
            echo "measure: unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

case "$WINDOW" in
    '' | *[!0-9]*)
        echo "measure: --seconds needs a number" >&2
        exit 2
        ;;
esac

if [ "$WINDOW" -lt 1 ]; then
    echo "measure: --seconds needs at least 1" >&2
    exit 2
fi

if [ ! -x "$BIN" ]; then
    echo "measure: no executable at $BIN" >&2
    echo "measure: build first, or pass --binary" >&2
    exit 1
fi

SANDBOX=$(mktemp -d "${TMPDIR:-/tmp}/tinyambient-measure.XXXXXX")
mkdir -p "$SANDBOX/xdg" "$SANDBOX/runtime"

export XDG_CONFIG_HOME="$SANDBOX/xdg"
export XDG_RUNTIME_DIR="$SANDBOX/runtime"
if [ "$BACKEND" = "null" ]; then
    export TINYAMBIENT_AUDIO=null
fi

PID=""
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        "$BIN" --quit >/dev/null 2>&1 || kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$SANDBOX"
}
trap cleanup EXIT INT TERM

"$BIN" --noise pink --volume 1 --foreground >"$SANDBOX/log" 2>&1 &
PID=$!

# Readiness is the socket, the same signal the integration tests use.
i=0
while [ ! -S "$SANDBOX/runtime/tinyambient.sock" ]; do
    i=$((i + 1))
    if [ "$i" -gt 100 ] || ! kill -0 "$PID" 2>/dev/null; then
        echo "measure: the daemon did not start" >&2
        cat "$SANDBOX/log" >&2
        exit 1
    fi
    sleep 0.05
done

# Warm-up: dlopen, the device buffers, the first touch of every page.
sleep 2

rss() { awk '/^VmRSS/{print $2}' "/proc/$PID/status"; }
peak() { awk '/^VmHWM/{print $2}' "/proc/$PID/status"; }
threads() { awk '/^Threads/{print $2}' "/proc/$PID/status"; }
ticks() { awk '{sub(/.*\) /,""); print $12+$13}' "/proc/$PID/stat"; }

HZ=$(getconf CLK_TCK)
TICKS_BEFORE=$(ticks)
RSS_BEFORE=$(rss)
sleep "$WINDOW"
TICKS_AFTER=$(ticks)
RSS_AFTER=$(rss)
PEAK=$(peak)
THREADS=$(threads)
BYTES=$(wc -c <"$BIN" | tr -d ' ')

if [ "$BACKEND" = "null" ]; then
    LABEL="miniaudio null backend"
else
    LABEL="system audio device"
fi

printf 'binary:  %s (%s bytes, %s KB)\n' "$BIN" "$BYTES" "$((BYTES / 1024))"
printf 'backend: %s\n' "$LABEL"
printf 'threads: %s\n' "$THREADS"
printf 'rss:     %s KB after warm-up, %s KB after %ss, peak %s KB\n' \
    "$RSS_BEFORE" "$RSS_AFTER" "$WINDOW" "$PEAK"
awk -v d="$((TICKS_AFTER - TICKS_BEFORE))" -v hz="$HZ" -v w="$WINDOW" \
    'BEGIN { printf "cpu:     %.2f%% of one core (%d ticks at %d Hz over %ds)\n", d / hz / w * 100, d, hz, w }'
echo
echo "The CI gate is 'ctest -R resource': null backend, 4096 KB peak, 2.0% CPU."
echo "The numbers above are the ones to publish, on hardware users actually have."
