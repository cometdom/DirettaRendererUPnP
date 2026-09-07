#!/usr/bin/env bash
# Decode regression suite: runs bin/test_decode over every audio file of a
# directory, served over HTTP with Range support, and checks
#   1. full decode (prefetch thread)      → frame count == container duration
#   2. full decode (--no-prefetch)        → same hash as 1.
#   3. five successive seeks, both modes  → same hash, frames ≈ expected
#   4. thirty random rapid seeks          → no error, frames ≈ expected
# Needs: ffprobe, python3, bin/test_decode (make test-decode).
#
#   tests/decode_suite.sh <directory-with-audio-files> [port]
set -u
DIR="${1:?usage: decode_suite.sh <dir> [port]}"
PORT="${2:-8097}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/test_decode"
SRV="$ROOT/tools/range_server.py"
[ -x "$BIN" ] || { echo "missing $BIN — run: make test-decode"; exit 2; }
command -v ffprobe >/dev/null || { echo "ffprobe not found"; exit 2; }

python3 "$SRV" "$PORT" "$DIR" </dev/null >/dev/null 2>&1 &
SRVPID=$!
trap 'kill $SRVPID 2>/dev/null' EXIT
sleep 0.5

pass=0; fail=0
ok()   { pass=$((pass+1)); printf "  PASS  %s\n" "$1"; }
ko()   { fail=$((fail+1)); printf "  FAIL  %s\n" "$1"; }

run() { # url bits extra... → sets R_FRAMES R_EXPECTED R_HASH R_RC R_ERR
    local out
    out=$(timeout 900 "$BIN" "$@" 2>&1); R_RC=$?
    R_FRAMES=$(printf "%s" "$out" | sed -n 's/^frames=\([0-9]*\).*/\1/p')
    R_EXPECTED=$(printf "%s" "$out" | sed -n 's/^frames=[0-9]* expected=\([0-9]*\).*/\1/p')
    R_HASH=$(printf "%s" "$out" | sed -n 's/^fnv1a=\(.*\)/\1/p')
    R_ERR=$(printf "%s" "$out" | grep -iE "error|fail|timeout" \
              | grep -v "decodeError=0" | grep -v "readTimeout=0" | head -1)
    : "${R_FRAMES:=0}" "${R_EXPECTED:=0}"
}

near() { # frames expected tolerance
    local d=$(( $1 - $2 )); [ $d -lt 0 ] && d=$(( -d )); [ $d -le $3 ]
}

for f in "$DIR"/*; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    case "${name,,}" in *.flac|*.wav|*.aif|*.aiff|*.m4a|*.mp3|*.ogg|*.opus|*.wv|*.dsf) ;; *) continue ;; esac
    p() { ffprobe -v error -select_streams a:0 -show_entries "$1" -of csv=p=0:nk=1 "$f" 2>/dev/null \
            | head -1 | tr -d ', '; }
    codec=$(p stream=codec_name); rate=$(p stream=sample_rate)
    rawbits=$(p stream=bits_per_raw_sample); dur=$(p format=duration)
    [ -n "$dur" ] || dur=0
    bits=24; [ "$rawbits" = "16" ] && bits=16; [ "$rawbits" = "32" ] && bits=32
    # ffprobe reports DSD as bytes per second per channel; the decoder counts 1-bit samples
    [ "$codec" != "${codec#dsd}" ] && rate=$(( rate * 8 ))
    echo "== $name ($codec ${rate}Hz ${rawbits}bit ${dur%.*}s → ${bits}-bit output)"
    url="http://127.0.0.1:$PORT/$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$name")"
    tol=$(( rate / 10 )); [ $tol -lt 8192 ] && tol=8192

    run "$url" $bits
    # DSD: the decoder returns exactly the DSF header's sample count, while the
    # "expected" figure is FFmpeg's duration estimate, which includes the zero
    # padding of the last block (a few tenths of a second). Accept ≤ 1 % under.
    frames_ok=0
    if [ "$R_FRAMES" != "0" ]; then
        if [ "$R_FRAMES" = "$R_EXPECTED" ]; then frames_ok=1
        elif [ "$codec" != "${codec#dsd}" ] && [ "$R_FRAMES" -le "$R_EXPECTED" ] \
             && [ $(( (R_EXPECTED - R_FRAMES) * 100 )) -le "$R_EXPECTED" ]; then frames_ok=1; fi
    fi
    if [ $R_RC -eq 0 ] && [ -z "$R_ERR" ] && [ $frames_ok -eq 1 ]; then
        ok "full decode, prefetch: $R_FRAMES frames"
    else
        ko "full decode, prefetch: rc=$R_RC frames=$R_FRAMES expected=$R_EXPECTED $R_ERR"
    fi
    h1=$R_HASH; total=$R_FRAMES   # seek expectations come from what the decoder actually delivers

    run "$url" $bits --no-prefetch
    if [ $R_RC -eq 0 ] && [ -z "$R_ERR" ] && [ "$R_HASH" = "$h1" ]; then
        ok "full decode, no-prefetch: same hash"
    else
        ko "full decode, no-prefetch: rc=$R_RC hash=$R_HASH vs $h1 $R_ERR"
    fi

    # seek positions as fractions of the duration
    secs=${dur%.*}; [ "$secs" -gt 4 ] || { echo "  skip seeks (too short)"; continue; }
    seq=$(python3 -c "d=$secs; print(','.join(str(int(d*x)) for x in (0.25,0.75,0.05,0.9,0.4)))")
    last=$(( secs * 40 / 100 ))
    exp_left=$(( total - last * rate ))

    run "$url" $bits --seeks "$seq"
    if [ $R_RC -eq 0 ] && [ -z "$R_ERR" ] && near "$R_FRAMES" "$exp_left" "$tol"; then
        ok "5 seeks, prefetch: $R_FRAMES frames left (≈$exp_left)"
    else
        ko "5 seeks, prefetch: rc=$R_RC frames=$R_FRAMES expected≈$exp_left $R_ERR"
    fi
    h2=$R_HASH

    run "$url" $bits --no-prefetch --seeks "$seq"
    if [ $R_RC -eq 0 ] && [ -z "$R_ERR" ] && [ "$R_HASH" = "$h2" ]; then
        ok "5 seeks, no-prefetch: same hash"
    else
        ko "5 seeks, no-prefetch: rc=$R_RC hash=$R_HASH vs $h2 $R_ERR"
    fi

    rnd=$(python3 -c "import random; random.seed(7); d=$secs; print(','.join(str(random.randint(0, d-2)) for _ in range(30)))")
    lastr=${rnd##*,}; exp_left=$(( total - lastr * rate ))
    run "$url" $bits --seeks "$rnd"
    if [ $R_RC -eq 0 ] && [ -z "$R_ERR" ] && near "$R_FRAMES" "$exp_left" "$tol"; then
        ok "30 random seeks, prefetch: $R_FRAMES frames left (≈$exp_left)"
    else
        ko "30 random seeks, prefetch: rc=$R_RC frames=$R_FRAMES expected≈$exp_left $R_ERR"
    fi
done

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
