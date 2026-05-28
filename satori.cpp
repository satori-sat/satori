/*******************************************************************************
 * Satori - SAT Solver for planning-style binary-dense CNF instances
 *
 * Copyright (c) 2026 Massimo Di Gruso <license@satori-sat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Build (static — recommended for distribution):
 *   g++ -O3 -std=c++17 -static -o satori satori.cpp
 *   strip satori                                 # optional: 2.4MB -> 1.9MB
 *
 * Build (dynamic — smaller binary, requires libstdc++ at runtime):
 *   g++ -O3 -std=c++17 -o satori satori.cpp     # ~76KB
 *
 * Usage:
 *   ./satori <file.cnf>
 ******************************************************************************/

#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cmath>

using namespace std;

static inline int lit_idx_of(int signed_lit) {
    return signed_lit > 0 ? (signed_lit << 1) : (((-signed_lit) << 1) | 1);
}
static inline int var_of(int lit_idx) { return lit_idx >> 1; }
static inline int neg_of(int lit_idx) { return lit_idx ^ 1; }
static inline bool is_pos(int lit_idx) { return (lit_idx & 1) == 0; }

struct VSIDSHeap {
    vector<int>    heap;
    vector<int>    pos;
    vector<double>* act;

    void init(int n_vars, vector<double>* activity) {
        act = activity;
        heap.clear();
        pos.assign(n_vars + 1, -1);
    }

    bool empty() const { return heap.empty(); }
    int  size()  const { return (int)heap.size(); }
    bool contains(int v) const { return pos[v] != -1; }

    void sift_up(int i) {
        int v = heap[i];
        while (i > 0) {
            int parent = (i - 1) >> 1;
            int pv = heap[parent];
            if ((*act)[v] > (*act)[pv]) {
                heap[i] = pv;
                pos[pv] = i;
                i = parent;
            } else break;
        }
        heap[i] = v;
        pos[v] = i;
    }

    void sift_down(int i) {
        int n = (int)heap.size();
        int v = heap[i];
        while (true) {
            int l = 2*i + 1, r = 2*i + 2, best = i;
            int best_v = v;
            if (l < n && (*act)[heap[l]] > (*act)[best_v]) { best = l; best_v = heap[l]; }
            if (r < n && (*act)[heap[r]] > (*act)[best_v]) { best = r; best_v = heap[r]; }
            if (best == i) break;
            heap[i] = best_v;
            pos[best_v] = i;
            i = best;
        }
        heap[i] = v;
        pos[v] = i;
    }

    void insert(int v) {
        if (pos[v] != -1) return;
        pos[v] = (int)heap.size();
        heap.push_back(v);
        sift_up(pos[v]);
    }

    int extract_max() {
        if (heap.empty()) return -1;
        int top = heap[0];
        int last = heap.back();
        heap.pop_back();
        pos[top] = -1;
        if (!heap.empty()) {
            heap[0] = last;
            pos[last] = 0;
            sift_down(0);
        }
        return top;
    }

    void increase(int v) {
        if (pos[v] == -1) return;
        sift_up(pos[v]);
    }
};

class SatoriCDCL {
public:

    string cnf_file;
    int    num_vars = 0;
    int    num_clauses_input = 0;

    vector<int32_t>         lit_arena;
    vector<uint32_t>        cl_begin;
    vector<uint32_t>        cl_size;

    inline int32_t*       cl_data(int cid)       { return lit_arena.data() + cl_begin[cid]; }
    inline const int32_t* cl_data(int cid) const { return lit_arena.data() + cl_begin[cid]; }
    inline uint32_t       cl_len (int cid) const { return cl_size[cid]; }
    inline int            cl_count() const       { return (int)cl_begin.size(); }

    vector<vector<int>>     watches;

    int                     n_problem_clauses = 0;

    vector<int>             lbd;
    vector<uint8_t>         removed;
    int                     max_learnts = 0;
    int                     n_learnts_alive = 0;
    int                     n_reductions = 0;
    double                  learnts_growth = 1.1;

    vector<int8_t>          assignment;
    vector<int>             level;
    vector<int>             reason;
    vector<int>             trail;
    vector<int>             trail_lim;
    int                     qhead = 0;

    vector<double>          activity;
    double                  var_inc = 1.0;
    double                  var_decay = 0.95;
    VSIDSHeap               order_heap;

    vector<int8_t>          phase;

    long long n_decisions = 0;
    long long n_conflicts = 0;
    long long n_propagations = 0;
    long long n_learned = 0;
    long long n_restarts = 0;
    double    parse_time = 0.0;
    double    solve_time = 0.0;

    int        restart_base = 100;

    void var_bump(int v) {
        activity[v] += var_inc;
        if (activity[v] > 1e100) {

            for (int i = 1; i <= num_vars; i++) activity[i] *= 1e-100;
            var_inc *= 1e-100;
        }
        order_heap.increase(v);
    }
    void var_decay_act() { var_inc *= (1.0 / var_decay); }

    inline int  decision_level() const { return (int)trail_lim.size(); }
    inline int8_t lit_value(int lit_idx) const {
        int v = var_of(lit_idx);
        int8_t a = assignment[v];
        if (a == 0) return 0;

        return is_pos(lit_idx) ? a : (int8_t)-a;
    }

    vector<uint8_t>         is_head;
    vector<int>             siblings_csr;
    vector<uint32_t>        siblings_off;
    vector<int>             head_of_var;
    long long               n_heads = 0;
    long long               n_sibling_pairs = 0;
    bool                    planning_pattern_detected = false;

    inline const int* sib_data(int v) const { return siblings_csr.data() + siblings_off[v]; }
    inline int        sib_len (int v) const { return (int)(siblings_off[v+1] - siblings_off[v]); }

    vector<int>             lit_remap;
    long long               n_scc_subst_vars = 0;
    long long               n_scc_bin_removed = 0;

    vector<vector<int>>     original_clauses_for_verify;
    bool                    scc_applied = false;

    bool compute_scc_leaders(const vector<vector<int>>& raw_clauses,
                             vector<int>& leader_out) {
        int N = 2 * (num_vars + 1);
        vector<vector<int>> graph(N);
        for (const auto& cl : raw_clauses) {
            if (cl.size() != 2) continue;
            int a = cl[0], b = cl[1];
            graph[neg_of(a)].push_back(b);
            graph[neg_of(b)].push_back(a);
        }

        vector<int> index(N, -1), lowlink(N, 0), stk;
        vector<uint8_t> on_stack(N, 0);
        leader_out.assign(N, -1);
        int idx_counter = 0;

        struct Frame { int v; int it; };
        vector<Frame> call_stack;

        for (int start = 0; start < N; start++) {
            if (index[start] != -1) continue;
            call_stack.push_back({start, 0});
            index[start] = idx_counter;
            lowlink[start] = idx_counter;
            idx_counter++;
            stk.push_back(start);
            on_stack[start] = 1;

            while (!call_stack.empty()) {
                Frame& f = call_stack.back();
                int v = f.v;
                const auto& nbrs = graph[v];
                if (f.it < (int)nbrs.size()) {
                    int w = nbrs[f.it++];
                    if (index[w] == -1) {
                        index[w] = idx_counter;
                        lowlink[w] = idx_counter;
                        idx_counter++;
                        stk.push_back(w);
                        on_stack[w] = 1;
                        call_stack.push_back({w, 0});
                    } else if (on_stack[w]) {
                        lowlink[v] = min(lowlink[v], index[w]);
                    }
                } else {

                    if (lowlink[v] == index[v]) {

                        int leader = -1;
                        vector<int> scc_members;
                        while (true) {
                            int w = stk.back(); stk.pop_back();
                            on_stack[w] = 0;
                            scc_members.push_back(w);
                            if (leader == -1 || w < leader) leader = w;
                            if (w == v) break;
                        }

                        vector<uint8_t> in_scc(num_vars + 1, 0);
                        for (int w : scc_members) {
                            int var = var_of(w);
                            if (in_scc[var]) {

                                for (int u : scc_members) {
                                    if (u == neg_of(w)) return false;
                                }

                            }
                            in_scc[var] = 1;
                        }

                        for (int w : scc_members) leader_out[w] = leader;
                    }

                    int popped = f.v;
                    call_stack.pop_back();
                    if (!call_stack.empty()) {
                        int parent = call_stack.back().v;
                        lowlink[parent] = min(lowlink[parent], lowlink[popped]);
                    }
                }
            }
        }
        return true;
    }

    bool apply_scc_substitution_with_budget(
            vector<vector<int>>& raw_clauses,
            chrono::time_point<chrono::high_resolution_clock> t_start,
            double budget_sec)
    {
        vector<int> leader;
        bool ok = compute_scc_leaders_with_budget(raw_clauses, leader, t_start, budget_sec);
        if (!ok) {

            if (leader.empty()) return true;
            return false;
        }

        lit_remap.assign(2 * (num_vars + 1), 0);
        for (int li = 0; li < (int)lit_remap.size(); li++) {
            lit_remap[li] = (leader[li] != -1) ? leader[li] : li;
        }
        for (int v = 1; v <= num_vars; v++) {
            int pos = v << 1;
            if (lit_remap[pos] != pos) n_scc_subst_vars++;
        }

        size_t orig_bin = 0, kept_bin = 0;
        vector<vector<int>> new_raw;
        new_raw.reserve(raw_clauses.size());
        for (auto& cl : raw_clauses) {
            if (cl.size() == 2) orig_bin++;
            vector<int> new_cl;
            new_cl.reserve(cl.size());
            for (int li : cl) new_cl.push_back(lit_remap[li]);
            sort(new_cl.begin(), new_cl.end());
            new_cl.erase(unique(new_cl.begin(), new_cl.end()), new_cl.end());
            bool taut = false;
            for (size_t i = 0; i + 1 < new_cl.size(); i++) {
                if (new_cl[i] == neg_of(new_cl[i+1])) { taut = true; break; }
            }
            if (taut) continue;
            if (new_cl.empty()) return false;
            if (new_cl.size() == 2) kept_bin++;
            new_raw.push_back(std::move(new_cl));
        }
        n_scc_bin_removed = (long long)orig_bin - (long long)kept_bin;
        raw_clauses = std::move(new_raw);
        return true;
    }

    bool compute_scc_leaders_with_budget(
            const vector<vector<int>>& raw_clauses,
            vector<int>& leader_out,
            chrono::time_point<chrono::high_resolution_clock> t_start,
            double budget_sec)
    {
        int N = 2 * (num_vars + 1);

        vector<uint32_t> head_off(N + 1, 0);
        size_t n_arcs = 0;
        for (const auto& cl : raw_clauses) {
            if (cl.size() != 2) continue;
            head_off[neg_of(cl[0]) + 1]++;
            head_off[neg_of(cl[1]) + 1]++;
            n_arcs += 2;
        }

        for (int i = 1; i <= N; i++) head_off[i] += head_off[i-1];
        vector<int32_t> adj_csr(n_arcs);
        vector<uint32_t> cur(N, 0);
        for (int i = 0; i < N; i++) cur[i] = head_off[i];
        for (const auto& cl : raw_clauses) {
            if (cl.size() != 2) continue;
            int a = cl[0], b = cl[1];
            adj_csr[cur[neg_of(a)]++] = b;
            adj_csr[cur[neg_of(b)]++] = a;
        }

        vector<uint32_t>().swap(cur);

        const int BUDGET_CHECK_INTERVAL = 10000;
        int ops_since_check = 0;

        vector<int> index(N, -1), lowlink(N, 0), stk;
        vector<uint8_t> on_stack(N, 0);
        leader_out.assign(N, -1);
        int idx_counter = 0;

        struct Frame { int v; int it; };
        vector<Frame> call_stack;

        for (int start = 0; start < N; start++) {
            if (index[start] != -1) continue;
            call_stack.push_back({start, 0});
            index[start] = idx_counter;
            lowlink[start] = idx_counter;
            idx_counter++;
            stk.push_back(start);
            on_stack[start] = 1;

            while (!call_stack.empty()) {
                if (++ops_since_check >= BUDGET_CHECK_INTERVAL) {
                    ops_since_check = 0;
                    double elapsed = chrono::duration<double>(
                        chrono::high_resolution_clock::now() - t_start).count();
                    if (elapsed > budget_sec) {
                        leader_out.clear();
                        return false;
                    }
                }

                Frame& f = call_stack.back();
                int v = f.v;

                int v_deg = (int)(head_off[v+1] - head_off[v]);
                if (f.it < v_deg) {
                    int w = adj_csr[head_off[v] + f.it++];
                    if (index[w] == -1) {
                        index[w] = idx_counter;
                        lowlink[w] = idx_counter;
                        idx_counter++;
                        stk.push_back(w);
                        on_stack[w] = 1;
                        call_stack.push_back({w, 0});
                    } else if (on_stack[w]) {
                        lowlink[v] = min(lowlink[v], index[w]);
                    }
                } else {
                    if (lowlink[v] == index[v]) {
                        int leader = -1;
                        vector<int> scc_members;
                        while (true) {
                            int w = stk.back(); stk.pop_back();
                            on_stack[w] = 0;
                            scc_members.push_back(w);
                            if (leader == -1 || w < leader) leader = w;
                            if (w == v) break;
                        }

                        for (int w : scc_members) {
                            for (int u : scc_members) {
                                if (u == neg_of(w)) {

                                    leader_out.assign(N, 0);
                                    return false;
                                }
                            }
                        }
                        for (int w : scc_members) leader_out[w] = leader;
                    }
                    int popped = f.v;
                    call_stack.pop_back();
                    if (!call_stack.empty()) {
                        int parent = call_stack.back().v;
                        lowlink[parent] = min(lowlink[parent], lowlink[popped]);
                    }
                }
            }
        }
        return true;
    }

    void detect_planning_pattern(const vector<vector<int>>& raw_clauses) {
        is_head     .assign(num_vars + 1, 0);
        head_of_var .assign(num_vars + 1, 0);

        int n_long_total = 0;
        int n_pattern    = 0;

        vector<pair<int,int>> pairs;
        pairs.reserve(raw_clauses.size() * 4);

        for (const auto& cl : raw_clauses) {
            if (cl.size() < 3) continue;
            n_long_total++;

            int neg_lit = 0;
            int neg_count = 0;
            bool ok = true;
            for (int li : cl) {
                if (!is_pos(li)) {
                    neg_lit = li;
                    neg_count++;
                    if (neg_count > 1) { ok = false; break; }
                }
            }
            if (!ok || neg_count != 1) continue;
            n_pattern++;

            int x_var = var_of(neg_lit);
            is_head[x_var] = 1;

            int ny = 0;
            int ys_local[64];
            vector<int> ys_overflow;
            for (int li : cl) {
                if (is_pos(li)) {
                    int yv = var_of(li);
                    if (head_of_var[yv] == 0) head_of_var[yv] = x_var;
                    if (ny < 64) ys_local[ny++] = yv;
                    else ys_overflow.push_back(yv);
                }
            }

            int total_ys = ny + (int)ys_overflow.size();
            auto get_y = [&](int i) { return i < ny ? ys_local[i] : ys_overflow[i - ny]; };
            for (int i = 0; i < total_ys; i++) {
                int y = get_y(i);
                for (int j = 0; j < total_ys; j++) {
                    if (i == j) continue;
                    pairs.emplace_back(y, get_y(j));
                }
            }
        }

        sort(pairs.begin(), pairs.end());
        pairs.erase(unique(pairs.begin(), pairs.end()), pairs.end());

        siblings_off.assign(num_vars + 2, 0);
        for (const auto& p : pairs) siblings_off[p.first + 1]++;
        for (int v = 1; v <= num_vars + 1; v++) siblings_off[v] += siblings_off[v-1];
        siblings_csr.assign(pairs.size(), 0);

        vector<uint32_t> cur(num_vars + 2, 0);
        for (int v = 0; v <= num_vars + 1; v++) cur[v] = siblings_off[v];
        for (const auto& p : pairs) {
            siblings_csr[cur[p.first]++] = p.second;
        }
        n_sibling_pairs = (long long)pairs.size();

        for (int v = 1; v <= num_vars; v++) if (is_head[v]) n_heads++;

        if (n_long_total > 0 && (double)n_pattern / n_long_total >= 0.05) {
            planning_pattern_detected = true;
        }

        if (n_heads >= 50) planning_pattern_detected = true;
    }

    void detect_planning_pattern_arena(const vector<int32_t>& raw_lits,
                                       const vector<uint32_t>& raw_off,
                                       int n_raw)
    {
        is_head     .assign(num_vars + 1, 0);
        head_of_var .assign(num_vars + 1, 0);

        int n_long_total = 0;
        int n_pattern    = 0;
        vector<pair<int,int>> pairs;
        pairs.reserve(n_raw * 4);

        for (int i = 0; i < n_raw; i++) {
            uint32_t a = raw_off[i], b = raw_off[i+1];
            int sz = (int)(b - a);
            if (sz < 3) continue;
            n_long_total++;

            int neg_lit = 0;
            int neg_count = 0;
            bool ok = true;
            for (uint32_t k = a; k < b; k++) {
                int li = raw_lits[k];
                if (!is_pos(li)) {
                    neg_lit = li;
                    neg_count++;
                    if (neg_count > 1) { ok = false; break; }
                }
            }
            if (!ok || neg_count != 1) continue;
            n_pattern++;

            int x_var = var_of(neg_lit);
            is_head[x_var] = 1;

            int ny = 0;
            int ys_local[64];
            vector<int> ys_overflow;
            for (uint32_t k = a; k < b; k++) {
                int li = raw_lits[k];
                if (is_pos(li)) {
                    int yv = var_of(li);
                    if (head_of_var[yv] == 0) head_of_var[yv] = x_var;
                    if (ny < 64) ys_local[ny++] = yv;
                    else ys_overflow.push_back(yv);
                }
            }
            int total_ys = ny + (int)ys_overflow.size();
            auto get_y = [&](int i) { return i < ny ? ys_local[i] : ys_overflow[i - ny]; };
            for (int i2 = 0; i2 < total_ys; i2++) {
                int y = get_y(i2);
                for (int j = 0; j < total_ys; j++) {
                    if (i2 == j) continue;
                    pairs.emplace_back(y, get_y(j));
                }
            }
        }

        sort(pairs.begin(), pairs.end());
        pairs.erase(unique(pairs.begin(), pairs.end()), pairs.end());

        siblings_off.assign(num_vars + 2, 0);
        for (const auto& p : pairs) siblings_off[p.first + 1]++;
        for (int v = 1; v <= num_vars + 1; v++) siblings_off[v] += siblings_off[v-1];
        siblings_csr.assign(pairs.size(), 0);
        vector<uint32_t> cur(num_vars + 2, 0);
        for (int v = 0; v <= num_vars + 1; v++) cur[v] = siblings_off[v];
        for (const auto& p : pairs) {
            siblings_csr[cur[p.first]++] = p.second;
        }
        n_sibling_pairs = (long long)pairs.size();

        for (int v = 1; v <= num_vars; v++) if (is_head[v]) n_heads++;

        if (n_long_total > 0 && (double)n_pattern / n_long_total >= 0.05) {
            planning_pattern_detected = true;
        }
        if (n_heads >= 50) planning_pattern_detected = true;
    }

    bool compute_scc_leaders_arena_with_budget(
            const vector<int32_t>& raw_lits,
            const vector<uint32_t>& raw_off,
            int n_raw,
            vector<int>& leader_out,
            chrono::time_point<chrono::high_resolution_clock> t_start,
            double budget_sec)
    {
        int N = 2 * (num_vars + 1);

        vector<uint32_t> head_off(N + 1, 0);
        size_t n_arcs = 0;
        for (int i = 0; i < n_raw; i++) {
            if (raw_off[i+1] - raw_off[i] != 2) continue;
            int a = raw_lits[raw_off[i]], b = raw_lits[raw_off[i]+1];
            head_off[neg_of(a) + 1]++;
            head_off[neg_of(b) + 1]++;
            n_arcs += 2;
        }
        for (int k = 1; k <= N; k++) head_off[k] += head_off[k-1];
        vector<int32_t> adj_csr(n_arcs);
        vector<uint32_t> cur(N, 0);
        for (int k = 0; k < N; k++) cur[k] = head_off[k];
        for (int i = 0; i < n_raw; i++) {
            if (raw_off[i+1] - raw_off[i] != 2) continue;
            int a = raw_lits[raw_off[i]], b = raw_lits[raw_off[i]+1];
            adj_csr[cur[neg_of(a)]++] = b;
            adj_csr[cur[neg_of(b)]++] = a;
        }
        vector<uint32_t>().swap(cur);

        const int BUDGET_CHECK_INTERVAL = 10000;
        int ops_since_check = 0;

        vector<int> index(N, -1), lowlink(N, 0), stk;
        vector<uint8_t> on_stack(N, 0);
        leader_out.assign(N, -1);
        int idx_counter = 0;

        struct Frame { int v; int it; };
        vector<Frame> call_stack;

        for (int start = 0; start < N; start++) {
            if (index[start] != -1) continue;
            call_stack.push_back({start, 0});
            index[start] = idx_counter;
            lowlink[start] = idx_counter;
            idx_counter++;
            stk.push_back(start);
            on_stack[start] = 1;

            while (!call_stack.empty()) {
                if (++ops_since_check >= BUDGET_CHECK_INTERVAL) {
                    ops_since_check = 0;
                    double elapsed = chrono::duration<double>(
                        chrono::high_resolution_clock::now() - t_start).count();
                    if (elapsed > budget_sec) {
                        leader_out.clear();
                        return false;
                    }
                }
                Frame& f = call_stack.back();
                int v = f.v;
                int v_deg = (int)(head_off[v+1] - head_off[v]);
                if (f.it < v_deg) {
                    int w = adj_csr[head_off[v] + f.it++];
                    if (index[w] == -1) {
                        index[w] = idx_counter;
                        lowlink[w] = idx_counter;
                        idx_counter++;
                        stk.push_back(w);
                        on_stack[w] = 1;
                        call_stack.push_back({w, 0});
                    } else if (on_stack[w]) {
                        lowlink[v] = min(lowlink[v], index[w]);
                    }
                } else {
                    if (lowlink[v] == index[v]) {
                        int leader = -1;
                        vector<int> scc_members;
                        while (true) {
                            int w = stk.back(); stk.pop_back();
                            on_stack[w] = 0;
                            scc_members.push_back(w);
                            if (leader == -1 || w < leader) leader = w;
                            if (w == v) break;
                        }

                        for (int w : scc_members) {
                            for (int u : scc_members) {
                                if (u == neg_of(w)) {
                                    leader_out.assign(N, 0);
                                    return false;
                                }
                            }
                        }
                        for (int w : scc_members) leader_out[w] = leader;
                    }
                    int popped = f.v;
                    call_stack.pop_back();
                    if (!call_stack.empty()) {
                        int parent = call_stack.back().v;
                        lowlink[parent] = min(lowlink[parent], lowlink[popped]);
                    }
                }
            }
        }
        return true;
    }

    bool add_clause_initial(vector<int>& lits) {

        sort(lits.begin(), lits.end());
        lits.erase(unique(lits.begin(), lits.end()), lits.end());
        for (size_t i = 0; i + 1 < lits.size(); i++) {
            if (lits[i] == neg_of(lits[i+1])) {

                return true;
            }
        }
        if (lits.empty()) return false;
        if (lits.size() == 1) {

            int8_t v = lit_value(lits[0]);
            if (v == 1) return true;
            if (v == -1) return false;
            int var = var_of(lits[0]);
            int val = is_pos(lits[0]) ? 1 : -1;
            assignment[var] = (int8_t)val;
            level[var] = 0;
            reason[var] = -2;
            trail.push_back(var);
            return true;
        }

        int cid = cl_count();
        cl_begin.push_back((uint32_t)lit_arena.size());
        cl_size .push_back((uint32_t)lits.size());
        for (int li : lits) lit_arena.push_back(li);
        lbd.push_back(0);
        removed.push_back(0);
        watches[lits[0]].push_back(cid);
        watches[lits[1]].push_back(cid);
        return true;
    }

    int add_learned_clause(vector<int>& lits, int learnt_lbd) {
        int cid = cl_count();
        cl_begin.push_back((uint32_t)lit_arena.size());
        cl_size .push_back((uint32_t)lits.size());
        for (int li : lits) lit_arena.push_back(li);
        lbd.push_back(learnt_lbd);
        removed.push_back(0);
        n_learnts_alive++;
        watches[lits[0]].push_back(cid);
        if (lits.size() >= 2)
            watches[lits[1]].push_back(cid);
        return cid;
    }

    bool parse_cnf() {
        auto t0 = chrono::high_resolution_clock::now();
        FILE* f = fopen(cnf_file.c_str(), "r");
        if (!f) { cerr << "ERROR: cannot open " << cnf_file << endl; exit(1); }

        char buffer[65536];
        bool header_seen = false;

        vector<int32_t>  raw_lits;
        vector<uint32_t> raw_off;
        raw_off.push_back(0);

        while (fgets(buffer, sizeof(buffer), f)) {
            char* line = buffer;
            while (*line==' '||*line=='\t'||*line=='\r'||*line=='\n') line++;
            if (*line == '\0' || *line == 'c') continue;
            if (!header_seen && strncmp(line, "p cnf", 5) == 0) {
                sscanf(line, "p cnf %d %d", &num_vars, &num_clauses_input);
                header_seen = true;

                raw_lits.reserve(num_clauses_input * 3);
                raw_off .reserve(num_clauses_input + 1);
                continue;
            }
            char* ptr = line;
            while (*ptr) {
                while (*ptr && (*ptr==' '||*ptr=='\t'||*ptr=='\r'||*ptr=='\n')) ptr++;
                if (!*ptr) break;
                bool neg = (*ptr == '-');
                if (neg) ptr++;
                if (!isdigit((unsigned char)*ptr)) break;
                int x = 0;
                while (isdigit((unsigned char)*ptr)) { x = x*10 + (*ptr - '0'); ptr++; }
                if (x == 0) {

                    if (raw_off.back() != raw_lits.size()) {
                        raw_off.push_back((uint32_t)raw_lits.size());
                    }
                } else {
                    int signed_lit = neg ? -x : x;
                    raw_lits.push_back(lit_idx_of(signed_lit));
                }
            }
        }
        if (raw_off.back() != raw_lits.size()) {
            raw_off.push_back((uint32_t)raw_lits.size());
        }
        fclose(f);

        int n_raw = (int)raw_off.size() - 1;

        assignment.assign(num_vars + 1, 0);
        level     .assign(num_vars + 1, -1);
        reason    .assign(num_vars + 1, -1);
        activity  .assign(num_vars + 1, 0.0);
        phase     .assign(num_vars + 1, -1);
        watches   .assign(2 * (num_vars + 1), {});
        trail.reserve(num_vars);

        order_heap.init(num_vars, &activity);

        {
            int n_bin_pre = 0;
            for (int i = 0; i < n_raw; i++) if (raw_off[i+1] - raw_off[i] == 2) n_bin_pre++;
            double bin_ratio = (double)n_bin_pre / max(1, n_raw);
            if (n_raw >= 10000 && bin_ratio >= 0.85) {
                auto t_scc_start = chrono::high_resolution_clock::now();
                vector<int> leader;
                bool ok = compute_scc_leaders_arena_with_budget(
                    raw_lits, raw_off, n_raw, leader, t_scc_start, 0.005);
                if (!ok) {
                    if (!leader.empty()) {
                        parse_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
                        return false;
                    }

                } else {
                    bool any_nontrivial = false;
                    for (int li = 0; li < (int)leader.size(); li++) {
                        if (leader[li] != -1 && leader[li] != li) { any_nontrivial = true; break; }
                    }
                    if (any_nontrivial) {

                        scc_applied = true;
                        original_clauses_for_verify.assign(n_raw, {});
                        for (int i = 0; i < n_raw; i++) {
                            uint32_t a = raw_off[i], b = raw_off[i+1];
                            original_clauses_for_verify[i].assign(
                                raw_lits.data() + a, raw_lits.data() + b);
                        }

                        lit_remap.assign(2 * (num_vars + 1), 0);
                        for (int li = 0; li < (int)lit_remap.size(); li++) {
                            lit_remap[li] = (leader[li] != -1) ? leader[li] : li;
                        }
                        for (int v = 1; v <= num_vars; v++) {
                            int pos = v << 1;
                            if (lit_remap[pos] != pos) n_scc_subst_vars++;
                        }

                        size_t orig_bin = 0, kept_bin = 0;
                        vector<int32_t>  raw_lits2;
                        vector<uint32_t> raw_off2;
                        raw_lits2.reserve(raw_lits.size());
                        raw_off2 .reserve(raw_off.size());
                        raw_off2.push_back(0);
                        bool unsat = false;
                        vector<int> tmp_cl;
                        for (int i = 0; i < n_raw; i++) {
                            uint32_t a = raw_off[i], b = raw_off[i+1];
                            if (b - a == 2) orig_bin++;
                            tmp_cl.assign(raw_lits.data() + a, raw_lits.data() + b);
                            for (auto& li : tmp_cl) li = lit_remap[li];
                            sort(tmp_cl.begin(), tmp_cl.end());
                            tmp_cl.erase(unique(tmp_cl.begin(), tmp_cl.end()), tmp_cl.end());
                            bool taut = false;
                            for (size_t k = 0; k + 1 < tmp_cl.size(); k++) {
                                if (tmp_cl[k] == neg_of(tmp_cl[k+1])) { taut = true; break; }
                            }
                            if (taut) continue;
                            if (tmp_cl.empty()) { unsat = true; break; }
                            if (tmp_cl.size() == 2) kept_bin++;
                            for (int li : tmp_cl) raw_lits2.push_back(li);
                            raw_off2.push_back((uint32_t)raw_lits2.size());
                        }
                        if (unsat) {
                            parse_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
                            return false;
                        }
                        n_scc_bin_removed = (long long)orig_bin - (long long)kept_bin;
                        raw_lits = std::move(raw_lits2);
                        raw_off  = std::move(raw_off2);
                        n_raw    = (int)raw_off.size() - 1;
                    }
                }
            }
        }

        detect_planning_pattern_arena(raw_lits, raw_off, n_raw);

        for (int i = 0; i < n_raw; i++) {
            uint32_t a = raw_off[i], b = raw_off[i+1];
            int len = (int)(b - a);
            if (len == 0) continue;
            int neg_count = 0;
            bool all_neg = true;
            for (uint32_t k = a; k < b; k++) {
                if (!is_pos(raw_lits[k])) neg_count++;
                else all_neg = false;
            }
            double w = 1.0 / max(1, len);
            if (all_neg) w *= 2.0;
            else if (neg_count == 1) {
                if (len <= 3) w *= 2.5;
                else if (len <= 5) w *= 1.5;
            }
            for (uint32_t k = a; k < b; k++) activity[var_of(raw_lits[k])] += w;
        }

        for (int v = 1; v <= num_vars; v++) order_heap.insert(v);

        vector<int> tmp_lits;
        for (int i = 0; i < n_raw; i++) {
            uint32_t a = raw_off[i], b = raw_off[i+1];
            tmp_lits.assign(raw_lits.data() + a, raw_lits.data() + b);
            if (!add_clause_initial(tmp_lits)) {
                parse_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
                return false;
            }
        }

        vector<int32_t>().swap(raw_lits);
        vector<uint32_t>().swap(raw_off);
        n_problem_clauses = cl_count();

        parse_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
        return true;
    }

    int propagate() {
        while (qhead < (int)trail.size()) {
            int v = trail[qhead++];
            int val = assignment[v];

            int false_lit = (val == 1) ? ((v << 1) | 1) : (v << 1);

            vector<int>& wl = watches[false_lit];
            int n = (int)wl.size();
            int i = 0, j = 0;
            int conflict_cid = -1;

            while (i < n) {
                int cid = wl[i];

                if (removed[cid]) { i++; continue; }
                int32_t* cl = cl_data(cid);
                int sz = (int)cl_len(cid);

                if (cl[0] == false_lit) std::swap(cl[0], cl[1]);

                int other = cl[0];
                int8_t other_val = lit_value(other);

                if (other_val == 1) {

                    wl[j++] = cid;
                    i++;
                    continue;
                }

                int k = 2;
                while (k < sz && lit_value(cl[k]) == -1) k++;
                if (k < sz) {

                    int new_watch = cl[k];
                    cl[1] = new_watch;
                    cl[k] = false_lit;
                    watches[new_watch].push_back(cid);

                    i++;
                    continue;
                }

                wl[j++] = cid;
                i++;
                if (other_val == 0) {

                    int uvar = var_of(other);
                    int uval = is_pos(other) ? 1 : -1;
                    assignment[uvar] = (int8_t)uval;
                    level[uvar] = decision_level();
                    reason[uvar] = cid;
                    trail.push_back(uvar);
                    n_propagations++;
                } else {

                    while (i < n) wl[j++] = wl[i++];
                    wl.resize(j);
                    return cid;
                }
            }
            wl.resize(j);
            if (conflict_cid != -1) return conflict_cid;
        }
        return -1;
    }

    vector<int8_t> seen;
    void analyze(int conflict_cid, vector<int>& out_learnt, int& out_btlevel) {
        if ((int)seen.size() != num_vars + 1) seen.assign(num_vars + 1, 0);

        out_learnt.clear();
        out_learnt.push_back(0);

        int path_count = 0;
        int p = -1;
        int idx = (int)trail.size() - 1;
        int confl = conflict_cid;
        int cur_level = decision_level();

        do {
            int32_t* cl = cl_data(confl);
            int sz = (int)cl_len(confl);

            for (int kk = 0; kk < sz; kk++) {
                int li = cl[kk];
                if (li == p) continue;
                int v = var_of(li);
                if (!seen[v] && level[v] > 0) {
                    var_bump(v);
                    seen[v] = 1;
                    if (level[v] >= cur_level) {
                        path_count++;
                    } else {
                        out_learnt.push_back(li);
                    }
                }
            }

            while (idx >= 0 && !seen[trail[idx]]) idx--;
            if (idx < 0) {

                break;
            }
            int v = trail[idx];

            p = (assignment[v] == 1) ? (v << 1) : ((v << 1) | 1);
            confl = reason[v];
            seen[v] = 0;
            path_count--;
            idx--;
        } while (path_count > 0);

        int uip_neg = p ^ 1;
        out_learnt[0] = uip_neg;

        if (out_learnt.size() == 1) {
            out_btlevel = 0;
        } else {
            int max_i = 1;
            int max_lvl = level[var_of(out_learnt[1])];
            for (int k = 2; k < (int)out_learnt.size(); k++) {
                int lv = level[var_of(out_learnt[k])];
                if (lv > max_lvl) { max_lvl = lv; max_i = k; }
            }
            std::swap(out_learnt[1], out_learnt[max_i]);
            out_btlevel = max_lvl;
        }

        for (int li : out_learnt) seen[var_of(li)] = 0;

        var_decay_act();
    }

    vector<int8_t> level_seen;
    int compute_lbd(const vector<int>& cl) {
        if ((int)level_seen.size() < decision_level() + 2)
            level_seen.assign(decision_level() + 2, 0);
        int count = 0;
        vector<int> touched;
        for (int li : cl) {
            int lv = level[var_of(li)];
            if (lv < 0) continue;
            if (!level_seen[lv]) {
                level_seen[lv] = 1;
                touched.push_back(lv);
                count++;
            }
        }
        for (int lv : touched) level_seen[lv] = 0;
        return count;
    }

    bool is_locked(int cid) {

        if (cl_len(cid) == 0) return false;
        int v = var_of(cl_data(cid)[0]);
        return assignment[v] != 0 && reason[v] == cid;
    }

    void reduce_db() {

        vector<int> learnts;
        learnts.reserve(n_learnts_alive);
        for (int cid = n_problem_clauses; cid < cl_count(); cid++) {
            if (!removed[cid]) learnts.push_back(cid);
        }

        sort(learnts.begin(), learnts.end(),
             [&](int a, int b) {
                 if (lbd[a] != lbd[b]) return lbd[a] < lbd[b];
                 return cl_len(a) < cl_len(b);
             });

        int keep_count = (int)learnts.size() / 2;
        int removed_count = 0;
        for (int idx = keep_count; idx < (int)learnts.size(); idx++) {
            int cid = learnts[idx];
            if (cl_len(cid) <= 2) continue;
            if (lbd[cid] <= 2) continue;
            if (is_locked(cid)) continue;
            removed[cid] = 1;
            n_learnts_alive--;
            removed_count++;
        }
        n_reductions++;

        max_learnts = (int)(max_learnts * learnts_growth);
        (void)removed_count;
    }

    void backtrack_to(int btlevel) {
        if (decision_level() <= btlevel) return;
        int target = trail_lim[btlevel];
        for (int i = (int)trail.size() - 1; i >= target; i--) {
            int v = trail[i];

            phase[v] = assignment[v];
            assignment[v] = 0;
            level[v] = -1;
            reason[v] = -1;
            order_heap.insert(v);
        }
        trail.resize(target);
        trail_lim.resize(btlevel);
        qhead = target;
    }

    static double luby(double y, int x) {

        int size, seq;
        for (size = 1, seq = 0; size < x + 1; seq++, size = size*2 + 1);
        while (size - 1 != x) {
            size = (size - 1) >> 1;
            seq--;
            x = x % size;
        }
        return pow(y, seq);
    }

    int pick_branch_var() {
        while (!order_heap.empty()) {
            int v = order_heap.extract_max();
            if (assignment[v] == 0) return v;
        }
        return -1;
    }

    long long n_flp_units = 0;
    long long n_flp_probes = 0;
    long long n_flp_sibling_skips = 0;
    int        flp_no_unit_streak_cap = 200;

    bool failed_literal_probing(int max_rounds = 3) {

        int n_binaries = 0;
        for (int cid = 0; cid < n_problem_clauses; cid++) {
            if (cl_len(cid) == 2) n_binaries++;
        }
        const int    FLP_MIN_CLAUSES   = 10000;
        const double FLP_MIN_BIN_RATIO = 0.85;
        double bin_ratio = (double)n_binaries / max(1, n_problem_clauses);
        if (n_problem_clauses < FLP_MIN_CLAUSES) return true;
        if (bin_ratio < FLP_MIN_BIN_RATIO)       return true;

        vector<int> degree(num_vars + 1, 0);
        for (int cid = 0; cid < n_problem_clauses; cid++) {
            int32_t* cl = cl_data(cid);
            int sz = (int)cl_len(cid);
            for (int kk = 0; kk < sz; kk++) degree[var_of(cl[kk])]++;
        }
        vector<int> all_vars_by_degree;
        all_vars_by_degree.reserve(num_vars);
        for (int v = 1; v <= num_vars; v++) all_vars_by_degree.push_back(v);
        sort(all_vars_by_degree.begin(), all_vars_by_degree.end(),
             [&](int a, int b){ return degree[a] > degree[b]; });

        vector<uint8_t> skip_pos_probe;
        if (planning_pattern_detected) {
            skip_pos_probe.assign(num_vars + 1, 0);
        }

        for (int round = 0; round < max_rounds; round++) {
            long long units_this_round = 0;
            int no_unit_streak = 0;
            if (planning_pattern_detected) {

                std::fill(skip_pos_probe.begin(), skip_pos_probe.end(), 0);
            }

            for (int v : all_vars_by_degree) {
                if (assignment[v] != 0) continue;

                bool fail_pos = false, fail_neg = false;
                bool did_probe_pos = false;

                if (!planning_pattern_detected || !skip_pos_probe[v]) {
                    did_probe_pos = true;
                    int trail_size_before = (int)trail.size();
                    trail_lim.push_back(trail_size_before);
                    assignment[v] = 1;
                    level[v] = 1;
                    reason[v] = -1;
                    trail.push_back(v);
                    n_flp_probes++;
                    int confl = propagate();
                    if (confl != -1) fail_pos = true;
                    for (int i = (int)trail.size() - 1; i >= trail_size_before; i--) {
                        int u = trail[i];
                        assignment[u] = 0;
                        level[u] = -1;
                        reason[u] = -1;
                    }
                    trail.resize(trail_size_before);
                    trail_lim.pop_back();
                    qhead = trail_size_before;
                } else {
                    n_flp_sibling_skips++;
                }

                {
                    int trail_size_before = (int)trail.size();
                    trail_lim.push_back(trail_size_before);
                    assignment[v] = -1;
                    level[v] = 1;
                    reason[v] = -1;
                    trail.push_back(v);
                    n_flp_probes++;
                    int confl = propagate();
                    if (confl != -1) fail_neg = true;
                    for (int i = (int)trail.size() - 1; i >= trail_size_before; i--) {
                        int u = trail[i];
                        assignment[u] = 0;
                        level[u] = -1;
                        reason[u] = -1;
                    }
                    trail.resize(trail_size_before);
                    trail_lim.pop_back();
                    qhead = trail_size_before;
                }

                if (fail_pos && fail_neg) return false;

                if (fail_pos || fail_neg) {
                    int val = fail_pos ? -1 : 1;
                    assignment[v] = (int8_t)val;
                    level[v] = 0;
                    reason[v] = -2;
                    trail.push_back(v);
                    n_flp_units++;
                    units_this_round++;
                    no_unit_streak = 0;
                    int confl = propagate();
                    if (confl != -1) return false;
                } else {
                    no_unit_streak++;
                    if (no_unit_streak >= flp_no_unit_streak_cap) break;
                }

                if (planning_pattern_detected && did_probe_pos && fail_pos) {
                    const int* ss = sib_data(v);
                    int sn = sib_len(v);
                    for (int kk = 0; kk < sn; kk++) {
                        int s = ss[kk];
                        if (assignment[s] == 0) skip_pos_probe[s] = 1;
                    }
                }
            }

            if (units_this_round == 0) break;
        }
        return true;
    }

    int solve() {
        auto t0 = chrono::high_resolution_clock::now();

        int c0 = propagate();
        if (c0 != -1) {
            solve_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
            return 20;
        }

        if (!failed_literal_probing()) {
            solve_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
            return 20;
        }

        max_learnts = max(100, n_problem_clauses / 3);

        long long restart_conflicts = (long long)(restart_base * luby(2.0, (int)n_restarts));
        long long conflicts_since_restart = 0;

        while (true) {
            int conflict_cid = propagate();
            if (conflict_cid != -1) {
                n_conflicts++;
                conflicts_since_restart++;
                if (decision_level() == 0) {
                    solve_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
                    return 20;
                }
                vector<int> learnt;
                int btlevel;
                analyze(conflict_cid, learnt, btlevel);
                backtrack_to(btlevel);

                if ((int)learnt.size() == 1) {

                    int li = learnt[0];
                    int v = var_of(li);
                    int val = is_pos(li) ? 1 : -1;
                    assignment[v] = (int8_t)val;
                    level[v] = 0;
                    reason[v] = -2;
                    trail.push_back(v);
                } else {
                    int learnt_lbd = compute_lbd(learnt);
                    int cid = add_learned_clause(learnt, learnt_lbd);
                    n_learned++;

                    int li = learnt[0];
                    int v = var_of(li);
                    int val = is_pos(li) ? 1 : -1;
                    assignment[v] = (int8_t)val;
                    level[v] = btlevel;
                    reason[v] = cid;
                    trail.push_back(v);
                }
            } else {

                if (conflicts_since_restart >= restart_conflicts) {
                    n_restarts++;
                    conflicts_since_restart = 0;
                    restart_conflicts = (long long)(restart_base * luby(2.0, (int)n_restarts));
                    backtrack_to(0);
                }

                if (decision_level() == 0 && n_learnts_alive > max_learnts) {
                    reduce_db();
                }

                int v = pick_branch_var();
                if (v == -1) {

                    solve_time = chrono::duration<double>(chrono::high_resolution_clock::now() - t0).count();
                    return 10;
                }
                int val = phase[v] >= 0 ? phase[v] : -1;
                if (val == 0) val = -1;
                trail_lim.push_back((int)trail.size());
                assignment[v] = (int8_t)val;
                level[v] = decision_level();
                reason[v] = -1;
                trail.push_back(v);
                n_decisions++;
            }
        }
    }

    bool verify() {
        if (scc_applied) {
            for (const auto& cl : original_clauses_for_verify) {
                bool sat = false;
                for (int li : cl) {
                    int mapped = lit_remap[li];
                    int v = var_of(mapped);
                    int a = assignment[v];
                    if (a == 0) a = 1;

                    if (is_pos(mapped) ? (a == 1) : (a == -1)) { sat = true; break; }
                }
                if (!sat) return false;
            }
            return true;
        }

        for (int cid = 0; cid < n_problem_clauses; cid++) {
            int32_t* cl = cl_data(cid);
            int sz = (int)cl_len(cid);
            bool sat = false;
            for (int kk = 0; kk < sz; kk++) {
                int li = cl[kk];
                int v = var_of(li);
                int a = assignment[v];
                if (a == 0) a = 1;
                if (is_pos(li) ? (a == 1) : (a == -1)) { sat = true; break; }
            }
            if (!sat) return false;
        }
        return true;
    }

    void run() {
        auto t_total = chrono::high_resolution_clock::now();
        bool ok = parse_cnf();
        int rc;
        if (!ok) rc = 20;
        else      rc = solve();
        double total = chrono::duration<double>(chrono::high_resolution_clock::now() - t_total).count();

        cout << "c ====================================================================" << endl;
        cout << "c SATORI-CDCL (1-UIP + VSIDS + Luby restart + watched literals)" << endl;
        cout << "c Input: " << cnf_file << endl;
        cout << "c Variables: " << num_vars << "  Clauses: " << num_clauses_input << endl;
        printf("c Parse Time: %.3f ms\n", parse_time * 1000);
        printf("c Solve Time: %.3f ms\n", solve_time * 1000);
        printf("c Total Time: %.3f ms\n", total      * 1000);
        cout << "c Decisions: "    << n_decisions    << endl;
        cout << "c Conflicts: "    << n_conflicts    << endl;
        cout << "c Propagations: " << n_propagations << endl;
        cout << "c FLP Probes: "   << n_flp_probes   << endl;
        cout << "c FLP Units: "    << n_flp_units    << endl;
        cout << "c FLP Sibling Skips: " << n_flp_sibling_skips << endl;
        cout << "c SCC Substituted Vars: " << n_scc_subst_vars << endl;
        cout << "c SCC Removed Binaries: " << n_scc_bin_removed << endl;
        cout << "c Planning Heads: " << n_heads << endl;
        cout << "c Planning Sibling Pairs: " << n_sibling_pairs << endl;
        cout << "c Planning Pattern Detected: " << (planning_pattern_detected ? "yes" : "no") << endl;
        cout << "c Learned: "      << n_learned      << endl;
        cout << "c Learnts Alive: " << n_learnts_alive << endl;
        cout << "c DB Reductions: " << n_reductions   << endl;
        cout << "c Restarts: "     << n_restarts     << endl;
        if (rc == 10) {
            bool v = verify();
            cout << "s SATISFIABLE" << endl;
            cout << "c Valid: " << (v ? "True" : "False") << endl;
        } else {
            cout << "s UNSATISFIABLE" << endl;
        }
    }
};

int main(int argc, char** argv) {
    if (argc < 2) { cerr << "Usage: " << argv[0] << " <file.cnf>" << endl; return 1; }
    SatoriCDCL s;
    s.cnf_file = argv[1];
    s.run();
    return 0;
}
