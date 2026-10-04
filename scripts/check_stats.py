#!/usr/bin/env python3
"""Check the statistics in `ttstats.py` against definitions rather than against another library.

`ttstats.py` uses the standard library only, so that the analysis has no dependency a reader has to
install and no version that can drift. The price of writing the tests oneself is that they have to be
checked, and the only honest way to check an exact test is to enumerate the null distribution.

    python3 scripts/check_stats.py

Every check either enumerates the exact distribution by brute force, or compares against a value that
can be computed by hand. Exits non-zero on any failure.
"""
import itertools
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ttstats as t  # noqa: E402

failures = []


def check(name, got, want, tol=1e-9):
    ok = abs(got - want) <= tol
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}: got {got!r}, want {want!r}")
    if not ok:
        failures.append(name)


def brute_wilcoxon(diffs):
    """The definition: rank the absolute differences, then enumerate every signing of the ranks."""
    n = len(diffs)
    order = sorted(range(n), key=lambda i: abs(diffs[i]))
    ranks = [0] * n
    for pos, i in enumerate(order):
        ranks[i] = pos + 1
    wp = sum(ranks[i] for i in range(n) if diffs[i] > 0)
    wm = sum(ranks[i] for i in range(n) if diffs[i] < 0)
    w = min(wp, wm)
    cnt = sum(1 for signs in itertools.product([0, 1], repeat=n)
              if sum(ranks[i] for i in range(n) if signs[i]) <= w)
    return w, min(1.0, 2.0 * cnt / 2 ** n)


def main():
    print("Wilcoxon signed-rank, exact branch, against enumeration of the null distribution")
    random.seed(20260913)
    trials = 0
    for _ in range(400):
        n = random.randint(4, 10)
        diffs = [d for d in random.sample(range(-60, 60), n) if d != 0]
        if len(set(abs(d) for d in diffs)) != len(diffs):
            continue                                   # the exact branch requires distinct |d|
        trials += 1
        w, p = t.wilcoxon_signed_rank(diffs, [0] * len(diffs))
        w2, p2 = brute_wilcoxon(diffs)
        if abs(w - w2) > 1e-9 or abs(p - p2) > 1e-12:
            failures.append(f"wilcoxon {diffs}")
            print(f"  FAIL  {diffs}: ({w},{p}) != ({w2},{p2})")
    print(f"  ok    {trials} random samples agree with the enumerated distribution")

    print("Wilcoxon: the extreme case")
    _, p = t.wilcoxon_signed_rank([9, 19, 29, 39, 49, 59], [0] * 6)
    check("every difference positive, n=6", p, 2 / 64)

    print("Sign test")
    check("8 of 10 positive", t.sign_test([1] * 8 + [-1] * 2, [0] * 10)[1],
          2 * sum(math.comb(10, k) for k in range(3)) / 2 ** 10)

    print("Vargha--Delaney A12")
    check("identical samples", t.vargha_delaney([1, 2, 3], [1, 2, 3]), 0.5)
    check("strictly greater", t.vargha_delaney([4, 5, 6], [1, 2, 3]), 1.0)
    check("strictly smaller", t.vargha_delaney([1, 2, 3], [4, 5, 6]), 0.0)

    print("Holm step-down")
    got = dict(t.holm([("a", 0.01), ("b", 0.04), ("c", 0.03)]))
    check("smallest p times m", got["a"], 0.03)
    check("monotone, so b is raised to c's", got["b"], 0.06)
    check("middle p times m-1", got["c"], 0.06)

    print("Chi-squared survival, at the tabulated 5% critical values")
    for x, df in [(3.8415, 1), (5.9915, 2), (7.8147, 3), (9.4877, 4)]:
        check(f"chi2_sf({x}, {df})", t.chi2_sf(x, df), 0.05, tol=5e-5)
    check("chi2_sf(0, 1)", t.chi2_sf(0.0, 1), 1.0)

    print("Friedman, against the closed form")
    chi, df, _, ranks = t.friedman([[1, 2, 3]] * 6)
    n, k, rs = 6, 3, [6, 12, 18]
    closed = 12.0 / (n * k * (k + 1)) * sum(r * r for r in rs) - 3 * n * (k + 1)
    check("perfect agreement, 6 blocks", chi, closed, tol=1e-9)
    check("degrees of freedom", df, 2)
    chi, _, _, ranks = t.friedman([[1, 2, 3], [3, 2, 1]] * 2)
    check("no agreement gives zero", chi, 0.0)
    _, _, _, ranks = t.friedman([[1, 1, 2]] * 5)
    check("tied values share a mean rank", ranks[0], 1.5)

    print("Bootstrap interval")
    random.seed(3)
    xs = [random.gauss(10, 2) for _ in range(200)]
    lo, hi = t.bootstrap_ci(xs, t.median, 2000)
    inside = lo <= t.median(xs) <= hi
    print(f"  {'ok  ' if inside else 'FAIL'}  the interval contains the point estimate")
    if not inside:
        failures.append("bootstrap")

    print("Block bootstrap interval")
    # Two properties the article relies on.  With blocks whose statistic is identical in every
    # resample -- here a paired ratio that is the same on every block -- the interval has to
    # collapse onto the point; and with blocks that differ it has to bracket the point.  The
    # second also checks that whole blocks are resampled: drawing the two members of a pair
    # independently would break the constant ratio and widen the first interval.
    same = [(2.0 * x, x) for x in (3.0, 7.0, 11.0, 5.0, 9.0, 13.0, 2.0, 4.0)]
    pt, lo, hi = t.block_bootstrap_ci(same, lambda bs: t.median([a / b for a, b in bs]))
    ok = (pt == 2.0 and lo == 2.0 and hi == 2.0)
    print(f"  {'ok  ' if ok else 'FAIL'}  a constant paired ratio gives a degenerate interval")
    if not ok:
        failures.append("block bootstrap constant")
    random.seed(11)
    mixed = [(random.gauss(12, 3), random.gauss(10, 2)) for _ in range(40)]
    pt, lo, hi = t.block_bootstrap_ci(mixed, lambda bs: t.median([a / b for a, b in bs]))
    ok = lo <= pt <= hi and lo < hi
    print(f"  {'ok  ' if ok else 'FAIL'}  a varying paired ratio is bracketed by its interval")
    if not ok:
        failures.append("block bootstrap varying")

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        return 1
    print("all statistics checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
