// Instances built backwards around a hidden feasible schedule.
//
// The generator first *constructs* a schedule -- walking the week slot by slot and placing each
// class into resources that are free at that moment and reachable in the time available -- and then
// reads the instance off it: participants, room suitability, every availability window and every
// per-day cap.  Two things follow, and they are what make a residual cost mean anything.
//
//   * A schedule with no hard violation provably exists for every instance produced, because one
//     was built before the instance was.  Anything a search cannot reach is a property of the
//     search, never of the data.
//   * Every result has a yardstick: the planted schedule is scored by the same independent
//     validator, so its own soft cost is reported alongside.  It is feasible and plausible but not
//     optimal, so a search beating it is expected; the question is by how much, and whether zero is
//     reached.
#pragma once

#include <cstdint>
#include <vector>

#include "model.hpp"

namespace tt {

struct GenOptions {
  int nClasses = 800;
  int nDays = 6;
  int nOrdinaryTimes = 7;
  int nSportTimes = 3;
  int nBuildings = 3;

  double roomSlack = 1.15;            ///< spare room capacity above the planted peak
  double lecturerConstraintShare = 0.35;
  double groupConstraintShare = 0.25;
  double roomConstraintShare = 0.15;
  double biweeklyShare = 0.20;        ///< share of classes taught in one calendar week only
  double onlineShare = 0.06;
  double sportShare = 0.05;           ///< classes on the second bell grid, in an abstract room
  double fixedShare = 0.04;           ///< immovable classes: obstacles the search must respect
  double streamShare = 0.18;          ///< classes taught to more than one academic group

  int roomDomainSample = 12;          ///< admissible rooms per class, planted room included
  int capSlack = 1;                   ///< per-day cap above the planted peak; 0 makes it binding
  int classesPerGroup = 18;
  int classesPerLecturer = 12;

  uint64_t seed = 1;
};

struct GeneratedInstance {
  Instance instance;
  std::vector<Placement> planted;   ///< the hidden feasible schedule
};

/// Build an instance together with the schedule it was read off.  Throws nothing; on a
/// pathological parameter set it returns fewer classes than asked for, which the caller should
/// check with `instance.nClasses()`.
GeneratedInstance generateInstance(const GenOptions& opt);

}  // namespace tt
