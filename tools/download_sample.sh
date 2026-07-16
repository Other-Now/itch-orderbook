#!/usr/bin/env bash
# Fetch a sample NASDAQ TotalView-ITCH 5.0 daily file for local testing.
#
# Nasdaq publishes sample daily binary files on its EMI FTP server. Files are
# named like 01302019.NASDAQ_ITCH50.gz (MMDDYYYY). We grab one, gunzip it, and
# drop it in ./data. If the FTP is unreachable, Databento hosts equivalent
# sample data (see PLAN.md).
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p data
cd data

# A historically-available sample date. Override with: DATE=MMDDYYYY ./download_sample.sh
# Note: these are full trading days (multi-GB compressed). HTTPS is used because
# the EMI FTP (port 21) is often firewalled; the HTTP(S) mirror serves the same
# files. Ctrl-C once you have enough -- the parser stops cleanly at a truncated
# tail, so a partial file is fine for testing.
DATE="${DATE:-01302019}"
FILE="${DATE}.NASDAQ_ITCH50"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${FILE}.gz"

if [[ -f "${FILE}" ]]; then
    echo "already have ${FILE}"
    exit 0
fi

echo "downloading ${URL} ..."
if curl -fL --retry 3 -o "${FILE}.gz" "${URL}"; then
    # `|| true`: a Ctrl-C'd download is a truncated gzip; decompress the valid
    # prefix anyway (gunzip errors on the missing trailer but the data is good).
    gzip -dc "${FILE}.gz" > "${FILE}" 2>/dev/null || true
    rm -f "${FILE}.gz"
    echo "ready: data/${FILE} ($(du -h "${FILE}" | cut -f1))"
else
    cat <<EOF
Could not fetch from Nasdaq EMI FTP.
Try a different DATE, or download a sample from Databento:
  https://databento.com/datasets/XNAS.ITCH  (free sample, MBO/L3 schema)
Then place a *.NASDAQ_ITCH50 file in ./data/ and run:
  ./build/itch_replay data/<file>.NASDAQ_ITCH50
EOF
    exit 1
fi
