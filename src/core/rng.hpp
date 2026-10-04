// A small, fast, reproducible pseudo-random number generator.
//
// xoshiro256** seeded through splitmix64.  Every experiment in this repository is reproducible
// from its seed alone: the generator is deterministic, its state is per-thread, and no library
// generator (whose implementation varies between standard libraries) is used anywhere.
#pragma once

#include <cstdint>
#include <vector>

namespace tt {

class Rng {
 public:
  explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ULL) { reseed(seed); }

  void reseed(uint64_t seed) {
    for (int i = 0; i < 4; ++i) s_[i] = splitmix(seed);
  }

  uint64_t next() {
    const uint64_t r = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return r;
  }

  /// Uniform on [0, n).  Lemire's multiply-shift rejection method: one multiplication in the
  /// common case and no modulo, with no bias.
  uint32_t below(uint32_t n) {
    if (n <= 1) return 0;
    uint64_t m = static_cast<uint64_t>(static_cast<uint32_t>(next())) * n;
    uint32_t l = static_cast<uint32_t>(m);
    if (l < n) {
      const uint32_t t = (-n) % n;
      while (l < t) {
        m = static_cast<uint64_t>(static_cast<uint32_t>(next())) * n;
        l = static_cast<uint32_t>(m);
      }
    }
    return static_cast<uint32_t>(m >> 32);
  }

  /// Uniform on [0, 1).
  double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

  bool coin(double p) { return unit() < p; }

  template <typename T>
  void shuffle(std::vector<T>& v) {
    for (size_t i = v.size(); i > 1; --i) {
      const size_t j = below(static_cast<uint32_t>(i));
      std::swap(v[i - 1], v[j]);
    }
  }

 private:
  static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

  uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  uint64_t s_[4];
};

}  // namespace tt
