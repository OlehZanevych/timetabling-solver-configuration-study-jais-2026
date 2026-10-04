// The mutable schedule, its bucket decomposition, and exact incremental evaluation.
//
// Every penalty term is a sum over (entity, day) buckets of a function of that bucket's contents
// alone (the separability lemma).  Moving one class therefore changes only the buckets its old and
// new placements touch -- at most 2*eta of them, where eta is the number of entities a class
// involves -- so the exact change in every term is computed by recomputing those buckets and
// nothing else.  The cost of a candidate move is O(eta * beta^2) with beta the mean bucket size,
// and is *independent of the number of classes in the instance*.
#pragma once

#include <cstdint>
#include <vector>

#include "model.hpp"

namespace tt {

/// The nine penalty terms, in the order used throughout.
enum Term {
  kLecturerConflict = 0,  ///< Pi_1
  kGroupConflict = 1,     ///< Pi_2
  kRoomConflict = 2,      ///< Pi_3
  kGroupTravel = 3,       ///< Pi_4
  kLecturerTravel = 4,    ///< Pi_5
  kAbstractOverflow = 5,  ///< Pi_6
  kLecturerWindow = 6,    ///< Pi_7
  kGroupWindow = 7,       ///< Pi_8
  kMixedOnlineDay = 8,    ///< Pi_9
  kNumTerms = 9
};

/// Default weights beta_i.  The first six terms are the hard ones; feasibility is enforced
/// lexicographically and these weights only rank schedules already comparable in feasibility.
constexpr double kDefaultBeta[kNumTerms] = {150, 100, 50, 90, 120, 50, 5, 20, 30};

/// A family of penalty terms that is *not* separable over (entity, day) buckets, plugged into the
/// evaluator from outside.
///
/// The nine terms above are bucket-separable, which is what makes their exact incremental
/// evaluation cost independent of the instance size.  Not every objective a reader may want is:
/// two of the four soft constraints of the standard curriculum-based formulation aggregate over a
/// whole course rather than over one (entity, day) pair, so no bucket function can express them.
/// This interface is how such a term is carried without weakening the decomposition: the state
/// tells it which class moved and where, and it maintains whatever aggregate it needs.  A null
/// pointer -- the case for every instance in this series apart from the benchmark study -- adds a
/// literal 0.0 to the objective and costs one branch per placement.
class ExtraTerms {
 public:
  virtual ~ExtraTerms() = default;
  /// Recompute from scratch for a whole schedule; `p[c].placed()` says whether c is placed.
  virtual void reset(const std::vector<Placement>& p) = 0;
  /// Class `c` moves from `from` to `to`; either may be unplaced.  Must be exactly invertible,
  /// because the candidate probe applies a move, reads the cost, and applies its inverse.
  virtual void move(int c, const Placement& from, const Placement& to) = 0;
  /// The terms' contribution to the objective, already weighted.
  virtual double value() const = 0;
  /// The part of that contribution a reader should see as a soft cost.
  virtual double soft() const = 0;
  /// What `value()` would become if class `c` moved from `from` to `to`, minus what it is now.
  ///
  /// The greedy re-insertion of the large neighbourhoods scores a candidate placement from the
  /// buckets it touches, which is exactly the information a non-separable term does not live in.
  /// This is the hook that lets such a term be seen at proposal time rather than only at
  /// acceptance time; returning zero -- the default -- leaves the proposal machinery blind to it,
  /// which is the comparison the benchmark study makes.
  virtual double delta(int c, const Placement& from, const Placement& to) {
    (void)c; (void)from; (void)to;
    return 0.0;
  }
};

/// The cached statistic of one (entity, day) bucket.
struct BucketStat {
  int32_t conflicts = 0;  ///< clashing pairs inside the bucket
  int32_t travel = 0;     ///< ordered pairs whose gap is too short for the journey
  int32_t winNum = 0, winDen = 0;   ///< idle periods, per calendar week
  int32_t mixNum = 0, mixDen = 0;   ///< mixed online/on-site day indicator, per calendar week
  int32_t overflow = 0;   ///< instants at which an abstract room is over capacity
  Mask occNum, occDen;    ///< the occupied tick set, per calendar week
  int32_t size = 0;       ///< number of classes in the bucket
};

class State {
 public:
  explicit State(const Instance& inst, const double* beta = kDefaultBeta);

  const Instance& instance() const { return inst_; }

  // ---- reading the schedule ------------------------------------------------------------------

  const Placement& placementOf(int c) const { return placement_[c]; }
  const std::vector<Placement>& placements() const { return placement_; }
  int unplaced() const { return unplaced_; }
  double term(int i) const { return pi_[i]; }

  /// The hard total H = Pi_1 + ... + Pi_6.
  double hard() const;
  /// The soft count reported to a user: idle periods and mixed days.
  double soft() const;
  /// f(sigma) with the three fractional terms rounded, as an independent validator computes it.
  double objective() const;
  /// The surrogate the search descends: identical, but with the fractional terms left unrounded,
  /// because rounding erases the gradient of a half-unit improvement in one calendar week.
  double surrogate() const;

  /// Buckets recomputed since the state was created.  A deterministic, machine-independent proxy
  /// for the work an operator has done; the credit-assignment study divides rewards by it.
  int64_t work() const { return work_; }

  // ---- changing the schedule -----------------------------------------------------------------

  /// Place (or move) a class.  Marks the affected buckets dirty; call flush() before reading any
  /// counter.  Several classes may be placed before one flush, so a bucket shared by two of them
  /// is recomputed once.
  void place(int c, const Placement& p);
  /// Lift a class out of the schedule entirely.
  void unplace(int c);
  /// Recompute every dirty bucket and bring the counters up to date.
  void flush();

  /// Apply a whole schedule at once (used by construction and by restarts).
  void setAll(const std::vector<Placement>& p);
  void clear();

  // ---- the two placement-time hard rules -----------------------------------------------------

  /// Room time-availability and the per-(entity, day) load cap.  Everything else is folded into
  /// the domains S(c) and R(c) at load time and cannot be violated by construction.
  bool placementAllowed(int c, int day, int time, Parity parity, int room) const;

  /// The load cap re-tested jointly for a set of classes already placed.  Operators that move more
  /// than one class at a time must call this: the cap is a property of a *set* of placements, and
  /// k classes can each satisfy it individually and breach it together.
  bool allLegal(const std::vector<int>& cs) const;

  // ---- bucket access, for the operators ------------------------------------------------------

  int bucketIndex(int entity, int day) const { return entity * stride_ + day; }
  const std::vector<int32_t>& members(int entity, int day) const {
    return members_[bucketIndex(entity, day)];
  }

  /// Mark every bucket of one entity dirty, so that the next flush() recomputes the entity's whole
  /// week rather than only the days a move touched.  This exists for the evaluator comparison of
  /// the computational study: an implementation that knows the penalty terms are separable by
  /// entity but not by day has to do exactly this much work per candidate.
  void markEntityWeekDirty(int entity) {
    for (int d = 0; d < stride_; ++d) markDirty(bucketIndex(entity, d));
  }
  const BucketStat& stat(int entity, int day) const { return stats_[bucketIndex(entity, day)]; }

  /// The entities a class touches at its current placement (participants, abstract room, room).
  void entitiesOf(int c, const Placement& p, std::vector<int32_t>& out) const;

  /// Exact change in the surrogate of placing class c at p, without committing to it.  Implemented
  /// by doing the move, flushing, and undoing -- which is cheap for the same reason the move is.
  double probe(int c, const Placement& p);

  /// The journey required between two classes at two placements, in minutes; see the case
  /// analysis in the model.  Public because the candidate-scoring function in the search needs it.
  int journey(int ca, const Placement& pa, int cb, const Placement& pb) const;

  /// Index of a class's duration in Instance::durations, precomputed.
  int durationIdx(int c) const { return durIdx_[c]; }

  /// Attach a non-separable term family.  The state does not own it and it must outlive the state.
  /// Passing null (the default) restores the pure bucket objective exactly: the added quantity is
  /// a literal zero, so every number this series reports is unchanged by the presence of the hook.
  void setExtraTerms(ExtraTerms* e) {
    extra_ = e;
    if (extra_) extra_->reset(placement_);
  }
  const ExtraTerms* extraTerms() const { return extra_; }

 private:
  void markDirty(int bucket);
  void recompute(int bucket);
  void addStat(int entity, const BucketStat& s, double sign);
  int buildingOf(int c, const Placement& p) const;

  const Instance& inst_;
  double beta_[kNumTerms];

  std::vector<Placement> placement_;
  std::vector<int32_t> durIdx_;
  std::vector<int32_t> building_;   ///< cached buildingOf(c, placement_[c])          ///< per class, index into Instance::durations
  std::vector<std::vector<int32_t>> members_;
  std::vector<BucketStat> stats_;
  std::vector<uint8_t> dirtyFlag_;
  std::vector<int32_t> dirtyList_;
  std::vector<int32_t> scratch_;

  ExtraTerms* extra_ = nullptr;   ///< not owned; null for a purely bucket-separable objective
  int stride_ = 8;   ///< buckets per entity: nDays + 1
  double pi_[kNumTerms] = {0};
  int unplaced_ = 0;
  int64_t work_ = 0;
};

}  // namespace tt
