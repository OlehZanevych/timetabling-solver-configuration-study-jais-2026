// A self-test that a reader can run before trusting any number in the paper.
//
// It checks the three things a computational study rests on and that a reader cannot otherwise
// verify without reading the whole implementation:
//
//   1. the planted schedule of every generated instance really is feasible, so that a residual hard
//      cost is a property of the search and never of the data;
//   2. the plain-text instance files round-trip -- an instance written and read back scores exactly
//      as the original did -- so that the files in data/ are the instances the experiments used;
//   3. the incremental evaluator agrees, term by term, with the independent validator along a random
//      walk of placements, so that the two implementations of the objective cannot be wrong in the
//      same way.
//
// It is a subcommand of every application in this series and takes seconds.
#pragma once

#include <string>

namespace tt {

/// Run every check.  Writes a line per check to stdout and returns 0 when all pass.
/// `scratchDir` is used for the round-trip files and may be any writable directory.
int runSelfTest(const std::string& scratchDir);

}  // namespace tt
