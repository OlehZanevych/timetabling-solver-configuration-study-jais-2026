// The compressed tick axis, and the two questions asked of it.
//
// Every time value that occurs in an instance -- a bell start, the end of a class -- is one of a
// few dozen distinct minute values.  Collect them into a sorted vector tau_0 < ... < tau_{m-1}
// (the *ticks*) and represent a class by the set of tick indices its half-open minute interval
// covers.  Because m <= 128 on every instance considered here, that set is a 128-bit word, and the
// two questions the objective asks of an occupancy set become a handful of machine instructions:
//
//     do a and b overlap?              mu(a) & mu(b) != 0
//     how many periods were skipped?   popcount(span(U) & ~U & bells)
//
// `span(U)` sets every bit between the lowest and the highest set bit of U.  It is the only
// non-obvious primitive here, and it is what turns the definition of an idle period ("a bell start
// strictly inside the occupied span of the day that is not itself occupied") into one expression.
#pragma once

#include <bit>
#include <cstdint>

namespace tt {

struct Mask {
  uint64_t lo = 0, hi = 0;

  constexpr bool empty() const { return (lo | hi) == 0; }
  constexpr bool intersects(const Mask& o) const { return (lo & o.lo) | (hi & o.hi); }

  constexpr Mask operator|(const Mask& o) const { return {lo | o.lo, hi | o.hi}; }
  constexpr Mask operator&(const Mask& o) const { return {lo & o.lo, hi & o.hi}; }
  constexpr Mask andNot(const Mask& o) const { return {lo & ~o.lo, hi & ~o.hi}; }

  Mask& operator|=(const Mask& o) {
    lo |= o.lo;
    hi |= o.hi;
    return *this;
  }

  constexpr bool operator==(const Mask& o) const { return lo == o.lo && hi == o.hi; }

  int count() const { return std::popcount(lo) + std::popcount(hi); }

  void set(int bit) {
    if (bit < 64) lo |= (1ULL << bit);
    else hi |= (1ULL << (bit - 64));
  }

  bool test(int bit) const {
    return bit < 64 ? ((lo >> bit) & 1ULL) : ((hi >> (bit - 64)) & 1ULL);
  }

  /// Index of the lowest set bit; 128 when empty.
  int lowest() const {
    if (lo) return std::countr_zero(lo);
    if (hi) return 64 + std::countr_zero(hi);
    return 128;
  }

  /// Index of the highest set bit; -1 when empty.
  int highest() const {
    if (hi) return 127 - std::countl_zero(hi);
    if (lo) return 63 - std::countl_zero(lo);
    return -1;
  }

  /// Every bit from the lowest set bit to the highest, inclusive.  Empty for an empty mask.
  Mask span() const {
    const int a = lowest();
    if (a == 128) return {};
    const int b = highest();
    return between(a, b);
  }

  /// All bits in [a, b], inclusive; a <= b assumed.
  static Mask between(int a, int b) {
    Mask m;
    const uint64_t all = ~0ULL;
    // Bits >= a.
    uint64_t loFrom = (a < 64) ? (all << a) : 0ULL;
    uint64_t hiFrom = (a < 64) ? all : (all << (a - 64));
    // Bits <= b.
    uint64_t loTo = (b < 64) ? (all >> (63 - b)) : all;
    uint64_t hiTo = (b < 64) ? 0ULL : (all >> (63 - (b - 64)));
    m.lo = loFrom & loTo;
    m.hi = hiFrom & hiTo;
    return m;
  }
};

}  // namespace tt
