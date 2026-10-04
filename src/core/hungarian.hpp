// The Hungarian (Kuhn-Munkres) method for the linear assignment problem, O(n^3).
//
// Used by the permutation operator: with k classes lifted out of the schedule, the cost of putting
// class r at placement q is readable independently of where the other k-1 go, so the best
// permutation of the k placements among the k classes is a linear assignment problem and is solved
// exactly.  Note what "exactly" means: exactly over that relaxation.  The residual coupling among
// the k -- two of them sent to placements that clash with *each other* -- is not priced by the cost
// matrix, which is why the operator re-evaluates the resulting schedule under the true objective
// and refuses it if it does not improve.
#pragma once

#include <algorithm>
#include <limits>
#include <vector>

namespace tt {

/// Minimum-cost assignment of n rows to n columns.  `cost` is row-major n x n.  Returns the total
/// cost and fills `rowToCol`.  Entries may be infinite (an inadmissible pairing).
inline double hungarian(const std::vector<double>& cost, int n, std::vector<int>& rowToCol) {
  const double kInf = std::numeric_limits<double>::infinity();
  std::vector<double> u(n + 1, 0), v(n + 1, 0);
  std::vector<int> p(n + 1, 0), way(n + 1, 0);

  for (int i = 1; i <= n; ++i) {
    p[0] = i;
    int j0 = 0;
    std::vector<double> minv(n + 1, kInf);
    std::vector<char> used(n + 1, false);
    do {
      used[j0] = true;
      const int i0 = p[j0];
      double delta = kInf;
      int j1 = 0;
      for (int j = 1; j <= n; ++j) {
        if (used[j]) continue;
        const double cur = cost[static_cast<size_t>(i0 - 1) * n + (j - 1)] - u[i0] - v[j];
        if (cur < minv[j]) {
          minv[j] = cur;
          way[j] = j0;
        }
        if (minv[j] < delta) {
          delta = minv[j];
          j1 = j;
        }
      }
      if (delta == kInf) break;  // no admissible completion; caller must handle
      for (int j = 0; j <= n; ++j) {
        if (used[j]) {
          u[p[j]] += delta;
          v[j] -= delta;
        } else {
          minv[j] -= delta;
        }
      }
      j0 = j1;
    } while (p[j0] != 0);
    while (j0) {
      const int j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    }
  }

  rowToCol.assign(n, -1);
  double total = 0;
  for (int j = 1; j <= n; ++j)
    if (p[j] >= 1 && p[j] <= n) {
      rowToCol[p[j] - 1] = j - 1;
      total += cost[static_cast<size_t>(p[j] - 1) * n + (j - 1)];
    }
  return total;
}

}  // namespace tt
