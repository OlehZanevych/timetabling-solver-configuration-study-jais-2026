# Acceptance criteria, escape strategies and budget allocation: a controlled configuration study

Experiment code, input data and raw results for the article of the same name (2026). Repository:
https://github.com/OlehZanevych/timetabling-solver-configuration-study-jais-2026

Everything needed to reproduce the experiments is here: the C++20 solver, instance generator and
independent validator (`src/`), the 24 instances the study uses with their planted schedules (48
files in `data/`), the per-run records the article was built from (`results/`), and the scripts that
run every experiment and regenerate every empirical table, the figure data and the numbers quoted
through macros from those records (`scripts/`). The sections below say what each experiment
measures, how to build and run it, and how to check a re-run against the records.

The article compares the configuration choices *inside* one timetabling solver -- the acceptance
criterion, its history length, the three levels of the escape from stagnation, where a perturbation
is anchored and how strong it is, the construction order, the price of infeasibility, and the restart
threshold -- under one objective, one instance family, one implementation and one budget,
varying one factor at a time and pairing every comparison by instance.

Before any comparison, the run-to-run dispersion of a single configuration on a single instance is
measured over twenty solver seeds. That number is descriptive context for how noisy one run is on
this problem; it is **not** a detection threshold for the factor comparisons, which are paired
across instances and carry their own block-resampled intervals.

## Experiments

| subcommand | what it measures | article |
|---|---|---|
| `dispersion` | the spread of one configuration on one instance at three budgets | §1.5 |
| `factors` | eight factors, one at a time, paired by instance | §1.6--1.9, 1.12 |
| `anchor` | where the perturbation is anchored, crossed with what happens to the acceptance history | §1.10 |
| `plateau` | the uphill acceptance rate against the age of the cost level, for three acceptance criteria | §1.11 |
| `budget` | quality against budget for three configurations | §1.9 |
| `construction` | scores the construction alone, on every campaign's own seed tuples, which bounds the unplaced count of every run | §1.4 |
| `instances` | writes the instances used to `data/` | -- |

Run `dispersion` first: the paper reports it before any comparison.

Each subcommand writes one CSV to `results/`, with a header row and one line per
run. The one exception is `plateau`, whose unit is not a run: it writes one line per occupied age
bin of a run -- 1755 lines for its 120 runs -- because a trace is a histogram and storing every
charged proposal individually would be pointless. Within a bin its three counts are sums; nothing
else is aggregated by the C++ code, so that the raw records survive.

These are the exact commands behind the published records; `scripts/run_all.sh` issues them in
this order and nothing in the paper was produced with other seed counts.

```bash
./build/experiments dispersion --seeds 20 --budget 200000
./build/experiments factors    --seeds 10 --budget 200000
./build/experiments anchor     --seeds 12 --budget 200000
./build/experiments plateau    --seeds 5
./build/experiments budget     --seeds 6
./build/experiments construction
./build/experiments instances  --data data
```

## Checking the implementation first

```bash
./build/experiments selftest
```

Three sampled checks, each a few seconds; they are checks, not proofs, and the sample is stated
here because it is smaller than the campaigns. Planted schedules are re-scored by the independent
validator and must be feasible, over three sizes and three seeds rather than over every instance
the experiments use. An instance written to `data/` and read back must score exactly as the
original did. And the incremental evaluator is compared term by term with the independent
validator at intervals along a random walk of placements -- which makes an identical error in two
implementations that share no scoring code unlikely, not impossible, and does not exercise every
operator's apply/undo path. `scripts/run_all.sh` runs it before anything else, together with

```bash
python3 scripts/check_stats.py
```

which checks the statistics in `scripts/ttstats.py` against their definitions rather than against
another library: the exact Wilcoxon signed-rank test is compared with an enumeration of its own null
distribution over several hundred random samples, the sign test and the effect size against values
that can be computed by hand, the chi-squared survival function against the tabulated critical
values, and Friedman's statistic against its closed form. `ttstats.py` uses the standard library
alone, so the analysis has no dependency to install and no version that can drift; the price is that
its tests have to be written, and this is them.

## Building

Requires a C++20 compiler and CMake 3.16 or newer. Nothing else: no external libraries, no package
manager, no network access.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Reproducing the paper

```bash
scripts/run_all.sh          # self-test, every experiment (one CSV each in results/), design tests
python3 scripts/analyse.py  # turns those CSVs into tables/*.tex and figures/*.dat
```

`run_all.sh` overwrites `results/`; to compare a fresh run with the shipped records instead, run
the experiments into another directory as shown under *Checking a re-run* below. The full design
takes about five hours on one core.

To regenerate the tables from the shipped records without re-running anything, straight after
cloning:

```bash
TT_MIN_AGE=0 python3 scripts/analyse.py
```

`TT_MIN_AGE=0` is needed only because a fresh checkout gives every file a new timestamp, and the
script normally refuses to read a results file written in the last two minutes (see below); after
two minutes the plain command works.

Run from a clone of this repository, `analyse.py` writes `tables/` and `figures/` inside it (both
git-ignored). Inside the article's source tree, where a `main.tex` sits one level up, it writes
them beside the paper instead, and the paper is then built there with `latexmk`.

The paper is typeset with the journal's `pcistyle` class, which requires **XeLaTeX** (it loads
polyglossia and mathspec) and the fonts Times New Roman and Arial, and its bibliography is UTF-8
and needs **bibtexu** rather than bibtex. `latexmk` in the article directory picks both up from
`latexmkrc`; by hand it is `xelatex main; bibtexu main; xelatex main; xelatex main`. `pdflatex`
stops with "XeTeX is required to compile this document".

`run_all.sh` accepts `--quick` to run a reduced design (fewer seeds, smaller budgets) that is enough
to check that everything works end to end; on one core it takes about 4m20s here, against hours
for the full design. It writes to `results-quick/` rather than `results/`, so a smoke test cannot
overwrite the records the paper was built from --- point
`analyse.py --results results-quick --development` at it to see the tables a reduced design
produces. The `--development` flag is needed because the publication validator is supposed to
reject a reduced design; do not weaken the validator to make the shorter command work.

Some experiments fix their own budgets, a ladder that
doubles or a figure set by the quantity being traced, and a smaller `--budget` cannot shorten those;
`--maxbudget K` caps what any one run may spend, and a ladder skips the rungs above the cap rather
than repeating the highest one it is allowed, so a capped run never records a budget it did not spend.

The instance files in `data/` are shipped gzipped and `run_all.sh` expands them on its first run.
They are the **complete** set the study uses -- both sizes, seeds 1 to 12, which covers every
campaign (the factors use seeds 1-10, the crossed campaign 1-12, the ladder 1-6, the trace 1-5 and
the dispersion study seed 1) -- each with the hidden feasible schedule it was built around. The
experiments themselves generate their instances in memory from (size, seed) rather than reading
these files; the files are for a solver that shares no code with this one. That the two are the
same instances is checked, not assumed: `experiments instances` regenerates all 48 files, and on
2026-10-03 every one was byte-identical to the shipped copy on both x86-64 and aarch64.

`analyse.py` refuses to read a results file that was written in the last two minutes, on the
grounds that an experiment appends to its CSV as it runs and a table built from a half-executed
design looks finished without being so. In publication mode that refusal is fatal: a regeneration
started a minute too early would otherwise clear the output directory, skip the campaign and exit
successfully, leaving the article short a table. Set `TT_MIN_AGE=0` while iterating on the analysis
itself.

Validation happens before anything is cleared, so a refusal leaves the previous `tables/` and
`figures/` exactly as they were.

It also validates the records against the **intended design** before it aggregates anything. That
design -- the factor levels, the two sizes, the block counts, the budget ladder, the dispersion
budgets and each campaign's instance/solver seed schedule -- is written out explicitly at the top
of `analyse.py`, in `DESIGN_FACTORS`, `DESIGN_CAMPAIGNS` and `DESIGN_TRACE_*`, and must be kept in
step with
`factors()` in `src/main.cpp` and with `scripts/run_all.sh`. Every cell of the declared design must
be present with exactly the declared blocks, once each, and no undeclared cell may appear.
Expectations inferred from the input itself cannot do this: a level absent at both sizes leaves
nothing in the file to notice its absence, and an earlier version of this script did exit zero on
records with one level removed. Declaring the design catches that, a missing size, a missing block,
a cell run with a different set of seeds, a duplicate from a restarted campaign, and a level the
records contain but the design does not.

The acceptance-trace file is checked differently, because its unit is a bin rather than a run and
which bins are occupied depends on the trajectory: what must hold is that all 120 declared run
identities are present, that no (run, bin) key appears twice, and that each bin's counts are whole
numbers with accepted <= offered <= total.

Fields used as *evidence* -- the audit's `unplaced` and `clean` -- are parsed strictly rather than
through the usual "0.0 if unparseable" helper. A blank `unplaced` is missing evidence, and missing
evidence must not read as a zero.

`scripts/check_design.py` is the test of that validator. It copies the records and damages each
copy in one way -- a factor level removed at both sizes, an instance size, a block, a ladder rung,
the construction audit, five sixths of the audit's tuples, an audited tuple recorded twice, an
audit `unplaced` field blanked, the trace file, one trace run, a trace bin recorded twice, and a
complete factor file with a fresh timestamp -- and requires `analyse.py` to refuse each one, and
`--development` to accept each one. The last case also checks that the refusal left the previous
`tables/` alone. It starts by requiring the unmodified records to pass, or none of the failures
would prove anything. It is run by `run_all.sh` after the campaigns, and it never writes to the
records it is pointed at.

Every per-run file must carry, for every run, the validator's `clean` flag, its `unplaced` count
and the `adoptions` counter, and the factor, ladder and dispersion files the proposals actually
spent (`candidates`); the script refuses to generate if a column is missing, if any run left a class
unplaced or failed the structural check, or if any budgeted run did not spend exactly its budget,
because the article states all three.

`results/construction.csv` is required, not optional: it is the second, independent route to the
completeness half of the feasibility definition (see `feasible()` in `analyse.py`); its rows must
cover every `(campaign, size, instance seed, solver seed, order)` tuple the design implies and must
all show zero unplaced and a clean structural check. A fault of any of these kinds is fatal -- the
script prints it and exits non-zero rather than generating a publication table. Pass `--development`
(`--allow-gaps` is the old spelling) to look at what a reduced or partial campaign produces.

The repository is therefore self-contained: clone it, build it, run it, and the tables are there.
The LaTeX sources of the article itself are not part of it.

All empirical result tables and figure data in the paper are produced this way. No value in any
generated table or figure is typed by hand: the analysis script reads the raw per-run records and
writes the LaTeX directly, and most of the numbers quoted in the prose reach the text as macros in
`tables/facts.tex`. There are two classes of exception and they are worth knowing before trusting a
re-run to update everything. The three tables in the article's `spec/` directory -- the model, the
design and the reference configuration -- are maintained by hand against the source files each of
them names; they state settings rather than results, so a re-run does not change them, but a change
to the solver's defaults would have to be carried across by hand. And a handful of ratios and
intervals are typed into running text where a macro would have made the sentence unreadable; they
are copied from the generated table beside them and need re-checking after a re-run.

## Layout

```
src/core/        the solver kernel, shared with the other applications in this series
  model.*        instances: entities, domains, the compressed tick axis
  state.*        the schedule, its (entity, day) bucket decomposition, incremental evaluation
  search.*       construction, acceptance, the seven neighbourhoods, the bandit, the escape
  validate.*     an INDEPENDENT scorer, sharing no code with state.cpp -- see below
  generate.*     instances built backwards around a hidden feasible schedule
  io.*           the plain-text instance format
  flow.hpp       Dinic's maximum flow, for the matching bounds
  hungarian.hpp  the assignment problem, for the permutation operator
  mask.hpp       128-bit tick masks
  rng.hpp        xoshiro256**, so that a seed reproduces a run exactly
src/             this application's experiment drivers
scripts/         run_all.sh, analyse.py, and ttstats.py (statistics, standard library only)
data/            instances in the plain-text format, with their planted schedules
results/         one CSV per experiment, one line per run (plateau: one line per age bin) --
                 the records the article was built from; see "The records" below
```

## The records

The files in `results/` were produced on 2026-10-03 by the sources in this directory, built as
above (CMake Release, `-O3 -std=gnu++20`, g++ 13.3), with the commands in `scripts/run_all.sh`, on
an x86-64 Intel Xeon processor at 2.10 GHz under Ubuntu 24.04 (Linux 6.18). The factor, ladder and dispersion campaigns -- the three that record
wall-clock -- were each run alone, so `elapsed_ms` is uncontended; the crossed, trace and
construction campaigns record no time. Values are written with fifteen significant digits, so every
objective is exact.

The same values were obtained more than once. An earlier complete execution, by a driver that
recorded fewer columns and rounded objectives to six significant digits, agrees with these files on
every shared non-timing column of every run, allowing for that rounding; a second execution of the
factor, ladder and dispersion campaigns with the current build agrees on every non-timing column;
and the factor and crossed campaigns were re-run one run per process on aarch64 with g++ 11.4 and
agree exactly. Nothing in the drivers depends on the platform -- the generator is a
fixed xoshiro256** with its own integer sampling and shuffle, runs are single-threaded, no unordered
container is iterated, every sort has a total order, there is no `-ffast-math`, and wall-clock
enters a run only through a 1800 s safety deadline that the longest run does not approach -- but
agreement across platforms is an observation, not a guarantee.

## Two things that make the numbers trustworthy

**An independent validator.** `src/core/validate.cpp` re-reads a schedule from scratch and counts
every penalty term and every hard filter by the most direct method the definitions allow -- sorting
and scanning, never a bitmask. It shares no *scoring* code with the incremental evaluator in
`state.cpp`, so that an error in one cannot hide behind an identical error in the other; both
necessarily read the same instance model. Every objective, hard count and unplaced count in the
paper comes from the validator. The event counters, the work counter and the acceptance traces are
instrumentation inside the search and have no second source.

**Instances with a known answer.** The generator builds a valid schedule first, walking the week slot
by slot and placing each class into resources that are free at that moment and reachable in the time
available, and then reads the instance off it. So a schedule with no hard violation provably exists
for every instance, and anything the search cannot reach is a property of the search rather than of
the data. The self-test checks that on a sample of sizes and seeds rather than on every instance
the experiments use, and it is a check rather than a proof.

## Budgets: what the counter charges

`--budget K` stops a run after K **ordinary-loop proposals**: iterations of the main search loop on
which the drawn operator actually produced a candidate. It deliberately does *not* charge the
twenty-four attempts inside a deep phase, the repair work inside a kick, a fresh construction, or
the inner placement scans an operator performs while producing one candidate. It is therefore a
fixed-proposal budget and **not** an equal-work or equal-time budget, and the article says so
wherever the difference matters.

`--maxbudget` caps it. A second counter, **bucket recomputations**, is the unit of work the
evaluation actually spends; it is recorded in `results/factors.csv` as `work`, can be used as a
stopping rule through `SearchOptions::workLimit`, and is what the article quotes when it needs to
say how much work an escape mechanism spent. Wall-clock is recorded alongside both and never
reported alone.

## Checking a re-run against the shipped records

```bash
./build/experiments factors --seeds 10 --budget 200000 --results rr
python3 scripts/compare_rerun.py results rr
```

`compare_rerun.py` matches the two campaigns row by row on their shared columns, ignoring
`elapsed_ms`, and reports any row that differs, any row the re-run has that the shipped file does
not, and -- with `--complete`, for a full re-run -- any shipped row or file the re-run is missing.
That is how a campaign re-run with a newer driver -- one that records an extra column, say -- can be
swapped in without silently replacing the science along with the instrumentation.

Values are compared exactly, as numbers. One exception exists for records written by an older
driver, which rounded objectives of a million or more to six significant digits in exponent form:
such a value is accepted when the exact re-run value rounds to it, and the script reports how many
cells were accepted that way, separately from the exact matches.

## Known gaps in the records

* Final schedules are not stored, only their scores, so the published runs cannot be re-scored
  without re-running them.
* `anchor.csv` records no wall-clock and no proposal count; its runs use the factor budget, which
  the factor file shows is spent exactly.

## Licence

Released under the MIT License -- see `LICENSE`. Copyright (c) 2026 Oleh Zanevych. The licence
covers everything in this repository, including the instance files in `data/` and the records in
`results/`. If you use the code or the data, please cite the article.
