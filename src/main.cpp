// Experiments for "Acceptance criteria, escape strategies and budget allocation: a controlled
// configuration study of a course timetabling solver".
//
// One implementation, one objective, one instance family, and one factor varied at a time, with the
// same seeds everywhere so that every comparison is paired.  Budgets are counted in candidate
// evaluations, not seconds, so a result is reproducible on other hardware.
//
//   factors   F1..F7  one row per (factor, level, instance, seed)
//   budget    F8      quality against budget for the best and the worst configuration
//   plateau           the acceptance bar's collapse, measured directly
//   dispersion        the resolution limit: the spread of one configuration at one budget
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/generate.hpp"
#include "core/io.hpp"
#include "core/search.hpp"
#include "core/selftest.hpp"
#include "core/validate.hpp"

using namespace tt;

namespace {

std::string gResults = "results";

std::ofstream openCsv(const std::string& name, const std::string& header) {
  std::ofstream f(gResults + "/" + name);
  // Unbuffered: one run is one line and runs are seconds apart, so flushing costs nothing and a
  // long experiment can be watched, or salvaged, while it is still running.
  f << std::unitbuf;
  // Fifteen significant digits, not the stream default of six.  Every objective is an integer --
  // a weighted sum of squared integer counts -- and with six digits anything from one million up
  // was written in exponent form and rounded: 20419175 went out as 2.04192e+07.  Fifteen digits
  // is exact for every integer this study can produce and still prints a short decimal such as
  // an age of 0.12 as "0.12" rather than as its nearest double.
  f.precision(15);
  f << header << "\n";
  return f;
}

GeneratedInstance makeInstance(int n, uint64_t seed) {
  GenOptions go;
  go.nClasses = n;
  go.seed = seed;
  return generateInstance(go);
}

struct Level {
  std::string name;
  std::function<void(SearchOptions&)> apply;
};

struct Factor {
  std::string name;
  std::vector<Level> levels;
};

std::vector<Factor> factors() {
  std::vector<Factor> f;

  f.push_back({"acceptance",
               {{"lahc", [](SearchOptions& o) { o.engine = "lahc"; }},
                {"lahc-canonical", [](SearchOptions& o) { o.engine = "lahc"; o.lahcCanonical = true; }},
                {"dlas", [](SearchOptions& o) { o.engine = "dlas"; }},
                {"schc", [](SearchOptions& o) { o.engine = "schc"; }},
                {"sa", [](SearchOptions& o) { o.engine = "sa"; }},
                {"hill-climbing", [](SearchOptions& o) { o.engine = "hc"; }}}});

  f.push_back({"history-length",
               {{"10", [](SearchOptions& o) { o.lahcLength = 10; }},
                {"50", [](SearchOptions& o) { o.lahcLength = 50; }},
                {"100", [](SearchOptions& o) { o.lahcLength = 100; }},
                {"500", [](SearchOptions& o) { o.lahcLength = 500; }},
                {"5000", [](SearchOptions& o) { o.lahcLength = 5000; }},
                {"50000", [](SearchOptions& o) { o.lahcLength = 50000; }}}});

  f.push_back({"escape-ladder",
               {{"none", [](SearchOptions& o) { o.useDeep = o.useKick = o.useFresh = false; }},
                {"deep", [](SearchOptions& o) { o.useKick = o.useFresh = false; }},
                {"deep+kick", [](SearchOptions& o) { o.useFresh = false; }},
                {"deep+kick+fresh", [](SearchOptions&) {}}}});

  f.push_back({"kick-anchor",
               {{"incumbent", [](SearchOptions& o) { o.kickFromBest = true; }},
                {"working-state", [](SearchOptions& o) { o.kickFromBest = false; }}}});

  f.push_back({"kick-strength",
               {{"fixed-12", [](SearchOptions& o) { o.adaptiveKick = false; o.kickMin = o.kickMax = 12; }},
                {"fixed-60", [](SearchOptions& o) { o.adaptiveKick = false; o.kickMin = o.kickMax = 60; }},
                {"fixed-300", [](SearchOptions& o) { o.adaptiveKick = false; o.kickMin = o.kickMax = 300; }},
                {"adaptive-12-300", [](SearchOptions& o) { o.adaptiveKick = true; o.kickMin = 12; o.kickMax = 300; }},
                {"adaptive-6-100", [](SearchOptions& o) { o.adaptiveKick = true; o.kickMin = 6; o.kickMax = 100; }}}});

  f.push_back({"construction",
               {{"most-constrained", [](SearchOptions& o) { o.construction = "mrv"; }},
                {"random", [](SearchOptions& o) { o.construction = "random"; }}}});

  f.push_back({"infeasibility-price",
               {{"scale-free", [](SearchOptions& o) { o.hardWeight = 0; }},
                {"fixed-1e5", [](SearchOptions& o) { o.hardWeight = 1e5; }},
                {"fixed-1e6", [](SearchOptions& o) { o.hardWeight = 1e6; }},
                {"fixed-1e8", [](SearchOptions& o) { o.hardWeight = 1e8; }}}});

  f.push_back({"restart-threshold",
               {{"60k", [](SearchOptions& o) { o.restartAfter = 60000; }},
                {"150k", [](SearchOptions& o) { o.restartAfter = 150000; }},
                {"400k", [](SearchOptions& o) { o.restartAfter = 400000; }},
                {"1200k", [](SearchOptions& o) { o.restartAfter = 1200000; }}}});

  return f;
}

SearchOptions baseline(long long budget, uint64_t seed) {
  SearchOptions so;
  so.timeLimitMs = 1800000;
  so.candidateLimit = budget;
  so.threads = 1;
  so.seed = seed;
  // Counters scaled to the candidate budget used throughout this study, so that every level of the
  // escape ladder actually fires within it -- at BOTH sizes.  With the earlier values the search at
  // n = 1600 was still improving when the budget ran out, the kick never fired, and the four levels
  // of the escape ladder returned the same schedule: a factor about a mechanism measured on runs in
  // which the mechanism never ran.
  so.deepEvery = 1200;
  so.stagnationMoves = 2500;
  so.restartAfter = 150000;
  // The reference configuration must not itself be a bad configuration, or every factor is measured
  // at a handicapped point.  The anchor experiment finds that refilling the acceptance history at
  // the perturbed state's cost -- the obvious implementation, and this solver's shipped default --
  // is the worst of the three policies by a factor of about three.  The reference therefore uses the
  // best of them, and the anchor experiment reports all three regardless.
  so.kickHistoryPolicy = 2;   // refill at the incumbent's cost
  return so;
}

void expFactors(int seeds, long long budget) {
  auto f = openCsv("factors.csv",
                   "factor,level,n,seed,hard,soft,unplaced,objective,candidates,work,elapsed_ms,kicks,"
                   "fresh_restarts,deep_phases,adoptions,clean");
  for (const Factor& fac : factors()) {
    for (const Level& lv : fac.levels) {
      for (int n : {400, 1600}) {
        for (int seed = 1; seed <= seeds; ++seed) {
          GeneratedInstance gi = makeInstance(n, seed);
          SearchOptions so = baseline(budget, 424242 + seed);
          lv.apply(so);
          SearchResult r = solve(gi.instance, so);
          Score sc = validate(gi.instance, r.best);
          f << fac.name << "," << lv.name << "," << n << "," << seed << "," << sc.hard() << ","
            << sc.soft() << "," << sc.unplaced << "," << sc.objective(kDefaultBeta) << ","
            << r.candidates << ","
            << r.workUnits << "," << r.elapsedMs << "," << r.kicks << "," << r.freshRestarts << ","
            << r.deepPhases << "," << r.adoptions << "," << (sc.clean() ? 1 : 0) << "\n";
        }
      }
    }
  }
  std::printf("factors done\n");
}

void expAnchorHistory(int seeds, long long budget) {
  // The one question the iterated-local-search literature leaves open: when the perturbation is
  // applied, what happens to the acceptance history?  Three policies, crossed with the anchor.
  auto f = openCsv("anchor.csv",
                   "anchor,history_policy,n,seed,hard,soft,unplaced,objective,kicks,fresh_restarts,"
                   "deep_phases,adoptions,clean");
  for (int anchor = 0; anchor < 2; ++anchor) {
    for (int pol = 0; pol < 3; ++pol) {
      for (int n : {400, 1600}) {
        for (int seed = 1; seed <= seeds; ++seed) {
          GeneratedInstance gi = makeInstance(n, seed);
          SearchOptions so = baseline(budget, 55055 + seed);
          so.kickFromBest = anchor == 0;
          so.kickHistoryPolicy = pol;
          // The factor under test is what the kick does, so the kick has to fire.  With the study's
          // default counters and this budget it does not at the larger size -- the search is still
          // improving when the budget runs out -- and every arm then returns the same schedule.
          // These counters are scaled so that tens of kicks occur at both sizes; the kick count is
          // reported in the table so that a reader can check it.
          so.useDeep = false;               // the deep phase is a different factor, and it would
          so.deepEvery = 800;               // reset the stagnation counter before a kick could fire
          so.stagnationMoves = 800;
          so.restartAfter = 1 << 30;        // so is the fresh construction
          so.useFresh = false;
          SearchResult r = solve(gi.instance, so);
          Score sc = validate(gi.instance, r.best);
          const char* pn[] = {"refill-at-kick", "keep", "reset-to-incumbent"};
          f << (anchor == 0 ? "incumbent" : "working-state") << "," << pn[pol] << "," << n << ","
            << seed << "," << sc.hard() << "," << sc.soft() << "," << sc.unplaced << ","
            << sc.objective(kDefaultBeta)
            << "," << r.kicks << "," << r.freshRestarts << "," << r.deepPhases << ","
            << r.adoptions << "," << (sc.clean() ? 1 : 0) << "\n";
        }
      }
    }
  }
  std::printf("anchor done\n");
}

void expBudget(int seeds) {
  auto f = openCsv("budget.csv", "arm,n,seed,budget,hard,soft,unplaced,objective,candidates,work,elapsed_ms,kicks,"
                                "fresh_restarts,deep_phases,adoptions,clean");
  const long long ladder[] = {25000, 50000, 100000, 200000, 400000, 800000, 1600000};
  struct Arm { const char* name; void (*apply)(SearchOptions&); };
  const Arm arms[] = {
      {"full", [](SearchOptions&) {}},
      {"no-escape", [](SearchOptions& o) { o.useDeep = o.useKick = o.useFresh = false; }},
      {"hill-climbing", [](SearchOptions& o) { o.engine = "hc"; }},
  };
  for (int n : {400, 1600}) {
    for (const Arm& a : arms) {
      for (long long b : ladder) {
        if (tt::budgetCap() > 0 && b > tt::budgetCap()) continue;
        for (int seed = 1; seed <= seeds; ++seed) {
          GeneratedInstance gi = makeInstance(n, seed);
          SearchOptions so = baseline(b, 90909 + seed);
          a.apply(so);
          SearchResult r = solve(gi.instance, so);
          Score sc = validate(gi.instance, r.best);
          f << a.name << "," << n << "," << seed << "," << b << "," << sc.hard() << ","
            << sc.soft() << "," << sc.unplaced << "," << sc.objective(kDefaultBeta) << ","
            << r.candidates << "," << r.workUnits << "," << r.elapsedMs << ","
            << r.kicks << "," << r.freshRestarts << "," << r.deepPhases << ","
            << r.adoptions << "," << (sc.clean() ? 1 : 0) << "\n";
        }
      }
    }
  }
  std::printf("budget done\n");
}

void expPlateau(int seeds) {
  // The acceptance bar, measured.  Bucket width is a fixed fraction of the history length, so the
  // horizontal axis is the age of the cost level *in units of the history length* and the curves
  // for different lengths can be drawn on one figure.  The prediction is that every curve reaches
  // zero at 1.
  auto f = openCsv("plateau.csv",
                   "n,engine,lahc_length,seed,bucket_width,bucket,age_lo,age_over_l,total,"
                   "uphill_offered,uphill_accepted");
  for (int n : {400, 1600}) {
    for (const char* engine : {"lahc", "dlas", "schc"}) {
      for (int L : {100, 1000, 5000, 15000}) {
        for (int seed = 1; seed <= seeds; ++seed) {
          GeneratedInstance gi = makeInstance(n, seed);
          SearchOptions so = baseline(400000, 606 + seed);
          so.engine = engine;
          so.lahcLength = L;
          so.schcCounter = L;
          so.useDeep = so.useKick = so.useFresh = false;  // the theorem is about the bar alone
          const int64_t width = std::max<int64_t>(1, L / 8);
          so.acceptanceTraceBucket = width;
          SearchResult r = solve(gi.instance, so);
          for (size_t b = 0; b < r.traceTotal.size(); ++b) {
            if (r.traceTotal[b] == 0) continue;
            f << n << "," << engine << "," << L << "," << seed << "," << width << "," << b << ","
              << b * width << "," << double(b * width) / L << "," << r.traceTotal[b] << ","
              << r.traceUphillOffered[b] << "," << r.traceUphillAccepted[b] << "\n";
          }
        }
      }
    }
  }
  std::printf("plateau done\n");
}

// Does the constructor ever leave a class unplaced?
//
// This is what entitles the paper to report complete feasibility rather than only a zero hard
// total.  The returned schedule is the lexicographic minimum, under (unplaced, hard, objective),
// of every incumbent the run ever held, and the first of those is the constructed schedule; so the
// number of unplaced classes in the result can never exceed the number in the construction.
// Measuring the construction on every instance the campaigns use therefore bounds every run.
// A zero time limit stops the search immediately after constructing, so what is scored here is the
// construction itself.
//
// The construction is *not* a function of the generated instance and the order alone: construct()
// shuffles its order with the worker's random number generator, which is seeded from the solver
// seed.  An audit indexed by instance seed alone would therefore certify the wrong constructions
// for every campaign that does not happen to use the factor campaign's solver seeds.  What is
// enumerated below is the actual (campaign, size, instance seed, solver seed, order) tuple of every
// run in the study, read off the drivers above; each row records those identifiers so that coverage
// can be checked against the published records rather than assumed.  The block counts must match
// scripts/run_all.sh, and analyse.py refuses to generate if any run in the records has no matching
// audit row.
void expConstruction(int) {
  struct Cell {
    const char* campaign;
    int instanceSeed;
    uint64_t solverSeed;
    const char* order;
  };
  std::vector<Cell> cells;
  // factors: 10 blocks, solver seed 424242+s, and the construction order is itself a factor, so
  // both orders are reachable.
  for (int s = 1; s <= 10; ++s) {
    cells.push_back({"factors", s, 424242ULL + s, "mrv"});
    cells.push_back({"factors", s, 424242ULL + s, "random"});
  }
  for (int s = 1; s <= 12; ++s) cells.push_back({"anchor", s, 55055ULL + s, "mrv"});
  for (int s = 1; s <= 6; ++s) cells.push_back({"budget", s, 90909ULL + s, "mrv"});
  for (int s = 1; s <= 5; ++s) cells.push_back({"plateau", s, 606ULL + s, "mrv"});
  // dispersion holds the instance fixed at index 1 and varies the solver seed.
  for (int s = 1; s <= 20; ++s) cells.push_back({"dispersion", 1, 700000ULL + s, "mrv"});

  auto f = openCsv("construction.csv",
                   "campaign,n,instance_seed,solver_seed,order,unplaced,hard,soft,objective,clean");
  for (int n : {400, 1600}) {
    for (const Cell& c : cells) {
      GeneratedInstance gi = makeInstance(n, c.instanceSeed);
      SearchOptions so = baseline(200000, c.solverSeed);
      so.construction = c.order;
      so.timeLimitMs = 0;
      SearchResult r = solve(gi.instance, so);
      Score sc = validate(gi.instance, r.best);
      f << c.campaign << "," << n << "," << c.instanceSeed << "," << c.solverSeed << "," << c.order
        << "," << sc.unplaced << "," << sc.hard() << "," << sc.soft() << ","
        << sc.objective(kDefaultBeta) << "," << (sc.clean() ? 1 : 0) << "\n";
    }
  }
  std::printf("construction done\n");
}

void expDispersion(int seeds, long long budget) {
  // The resolution limit of every other comparison in the paper: the spread of one configuration on
  // one instance at one budget, over many seeds.
  auto f = openCsv("dispersion.csv", "n,budget,seed,hard,soft,unplaced,objective,candidates,work,elapsed_ms,kicks,"
                                    "fresh_restarts,deep_phases,adoptions,clean");
  for (int n : {400, 1600}) {
    for (long long b : {budget / 4, budget, budget * 4}) {
      for (int seed = 1; seed <= seeds; ++seed) {
        GeneratedInstance gi = makeInstance(n, 1);
        SearchOptions so = baseline(b, 700000 + seed);
        SearchResult r = solve(gi.instance, so);
        Score sc = validate(gi.instance, r.best);
        f << n << "," << b << "," << seed << "," << sc.hard() << "," << sc.soft() << ","
          << sc.unplaced << "," << sc.objective(kDefaultBeta) << "," << r.candidates << ","
          << r.workUnits << "," << r.elapsedMs << "," << r.kicks << ","
          << r.freshRestarts << "," << r.deepPhases << "," << r.adoptions << ","
          << (sc.clean() ? 1 : 0) << "\n";
      }
    }
  }
  std::printf("dispersion done\n");
}

void writeInstances(const std::string& dir) {
  for (int n : {400, 1600}) {
    for (int seed = 1; seed <= 12; ++seed) {
      GeneratedInstance gi = makeInstance(n, seed);
      char path[512];
      std::snprintf(path, sizeof(path), "%s/n%05d-s%d.txt", dir.c_str(), n, seed);
      writeInstance(path, gi.instance);
      std::snprintf(path, sizeof(path), "%s/n%05d-s%d.planted.txt", dir.c_str(), n, seed);
      writeSchedule(path, gi.planted);
    }
  }
  std::printf("instances written\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: experiments <factors|anchor|budget|plateau|dispersion|construction|"
                "instances|selftest> "
                "[--seeds N] [--budget K] [--results DIR] [--data DIR]\n");
    return 2;
  }
  const std::string cmd = argv[1];
  int seeds = 20;
  // The study's budget.  run_all.sh passes it explicitly, but a subcommand run by hand without
  // --budget should run the experiment the paper reports, not a different one.
  long long budget = 200000;
  std::string data = "data";
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--budget") && i + 1 < argc) budget = std::atoll(argv[++i]);
    else if (!std::strcmp(argv[i], "--maxbudget") && i + 1 < argc)
      tt::setBudgetCap(std::atoll(argv[++i]));
    else if (!std::strcmp(argv[i], "--results") && i + 1 < argc) gResults = argv[++i];
    else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
  }
  if (cmd == "factors") expFactors(seeds, budget);
  else if (cmd == "anchor") expAnchorHistory(seeds, budget);
  else if (cmd == "budget") expBudget(seeds);
  else if (cmd == "plateau") expPlateau(seeds);
  else if (cmd == "dispersion") expDispersion(seeds, budget);
  else if (cmd == "construction") expConstruction(seeds);
  else if (cmd == "instances") writeInstances(data);
  else if (cmd == "selftest") return runSelfTest(gResults);
  else {
    std::printf("unknown subcommand %s\n", cmd.c_str());
    return 2;
  }
  return 0;
}
