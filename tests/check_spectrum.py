#!/usr/bin/env python3
"""Slope and RMS check for a rendered noise file.

Slope is measured in third-octave bands from 40 Hz to 12 kHz by Welch-averaged
FFT and a least-squares fit of band level in dB against log2(frequency), which
gives dB/octave.

Usage:
    check_spectrum.py FILE [--type white|pink|brown] [--rate 48000]
                           [--channels 2] [--strict]

Without --strict the script prints the numbers and exits 0. With --strict it
exits 1 when a value is outside the PRD FR-02 tolerance, which is what CI uses.

Only the C standard library is guaranteed, so numpy is optional: it is used when
present and a radix-2 FFT is used otherwise.
"""

import argparse
import array
import math
import sys

BAND_MIN_HZ = 40.0
BAND_MAX_HZ = 12000.0
SEGMENT = 16384
HOP = 8192

# PRD FR-02
TARGET_RMS_DBFS = -18.0
TOLERANCE = {
    "white": 0.5,
    "pink": 0.5,
    "brown": 1.0,
}
TARGET_SLOPE = {
    "white": 0.0,
    "pink": -3.0,
    "brown": -6.0,
}
RMS_TOLERANCE_DB = 0.5


def read_raw(path):
    data = array.array("f")
    with open(path, "rb") as handle:
        data.frombytes(handle.read())
    if sys.byteorder != "little":
        data.byteswap()
    return data


def read_wav(path):
    """Reads a float32 WAV file. Integer PCM arrives with the M3 writer."""
    with open(path, "rb") as handle:
        blob = handle.read()
    if blob[0:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise SystemExit(f"check_spectrum: {path} is not a RIFF/WAVE file")

    pos = 12
    channels = 2
    rate = 48000
    bits = 32
    audio_format = 3
    payload = None

    while pos + 8 <= len(blob):
        chunk_id = blob[pos : pos + 4]
        chunk_size = int.from_bytes(blob[pos + 4 : pos + 8], "little")
        body = blob[pos + 8 : pos + 8 + chunk_size]
        if chunk_id == b"fmt ":
            audio_format = int.from_bytes(body[0:2], "little")
            channels = int.from_bytes(body[2:4], "little")
            rate = int.from_bytes(body[4:8], "little")
            bits = int.from_bytes(body[14:16], "little")
        elif chunk_id == b"data":
            payload = body
        pos += 8 + chunk_size + (chunk_size & 1)

    if payload is None:
        raise SystemExit(f"check_spectrum: {path} has no data chunk")
    if audio_format != 3 or bits != 32:
        raise SystemExit("check_spectrum: only float32 WAV is supported")

    data = array.array("f")
    data.frombytes(payload)
    if sys.byteorder != "little":
        data.byteswap()
    return data, channels, rate


def third_octave_centers():
    centers = []
    for n in range(-30, 31):
        center = 1000.0 * (2.0 ** (n / 3.0))
        if BAND_MIN_HZ <= center <= BAND_MAX_HZ:
            centers.append(center)
    return centers


def hann(n):
    return [0.5 - 0.5 * math.cos(2.0 * math.pi * i / n) for i in range(n)]


def fft_inplace(a):
    """Iterative radix-2 Cooley-Tukey. len(a) must be a power of two."""
    n = len(a)

    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]

    length = 2
    while length <= n:
        angle = -2.0 * math.pi / length
        step = complex(math.cos(angle), math.sin(angle))
        half = length >> 1
        for start in range(0, n, length):
            w = 1.0 + 0.0j
            for k in range(start, start + half):
                u = a[k]
                v = a[k + half] * w
                a[k] = u + v
                a[k + half] = u - v
                w *= step
        length <<= 1


def power_spectrum(samples, rate, use_numpy):
    """Welch-averaged one-sided power spectrum. Returns (powers, bin_hz)."""
    if len(samples) < SEGMENT:
        raise SystemExit(
            f"check_spectrum: need at least {SEGMENT} frames, got {len(samples)}"
        )

    if use_numpy:
        import numpy

        signal = numpy.asarray(samples, dtype=numpy.float64)
        window = numpy.hanning(SEGMENT)
        accumulator = numpy.zeros(SEGMENT // 2 + 1)
        starts = range(0, len(signal) - SEGMENT + 1, HOP)
        for start in starts:
            chunk = signal[start : start + SEGMENT] * window
            spectrum = numpy.fft.rfft(chunk)
            accumulator += (spectrum.real**2 + spectrum.imag**2)
        powers = (accumulator / len(list(starts))).tolist()
        return powers, rate / SEGMENT

    window = hann(SEGMENT)
    accumulator = [0.0] * (SEGMENT // 2 + 1)
    starts = list(range(0, len(samples) - SEGMENT + 1, HOP))
    for start in starts:
        block = [complex(samples[start + i] * window[i]) for i in range(SEGMENT)]
        fft_inplace(block)
        for k in range(SEGMENT // 2 + 1):
            re = block[k].real
            im = block[k].imag
            accumulator[k] += re * re + im * im

    count = float(len(starts))
    return [value / count for value in accumulator], rate / SEGMENT


def band_levels(powers, bin_hz, centers):
    """Third-octave levels as power spectral density.

    A band's raw power grows with its width, and a third-octave band's width is
    proportional to its center frequency. Summing power directly would therefore
    report +3 dB/oct for flat-spectrum white noise. Dividing by the band width
    gives dB per Hz, which is the quantity the PRD slopes describe.
    """
    levels = []
    for center in centers:
        low = center * (2.0 ** (-1.0 / 6.0))
        high = center * (2.0 ** (1.0 / 6.0))
        total = 0.0
        bins = 0
        for index, power in enumerate(powers):
            freq = index * bin_hz
            if low <= freq < high:
                total += power
                bins += 1
        if bins == 0 or total <= 0.0:
            levels.append(-200.0)
            continue
        # width in Hz of the bins actually accumulated
        width = bins * bin_hz
        levels.append(10.0 * math.log10(total / width))
    return levels


def fit_slope(centers, levels):
    """Least-squares dB per octave."""
    xs = [math.log2(c) for c in centers]
    mean_x = sum(xs) / len(xs)
    mean_y = sum(levels) / len(levels)
    numerator = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, levels))
    denominator = sum((x - mean_x) ** 2 for x in xs)
    return numerator / denominator


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file")
    parser.add_argument("--type", choices=["white", "pink", "brown"], default=None)
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.file.lower().endswith(".wav"):
        data, channels, rate = read_wav(args.file)
    else:
        data = read_raw(args.file)
        channels, rate = args.channels, args.rate

    if channels <= 0 or len(data) % channels != 0:
        raise SystemExit("check_spectrum: file size is not a whole frame count")

    frames = len(data) // channels

    energy = 0.0
    for value in data:
        energy += value * value
    rms = math.sqrt(energy / len(data))
    rms_dbfs = 20.0 * math.log10(rms) if rms > 0.0 else -200.0

    # Slope is measured on the left channel; both channels are independent
    # generators, so either one is representative.
    left = data[0::channels]

    try:
        import numpy  # noqa: F401

        use_numpy = True
    except ImportError:
        use_numpy = False

    powers, bin_hz = power_spectrum(left, rate, use_numpy)
    centers = third_octave_centers()
    levels = band_levels(powers, bin_hz, centers)
    slope = fit_slope(centers, levels)

    print(f"file      : {args.file}")
    print(f"frames    : {frames} ({frames / rate:.2f} s at {rate} Hz)")
    print(f"rms       : {rms_dbfs:.3f} dBFS")
    print(f"slope     : {slope:.3f} dB/oct over {BAND_MIN_HZ:.0f}-{BAND_MAX_HZ:.0f} Hz")
    print(f"bands     : {len(centers)} third-octave")
    print(f"fft       : {'numpy' if use_numpy else 'built-in radix-2'}")

    if args.verbose:
        for center, level in zip(centers, levels):
            print(f"  {center:8.1f} Hz  {level:8.2f} dB")

    if not args.strict:
        return 0

    if args.type is None:
        raise SystemExit("check_spectrum: --strict requires --type")

    ok = True

    if abs(rms_dbfs - TARGET_RMS_DBFS) > RMS_TOLERANCE_DB:
        print(
            f"FAIL: rms {rms_dbfs:.3f} dBFS is outside "
            f"{TARGET_RMS_DBFS} +/- {RMS_TOLERANCE_DB} dB",
            file=sys.stderr,
        )
        ok = False

    expected = TARGET_SLOPE[args.type]
    tolerance = TOLERANCE[args.type]
    if abs(slope - expected) > tolerance:
        print(
            f"FAIL: slope {slope:.3f} dB/oct is outside "
            f"{expected} +/- {tolerance} for {args.type}",
            file=sys.stderr,
        )
        ok = False

    if not ok:
        return 1

    print(f"ok        : {args.type} within tolerance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
