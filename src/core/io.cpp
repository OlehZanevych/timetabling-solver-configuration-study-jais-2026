#include "io.hpp"

#include <fstream>
#include <sstream>

namespace tt {

bool writeInstance(const std::string& path, const Instance& in) {
  std::ofstream f(path);
  if (!f) return false;
  f << "tt-instance 1\n";
  f << "name " << (in.name.empty() ? "instance" : in.name) << "\n";
  f << "days " << in.nDays << "\n";
  f << "times " << in.times.size() << "\n";
  for (const TimeSpec& t : in.times) f << t.startMinute << " " << t.grid << "\n";
  f << "buildings " << in.nBuildings << "\n";
  for (int a = 0; a < in.nBuildings; ++a) {
    for (int b = 0; b < in.nBuildings; ++b)
      f << in.travel[static_cast<size_t>(a) * in.nBuildings + b] << (b + 1 == in.nBuildings ? '\n' : ' ');
  }
  f << "commute " << in.commute << "\n";
  f << "abstract_travel " << in.abstractTravel << "\n";
  f << "rooms " << in.nRooms << "\n";
  for (int r = 0; r < in.nRooms; ++r)
    f << in.roomBuilding[r] << " " << in.roomCapacity[r] << "\n";
  f << "abstract " << in.nAbstract << "\n";
  for (int a = 0; a < in.nAbstract; ++a)
    f << in.abstractBuilding[a] << " " << in.abstractCapacity[a] << "\n";
  f << "lecturers " << in.nLecturers << "\n";
  f << "groups " << in.nGroups << "\n";
  f << "max_per_day " << in.maxPerDay.size() << "\n";
  for (size_t i = 0; i < in.maxPerDay.size(); ++i)
    f << in.maxPerDay[i] << (i + 1 == in.maxPerDay.size() ? '\n' : ' ');
  size_t blocked = 0;
  for (uint8_t v : in.roomBlocked) blocked += v ? 1 : 0;
  f << "room_blocked " << blocked << "\n";
  if (blocked) {
    const size_t nT = in.times.size();
    for (int r = 0; r < in.nRooms; ++r)
      for (int d = 0; d <= in.nDays; ++d)
        for (size_t t = 0; t < nT; ++t)
          if (in.roomBlocked[(static_cast<size_t>(r) * (in.nDays + 1) + d) * nT + t])
            f << r << " " << d << " " << t << "\n";
  }
  f << "classes " << in.classes.size() << "\n";
  for (const ClassSpec& c : in.classes) {
    f << (c.movable ? 1 : 0) << " " << static_cast<int>(c.kind) << " " << c.duration << " "
      << c.students << " " << c.abstractRoom << "\n";
    f << "  L " << c.lecturers.size();
    for (int32_t l : c.lecturers) f << " " << l;
    f << "\n";
    f << "  G " << c.groups.size();
    for (int32_t g : c.groups) f << " " << g;
    f << "\n";
    f << "  S " << c.slots.size();
    for (const Slot& s : c.slots)
      f << " " << int(s.day) << " " << int(s.time) << " " << int(s.parity);
    f << "\n";
    f << "  R " << c.rooms.size();
    for (int32_t r : c.rooms) f << " " << r;
    f << "\n";
    if (!c.movable)
      f << "  F " << int(c.fixed.day) << " " << int(c.fixed.time) << " "
        << int(c.fixed.parity) << " " << c.fixed.room << "\n";
  }
  return true;
}

namespace {
bool expect(std::istream& f, const char* key) {
  std::string k;
  f >> k;
  return k == key;
}
}  // namespace

bool readInstance(const std::string& path, Instance& in) {
  std::ifstream f(path);
  if (!f) return false;
  std::string tok;
  int version = 0;
  f >> tok >> version;
  if (tok != "tt-instance" || version != 1) return false;
  if (!expect(f, "name")) return false;
  f >> in.name;
  if (!expect(f, "days")) return false;
  f >> in.nDays;
  if (!expect(f, "times")) return false;
  size_t nT = 0;
  f >> nT;
  in.times.resize(nT);
  for (size_t i = 0; i < nT; ++i) f >> in.times[i].startMinute >> in.times[i].grid;
  if (!expect(f, "buildings")) return false;
  f >> in.nBuildings;
  in.travel.assign(static_cast<size_t>(in.nBuildings) * in.nBuildings, 0);
  for (auto& v : in.travel) f >> v;
  if (!expect(f, "commute")) return false;
  f >> in.commute;
  if (!expect(f, "abstract_travel")) return false;
  f >> in.abstractTravel;
  if (!expect(f, "rooms")) return false;
  f >> in.nRooms;
  in.roomBuilding.resize(in.nRooms);
  in.roomCapacity.resize(in.nRooms);
  for (int r = 0; r < in.nRooms; ++r) f >> in.roomBuilding[r] >> in.roomCapacity[r];
  if (!expect(f, "abstract")) return false;
  f >> in.nAbstract;
  in.abstractBuilding.resize(in.nAbstract);
  in.abstractCapacity.resize(in.nAbstract);
  for (int a = 0; a < in.nAbstract; ++a) f >> in.abstractBuilding[a] >> in.abstractCapacity[a];
  if (!expect(f, "lecturers")) return false;
  f >> in.nLecturers;
  if (!expect(f, "groups")) return false;
  f >> in.nGroups;
  if (!expect(f, "max_per_day")) return false;
  size_t nE = 0;
  f >> nE;
  in.maxPerDay.resize(nE);
  for (auto& v : in.maxPerDay) f >> v;
  if (!expect(f, "room_blocked")) return false;
  size_t nB = 0;
  f >> nB;
  in.roomBlocked.assign(static_cast<size_t>(in.nRooms) * (in.nDays + 1) * nT, 0);
  for (size_t i = 0; i < nB; ++i) {
    int r, d, t;
    f >> r >> d >> t;
    in.roomBlocked[(static_cast<size_t>(r) * (in.nDays + 1) + d) * nT + t] = 1;
  }
  if (!expect(f, "classes")) return false;
  size_t nC = 0;
  f >> nC;
  in.classes.assign(nC, ClassSpec{});
  for (size_t i = 0; i < nC; ++i) {
    ClassSpec& c = in.classes[i];
    int mov = 0, kind = 0;
    f >> mov >> kind >> c.duration >> c.students >> c.abstractRoom;
    c.movable = mov != 0;
    c.kind = static_cast<PlaceKind>(kind);
    size_t k = 0;
    if (!expect(f, "L")) return false;
    f >> k;
    c.lecturers.resize(k);
    for (auto& v : c.lecturers) f >> v;
    if (!expect(f, "G")) return false;
    f >> k;
    c.groups.resize(k);
    for (auto& v : c.groups) f >> v;
    if (!expect(f, "S")) return false;
    f >> k;
    c.slots.resize(k);
    for (auto& s : c.slots) {
      int d, t, p;
      f >> d >> t >> p;
      s.day = static_cast<uint8_t>(d);
      s.time = static_cast<uint16_t>(t);
      s.parity = static_cast<Parity>(p);
    }
    if (!expect(f, "R")) return false;
    f >> k;
    c.rooms.resize(k);
    for (auto& v : c.rooms) f >> v;
    if (!c.movable) {
      if (!expect(f, "F")) return false;
      int d, t, p, r;
      f >> d >> t >> p >> r;
      c.fixed.day = static_cast<int8_t>(d);
      c.fixed.time = static_cast<int16_t>(t);
      c.fixed.parity = static_cast<Parity>(p);
      c.fixed.room = r;
    }
  }
  in.finalise();
  return static_cast<bool>(f);
}

bool writeSchedule(const std::string& path, const std::vector<Placement>& sigma) {
  std::ofstream f(path);
  if (!f) return false;
  f << "tt-schedule 1 " << sigma.size() << "\n";
  for (const Placement& p : sigma)
    f << int(p.day) << " " << int(p.time) << " " << int(p.parity) << " " << p.room << "\n";
  return true;
}

bool readSchedule(const std::string& path, std::vector<Placement>& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string tok;
  int version = 0;
  size_t n = 0;
  f >> tok >> version >> n;
  if (tok != "tt-schedule" || version != 1) return false;
  out.assign(n, Placement{});
  for (size_t i = 0; i < n; ++i) {
    int d, t, p, r;
    f >> d >> t >> p >> r;
    out[i].day = static_cast<int8_t>(d);
    out[i].time = static_cast<int16_t>(t);
    out[i].parity = static_cast<Parity>(p);
    out[i].room = r;
  }
  return static_cast<bool>(f);
}

}  // namespace tt
