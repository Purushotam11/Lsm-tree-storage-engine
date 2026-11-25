// lsm.hpp -- public LSM-tree Database API.
//
// Operations
//   put(k, v)    : writes WAL + adds to MemTable.  flushes to a new
//                  SSTable on disk when MemTable hits the threshold.
//   get(k)       : checks MemTable first; on miss walks SSTables
//                  newest-to-oldest (Bloom-skipped).
//   del(k)       : writes a tombstone (a delete is just a special put).
//
// Recovery
//   open(dir)    : enumerates *.sst files in `dir` to rebuild the
//                  on-disk view, then replays the WAL to rebuild the
//                  in-memory MemTable to its pre-crash state.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "memtable.hpp"
#include "sstable.hpp"
#include "wal.hpp"

namespace lsm {

struct DBOptions {
    std::size_t memtable_flush_threshold = 1024;   // entries before flush
};

class Database {
public:
    explicit Database(std::string dir, DBOptions opts = {});

    void put(std::string key, std::string value);
    void del(std::string key);
    std::optional<std::string> get(const std::string& key) const;

    void flush();      // explicit flush of MemTable to a new SSTable

    // Stats
    std::size_t memtable_size() const noexcept { return mem_.size(); }
    std::size_t sstable_count() const noexcept { return ssts_.size(); }

private:
    std::string next_sstable_path();
    void        replay_wal();
    void        load_existing_sstables();

    std::string                                dir_;
    DBOptions                                  opts_;
    MemTable                                   mem_;
    std::unique_ptr<Wal>                       wal_;
    // SSTables in the order they were flushed (newest LAST in the vector).
    // get() walks back-to-front so newer values mask older ones.
    std::vector<std::unique_ptr<SSTable>>      ssts_;
    std::uint64_t                              next_sst_id_ = 0;
};

}  // namespace lsm
