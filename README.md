# lsm-tree

A **Log-Structured Merge Tree** key-value store in C++20, written from
scratch -- the same on-disk design pattern used by **LevelDB, RocksDB,
Cassandra, ScyllaDB, TiKV, and HBase**.  Implements the write-optimized
storage layer in ~700 lines of clean header-mostly code, with a full
test suite + benchmark + crash-safe WAL replay.

```
$ make test
  13 passed, 0 failed (of 13)

$ make demo
  [1] open empty db
  [2] put 6 keys (auto-flush at 4) -> 1 SSTable on disk
  [3] read everything back (incl. cross-SSTable + memtable masking)
  [4] update + delete + flush       -> 2 SSTables, tombstone hides cherry
  [5] close + re-open (process restart) -> all data queryable
  [6] simulate crash mid-write       -> WAL replay recovers unflushed puts

$ make bench
  WRITES   103,660 ops/s   (100k keys + 98 SSTable flushes in 0.96 s)
  READS    p50 = 30 us   p95 = 258 us   p99 = 360 us   (across 98 SSTables)
```

## Why LSM-tree?

The LSM-tree is the answer to: *how do you get fast writes on a disk
with slow random I/O?*

  - Buffer all writes in **sorted memory** (the MemTable).
  - When the buffer is full, **flush it sequentially** to a new
    immutable sorted file on disk (an SSTable).
  - Reads walk the SSTables from newest to oldest, **using a Bloom
    filter per file** to skip most of them in O(1).
  - Background compaction (planned v0.2.0) merges SSTables to bound
    read amplification.

The result: **sequential writes** (fast on any storage including SSDs
and even rotating disks) instead of the random writes a B+tree would
do.  The trade-off is read amplification, which Bloom filters mitigate.

This repo is the storage layer that every production write-heavy DB
since 2007 (BigTable → Cassandra → DynamoDB → RocksDB → ...) is built on.

## Architecture

```
                +----------------+
   put(k, v) -->| 1. WAL append  |  (crash recovery)
                +----------------+
                        |
                        v
                +----------------+
                | 2. MemTable    |  (sorted in-memory, std::map)
                +----------------+
                        |
                        | when size >= threshold
                        v
                +----------------+
                | 3. SSTable     |  immutable sorted file with sparse
                +----------------+  index + Bloom filter trailer
                  oldest ... newest

   get(k) -->  check MemTable, then walk SSTables newest -> oldest
               with Bloom filter skipping in O(1) per file
```

### 1. WAL (write-ahead log) -- `src/wal.hpp`

Every put/delete is appended to `wal.log` BEFORE touching the MemTable.
The record carries a CRC-32 trailer so torn writes (partial records
from a power-loss mid-append) are detected and replay stops cleanly at
the last good record.

```
[op:1] [key_len:4] [key] [val_len:4] [val] [crc32:4]
```

Replay on startup rebuilds the MemTable to its pre-crash state.

### 2. MemTable -- `src/memtable.hpp`

`std::map<string, Entry>` -- sorted by key, O(log n) put/get.  An
entry carries a `is_tombstone` flag so deletes can mask older values
in older SSTables.

When the MemTable hits its size threshold (default 1024 entries), it
flushes to a new on-disk SSTable.  After a successful flush the WAL
is truncated -- the SSTable IS the durability.

### 3. SSTable (sorted string table) -- `src/sstable.hpp`

Immutable on-disk file with this layout:

```
[magic:4 "SST1"] [n_entries:8] [index_offset:8] [bloom_offset:8]
... data block (sorted by key) ...
    [key_len:4] [key] [val_len:4] [val] [is_tombstone:1]
    ...
... sparse index (1 entry per ~16 keys) ...
    [key_len:4] [key] [file_offset:8]
    ...
... Bloom filter ...
    [m_bits:8] [k_hashes:8] [bits...]
```

The sparse index keeps RAM usage O(N/16); a Bloom filter at the file
trailer lets us reject "key definitely not here" in one hash without
opening the data block.

### Reads -- the path that matters

A typical get walks:

1. **MemTable**.  If the key is present, return its value (or `none`
   if it's a tombstone).  This serves all writes from the current run.
2. **SSTables, newest-to-oldest.**  For each:
   a) Bloom filter `maybe_contains(key)`?  If no -> O(1) skip.
   b) Binary-search the sparse index for the greatest indexed key ≤ target.
   c) `fseek` to that offset, linear scan up to ~16 entries forward.

With Bloom FPR=1%, in a DB with 100 SSTables you scan ~1 file on average
per miss and exactly 1 file on a hit.  That's how DBs like RocksDB
survive a workload that creates thousands of SSTables.

## Tests (13/13 passing)

```
bloom_no_false_negatives             - 1k inserts, all queryable
bloom_fpr_close_to_target            - observed FPR within 3x of target (1%)
bloom_serialization_roundtrip        - to_bytes() / from_bytes()

empty_get_returns_none               - cold DB
put_and_get_round_trip               - basic
later_put_overwrites_earlier         - 3 puts to same key
delete_makes_key_invisible           - put -> del -> get = none

flush_creates_sstable_and_clears_memtable  - flush invariants
reopen_recovers_all_data             - close + open recovers 100 keys
wal_replay_recovers_unflushed_writes - crash without flush -> WAL replay

newer_sstable_masks_older            - 2 SSTables, newer wins
tombstone_in_newer_sstable_hides_older_value  - tombstone propagation

stress_5000_keys_with_reopens        - 5000 puts + 1/7 deletes,
                                       reopen, verify every key
```

## Benchmark

Workload: 100,000 random 10-byte keys with 20-byte values, threshold
1024 (so ~98 SSTable flushes happen during the run).  Read phase
probes 100,000 keys split 50/50 hits/misses.

| Operation                | Throughput     | Latency           |
|--------------------------|---------------:|-------------------|
| Sequential writes        | **103.6K ops/s** | -                  |
| Reads (50% hit, 98 SSTs) | 13.3K ops/s    | p50=30us p99=360us |

Notes:
  - Write path dominates: ~10us per insert including WAL fsync.
  - Read path is dominated by Bloom filter checks across 98 SSTables.
    Without Bloom each lookup would touch all 98 sparse indices.
  - **p99 is 12x p50** -- that's the bloom-rare event where the key
    appears live across multiple recent SSTables before the hit.

## Build & run

Needs a C++20 compiler.  Tested with `g++ 15.2` (MSYS2 UCRT64).

```bash
make            # builds demo, test, bench
make test       # 13/13 in <1s
make demo       # scripted walkthrough
make bench      # 100k put + 100k get throughput + latencies
```

Header-only structure means you can also single-shot compile any
target:

```bash
g++ -std=c++20 -O3 -Isrc src/lsm.cpp src/demo.cpp -o demo
```

## Layout

```
src/
  bloom.hpp       Bloom filter (Kirsch-Mitzenmacher 2006 double-hashing)
  wal.hpp         Write-ahead log with CRC32-detected torn writes
  memtable.hpp    Sorted in-memory store (std::map + tombstone flag)
  sstable.hpp     Immutable on-disk sorted file with sparse index + bloom
  lsm.hpp/cpp     Database coordinator (put / get / del / flush / reopen)
  demo.cpp        Scripted demo
tests/
  test_lsm.cpp    13 invariant tests covering bloom, persistence, WAL replay,
                  multi-SSTable masking, tombstones, 5000-key stress
bench/
  bench.cpp      Write throughput + read latency (p50/p95/p99)
Makefile
```

## What's intentionally NOT here yet

This is the **MVP storage engine**.  Production-grade extensions on
the roadmap:

  - **Leveled compaction** (LCS).  Merge L0 SSTables into L1, L1 into
    L2, ... to bound the number of files a read scans.  Brings read
    cost back from O(N_sstables) to O(log N).
  - **Snapshot isolation + iterators**.  Range scans across MemTable
    + multiple SSTables in sorted order via merge iterator.
  - **Block cache + LRU eviction**.  Right now every SSTable read is
    an fopen.  A page cache would push read p50 below 10us.
  - **Async / threaded compaction**.  Currently everything is single-
    threaded.  RocksDB-style background compaction threads next.
  - **Configurable block compression** (Snappy / ZSTD) on data blocks.
  - **Atomic batch writes** (multi-key transactions in one WAL record).

These are sketched in the v0.2.0 / v0.3.0 milestones.

## References

- **O'Neil, P. et al. (1996).**  *The Log-Structured Merge-Tree (LSM-Tree).*
  Acta Informatica 33(4): 351-385.  (The original paper.)
- **Chang, F. et al. (2006).**  *Bigtable: A Distributed Storage System
  for Structured Data.*  OSDI.  (Google's first deployed LSM-tree at
  scale; introduced the SSTable terminology.)
- **Sears, R. & Ramakrishnan, R. (2012).**  *bLSM: A General Purpose
  Log Structured Merge Tree.*  SIGMOD.  (Modern leveled-compaction
  analysis.)
- **Mitzenmacher, M. & Upfal, E. (2005).**  Bloom filter sizing
  formulas from *Probability and Computing*, Ch. 5.
- **Kirsch, A. & Mitzenmacher, M. (2006).**  *Less Hashing, Same
  Performance: Building a Better Bloom Filter.*  RSA 33(2): 187-218.
  (The double-hashing trick used in `bloom.hpp`.)

## Related repos

  - [`scrape-arsenal`](https://github.com/sauloduttra/scrape-arsenal) --
    the Python Bloom filter that `bloom.hpp` is the port of.
  - [`lob-engine`](https://github.com/sauloduttra/lob-engine) -- single-
    threaded matching engine, similar architectural shape (sorted
    in-memory data structures + careful memory layout).
