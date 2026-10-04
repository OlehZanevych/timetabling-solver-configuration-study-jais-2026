#include "generate.hpp"

#include <algorithm>
#include <cmath>

#include "rng.hpp"

namespace tt {
namespace {

struct Planner {
  const GenOptions& opt;
  Rng rng;
  Instance in;
  std::vector<Placement> planted;

  /// One occupied interval of one entity on one day of one calendar week.
  struct Busy {
    int start, end;   // minutes
    int cls;
  };
  // occ[(entity * (nDays+1) + day) * 2 + (week-1)]
  std::vector<std::vector<Busy>> occ;

  std::vector<int32_t> groupLoad, lecturerLoad;
  std::vector<std::vector<int32_t>> departmentOf;
  std::vector<int32_t> timeOrder;   ///< time indices sorted by start minute

  int nOrd = 0, nSport = 0, nTimes = 0;

  explicit Planner(const GenOptions& o) : opt(o), rng(o.seed * 0x2545F4914F6CDD1DULL + 17) {}

  size_t occIndex(int entity, int day, int week) const {
    return (static_cast<size_t>(entity) * (opt.nDays + 1) + day) * 2 + (week - 1);
  }

  /// Is the entity already occupied at any instant of [start, end) in a week the parity touches?
  bool isBusy(int entity, int day, Parity p, int start, int end) const {
    for (int w = 1; w <= 2; ++w) {
      if (!inWeek(p, w)) continue;
      for (const Busy& b : occ[occIndex(entity, day, w)])
        if (b.start < end && start < b.end) return true;
    }
    return false;
  }

  void markBusy(int entity, int day, Parity p, int start, int end, int cls) {
    for (int w = 1; w <= 2; ++w)
      if (inWeek(p, w)) occ[occIndex(entity, day, w)].push_back({start, end, cls});
  }

  /// Can this entity reach the candidate class from -- and get from it to -- every class it already
  /// has that day?  The walk no longer visits times in ascending order (two bell grids interleave),
  /// so both directions have to be checked.
  bool travelOk(int entity, int day, Parity p, int start, int end, int building, PlaceKind kind,
                int abstractE) const {
    const bool online = kind == PlaceKind::Online;
    auto journeyTo = [&](int otherCls, bool otherFirst) {
      const ClassSpec& cs = in.classes[otherCls];
      const Placement& q = planted[otherCls];
      const bool oOnline = cs.kind == PlaceKind::Online;
      if (oOnline && online) return 0;
      if (oOnline != online) return in.commute;
      if (cs.abstractRoom >= 0 && cs.abstractRoom == abstractE) return 0;
      if (cs.kind == PlaceKind::AbstractNowhere || kind == PlaceKind::AbstractNowhere)
        return in.abstractTravel;
      int b0 = -2;
      if (cs.kind == PlaceKind::AbstractHere)
        b0 = in.abstractBuilding[cs.abstractRoom - (in.nLecturers + in.nGroups + in.nRooms)];
      else if (q.room >= 0)
        b0 = in.roomBuilding[q.room];
      if (b0 < 0 || building < 0 || b0 == building) return 0;
      return otherFirst ? in.travel[static_cast<size_t>(b0) * in.nBuildings + building]
                        : in.travel[static_cast<size_t>(building) * in.nBuildings + b0];
    };
    for (int w = 1; w <= 2; ++w) {
      if (!inWeek(p, w)) continue;
      for (const Busy& b : occ[occIndex(entity, day, w)]) {
        if (b.end <= start) {
          if (start - b.end < journeyTo(b.cls, true)) return false;
        } else if (end <= b.start) {
          if (b.start - end < journeyTo(b.cls, false)) return false;
        } else {
          return false;  // overlapping
        }
      }
    }
    return true;
  }

  /// Would adding `headcount` at [start, end) push the abstract room over its capacity?
  bool abstractFits(int entity, int day, Parity p, int start, int end, int headcount) const {
    const int a = entity - (in.nLecturers + in.nGroups + in.nRooms);
    const int cap = in.abstractCapacity[a];
    if (cap < 0) return true;
    for (int w = 1; w <= 2; ++w) {
      if (!inWeek(p, w)) continue;
      // The summed headcount peaks at one of the interval starts.
      std::vector<int> instants{start};
      for (const Busy& b : occ[occIndex(entity, day, w)])
        if (b.start < end && start < b.end) instants.push_back(b.start);
      for (int s : instants) {
        if (s < start || s >= end) continue;
        int total = headcount;
        for (const Busy& b : occ[occIndex(entity, day, w)])
          if (b.start <= s && s < b.end) total += in.classes[b.cls].students;
        if (total > cap) return false;
      }
    }
    return true;
  }

  void build();
  void plant();
  void deriveDomains();
};

void Planner::build() {
  // ---- sizes ---------------------------------------------------------------------------------
  const int n = opt.nClasses;
  in.nDays = opt.nDays;
  in.nBuildings = std::max(1, opt.nBuildings);
  in.nGroups = std::max(2, static_cast<int>(std::lround(double(n) / opt.classesPerGroup)));
  in.nLecturers = std::max(2, static_cast<int>(std::lround(double(n) / opt.classesPerLecturer)));

  nOrd = opt.nOrdinaryTimes;
  nSport = opt.nSportTimes;
  nTimes = nOrd + nSport;
  in.times.clear();
  for (int i = 0; i < nOrd; ++i) in.times.push_back({510 + 100 * i, 0});   // 08:30, every 100 min
  for (int i = 0; i < nSport; ++i) in.times.push_back({540 + 180 * i, 1}); // 09:00, every 180 min

  timeOrder.clear();
  for (int i = 0; i < nTimes; ++i) timeOrder.push_back(i);
  // Ties are broken by index.  With the default bell grids no two slots share a start minute, so
  // this changes nothing for the instances in this study; without it, a grid that did produce a
  // tie would be ordered differently by different standard libraries, and the same seed would
  // generate a different instance on a different platform.
  std::sort(timeOrder.begin(), timeOrder.end(), [&](int32_t a, int32_t b) {
    if (in.times[a].startMinute != in.times[b].startMinute)
      return in.times[a].startMinute < in.times[b].startMinute;
    return a < b;
  });

  const double onSite = n * std::max(0.0, 1.0 - opt.onlineShare - opt.sportShare);
  const double effectiveSlots = double(opt.nDays) * nOrd * (1.0 + opt.biweeklyShare);
  in.nRooms = std::max(2, static_cast<int>(std::ceil(onSite / effectiveSlots * opt.roomSlack)));
  in.nAbstract = std::max(1, in.nGroups / 40 + 1);

  // ---- rooms, buildings, travel --------------------------------------------------------------
  in.roomBuilding.resize(in.nRooms);
  for (int r = 0; r < in.nRooms; ++r) in.roomBuilding[r] = rng.below(in.nBuildings);
  in.travel.assign(static_cast<size_t>(in.nBuildings) * in.nBuildings, 0);
  for (int a = 0; a < in.nBuildings; ++a)
    for (int b = a + 1; b < in.nBuildings; ++b) {
      // Directed and asymmetric: a hill, a one-way street, a funicular.  The two directions
      // routinely disagree, which is why a pair of classes has to be ordered in time before the
      // journey between them can be read.
      const int base = 5 + static_cast<int>(rng.below(11));
      in.travel[static_cast<size_t>(a) * in.nBuildings + b] = base;
      in.travel[static_cast<size_t>(b) * in.nBuildings + a] = base + static_cast<int>(rng.below(6));
    }
  in.commute = 25;
  in.abstractTravel = 30;
  in.abstractCapacity.assign(in.nAbstract, 0);
  in.abstractBuilding.assign(in.nAbstract, 0);
  for (int a = 0; a < in.nAbstract; ++a) {
    in.abstractCapacity[a] = 120 + 20 * static_cast<int>(rng.below(5));
    in.abstractBuilding[a] = (a == 0) ? -1 : static_cast<int>(rng.below(in.nBuildings));
  }

  // Room capacities: a long tail of seminar rooms and a few lecture halls.
  in.roomCapacity.assign(in.nRooms, 0);
  for (int r = 0; r < in.nRooms; ++r) {
    const double u = rng.unit();
    in.roomCapacity[r] = u < 0.62 ? 20 + static_cast<int>(rng.below(15))
                       : u < 0.90 ? 40 + static_cast<int>(rng.below(30))
                                  : 90 + static_cast<int>(rng.below(80));
  }

  // ---- departments: each group draws its lecturers from a small pool -------------------------
  const int nDept = std::max(1, in.nLecturers / 8);
  departmentOf.assign(in.nGroups, {});
  std::vector<std::vector<int32_t>> deptMembers(nDept);
  for (int l = 0; l < in.nLecturers; ++l) deptMembers[l % nDept].push_back(l);
  for (int g = 0; g < in.nGroups; ++g) {
    const int d0 = static_cast<int>(rng.below(nDept));
    const int d1 = static_cast<int>(rng.below(nDept));
    departmentOf[g] = deptMembers[d0];
    for (int32_t l : deptMembers[d1]) departmentOf[g].push_back(l);
    std::sort(departmentOf[g].begin(), departmentOf[g].end());
    departmentOf[g].erase(std::unique(departmentOf[g].begin(), departmentOf[g].end()),
                          departmentOf[g].end());
  }

  const int nEnt = in.nLecturers + in.nGroups + in.nRooms + in.nAbstract;
  occ.assign(static_cast<size_t>(nEnt) * (opt.nDays + 1) * 2, {});
  groupLoad.assign(in.nGroups, 0);
  lecturerLoad.assign(in.nLecturers, 0);
}

void Planner::plant() {
  const int target = opt.nClasses;
  planted.clear();
  in.classes.clear();

  for (int day = 1; day <= opt.nDays && in.nClasses() < target; ++day) {
    for (int32_t ti : timeOrder) {
      if (in.nClasses() >= target) break;
      const bool sportSlot = in.times[ti].grid == 1;
      const int start = in.times[ti].startMinute;
      const int duration = sportSlot ? 90 : 80;
      const int end = start + duration;
      const int perSlot = sportSlot ? std::max(1, in.nAbstract * 2) : in.nRooms;

      for (int k = 0; k < perSlot * 2 && in.nClasses() < target; ++k) {
        Parity parity = Parity::Weekly;
        if (rng.coin(opt.biweeklyShare))
          parity = rng.coin(0.5) ? Parity::Numerator : Parity::Denominator;

        PlaceKind kind = PlaceKind::Room;
        int abstractE = -1;
        if (sportSlot) {
          const int a = static_cast<int>(rng.below(in.nAbstract));
          abstractE = in.abstractId(a);
          kind = in.abstractBuilding[a] >= 0 ? PlaceKind::AbstractHere : PlaceKind::AbstractNowhere;
        } else if (rng.coin(opt.onlineShare / std::max(0.05, 1.0 - opt.sportShare))) {
          kind = PlaceKind::Online;
        }

        // ---- a group with the lightest load that is free here ---------------------------------
        int g = -1;
        for (int tries = 0; tries < 24; ++tries) {
          const int cand = static_cast<int>(rng.below(in.nGroups));
          if (isBusy(in.groupId(cand), day, parity, start, end)) continue;
          if (g < 0 || groupLoad[cand] < groupLoad[g]) g = cand;
        }
        if (g < 0) continue;

        const int students = 18 + static_cast<int>(rng.below(14));
        std::vector<int32_t> groups{in.groupId(g)};
        int headcount = students;
        if (!sportSlot && rng.coin(opt.streamShare)) {
          const int extra = 1 + static_cast<int>(rng.below(2));
          for (int e = 0; e < extra; ++e) {
            const int g2 = static_cast<int>(rng.below(in.nGroups));
            if (g2 == g) continue;
            if (isBusy(in.groupId(g2), day, parity, start, end)) continue;
            if (std::find(groups.begin(), groups.end(), in.groupId(g2)) != groups.end()) continue;
            groups.push_back(in.groupId(g2));
            headcount += 18 + static_cast<int>(rng.below(14));
          }
        }

        const std::vector<int32_t>& pool = departmentOf[g];
        int lect = -1;
        for (int tries = 0; tries < 24 && !pool.empty(); ++tries) {
          const int cand = pool[rng.below(static_cast<uint32_t>(pool.size()))];
          if (isBusy(cand, day, parity, start, end)) continue;
          if (lect < 0 || lecturerLoad[cand] < lecturerLoad[lect]) lect = cand;
        }
        if (lect < 0) continue;

        int room = -1, building = -2;
        if (kind == PlaceKind::Room) {
          const int from = static_cast<int>(rng.below(static_cast<uint32_t>(in.nRooms)));
          for (int s = 0; s < in.nRooms; ++s) {
            const int r = (from + s) % in.nRooms;
            if (in.roomCapacity[r] < headcount) continue;
            if (isBusy(in.roomId(r), day, parity, start, end)) continue;
            room = r;
            break;
          }
          if (room < 0) continue;
          building = in.roomBuilding[room];
        } else if (kind == PlaceKind::Online) {
          building = -1;
        } else {
          const int a = abstractE - (in.nLecturers + in.nGroups + in.nRooms);
          building = in.abstractBuilding[a];
          if (!abstractFits(abstractE, day, parity, start, end, headcount)) continue;
        }

        bool ok = travelOk(lect, day, parity, start, end, building, kind, abstractE);
        for (int32_t gg : groups)
          if (ok) ok = travelOk(gg, day, parity, start, end, building, kind, abstractE);
        if (!ok) continue;

        // ---- commit ---------------------------------------------------------------------------
        const int idx = in.nClasses();
        ClassSpec cs;
        cs.lecturers = {static_cast<int32_t>(lect)};
        cs.groups = groups;
        cs.abstractRoom = abstractE;
        cs.kind = kind;
        cs.duration = duration;
        cs.students = headcount;
        cs.movable = true;
        in.classes.push_back(cs);

        Placement p;
        p.day = static_cast<int8_t>(day);
        p.time = static_cast<int16_t>(ti);
        p.parity = parity;
        p.room = room;
        planted.push_back(p);

        markBusy(lect, day, parity, start, end, idx);
        ++lecturerLoad[lect];
        for (int32_t gg : groups) {
          markBusy(gg, day, parity, start, end, idx);
          ++groupLoad[gg - in.nLecturers];
        }
        if (room >= 0) markBusy(in.roomId(room), day, parity, start, end, idx);
        if (abstractE >= 0) markBusy(abstractE, day, parity, start, end, idx);
      }
    }
  }
}

void Planner::deriveDomains() {
  const int nCls = in.nClasses();

  // ---- per-entity daily peak, and the cap read off it -----------------------------------------
  const int nEnt = in.nLecturers + in.nGroups + in.nRooms + in.nAbstract;
  std::vector<int32_t> peak(static_cast<size_t>(nEnt) * (opt.nDays + 1), 0);
  for (int c = 0; c < nCls; ++c) {
    const Placement& p = planted[c];
    const ClassSpec& cs = in.classes[c];
    auto bump = [&](int e) { ++peak[static_cast<size_t>(e) * (opt.nDays + 1) + p.day]; };
    for (int32_t e : cs.lecturers) bump(e);
    for (int32_t e : cs.groups) bump(e);
    if (p.room >= 0) bump(in.roomId(p.room));
  }
  in.maxPerDay.assign(nEnt, 1 << 20);
  for (int e = 0; e < nEnt; ++e) {
    if (e >= in.nLecturers + in.nGroups) continue;  // cap lecturers and groups only
    int mx = 0;
    for (int d = 1; d <= opt.nDays; ++d)
      mx = std::max(mx, peak[static_cast<size_t>(e) * (opt.nDays + 1) + d]);
    if (mx > 0) in.maxPerDay[e] = mx + opt.capSlack;
  }

  // ---- availability windows, consistent with the planted schedule ----------------------------
  // A window is only imposed where the planted schedule leaves the entity free, so the hidden
  // solution survives every restriction the instance declares.
  std::vector<uint8_t> blockedPerson(static_cast<size_t>(in.nLecturers + in.nGroups) *
                                     (opt.nDays + 1) * nTimes, 0);
  std::vector<uint8_t> used(static_cast<size_t>(nEnt) * (opt.nDays + 1) * nTimes, 0);
  for (int c = 0; c < nCls; ++c) {
    const Placement& p = planted[c];
    const ClassSpec& cs = in.classes[c];
    auto mark = [&](int e) {
      used[(static_cast<size_t>(e) * (opt.nDays + 1) + p.day) * nTimes + p.time] = 1;
    };
    for (int32_t e : cs.lecturers) mark(e);
    for (int32_t e : cs.groups) mark(e);
    if (p.room >= 0) mark(in.roomId(p.room));
  }

  auto blockPerson = [&](int e, double share) {
    if (!rng.coin(share)) return;
    const int day = 1 + static_cast<int>(rng.below(opt.nDays));
    const int from = static_cast<int>(rng.below(static_cast<uint32_t>(nOrd)));
    const int to = std::min(nOrd - 1, from + 1 + static_cast<int>(rng.below(3)));
    for (int t = from; t <= to; ++t) {
      if (used[(static_cast<size_t>(e) * (opt.nDays + 1) + day) * nTimes + t]) continue;
      blockedPerson[(static_cast<size_t>(e) * (opt.nDays + 1) + day) * nTimes + t] = 1;
    }
  };
  for (int l = 0; l < in.nLecturers; ++l) blockPerson(l, opt.lecturerConstraintShare);
  for (int g = 0; g < in.nGroups; ++g)
    blockPerson(in.groupId(g), opt.groupConstraintShare);

  in.roomBlocked.assign(static_cast<size_t>(in.nRooms) * (opt.nDays + 1) * nTimes, 0);
  for (int r = 0; r < in.nRooms; ++r) {
    if (!rng.coin(opt.roomConstraintShare)) continue;
    const int day = 1 + static_cast<int>(rng.below(opt.nDays));
    const int from = static_cast<int>(rng.below(static_cast<uint32_t>(nOrd)));
    const int to = std::min(nOrd - 1, from + static_cast<int>(rng.below(3)));
    for (int t = from; t <= to; ++t) {
      if (used[(static_cast<size_t>(in.roomId(r)) * (opt.nDays + 1) + day) * nTimes + t]) continue;
      in.roomBlocked[(static_cast<size_t>(r) * (opt.nDays + 1) + day) * nTimes + t] = 1;
    }
  }

  // ---- the time and room domains --------------------------------------------------------------
  for (int c = 0; c < nCls; ++c) {
    ClassSpec& cs = in.classes[c];
    const Placement& p = planted[c];
    const int grid = in.times[p.time].grid;

    std::vector<Parity> parities;
    if (p.parity == Parity::Weekly) parities = {Parity::Weekly};
    else parities = {Parity::Numerator, Parity::Denominator};

    cs.slots.clear();
    for (int d = 1; d <= opt.nDays; ++d) {
      for (int t = 0; t < nTimes; ++t) {
        if (in.times[t].grid != grid) continue;
        bool ok = true;
        for (int32_t e : cs.lecturers)
          ok = ok && !blockedPerson[(static_cast<size_t>(e) * (opt.nDays + 1) + d) * nTimes + t];
        for (int32_t e : cs.groups)
          ok = ok && !blockedPerson[(static_cast<size_t>(e) * (opt.nDays + 1) + d) * nTimes + t];
        if (!ok) continue;
        for (Parity pp : parities)
          cs.slots.push_back({static_cast<uint8_t>(d), static_cast<uint16_t>(t), pp});
      }
    }

    cs.rooms.clear();
    if (cs.kind == PlaceKind::Room) {
      std::vector<int32_t> eligible;
      for (int r = 0; r < in.nRooms; ++r)
        if (in.roomCapacity[r] >= cs.students) eligible.push_back(r);
      rng.shuffle(eligible);
      if (static_cast<int>(eligible.size()) > opt.roomDomainSample)
        eligible.resize(opt.roomDomainSample);
      if (std::find(eligible.begin(), eligible.end(), p.room) == eligible.end())
        eligible.push_back(p.room);
      std::sort(eligible.begin(), eligible.end());
      cs.rooms = eligible;
    }
  }

  // ---- immovable classes -----------------------------------------------------------------------
  for (int c = 0; c < nCls; ++c) {
    if (!rng.coin(opt.fixedShare)) continue;
    in.classes[c].movable = false;
    in.classes[c].fixed = planted[c];
  }

  in.name = "planted";
  in.finalise();
}

}  // namespace

GeneratedInstance generateInstance(const GenOptions& opt) {
  Planner pl(opt);
  pl.build();
  pl.plant();
  pl.deriveDomains();
  GeneratedInstance out;
  out.instance = std::move(pl.in);
  out.planted = std::move(pl.planted);
  return out;
}

}  // namespace tt
