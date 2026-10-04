#include "validate.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace tt {

double Score::soft() const {
  return std::llround(pi[6]) + std::llround(pi[7]) + std::llround(pi[8]);
}

double Score::objective(const double* beta) const {
  double f = 0;
  for (int i = 0; i < 9; ++i) {
    const double v = (i >= 6) ? static_cast<double>(std::llround(pi[i])) : pi[i];
    f += beta[i] * v * v;
  }
  return f;
}

namespace {

struct Item {
  int cls;
  int start, end;   // minutes
  Parity parity;
};

bool sharesWeek(Parity a, Parity b) {
  return a == Parity::Weekly || b == Parity::Weekly || a == b;
}

bool taught(Parity p, int w) {
  return p == Parity::Weekly || (p == Parity::Numerator && w == 1) ||
         (p == Parity::Denominator && w == 2);
}

int journeyMinutes(const Instance& in, int ca, const Placement& pa, int cb, const Placement& pb) {
  const ClassSpec& a = in.classes[ca];
  const ClassSpec& b = in.classes[cb];
  const bool ao = a.kind == PlaceKind::Online, bo = b.kind == PlaceKind::Online;
  if (ao && bo) return 0;
  if (ao != bo) return in.commute;
  if (a.abstractRoom >= 0 && a.abstractRoom == b.abstractRoom) return 0;
  if (a.kind == PlaceKind::AbstractNowhere || b.kind == PlaceKind::AbstractNowhere)
    return in.abstractTravel;
  auto building = [&](int c, const Placement& p) -> int {
    const ClassSpec& cs = in.classes[c];
    if (cs.kind == PlaceKind::AbstractHere) {
      const int idx = cs.abstractRoom - (in.nLecturers + in.nGroups + in.nRooms);
      return (idx >= 0 && idx < in.nAbstract) ? in.abstractBuilding[idx] : -2;
    }
    return p.room >= 0 ? in.roomBuilding[p.room] : -2;
  };
  const int ba = building(ca, pa), bb = building(cb, pb);
  if (ba < 0 || bb < 0 || ba == bb) return 0;
  return in.travel[static_cast<size_t>(ba) * in.nBuildings + bb];
}

}  // namespace

Score validate(const Instance& in, const std::vector<Placement>& sigma) {
  Score sc;

  // ---- the hard filters, checked directly against the declared domains -----------------------
  for (int c = 0; c < in.nClasses(); ++c) {
    const ClassSpec& cs = in.classes[c];
    const Placement& p = sigma[c];
    if (!cs.movable) {
      if (!(p == cs.fixed)) ++sc.immovableMoved;
      continue;
    }
    if (!p.placed()) {
      ++sc.unplaced;
      continue;
    }
    bool slotOk = false;
    for (const Slot& s : cs.slots)
      if (s.day == p.day && s.time == p.time && s.parity == p.parity) {
        slotOk = true;
        break;
      }
    bool roomOk = cs.rooms.empty() ? (p.room < 0)
                                   : std::find(cs.rooms.begin(), cs.rooms.end(), p.room) !=
                                         cs.rooms.end();
    if (!slotOk || !roomOk) ++sc.domainBreaches;
    if (p.room >= 0 && !in.roomFree(p.room, p.day, p.time)) ++sc.availabilityBreaches;
  }

  // ---- group the placed classes by (entity, day) ---------------------------------------------
  std::map<std::pair<int, int>, std::vector<Item>> bucket;
  for (int c = 0; c < in.nClasses(); ++c) {
    const Placement& p = sigma[c];
    if (!p.placed()) continue;
    const ClassSpec& cs = in.classes[c];
    Item it{c, in.times[p.time].startMinute, in.times[p.time].startMinute + cs.duration, p.parity};
    for (int32_t e : cs.lecturers) bucket[{e, p.day}].push_back(it);
    for (int32_t e : cs.groups) bucket[{e, p.day}].push_back(it);
    if (cs.abstractRoom >= 0) bucket[{cs.abstractRoom, p.day}].push_back(it);
    if (p.room >= 0) bucket[{in.roomId(p.room), p.day}].push_back(it);
  }

  // ---- the per-(entity, day) load cap --------------------------------------------------------
  for (const auto& kv : bucket) {
    const int e = kv.first.first;
    if (e >= in.nLecturers + in.nGroups + in.nRooms) continue;  // abstract rooms are uncapped
    const int cap = in.maxPerDay[e];
    if (cap < (1 << 20) && static_cast<int>(kv.second.size()) > cap) ++sc.capBreaches;
  }

  // ---- the nine terms --------------------------------------------------------------------------
  for (const auto& kv : bucket) {
    const int e = kv.first.first;
    std::vector<Item> v = kv.second;
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) {
      return a.start != b.start ? a.start < b.start : a.cls < b.cls;
    });

    const bool isRoom = e >= in.nLecturers + in.nGroups &&
                        e < in.nLecturers + in.nGroups + in.nRooms;
    const bool isAbstract = e >= in.nLecturers + in.nGroups + in.nRooms;
    const bool isGroup = !isRoom && !isAbstract && e >= in.nLecturers;
    const bool isPerson = !isRoom && !isAbstract;

    if (!isAbstract) {
      for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j) {
          if (!sharesWeek(v[i].parity, v[j].parity)) continue;
          if (v[i].start < v[j].end && v[j].start < v[i].end) {
            if (isRoom) sc.pi[2] += 1;
            else if (isGroup) sc.pi[1] += 1;
            else sc.pi[0] += 1;
          }
        }
    }

    if (isPerson) {
      for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j) {
          if (!sharesWeek(v[i].parity, v[j].parity)) continue;
          if (v[i].start == v[j].start) continue;
          if (v[i].end > v[j].start) continue;  // overlapping
          const int gap = v[j].start - v[i].end;
          if (gap < journeyMinutes(in, v[i].cls, sigma[v[i].cls], v[j].cls, sigma[v[j].cls]))
            sc.pi[isGroup ? 3 : 4] += 1;
        }

      for (int w = 1; w <= 2; ++w) {
        std::vector<std::pair<int, int>> iv;
        for (const Item& it : v)
          if (taught(it.parity, w)) iv.push_back({it.start, it.end});
        if (iv.size() < 2) continue;
        int lo = iv[0].first, hi = iv[0].second;
        for (const auto& x : iv) {
          lo = std::min(lo, x.first);
          hi = std::max(hi, x.second);
        }
        int idle = 0;
        for (const TimeSpec& t : in.times) {
          if (t.startMinute <= lo || t.startMinute >= hi) continue;
          bool occupied = false;
          for (const auto& x : iv)
            if (x.first <= t.startMinute && t.startMinute < x.second) {
              occupied = true;
              break;
            }
          if (!occupied) ++idle;
        }
        sc.pi[isGroup ? 7 : 6] += 0.5 * idle;
      }
    }

    if (isGroup) {
      for (int w = 1; w <= 2; ++w) {
        bool online = false, onsite = false;
        for (const Item& it : v) {
          if (!taught(it.parity, w)) continue;
          if (in.classes[it.cls].kind == PlaceKind::Online) online = true;
          else onsite = true;
        }
        if (online && onsite) sc.pi[8] += 0.5;
      }
    }

    if (isAbstract) {
      const int idx = e - (in.nLecturers + in.nGroups + in.nRooms);
      const int cap = in.abstractCapacity[idx];
      if (cap < 0) continue;
      for (int w = 1; w <= 2; ++w) {
        std::set<int> instants;
        for (const Item& it : v)
          if (taught(it.parity, w)) instants.insert(it.start);
        for (int s : instants) {
          int total = 0;
          for (const Item& it : v)
            if (taught(it.parity, w) && it.start <= s && s < it.end)
              total += in.classes[it.cls].students;
          if (total > cap) sc.pi[5] += 1;
        }
      }
    }
  }

  return sc;
}

}  // namespace tt
