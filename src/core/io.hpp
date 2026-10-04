// A plain-text instance format.
//
// Deliberately line-oriented and human-readable: an instance can be inspected, diffed and
// version-controlled, and a reader for it can be written in an afternoon in any language. Every
// experiment in this repository runs from files in this format rather than from a generator call,
// so that a result stays reproducible even if the generator is later changed.
#pragma once

#include <string>
#include <vector>

#include "model.hpp"

namespace tt {

/// Write an instance, and optionally a schedule alongside it, to `path`.
bool writeInstance(const std::string& path, const Instance& in);
bool readInstance(const std::string& path, Instance& out);

/// A schedule as `n` lines of "day time parity room" (-1 -1 W -1 for unplaced).
bool writeSchedule(const std::string& path, const std::vector<Placement>& sigma);
bool readSchedule(const std::string& path, std::vector<Placement>& out);

}  // namespace tt
