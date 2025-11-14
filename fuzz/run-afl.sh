#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

: "${AFL_SKIP_CPUFREQ:=1}"
: "${AFL_AUTORESUME:=1}"
: "${AFL_NO_AFFINITY:=1}"
: "${AFL_USE_ASAN:=1}"
: "${AFL_DONT_OPTIMIZE:=1}"

if ! command -v afl-fuzz >/dev/null 2>&1; then
	echo "fuzz/run-afl.sh: afl-fuzz not found on PATH" >&2
	exit 1
fi

if [ -n "${TIKL_AFL_CC:-}" ]; then
	AFL_WRAPPER=$TIKL_AFL_CC
elif [ -n "${AFL_CC:-}" ] && command -v "${AFL_CC}" >/dev/null 2>&1; then
	AFL_WRAPPER=$AFL_CC
else
	for cc in afl-clang-lto afl-clang-fast afl-clang afl-cc afl-gcc; do
		if command -v "$cc" >/dev/null 2>&1; then
			AFL_WRAPPER=$cc
			break
		fi
	done
fi

if [ -z "${AFL_WRAPPER:-}" ]; then
	echo "fuzz/run-afl.sh: no AFL compiler found (set TIKL_AFL_CC or AFL_CC)" >&2
	exit 1
fi

echo "[-] using AFL compiler: $AFL_WRAPPER" >&2

BUILD_DIR="$ROOT/fuzz/build"
OUT_DIR="$ROOT/fuzz/out"
SEED_DIR="$ROOT/fuzz/seeds"
DICT_PATH="$ROOT/fuzz/tikl.dict"

mkdir -p "$OUT_DIR" "$SEED_DIR"

make clean
make CFLAGS="-g3" CC=afl-gcc
TARGET=./tikl


set -- afl-fuzz -i "$SEED_DIR" -o "$OUT_DIR" -m 256 -t100
if [ -f "$DICT_PATH" ]; then
	set -- "$@" -x "$DICT_PATH"
fi
if [ -n "${AFL_FUZZ_ARGS:-}" ]; then
	# shellcheck disable=SC2086 # users may deliberately pass multiple words
	set -- "$@" ${AFL_FUZZ_ARGS}
fi
set -- "$@" -- "$TARGET" -q @@

echo "[-] launching afl-fuzz" >&2
exec "$@"
