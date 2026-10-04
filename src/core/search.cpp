#include "search.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

#include "hungarian.hpp"
#include "rng.hpp"

namespace tt {

const char* operatorName(int op) {
  switch (op) {
    case kMove: return "move";
    case kSwap: return "swap";
    case kChain: return "chain";
    case kKempe: return "kempe";
    case kRuin: return "ruin";
    case kRepack: return "repack";
    case kKopt: return "kopt";
    case kDayFix: return "dayfix";
    case kWinFix: return "winfix";
    default: return "?";
  }
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kClashPrice = 1e9;   ///< Lambda in the candidate-scoring function
constexpr double kTravelPrice = 1e7;  ///< Theta

/// The lexicographic incumbent key (unplaced, hard, f).  Nothing in f counts a class that is not
/// scheduled at all -- an unplaced class is in no bucket -- so deleting a class would *lower* f.
/// Every "is this better" test in the search uses this key, not f.
struct Key {
  int unplaced = 1 << 30;
  double hard = 1e300;
  double obj = 1e300;
  bool operator<(const Key& o) const {
    if (unplaced != o.unplaced) return unplaced < o.unplaced;
    if (hard != o.hard) return hard < o.hard;
    return obj < o.obj;
  }
};

struct Shared {
  std::mutex mu;
  Key bestKey;
  std::vector<Placement> best;
  std::vector<std::vector<Placement>> pool;
  std::vector<Key> poolKey;
  std::atomic<bool> stop{false};
};

}  // namespace

// =================================================================================================
//  Clusters
// =================================================================================================

std::vector<int32_t> buildClusters(const Instance& inst, uint64_t seed, int rounds) {
  Rng rng(seed);
  const int nC = inst.nClasses();
  const int nE = inst.nEntities();
  std::vector<int32_t> classLabel(nC), entityLabel(nE, -1);
  for (int c = 0; c < nC; ++c) classLabel[c] = c;

  std::vector<std::vector<int32_t>> entityClasses(nE);
  for (int c = 0; c < nC; ++c)
    for (int32_t e : inst.classes[c].staticEntities) entityClasses[e].push_back(c);

  std::vector<int32_t> tally;
  auto plurality = [&](const std::vector<int32_t>& labels) -> int32_t {
    if (labels.empty()) return -1;
    tally = labels;
    std::sort(tally.begin(), tally.end());
    int32_t best = tally[0];
    int bestCount = 0, run = 0;
    for (size_t i = 0; i < tally.size(); ++i) {
      run = (i > 0 && tally[i] == tally[i - 1]) ? run + 1 : 1;
      if (run > bestCount || (run == bestCount && rng.coin(0.5))) {
        bestCount = run;
        best = tally[i];
      }
    }
    return best;
  };

  std::vector<int32_t> buf;
  for (int r = 0; r < rounds; ++r) {
    for (int e = 0; e < nE; ++e) {
      buf.clear();
      for (int32_t c : entityClasses[e]) buf.push_back(classLabel[c]);
      entityLabel[e] = plurality(buf);
    }
    for (int c = 0; c < nC; ++c) {
      buf.clear();
      for (int32_t e : inst.classes[c].staticEntities)
        if (entityLabel[e] >= 0) buf.push_back(entityLabel[e]);
      const int32_t l = plurality(buf);
      if (l >= 0) classLabel[c] = l;
    }
  }

  // Compact the labels.
  std::vector<int32_t> remap(nC, -1);
  int next = 0;
  for (int c = 0; c < nC; ++c) {
    if (remap[classLabel[c]] < 0) remap[classLabel[c]] = next++;
    classLabel[c] = remap[classLabel[c]];
  }
  return classLabel;
}

double partitionModularity(const Instance& inst, const std::vector<int32_t>& label) {
  // The class-class conflict graph: two classes adjacent when they share a lecturer or a group.
  const int nC = inst.nClasses();
  std::vector<std::vector<int32_t>> ent(inst.nEntities());
  for (int c = 0; c < nC; ++c)
    for (int32_t e : inst.classes[c].staticEntities)
      if (!inst.isAbstractEntity(e)) ent[e].push_back(c);

  std::vector<double> degree(nC, 0.0);
  double m2 = 0.0, inside = 0.0;
  for (const auto& members : ent) {
    for (size_t i = 0; i < members.size(); ++i)
      for (size_t j = i + 1; j < members.size(); ++j) {
        const int a = members[i], b = members[j];
        degree[a] += 1;
        degree[b] += 1;
        m2 += 2;
        if (label[a] == label[b]) inside += 2;
      }
  }
  if (m2 == 0) return 0.0;
  std::vector<double> comDeg;
  for (int c = 0; c < nC; ++c) {
    const int l = label[c];
    if (static_cast<int>(comDeg.size()) <= l) comDeg.resize(l + 1, 0.0);
    comDeg[l] += degree[c];
  }
  double expected = 0.0;
  for (double d : comDeg) expected += (d / m2) * (d / m2);
  return inside / m2 - expected;
}

// =================================================================================================
//  Worker
// =================================================================================================

namespace {

class Worker {
 public:
  Worker(const Instance& inst, const SearchOptions& opt, const double* beta, uint64_t seed,
         Shared& shared)
      : in_(inst), opt_(opt), rng_(seed), st_(inst, beta), shared_(shared) {
    // A worker owns its own copy of any non-separable term family, because it is mutable state
    // that travels with the schedule the worker is holding.
    if (opt.extraFactory) {
      extraOwn_ = opt.extraFactory();
      extra_ = extraOwn_.get();
      st_.setExtraTerms(extra_);
    }
    for (int i = 0; i < kNumTerms; ++i) beta_[i] = beta[i];
    enabled_.assign(kNumOperators, false);
    enabled_[kMove] = opt.useMove;
    enabled_[kSwap] = opt.useSwap;
    enabled_[kChain] = opt.useChain;
    enabled_[kKempe] = opt.useKempe;
    enabled_[kRuin] = opt.useLns;
    enabled_[kRepack] = opt.useRepack;
    enabled_[kKopt] = opt.useKopt;
    enabled_[kDayFix] = opt.useDayFix;
    enabled_[kWinFix] = opt.useWinFix;
    for (int i = 0; i < kNumOperators; ++i)
      if (enabled_[i]) active_.push_back(i);
    if (active_.empty()) {
      enabled_[kMove] = true;
      active_.push_back(kMove);
    }
    weight_.assign(kNumOperators, 1.0);
    weight_[kMove] = 4.0;
    weight_[kSwap] = 4.0;
    stats_.assign(kNumOperators, OperatorStat{});
    segReward_.assign(kNumOperators, 0.0);
    segWork_.assign(kNumOperators, 0.0);
    segTime_.assign(kNumOperators, 0.0);
    segDraws_.assign(kNumOperators, 0.0);
    traceTotal_.assign(64, 0);
    traceUpOffered_.assign(64, 0);
    traceUpAccepted_.assign(64, 0);
  }

  void setClusters(const std::vector<int32_t>* cl, const std::vector<std::vector<int32_t>>* mem) {
    cluster_ = cl;
    clusterMembers_ = mem;
  }

  void run(Clock::time_point deadline, SearchResult& out);

  const State& state() const { return st_; }
  int64_t candidates() const { return candidates_; }

 private:
  // ---- construction ---------------------------------------------------------------------------
  void construct();

  // ---- candidate scoring ----------------------------------------------------------------------
  double scoreCandidate(int c, int day, int time, Parity parity, int room);
  /// The part of the candidate score that does not depend on the room: clashes and idle-period
  /// change over the class's own participants.  Hoisting it out of the room loop is what makes a
  /// full scan of S(c) x R(c) affordable.
  double peopleScore(int c, int day, int time, Parity parity, double& clashesOut);
  int roomClashes(int c, int day, int time, Parity parity, int room);
  int travelViolations(int c, int day, int time, Parity parity, int room);
  bool capOkPeople(int c, int day);
  bool scanBest(int c, bool wide, Placement& out);

  // ---- acceptance -----------------------------------------------------------------------------
  double cost() const {
    return lambda_ * st_.hard() + st_.surrogate() + 8.0 * lambda_ * st_.unplaced();
  }
  bool acceptTest(double candidate, double current);
  void refillHistory(double c);
  /// Write one history cell while maintaining the running maximum in O(1) amortised time.  The
  /// maximum is recomputed only when the last cell holding it is overwritten.
  void replaceHistory(size_t idx, double value);

  // ---- candidate bookkeeping ------------------------------------------------------------------
  void begin() { journal_.clear(); }
  void doPlace(int c, const Placement& p) {
    journal_.push_back({c, st_.placementOf(c)});
    st_.place(c, p);
  }
  void rollback() {
    for (size_t i = journal_.size(); i-- > 0;) st_.place(journal_[i].first, journal_[i].second);
    st_.flush();
    journal_.clear();
  }

  // ---- operators ------------------------------------------------------------------------------
  bool opMove();
  bool opSwap();
  bool opChain();
  bool opKempe();
  bool opRuin();
  bool opRepack();
  bool opKopt();
  bool opDayFix();
  bool opWinFix();
  bool apply(int op);

  // ---- targeting ------------------------------------------------------------------------------
  int pick();
  void refreshLists();
  int selectVictims(int selector, int k, std::vector<int>& out);
  bool recreate(std::vector<int>& victims);
  void refinePairs(const std::vector<int>& members, int rounds);

  // ---- escape ---------------------------------------------------------------------------------
  bool deepPhase();
  void kickFromBest();
  void restartFresh();
  bool adoptElite();
  /// Record the working state as the incumbent if it is lexicographically better.
  bool noteIncumbent();

  int selectOperator();
  void endSegment();

  const Instance& in_;
  const SearchOptions& opt_;
  Rng rng_;
  State st_;
  std::unique_ptr<ExtraTerms> extraOwn_;   ///< owned; attached to st_ when a factory is supplied
  ExtraTerms* extra_ = nullptr;            ///< == extraOwn_.get(), or null
  Shared& shared_;
  double beta_[kNumTerms];

  std::vector<uint8_t> enabled_;
  std::vector<int> active_;
  std::vector<double> weight_, segReward_, segWork_, segTime_, segDraws_;
  std::vector<OperatorStat> stats_;
  int64_t segApplied_ = 0;

  std::vector<std::pair<int, Placement>> journal_;
  std::vector<double> history_;
  double dlasMax_ = 0;
  int64_t dlasMaxCount_ = 0;
  double schcBound_ = 0;
  int64_t schcCount_ = 0;
  double temperature_ = 0;
  double lambda_ = 1e6;

  std::vector<Placement> incumbent_;
  Key incumbentKey_;
  double incumbentSoft_ = 0;
  int64_t candidates_ = 0;
  int64_t sinceBest_ = 0, sinceDeep_ = 0, movesSinceIncumbent_ = 0, sinceAdopt_ = 0;
  int64_t blockAdoptUntil_ = 0;
  int kickSize_ = 12;
  int koptWidth_ = 4;

  std::vector<int64_t> traceTotal_, traceUpOffered_, traceUpAccepted_;
  int64_t levelAge_ = 0;
  std::vector<int32_t> hot_, warm_;
  const std::vector<int32_t>* cluster_ = nullptr;
  const std::vector<std::vector<int32_t>>* clusterMembers_ = nullptr;

  std::vector<int32_t> ents_;
  std::vector<int> victims_, scratchInt_;

 public:
  int64_t kicks_ = 0, fresh_ = 0, deeps_ = 0, adoptions_ = 0;
  double constructedObjective_ = 0;
};

// ---- construction -------------------------------------------------------------------------------

void Worker::construct() {
  st_.clear();
  std::vector<int32_t> order = in_.movable;
  rng_.shuffle(order);
  if (opt_.construction != "random") {
    // Most-constrained-first.  The shuffle above matters: the difficulty key is coarse, thousands
    // of classes share a value, and without it every construction is the same construction, so a
    // restart lands exactly where the last one did.
    std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
      const ClassSpec& ca = in_.classes[a];
      const ClassSpec& cb = in_.classes[b];
      const int64_t da = static_cast<int64_t>(ca.slots.size()) * std::max<size_t>(1, ca.rooms.size());
      const int64_t db = static_cast<int64_t>(cb.slots.size()) * std::max<size_t>(1, cb.rooms.size());
      if (da != db) return da < db;
      return ca.lecturers.size() + ca.groups.size() > cb.lecturers.size() + cb.groups.size();
    });
  }
  Placement p;
  for (int32_t c : order) {
    if (scanBest(c, false, p) || scanBest(c, true, p)) st_.place(c, p);
  }
  st_.flush();
}

// ---- candidate scoring --------------------------------------------------------------------------

bool Worker::capOkPeople(int c, int day) {
  const ClassSpec& cs = in_.classes[c];
  for (int32_t e : cs.staticEntities) {
    if (in_.isAbstractEntity(e)) continue;
    const int cap = in_.maxPerDay[e];
    if (cap >= (1 << 20)) continue;
    int n = static_cast<int>(st_.members(e, day).size());
    for (int32_t x : st_.members(e, day))
      if (x == c) --n;
    if (n + 1 > cap) return false;
  }
  return true;
}

double Worker::peopleScore(int c, int day, int time, Parity parity, double& clashesOut) {
  const ClassSpec& cs = in_.classes[c];
  const Mask& mu = in_.maskOf(time, st_.durationIdx(c));
  double clashes = 0, windowDelta = 0;
  for (int32_t e : cs.staticEntities) {
    const bool abstractEntity = in_.isAbstractEntity(e);
    const std::vector<int32_t>& mem = st_.members(e, day);
    if (!abstractEntity) {
      for (int32_t m : mem) {
        if (m == c) continue;
        const Placement& pm = st_.placementOf(m);
        if (!weeksOverlap(parity, pm.parity)) continue;
        if (mu.intersects(in_.maskOf(pm.time, st_.durationIdx(m)))) ++clashes;
      }
      const BucketStat& s = st_.stat(e, day);
      for (int w = 1; w <= 2; ++w) {
        if (!inWeek(parity, w)) continue;
        const Mask& occ = (w == 1) ? s.occNum : s.occDen;
        const int before = occ.span().andNot(occ).operator&(in_.bells).count();
        const Mask after = occ | mu;
        const int post = after.span().andNot(after).operator&(in_.bells).count();
        windowDelta += 0.5 * (post - before);
      }
    }
  }
  clashesOut = clashes;
  return kClashPrice * clashes + windowDelta;
}

int Worker::roomClashes(int c, int day, int time, Parity parity, int room) {
  if (room < 0) return 0;
  const Mask& mu = in_.maskOf(time, st_.durationIdx(c));
  int n = 0;
  for (int32_t m : st_.members(in_.roomId(room), day)) {
    if (m == c) continue;
    const Placement& pm = st_.placementOf(m);
    if (!weeksOverlap(parity, pm.parity)) continue;
    if (mu.intersects(in_.maskOf(pm.time, st_.durationIdx(m)))) ++n;
  }
  return n;
}

int Worker::travelViolations(int c, int day, int time, Parity parity, int room) {
  const ClassSpec& cs = in_.classes[c];
  const Mask& mu = in_.maskOf(time, st_.durationIdx(c));
  Placement cand{static_cast<int8_t>(day), static_cast<int16_t>(time), parity,
                 static_cast<int32_t>(room)};
  const int sc = in_.times[time].startMinute, ec = sc + cs.duration;
  int bad = 0;
  for (int32_t e : cs.staticEntities) {
    if (in_.isAbstractEntity(e) || in_.isRoomEntity(e)) continue;
    for (int32_t m : st_.members(e, day)) {
      if (m == c) continue;
      const Placement& pm = st_.placementOf(m);
      if (!weeksOverlap(parity, pm.parity)) continue;
      if (mu.intersects(in_.maskOf(pm.time, st_.durationIdx(m)))) continue;
      const int sm = in_.times[pm.time].startMinute, em = sm + in_.classes[m].duration;
      if (sc < sm) {
        if (sm - ec < st_.journey(c, cand, m, pm)) ++bad;
      } else if (sm < sc) {
        if (sc - em < st_.journey(m, pm, c, cand)) ++bad;
      }
    }
  }
  return bad;
}

double Worker::scoreCandidate(int c, int day, int time, Parity parity, int room) {
  double clashes = 0;
  const double base = peopleScore(c, day, time, parity, clashes);
  const Placement cand{static_cast<int8_t>(day), static_cast<int16_t>(time), parity,
                       static_cast<int32_t>(room)};
  return base + kClashPrice * roomClashes(c, day, time, parity, room) +
         kTravelPrice * travelViolations(c, day, time, parity, room) +
         (extra_ ? extra_->delta(c, st_.placementOf(c), cand) : 0.0);
}

bool Worker::scanBest(int c, bool wide, Placement& out) {
  const ClassSpec& cs = in_.classes[c];
  if (cs.slots.empty()) return false;
  double best = 1e300;
  bool found = false;

  const size_t nRooms = cs.rooms.size();
  const bool sample = !wide && nRooms > static_cast<size_t>(opt_.roomScanFullBelow);
  const size_t take = sample ? static_cast<size_t>(opt_.roomSample) : nRooms;
  const size_t roomOffset = nRooms ? (rng_.next() % nRooms) : 0;

  int lastDay = -1;
  bool dayCapOk = false;
  for (const Slot& s : cs.slots) {
    if (s.day != lastDay) {
      lastDay = s.day;
      dayCapOk = capOkPeople(c, s.day);
    }
    if (!dayCapOk) continue;

    // The room-independent half, computed once per slot.
    double clashes = 0;
    const double base = peopleScore(c, s.day, s.time, s.parity, clashes);
    if (base >= best) continue;

    if (nRooms == 0) {
      Placement cand{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, -1};
      const double total = base + kTravelPrice * travelViolations(c, s.day, s.time, s.parity, -1) +
                           (extra_ ? extra_->delta(c, st_.placementOf(c), cand) : 0.0);
      if (total < best) {
        best = total;
        out = Placement{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, -1};
        found = true;
      }
      continue;
    }

    for (size_t k = 0; k < take; ++k) {
      const int r = cs.rooms[(roomOffset + k) % nRooms];
      const double partial = base + kClashPrice * roomClashes(c, s.day, s.time, s.parity, r);
      // The travel term is the expensive one; evaluate it only for a candidate that is already
      // better than the best seen without it.
      if (partial >= best) continue;
      if (!in_.roomFree(r, s.day, s.time)) continue;
      const int cap = in_.maxPerDay[in_.roomId(r)];
      if (cap < (1 << 20)) {
        int n = static_cast<int>(st_.members(in_.roomId(r), s.day).size());
        for (int32_t x : st_.members(in_.roomId(r), s.day))
          if (x == c) --n;
        if (n + 1 > cap) continue;
      }
      Placement cand{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, r};
      const double total = partial +
                           kTravelPrice * travelViolations(c, s.day, s.time, s.parity, r) +
                           (extra_ ? extra_->delta(c, st_.placementOf(c), cand) : 0.0);
      if (total < best) {
        best = total;
        out = cand;
        found = true;
      }
    }
  }
  return found;
}

// ---- acceptance -----------------------------------------------------------------------------------

void Worker::refillHistory(double c) {
  history_.assign(std::max(1, opt_.lahcLength), c);
  dlasMax_ = c;
  dlasMaxCount_ = static_cast<int64_t>(history_.size());
  schcBound_ = c;
  schcCount_ = 0;
}

void Worker::replaceHistory(size_t idx, double value) {
  const double old = history_[idx];
  history_[idx] = value;
  if (value > dlasMax_) {
    dlasMax_ = value;
    dlasMaxCount_ = 1;
    return;
  }
  if (value == dlasMax_) {
    if (old != dlasMax_) ++dlasMaxCount_;
    return;
  }
  if (old == dlasMax_ && --dlasMaxCount_ == 0) {
    dlasMax_ = history_[0];
    dlasMaxCount_ = 0;
    for (double v : history_) {
      if (v > dlasMax_) {
        dlasMax_ = v;
        dlasMaxCount_ = 1;
      } else if (v == dlasMax_) {
        ++dlasMaxCount_;
      }
    }
  }
}

bool Worker::acceptTest(double candidate, double cur) {
  if (opt_.engine == "hc") return candidate <= cur;

  if (opt_.engine == "sa") {
    if (candidate <= cur) return true;
    if (temperature_ <= 0) return false;
    return rng_.unit() < std::exp(-(candidate - cur) / temperature_);
  }

  if (opt_.engine == "schc") {
    // Step counting hill climbing: one bound, held for a counted number of steps.
    const bool ok = candidate <= schcBound_ || candidate <= cur;
    if (++schcCount_ >= opt_.schcCounter) {
      schcBound_ = cur;
      schcCount_ = 0;
    }
    return ok;
  }

  const size_t L = history_.size();
  const size_t idx = static_cast<size_t>(candidates_ % L);

  if (opt_.engine == "dlas") {
    // Diversified late acceptance (Namazi et al.): the bar is the *maximum* of the history rather
    // than one cell of it, and a cell is replaced only when the candidate is above it, or below it
    // and below the current cost.  That replacement rule is what keeps the history from filling
    // with copies of one value, which is the failure mode the method was designed against.
    const bool ok = candidate < dlasMax_ || candidate <= cur;
    const double newCurrent = ok ? candidate : cur;
    if (newCurrent > history_[idx] || (newCurrent < history_[idx] && newCurrent < cur)) {
      replaceHistory(idx, newCurrent);
    }
    return ok;
  }

  // Late acceptance hill climbing.
  const bool ok = candidate <= history_[idx] || candidate <= cur;
  if (ok) {
    if (opt_.lahcCanonical) history_[idx] = candidate;
    else if (candidate < history_[idx]) history_[idx] = candidate;
  } else if (opt_.lahcCanonical) {
    history_[idx] = cur;
  }
  return ok;
}

// ---- targeting ---------------------------------------------------------------------------------

void Worker::refreshLists() {
  hot_.clear();
  warm_.clear();
  const int nE = in_.nEntities();
  for (int e = 0; e < nE; ++e) {
    for (int d = 1; d <= in_.nDays; ++d) {
      const BucketStat& s = st_.stat(e, d);
      if (s.size == 0) continue;
      if (s.conflicts || s.travel || s.overflow) {
        for (int32_t c : st_.members(e, d))
          if (in_.classes[c].movable) hot_.push_back(c);
      } else if (s.winNum || s.winDen || s.mixNum || s.mixDen) {
        for (int32_t c : st_.members(e, d))
          if (in_.classes[c].movable) warm_.push_back(c);
      }
    }
  }
}

int Worker::pick() {
  const std::vector<int32_t>& list = (st_.hard() > 0) ? hot_ : warm_;
  if (!list.empty() && rng_.coin(opt_.hotShare))
    return list[rng_.below(static_cast<uint32_t>(list.size()))];
  return in_.movable[rng_.below(static_cast<uint32_t>(in_.movable.size()))];
}

// ---- operators ----------------------------------------------------------------------------------

bool Worker::opMove() {
  const int c = pick();
  const ClassSpec& cs = in_.classes[c];
  if (cs.slots.empty()) return false;
  const Slot& s = cs.slots[rng_.below(static_cast<uint32_t>(cs.slots.size()))];
  const int room = cs.rooms.empty()
                       ? -1
                       : cs.rooms[rng_.below(static_cast<uint32_t>(cs.rooms.size()))];
  if (!st_.placementAllowed(c, s.day, s.time, s.parity, room)) return false;
  Placement p{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, room};
  if (p == st_.placementOf(c)) return false;
  doPlace(c, p);
  return true;
}

bool Worker::opSwap() {
  const int i = pick();
  const ClassSpec& ci = in_.classes[i];
  if (ci.slots.empty() || ci.rooms.empty()) return false;
  const Slot& s = ci.slots[rng_.below(static_cast<uint32_t>(ci.slots.size()))];
  const int room = ci.rooms[rng_.below(static_cast<uint32_t>(ci.rooms.size()))];

  // Whoever is in the way, rather than a randomly drawn second class: at high density a random
  // pair almost never has compatible domains.
  int j = -1;
  for (int32_t m : st_.members(in_.roomId(room), s.day)) {
    if (m == i) continue;
    const Placement& pm = st_.placementOf(m);
    if (pm.time == s.time && weeksOverlap(pm.parity, s.parity) && in_.classes[m].movable) {
      j = m;
      break;
    }
  }

  const Placement pi = st_.placementOf(i);
  Placement target{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, room};
  if (j < 0) {
    if (!st_.placementAllowed(i, s.day, s.time, s.parity, room)) return false;
    if (target == pi) return false;
    doPlace(i, target);
    return true;
  }
  if (!pi.placed()) return false;
  // The partner must be able to take i's placement.
  const ClassSpec& cj = in_.classes[j];
  bool ok = false;
  for (const Slot& t : cj.slots)
    if (t.day == pi.day && t.time == pi.time && t.parity == pi.parity) {
      ok = true;
      break;
    }
  if (!ok) return false;
  if (pi.room >= 0 && std::find(cj.rooms.begin(), cj.rooms.end(), pi.room) == cj.rooms.end())
    return false;
  doPlace(i, target);
  doPlace(j, pi);
  scratchInt_ = {i, j};
  st_.flush();
  if (!st_.allLegal(scratchInt_)) return false;
  return true;
}

bool Worker::opChain() {
  const int i = pick();
  const ClassSpec& ci = in_.classes[i];
  if (ci.slots.empty()) return false;
  const Slot& s = ci.slots[rng_.below(static_cast<uint32_t>(ci.slots.size()))];
  const int room = ci.rooms.empty()
                       ? -1
                       : ci.rooms[rng_.below(static_cast<uint32_t>(ci.rooms.size()))];
  if (!st_.placementAllowed(i, s.day, s.time, s.parity, room)) return false;
  doPlace(i, Placement{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, room});
  st_.flush();

  int last = i;
  for (int d = 0; d < opt_.chainDepth; ++d) {
    const Placement pl = st_.placementOf(last);
    if (!pl.placed()) break;
    const Mask ml = in_.maskOf(pl.time, st_.durationIdx(last));
    int victim = -1;
    ents_ = in_.classes[last].staticEntities;
    if (pl.room >= 0) ents_.push_back(in_.roomId(pl.room));
    for (int32_t e : ents_) {
      if (in_.isAbstractEntity(e)) continue;
      for (int32_t m : st_.members(e, pl.day)) {
        if (m == last || !in_.classes[m].movable) continue;
        const Placement& pm = st_.placementOf(m);
        if (!weeksOverlap(pl.parity, pm.parity)) continue;
        if (ml.intersects(in_.maskOf(pm.time, st_.durationIdx(m)))) {
          victim = m;
          break;
        }
      }
      if (victim >= 0) break;
    }
    if (victim < 0) break;

    // The displaced class goes where *it* would choose, not into the slot just vacated.  That is
    // what lets the operator reach rearrangements every intermediate state of which is worse than
    // both ends.
    const Placement was = st_.placementOf(victim);
    st_.unplace(victim);
    st_.flush();
    Placement target;
    const bool ok = scanBest(victim, false, target) || scanBest(victim, true, target);
    st_.place(victim, was);
    st_.flush();
    if (!ok || target == was) break;
    doPlace(victim, target);
    st_.flush();
    last = victim;
  }
  return true;
}

bool Worker::opKempe() {
  const int i = pick();
  const ClassSpec& ci = in_.classes[i];
  if (ci.slots.empty()) return false;
  const Placement pi = st_.placementOf(i);
  if (!pi.placed()) return false;
  const Slot& s = ci.slots[rng_.below(static_cast<uint32_t>(ci.slots.size()))];
  if (s.day == pi.day && s.time == pi.time && s.parity == pi.parity) return false;

  // The alternating set: everything that blocks a member between the source and the target slot.
  std::vector<int> set{i};
  std::vector<int> toSlot{1};  // 1 = move to target, 0 = move to source
  for (size_t head = 0; head < set.size() && set.size() < 12; ++head) {
    const int c = set[head];
    const int dir = toSlot[head];
    const int day = dir ? s.day : pi.day;
    const int time = dir ? s.time : pi.time;
    const Parity par = dir ? s.parity : pi.parity;
    const Mask& mu = in_.maskOf(time, st_.durationIdx(c));
    for (int32_t e : in_.classes[c].staticEntities) {
      if (in_.isAbstractEntity(e)) continue;
      for (int32_t m : st_.members(e, day)) {
        if (!in_.classes[m].movable) continue;
        if (std::find(set.begin(), set.end(), m) != set.end()) continue;
        const Placement& pm = st_.placementOf(m);
        if (!weeksOverlap(par, pm.parity)) continue;
        if (!mu.intersects(in_.maskOf(pm.time, st_.durationIdx(m)))) continue;
        set.push_back(m);
        toSlot.push_back(1 - dir);
        if (set.size() >= 12) break;
      }
      if (set.size() >= 12) break;
    }
  }
  if (set.size() < 2) return false;

  for (size_t k = 0; k < set.size(); ++k) {
    const int c = set[k];
    const Placement& pc = st_.placementOf(c);
    const int day = toSlot[k] ? s.day : pi.day;
    const int time = toSlot[k] ? s.time : pi.time;
    const Parity par = toSlot[k] ? s.parity : pi.parity;
    bool ok = false;
    for (const Slot& t : in_.classes[c].slots)
      if (t.day == day && t.time == time && t.parity == par) {
        ok = true;
        break;
      }
    if (!ok) return false;
    if (!st_.placementAllowed(c, day, time, par, pc.room)) return false;
    doPlace(c, Placement{static_cast<int8_t>(day), static_cast<int16_t>(time), par, pc.room});
  }
  st_.flush();
  scratchInt_.assign(set.begin(), set.end());
  return st_.allLegal(scratchInt_);
}

int Worker::selectVictims(int selector, int k, std::vector<int>& out) {
  out.clear();
  if (k <= 0) return 0;
  switch (selector) {
    case 0: {  // uniform
      for (int i = 0; i < k; ++i)
        out.push_back(in_.movable[rng_.below(static_cast<uint32_t>(in_.movable.size()))]);
      break;
    }
    case 1: {  // Shaw-style relatedness: a seed and everything sharing an entity with it
      const int seed = pick();
      out.push_back(seed);
      for (int32_t e : in_.classes[seed].staticEntities) {
        if (in_.isAbstractEntity(e)) continue;
        for (int d = 1; d <= in_.nDays; ++d)
          for (int32_t m : st_.members(e, d))
            if (in_.classes[m].movable) out.push_back(m);
      }
      break;
    }
    case 2: {  // one (entity, day) bucket
      const int seed = pick();
      const Placement& p = st_.placementOf(seed);
      if (!p.placed()) return 0;
      const std::vector<int32_t>& ents = in_.classes[seed].staticEntities;
      if (ents.empty()) return 0;
      const int e = ents[rng_.below(static_cast<uint32_t>(ents.size()))];
      for (int32_t m : st_.members(e, p.day))
        if (in_.classes[m].movable) out.push_back(m);
      break;
    }
    case 3: {  // one community of the conflict graph
      if (!clusterMembers_ || clusterMembers_->empty()) return 0;
      const size_t ci = rng_.below(static_cast<uint32_t>(clusterMembers_->size()));
      for (int32_t m : (*clusterMembers_)[ci])
        if (in_.classes[m].movable) out.push_back(m);
      break;
    }
    default: {  // the buckets with the largest window cost
      const int seed = warm_.empty() ? pick() : warm_[rng_.below(static_cast<uint32_t>(warm_.size()))];
      const Placement& p = st_.placementOf(seed);
      if (!p.placed()) return 0;
      for (int32_t e : in_.classes[seed].staticEntities) {
        if (in_.isAbstractEntity(e)) continue;
        const BucketStat& s = st_.stat(e, p.day);
        if (s.winNum || s.winDen)
          for (int32_t m : st_.members(e, p.day))
            if (in_.classes[m].movable) out.push_back(m);
      }
      break;
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  // Truncate by shuffling, so a bucket or a community larger than k is sampled, not removed whole.
  if (static_cast<int>(out.size()) > k) {
    for (size_t i = out.size(); i > 1; --i) std::swap(out[i - 1], out[rng_.below(static_cast<uint32_t>(i))]);
    out.resize(k);
  }
  return static_cast<int>(out.size());
}

bool Worker::recreate(std::vector<int>& victims) {
  for (size_t i = victims.size(); i > 1; --i)
    std::swap(victims[i - 1], victims[rng_.below(static_cast<uint32_t>(i))]);
  Placement p;
  for (int v : victims) {
    if (scanBest(v, false, p) || scanBest(v, true, p)) {
      st_.place(v, p);
      st_.flush();
    } else {
      // Restore it where it was rather than leaving it out: a ruin that drops a class looks like an
      // improvement to any objective that does not count what is missing.
      for (size_t i = journal_.size(); i-- > 0;)
        if (journal_[i].first == v) {
          st_.place(v, journal_[i].second);
          st_.flush();
          break;
        }
    }
  }
  return true;
}

bool Worker::opRuin() {
  const int k = opt_.lnsMin + static_cast<int>(rng_.below(
                                  static_cast<uint32_t>(std::max(1, opt_.lnsMax - opt_.lnsMin + 1))));
  const int selector = opt_.ruinSelector >= 0
                           ? opt_.ruinSelector
                           : static_cast<int>(rng_.below(clusterMembers_ ? 5 : 4));
  if (selectVictims(selector, k, victims_) == 0) return false;
  for (int v : victims_) {
    journal_.push_back({v, st_.placementOf(v)});
    st_.unplace(v);
  }
  st_.flush();
  recreate(victims_);
  return st_.allLegal(victims_);
}

bool Worker::opRepack() {
  if (selectVictims(2, 16, victims_) == 0) return false;
  for (int v : victims_) {
    journal_.push_back({v, st_.placementOf(v)});
    st_.unplace(v);
  }
  st_.flush();
  recreate(victims_);
  refinePairs(victims_, 1);
  return st_.allLegal(victims_);
}

void Worker::refinePairs(const std::vector<int>& members, int rounds) {
  // The relaxation the permutation operator solves ignores the residual coupling among its
  // members; this repairs it under the true objective.  It is O(k^2) evaluations per round, so the
  // number of pairs examined is capped: past a few dozen the marginal repair is not worth the
  // draws it takes from the rest of the portfolio.
  int budget = 48;
  for (int r = 0; r < rounds && budget > 0; ++r) {
    bool changed = false;
    for (size_t a = 0; a < members.size() && budget > 0; ++a)
      for (size_t b = a + 1; b < members.size() && budget > 0; ++b) {
        --budget;
        const int x = members[a], y = members[b];
        const Placement px = st_.placementOf(x), py = st_.placementOf(y);
        if (!px.placed() || !py.placed()) continue;
        // Is the exchange admissible for both?
        auto admits = [&](int c, const Placement& p) {
          const ClassSpec& cs = in_.classes[c];
          bool slotOk = false;
          for (const Slot& s : cs.slots)
            if (s.day == p.day && s.time == p.time && s.parity == p.parity) {
              slotOk = true;
              break;
            }
          if (!slotOk) return false;
          if (p.room < 0) return cs.rooms.empty();
          return std::find(cs.rooms.begin(), cs.rooms.end(), p.room) != cs.rooms.end();
        };
        if (!admits(x, py) || !admits(y, px)) continue;
        const double before = st_.surrogate();
        st_.place(x, py);
        st_.place(y, px);
        st_.flush();
        // The exchange leaves the multiset of occupied placements alone, so it cannot create a
        // room clash -- but it moves two classes with different participants between days, and the
        // per-(entity, day) load cap is a property of a *set* of placements.  Two placements that
        // were each legal can be illegal after the exchange, so the cap is re-tested here.  Leaving
        // this out is how an operator that only ever swaps can still produce a schedule the
        // independent validator rejects.
        const std::vector<int> exchanged{x, y};
        if (st_.surrogate() < before - 1e-9 && st_.allLegal(exchanged)) {
          journal_.push_back({x, px});
          journal_.push_back({y, py});
          changed = true;
        } else {
          st_.place(x, px);
          st_.place(y, py);
          st_.flush();
        }
      }
    if (!changed) break;
  }
}

bool Worker::opKopt() {
  const int k = std::max(2, opt_.koptK);
  const int selector = (clusterMembers_ && rng_.coin(0.35)) ? 3 : 4;
  if (selectVictims(selector, k, victims_) < 2) return false;
  const int m = static_cast<int>(victims_.size());

  std::vector<Placement> slotsHeld;
  for (int v : victims_) {
    const Placement& p = st_.placementOf(v);
    if (!p.placed()) return false;
    slotsHeld.push_back(p);
  }
  for (int i = 0; i < opt_.koptExtra; ++i) {
    const int v = victims_[rng_.below(static_cast<uint32_t>(m))];
    const ClassSpec& cs = in_.classes[v];
    if (cs.slots.empty()) continue;
    const Slot& s = cs.slots[rng_.below(static_cast<uint32_t>(cs.slots.size()))];
    const int r = cs.rooms.empty() ? -1 : cs.rooms[rng_.below(static_cast<uint32_t>(cs.rooms.size()))];
    slotsHeld.push_back(Placement{static_cast<int8_t>(s.day), static_cast<int16_t>(s.time), s.parity, r});
  }

  for (int v : victims_) {
    journal_.push_back({v, st_.placementOf(v)});
    st_.unplace(v);
  }
  st_.flush();

  const int q = static_cast<int>(slotsHeld.size());
  const int n = std::max(m, q);
  const double kInf = 1e18;
  std::vector<double> cmat(static_cast<size_t>(n) * n, 0.0);
  for (int r = 0; r < n; ++r)
    for (int col = 0; col < n; ++col) {
      if (r >= m || col >= q) {
        cmat[static_cast<size_t>(r) * n + col] = 0.0;  // dummy row or column
        continue;
      }
      const int c = victims_[r];
      const Placement& p = slotsHeld[col];
      const ClassSpec& cs = in_.classes[c];
      bool slotOk = false;
      for (const Slot& s : cs.slots)
        if (s.day == p.day && s.time == p.time && s.parity == p.parity) {
          slotOk = true;
          break;
        }
      bool roomOk = (p.room < 0) ? cs.rooms.empty()
                                 : std::find(cs.rooms.begin(), cs.rooms.end(), p.room) != cs.rooms.end();
      if (!slotOk || !roomOk || !st_.placementAllowed(c, p.day, p.time, p.parity, p.room)) {
        cmat[static_cast<size_t>(r) * n + col] = kInf;
        continue;
      }
      cmat[static_cast<size_t>(r) * n + col] = scoreCandidate(c, p.day, p.time, p.parity, p.room);
    }

  std::vector<int> assign;
  hungarian(cmat, n, assign);

  // Row r holds placement slotsHeld[r], so the assignment is the identity exactly when every row
  // is sent back to its own column.
  bool identity = true, feasible = true;
  for (int r = 0; r < m; ++r) {
    const int col = assign[r];
    if (col < 0 || col >= q || cmat[static_cast<size_t>(r) * n + col] >= kInf) {
      feasible = false;
      break;
    }
    if (col != r) identity = false;
  }
  if (!feasible) return false;
  // The relaxation very often returns everybody to where they were.  Putting them back is a
  // candidate that costs exactly zero and is therefore always accepted, which is a pure waste of
  // the budget: before this check the operator's acceptance rate was 99.96% and its improvement
  // rate 0.8%.
  if (identity && opt_.koptIdentityCheck) return false;

  for (int r = 0; r < m; ++r) st_.place(victims_[r], slotsHeld[assign[r]]);
  st_.flush();
  if (!st_.allLegal(victims_)) return false;
  refinePairs(victims_, 3);
  return true;
}

bool Worker::opDayFix() {
  // An exhaustive re-pack of one (entity, day): lift that day's classes out and try every
  // arrangement of them within the day.  The operator most directly matched to the residual.
  if (selectVictims(2, 6, victims_) < 2) return false;
  std::vector<Placement> held;
  for (int v : victims_) {
    const Placement& p = st_.placementOf(v);
    if (!p.placed()) return false;
    held.push_back(p);
    journal_.push_back({v, p});
  }
  const int m = static_cast<int>(victims_.size());
  std::vector<int> perm(m);
  for (int i = 0; i < m; ++i) perm[i] = i;
  double best = st_.surrogate();
  std::vector<int> bestPerm = perm;
  int tried = 0;
  do {
    if (++tried > 720) break;
    bool ok = true;
    for (int i = 0; i < m && ok; ++i) {
      const ClassSpec& cs = in_.classes[victims_[i]];
      const Placement& p = held[perm[i]];
      bool slotOk = false;
      for (const Slot& s : cs.slots)
        if (s.day == p.day && s.time == p.time && s.parity == p.parity) {
          slotOk = true;
          break;
        }
      bool roomOk = (p.room < 0) ? cs.rooms.empty()
                                 : std::find(cs.rooms.begin(), cs.rooms.end(), p.room) != cs.rooms.end();
      ok = slotOk && roomOk;
    }
    if (!ok) continue;
    for (int i = 0; i < m; ++i) st_.place(victims_[i], held[perm[i]]);
    st_.flush();
    if (st_.surrogate() < best - 1e-9) {
      best = st_.surrogate();
      bestPerm = perm;
    }
  } while (std::next_permutation(perm.begin(), perm.end()));
  for (int i = 0; i < m; ++i) st_.place(victims_[i], held[bestPerm[i]]);
  st_.flush();
  return st_.allLegal(victims_);
}

bool Worker::opWinFix() {
  // Read the idle bell starts off a cached occupancy mask and pull a class from the far side of one
  // into it: the only move family that closes a gap by construction rather than by luck.
  if (warm_.empty()) return false;
  const int seed = warm_[rng_.below(static_cast<uint32_t>(warm_.size()))];
  const Placement& p = st_.placementOf(seed);
  if (!p.placed()) return false;
  for (int32_t e : in_.classes[seed].staticEntities) {
    if (in_.isAbstractEntity(e)) continue;
    const BucketStat& s = st_.stat(e, p.day);
    for (int w = 1; w <= 2; ++w) {
      const Mask& occ = (w == 1) ? s.occNum : s.occDen;
      const Mask idle = occ.span().andNot(occ).operator&(in_.bells);
      if (idle.empty()) continue;
      const int tick = idle.lowest();
      const int minute = in_.ticks[tick];
      int targetTime = -1;
      for (size_t t = 0; t < in_.times.size(); ++t)
        if (in_.times[t].startMinute == minute) {
          targetTime = static_cast<int>(t);
          break;
        }
      if (targetTime < 0) continue;
      // Pull the last class of the day into the gap.
      int mover = -1, latest = -1;
      for (int32_t m : st_.members(e, p.day)) {
        if (!in_.classes[m].movable) continue;
        const Placement& pm = st_.placementOf(m);
        if (in_.times[pm.time].startMinute > latest) {
          latest = in_.times[pm.time].startMinute;
          mover = m;
        }
      }
      if (mover < 0) continue;
      const ClassSpec& cs = in_.classes[mover];
      for (const Slot& t : cs.slots) {
        if (t.day != p.day || t.time != targetTime) continue;
        const int room = st_.placementOf(mover).room;
        if (!st_.placementAllowed(mover, t.day, t.time, t.parity, room)) continue;
        doPlace(mover, Placement{static_cast<int8_t>(t.day), static_cast<int16_t>(t.time), t.parity, room});
        return true;
      }
    }
  }
  return false;
}

bool Worker::apply(int op) {
  switch (op) {
    case kMove: return opMove();
    case kSwap: return opSwap();
    case kChain: return opChain();
    case kKempe: return opKempe();
    case kRuin: return opRuin();
    case kRepack: return opRepack();
    case kKopt: return opKopt();
    case kDayFix: return opDayFix();
    case kWinFix: return opWinFix();
    default: return false;
  }
}

// ---- adaptive operator selection -----------------------------------------------------------------

int Worker::selectOperator() {
  if (opt_.credit == Credit::None || active_.size() == 1)
    return active_[rng_.below(static_cast<uint32_t>(active_.size()))];
  double total = 0;
  for (int op : active_) total += weight_[op];
  double r = rng_.unit() * total;
  for (int op : active_) {
    r -= weight_[op];
    if (r <= 0) return op;
  }
  return active_.back();
}

void Worker::endSegment() {
  for (int op : active_) {
    double norm = 1.0;
    switch (opt_.credit) {
      case Credit::Application: norm = std::max(1.0, segDraws_[op]); break;
      case Credit::Work: norm = std::max(1.0, segWork_[op]); break;
      case Credit::Time: norm = std::max(1.0, segTime_[op]); break;
      default: norm = 1.0;
    }
    const double rate = opt_.banditScale * segReward_[op] / norm;
    weight_[op] = opt_.banditDecay * weight_[op] + (1.0 - opt_.banditDecay) * (1.0 + rate);
    weight_[op] = std::min(opt_.weightCeiling, std::max(opt_.weightFloor, weight_[op]));
    segReward_[op] = segWork_[op] = segTime_[op] = segDraws_[op] = 0;
  }
  segApplied_ = 0;
}

// ---- escape ----------------------------------------------------------------------------------------

bool Worker::deepPhase() {
  ++deeps_;
  const double before = cost();
  for (int attempt = 0; attempt < 24; ++attempt) {
    begin();
    const double pre = st_.surrogate() + lambda_ * st_.hard();
    bool applied = false;
    const int which = attempt % 3;
    if (which == 0 && opt_.useKopt) applied = opKopt();
    else if (which == 1 && opt_.useLns) applied = opRuin();
    else if (opt_.useRepack) applied = opRepack();
    if (!applied) {
      rollback();
      continue;
    }
    st_.flush();
    const double post = st_.surrogate() + lambda_ * st_.hard();
    if (post >= pre - 1e-9) rollback();
  }
  st_.flush();
  const double after = cost();
  if (after < before - 1e-9) {
    koptWidth_ = std::max(4, koptWidth_ - 2);
    return true;
  }
  koptWidth_ = std::min(opt_.koptK, koptWidth_ + 4);
  return false;
}

void Worker::kickFromBest() {
  ++kicks_;
  if (opt_.kickFromBest && !incumbent_.empty()) st_.setAll(incumbent_);
  const int size = std::min(opt_.kickMax, std::max(opt_.kickMin, kickSize_));
  int broken = 0;
  for (int round = 0; round < 12 && broken < size; ++round) {
    const int sel = 1 + static_cast<int>(rng_.below(clusterMembers_ ? 4 : 3));
    journal_.clear();
    if (selectVictims(sel, std::min(size - broken, opt_.lnsMax), victims_) == 0) continue;
    for (int v : victims_) {
      journal_.push_back({v, st_.placementOf(v)});
      st_.unplace(v);
    }
    st_.flush();
    recreate(victims_);
    if (st_.allLegal(victims_)) broken += static_cast<int>(victims_.size());
    else rollback();
  }
  journal_.clear();
  st_.flush();
  if (opt_.kickHistoryPolicy == 0) refillHistory(cost());
  else if (opt_.kickHistoryPolicy == 2) refillHistory(lambda_ * incumbentKey_.hard + incumbentKey_.obj);
  temperature_ = opt_.saT0Factor * std::max(1.0, st_.surrogate()) * 0.5;
  sinceBest_ = 0;
  refreshLists();
}

void Worker::restartFresh() {
  ++fresh_;
  if (opt_.cooperate && !incumbent_.empty()) {
    std::lock_guard<std::mutex> lk(shared_.mu);
    if (incumbentKey_ < shared_.bestKey) {
      shared_.bestKey = incumbentKey_;
      shared_.best = incumbent_;
    }
    if (static_cast<int>(shared_.pool.size()) < opt_.poolSize) {
      shared_.pool.push_back(incumbent_);
      shared_.poolKey.push_back(incumbentKey_);
    }
  }
  incumbent_.clear();
  incumbentKey_ = Key{};
  construct();
  refillHistory(cost());
  temperature_ = opt_.saT0Factor * std::max(1.0, st_.surrogate());
  sinceBest_ = 0;
  movesSinceIncumbent_ = 0;
  blockAdoptUntil_ = candidates_ + opt_.restartAfter;
  refreshLists();
}

bool Worker::adoptElite() {
  if (candidates_ < blockAdoptUntil_) return false;
  std::vector<Placement> cand;
  {
    std::lock_guard<std::mutex> lk(shared_.mu);
    if (shared_.best.empty()) return false;
    if (!(shared_.bestKey < incumbentKey_)) return false;
    // Only adopt when the shared best is markedly better; otherwise the workers converge onto one
    // basin and the portfolio stops being a portfolio.
    if (incumbentKey_.obj < 1.03 * shared_.bestKey.obj) return false;
    cand = shared_.best;
  }
  st_.setAll(cand);
  refillHistory(cost());
  sinceAdopt_ = 0;
  ++adoptions_;
  refreshLists();
  return true;
}

bool Worker::noteIncumbent() {
  const Key k{st_.unplaced(), st_.hard(), st_.objective()};
  if (!(k < incumbentKey_)) return false;
  incumbentKey_ = k;
  incumbentSoft_ = st_.soft();
  incumbent_ = st_.placements();
  sinceBest_ = 0;
  movesSinceIncumbent_ = 0;
  kickSize_ = opt_.kickMin;
  if (opt_.cooperate) {
    std::lock_guard<std::mutex> lk(shared_.mu);
    if (k < shared_.bestKey) {
      shared_.bestKey = k;
      shared_.best = incumbent_;
    }
  }
  return true;
}

// ---- the loop -------------------------------------------------------------------------------------

void Worker::run(Clock::time_point deadline, SearchResult& out) {
  if (opt_.warmStart && opt_.warmStart->size() == in_.classes.size()) st_.setAll(*opt_.warmStart);
  else construct();
  constructedObjective_ = st_.objective();
  lambda_ = opt_.hardWeight > 0 ? opt_.hardWeight
                                : std::max(1e6, 0.02 * std::max(1.0, st_.surrogate()));
  refillHistory(cost());
  temperature_ = opt_.saT0Factor * std::max(1.0, st_.surrogate());
  refreshLists();

  incumbent_ = st_.placements();
  incumbentKey_ = Key{st_.unplaced(), st_.hard(), st_.objective()};
  incumbentSoft_ = st_.soft();
  kickSize_ = opt_.kickMin;

  const auto started = Clock::now();
  auto nextSample = started;
  int64_t nextSampleCandidates = opt_.sampleEveryCandidates;
  int64_t sinceReheat = 0;

  while (true) {
    if ((candidates_ & 1023) == 0) {
      if (Clock::now() >= deadline) break;
      if (shared_.stop.load(std::memory_order_relaxed)) break;
      if (opt_.sampleEveryCandidates > 0 && candidates_ >= nextSampleCandidates) {
        TrajectoryPoint tp;
        tp.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        tp.candidates = candidates_;
        tp.hard = incumbentKey_.hard;
        tp.soft = incumbentSoft_;
        tp.objective = incumbentKey_.obj;
        out.trajectory.push_back(tp);
        nextSampleCandidates += opt_.sampleEveryCandidates;
      }
      if (opt_.sampleEveryMs > 0 && Clock::now() >= nextSample) {
        TrajectoryPoint tp;
        tp.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        tp.candidates = candidates_;
        tp.hard = incumbentKey_.hard;
        tp.soft = incumbentSoft_;
        tp.objective = incumbentKey_.obj;
        out.trajectory.push_back(tp);
        nextSample += std::chrono::milliseconds(opt_.sampleEveryMs);
      }
    }
    if (opt_.candidateLimit > 0 && candidates_ >= opt_.candidateLimit) break;
    if (opt_.workLimit > 0 && st_.work() >= opt_.workLimit) break;
    if (st_.hard() <= 0 && st_.objective() <= 0 && st_.unplaced() == 0) break;

    const int op = selectOperator();
    ++stats_[op].draws;
    segDraws_[op] += 1;

    const int64_t workBefore = st_.work();
    const auto tickBefore = (opt_.credit == Credit::Time) ? Clock::now() : Clock::time_point{};
    const double before = cost();

    begin();
    const bool applied = apply(op);
    st_.flush();

    double reward = 0;
    if (!applied) {
      rollback();
    } else {
      ++candidates_;
      ++stats_[op].applied;
      ++segApplied_;
      const double after = cost();
      const bool uphill = after > before + 1e-9;
      int traceBucket = -1;
      if (opt_.acceptanceTraceBucket > 0) {
        traceBucket = static_cast<int>(std::min<int64_t>(63, levelAge_ / opt_.acceptanceTraceBucket));
        ++traceTotal_[traceBucket];
        if (uphill) ++traceUpOffered_[traceBucket];
      }
      if (acceptTest(after, before)) {
        ++stats_[op].accepted;
        journal_.clear();
        if (traceBucket >= 0 && uphill) ++traceUpAccepted_[traceBucket];
        if (after < before - 1e-9) {
          ++stats_[op].improved;
          stats_[op].gain += before - after;
          reward = 1.5;
        } else {
          reward = 0.3;
        }
        // The age of the cost *level*: the number of applied candidates since the working cost last
        // changed, in either direction.  That is the hypothesis of the bar-collapse theorem -- a
        // constant cost -- and not the same as the time since the last improvement, which is what an
        // earlier version of this instrumentation recorded.
        if (std::fabs(after - before) > 1e-9) levelAge_ = 0;
        else ++levelAge_;
        const Key k{st_.unplaced(), st_.hard(), st_.objective()};
        if (k < incumbentKey_) {
          incumbentKey_ = k;
          incumbentSoft_ = st_.soft();
          incumbent_ = st_.placements();
          sinceBest_ = 0;
          movesSinceIncumbent_ = 0;
          reward = 4.0;
          kickSize_ = opt_.kickMin;
          if (opt_.cooperate) {
            std::lock_guard<std::mutex> lk(shared_.mu);
            if (k < shared_.bestKey) {
              shared_.bestKey = k;
              shared_.best = incumbent_;
            }
          }
        }
      } else {
        rollback();
        ++levelAge_;   // a refused candidate leaves the cost where it was
      }
    }

    const int64_t workDone = st_.work() - workBefore;
    stats_[op].work += workDone;
    segWork_[op] += static_cast<double>(workDone);
    segReward_[op] += reward;
    if (opt_.credit == Credit::Time)
      segTime_[op] += std::chrono::duration<double, std::micro>(Clock::now() - tickBefore).count();

    ++sinceBest_;
    ++sinceDeep_;
    ++movesSinceIncumbent_;
    ++sinceAdopt_;
    ++sinceReheat;

    if (segApplied_ >= opt_.banditSegment) endSegment();
    if (candidates_ % opt_.hotRefresh == 0) refreshLists();

    if (opt_.engine == "sa") {
      if ((candidates_ & 4095) == 0) temperature_ *= opt_.saCooling;
      const double floorT = 1e-4 * opt_.saT0Factor * std::max(1.0, constructedObjective_);
      if (temperature_ < floorT) temperature_ = floorT;
      if (sinceReheat > opt_.saReheatAfter) {
        temperature_ = 0.5 * opt_.saT0Factor * std::max(1.0, constructedObjective_);
        sinceReheat = 0;
      }
    }

    // ---- the three-level escape -------------------------------------------------------------
    if (sinceBest_ >= opt_.deepEvery && sinceDeep_ >= opt_.deepEvery) {
      sinceDeep_ = 0;
      if (opt_.useDeep && deepPhase()) {
        noteIncumbent();
        continue;
      }
      if (sinceBest_ < opt_.stagnationMoves) continue;
      if (opt_.useFresh && movesSinceIncumbent_ >= opt_.restartAfter) {
        restartFresh();
        noteIncumbent();
        continue;
      }
      if (opt_.cooperate && sinceAdopt_ > opt_.stagnationMoves && adoptElite()) {
        noteIncumbent();
        continue;
      }
      if (opt_.useKick) {
        kickFromBest();
        noteIncumbent();
        if (opt_.adaptiveKick) kickSize_ = std::min(opt_.kickMax, kickSize_ * 3 / 2 + opt_.kickMin);
      }
    }
  }

  out.candidates += candidates_;
  out.workUnits += st_.work();
  out.constructedObjective = constructedObjective_;
  out.traceTotal = traceTotal_;
  out.traceUphillOffered = traceUpOffered_;
  out.traceUphillAccepted = traceUpAccepted_;
  out.kicks += kicks_;
  out.freshRestarts += fresh_;
  out.deepPhases += deeps_;
  out.adoptions += adoptions_;
  if (opt_.collectOperatorStats) {
    if (out.ops.empty()) out.ops.assign(kNumOperators, OperatorStat{});
    for (int i = 0; i < kNumOperators; ++i) {
      out.ops[i].draws += stats_[i].draws;
      out.ops[i].applied += stats_[i].applied;
      out.ops[i].accepted += stats_[i].accepted;
      out.ops[i].improved += stats_[i].improved;
      out.ops[i].work += stats_[i].work;
      out.ops[i].gain += stats_[i].gain;
      out.ops[i].finalWeight = weight_[i];
    }
  }

  std::lock_guard<std::mutex> lk(shared_.mu);
  if (!incumbent_.empty() && incumbentKey_ < shared_.bestKey) {
    shared_.bestKey = incumbentKey_;
    shared_.best = incumbent_;
  }
}

}  // namespace

// =================================================================================================

namespace {
int64_t gBudgetCap = 0;
}  // namespace

void setBudgetCap(int64_t cap) { gBudgetCap = cap > 0 ? cap : 0; }
int64_t budgetCap() { return gBudgetCap; }

SearchResult solve(const Instance& inst, const SearchOptions& given, const double* beta) {
  // The ceiling is applied here rather than at each call site, because an experiment that fixes its
  // own budget is exactly the one a smaller --budget cannot shorten.  With no ceiling set this is
  // the identity, which is the state every reported number was produced in.
  SearchOptions capped = given;
  if (gBudgetCap > 0 && (capped.candidateLimit <= 0 || capped.candidateLimit > gBudgetCap))
    capped.candidateLimit = gBudgetCap;
  const SearchOptions& opt = capped;

  SearchResult out;
  Shared shared;
  const auto started = Clock::now();
  const auto deadline = started + std::chrono::milliseconds(opt.timeLimitMs);

  std::vector<int32_t> clusters;
  std::vector<std::vector<int32_t>> clusterMembers;
  if (opt.useClusters) {
    clusters = buildClusters(inst, opt.seed ^ 0x5DEECE66DULL);
    int maxLabel = 0;
    for (int32_t l : clusters) maxLabel = std::max(maxLabel, static_cast<int>(l));
    clusterMembers.assign(maxLabel + 1, {});
    for (int c = 0; c < inst.nClasses(); ++c)
      if (inst.classes[c].movable) clusterMembers[clusters[c]].push_back(c);
    clusterMembers.erase(std::remove_if(clusterMembers.begin(), clusterMembers.end(),
                                        [](const std::vector<int32_t>& v) { return v.empty(); }),
                         clusterMembers.end());
  }

  const int nThreads = std::max(1, opt.threads);
  std::vector<SearchResult> partial(nThreads);
  std::vector<std::unique_ptr<Worker>> workers;
  for (int i = 0; i < nThreads; ++i) {
    workers.push_back(std::make_unique<Worker>(inst, opt, beta, opt.seed + 7919ULL * i, shared));
    if (opt.useClusters && !clusterMembers.empty())
      workers.back()->setClusters(&clusters, &clusterMembers);
  }

  if (nThreads == 1) {
    workers[0]->run(deadline, partial[0]);
  } else {
    std::vector<std::thread> th;
    for (int i = 0; i < nThreads; ++i)
      th.emplace_back([&, i] { workers[i]->run(deadline, partial[i]); });
    for (auto& t : th) t.join();
  }

  out.ops.assign(kNumOperators, OperatorStat{});
  for (SearchResult& p : partial) {
    out.candidates += p.candidates;
    out.workUnits += p.workUnits;
    out.kicks += p.kicks;
    out.freshRestarts += p.freshRestarts;
    out.deepPhases += p.deepPhases;
    out.adoptions += p.adoptions;
    for (int i = 0; i < kNumOperators && i < static_cast<int>(p.ops.size()); ++i) {
      out.ops[i].draws += p.ops[i].draws;
      out.ops[i].applied += p.ops[i].applied;
      out.ops[i].accepted += p.ops[i].accepted;
      out.ops[i].improved += p.ops[i].improved;
      out.ops[i].work += p.ops[i].work;
      out.ops[i].gain += p.ops[i].gain;
      out.ops[i].finalWeight = std::max(out.ops[i].finalWeight, p.ops[i].finalWeight);
    }
    if (out.trajectory.empty()) out.trajectory = p.trajectory;
    if (!p.traceTotal.empty()) {
      if (out.traceTotal.empty()) {
        out.traceTotal = p.traceTotal;
        out.traceUphillOffered = p.traceUphillOffered;
        out.traceUphillAccepted = p.traceUphillAccepted;
      } else {
        for (size_t i = 0; i < out.traceTotal.size(); ++i) {
          out.traceTotal[i] += p.traceTotal[i];
          out.traceUphillOffered[i] += p.traceUphillOffered[i];
          out.traceUphillAccepted[i] += p.traceUphillAccepted[i];
        }
      }
    }
  }
  out.constructedObjective = partial[0].constructedObjective;

  out.best = shared.best;
  if (out.best.empty()) {
    // No worker published -- only possible when the deadline had already passed on entry.  Return a
    // schedule that at least respects the immovable classes, rather than an empty one, so that the
    // caller never has to special-case it.
    State fallback(inst, beta);
    out.best = fallback.placements();
  }
  State check(inst, beta);
  check.setAll(out.best);
  out.hard = check.hard();
  out.soft = check.soft();
  out.objective = check.objective();
  out.unplaced = check.unplaced();
  out.elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
  return out;
}

}  // namespace tt
