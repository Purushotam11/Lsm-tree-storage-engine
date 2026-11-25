// test_lsm.cpp -- invariants for the LSM Database.
//
// Each test is registered via a TEST() macro and runs in its own
// temporary directory.  Assert-based, no framework.

#include "../src/lsm.hpp"
#include "../src/bloom.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace lsm;

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase> g_tests;

#define TEST(name)                                       \
    static void name();                                  \
    static int reg_##name = (g_tests.push_back({#name, &name}), 0); \
    static void name()


// Helper: unique scratch directory per test
static std::string scratch_dir(const std::string& tag) {
    auto d = fs::temp_directory_path() / ("lsm-test-" + tag);
    if (fs::exists(d)) fs::remove_all(d);
    return d.string();
}


// ---------------------------------------------------------------------
//  Bloom filter
// ---------------------------------------------------------------------

TEST(bloom_no_false_negatives) {
    auto b = Bloom::optimal(1000, 0.01);
    for (int i = 0; i < 1000; ++i) b.add("key-" + std::to_string(i));
    for (int i = 0; i < 1000; ++i) assert(b.maybe_contains("key-" + std::to_string(i)));
}

TEST(bloom_fpr_close_to_target) {
    const std::size_t n = 5000;
    const double target = 0.01;
    auto b = Bloom::optimal(n, target);
    for (std::size_t i = 0; i < n; ++i) b.add("in-" + std::to_string(i));
    std::size_t fp = 0;
    const std::size_t probes = n * 10;
    for (std::size_t i = 0; i < probes; ++i) {
        if (b.maybe_contains("out-" + std::to_string(i))) ++fp;
    }
    const double observed = static_cast<double>(fp) / probes;
    // Allow generous slack -- bound is asymptotic
    assert(observed < target * 3.0);
}

TEST(bloom_serialization_roundtrip) {
    auto b = Bloom::optimal(500, 0.05);
    for (int i = 0; i < 200; ++i) b.add("k" + std::to_string(i));
    auto blob = b.to_bytes();
    auto b2 = Bloom::from_bytes(blob.data(), blob.size());
    for (int i = 0; i < 200; ++i) assert(b2.maybe_contains("k" + std::to_string(i)));
}


// ---------------------------------------------------------------------
//  Basic put / get / delete
// ---------------------------------------------------------------------

TEST(empty_get_returns_none) {
    auto dir = scratch_dir("empty");
    Database db(dir);
    assert(!db.get("anything").has_value());
}

TEST(put_and_get_round_trip) {
    auto dir = scratch_dir("put-get");
    Database db(dir);
    db.put("hello", "world");
    auto v = db.get("hello");
    assert(v.has_value());
    assert(*v == "world");
}

TEST(later_put_overwrites_earlier) {
    auto dir = scratch_dir("overwrite");
    Database db(dir);
    db.put("k", "v1");
    db.put("k", "v2");
    db.put("k", "v3");
    auto v = db.get("k");
    assert(*v == "v3");
}

TEST(delete_makes_key_invisible) {
    auto dir = scratch_dir("delete");
    Database db(dir);
    db.put("k", "v");
    db.del("k");
    assert(!db.get("k").has_value());
}


// ---------------------------------------------------------------------
//  Persistence (flush + reopen)
// ---------------------------------------------------------------------

TEST(flush_creates_sstable_and_clears_memtable) {
    auto dir = scratch_dir("flush");
    Database db(dir, {.memtable_flush_threshold = 1000});
    for (int i = 0; i < 100; ++i)
        db.put("k" + std::to_string(i), "v" + std::to_string(i));
    assert(db.memtable_size() == 100);
    assert(db.sstable_count() == 0);
    db.flush();
    assert(db.memtable_size() == 0);
    assert(db.sstable_count() == 1);
    // After flush, every key still readable through the SSTable
    for (int i = 0; i < 100; ++i) {
        auto v = db.get("k" + std::to_string(i));
        assert(v.has_value() && *v == "v" + std::to_string(i));
    }
}

TEST(reopen_recovers_all_data) {
    auto dir = scratch_dir("reopen");
    {
        Database db(dir, {.memtable_flush_threshold = 30});
        for (int i = 0; i < 100; ++i)
            db.put("kk" + std::to_string(i), "vv" + std::to_string(i));
        db.flush();          // pin remaining memtable
    }
    Database db(dir);
    for (int i = 0; i < 100; ++i) {
        auto v = db.get("kk" + std::to_string(i));
        assert(v.has_value() && *v == "vv" + std::to_string(i));
    }
}

TEST(wal_replay_recovers_unflushed_writes) {
    auto dir = scratch_dir("wal-replay");
    {
        // High threshold so MemTable never flushes during this block.
        Database db(dir, {.memtable_flush_threshold = 10000});
        db.put("alpha", "1");
        db.put("beta",  "2");
        db.del("alpha");
        // ~~ process "crashes" here -- destructor runs but no flush happens
    }
    Database db(dir);
    // WAL replay must have re-applied the put+put+delete sequence
    assert(!db.get("alpha").has_value());
    assert(db.get("beta").has_value() && *db.get("beta") == "2");
}


// ---------------------------------------------------------------------
//  Multi-SSTable read path (newer masks older)
// ---------------------------------------------------------------------

TEST(newer_sstable_masks_older) {
    auto dir = scratch_dir("masking");
    Database db(dir, {.memtable_flush_threshold = 1000});
    db.put("k", "old");
    db.flush();
    db.put("k", "new");
    db.flush();
    assert(db.sstable_count() == 2);
    auto v = db.get("k");
    assert(v.has_value() && *v == "new");
}

TEST(tombstone_in_newer_sstable_hides_older_value) {
    auto dir = scratch_dir("tombstone-multi");
    Database db(dir, {.memtable_flush_threshold = 1000});
    db.put("k", "alive");
    db.flush();
    db.del("k");
    db.flush();
    assert(db.sstable_count() == 2);
    assert(!db.get("k").has_value());
}


// ---------------------------------------------------------------------
//  Stress: many keys with mix of operations
// ---------------------------------------------------------------------

TEST(stress_5000_keys_with_reopens) {
    auto dir = scratch_dir("stress");
    const int N = 5000;
    {
        Database db(dir, {.memtable_flush_threshold = 200});
        for (int i = 0; i < N; ++i)
            db.put("key-" + std::to_string(i), "val-" + std::to_string(i));
        // Delete every 7th key
        for (int i = 0; i < N; i += 7) db.del("key-" + std::to_string(i));
        db.flush();
    }
    // Reopen and verify
    Database db(dir);
    for (int i = 0; i < N; ++i) {
        const std::string k = "key-" + std::to_string(i);
        auto v = db.get(k);
        if (i % 7 == 0) {
            assert(!v.has_value());
        } else {
            assert(v.has_value() && *v == "val-" + std::to_string(i));
        }
    }
}


// ---------------------------------------------------------------------
//  Runner
// ---------------------------------------------------------------------

int main() {
    int passed = 0, failed = 0;
    for (const auto& t : g_tests) {
        try {
            t.fn();
            std::printf("  [ok]   %s\n", t.name);
            ++passed;
        } catch (const std::exception& e) {
            std::printf("  [FAIL] %s -- %s\n", t.name, e.what());
            ++failed;
        } catch (...) {
            std::printf("  [FAIL] %s -- unknown exception\n", t.name);
            ++failed;
        }
    }
    std::printf("\n  %d passed, %d failed (of %zu)\n",
                passed, failed, g_tests.size());
    return failed == 0 ? 0 : 1;
}
