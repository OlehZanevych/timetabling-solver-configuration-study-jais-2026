// An independent scorer.
//
// This file deliberately shares no code with `state.cpp`.  It re-reads a schedule from scratch and
// counts the nine penalty terms and the four hard filters by the most direct method the
// definitions allow -- sorting and scanning, never a bitmask -- so that an error in the
// incremental evaluator cannot hide behind an identical error in the thing that checks it.  Every
// number reported by every experiment in this repository is produced here, not by the search's own
// counters.
#pragma once

#include <vector>

#include "model.hpp"

namespace tt {

struct Score {
  double pi[9] = {0};
  int domainBreaches = 0;      ///< placements outside S(c) x R(c)
  int availabilityBreaches = 0;///< a room used while unavailable
  int capBreaches = 0;         ///< per-(entity, day) load cap exceeded
  int unplaced = 0;
  int immovableMoved = 0;

  double hard() const { return pi[0] + pi[1] + pi[2] + pi[3] + pi[4] + pi[5]; }
  double soft() const;
  double objective(const double* beta) const;
  bool clean() const {
    return domainBreaches == 0 && availabilityBreaches == 0 && capBreaches == 0 &&
           immovableMoved == 0;
  }
};

Score validate(const Instance& inst, const std::vector<Placement>& sigma);

}  // namespace tt
