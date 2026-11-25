// sstable.hpp -- sorted string table (immutable on-disk format).
//
// File layout (little-endian throughout):
//
//   [magic:4]               "SST1"
//   [n_entries:8]
//   [index_offset:8]
//   [bloom_offset:8]
//   ...data block (sorted by key)...
//     [key_len:4][key][val_len:4][val][is_tombstone:1]
//   ...sparse index (~1 entry per INDEX_INTERVAL keys)...
//     [key_len:4][key][file_offset:8]
//   ...bloom filter bytes...
//
// The sparse index is small (one entry per ~16 data entries) and is
// loaded fully into memory at open time.  Reads binary-search the
// index, then scan forward in the data block.

#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "bloom.hpp"
#include "memtable.hpp"

namespace lsm {

inline constexpr std::uint32_t SSTABLE_MAGIC = 0x31545353u;  // "SST1" little-endian
inline constexpr std::size_t   INDEX_INTERVAL = 16;            // sparse index density

struct SSTableIndexEntry {
    std::string   key;
    std::uint64_t offset;
};

class SSTable {
public:
    // Build a new SSTable on disk from a sorted MemTable.
    static void build(const std::string& path, const MemTable& mt) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) throw std::runtime_error("SSTable::build: cannot open " + path);

        // Reserve header space
        std::uint64_t n = mt.size();
        write_u32(out, SSTABLE_MAGIC);
        write_u64(out, n);
        write_u64(out, 0);      // placeholder index_offset
        write_u64(out, 0);      // placeholder bloom_offset

        // Bloom sized for the table; FPR 1% gives ~9.6 bits/key.
        Bloom bloom = Bloom::optimal(std::max<std::size_t>(64, n), 0.01);

        std::vector<SSTableIndexEntry> sparse;
        std::size_t i = 0;
        for (const auto& [key, entry] : mt) {
            const std::uint64_t offset = static_cast<std::uint64_t>(out.tellp());
            if (i % INDEX_INTERVAL == 0) sparse.push_back({key, offset});
            write_u32(out, static_cast<std::uint32_t>(key.size()));
            out.write(key.data(), static_cast<std::streamsize>(key.size()));
            write_u32(out, static_cast<std::uint32_t>(entry.value.size()));
            out.write(entry.value.data(), static_cast<std::streamsize>(entry.value.size()));
            const std::uint8_t tomb = entry.is_tombstone ? 1u : 0u;
            out.write(reinterpret_cast<const char*>(&tomb), 1);
            bloom.add(key);
            ++i;
        }

        // Write the sparse index
        const std::uint64_t index_offset = static_cast<std::uint64_t>(out.tellp());
        for (const auto& e : sparse) {
            write_u32(out, static_cast<std::uint32_t>(e.key.size()));
            out.write(e.key.data(), static_cast<std::streamsize>(e.key.size()));
            write_u64(out, e.offset);
        }

        // Write the bloom filter
        const std::uint64_t bloom_offset = static_cast<std::uint64_t>(out.tellp());
        const auto blob = bloom.to_bytes();
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));

        // Backfill the header offsets
        out.seekp(4 + 8);       // after magic + n
        write_u64(out, index_offset);
        write_u64(out, bloom_offset);
    }

    explicit SSTable(std::string path) : path_(std::move(path)) {
        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) throw std::runtime_error("SSTable: cannot open " + path_);

        const std::uint32_t magic = read_u32(in);
        if (magic != SSTABLE_MAGIC) throw std::runtime_error("SSTable: bad magic");
        n_entries_     = read_u64(in);
        index_offset_  = read_u64(in);
        bloom_offset_  = read_u64(in);

        // Read sparse index
        in.seekg(static_cast<std::streamoff>(index_offset_));
        std::uint64_t bytes_to_read = bloom_offset_ - index_offset_;
        while (bytes_to_read > 0) {
            std::uint32_t klen = read_u32(in);
            std::string key(klen, '\0');
            in.read(key.data(), klen);
            std::uint64_t off = read_u64(in);
            sparse_.push_back({std::move(key), off});
            bytes_to_read -= (4 + klen + 8);
        }

        // Read bloom
        in.seekg(static_cast<std::streamoff>(bloom_offset_));
        std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
        bloom_ = Bloom::from_bytes(blob.data(), blob.size());
    }

    // None -> key definitely not in this SSTable
    // {value, is_tombstone=true}  -> key was deleted in this SSTable
    // {value, is_tombstone=false} -> live value
    std::optional<Entry> get(const std::string& key) const {
        if (!bloom_.maybe_contains(key)) return std::nullopt;

        // Binary-search the sparse index: greatest key <= target
        auto it = std::upper_bound(sparse_.begin(), sparse_.end(), key,
                                   [](const std::string& k, const SSTableIndexEntry& e) {
                                       return k < e.key;
                                   });
        if (it == sparse_.begin()) return std::nullopt;
        --it;
        std::uint64_t scan_offset = it->offset;
        std::uint64_t scan_end    = index_offset_;
        if ((it + 1) != sparse_.end()) scan_end = (it + 1)->offset;

        std::ifstream in(path_, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(scan_offset));
        while (static_cast<std::uint64_t>(in.tellg()) < scan_end) {
            const std::uint32_t klen = read_u32(in);
            std::string k(klen, '\0');
            in.read(k.data(), klen);
            const std::uint32_t vlen = read_u32(in);
            std::string v(vlen, '\0');
            in.read(v.data(), vlen);
            std::uint8_t tomb;
            in.read(reinterpret_cast<char*>(&tomb), 1);
            if (k == key) return Entry{std::move(v), tomb != 0};
            if (k > key)  return std::nullopt;     // sorted -- not present
        }
        return std::nullopt;
    }

    std::uint64_t       n_entries() const noexcept { return n_entries_; }
    const std::string&  path()      const noexcept { return path_; }
    std::size_t         bloom_bytes() const noexcept { return bloom_.size_bytes(); }

private:
    static void write_u32(std::ofstream& o, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = static_cast<std::uint8_t>(v >> (8 * i));
            o.write(reinterpret_cast<const char*>(&b), 1);
        }
    }
    static void write_u64(std::ofstream& o, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            std::uint8_t b = static_cast<std::uint8_t>(v >> (8 * i));
            o.write(reinterpret_cast<const char*>(&b), 1);
        }
    }
    static std::uint32_t read_u32(std::ifstream& in) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b;
            in.read(reinterpret_cast<char*>(&b), 1);
            v |= static_cast<std::uint32_t>(b) << (8 * i);
        }
        return v;
    }
    static std::uint64_t read_u64(std::ifstream& in) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            std::uint8_t b;
            in.read(reinterpret_cast<char*>(&b), 1);
            v |= static_cast<std::uint64_t>(b) << (8 * i);
        }
        return v;
    }

    std::string  path_;
    std::uint64_t n_entries_    = 0;
    std::uint64_t index_offset_ = 0;
    std::uint64_t bloom_offset_ = 0;
    std::vector<SSTableIndexEntry> sparse_;
    Bloom bloom_;
};

}  // namespace lsm
