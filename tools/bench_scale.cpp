// Headless scaling probe for zoigraph's compute/persistence subsystems.
//
// Times each N-scaling component so we can see where interactivity breaks
// before the GUI is even involved. NOT a correctness test — a perf probe.
// Build it on demand (it is EXCLUDE_FROM_ALL):
//
//   cmake --build build --target bench_scale
//   ./build/bench_scale 10000 100000 500000
//   ./build/bench_scale --edge-ratio 2 --naive-cap 20000 500000
//
// Each tick figure is ONE integrate_step. The naive O(N^2) Coulomb pass is
// skipped above --naive-cap and projected from the last measured point, so a
// 500k run doesn't wedge for minutes. ppr is one neighbourhood query (what the
// query channel runs per LLM ask). RSS (Linux) is the peak for the run, so
// invoke once per size for a clean per-N memory figure.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "graph/ppr.h"
#include "persistence/db.h"
#include "persistence/seed.h"
#include "physics/physics_thread.h"  // integrate_step + SimParams
#include "physics/barnes_hut.h"

#if defined(__linux__)
static long read_proc_kb(const char* key) {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    const std::size_t klen = std::strlen(key);
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, klen) == 0) {
            std::sscanf(line + klen, " %ld kB", &kb);
            break;
        }
    }
    std::fclose(f);
    return kb;
}
static double rss_mb()  { long kb = read_proc_kb("VmRSS:");  return kb < 0 ? -1.0 : kb / 1024.0; }
static double peak_mb() { long kb = read_proc_kb("VmHWM:");  return kb < 0 ? -1.0 : kb / 1024.0; }
#else
static double rss_mb()  { return -1.0; }
static double peak_mb() { return -1.0; }
#endif

template <class F>
static double ms(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main(int argc, char** argv) {
    std::vector<long long> sizes;
    long long   edge_ratio = 2;
    long long   naive_cap  = 20000;
    std::string write_db;  // if set: generate the first size, save to this DB, exit
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--edge-ratio" && i + 1 < argc)      edge_ratio = std::atoll(argv[++i]);
        else if (a == "--naive-cap" && i + 1 < argc)  naive_cap  = std::atoll(argv[++i]);
        else if (a == "--write-db" && i + 1 < argc)   write_db   = argv[++i];
        else                                          sizes.push_back(std::atoll(a.c_str()));
    }
    if (sizes.empty()) sizes = {10000, 100000, 500000};

    // DB-generation mode: write a real project DB so the GUI can open it.
    if (!write_db.empty()) {
        const long long N = sizes.front();
        const long long E = N * edge_ratio;
        auto g = zg::persistence::make_random_fill(
            static_cast<int>(N), static_cast<int>(E), 0, 0.0, 200.0f, 42, true);
        zg::persistence::Database db(write_db);
        db.save_graph(g.nodes, g.edges);
        std::printf("wrote %s: %zu nodes, %zu edges\n",
                    write_db.c_str(), g.nodes.size(), g.edges.size());
        return 0;
    }

    std::printf("zoigraph scaling probe  (edge_ratio=%lld, naive_cap=%lld, DB=:memory:)\n",
                edge_ratio, naive_cap);
    std::printf("%10s %8s %9s %9s %8s %11s %9s %9s %9s\n",
                "N", "gen ms", "save ms", "load ms", "srch ms", "naive ms",
                "bh ms", "ppr ms", "rss MB");

    double   naive_ref_ms = -1.0;
    long long naive_ref_n = 0;

    for (long long N : sizes) {
        const long long E = N * edge_ratio;

        zg::persistence::InitialGraph g;
        const double t_gen = ms([&] {
            g = zg::persistence::make_random_fill(
                static_cast<int>(N), static_cast<int>(E),
                /*start_id=*/0, /*now_unix=*/0.0, /*spread=*/200.0f,
                /*rng_seed=*/42, /*with_data=*/true);
        });

        // Physics inputs derived from the generated graph.
        std::vector<Vector3> pos;
        pos.reserve(g.nodes.size());
        for (const auto& n : g.nodes) pos.push_back(n.position);
        const std::vector<Vector3> vel0(g.nodes.size(), Vector3{0, 0, 0});

        zg::persistence::Database db(":memory:");
        const double t_save = ms([&] { db.save_graph(g.nodes, g.edges); });

        std::vector<zg::persistence::StoredNode> ln;
        std::vector<zg::graph::Edge>             le;
        const double t_load = ms([&] { db.load_graph(ln, le); });

        std::size_t hits = 0;
        const double t_search = ms([&] { hits = db.search("alpha").size(); });
        (void)hits;

        // Naive O(N^2) Coulomb — one tick, only under the cap.
        double t_naive = -1.0;
        if (N <= naive_cap) {
            zg::physics::SimParams p;
            p.use_barnes_hut = false;
            auto p2 = pos;
            auto v2 = vel0;
            t_naive = ms([&] {
                zg::physics::integrate_step(p2, v2, g.edges, p);
            });
            naive_ref_ms = t_naive;
            naive_ref_n  = N;
        }

        // Barnes-Hut — one tick (the default physics path).
        double t_bh = 0.0;
        {
            zg::physics::SimParams p;
            p.use_barnes_hut = true;
            auto p2 = pos;
            auto v2 = vel0;
            t_bh = ms([&] {
                zg::physics::integrate_step(p2, v2, g.edges, p);
            });
        }

        // One neighbourhood query (anchor 0), as the channel runs per ask.
        // top_related lives in another TU, so the call can't be optimised away.
        const double t_ppr = ms([&] {
            auto r = zg::graph::top_related(
                static_cast<std::size_t>(N), g.edges, 0, 16);
            (void)r;
        });

        char naive_col[16];
        if (t_naive >= 0.0) std::snprintf(naive_col, sizeof(naive_col), "%.1f", t_naive);
        else                std::snprintf(naive_col, sizeof(naive_col), "skip");

        std::printf("%10lld %8.1f %9.1f %9.1f %8.1f %11s %9.1f %9.2f %9.1f\n",
                    N, t_gen, t_save, t_load, t_search, naive_col, t_bh, t_ppr, rss_mb());
        std::fflush(stdout);
    }

    if (naive_ref_ms > 0.0) {
        std::printf("\nnaive O(N^2) projection from N=%lld (%.1f ms/tick):\n",
                    naive_ref_n, naive_ref_ms);
        for (long long N : sizes) {
            if (N > naive_cap) {
                const double r = static_cast<double>(N) / static_cast<double>(naive_ref_n);
                std::printf("  N=%-8lld ~%.0f ms/tick (~%.1f ticks/sec)\n",
                            N, naive_ref_ms * r * r,
                            1000.0 / (naive_ref_ms * r * r));
            }
        }
    }
    std::printf("\npeak RSS this run: %.1f MB\n", peak_mb());
    return 0;
}
