// The instance: sets, domains, and the nine penalty terms' data.
//
// Notation follows the formal model.  Classes are C, of which the movable ones are C^m; entities
// are lecturers L, academic groups G, rooms R and abstract rooms A, carried in one index space so
// that a bucket can be addressed by (entity, day) regardless of what kind of entity it is.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mask.hpp"

namespace tt {

/// Parity of a placement: every week, numerator week only, denominator week only.
enum class Parity : uint8_t { Weekly = 0, Numerator = 1, Denominator = 2 };

/// Where a class is held.  The distinction matters to the travel term and to the overflow term.
enum class PlaceKind : uint8_t {
  Room = 0,             ///< an ordinary room drawn from the class's room domain
  AbstractHere = 1,     ///< a shared place with an address (a sports hall, a partner laboratory)
  AbstractNowhere = 2,  ///< a shared place with no address
  Online = 3            ///< held online; occupies no room
};

/// One admissible (day, time, parity) triple from a class's time domain S(c).
struct Slot {
  uint8_t day;     ///< 1..nDays
  uint16_t time;   ///< index into Instance::times
  Parity parity;
};

/// A placement of one class.  `room` is an index into the global room table, or -1.
struct Placement {
  int8_t day = -1;
  int16_t time = -1;
  Parity parity = Parity::Weekly;
  int32_t room = -1;

  bool placed() const { return day >= 0; }
  bool operator==(const Placement& o) const {
    return day == o.day && time == o.time && parity == o.parity && room == o.room;
  }
};

struct TimeSpec {
  int startMinute;  ///< minutes since midnight
  int grid;         ///< which bell grid this time belongs to
};

struct ClassSpec {
  std::vector<int32_t> lecturers;  ///< entity ids
  std::vector<int32_t> groups;     ///< entity ids
  int32_t abstractRoom = -1;       ///< entity id, or -1
  PlaceKind kind = PlaceKind::Room;
  int32_t duration = 80;    ///< minutes
  int32_t students = 25;
  bool movable = true;
  Placement fixed;          ///< the placement of an immovable class

  std::vector<Slot> slots;      ///< S(c)
  std::vector<int32_t> rooms;   ///< R(c); empty when the class needs no room

  /// Entities touched by the class other than its room: lecturers, groups, abstract room.
  /// Filled by Instance::finalise.
  std::vector<int32_t> staticEntities;
};

struct Instance {
  std::string name;
  int nLecturers = 0, nGroups = 0, nRooms = 0, nAbstract = 0, nBuildings = 1, nDays = 6;

  std::vector<TimeSpec> times;
  std::vector<int32_t> ticks;            ///< sorted distinct minute values
  Mask bells;                            ///< ticks that are a bell start on some grid
  std::vector<Mask> timeMaskByTimeDur;   ///< [time * nDurations + durIdx]; see finalise()
  std::vector<int32_t> durations;        ///< distinct class durations, sorted

  std::vector<int32_t> travel;           ///< nBuildings x nBuildings, directed, minutes
  int32_t commute = 30;                  ///< minutes charged between an online and an on-site class
  int32_t abstractTravel = 30;           ///< minutes charged when a place has no address

  std::vector<int32_t> roomBuilding;     ///< nRooms
  std::vector<int32_t> roomCapacity;     ///< nRooms; shapes R(c), not the objective
  std::vector<int32_t> abstractCapacity; ///< nAbstract; -1 when the place has no capacity
  std::vector<int32_t> abstractBuilding; ///< nAbstract; -1 when the place has no address
  std::vector<int32_t> maxPerDay;        ///< per entity; a large value means "no cap"
  std::vector<uint8_t> roomBlocked;      ///< nRooms * (nDays+1) * nTimes, 1 = unavailable

  std::vector<ClassSpec> classes;
  std::vector<int32_t> movable;          ///< indices of the movable classes

  // ---- derived sizes -------------------------------------------------------------------------

  int nClasses() const { return static_cast<int>(classes.size()); }
  int nTimes() const { return static_cast<int>(times.size()); }
  int nEntities() const { return nLecturers + nGroups + nRooms + nAbstract; }

  int lecturerId(int i) const { return i; }
  int groupId(int i) const { return nLecturers + i; }
  int roomId(int i) const { return nLecturers + nGroups + i; }
  int abstractId(int i) const { return nLecturers + nGroups + nRooms + i; }

  bool isRoomEntity(int e) const {
    return e >= nLecturers + nGroups && e < nLecturers + nGroups + nRooms;
  }
  bool isAbstractEntity(int e) const { return e >= nLecturers + nGroups + nRooms; }
  int roomOfEntity(int e) const { return e - (nLecturers + nGroups); }
  int abstractOfEntity(int e) const { return e - (nLecturers + nGroups + nRooms); }

  bool roomFree(int room, int day, int time) const {
    if (roomBlocked.empty()) return true;
    return !roomBlocked[(static_cast<size_t>(room) * (nDays + 1) + day) * times.size() + time];
  }

  /// Duration index of a class, for the time-mask table.
  int durationIndex(int duration) const {
    for (size_t i = 0; i < durations.size(); ++i)
      if (durations[i] == duration) return static_cast<int>(i);
    return 0;
  }

  const Mask& maskOf(int time, int durIdx) const {
    return timeMaskByTimeDur[static_cast<size_t>(time) * durations.size() + durIdx];
  }

  /// Build the tick axis, the bell mask, the per-(time, duration) masks and the static entity
  /// lists.  Call once, after the instance is fully populated.
  void finalise();
};

/// Do two parities share a calendar week?
inline bool weeksOverlap(Parity a, Parity b) {
  return a == Parity::Weekly || b == Parity::Weekly || a == b;
}

/// Is a class with parity p taught in calendar week w (1 or 2)?
inline bool inWeek(Parity p, int w) {
  return p == Parity::Weekly || (p == Parity::Numerator && w == 1) ||
         (p == Parity::Denominator && w == 2);
}

}  // namespace tt
