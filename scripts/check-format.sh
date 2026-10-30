#!/bin/sh
# Formatting helper. Usage:
#   scripts/check-format.sh          # verify, exit 1 on drift (what CI runs)
#   scripts/check-format.sh --write  # rewrite files in place
#
# third_party/ is never formatted: miniaudio is vendored verbatim and its
# upstream formatting is not ours to change.

set -eu

CLANG_FORMAT_VERSION="23.1.3"
MODE="check"

case "${1:-}" in
    "")
        ;;
    --write)
        MODE="write"
        ;;
    -h | --help)
        echo "usage: $0 [--write]"
        exit 0
        ;;
    *)
        echo "check-format: unknown argument: $1" >&2
        exit 2
        ;;
esac

if ! command -v clang-format > /dev/null 2>&1; then
    echo "check-format: clang-format not found." >&2
    echo "check-format: install it with: python3 -m pip install --user clang-format==${CLANG_FORMAT_VERSION}" >&2
    exit 1
fi

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

# Only own code: never third_party/.
files=$(find src tests -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null | sort)
if [ -z "$files" ]; then
    echo "check-format: no source files found" >&2
    exit 1
fi

if [ "$MODE" = "write" ]; then
    # shellcheck disable=SC2086
    clang-format -i $files
    echo "check-format: formatted $(echo "$files" | wc -l) files"
    exit 0
fi

# shellcheck disable=SC2086
if ! clang-format --dry-run --Werror $files; then
    echo "check-format: formatting drift detected." >&2
    echo "check-format: run scripts/check-format.sh --write" >&2
    exit 1
fi

echo "check-format: ok"
