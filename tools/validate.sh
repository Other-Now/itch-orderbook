#!/usr/bin/env bash
# Reproduce the README's real-data validation: replay the full 2019-01-30 NASDAQ
# TotalView-ITCH 5.0 day and check that every message decodes at its canonical
# length (0 length mismatches) and every order-referencing message resolves to a
# live order (0 unknown references). A partial download validates its prefix.
set -euo pipefail
cd "$(dirname "$0")/.."

DATE="${DATE:-01302019}"                 # 2019-01-30, the day quoted in the README
FILE="data/${DATE}.NASDAQ_ITCH50"
BIN="build/itch_replay"

if [[ ! -f "$FILE" ]]; then
    echo "== fetching ${DATE} (multi-GB; Ctrl-C after enough for a partial check) =="
    DATE="$DATE" ./tools/download_sample.sh
fi

if [[ ! -x "$BIN" ]]; then
    echo "== building itch_replay =="
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build --target itch_replay -j >/dev/null
fi

echo "== replaying ${FILE} =="
out="$("$BIN" "$FILE")"
echo "$out"

mismatch=$(awk '/length mismatch/ {print $NF}' <<<"$out")
unknown=$(awk '/unknown ref/      {print $NF}' <<<"$out")
msgs=$(awk '/^messages/           {print $NF}' <<<"$out")
stocks=$(awk '/stocks seen/       {print $NF}' <<<"$out")

echo
echo "messages replayed : ${msgs:-?}"
echo "symbols seen      : ${stocks:-?}"
echo "length mismatches : ${mismatch:-?}"
echo "unknown refs      : ${unknown:-?}"

if [[ "${mismatch:-1}" == "0" && "${unknown:-1}" == "0" ]]; then
    echo "PASS: framing/offsets and order-lifetime semantics correct on real data"
else
    echo "FAIL: non-zero length mismatch or unknown ref"
    exit 1
fi
