#!/usr/bin/env python3
"""Does the design validator in analyse.py actually catch a changed experiment?

A validator that reads its expectations out of its own input passes everything, and the way that
is discovered is usually by someone removing a level and noticing that nothing happened.  So the
removals are done here, deliberately, on throwaway copies of the published records, and each one
has to make `analyse.py` exit non-zero.  Run it with no arguments from anywhere:

    python3 scripts/check_design.py [--results DIR]

It never writes to the results directory it is pointed at.
"""

import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ANALYSE = os.path.join(HERE, "analyse.py")


def run_analyse(results, out):
    p = subprocess.run([sys.executable, ANALYSE, "--results", results, "--out", out],
                       capture_output=True, text=True, env={**os.environ, "TT_MIN_AGE": "0"})
    return p.returncode, p.stderr


def _write(path, rows, fieldnames):
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def rewrite(path, keep):
    rows = list(csv.DictReader(open(path)))
    kept = [r for r in rows if keep(r)]
    assert len(kept) < len(rows), f"mutation removed nothing from {path}"
    _write(path, kept, rows[0].keys())


def duplicate_first(path):
    rows = list(csv.DictReader(open(path)))
    _write(path, [rows[0]] + rows, rows[0].keys())


def blank_field(path, key, which=0):
    rows = list(csv.DictReader(open(path)))
    rows[which][key] = ""
    _write(path, rows, rows[0].keys())


def touch(path):
    os.utime(path, None)


MUTATIONS = [
    ("a factor level removed at both sizes",
     lambda d: rewrite(os.path.join(d, "factors.csv"),
                       lambda r: not (r["factor"] == "history-length" and r["level"] == "50000"))),
    ("a whole instance size removed from the ladder",
     lambda d: rewrite(os.path.join(d, "budget.csv"), lambda r: r["n"] != "1600")),
    ("one block removed from every cell of the crossed campaign",
     lambda d: rewrite(os.path.join(d, "anchor.csv"), lambda r: r["seed"] != "12")),
    ("the top rung removed from the ladder",
     lambda d: rewrite(os.path.join(d, "budget.csv"), lambda r: r["budget"] != "1600000")),
    ("the construction audit removed",
     lambda d: os.remove(os.path.join(d, "construction.csv"))),
    ("the construction audit reduced to one campaign's tuples",
     lambda d: rewrite(os.path.join(d, "construction.csv"),
                       lambda r: r["campaign"] == "factors")),
    ("a construction-audit tuple recorded twice",
     lambda d: duplicate_first(os.path.join(d, "construction.csv"))),
    ("a construction-audit unplaced count blanked",
     lambda d: blank_field(os.path.join(d, "construction.csv"), "unplaced")),
    ("the acceptance-trace file removed",
     lambda d: os.remove(os.path.join(d, "plateau.csv"))),
    ("one acceptance-trace run removed",
     lambda d: rewrite(os.path.join(d, "plateau.csv"),
                       lambda r: not (r["n"] == "1600" and r["engine"] == "lahc"
                                      and r["lahc_length"] == "15000" and r["seed"] == "5"))),
    ("an acceptance-trace bin recorded twice",
     lambda d: duplicate_first(os.path.join(d, "plateau.csv"))),
]

# The age guard is tested separately, because it is the one fault a mutation of the *contents*
# cannot produce: the file is complete and correct, and only its timestamp is wrong.  Publication
# mode must refuse it rather than skip the campaign and report success.
AGE_TEST = ("a complete factor file written moments ago",
            lambda d: touch(os.path.join(d, "factors.csv")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(HERE, "..", "results"))
    args = ap.parse_args()
    src = os.path.abspath(args.results)
    if not os.path.isdir(src):
        print(f"no results directory at {src}", file=sys.stderr)
        return 2

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        # The unmodified records must pass, or every failure below proves nothing.
        base = os.path.join(tmp, "base")
        shutil.copytree(src, base)
        code, err = run_analyse(base, os.path.join(tmp, "base-out"))
        if code != 0:
            print("FAIL: the unmodified records do not pass validation:\n" + err, file=sys.stderr)
            return 1
        print("ok    unmodified records pass")

        for i, (name, mutate) in enumerate(MUTATIONS):
            d = os.path.join(tmp, f"m{i}")
            shutil.copytree(src, d)
            mutate(d)
            code, err = run_analyse(d, os.path.join(tmp, f"m{i}-out"))
            if code == 0:
                print(f"FAIL  {name}: analyse.py exited 0", file=sys.stderr)
                failures += 1
            else:
                first = next((l for l in err.splitlines() if l.startswith("GRID FAULT")), "")
                print(f"ok    {name}: caught ({first[:88]})")
            # And development mode must still let the author look at it.
            code2 = subprocess.run(
                [sys.executable, ANALYSE, "--results", d, "--out", os.path.join(tmp, f"m{i}-dev"),
                 "--development"],
                capture_output=True, text=True,
                env={**os.environ, "TT_MIN_AGE": "0"}).returncode
            if code2 != 0:
                print(f"FAIL  {name}: --development also refused", file=sys.stderr)
                failures += 1

        # The age guard, with the real default threshold rather than TT_MIN_AGE=0, and a check
        # that a refusal leaves the previous outputs where they were.
        name, mutate = AGE_TEST
        d = os.path.join(tmp, "age")
        out = os.path.join(tmp, "age-out")
        shutil.copytree(src, d)
        run_analyse(d, out)                       # populate the output directory first
        before = sorted(os.listdir(os.path.join(out, "tables")))
        mutate(d)
        p = subprocess.run([sys.executable, ANALYSE, "--results", d, "--out", out],
                           capture_output=True, text=True,
                           env={k: v for k, v in os.environ.items() if k != "TT_MIN_AGE"})
        if p.returncode == 0:
            print(f"FAIL  {name}: analyse.py exited 0", file=sys.stderr)
            failures += 1
        else:
            print(f"ok    {name}: caught")
        after = sorted(os.listdir(os.path.join(out, "tables")))
        if after != before:
            print(f"FAIL  {name}: the refusal changed the output directory", file=sys.stderr)
            failures += 1
        else:
            print("ok    a refusal leaves the previous outputs untouched")

    print(f"{len(MUTATIONS) + 1} mutations, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
