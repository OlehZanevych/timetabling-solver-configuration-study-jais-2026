#!/usr/bin/env python3
"""Tables, figure data and inline numbers for the configuration study."""

import argparse
import collections
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ttstats as t  # noqa: E402

FACTOR_TITLES = {
    "acceptance": "the acceptance criterion",
    "history-length": "the acceptance history length $\\ell$",
    "escape-ladder": "the escape ladder",
    "kick-anchor": "where the perturbation is anchored",
    "kick-strength": "how strong the perturbation is",
    "construction": "the construction order",
    "infeasibility-price": "the price of infeasibility $\\lambda$",
    "restart-threshold": "when to reconstruct",
}

# Short names for the bar chart.  The long titles are what Table 5 prints; on a figure axis they
# force type small enough to be unreadable, and the mapping between the two is one-to-one.
FACTOR_SHORT = {
    "acceptance": "acceptance criterion",
    "history-length": "history length $\\ell$",
    "escape-ladder": "escape ladder",
    "kick-anchor": "kick anchor",
    "kick-strength": "kick strength",
    "construction": "construction order",
    "infeasibility-price": "price of infeasibility",
    "restart-threshold": "restart threshold",
}

REFERENCE = {
    "acceptance": "lahc",
    "history-length": "100",
    "escape-ladder": "deep+kick+fresh",
    "kick-anchor": "incumbent",
    "kick-strength": "adaptive-12-300",
    "construction": "most-constrained",
    "infeasibility-price": "scale-free",
    "restart-threshold": "150k",
}

# ---- the intended design --------------------------------------------------------------------
#
# Everything below is the design the published article reports, written down explicitly rather than
# inferred from whatever happens to be in the input.  A validator that reads its own expectations
# out of the data can only catch a design that is internally ragged; it cannot catch a level, a
# size or a block that is absent from every cell, because then nothing in the file mentions it.
# That is exactly the failure mode that silently changes an experiment between one regeneration and
# the next, so the expectations are stated here and checked against the records.
#
# The levels must match factors() in src/main.cpp; the block counts and budgets must match
# scripts/run_all.sh; the solver seeds must match the drivers in src/main.cpp.  A reduced design --
# run_all.sh --quick, or a campaign still in flight -- is a legitimate thing to look at, and
# --development turns these requirements into warnings for that purpose.
SIZES = ["400", "1600"]

DESIGN_FACTORS = {
    "acceptance": ["lahc", "lahc-canonical", "dlas", "schc", "sa", "hill-climbing"],
    "history-length": ["10", "50", "100", "500", "5000", "50000"],
    "escape-ladder": ["none", "deep", "deep+kick", "deep+kick+fresh"],
    "kick-anchor": ["incumbent", "working-state"],
    "kick-strength": ["fixed-12", "fixed-60", "fixed-300", "adaptive-12-300", "adaptive-6-100"],
    "construction": ["most-constrained", "random"],
    "infeasibility-price": ["scale-free", "fixed-1e5", "fixed-1e6", "fixed-1e8"],
    "restart-threshold": ["60k", "150k", "400k", "1200k"],
}

# campaign -> (blocks, instance seed of block i, solver seed of block i, construction orders)
DESIGN_CAMPAIGNS = {
    "factors": (10, lambda s: s, lambda s: 424242 + s, ["mrv", "random"]),
    "anchor": (12, lambda s: s, lambda s: 55055 + s, ["mrv"]),
    "budget": (6, lambda s: s, lambda s: 90909 + s, ["mrv"]),
    "plateau": (5, lambda s: s, lambda s: 606 + s, ["mrv"]),
    "dispersion": (20, lambda s: 1, lambda s: 700000 + s, ["mrv"]),
}

# The proposal budget of every factor run, which the factor CSV does not repeat per row.
DESIGN_FACTOR_BUDGET = 200000

DESIGN_ANCHOR = (["incumbent", "working-state"],
                 ["refill-at-kick", "keep", "reset-to-incumbent"])
DESIGN_BUDGET_ARMS = ["full", "no-escape", "hill-climbing"]
DESIGN_BUDGET_LADDER = ["25000", "50000", "100000", "200000", "400000", "800000", "1600000"]
DESIGN_DISPERSION_BUDGETS = ["50000", "200000", "800000"]
DESIGN_TRACE_ENGINES = ["lahc", "dlas", "schc"]
DESIGN_TRACE_LENGTHS = [100, 1000, 5000, 15000]


def key(r):
    return t.num(r, "hard") * 1e9 + t.num(r, "objective")


def feasible(r):
    """Did this run return a complete, structurally legal, hard-violation-free schedule?

    Three conditions, not one.  H = 0 says no hard penalty was incurred by the classes that were
    placed; it says nothing about classes left unplaced, and nothing about the structural filters
    -- an unavailable room, a per-day load cap, an immovable class -- which are enforced outside
    the penalty terms and checked by the independent validator.  A schedule that leaves a class
    unplaced can have H = 0 and is not a timetable.

    Every per-run file now stores both `unplaced` and `clean` for each run, and main() refuses to
    generate from a file that lacks them.  The tests below are still written to use only what a
    row carries, so that --development can look at older records.

    The constructor bound: the returned schedule is the lexicographic minimum under
    (unplaced, hard, objective) of every incumbent the run held, the first of which is the
    constructed schedule, so it cannot have more unplaced classes than the construction did.  The
    `construction` experiment measures that on the actual campaign/instance-seed/solver-seed/order
    tuples, and main() refuses to generate unless every one of those tuples is present and came out
    at zero unplaced.  It is a second, independent route to completeness beside the stored count.
    """
    if t.num(r, "hard") > 0:
        return False
    if "unplaced" in r and t.num(r, "unplaced") > 0:
        return False
    if "clean" in r and t.num(r, "clean") < 1:
        return False
    return True


def _pless(txt, bound):
    """Is a formatted p-value below a bound?  t.fmt_p emits a number or a "<" form."""
    txt = str(txt).strip().lstrip("$").lstrip("<").strip()
    try:
        return float(txt) < bound
    except ValueError:
        return txt.startswith("<") or "0.001" in txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--allow-gaps", "--development", dest="allow_gaps", action="store_true",
                    help="development mode: generate tables even when the records do not hold the "
                         "design declared at the top of this file.  The default is publication "
                         "mode, which refuses, so that a publication table cannot be built from a "
                         "half-finished, reduced or silently altered campaign")
    ap.add_argument("--out", default=None,
                    help="where tables/ and figures/ are written; defaults to the article directory "
                         "above this one when it holds a main.tex, and otherwise to this directory, "
                         "so that the application stands alone when published on its own")
    args = ap.parse_args()
    if args.out is None:
        args.out = ".." if os.path.exists(os.path.join("..", "main.tex")) else "."
    R, TAB, FIG = args.results, os.path.join(args.out, "tables"), os.path.join(args.out, "figures")
    os.makedirs(TAB, exist_ok=True)
    os.makedirs(FIG, exist_ok=True)
    facts = {}

    # Before anything is aggregated, check that the records hold the design declared at the top of
    # this file.  The age guard in t.complete() catches a file still being written; it does not
    # catch a campaign that was interrupted and restarted, nor one whose level list was quietly
    # edited between two regenerations.  Expectations read out of the input itself cannot catch the
    # second case at all -- a level absent at both sizes leaves nothing in the file to notice its
    # absence -- so what every cell is checked against is DESIGN_*, not the file's own contents.
    faults = []

    def readable(name):
        """Is this required file present AND finished being written?

        t.complete()'s age guard exists so that a table is never built from a campaign still in
        flight.  Left to itself it makes the analysis *skip* that file and exit successfully, which
        in publication mode is the worse failure: the outputs have already been cleared, so a
        regeneration started a minute too early silently produces a paper with tables missing.  So
        a required file that cannot be read is a fault here, before anything is cleared, and not a
        skipped section later.
        """
        pth = os.path.join(R, name)
        if not os.path.exists(pth):
            faults.append(f"{name}: missing; the published design needs it")
            return False
        if not t.complete(pth):
            faults.append(f"{name}: written too recently to be read as finished; wait for the "
                          f"campaign to end, or set TT_MIN_AGE=0 if it has")
            return False
        return True

    def whole(name, row, key, lo=0):
        """Read a field that is used as evidence, refusing anything that is not a whole number.

        t.num() returns 0.0 for a blank or malformed field, which is the right default for an
        optional column and exactly the wrong one for `unplaced`: it would turn missing evidence
        into evidence of completeness.  Every such field goes through here instead.
        """
        raw = row.get(key)
        try:
            v = float(raw)
        except (TypeError, ValueError):
            faults.append(f"{name}: field {key!r} is {raw!r}, which is not a number; "
                          f"missing evidence is not zero")
            return None
        if not math.isfinite(v) or v != int(v) or v < lo:
            faults.append(f"{name}: field {key!r} is {raw!r}, expected a whole number >= {lo}")
            return None
        return int(v)

    def want(name, cell, unit, expected_cells, blocks, required=True):
        """Every cell of `expected_cells` must hold exactly `blocks`, once each."""
        pth = os.path.join(R, name)
        if not os.path.exists(pth):
            if required:
                faults.append(f"{name}: missing; the published design needs it")
            return None
        rows_ = t.read(pth)
        seen = collections.defaultdict(list)
        for r in rows_:
            seen[tuple(r[k] for k in cell)].append(r[unit])
        for k in expected_cells:
            got = seen.get(k, [])
            if sorted(set(got)) != sorted(blocks):
                faults.append(f"{name}: cell {k} has {unit}s {sorted(set(got))}, "
                              f"expected {sorted(blocks)}")
            dup = [u for u, c in collections.Counter(got).items() if c > 1]
            if dup:
                faults.append(f"{name}: cell {k} has duplicate {unit}s {sorted(dup)}")
        extra = sorted(set(seen) - set(expected_cells))
        if extra:
            faults.append(f"{name}: unexpected cells not in the declared design: {extra}")
        return rows_

    for required_file in ("factors.csv", "anchor.csv", "budget.csv", "dispersion.csv",
                          "plateau.csv", "construction.csv"):
        readable(required_file)

    fblocks = [str(s) for s in range(1, DESIGN_CAMPAIGNS["factors"][0] + 1)]
    want("factors.csv", ("factor", "level", "n"), "seed",
         [(f, lv, n) for f, lvs in DESIGN_FACTORS.items() for lv in lvs for n in SIZES], fblocks)
    want("anchor.csv", ("anchor", "history_policy", "n"), "seed",
         [(a, p, n) for a in DESIGN_ANCHOR[0] for p in DESIGN_ANCHOR[1] for n in SIZES],
         [str(s) for s in range(1, DESIGN_CAMPAIGNS["anchor"][0] + 1)])
    want("budget.csv", ("arm", "n", "budget"), "seed",
         [(a, n, b) for a in DESIGN_BUDGET_ARMS for n in SIZES for b in DESIGN_BUDGET_LADDER],
         [str(s) for s in range(1, DESIGN_CAMPAIGNS["budget"][0] + 1)])
    want("dispersion.csv", ("n", "budget"), "seed",
         [(n, b) for n in SIZES for b in DESIGN_DISPERSION_BUDGETS],
         [str(s) for s in range(1, DESIGN_CAMPAIGNS["dispersion"][0] + 1)])

    # --- stored per-run evidence ------------------------------------------------------------------
    # Every per-run file stores, for each run, the validator's structural verdict (`clean`), its
    # count of classes left unplaced, and the escape counters; the three budgeted campaigns also
    # store the proposals actually spent.  The article states that every run was checked on the
    # first two and that every budgeted run spent exactly its budget, so those statements are
    # computed here, from strictly parsed fields, rather than asserted.  A missing column is a
    # fault: the sentence it supports would otherwise be printed without its evidence.
    evid = {"factors.csv": "fac", "anchor.csv": "anchor", "budget.csv": "ladder",
            "dispersion.csv": "disp"}
    runs_all = runs_complete = budget_runs = 0
    for name, tag in evid.items():
        pth = os.path.join(R, name)
        if not os.path.exists(pth):
            continue
        rows_ = t.read(pth)
        if not rows_:
            continue
        missing = [c for c in ("unplaced", "clean", "adoptions") if c not in rows_[0]]
        if missing:
            faults.append(f"{name}: no {', '.join(missing)} column; re-run the campaign with the "
                          f"current driver, which records them for every run")
            continue
        complete = adopted = 0
        for r in rows_:
            u = whole(name, r, "unplaced")
            c = whole(name, r, "clean")
            a = whole(name, r, "adoptions")
            if None in (u, c, a):
                break
            complete += (u == 0 and c >= 1)
            adopted += (a > 0)
        runs_all += len(rows_)
        runs_complete += complete
        facts[tag + "AdoptRuns"] = str(adopted)
        facts[tag + "AdoptMax"] = t.fmt(max(t.num(r, "adoptions") for r in rows_), 0)
        facts[tag + "EvidenceRuns"] = str(len(rows_))
        if name != "anchor.csv":
            if "candidates" not in rows_[0]:
                faults.append(f"{name}: no candidates column, so the achieved budget cannot be "
                              f"checked")
                continue
            short = 0
            for r in rows_:
                spent = whole(name, r, "candidates")
                if spent is None:
                    break
                limit = int(t.num(r, "budget")) if "budget" in r else DESIGN_FACTOR_BUDGET
                short += (spent != limit)
            if short:
                faults.append(f"{name}: {short} run(s) did not spend exactly their budget in "
                              f"proposals; the article says every run did")
            budget_runs += len(rows_)
    facts["adoptMax"] = t.fmt(max([0.0] + [float(facts[g + "AdoptMax"]) for g in evid.values()
                                            if g + "AdoptMax" in facts]), 0)
    if runs_complete != runs_all:
        faults.append(f"{runs_all - runs_complete} run(s) left a class unplaced or failed the "
                      f"structural validator; the article says every run passed both")
    facts["runsStored"] = str(runs_all)
    facts["runsComplete"] = str(runs_complete)
    facts["budgetRunsChecked"] = str(budget_runs)

    # The acceptance-trace campaign cannot go through want(), because its file is not one row per
    # run: it is one row per occupied age bin of a run, and which bins are occupied depends on the
    # trajectory.  So what is checked is the set of run identities, that no (run, bin) key appears
    # twice, and that the counts in each bin are whole numbers that nest the way they must.
    path = os.path.join(R, "plateau.csv")
    if os.path.exists(path):
        prows = t.read(path)
        runs = collections.Counter()
        binkeys = collections.Counter()
        for r in prows:
            runs[(r["n"], r["engine"], r["lahc_length"], r["seed"])] += 1
            binkeys[(r["n"], r["engine"], r["lahc_length"], r["seed"], r["bucket"])] += 1
        want_runs = {(n, e, str(L), str(s))
                     for n in SIZES for e in DESIGN_TRACE_ENGINES
                     for L in DESIGN_TRACE_LENGTHS
                     for s in range(1, DESIGN_CAMPAIGNS["plateau"][0] + 1)}
        for k in sorted(want_runs - set(runs)):
            faults.append(f"plateau.csv: no trace rows for run {k}")
        for k in sorted(set(runs) - want_runs):
            faults.append(f"plateau.csv: run {k} is not in the declared design")
        dupbins = sorted(k for k, c in binkeys.items() if c > 1)
        if dupbins:
            faults.append(f"plateau.csv: {len(dupbins)} duplicated (run, bin) key(s), "
                          f"first {dupbins[0]}")
        for r in prows[:]:
            tot = whole("plateau.csv", r, "total", lo=1)
            off = whole("plateau.csv", r, "uphill_offered")
            acc = whole("plateau.csv", r, "uphill_accepted")
            if None in (tot, off, acc):
                break            # the field report is already a fault; one is enough
            if not (acc <= off <= tot):
                faults.append(f"plateau.csv: bin {r['bucket']} of run "
                              f"({r['n']}, {r['engine']}, {r['lahc_length']}, {r['seed']}) has "
                              f"accepted {acc}, offered {off}, total {tot}")
                break
        facts["plateauRuns"] = str(len(runs))

    # --- the bound on unplaced classes ------------------------------------------------------------
    # See feasible() above: an independent bound on unplaced classes for every run, beside the
    # per-run count each record now stores.
    #
    # The construction a run starts from is a function of the generated instance, the construction
    # order AND the solver seed, because construct() shuffles its order with the worker's random
    # number generator.  An audit indexed by instance seed alone would certify the wrong
    # constructions for four of the five campaigns.  So the tuples required here are enumerated
    # from the declared design, and the file has to contain every one of them.
    need = set()
    for camp, (nblocks, iseed, sseed, orders) in DESIGN_CAMPAIGNS.items():
        for s in range(1, nblocks + 1):
            for order in orders:
                for n in SIZES:
                    need.add((camp, n, str(iseed(s)), str(sseed(s)), order))
    path = os.path.join(R, "construction.csv")
    if os.path.exists(path):
        crows = t.read(path)
        have = collections.Counter(
            (r["campaign"], r["n"], r["instance_seed"], r["solver_seed"], r["order"])
            for r in crows)
        for k in sorted(need - set(have)):
            faults.append(f"construction.csv: no audit row for {k}")
        for k in sorted(set(have) - need):
            faults.append(f"construction.csv: audit row {k} is not in the declared design")
        dup = sorted(k for k, c in have.items() if c > 1)
        if dup:
            faults.append(f"construction.csv: {len(dup)} tuple(s) audited more than once, "
                          f"first {dup[0]}; every tuple must appear exactly once")
        # Each row's evidence is parsed strictly.  A blank or malformed `unplaced` is missing
        # evidence, and missing evidence must not read as a zero.
        bad = 0
        for r in crows:
            u = whole("construction.csv", r, "unplaced")
            c = whole("construction.csv", r, "clean")
            if u is None or c is None:
                break
            if u > 0 or c < 1:
                bad += 1
        if bad:
            faults.append(f"construction.csv: {bad} construction(s) left a class unplaced or "
                          f"failed the structural validator; the completeness bound in the paper "
                          f"does not hold")
        facts["constrRuns"] = str(len(crows))
        facts["constrTuples"] = str(len(need))
        facts["constrCampaigns"] = str(len({r["campaign"] for r in crows}))
        facts["constrMaxUnplaced"] = t.fmt(max(t.num(r, "unplaced") for r in crows), 0)
        facts["constrClean"] = str(sum(1 for r in crows if t.num(r, "clean") >= 1))
    else:
        # Not a warning.  The article reports this audit as the second route to completeness;
        # without the file that statement is unknown, not true.
        faults.append("construction.csv: missing; run `experiments construction`.  Without it the "
                      "constructor bound the article reports is unknown and no feasibility count "
                      "may be published")
        for k_ in ("constrRuns", "constrTuples", "constrCampaigns", "constrMaxUnplaced",
                   "constrClean"):
            facts[k_] = "\\textbf{unknown}"

    if faults:
        for f_ in faults:
            print(f"GRID FAULT {f_}", file=sys.stderr)
        if not args.allow_gaps:
            print(f"{len(faults)} grid fault(s); refusing to generate. Re-run the affected "
                  f"experiment, or pass --development to look at a reduced design.",
                  file=sys.stderr)
            return 1

    # Only now.  Everything in those two directories is generated here, and clearing them first
    # means a table whose results file has been withdrawn -- because the experiment that produced
    # it turned out to measure the wrong thing -- cannot survive on disk and go on appearing in the
    # paper, sourced from data that no longer exists.  But clearing them *before* the design is
    # checked would let a refusal leave the article with no tables at all, so the refusal above
    # happens first and leaves the previous outputs untouched.
    t.reset_outputs(TAB, FIG)

    # --- dispersion: the resolution limit of everything else -------------------------------------
    path = os.path.join(R, "dispersion.csv")
    if t.complete(path):
        rows = t.read(path)
        by = collections.defaultdict(list)
        for r in rows:
            by[(int(r["n"]), int(r["budget"]))].append(r)
        drows = []
        dispCells = {}
        for k in sorted(by):
            v = by[k]
            soft = [t.num(r, "soft") for r in v]
            obj = [t.num(r, "objective") for r in v]
            dispCells[k] = obj
            hard = [t.num(r, "hard") for r in v]
            # The resolution limit has to be quoted in the same currency as the factor spreads it
            # is used to judge, and those are ratios of median OBJECTIVE.  Reporting it on the raw
            # soft count understates it, because the objective is quadratic in the counts.
            drows.append([k[0], k[1], len(v), t.fmt(sum(1 for r in v if not feasible(r)), 0),
                          t.fmt(t.median(obj), 0), t.fmt(min(obj), 0), t.fmt(max(obj), 0),
                          t.fmt(max(obj) / max(1.0, min(obj)), 2),
                          t.fmt(t.iqr(obj) / max(1.0, t.median(obj)), 3)])
            facts[f"dispRatio{k[0]}b{k[1]}"] = t.fmt(max(obj) / max(1.0, min(obj)), 2)
            facts[f"dispRatioSoft{k[0]}b{k[1]}"] = t.fmt(max(soft) / max(1.0, min(soft)), 2)
        t.write_table(
            os.path.join(TAB, "dispersion.tex"),
            "The spread of one configuration on one fixed instance at one budget, over twenty "
            "solver seeds, in the objective $f$. \\emph{worst/best} says how far one run can be "
            "from another and is what a practitioner feels. \\emph{infeas.} counts the runs of "
            "the cell that did not reach feasibility, and \\emph{rel.\\ IQR} is the interquartile "
            "range of $f$ over its median. This table is descriptive context for how noisy a "
            "single run is on this problem; it is not the reference distribution for the factor "
            "comparisons, which are paired across instances and carry their own intervals.",
            "tab:dispersion",
            ["$n$", "budget", "runs", "infeas.", "median $f$", "best $f$", "worst $f$",
             "worst/best", "rel.\\ IQR"],
            drows, align="rrrrrrrrr")
        # How stable a median of ten is, on the same runs.  This is quoted in the paper as a second
        # descriptive statement about seed noise -- a ratio of medians is much steadier than a ratio
        # of single runs -- and NOT as a detection threshold for the factor comparisons.  It could
        # not serve as one: it is computed by splitting one configuration's runs on ONE instance,
        # so it holds the instance fixed, while every factor comparison varies the instance and
        # pairs on it; it ignores that pairing; it is pooled over sizes and budgets rather than
        # being a cell-specific null; and it takes no account of the best-versus-worst selection
        # among a factor's levels or of the selection of the size at which that spread is largest.
        # The uncertainty a factor difference is read against is the per-comparison block bootstrap
        # interval computed with each factor table instead.
        import itertools as _it
        import random as _rnd
        ratios = []
        for kk, vv in sorted(dispCells.items()):
            if len(vv) < 20:
                continue
            rng = _rnd.Random(20260914)
            for _ in range(2000):
                idx = list(range(len(vv)))
                rng.shuffle(idx)
                h = len(vv) // 2
                m1 = t.median([vv[i] for i in idx[:h]])
                m2 = t.median([vv[i] for i in idx[h:2 * h]])
                lo, hi = min(m1, m2), max(m1, m2)
                if lo > 0:
                    ratios.append(hi / lo)
        if ratios:
            ratios.sort()
            facts["dispMedianRatio"] = t.fmt(ratios[int(0.95 * len(ratios))], 2)
            facts["dispMedianRatioMed"] = t.fmt(t.median(ratios), 2)
            facts["dispMedianSplits"] = str(len(ratios))
            # The pooled 95th percentile is not the worst cell's.  A limit quoted as a single
            # number has to say which, or a reader will apply it to the cell it is loosest in.
            percell = []
            for kk, vv in sorted(dispCells.items()):
                if len(vv) < 20:
                    continue
                rr = []
                rng = _rnd.Random(20260914)
                for _ in range(2000):
                    idx = list(range(len(vv)))
                    rng.shuffle(idx)
                    h = len(vv) // 2
                    m1 = t.median([vv[i] for i in idx[:h]])
                    m2 = t.median([vv[i] for i in idx[h:2 * h]])
                    lo2, hi2 = min(m1, m2), max(m1, m2)
                    if lo2 > 0:
                        rr.append(hi2 / lo2)
                if rr:
                    rr.sort()
                    percell.append(rr[int(0.95 * len(rr))])
            if percell:
                facts["dispMedianRatioWorstCell"] = t.fmt(max(percell), 2)
        allr = [t.num(r, "soft") for r in rows]
        if allr:
            facts["dispWorstBest"] = t.fmt(max(
                float(v) for k, v in facts.items()
                if k.startswith("dispRatio") and not k.startswith("dispRatioSoft")), 2)
            facts["dispWorstBestSoft"] = t.fmt(max(
                float(v) for k, v in facts.items() if k.startswith("dispRatioSoft")), 2)

    # --- the factor sweep -------------------------------------------------------------------------
    path = os.path.join(R, "factors.csv")
    if t.complete(path):
        rows = t.read(path)
        by = collections.defaultdict(list)
        for r in rows:
            by[(r["factor"], int(r["n"]), r["level"])].append(r)
        factors = []
        for r in rows:
            if r["factor"] not in factors:
                factors.append(r["factor"])
        # The validator's structural verdict on every run of the study: a schedule that placed a
        # class outside its domain, used an unavailable room, broke a load cap or moved an
        # immovable entry would be reported here rather than scored.  The paper should be able to
        # say how many runs it checked, not only that it checks.
        if "clean" in rows[0]:
            facts["facRunsChecked"] = str(len(rows))
            facts["facRunsClean"] = str(sum(1 for r in rows if t.num(r, "clean") >= 1))
            facts["facRunsFeasible"] = str(sum(1 for r in rows if feasible(r)))
            facts["facRunsUnplaced"] = str(sum(1 for r in rows if t.num(r, "unplaced") > 0))
        # How often the kick actually fires under the study's REFERENCE escape counters, which is
        # what the crossing of sec:anchor had to be redesigned away from.  The section used to
        # assert that the kick never fired at the larger size; this is the measurement that
        # replaces the assertion, and it is taken from the one-at-a-time anchor arm, which runs at
        # the reference configuration.
        # It is the REFERENCE level of that factor that is the reference configuration; pooling the
        # two anchors would answer a different question from the one the sentence asks.
        for n in sorted({int(r["n"]) for r in rows if r["factor"] == "kick-anchor"}):
            v = [r for r in rows if r["factor"] == "kick-anchor" and int(r["n"]) == n
                 and r["level"] == REFERENCE["kick-anchor"]]
            if v and "kicks" in v[0]:
                tag = "Small" if n == min(int(r["n"]) for r in rows) else "Large"
                # Half-integer medians are real here: ten runs give a median between two counts,
                # and rounding one to an integer would hide how small these counts are.
                facts["anchorDefaultKicks" + tag] = t.fmt(
                    t.median([t.num(r, "kicks") for r in v]), 1)

        summary = []
        for fac in factors:
            levels = []
            for r in rows:
                if r["factor"] == fac and r["level"] not in levels:
                    levels.append(r["level"])
            ref = REFERENCE.get(fac, levels[0])
            frows = []
            spread = {}
            spread2 = {}
            for n in sorted({k[1] for k in by if k[0] == fac}):
                base = by.get((fac, n, ref), [])
                tests = []
                meds = {}
                for lv in levels:
                    v = by.get((fac, n, lv))
                    if not v:
                        continue
                    soft = [t.num(r, "soft") for r in v]
                    obj = [t.num(r, "objective") for r in v]
                    meds[lv] = t.median(obj)
                    # Feasibility is not a quality statistic and must not be folded into one.  A
                    # level that fails to reach H = 0 has a huge objective for that reason, so the
                    # count of runs that did reach it is reported beside the objective rather than
                    # left for a reader to infer from a median.
                    feas = sum(1 for r in v if feasible(r))
                    p = float("nan")
                    cell = "--"
                    if lv != ref and base:
                        a = {r["seed"]: key(r) for r in base}
                        b = {r["seed"]: key(r) for r in v}
                        ks = sorted(set(a) & set(b))
                        if len(ks) >= 5:
                            _, p = t.wilcoxon_signed_rank([a[k] for k in ks], [b[k] for k in ks])
                            tests.append((lv, p))
                        # The paired effect size, with an interval obtained by resampling whole
                        # instance blocks.  This is the quantity a reader needs and a p-value is
                        # not: how much worse (or better) the level is than the reference on the
                        # same instance, and how precisely this design pins that down.
                        ao = {r["seed"]: t.num(r, "objective") for r in base}
                        bo = {r["seed"]: t.num(r, "objective") for r in v}
                        kr = sorted(set(ao) & set(bo))
                        blocks = [(bo[k], ao[k]) for k in kr if ao[k] > 0]
                        if len(blocks) >= 5:
                            pt, lo_, hi_ = t.block_bootstrap_ci(
                                blocks, lambda bs: t.median([x / y for x, y in bs]))
                            # Two decimals on a ratio near one, one on a ratio in the tens,
                            # none above a hundred: the significant figures a reader can use,
                            # and a cell narrow enough for a 12.5 cm measure.
                            def _r(x):
                                # Always the same number of decimals within a cell, even when the
                                # value is a whole number: a table that prints "1" beside a text
                                # that quotes "1.00" invites a reader to look for a difference.
                                return ("%.2f" % x if x < 10 else
                                        "%.1f" % x if x < 100 else "%.0f" % x)
                            cell = (f"{_r(pt)} [{_r(lo_)}--{_r(hi_)}]"
                                    if lo_ == lo_ else _r(pt))
                    # The event columns are what tell a reader whether the mechanism under test
                    # actually engaged: a factor whose levels never fire is a null result of the
                    # protocol, not of the mechanism.  They are reported separately because a sum
                    # cannot show it -- a restart threshold whose reconstruction never fires looks
                    # busy if the deep phases of the same runs are added to its column.
                    ev = "/".join(t.fmt(t.median([t.num(r, c) for r in v]), 1)
                                  for c in ("deep_phases", "kicks", "fresh_restarts"))
                    frows.append([n, lv, f"{feas}/{len(v)}", t.fmt(t.median(soft), 0),
                                  t.fmt(t.median(obj), 0),
                                  t.fmt(t.iqr(obj) / max(1.0, t.median(obj)), 3),
                                  ev, cell, t.fmt_p(p)])
                # The table must print the SAME p the prose quotes, which is the corrected one:
                # the caption promises Holm within the instance size, so correct before writing.
                adjusted = dict(t.holm(tests))
                for row in frows:
                    if row[0] == n and row[1] in adjusted:
                        row[8] = t.fmt_p(adjusted[row[1]])
                for label, p in t.holm(tests):
                    facts["facP" + fac.replace("-", "") + str(n) +
                          label.replace("-", "").replace("+", "p")] = t.fmt_p(p)
                if meds:
                    lo, hi = min(meds.values()), max(meds.values())
                    spread[n] = hi / max(1.0, lo)
                    # worst/best is driven by whichever level fails outright, and a factor that
                    # happens to contain such a level looks enormous for that reason alone.  The
                    # ratio of the SECOND worst to the best answers the other question a designer
                    # has: what does a careless but not catastrophic choice cost?
                    ordered = sorted(meds.values())
                    second = ordered[-2] if len(ordered) >= 2 else ordered[-1]
                    spread2[n] = second / max(1.0, lo)
            t.write_table(
                os.path.join(TAB, f"factor-{fac}.tex"),
                f"Factor: {FACTOR_TITLES.get(fac, fac)}. Reference level \\texttt{{{ref}}}. "
                "\\emph{feas./runs} counts the runs that returned a complete, structurally legal "
                "schedule with no hard violation. \\emph{soft} and $f$ are "
                "medians and \\emph{IQR/$f$} is the interquartile range of $f$ over its median. "
                "\\emph{d/k/f} gives the median numbers of deep phases, kicks and reconstructions "
                "separately, because a level whose own mechanism never fires cannot be "
                "distinguished from one that has none. \\emph{ratio} is the median over instances "
                "of the level's $f$ divided by the reference's $f$ on the same instance, with a "
                "95\\% percentile interval from resampling whole instance blocks. $p$ is the "
                "two-sided exact Wilcoxon signed-rank test on $10^{9}H+f$, paired by instance and "
                "Holm-corrected within the instance size.",
                f"tab:factor-{fac}",
                ["$n$", "level", "feas./runs", "soft", "$f$", "IQR/$f$", "d/k/f",
                 "ratio [95\\,\\%]", "$p$"],
                frows, align="rlrrrrrlr", small=True)
            # How many of this factor's comparisons survive the correction.  The text must not
            # claim a factor is unresolved when one of its levels rejects.
            resolved = sum(1 for row in frows
                           if row[8] not in ("--", "") and _pless(row[8], 0.05))
            facts["facResolved" + fac.replace("-", "")] = str(resolved)
            facts["facComparisons" + fac.replace("-", "")] = str(
                sum(1 for row in frows if row[8] not in ("--", "")))
            # Feasibility, separately from quality: how many runs of this factor reached H = 0 at
            # each size, and which levels account for every failure.
            for n in sorted({k[1] for k in by if k[0] == fac}):
                tg = "FourHundred" if n == 400 else "SixteenHundred"
                sel = [r for r in rows if r["factor"] == fac and int(r["n"]) == n]
                facts["facFeas" + fac.replace("-", "") + tg] = str(
                    sum(1 for r in sel if feasible(r)))
                facts["facRuns" + fac.replace("-", "") + tg] = str(len(sel))
            # For the history length specifically, the paper makes a claim about where the optimum
            # sits.  Record the best level and the largest level that is *not* resolved as worse
            # than the reference, because the second is what the design can actually support.
            if fac == "acceptance":
                for n in sorted({k[1] for k in by if k[0] == fac}):
                    tg = "FourHundred" if n == 400 else "SixteenHundred"
                    rowsn = {r[1]: r for r in frows if r[0] == n}
                    if "hill-climbing" in rowsn and ref in rowsn:
                        hc = float(str(rowsn["hill-climbing"][4]).replace("e+", "e"))
                        rf = float(str(rowsn[ref][4]).replace("e+", "e"))
                        facts["accHcRatio" + tg] = t.fmt(hc / max(1.0, rf), 2)
            if fac == "history-length":
                for n in sorted({k[1] for k in by if k[0] == fac}):
                    tg = "FourHundred" if n == 400 else "SixteenHundred"
                    cells = [(lv, row) for lv, row in
                             [(r[1], r) for r in frows if r[0] == n]]
                    if not cells:
                        continue
                    best = min(cells, key=lambda c: float(str(c[1][4]).replace("e+", "e")))
                    facts["histBest" + tg] = str(best[0])
                    tolerated = [int(lv) for lv, row in cells
                                 if row[8] == "--" or not _pless(row[8], 0.05)]
                    facts["histTolerated" + tg] = str(max(tolerated)) if tolerated else "--"
                    # Feasibility, separately: the longest history at which every run still
                    # reached H = 0.  This is what the "stops reaching feasibility" sentence rests
                    # on, and it is a count of runs rather than a p-value.
                    feasLv = [int(lv) for lv, row in cells
                              if row[2].split("/")[0] == row[2].split("/")[1]]
                    facts["histFeasible" + tg] = str(max(feasLv)) if feasLv else "--"
            if spread:
                facts["spread2" + fac.replace("-", "")] = t.fmt(max(spread2.values()), 1)
                # For a two-level factor the second-worst level IS the best one, so the column is
                # 1.00 by construction and says nothing; print a dash rather than a number a reader
                # would compare with the others.
                two = len(levels) <= 2
                # Which size the quoted spread was taken at.  Without it the table looks like a
                # universal ranking, when it is a maximum over two sizes taken factor by factor.
                atSize = max(spread, key=lambda kk: spread[kk])
                summary.append([FACTOR_TITLES.get(fac, fac), len(levels), atSize,
                                "---" if two else t.fmt(max(spread2.values()), 1),
                                t.fmt(max(spread.values()), 1),
                                facts.get("facResolved" + fac.replace("-", ""), "--") + "/" +
                                facts.get("facComparisons" + fac.replace("-", ""), "--"),
                                FACTOR_SHORT.get(fac, fac)])
                facts["spread" + fac.replace("-", "")] = t.fmt(max(spread.values()), 1)
                facts["spreadAt" + fac.replace("-", "")] = str(atSize)
        # A descriptive grouping of the eight, by the size of the observed spread.  It is not a
        # significance statement and must not be read as one: which comparisons are resolved is
        # the last column of the summary table and the p column of each factor table.
        if summary:
            big = [r for r in summary if float(r[4]) >= 10]
            smallf = [r for r in summary if float(r[4]) < 1.5]
            facts["facTotal"] = str(len(summary))
            facts["facLargeCount"] = str(len(big))
            facts["facLargeNames"] = ", ".join(r[0] for r in big)
            facts["facSmallCount"] = str(len(smallf))
            facts["facSmallNames"] = ", ".join(r[0] for r in smallf)
            if smallf:
                facts["facSmallMax"] = t.fmt(max(float(r[4]) for r in smallf), 1)
            facts["facWithResolved"] = str(sum(
                1 for r in summary if r[5] != "--/--" and int(r[5].split("/")[0]) > 0))
        if summary:
            # Ordered by worst/best, which is defined for every factor.
            summary.sort(key=lambda s: -float(s[4]))
            t.write_table(
                os.path.join(TAB, "factor-summary.tex"),
                "The observed range of each factor, at the instance size where that range is "
                "largest; \\emph{at $n$} names that size. \\emph{worst/best} is the ratio of the "
                "worst level's median objective to the best level's, and is dominated by any "
                "level that fails outright; \\emph{2nd worst/best} excludes that level and "
                "answers the other question a designer has, which is what a careless but not "
                "catastrophic choice costs --- it is undefined for a two-level factor, where the "
                "second-worst level is the best one. \\emph{resolved} counts the level "
                "comparisons of that factor, over both sizes, whose Holm-corrected $p$ is below "
                "0.05. Both ratios depend on the levels chosen and on their number, so they rank "
                "the ranges this study varied rather than the factors themselves. Rows are "
                "ordered by \\emph{worst/best}.",
                "tab:factor-summary",
                ["factor", "levels", "at $n$", "2nd worst/best", "worst/best", "resolved"],
                [row[:6] for row in summary], align="lrrrrr")
            # The label column holds phrases with spaces ("the acceptance criterion").  pgfplots
            # splits an unbraced cell on whitespace and then reports every extra word as a
            # surplus column, so the tick labels are lost and the log fills with errors.  Braces
            # make each label one cell.
            t.write_dat(os.path.join(FIG, "factor-summary.dat"),
                        ["index", "ratio", "label"],
                        [[i, float(s[4]), "{%s}" % s[6]] for i, s in enumerate(summary)])
            facts["strongestFactor"] = summary[0][0]
            facts["strongestFactorRatio"] = summary[0][4]
            facts["secondFactor"] = summary[1][0] if len(summary) > 1 else "--"
            facts["secondFactorRatio"] = summary[1][4] if len(summary) > 1 else "--"
            facts["weakestFactor"] = summary[-1][0]
            facts["weakestFactorRatio"] = summary[-1][4]
            # The prose says "the remaining factors move it by at most X"; X must be the largest of
            # the remaining ones, not whichever of them the sentence happened to name.
            rest = summary[2:]
            if rest:
                facts["restFactorCount"] = str(len(rest))
                facts["restFactorMax"] = t.fmt(max(float(s[4]) for s in rest), 1)
                facts["restFactorMaxName"] = max(rest, key=lambda s: float(s[4]))[0]

        # What the budget actually holds fixed, and what it does not.  Every run in this campaign
        # stops at the same number of ordinary-loop proposals, and the CSV records that number, so
        # the claim can be checked rather than asserted.  The work counter and the clock are the
        # two things the budget does NOT equalise: the escape mechanisms spend both without
        # spending proposals.  Reporting them is what lets the escape result be stated without the
        # budget-displacement story the counter does not support.
        if "candidates" in rows[0]:
            cand = {int(t.num(r, "candidates")) for r in rows}
            facts["facBudget"] = str(max(cand))
            facts["facBudgetExact"] = "yes" if len(cand) == 1 else "no"
        if "work" in rows[0] and "elapsed_ms" in rows[0]:
            for n in sorted({int(r["n"]) for r in rows if r["factor"] == "escape-ladder"}):
                tg = "FourHundred" if n == 400 else "SixteenHundred"
                sel = {lv: [r for r in rows if r["factor"] == "escape-ladder"
                            and int(r["n"]) == n and r["level"] == lv]
                       for lv in ("none", "deep+kick+fresh")}
                if all(sel.values()):
                    for col, nm in (("work", "Work"), ("elapsed_ms", "Time")):
                        a = t.median([t.num(r, col) for r in sel["deep+kick+fresh"]])
                        b = t.median([t.num(r, col) for r in sel["none"]])
                        facts["escapeExtra" + nm + tg] = t.fmt(a / max(1.0, b), 3)

    # --- anchor x history policy --------------------------------------------------------------------
    path = os.path.join(R, "anchor.csv")
    if t.complete(path):
        rows = t.read(path)
        # The crossed campaign is the second of the two that store per-run completeness and
        # legality flags; the article quotes the count so that the reader can tell which campaigns
        # rest on stored evidence and which on the constructor bound.
        facts["anchorRuns"] = str(len(rows))
        facts["anchorClean"] = str(sum(1 for r in rows
                                       if t.num(r, "clean") >= 1 and t.num(r, "unplaced") == 0))
        by = collections.defaultdict(list)
        for r in rows:
            by[(int(r["n"]), r["anchor"], r["history_policy"])].append(r)
        arows = []
        for n in sorted({k[0] for k in by}):
            for anchor in ("incumbent", "working-state"):
                for pol in ("refill-at-kick", "keep", "reset-to-incumbent"):
                    v = by.get((n, anchor, pol))
                    if not v:
                        continue
                    soft = [t.num(r, "soft") for r in v]
                    obj = [t.num(r, "objective") for r in v]
                    arows.append([n, anchor, pol,
                                  "%d/%d" % (sum(1 for r in v if feasible(r)), len(v)),
                                  t.fmt(t.median(soft), 0), t.fmt(t.median(obj), 0),
                                  t.fmt(t.iqr(obj) / max(1.0, t.median(obj)), 3),
                                  t.fmt(t.median([t.num(r, "kicks") for r in v]), 1)])
            tag = tagn = "FourHundred" if n == 400 else "SixteenHundred"

            # The unit of replication in this experiment is the INSTANCE, not the run.  Each of the
            # twelve instances at a size was solved once in each of the six cells, so the six
            # outcomes that share an instance are repeated observations of one block and not six
            # independent trials.  Every contrast below is therefore formed inside a block first,
            # giving one number per instance, and only then aggregated across the twelve.  Pooling
            # the thirty-six (seed, policy) differences into one signed-rank test, which an earlier
            # version of this script did, counts each instance three times.
            cells = {}
            for r in rows:
                if int(r["n"]) != n:
                    continue
                cells[(r["seed"], r["anchor"], r["history_policy"])] = t.num(r, "objective")
            seeds = sorted({s for (s, _, _) in cells})
            pols = ("refill-at-kick", "keep", "reset-to-incumbent")
            full = [s for s in seeds
                    if all((s, a, p) in cells and cells[(s, a, p)] > 0
                           for a in ("incumbent", "working-state") for p in pols)]
            facts["anchorBlocks" + tag] = str(len(full))

            def lr(s, p):
                """log of the working-state-to-incumbent ratio on instance s under policy p."""
                return math.log(cells[(s, "working-state", p)] / cells[(s, "incumbent", p)])

            # The anchor effect: one number per instance, the mean over the three policies of the
            # within-instance log ratio, reported back as a ratio.
            if len(full) >= 5:
                blocks = [sum(lr(s, p) for p in pols) / 3.0 for s in full]
                _, pv = t.wilcoxon_signed_rank(blocks, [0.0] * len(blocks))
                pt, lo_, hi_ = t.block_bootstrap_ci(
                    blocks, lambda bs: math.exp(t.median(bs)))
                facts["anchorP" + tag] = t.fmt_p(pv)
                facts["anchorRatio" + tag] = t.fmt(pt, 2)
                facts["anchorRatioLo" + tag] = t.fmt(lo_, 2)
                facts["anchorRatioHi" + tag] = t.fmt(hi_, 2)

            # The anchor effect within each policy, and the interaction contrast: the difference
            # between two within-instance anchor effects.  That difference is what "the anchor
            # matters more under one policy than another" means as a measurement; comparing a
            # significant cell with a non-significant one is not a test of it.
            cellTests = []
            for pol in pols:
                if len(full) < 5:
                    continue
                blocks = [lr(s, pol) for s in full]
                _, pv = t.wilcoxon_signed_rank(blocks, [0.0] * len(blocks))
                pt, lo_, hi_ = t.block_bootstrap_ci(blocks, lambda bs: math.exp(t.median(bs)))
                nm = tagn + pol.replace("-", "")
                cellTests.append((nm, pv))
                facts["anchorCellRatio" + nm] = t.fmt(pt, 2)
                facts["anchorCellLo" + nm] = t.fmt(lo_, 2)
                facts["anchorCellHi" + nm] = t.fmt(hi_, 2)
            for nm, pv in t.holm(cellTests):
                facts["anchorCellP" + nm] = t.fmt_p(pv)
            if len(full) >= 5:
                inter = [lr(s, "refill-at-kick") - lr(s, "reset-to-incumbent") for s in full]
                _, pv = t.wilcoxon_signed_rank(inter, [0.0] * len(inter))
                pt, lo_, hi_ = t.block_bootstrap_ci(inter, lambda bs: math.exp(t.median(bs)))
                facts["anchorInterP" + tag] = t.fmt_p(pv)
                facts["anchorInterRatio" + tag] = t.fmt(pt, 2)
                facts["anchorInterLo" + tag] = t.fmt(lo_, 2)
                facts["anchorInterHi" + tag] = t.fmt(hi_, 2)

            # The history policy, averaged over anchors inside each instance and then compared
            # across instances, against the policy that wins.
            for pol in pols:
                facts["histMed" + tag + pol.replace("-", "")] = t.fmt(
                    t.median([t.num(r, "objective") for r in rows
                              if int(r["n"]) == n and r["history_policy"] == pol]), 0)
            tests = []
            for pol in ("refill-at-kick", "keep"):
                if len(full) < 5:
                    continue
                blocks = [0.5 * sum(math.log(cells[(s, a, pol)]
                                             / cells[(s, a, "reset-to-incumbent")])
                                    for a in ("incumbent", "working-state")) for s in full]
                _, pv = t.wilcoxon_signed_rank(blocks, [0.0] * len(blocks))
                pt, lo_, hi_ = t.block_bootstrap_ci(blocks, lambda bs: math.exp(t.median(bs)))
                tests.append((pol, pv))
                facts["histRatio" + tag + pol.replace("-", "")] = t.fmt(pt, 2)
                facts["histLo" + tag + pol.replace("-", "")] = t.fmt(lo_, 2)
                facts["histHi" + tag + pol.replace("-", "")] = t.fmt(hi_, 2)
            for pol, pv in t.holm(tests):
                facts["histP" + tag + pol.replace("-", "")] = t.fmt_p(pv)
            mx = max(float(facts["histMed" + tag + q.replace("-", "")]) for q in pols)
            mn = min(float(facts["histMed" + tag + q.replace("-", "")]) for q in pols)
            facts["histSpread" + tag] = t.fmt(mx / max(1.0, mn), 2)
        t.write_table(
            os.path.join(TAB, "anchor.tex"),
            "Where the perturbation is anchored, crossed with what happens to the acceptance "
            "history when it is applied. The second factor is the one the iterated-local-search "
            "literature does not discuss. \\emph{feas./runs} counts the runs that returned a "
            "complete, structurally legal schedule with no hard violation, "
            "\\emph{IQR/$f$} the interquartile range of $f$ over its median, and \\emph{kicks} "
            "the median number of perturbations in a run of the cell.",
            "tab:anchor",
            ["$n$", "anchor", "history", "feas./runs", "soft", "$f$", "IQR/$f$", "kicks"],
            arows, align="rllrrrrr", small=True)

    # --- quality against budget ----------------------------------------------------------------------
    path = os.path.join(R, "budget.csv")
    if t.complete(path):
        rows = t.read(path)
        by = collections.defaultdict(lambda: collections.defaultdict(list))
        paired_ = collections.defaultdict(dict)
        for r in rows:
            by[(r["arm"], int(r["n"]))][int(r["budget"])].append(t.num(r, "objective"))
            paired_[(r["arm"], int(r["n"]), int(r["budget"]))][r["seed"]] = (
                t.num(r, "hard") * 1e9 + t.num(r, "objective"))
        for k in sorted(by):
            out = []
            for b in sorted(by[k]):
                v = by[k][b]
                out.append([b, round(t.median(v), 1), round(t.quantile(v, 0.25), 1),
                            round(t.quantile(v, 0.75), 1)])
            t.write_dat(os.path.join(FIG, f"budget-{k[0]}-n{k[1]}.dat"),
                        ["budget", "median", "q25", "q75"], out)
        for n in sorted({k[1] for k in by}):
            full = by.get(("full", n), {})
            noesc = by.get(("no-escape", n), {})
            if full and noesc:
                tag = "FourHundred" if n == 400 else "SixteenHundred"
                bmax = max(full)
                # How much the escape ladder costs at the largest budget: the full configuration's
                # median objective divided by the escape-free one's.  Above one means the escape is
                # a net loss, which is what this family reports and is worth naming rather than
                # hiding inside a ratio whose direction a reader has to work out.
                facts["escapeCost" + tag] = t.fmt(
                    t.median(full[bmax]) / max(1.0, t.median(noesc[bmax])), 1)
                facts["escapeBudgetMax" + tag] = str(bmax)
                # ... and at the smallest budget, to show which way the trend runs.
                bmin = min(full)
                facts["escapeCostSmall" + tag] = t.fmt(
                    t.median(full[bmin]) / max(1.0, t.median(noesc[bmin])), 2)
                facts["escapeBudgetMin" + tag] = str(bmin)
                facts["escapeGain" + tag] = t.fmt(
                    t.median(noesc[bmax]) / max(1.0, t.median(full[bmax])), 1)
                # A block-bootstrap interval on the same ratio, resampling the six instances.  The
                # point estimate alone invites a reader to take 3.3 as a measured quantity; the
                # interval is what says how little six blocks pin it down.
                pf = paired_[("full", n, bmax)]
                pn = paired_[("no-escape", n, bmax)]
                kb = sorted(set(pf) & set(pn))
                objf = {r["seed"]: t.num(r, "objective") for r in rows
                        if r["arm"] == "full" and int(r["n"]) == n and int(r["budget"]) == bmax}
                objn = {r["seed"]: t.num(r, "objective") for r in rows
                        if r["arm"] == "no-escape" and int(r["n"]) == n and int(r["budget"]) == bmax}
                blocksB = [(objf[k], objn[k]) for k in kb if objn.get(k, 0) > 0]
                if len(blocksB) >= 3:
                    pt, lo_, hi_ = t.block_bootstrap_ci(
                        blocksB, lambda bs: t.median([x / y for x, y in bs]))
                    facts["escapePaired" + tag] = t.fmt(pt, 2)
                    facts["escapePairedLo" + tag] = t.fmt(lo_, 2)
                    facts["escapePairedHi" + tag] = t.fmt(hi_, 2)
                # The ladder is the paper's central negative result, so it needs the same
                # apparatus every other comparison gets: a seed count, a paired test at every
                # rung, and an honest account of the shape of the curve.  "Monotone" was once
                # asserted here and is false at the smaller size, so the shape is measured:
                # where the ratio peaks, and over how many consecutive rungs it rises.
                seeds, resolved, firstBig = 0, 0, None
                rungTests = []
                limit = float(facts.get("dispWorstBest", 0) or 0)
                ratios = []
                for b in sorted(full):
                    a_ = paired_[("full", n, b)]
                    c_ = paired_[("no-escape", n, b)]
                    ks = sorted(set(a_) & set(c_))
                    if len(ks) < 5:
                        continue
                    seeds = max(seeds, len(ks))
                    _, pv = t.wilcoxon_signed_rank([a_[k] for k in ks], [c_[k] for k in ks])
                    rungTests.append((str(b), pv))
                    ratio = t.median(full[b]) / max(1.0, t.median(noesc[b]))
                    ratios.append((b, ratio))
                    if firstBig is None and limit and ratio > limit:
                        firstBig = b
                # Holm across the rungs of one ladder, as the design section promises.  With six
                # paired runs the exact test floors at 2/2^6, so seven corrected rungs cannot
                # resolve anything and the evidence has to be the consistency of the direction
                # across the ladder rather than any single rung.  The rungs reuse the same six
                # instances, so that consistency is one curve agreeing with itself and not seven
                # independent confirmations.
                resolved = sum(1 for _, pv in t.holm(rungTests) if pv < 0.05)
                facts["escapeRawResolved" + tag] = str(
                    sum(1 for _, pv in rungTests if pv < 0.05))
                facts["escapeAgainst" + tag] = str(sum(
                    1 for b2 in sorted(full)
                    if t.median(full[b2]) > t.median(noesc[b2])))
                facts["escapeSeeds"] = str(seeds)
                facts["escapeResolved" + tag] = str(resolved)
                facts["escapeRungs" + tag] = str(len(full))
                facts["escapeFirstBig" + tag] = str(firstBig) if firstBig else "--"
                if ratios:
                    pk = max(ratios, key=lambda x: x[1])
                    facts["escapeCostPeak" + tag] = t.fmt(pk[1], 2)
                    facts["escapeCostPeakAt" + tag] = str(pk[0])
                    rise = 0
                    for (b1, r1), (b2, r2) in zip(ratios, ratios[1:]):
                        if r2 > r1:
                            rise += 1
                        else:
                            break
                    facts["escapeRising" + tag] = str(rise)
                    facts["escapeFalling" + tag] = str(sum(
                        1 for (b1, r1), (b2, r2) in zip(ratios, ratios[1:]) if r2 <= r1))
                if seeds:
                    facts["escapeFloorP"] = t.fmt_p(2.0 / (2 ** seeds))

    # What one doubling of the budget buys at the budget the factor sweep runs at.  A factor
    # spread has to be compared against something a designer could get instead, and this is the
    # obvious alternative use of the same effort.
    path2 = os.path.join(R, "budget.csv")
    if t.complete(path2):
        brows2 = t.read(path2)
        bb = collections.defaultdict(list)
        for r in brows2:
            if r["arm"] == "full":
                bb[(int(r["n"]), int(r["budget"]))].append(t.num(r, "objective"))
        gains = []
        for n in sorted({k[0] for k in bb}):
            buds = sorted({k[1] for k in bb if k[0] == n})
            for lo, hi in zip(buds, buds[1:]):
                if hi == 2 * lo and lo >= 100000:
                    gains.append(t.median(bb[(n, lo)]) / max(1.0, t.median(bb[(n, hi)])))
        if gains:
            facts["doublingWorthMin"] = t.fmt(min(gains), 2)
            facts["doublingWorthMax"] = t.fmt(max(gains), 2)
            facts["doublingWorthMed"] = t.fmt(t.median(gains), 2)

    # --- the acceptance bar, measured -----------------------------------------------------------------
    path = os.path.join(R, "plateau.csv")
    if t.complete(path):
        # Bucket 63 is the overflow bin: the instrumentation caps the index there, so it
        # collects every candidate whose cost level is older than 63 bucket widths and its
        # nominal age is meaningless.  It is dropped rather than plotted.
        allPlateau = t.read(path)
        rows = [r for r in allPlateau
                if r.get("age_over_l") and int(r.get("bucket", 0)) < 63]
        # The design behind the table, so that the paper can state it rather than leave a reader
        # to infer it from a row count: how many instances per setting, which sizes and which
        # history lengths the table pools over, and how many rows the overflow bin removed.
        facts["plateauSeeds"] = str(len({r["seed"] for r in allPlateau}))
        facts["plateauSizes"] = ", ".join(str(x) for x in sorted(
            {int(r["n"]) for r in allPlateau}))
        facts["plateauLengths"] = ", ".join(str(x) for x in sorted(
            {int(r["lahc_length"]) for r in allPlateau}))
        facts["plateauDropped"] = str(len(allPlateau) - len(rows))
        # The settings Figure 4 draws; see the figure in main.tex.
        PLOTTED_TRACES = {(400, "lahc", 100), (400, "lahc", 1000),
                          (400, "schc", 1000), (400, "dlas", 1000)}
        plottedRates = []
        agg = collections.defaultdict(lambda: [0, 0, 0])
        for r in rows:
            k = (int(r["n"]), r["engine"], int(r["lahc_length"]), round(t.num(r, "age_over_l"), 4))
            a = agg[k]
            a[0] += t.num(r, "total")
            a[1] += t.num(r, "uphill_offered")
            a[2] += t.num(r, "uphill_accepted")
        for n in sorted({k[0] for k in agg}):
            for eng in sorted({k[1] for k in agg if k[0] == n}):
                for L in sorted({k[2] for k in agg if k[0] == n and k[1] == eng}):
                    out = []
                    for k in sorted(k for k in agg if k[:3] == (n, eng, L)):
                        a = agg[k]
                        if a[1] <= 0:
                            continue
                        rate = a[2] / a[1]
                        # Whether the bin accepted anything is decided here, not in the figure.
                        # A "rate == 0" test inside pgfmath is not a reliable classifier: the
                        # parser has about 1e-5 resolution, so a genuinely positive rate can test
                        # equal to zero.  The flag is exact and the figure partitions on it.
                        positive = 1 if a[2] > 0 else 0
                        shown = round(rate, 6)
                        if positive and shown == 0:
                            # Reached long after the design-validation block above has returned,
                            # so this refuses outright rather than collecting a fault nobody reads.
                            raise SystemExit(
                                f"plateau: bin {k} has a positive rate {rate!r} that rounds to "
                                f"zero at six decimals; the figure would plot it as zero. Widen "
                                f"the precision of the figure data before regenerating.")
                        out.append([k[3], shown, int(a[1]), positive])
                    if out:
                        t.write_dat(os.path.join(FIG, f"plateau-{eng}-n{n}-L{L}.dat"),
                                    ["age_over_l", "uphill_rate", "offered", "positive"], out)
                        # The smallest positive rate anywhere in the window Figure 4 plots, which
                        # is what the text cites as the reason that axis is logarithmic.  Taken
                        # from the four plotted settings only, so the number and the picture agree.
                        if (n, eng, L) in PLOTTED_TRACES:
                            for row in out:
                                if row[0] <= 3.0 and row[3]:
                                    plottedRates.append(row[1])
        if plottedRates:
            facts["upLowestPlotted"] = t.fmt(100 * min(plottedRates), 4)
            facts["upHighestPlotted"] = t.fmt(100 * max(plottedRates), 2)
        prows = []
        for eng in sorted({k[1] for k in agg}):
            before = [agg[k][2] / agg[k][1] for k in agg if k[1] == eng and k[3] < 0.2 and agg[k][1]]
            after = [agg[k][2] / agg[k][1] for k in agg if k[1] == eng and k[3] >= 1.0 and agg[k][1]]
            offeredAfter = int(sum(agg[k][1] for k in agg if k[1] == eng and k[3] >= 1.0))
            prows.append([eng, t.fmt(100 * t.median(before), 2) if before else "--",
                          f"{offeredAfter:d}",
                          t.fmt(100 * t.median(after), 3) if after else "--",
                          t.fmt(100 * max(after), 3) if after else "--"])
            if before:
                facts["upBefore" + eng] = t.fmt(100 * t.median(before), 2)
                # The same quantity pooled over candidates rather than over cells.  A cell holding
                # a million offered candidates and one holding forty count equally in the median,
                # so the two numbers differ by a factor and the paper should say which is which.
                num = sum(agg[k][2] for k in agg if k[1] == eng and k[3] < 0.2)
                den = sum(agg[k][1] for k in agg if k[1] == eng and k[3] < 0.2)
                if den:
                    facts["upBeforePooled" + eng] = t.fmt(100 * num / den, 2)
            if after:
                facts["upAfter" + eng] = t.fmt(100 * t.median(after), 3)
            # How much evidence there is beyond one history length at all.  A criterion whose
            # working cost never settles never reaches that region, and that is a different way of
            # avoiding the pathology from tolerating it -- so the count belongs in the paper.
            facts["upCells" + eng] = str(len(after))
            facts["upOffered" + eng] = str(int(sum(
                agg[k][1] for k in agg if k[1] == eng and k[3] >= 1.0)))
            facts["upOfferedYoung" + eng] = str(int(sum(
                agg[k][1] for k in agg if k[1] == eng and k[3] < 0.2)))
        t.write_table(
            os.path.join(TAB, "plateau.tex"),
            "The uphill acceptance rate before and after the current cost level is one history "
            "length old, for three acceptance criteria, pooled over both instance sizes and all "
            "four history lengths. The two percentage columns are medians over age bins, each bin "
            "being one age band of one (size, length) combination; the \\emph{offered} column is "
            "instead a pooled count of worsening candidates over those bins, so the fourth column "
            "is not the accepted total divided by the third. A criterion whose working cost never "
            "settles long enough for a level to reach that age contributes no observations to the "
            "last two columns, which is a different way of avoiding the pathology from tolerating "
            "it. Bins are indexed by their lower endpoint, so the thresholds are approximate, and "
            "the overflow bin that collects every level older than 63 bin widths is excluded.",
            "tab:plateau",
            ["criterion", "age $<$ 0.2$\\ell$ \\%", "offered at age $\\ge\\ell$",
             "age $\\ge\\ell$ \\%", "max at age $\\ge\\ell$ \\%"],
            prows, align="lrrrr", small=True)

    t.write_macros(os.path.join(TAB, "facts.tex"), facts)
    print(f"wrote {len(facts)} macros")
    return 0


if __name__ == "__main__":
    sys.exit(main())
