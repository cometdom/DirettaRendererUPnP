#!/usr/bin/env bash
# Bit-exact suite: decode generated WAV/FLAC (16/44 and 24/96, the 24-bit file
# starting with 2 s of silence) through bin/test_decode with and without the
# prefetch thread and compare the output hash with the SOURCE samples (24-bit
# expanded to S32<<8 as FFmpeg does). Needs python3 and bin/test_decode.
#
#   tests/bitexact_suite.sh [workdir] [port]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-${TMPDIR:-/tmp}/drup-bitexact}"
PORT="${2:-8094}"
BIN="$ROOT/bin/test_decode"
[ -x "$BIN" ] || { echo "missing $BIN — run: make test-decode"; exit 2; }

python3 "$ROOT/tools/make_test_audio.py" "$DIR" >/dev/null || { echo "generator failed"; exit 2; }
python3 "$ROOT/tools/range_server.py" "$PORT" "$DIR" </dev/null >/dev/null 2>&1 &
SRVPID=$!
trap 'kill $SRVPID 2>/dev/null' EXIT
sleep 0.5

ref() {  # reference FNV-1a of the source samples in the output container format
    python3 - "$1" <<'PY'
import sys, wave
w = wave.open(sys.argv[1], "rb"); width = w.getsampwidth(); n = w.getnframes(); raw = w.readframes(n); w.close()
if width == 3:
    raw = b"".join(b"\x00" + raw[i:i + 3] for i in range(0, len(raw), 3))
h = 1469598103934665603
for b in raw:
    h ^= b; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
print("%016x %d" % (h, n))
PY
}

pass=0; fail=0
for spec in "s16_44.wav 16" "s16_44.flac 16" "s24_96.wav 24" "s24_96.flac 24"; do
    set -- $spec
    src="$DIR/${1%.*}.wav"
    read -r refhash refframes < <(ref "$src")
    for mode in "" "--no-prefetch"; do
        out=$(timeout 300 "$BIN" "http://127.0.0.1:$PORT/$1" "$2" $mode 2>&1)
        hash=$(printf "%s" "$out" | sed -n 's/^fnv1a=//p'); frames=$(printf "%s" "$out" | sed -n 's/^frames=\([0-9]*\).*/\1/p')
        if [ "$hash" = "$refhash" ] && [ "$frames" = "$refframes" ]; then
            pass=$((pass + 1)); printf "  PASS  %-12s %-13s bit-exact vs source (%s frames)\n" "$1" "${mode:-prefetch}" "$frames"
        else
            fail=$((fail + 1)); printf "  FAIL  %-12s %-13s hash=%s frames=%s (ref %s / %s)\n" "$1" "${mode:-prefetch}" "$hash" "$frames" "$refhash" "$refframes"
        fi
    done
done
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
