// bloom.hpp -- minimal Bloom filter for SSTable key membership.
//
// Design follows Mitzenmacher & Upfal (2005) for optimal sizing and
// Kirsch-Mitzenmacher (2006) double-hashing for k hash functions from
// two 64-bit splits of a single hash.  This is the same construction
// as the Python version in scrape-arsenal (arsenal/bloom.py), ported
// to be embeddable inside the on-disk SSTable file format.
//
// Storage layout (matches the serialized form):
//   [m_bits:8 le][k_hashes:8 le][bits...]

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

class Bloom {
public:
    // Empty filter (for deserialization).
    Bloom() = default;

    // Build a filter sized for `capacity` items at false-positive rate `fpr`.
    static Bloom optimal(std::size_t capacity, double fpr) {
        if (capacity == 0)            throw std::invalid_argument("capacity must be > 0");
        if (fpr <= 0.0 || fpr >= 1.0) throw std::invalid_argument("fpr must be in (0,1)");
        const double m = std::ceil(-static_cast<double>(capacity) * std::log(fpr)
                                   / (std::log(2.0) * std::log(2.0)));
        const std::size_t mb = static_cast<std::size_t>(m);
        const std::size_t kb = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::round((mb / static_cast<double>(capacity)) * std::log(2.0))));
        Bloom b;
        b.m_ = mb;
        b.k_ = kb;
        b.bits_.assign((mb + 7) / 8, 0);
        return b;
    }

    void add(std::string_view key) {
        std::uint64_t h1, h2;
        hash128(key, h1, h2);
        for (std::size_t i = 0; i < k_; ++i) {
            const std::size_t pos = (h1 + i * h2) % m_;
            bits_[pos >> 3] |= static_cast<std::uint8_t>(1u << (pos & 7));
        }
    }

    bool maybe_contains(std::string_view key) const {
        if (m_ == 0) return true;             // empty filter: no info -> assume yes
        std::uint64_t h1, h2;
        hash128(key, h1, h2);
        for (std::size_t i = 0; i < k_; ++i) {
            const std::size_t pos = (h1 + i * h2) % m_;
            if (!(bits_[pos >> 3] & (1u << (pos & 7)))) return false;
        }
        return true;
    }

    // Serialization to a byte vector (for embedding in SSTable).
    std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> out;
        out.reserve(16 + bits_.size());
        write_u64(out, m_);
        write_u64(out, k_);
        out.insert(out.end(), bits_.begin(), bits_.end());
        return out;
    }

    static Bloom from_bytes(const std::uint8_t* data, std::size_t n) {
        if (n < 16) throw std::runtime_error("bloom blob too small");
        Bloom b;
        b.m_ = read_u64(data + 0);
        b.k_ = read_u64(data + 8);
        const std::size_t bytes_needed = (b.m_ + 7) / 8;
        if (n != 16 + bytes_needed) {
            throw std::runtime_error("bloom blob size mismatch");
        }
        b.bits_.assign(data + 16, data + 16 + bytes_needed);
        return b;
    }

    std::size_t size_bits()  const noexcept { return m_; }
    std::size_t size_bytes() const noexcept { return bits_.size(); }
    std::size_t k_hashes()   const noexcept { return k_; }

private:
    // FNV-1a 64-bit then split for two independent hash values.
    // Kirsch-Mitzenmacher 2006: k positions = h1 + i*h2 mod m.
    static void hash128(std::string_view s, std::uint64_t& h1, std::uint64_t& h2) {
        constexpr std::uint64_t FNV_OFFSET_64 = 14695981039346656037ULL;
        constexpr std::uint64_t FNV_PRIME_64  = 1099511628211ULL;
        std::uint64_t a = FNV_OFFSET_64;
        for (unsigned char c : s) {
            a ^= c;
            a *= FNV_PRIME_64;
        }
        // Second hash: same algorithm, different seed (rotate constants).
        std::uint64_t b = FNV_OFFSET_64 ^ 0xCBF29CE484222325ULL;
        for (unsigned char c : s) {
            b *= FNV_PRIME_64;
            b ^= c;
        }
        h1 = a;
        h2 = b;
        if (h2 == 0) h2 = 1;       // guard against zero step
    }

    static void write_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    static std::uint64_t read_u64(const std::uint8_t* p) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        return v;
    }

    std::size_t            m_ = 0;
    std::size_t            k_ = 0;
    std::vector<std::uint8_t> bits_;
};

}  // namespace lsm
