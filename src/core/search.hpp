// The search: construction, acceptance, the neighbourhood portfolio, adaptive operator selection,
// and the three-level escape.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model.hpp"
#include "state.hpp"

namespace tt {

/// The operators, in the order their statistics are reported.
enum Operator {
  kMove = 0,
  kSwap,
  kChain,
  kKempe,
  kRuin,
  kRepack,
  kKopt,
  kDayFix,
  kWinFix,
  kNumOperators
};

const char* operatorName(int op);

/// How an operator's credit is normalised before the bandit uses it.
///   none        -- uniform selection; the bandit is off
///   application -- reward per application, the ALNS convention
///   work        -- reward per bucket recomputed: deterministic and machine-independent
///   time        -- reward per microsecond, the choice-function convention
enum class Credit { None, Application, Work, Time };

struct SearchOptions {
  int64_t timeLimitMs = 30000;
  int64_t candidateLimit = 0;   ///< 0 = unlimited; a candidate budget instead of a clock budget
  /// 0 = unlimited.  A budget counted in bucket recomputations -- the unit of work the evaluation
  /// actually spends.  It is the only budget under which a local search and a population method can
  /// be compared without one of them being charged for something the other does not do.
  int64_t workLimit = 0;
  int threads = 1;
  uint64_t seed = 1;

  // ---- acceptance ----------------------------------------------------------------------------
  std::string engine = "lahc";  ///< lahc | sa | dlas | schc | hc
  int lahcLength = 100;
  bool lahcCanonical = false;   ///< write the current cost back unconditionally (Burke and Bykov)
  int schcCounter = 1000;
  double saT0Factor = 0.0015;
  double saCooling = 0.995;
  int64_t saReheatAfter = 400000;
  double hardWeight = 0;        ///< 0 = max(1e6, 0.02 * surrogate of the constructed schedule)

  // ---- the neighbourhood portfolio -----------------------------------------------------------
  bool useMove = true, useSwap = true, useChain = true, useKempe = true;
  bool useLns = true, useRepack = true, useKopt = true;
  bool useDayFix = false, useWinFix = false;
  bool useClusters = true;
  int chainDepth = 3;
  int lnsMin = 6, lnsMax = 40;
  /// Which victim selector the ruin operator uses: -1 draws uniformly among all of them, otherwise
  /// 0 uniform, 1 relatedness (Shaw), 2 one (entity, day) bucket, 3 one community of the conflict
  /// graph, 4 the buckets carrying the largest idle-period cost.  Fixing it is what lets the
  /// selectors be compared against one another.
  int ruinSelector = -1;
  int koptK = 12, koptExtra = 0;
  bool koptIdentityCheck = true;

  // ---- adaptive operator selection -----------------------------------------------------------
  Credit credit = Credit::Work;
  int banditSegment = 4000;
  double banditDecay = 0.7;
  double banditScale = 300.0;
  double weightFloor = 0.05, weightCeiling = 40.0;

  // ---- targeting -----------------------------------------------------------------------------
  double hotShare = 0.7;
  int64_t hotRefresh = 50000;
  int roomSample = 96;
  int roomScanFullBelow = 256;

  // ---- escape --------------------------------------------------------------------------------
  bool useDeep = true;          ///< level 1: bounded strict-descent large-neighbourhood phase
  bool useKick = true;          ///< level 2: perturb and repair
  bool useFresh = true;         ///< level 3: forget the incumbent and construct again
  bool kickFromBest = true;     ///< anchor the perturbation at the incumbent, not the working state
  bool adaptiveKick = true;
  int kickMin = 12, kickMax = 300;
  /// What happens to the acceptance history when a perturbation is applied.  The iterated
  /// local search literature analyses the *anchor* of a perturbation at length and says nothing
  /// about this, yet the three policies produce materially different post-kick dynamics.
  ///   0  refill every cell at the perturbed state's cost
  ///   1  leave the history untouched
  ///   2  refill every cell at the incumbent's cost
  int kickHistoryPolicy = 0;
  int64_t stagnationMoves = 60000;
  int64_t deepEvery = 20000;
  int64_t restartAfter = 1200000;

  bool cooperate = true;
  int poolSize = 8;
  double poolMinDist = 0.01;

  // ---- construction --------------------------------------------------------------------------
  std::string construction = "mrv";  ///< mrv | random | shuffled-mrv (= mrv with a shuffled prefix)
  /// Start from this schedule instead of constructing one.  Used when the search is the local
  /// search inside a memetic algorithm, so that the comparison charges both methods the same work
  /// for the same thing.
  const std::vector<Placement>* warmStart = nullptr;

  // ---- a non-separable term family -----------------------------------------------------------
  /// Optional factory for an ExtraTerms carried alongside the bucket objective.  A factory rather
  /// than a pointer because each worker owns a State and an ExtraTerms is mutable state that two
  /// of them must not share.  Null -- the case for every instance in this series apart from the
  /// standard-benchmark study -- leaves the objective exactly the bucket-separable one.
  std::function<std::unique_ptr<ExtraTerms>()> extraFactory;

  // ---- reporting -----------------------------------------------------------------------------
  int64_t sampleEveryMs = 0;    ///< 0 = no trajectory
  int64_t sampleEveryCandidates = 0;  ///< machine-independent alternative to sampleEveryMs
  bool collectOperatorStats = true;
  /// Width, in candidate evaluations, of the buckets of the acceptance trace.  Zero turns the trace
  /// off.  With it on, the search records for every candidate the age of the current cost level --
  /// the number of candidates since the working cost last strictly improved -- and whether the
  /// candidate was a strictly worsening one that the acceptance rule nevertheless admitted.  That
  /// is the observable form of the bar-collapse theorem: the uphill acceptance rate must fall to
  /// zero within the history length of the last strict improvement.
  int64_t acceptanceTraceBucket = 0;
};

struct OperatorStat {
  int64_t draws = 0;        ///< times the operator was selected
  int64_t applied = 0;      ///< times it produced a candidate
  int64_t accepted = 0;
  int64_t improved = 0;
  int64_t work = 0;         ///< buckets recomputed
  double gain = 0;          ///< total surrogate reduction credited to it
  double finalWeight = 0;
};

struct TrajectoryPoint {
  int64_t ms = 0;
  int64_t candidates = 0;
  double hard = 0, soft = 0, objective = 0;
};

struct SearchResult {
  std::vector<Placement> best;
  double hard = 0, soft = 0, objective = 0;
  int unplaced = 0;
  int64_t candidates = 0, workUnits = 0, elapsedMs = 0;
  int64_t kicks = 0, freshRestarts = 0, deepPhases = 0, adoptions = 0;
  double constructedObjective = 0;
  std::vector<OperatorStat> ops;
  std::vector<TrajectoryPoint> trajectory;
  /// Acceptance trace, bucketed by the age of the current cost level; see
  /// SearchOptions::acceptanceTraceBucket.
  std::vector<int64_t> traceTotal, traceUphillOffered, traceUphillAccepted;
};

SearchResult solve(const Instance& inst, const SearchOptions& opt,
                   const double* beta = kDefaultBeta);

/// A ceiling on the candidate budget of every subsequent solve(), and the ceiling in force.
///
/// Several experiments set their own budgets -- a ladder that doubles, or a figure fixed by the
/// quantity being traced -- so a smoke run cannot be made short by passing a smaller --budget.  The
/// cap exists so that such a run can execute the whole design at a size that finishes in minutes.
/// Zero means no ceiling, which is the default and is what every reported number was produced under;
/// an experiment that walks a budget ladder skips the rungs above the ceiling rather than repeating
/// the highest one it is allowed, so a capped run never records a budget it did not spend.
void setBudgetCap(int64_t cap);
int64_t budgetCap();

/// Label propagation over the class-entity bipartite graph: the partition a large neighbourhood is
/// aimed at.  Exposed because one of the studies measures the partition itself.
std::vector<int32_t> buildClusters(const Instance& inst, uint64_t seed, int rounds = 6);

/// Modularity of a partition of the movable classes with respect to the class-class conflict graph
/// (two classes adjacent when they share an entity).  Used to compare the discovered communities
/// against a declared partition.
double partitionModularity(const Instance& inst, const std::vector<int32_t>& label);

}  // namespace tt
