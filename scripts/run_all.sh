#!/usr/bin/env bash
# Run every experiment of this article, in the order their conclusions depend on
# one another.  --quick runs a reduced design that finishes in minutes.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -x build/experiments ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build
fi
mkdir -p results data

# The instance files ship gzipped, because in plain text they are the only thing in the repository
# large enough to matter to a git host.  Each archive is expanded beside itself, and kept: the .gz
# files are tracked, so deleting them (gunzip's default) would show the whole of data/ as deleted
# in a clone after the first run.  The expansions are git-ignored.  After the first run this is a
# no-op.
if compgen -G "data/*.txt.gz" > /dev/null; then
  echo "expanding shipped instance files"
  for f in data/*.txt.gz; do [ -f "${f%.gz}" ] || gunzip -kf "$f"; done
fi

# Before any experiment: check the generator, the instance file format and the two independent
# evaluators against each other.  A failure here would make every number below meaningless.
./build/experiments selftest
python3 scripts/check_stats.py

QUICK=0
for arg in "$@"; do [ "$arg" = "--quick" ] && QUICK=1; done

# A quick run is a smoke test, not a replication: it writes to its own directory so that it cannot
# overwrite the records the paper was built from.  Point analyse.py at it with --results to see the
# tables a reduced design produces.
RES=results
if [ $QUICK -eq 1 ]; then RES=results-quick; fi
mkdir -p "$RES"
run() { ./build/experiments "$@" --results "$RES"; }

if [ $QUICK -eq 1 ]; then
  run dispersion --seeds 3 --budget 20000
  run factors --seeds 3 --budget 20000
  run anchor --seeds 3 --budget 20000
  run plateau --seeds 1 --maxbudget 20000
  run budget --seeds 2 --maxbudget 50000
  run instances --data data
  # The audit enumerates the published campaign design, so under --quick it certifies more
  # constructions than the reduced campaigns use.  That is harmless; it is never fewer.
  run construction
else
  run dispersion --seeds 20 --budget 200000
  run factors    --seeds 10 --budget 200000
  run anchor     --seeds 12 --budget 200000
  run plateau    --seeds 5
  run budget     --seeds 6
  run instances  --data data
  # The constructor audit that bounds the number of unplaced classes in every run above.  It
  # enumerates the campaign/instance-seed/solver-seed/order tuples of the five campaigns, and takes
  # a few minutes because each of its runs stops as soon as it has constructed.
  run construction
fi

# The design validator is only worth having if it fails when the design changes.  This mutates
# throwaway copies of the records just written and requires analyse.py to refuse each one.  It is
# skipped under --quick, whose whole point is a design the validator is supposed to reject.
if [ $QUICK -eq 0 ]; then
  python3 scripts/check_design.py --results "$RES"
fi

# A reduced design is one the publication validator is meant to reject, so point the reader at
# the command that actually works for what they just ran.
DEV=""
if [ $QUICK -eq 1 ]; then DEV=" --development"; fi

echo
echo "results written to $RES/; now run: python3 scripts/analyse.py --results $RES$DEV"
