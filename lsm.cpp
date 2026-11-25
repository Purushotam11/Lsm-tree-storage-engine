// lsm.cpp -- coordinator implementation.

#include "lsm.hpp"

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lsm {

Database::Database(std::string dir, DBOptions opts)
    : dir_(std::move(dir)), opts_(opts)
{
    fs::create_directories(dir_);

    load_existing_sstables();

    wal_ = std::make_unique<Wal>((fs::path(dir_) / "wal.log").string());
    replay_wal();
}

void Database::put(std::string key, std::string value) {
    wal_->append(WalOp::Put, key, value);
    mem_.put(std::move(key), std::move(value));
    if (mem_.size() >= opts_.memtable_flush_threshold) flush();
}

void Database::del(std::string key) {
    wal_->append(WalOp::Delete, key, "");
    mem_.del(std::move(key));
    if (mem_.size() >= opts_.memtable_flush_threshold) flush();
}

std::optional<std::string> Database::get(const std::string& key) const {
    // 1. MemTable first -- newest possible value, including tombstone.
    if (auto e = mem_.get(key); e.has_value()) {
        if (e->is_tombstone) return std::nullopt;
        return e->value;
    }
    // 2. SSTables, newest-to-oldest.
    for (auto it = ssts_.rbegin(); it != ssts_.rend(); ++it) {
        if (auto e = (*it)->get(key); e.has_value()) {
            if (e->is_tombstone) return std::nullopt;
            return e->value;
        }
    }
    return std::nullopt;
}

void Database::flush() {
    if (mem_.empty()) return;
    const std::string path = next_sstable_path();
    SSTable::build(path, mem_);
    mem_.clear();
    wal_->truncate();
    ssts_.push_back(std::make_unique<SSTable>(path));
}

std::string Database::next_sstable_path() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "sst-%08llu.sst",
                  static_cast<unsigned long long>(next_sst_id_++));
    return (fs::path(dir_) / buf).string();
}

void Database::replay_wal() {
    const std::string wal_path = (fs::path(dir_) / "wal.log").string();
    if (!fs::exists(wal_path)) return;
    for (const auto& r : Wal::replay(wal_path)) {
        if (r.op == WalOp::Put) {
            mem_.put(r.key, r.value);
        } else {
            mem_.del(r.key);
        }
    }
}

void Database::load_existing_sstables() {
    // Match files named  sst-XXXXXXXX.sst  to recover ordering + max ID.
    const std::regex pattern("^sst-(\\d+)\\.sst$");
    std::vector<std::pair<std::uint64_t, std::string>> found;
    for (const auto& entry : fs::directory_iterator(dir_)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        std::smatch m;
        if (std::regex_match(name, m, pattern)) {
            std::uint64_t id = std::stoull(m[1]);
            found.emplace_back(id, entry.path().string());
        }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, path] : found) {
        ssts_.push_back(std::make_unique<SSTable>(path));
        next_sst_id_ = std::max(next_sst_id_, id + 1);
    }
}

}  // namespace lsm
