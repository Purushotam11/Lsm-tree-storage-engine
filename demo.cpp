// demo.cpp -- scripted walkthrough of the LSM Database.

#include "lsm.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using namespace lsm;

static void print_get(const Database& db, const std::string& key) {
    auto v = db.get(key);
    if (v.has_value()) std::cout << "  get(" << key << ") = \"" << *v << "\"\n";
    else               std::cout << "  get(" << key << ") = <none>\n";
}

int main() {
    const std::string dir = "lsm-demo";
    if (fs::exists(dir)) fs::remove_all(dir);

    std::cout << "================================================================\n";
    std::cout << "  lsm-tree demo\n";
    std::cout << "================================================================\n\n";

    {
        std::cout << "[1] open empty db at ./" << dir << "/\n";
        Database db(dir, {.memtable_flush_threshold = 4});

        std::cout << "[2] put 6 keys (threshold=4 -> auto-flush at 4)\n";
        db.put("apple", "1");
        db.put("banana", "2");
        db.put("cherry", "3");
        db.put("date", "4");                  // <-- this triggers flush
        std::cout << "    sstables on disk:  " << db.sstable_count() << "\n";
        std::cout << "    memtable entries:  " << db.memtable_size() << "\n";

        db.put("elder", "5");
        db.put("fig",   "6");

        std::cout << "\n[3] read everything back\n";
        for (const auto& k : {"apple", "banana", "cherry", "date", "elder", "fig", "grape"})
            print_get(db, k);

        std::cout << "\n[4] update apple, delete cherry, then flush\n";
        db.put("apple", "100");
        db.del("cherry");
        db.flush();
        std::cout << "    sstables on disk:  " << db.sstable_count() << "\n";
        for (const auto& k : {"apple", "banana", "cherry", "date"})
            print_get(db, k);
    }

    std::cout << "\n[5] close and re-open  (simulates a process restart)\n";
    {
        Database db(dir);
        std::cout << "    sstables on disk:  " << db.sstable_count() << "\n";
        std::cout << "    -- all data must still be queryable --\n";
        for (const auto& k : {"apple", "banana", "cherry", "date", "elder", "fig"})
            print_get(db, k);
    }

    std::cout << "\n[6] simulate a crash mid-write (WAL not yet flushed to SSTable)\n";
    {
        Database db(dir, {.memtable_flush_threshold = 1000});  // big threshold -> no auto flush
        db.put("zulu", "z1");
        db.put("yankee", "y1");
        // No explicit flush -- next open() must replay WAL to recover these.
    }
    {
        Database db(dir);
        std::cout << "    after restart, WAL replayed -> these should still exist:\n";
        print_get(db, "zulu");
        print_get(db, "yankee");
    }

    std::cout << "\n[done]\n";
    return 0;
}
