#include "state.hpp"

#include <algorithm>
#include <cmath>

namespace tt {

State::State(const Instance& inst, const double* beta) : inst_(inst) {
  for (int i = 0; i < kNumTerms; ++i) beta_[i] = beta[i];
  placement_.assign(inst.classes.size(), Placement{});
  durIdx_.resize(inst.classes.size());
  building_.assign(inst.classes.size(), -2);
  for (size_t i = 0; i < inst.classes.size(); ++i)
    durIdx_[i] = inst.durationIndex(inst.classes[i].duration);
  stride_ = inst.nDays + 1;
  const size_t nb = static_cast<size_t>(inst.nEntities()) * stride_;
  members_.assign(nb, {});
  stats_.assign(nb, BucketStat{});
  dirtyFlag_.assign(nb, 0);
  unplaced_ = static_cast<int>(inst.movable.size());
  // Immovable classes are obstacles: they are in the buckets from the start and never leave.
  for (size_t i = 0; i < inst.classes.size(); ++i) {
    if (!inst.classes[i].movable && inst.classes[i].fixed.placed())
      place(static_cast<int>(i), inst.classes[i].fixed);
  }
  flush();
  unplaced_ = static_cast<int>(inst.movable.size());
}

void State::entitiesOf(int c, const Placement& p, std::vector<int32_t>& out) const {
  const ClassSpec& cs = inst_.classes[c];
  out = cs.staticEntities;
  if (p.room >= 0) out.push_back(inst_.roomId(p.room));
}

void State::markDirty(int bucket) {
  if (!dirtyFlag_[bucket]) {
    dirtyFlag_[bucket] = 1;
    dirtyList_.push_back(bucket);
  }
}

void State::place(int c, const Placement& p) {
  const Placement old = placement_[c];
  if (old.placed()) {
    entitiesOf(c, old, scratch_);
    for (int32_t e : scratch_) {
      const int b = bucketIndex(e, old.day);
      auto& m = members_[b];
      m.erase(std::find(m.begin(), m.end(), c));
      markDirty(b);
    }
  } else if (inst_.classes[c].movable) {
    --unplaced_;
  }
  placement_[c] = p;
  building_[c] = buildingOf(c, p);
  if (p.placed()) {
    entitiesOf(c, p, scratch_);
    for (int32_t e : scratch_) {
      const int b = bucketIndex(e, p.day);
      members_[b].push_back(c);
      markDirty(b);
    }
  } else if (inst_.classes[c].movable) {
    ++unplaced_;
  }
  // A non-separable term family, when one is attached, sees the same move the buckets do.  The
  // call is exactly invertible, which is what lets probe() apply a candidate and undo it.
  if (extra_) extra_->move(c, old, p);
}

void State::unplace(int c) { place(c, Placement{}); }

void State::clear() {
  for (int32_t c : inst_.movable) unplace(c);
  flush();
}

void State::setAll(const std::vector<Placement>& p) {
  for (int32_t c : inst_.movable) place(c, p[c]);
  flush();
}

void State::addStat(int entity, const BucketStat& s, double sign) {
  if (inst_.isAbstractEntity(entity)) {
    pi_[kAbstractOverflow] += sign * s.overflow;
    return;
  }
  if (inst_.isRoomEntity(entity)) {
    pi_[kRoomConflict] += sign * s.conflicts;
    return;
  }
  if (entity < inst_.nLecturers) {
    pi_[kLecturerConflict] += sign * s.conflicts;
    pi_[kLecturerTravel] += sign * s.travel;
    pi_[kLecturerWindow] += sign * 0.5 * (s.winNum + s.winDen);
  } else {
    pi_[kGroupConflict] += sign * s.conflicts;
    pi_[kGroupTravel] += sign * s.travel;
    pi_[kGroupWindow] += sign * 0.5 * (s.winNum + s.winDen);
    pi_[kMixedOnlineDay] += sign * 0.5 * (s.mixNum + s.mixDen);
  }
}

int State::buildingOf(int c, const Placement& p) const {
  const ClassSpec& cs = inst_.classes[c];
  switch (cs.kind) {
    case PlaceKind::Online:
      return -1;  // nowhere, but known: an online class is at home
    case PlaceKind::AbstractNowhere:
      return -2;  // unknown address
    case PlaceKind::AbstractHere: {
      const int a = inst_.abstractOfEntity(cs.abstractRoom);
      return (a >= 0 && a < inst_.nAbstract) ? inst_.abstractBuilding[a] : -2;
    }
    case PlaceKind::Room:
    default:
      return (p.room >= 0) ? inst_.roomBuilding[p.room] : -2;
  }
}

int State::journey(int ca, const Placement& pa, int cb, const Placement& pb) const {
  const ClassSpec& a = inst_.classes[ca];
  const ClassSpec& b = inst_.classes[cb];
  const bool aOnline = a.kind == PlaceKind::Online;
  const bool bOnline = b.kind == PlaceKind::Online;
  if (aOnline && bOnline) return 0;
  if (aOnline != bOnline) return inst_.commute;
  if (a.abstractRoom >= 0 && a.abstractRoom == b.abstractRoom) return 0;
  if (a.kind == PlaceKind::AbstractNowhere || b.kind == PlaceKind::AbstractNowhere)
    return inst_.abstractTravel;
  const int ba = (placement_[ca] == pa) ? building_[ca] : buildingOf(ca, pa);
  const int bb = (placement_[cb] == pb) ? building_[cb] : buildingOf(cb, pb);
  if (ba < 0 || bb < 0 || ba == bb) return 0;
  return inst_.travel[static_cast<size_t>(ba) * inst_.nBuildings + bb];
}

void State::recompute(int bucket) {
  ++work_;
  const int entity = bucket / stride_;
  BucketStat s;
  const std::vector<int32_t>& m = members_[bucket];
  s.size = static_cast<int32_t>(m.size());
  if (m.empty()) {
    stats_[bucket] = s;
    return;
  }

  const bool abstractEntity = inst_.isAbstractEntity(entity);
  const bool roomEntity = inst_.isRoomEntity(entity);
  const bool groupEntity = !abstractEntity && !roomEntity && entity >= inst_.nLecturers;
  const bool personEntity = !abstractEntity && !roomEntity;

  // ---- occupancy masks, per calendar week ----------------------------------------------------
  for (int32_t c : m) {
    const Placement& p = placement_[c];
    const Mask& mu = inst_.maskOf(p.time, durIdx_[c]);
    if (inWeek(p.parity, 1)) s.occNum |= mu;
    if (inWeek(p.parity, 2)) s.occDen |= mu;
  }

  // ---- conflicts: clashing pairs -------------------------------------------------------------
  if (!abstractEntity && m.size() > 1) {
    for (size_t i = 0; i < m.size(); ++i) {
      const Placement& pi = placement_[m[i]];
      const Mask& mi = inst_.maskOf(pi.time, durIdx_[m[i]]);
      for (size_t j = i + 1; j < m.size(); ++j) {
        const Placement& pj = placement_[m[j]];
        if (!weeksOverlap(pi.parity, pj.parity)) continue;
        if (mi.intersects(inst_.maskOf(pj.time, durIdx_[m[j]]))) ++s.conflicts;
      }
    }
  }

  // ---- travel: ordered non-overlapping pairs with too short a gap ----------------------------
  if (personEntity && m.size() > 1) {
    for (size_t i = 0; i < m.size(); ++i) {
      for (size_t j = 0; j < m.size(); ++j) {
        if (i == j) continue;
        const int ca = m[i], cb = m[j];
        const Placement& pa = placement_[ca];
        const Placement& pb = placement_[cb];
        if (!weeksOverlap(pa.parity, pb.parity)) continue;
        const int sa = inst_.times[pa.time].startMinute;
        const int sb = inst_.times[pb.time].startMinute;
        const int ea = sa + inst_.classes[ca].duration;
        if (sa > sb) continue;                   // consider each pair once, earlier first
        if (sa == sb) continue;                  // simultaneous: a clash, not a journey
        if (ea > sb) continue;                   // overlapping: a clash, not a journey
        const int gap = sb - ea;
        if (gap < journey(ca, pa, cb, pb)) ++s.travel;
      }
    }
  }

  // ---- idle periods, per calendar week -------------------------------------------------------
  if (personEntity) {
    s.winNum = static_cast<int32_t>(s.occNum.span().andNot(s.occNum).operator&(inst_.bells).count());
    s.winDen = static_cast<int32_t>(s.occDen.span().andNot(s.occDen).operator&(inst_.bells).count());
  }

  // ---- mixed online / on-site day, per calendar week -----------------------------------------
  if (groupEntity) {
    bool onlineNum = false, siteNum = false, onlineDen = false, siteDen = false;
    for (int32_t c : m) {
      const Placement& p = placement_[c];
      const bool online = inst_.classes[c].kind == PlaceKind::Online;
      if (inWeek(p.parity, 1)) (online ? onlineNum : siteNum) = true;
      if (inWeek(p.parity, 2)) (online ? onlineDen : siteDen) = true;
    }
    s.mixNum = (onlineNum && siteNum) ? 1 : 0;
    s.mixDen = (onlineDen && siteDen) ? 1 : 0;
  }

  // ---- abstract-room overflow ----------------------------------------------------------------
  // The summed headcount of a set of intervals peaks at one of their starts, so testing every
  // distinct start instant finds every breach.  The breach is charged once per instant.
  if (abstractEntity) {
    const int a = inst_.abstractOfEntity(entity);
    const int cap = inst_.abstractCapacity[a];
    if (cap >= 0) {
      for (int w = 1; w <= 2; ++w) {
        for (size_t i = 0; i < m.size(); ++i) {
          const Placement& pi = placement_[m[i]];
          if (!inWeek(pi.parity, w)) continue;
          const int instant = inst_.times[pi.time].startMinute;
          // Skip duplicates: charge each distinct start instant once.
          bool seen = false;
          for (size_t k = 0; k < i; ++k) {
            const Placement& pk = placement_[m[k]];
            if (inWeek(pk.parity, w) && inst_.times[pk.time].startMinute == instant) {
              seen = true;
              break;
            }
          }
          if (seen) continue;
          int total = 0;
          for (int32_t c : m) {
            const Placement& p = placement_[c];
            if (!inWeek(p.parity, w)) continue;
            const int st = inst_.times[p.time].startMinute;
            const int en = st + inst_.classes[c].duration;
            if (st <= instant && instant < en) total += inst_.classes[c].students;
          }
          if (total > cap) ++s.overflow;
        }
      }
    }
  }

  stats_[bucket] = s;
}

void State::flush() {
  for (int32_t b : dirtyList_) {
    const int entity = b / stride_;
    addStat(entity, stats_[b], -1.0);
    recompute(b);
    addStat(entity, stats_[b], +1.0);
    dirtyFlag_[b] = 0;
  }
  dirtyList_.clear();
  // Guard against accumulated floating point drift on the half-unit terms.
  for (int i = 0; i < kNumTerms; ++i)
    if (pi_[i] > -1e-9 && pi_[i] < 1e-9) pi_[i] = 0.0;
}

double State::hard() const {
  double h = 0;
  for (int i = 0; i <= kAbstractOverflow; ++i) h += pi_[i];
  return h;
}

double State::soft() const {
  return std::llround(pi_[kLecturerWindow]) + std::llround(pi_[kGroupWindow]) +
         std::llround(pi_[kMixedOnlineDay]) + (extra_ ? extra_->soft() : 0.0);
}

double State::objective() const {
  double f = 0;
  for (int i = 0; i < kNumTerms; ++i) {
    const double v = (i >= kLecturerWindow) ? static_cast<double>(std::llround(pi_[i])) : pi_[i];
    f += beta_[i] * v * v;
  }
  return f + (extra_ ? extra_->value() : 0.0);
}

double State::surrogate() const {
  double f = 0;
  for (int i = 0; i < kNumTerms; ++i) f += beta_[i] * pi_[i] * pi_[i];
  return f + (extra_ ? extra_->value() : 0.0);
}

bool State::placementAllowed(int c, int day, int time, Parity parity, int room) const {
  (void)parity;
  if (room >= 0 && !inst_.roomFree(room, day, time)) return false;
  const ClassSpec& cs = inst_.classes[c];
  auto capOk = [&](int entity) {
    const int cap = inst_.maxPerDay[entity];
    if (cap >= (1 << 20)) return true;
    const std::vector<int32_t>& m = members_[bucketIndex(entity, day)];
    int n = static_cast<int>(m.size());
    for (int32_t x : m)
      if (x == c) --n;  // the class may already be counted on this day
    return n + 1 <= cap;
  };
  for (int32_t e : cs.staticEntities)
    if (!inst_.isAbstractEntity(e) && !capOk(e)) return false;
  if (room >= 0 && !capOk(inst_.roomId(room))) return false;
  return true;
}

bool State::allLegal(const std::vector<int>& cs) const {
  for (int c : cs) {
    const Placement& p = placement_[c];
    if (!p.placed()) continue;
    if (p.room >= 0 && !inst_.roomFree(p.room, p.day, p.time)) return false;
  }
  // The load cap, re-tested against the final state.
  std::vector<int32_t> ents;
  for (int c : cs) {
    const Placement& p = placement_[c];
    if (!p.placed()) continue;
    entitiesOf(c, p, ents);
    for (int32_t e : ents) {
      if (inst_.isAbstractEntity(e)) continue;
      const int cap = inst_.maxPerDay[e];
      if (cap >= (1 << 20)) continue;
      if (static_cast<int>(members_[bucketIndex(e, p.day)].size()) > cap) return false;
    }
  }
  return true;
}

double State::probe(int c, const Placement& p) {
  const Placement old = placement_[c];
  const double before = surrogate();
  place(c, p);
  flush();
  const double after = surrogate();
  place(c, old);
  flush();
  return after - before;
}

}  // namespace tt
