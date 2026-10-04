#include "model.hpp"

#include <algorithm>

namespace tt {

void Instance::finalise() {
  // ---- the tick axis -------------------------------------------------------------------------
  // Every minute value that can begin or end an occupied interval.  A class occupies the half-open
  // interval [start, start + duration), so the ticks are the bell starts together with every
  // start + duration that occurs.
  durations.clear();
  for (const ClassSpec& c : classes) durations.push_back(c.duration);
  std::sort(durations.begin(), durations.end());
  durations.erase(std::unique(durations.begin(), durations.end()), durations.end());
  if (durations.empty()) durations.push_back(80);

  std::vector<int32_t> raw;
  for (const TimeSpec& t : times) {
    raw.push_back(t.startMinute);
    for (int32_t d : durations) raw.push_back(t.startMinute + d);
  }
  std::sort(raw.begin(), raw.end());
  raw.erase(std::unique(raw.begin(), raw.end()), raw.end());
  ticks = raw;

  auto tickIndex = [&](int32_t minute) {
    return static_cast<int>(std::lower_bound(ticks.begin(), ticks.end(), minute) - ticks.begin());
  };

  // ---- the bell mask -------------------------------------------------------------------------
  bells = Mask{};
  for (const TimeSpec& t : times) bells.set(tickIndex(t.startMinute));

  // ---- per-(time, duration) occupancy masks --------------------------------------------------
  timeMaskByTimeDur.assign(times.size() * durations.size(), Mask{});
  for (size_t ti = 0; ti < times.size(); ++ti) {
    for (size_t di = 0; di < durations.size(); ++di) {
      const int a = tickIndex(times[ti].startMinute);
      const int b = tickIndex(times[ti].startMinute + durations[di]);  // exclusive
      Mask m;
      for (int k = a; k < b; ++k) m.set(k);
      timeMaskByTimeDur[ti * durations.size() + di] = m;
    }
  }

  // ---- static entity lists and the movable index ---------------------------------------------
  movable.clear();
  for (size_t i = 0; i < classes.size(); ++i) {
    ClassSpec& c = classes[i];
    c.staticEntities.clear();
    for (int32_t l : c.lecturers) c.staticEntities.push_back(l);
    for (int32_t g : c.groups) c.staticEntities.push_back(g);
    if (c.abstractRoom >= 0) c.staticEntities.push_back(c.abstractRoom);
    if (c.movable) movable.push_back(static_cast<int32_t>(i));
  }

  if (static_cast<int>(maxPerDay.size()) < nEntities())
    maxPerDay.resize(nEntities(), 1 << 20);
  if (static_cast<int>(roomBuilding.size()) < nRooms) roomBuilding.resize(nRooms, 0);
  if (static_cast<int>(abstractCapacity.size()) < nAbstract) abstractCapacity.resize(nAbstract, -1);
  if (static_cast<int>(abstractBuilding.size()) < nAbstract) abstractBuilding.resize(nAbstract, -1);
  if (travel.empty()) travel.assign(static_cast<size_t>(nBuildings) * nBuildings, 0);
}

}  // namespace tt
