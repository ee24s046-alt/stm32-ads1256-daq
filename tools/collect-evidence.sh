#!/usr/bin/env bash
#
# Collect the host-side evidence artifacts listed in docs/evidence.md.
#
#   ./tools/collect-evidence.sh /dev/ttyACM0 [seconds]
#
# Produces:
#   captures/reference/shorted-10s.npz   short capture, committed to the repo
#   captures/reference/shorted-10s.bin   the raw bytes behind it
#   docs/img/noise-shorted.png           time series + noise density plot
#   docs/evidence/verify.txt             the loss-detection report
#   docs/evidence/noise.txt              the numbers behind the plot
#
# The logic-analyser captures and the bench photograph are manual by nature;
# see docs/evidence.md sections 2, 3 and 6.

set -euo pipefail

PORT="${1:-}"
SECONDS_LONG="${2:-300}"

if [[ -z "$PORT" ]]; then
    echo "usage: $0 <serial-port> [seconds]" >&2
    echo "       e.g. $0 /dev/ttyACM0 300" >&2
    exit 1
fi

if ! command -v ads1256 >/dev/null 2>&1; then
    echo "error: 'ads1256' not on PATH. Run:  pip install -e host[dev]" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p captures/reference docs/img docs/evidence

echo "=============================================================="
echo " Short ALL FOUR differential pairs at the INPUT CONNECTOR"
echo " (not at the ADC pins - that measures the converter, not the"
echo "  instrument), then press Enter."
echo "=============================================================="
read -r

# ---------------------------------------------------------------- reference
echo "[1/3] 10 s reference capture (committed to the repo) ..."
ads1256 capture --port "$PORT" --packets 20000 \
        --out captures/reference/shorted-10s.npz \
        --raw captures/reference/shorted-10s.bin

echo "      verifying ..."
ads1256 verify captures/reference/shorted-10s.npz | tee docs/evidence/verify.txt

# ---------------------------------------------------------------- long run
echo "[2/3] ${SECONDS_LONG} s characterisation run ..."
ads1256 capture --port "$PORT" --seconds "$SECONDS_LONG" --out /tmp/shorted-long.npz

# ---------------------------------------------------------------- analysis
echo "[3/3] noise analysis + plot ..."
ads1256 noise /tmp/shorted-long.npz --plot docs/img/noise-shorted.png \
    | tee docs/evidence/noise.txt

cat <<'EOF'

--------------------------------------------------------------------
Done. Still to capture by hand (docs/evidence.md):

  [ ] 1  stats dump over the UART during the long run
         -> docs/evidence/stats-300s.txt
  [ ] 2  logic analyser: /DRDY falling edge to /CS falling edge
         -> docs/img/la-drdy-to-cs.png
  [ ] 3  logic analyser: one full 133 us window, SPI decoder enabled
         -> docs/img/la-scan-window.png
  [ ] 6  bench photograph
         -> docs/img/bench.jpg
  [ ] 7  fault handler demonstration
         -> docs/evidence/fault-example.txt

Then replace the tables in docs/measured-results.md with the real
numbers and tick the boxes in docs/evidence.md.
--------------------------------------------------------------------
EOF
