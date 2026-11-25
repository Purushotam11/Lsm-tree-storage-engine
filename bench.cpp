// bench.cpp -- write throughput + read latency benchmark.
//
// Workload:
//   1. PUT N random keys (10-char ASCII) with 20-byte values
//   2. GET N keys: half hit (existing), half miss (random)
//
// Reports ops/sec for writes, median + p99 latency for reads.

#include "../src/lsm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace lsm;
using Clock = std::chrono::high_resolution_clock;

static std::string random_string(std::mt19937& rng, std::size_t n) {
    static constexpr char ALPHABET[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(ALPHABET) - 2);
    std::string s(n, '\0');
    for (auto& c : s) c = ALPHABET[dist(rng)];
    return s;
}

int main(int argc, char** argv) {
    std::size_t N = 100'000;
    if (argc > 1) N = std::stoull(argv[1]);

    const auto dir = (fs::temp_directory_path() / "lsm-bench").string();
    if (fs::exists(dir)) fs::remove_all(dir);

    std::printf("== lsm-tree benchmark ==\n");
    std::printf("  inserting %zu keys (10-char) with 20-byte values\n", N);
    std::printf("  memtable_flush_threshold = 1024 -> ~%zu SSTables expected\n\n", N / 1024);

    std::mt19937 rng(42);
    std::vector<std::string> keys; keys.reserve(N);
    for (std::size_t i = 0; i < N; ++i) keys.push_back(random_string(rng, 10));
    const std::string value(20, 'v');

    Database db(dir, {.memtable_flush_threshold = 1024});

    // --- Writes ---
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < N; ++i) db.put(keys[i], value);
    db.flush();
    auto t1 = Clock::now();
    const double w_sec = std::chrono::duration<double>(t1 - t0).count();

    // --- Reads ---
    // Half hits (sampled from inserted keys), half misses (fresh random)
    std::vector<std::string> probes; probes.reserve(N);
    std::uniform_int_distribution<std::size_t> sel(0, N - 1);
    for (std::size_t i = 0; i < N / 2; ++i) probes.push_back(keys[sel(rng)]);
    for (std::size_t i = 0; i < N / 2; ++i) probes.push_back(random_string(rng, 10));
    std::shuffle(probes.begin(), probes.end(), rng);

    std::vector<double> latencies; latencies.reserve(probes.size());
    std::size_t hits = 0;
    auto t2 = Clock::now();
    for (const auto& k : probes) {
        auto a = Clock::now();
        auto v = db.get(k);
        auto b = Clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(b - a).count());
        if (v.has_value()) ++hits;
    }
    auto t3 = Clock::now();
    const double r_sec = std::chrono::duration<double>(t3 - t2).count();

    std::sort(latencies.begin(), latencies.end());
    const double p50 = latencies[latencies.size() / 2];
    const double p95 = latencies[static_cast<std::size_t>(0.95 * (latencies.size() - 1))];
    const double p99 = latencies[static_cast<std::size_t>(0.99 * (latencies.size() - 1))];

    std::printf("  WRITES\n");
    std::printf("    time           : %.4f s\n", w_sec);
    std::printf("    throughput     : %.2f K ops/s\n", N / w_sec / 1e3);
    std::printf("    SSTables flushed: %zu\n", db.sstable_count());
    std::printf("\n");
    std::printf("  READS (%zu probes, %.0f%% hit rate)\n",
                probes.size(), 100.0 * hits / probes.size());
    std::printf("    total time     : %.4f s\n", r_sec);
    std::printf("    throughput     : %.2f K ops/s\n", probes.size() / r_sec / 1e3);
    std::printf("    p50 latency    : %.2f us\n", p50);
    std::printf("    p95 latency    : %.2f us\n", p95);
    std::printf("    p99 latency    : %.2f us\n", p99);
    std::printf("\n  data dir: %s\n", dir.c_str());

    return 0;
}
