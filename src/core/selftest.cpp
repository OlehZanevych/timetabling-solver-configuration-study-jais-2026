#include "selftest.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "generate.hpp"
#include "io.hpp"
#include "model.hpp"
#include "rng.hpp"
#include "state.hpp"
#include "validate.hpp"

namespace tt {
namespace {

bool nearly(double a, double b) { return std::fabs(a - b) < 1e-6; }

/// Every hard term of the planted schedule must be zero, and no placement-time rule may be broken.
/// If this fails the instance family is unusable, because a residual hard cost could then be the
/// generator's fault rather than the search's.
bool checkPlanted(int& failures) {
  bool ok = true;
  for (int n : {200, 800, 3200}) {
    for (uint64_t seed = 1; seed <= 3; ++seed) {
      GenOptions go;
      go.nClasses = n;
      go.seed = seed;
      const GeneratedInstance gi = generateInstance(go);
      const Score sc = validate(gi.instance, gi.planted);
      const bool good = sc.hard() == 0 && sc.clean() && sc.unplaced == 0;
      if (!good) {
        std::printf("  FAIL planted n=%d seed=%llu hard=%.0f clean=%d unplaced=%d\n", n,
                    (unsigned long long)seed, sc.hard(), int(sc.clean()), sc.unplaced);
        ++failures;
        ok = false;
      }
    }
  }
  return ok;
}

/// An instance written to disk and read back must score exactly as the original did, on the same
/// schedule.  This is what makes the files in data/ evidence rather than decoration.
bool checkRoundTrip(const std::string& dir, int& failures) {
  bool ok = true;
  for (int n : {200, 800}) {
    for (uint64_t seed = 1; seed <= 2; ++seed) {
      GenOptions go;
      go.nClasses = n;
      go.seed = seed;
      const GeneratedInstance gi = generateInstance(go);

      const std::string ip = dir + "/.selftest-instance.txt";
      const std::string sp = dir + "/.selftest-schedule.txt";
      if (!writeInstance(ip, gi.instance) || !writeSchedule(sp, gi.planted)) {
        // Almost always the working directory rather than a real fault: the round-trip test needs
        // somewhere to put a scratch file, and the path is relative to wherever the binary was
        // invoked from.  Say so, rather than reporting it as a failed round trip.
        std::printf("  FAIL could not write to %s -- does that directory exist?  Run this from the\n"
                    "       application directory, or pass --results DIR.\n", dir.c_str());
        ++failures;
        return false;
      }
      Instance back;
      std::vector<Placement> sigma;
      if (!readInstance(ip, back) || !readSchedule(sp, sigma)) {
        std::printf("  FAIL could not read back n=%d seed=%llu\n", n, (unsigned long long)seed);
        ++failures;
        ok = false;
        continue;
      }
      if (back.nClasses() != gi.instance.nClasses()) {
        std::printf("  FAIL class count %d != %d\n", back.nClasses(), gi.instance.nClasses());
        ++failures;
        ok = false;
        continue;
      }
      const Score a = validate(gi.instance, gi.planted);
      const Score b = validate(back, sigma);
      for (int i = 0; i < 9; ++i) {
        if (!nearly(a.pi[i], b.pi[i])) {
          std::printf("  FAIL round-trip n=%d seed=%llu term %d: %.6f != %.6f\n", n,
                      (unsigned long long)seed, i + 1, a.pi[i], b.pi[i]);
          ++failures;
          ok = false;
          break;
        }
      }
      if (b.unplaced != a.unplaced || b.clean() != a.clean()) {
        std::printf("  FAIL round-trip n=%d seed=%llu: unplaced or legality differs\n", n,
                    (unsigned long long)seed);
        ++failures;
        ok = false;
      }
      // The scratch files are an artefact of the check, not a result: leave nothing behind.
      std::remove(ip.c_str());
      std::remove(sp.c_str());
    }
  }
  return ok;
}

/// Walk the schedule with random relocations and compare the incremental evaluator with the
/// independent validator after each one.  The two share no code; if they agree term by term over a
/// long walk, an error would have to be present in both in the same form.
bool checkEvaluatorAgreement(int& failures) {
  bool ok = true;
  for (int n : {200, 800}) {
    GenOptions go;
    go.nClasses = n;
    go.seed = 7;
    const GeneratedInstance gi = generateInstance(go);
    State st(gi.instance);
    st.setAll(gi.planted);
    st.flush();

    Rng rng(20260913u + n);
    for (int step = 0; step < 400; ++step) {
      const int c = static_cast<int>(rng.below(static_cast<uint32_t>(gi.instance.nClasses())));
      const ClassSpec& cs = gi.instance.classes[c];
      if (!cs.movable || cs.slots.empty()) continue;
      const Slot& s = cs.slots[rng.below(static_cast<uint32_t>(cs.slots.size()))];
      Placement p;
      p.day = static_cast<int8_t>(s.day);
      p.time = static_cast<int16_t>(s.time);
      p.parity = s.parity;
      p.room = cs.rooms.empty() ? -1 : cs.rooms[rng.below(static_cast<uint32_t>(cs.rooms.size()))];
      st.place(c, p);
      st.flush();

      if ((step % 40) != 0) continue;   // validating every step would dominate the runtime
      const Score sc = validate(gi.instance, st.placements());
      for (int i = 0; i < 9; ++i) {
        if (!nearly(sc.pi[i], st.term(i))) {
          std::printf("  FAIL evaluator n=%d step=%d term %d: validator %.6f, incremental %.6f\n",
                      n, step, i + 1, sc.pi[i], st.term(i));
          ++failures;
          ok = false;
          step = 1 << 20;               // one report per size is enough
          break;
        }
      }
    }
  }
  return ok;
}

}  // namespace

int runSelfTest(const std::string& scratchDir) {
  int failures = 0;
  std::printf("planted schedules feasible ......... %s\n",
              checkPlanted(failures) ? "ok" : "FAILED");
  std::printf("instance files round-trip .......... %s\n",
              checkRoundTrip(scratchDir, failures) ? "ok" : "FAILED");
  std::printf("evaluators agree term by term ...... %s\n",
              checkEvaluatorAgreement(failures) ? "ok" : "FAILED");
  if (failures == 0) std::printf("\nself-test passed\n");
  else std::printf("\nself-test FAILED with %d problem(s)\n", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace tt
