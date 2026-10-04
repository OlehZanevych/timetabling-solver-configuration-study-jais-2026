// Dinic's maximum-flow algorithm.
//
// Used to decide the capacitated Hall condition of one entity exactly, and to compute the maximum
// deficiency of Theorem 3.4 -- the minimum number of classes that entity must leave unplaced.
#pragma once

#include <algorithm>
#include <queue>
#include <vector>

namespace tt {

class MaxFlow {
 public:
  explicit MaxFlow(int n) : n_(n), head_(n, -1) {}

  void addEdge(int u, int v, int cap) {
    to_.push_back(v); cap_.push_back(cap); next_.push_back(head_[u]); head_[u] = (int)to_.size() - 1;
    to_.push_back(u); cap_.push_back(0);   next_.push_back(head_[v]); head_[v] = (int)to_.size() - 1;
  }

  int run(int s, int t) {
    int flow = 0;
    while (bfs(s, t)) {
      it_ = head_;
      while (int pushed = dfs(s, t, 1 << 30)) flow += pushed;
    }
    return flow;
  }

  /// After `run`, the vertices reachable from the source in the residual graph: the source side of
  /// a minimum cut, which is the witness set of the deficiency.
  std::vector<char> sourceSide(int s) {
    std::vector<char> seen(n_, 0);
    std::vector<int> st{s};
    seen[s] = 1;
    while (!st.empty()) {
      const int u = st.back();
      st.pop_back();
      for (int e = head_[u]; e != -1; e = next_[e])
        if (cap_[e] > 0 && !seen[to_[e]]) {
          seen[to_[e]] = 1;
          st.push_back(to_[e]);
        }
    }
    return seen;
  }

 private:
  bool bfs(int s, int t) {
    level_.assign(n_, -1);
    std::queue<int> q;
    q.push(s);
    level_[s] = 0;
    while (!q.empty()) {
      const int u = q.front();
      q.pop();
      for (int e = head_[u]; e != -1; e = next_[e])
        if (cap_[e] > 0 && level_[to_[e]] < 0) {
          level_[to_[e]] = level_[u] + 1;
          q.push(to_[e]);
        }
    }
    return level_[t] >= 0;
  }

  int dfs(int u, int t, int f) {
    if (u == t) return f;
    for (int& e = it_[u]; e != -1; e = next_[e]) {
      const int v = to_[e];
      if (cap_[e] <= 0 || level_[v] != level_[u] + 1) continue;
      const int d = dfs(v, t, std::min(f, cap_[e]));
      if (d > 0) {
        cap_[e] -= d;
        cap_[e ^ 1] += d;
        return d;
      }
    }
    return 0;
  }

  int n_;
  std::vector<int> head_, next_, to_, cap_, level_, it_;
};

}  // namespace tt
