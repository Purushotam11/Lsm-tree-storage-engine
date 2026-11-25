// wal.hpp -- write-ahead log for crash recovery.
//
// Every put() and delete() is FIRST appended to this log, then applied
// to the in-memory MemTable.  After a crash, we replay the log to
// reconstruct the MemTable to its pre-crash state.
//
// Record format (little-endian throughout):
//
//   [op:1]       1 = put,   2 = delete
//   [key_len:4]
//   [key bytes]
//   [val_len:4]  (0 for delete)
//   [val bytes]  (empty for delete)
//   [crc32:4]    of the bytes above this position
//
// CRC catches torn writes -- if the last record is partial (interrupted
// power loss), replay stops at the last good record.

#pragma once

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lsm {

enum class WalOp : std::uint8_t {
    Put    = 1,
    Delete = 2,
};

struct WalRecord {
    WalOp        op;
    std::string  key;
    std::string  value;     // empty for Delete
};

// Polynomial CRC-32 (IEEE 802.3 standard polynomial, table-free).  Sufficient
// to detect torn writes; not for cryptographic integrity.
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int j = 0; j < 8; ++j) {
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
        }
    }
    return c ^ 0xFFFFFFFFu;
}

class Wal {
public:
    explicit Wal(std::string path) : path_(std::move(path)) {
        out_.open(path_, std::ios::binary | std::ios::app);
        if (!out_.is_open()) {
            throw std::runtime_error("Wal: cannot open " + path_);
        }
    }

    ~Wal() {
        if (out_.is_open()) out_.close();
    }

    void append(WalOp op, std::string_view key, std::string_view value) {
        std::vector<std::uint8_t> buf;
        buf.reserve(1 + 4 + key.size() + 4 + value.size());
        buf.push_back(static_cast<std::uint8_t>(op));
        write_u32(buf, static_cast<std::uint32_t>(key.size()));
        buf.insert(buf.end(), key.begin(), key.end());
        write_u32(buf, static_cast<std::uint32_t>(value.size()));
        buf.insert(buf.end(), value.begin(), value.end());

        const std::uint32_t c = crc32(buf.data(), buf.size());
        write_u32(buf, c);

        out_.write(reinterpret_cast<const char*>(buf.data()),
                   static_cast<std::streamsize>(buf.size()));
        out_.flush();    // ensure record hits the kernel; durability depends on fsync
    }

    void truncate() {
        out_.close();
        std::ofstream clear(path_, std::ios::binary | std::ios::trunc);
        clear.close();
        out_.open(path_, std::ios::binary | std::ios::app);
    }

    // Read all records back in order.  Stops cleanly on a torn/short tail
    // (so replay is safe after a crash mid-write).
    static std::vector<WalRecord> replay(const std::string& path) {
        std::vector<WalRecord> out;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return out;     // no log = empty replay

        while (true) {
            char op_byte;
            if (!in.read(&op_byte, 1)) break;
            const auto op = static_cast<WalOp>(static_cast<std::uint8_t>(op_byte));
            std::uint32_t klen, vlen;
            if (!read_u32(in, klen)) break;
            std::string key(klen, '\0');
            if (klen > 0 && !in.read(key.data(), klen)) break;
            if (!read_u32(in, vlen)) break;
            std::string val(vlen, '\0');
            if (vlen > 0 && !in.read(val.data(), vlen)) break;
            std::uint32_t want;
            if (!read_u32(in, want)) break;

            // Recompute crc on the recorded bytes
            std::vector<std::uint8_t> buf;
            buf.reserve(1 + 4 + klen + 4 + vlen);
            buf.push_back(static_cast<std::uint8_t>(op));
            write_u32(buf, klen);
            buf.insert(buf.end(), key.begin(), key.end());
            write_u32(buf, vlen);
            buf.insert(buf.end(), val.begin(), val.end());
            const std::uint32_t got = crc32(buf.data(), buf.size());
            if (got != want) break;        // torn record -- stop here

            out.push_back(WalRecord{op, std::move(key), std::move(val)});
        }
        return out;
    }

    const std::string& path() const noexcept { return path_; }

private:
    static void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    static bool read_u32(std::ifstream& in, std::uint32_t& v) {
        char buf[4];
        if (!in.read(buf, 4)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[i])) << (8 * i);
        }
        return true;
    }

    std::string  path_;
    std::ofstream out_;
};

}  // namespace lsm
