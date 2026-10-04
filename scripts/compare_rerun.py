#!/usr/bin/env python3
"""Compare a re-run against the shipped records, column by column.

    python3 scripts/compare_rerun.py [OLD_DIR] [NEW_DIR] [--complete]

A re-run with a newer driver adds columns; it must not change the old ones.  This prints, for each
results file present in both directories, how many rows matched on every shared column and which
did not.  It is the check that lets a campaign be replaced by one carrying extra instrumentation
without silently replacing the science as well.

Values are compared as numbers where both sides parse as numbers, and exactly: no tolerance.  The
one exception is a value the old file wrote in exponent form.  Older drivers wrote with the
stream's default six significant digits, so every objective from one million up went out rounded
(20419175 as 2.04192e+07); the current one writes fifteen.  Such a value is accepted when the new, exact value rounds to it at six
significant digits -- and those cells are counted and reported separately, so the reader can see
how much of the agreement rests on that rule rather than on exact equality.

--complete makes a missing row a failure: use it when NEW_DIR is meant to be a full re-run rather
than a partial one.
"""
import csv
import os
import re
import sys

KEYS = {
    "factors.csv": ("factor", "level", "n", "seed"),
    "anchor.csv": ("anchor", "history_policy", "n", "seed"),
    "budget.csv": ("arm", "n", "seed", "budget"),
    "dispersion.csv": ("n", "budget", "seed"),
    "plateau.csv": ("n", "engine", "lahc_length", "seed", "bucket"),
    "construction.csv": ("campaign", "n", "instance_seed", "solver_seed", "order"),
}
# Columns that legitimately differ between two runs of the same experiment.
VOLATILE = {"elapsed_ms"}
_EXP = re.compile(r"[eE][+-]?\d")


def load(path, key):
    with open(path, newline="") as f:
        return {tuple(r[k] for k in key): r for r in csv.DictReader(f)}


def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def same(old, new):
    """'exact', 'rounded' (six-digit rule for older records) or None."""
    if old == new:
        return "exact"
    a, b = _num(old), _num(new)
    if a is None or b is None:
        return None
    if a == b:
        return "exact"
    if _EXP.search(old) and float(f"{b:.6g}") == a:
        return "rounded"
    return None


def main(argv):
    complete = "--complete" in argv
    args = [a for a in argv if not a.startswith("--")]
    old_dir = args[0] if len(args) > 0 else "results"
    new_dir = args[1] if len(args) > 1 else "rr"

    bad = 0
    for name, key in KEYS.items():
        a, b = os.path.join(old_dir, name), os.path.join(new_dir, name)
        if not os.path.exists(a):
            continue
        if not os.path.exists(b):
            if complete:
                print(f"{name}: MISSING from {new_dir}")
                bad += 1
            continue
        old, new = load(a, key), load(b, key)
        shared = set(old) & set(new)
        missing = set(old) - set(new)
        extra = set(new) - set(old)
        cols = sorted((set(next(iter(old.values()))) & set(next(iter(new.values()))))
                      - VOLATILE - set(key))
        rows_ok = rows_bad = cells = cells_rounded = 0
        for k in sorted(shared):
            verdicts = [(c, same(old[k][c], new[k][c])) for c in cols]
            wrong = [c for c, v in verdicts if v is None]
            cells += len(verdicts)
            cells_rounded += sum(1 for _, v in verdicts if v == "rounded")
            if wrong:
                rows_bad += 1
                if rows_bad <= 3:
                    print(f"  DIFFERS {name} {k}: " +
                          ", ".join(f"{c} {old[k][c]!r} -> {new[k][c]!r}" for c in wrong))
            else:
                rows_ok += 1
        note = f", {cells_rounded} of {cells} cells equal only after six-digit rounding" \
            if cells_rounded else ""
        print(f"{name}: {len(shared)}/{len(old)} rows re-run; {rows_ok} identical, "
              f"{rows_bad} different, over {len(cols)} shared columns{note}")
        if extra:
            print(f"  {len(extra)} row(s) in the re-run that the shipped file does not have")
        if missing and complete:
            print(f"  {len(missing)} shipped row(s) missing from the re-run")
            bad += len(missing)
        bad += rows_bad + len(extra)
    print("\n" + ("re-run reproduces the shipped records" if not bad
                  else f"{bad} problem(s) -- do NOT swap the records in"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
