// memtable.hpp -- in-memory sorted store, the write buffer of the LSM.
//
// Every put() and delete() lands here first.  When the table reaches a
// configurable size threshold we flush() it to an immutable SSTable on
// disk.  A delete is recorded as a tombstone (the entry value carries
// a `is_tombstone` flag) so that reads finding the tombstone in the
// MemTable correctly mask any older SSTable value.
//
// Backed by std::map: O(log n) inserts and lookups, in-order iteration
// for the flush step.

#pragma once

#include <map>
#include <optional>
#include <string>

namespace lsm {

struct Entry {
    std::string  value;
    bool         is_tombstone = false;
};

class MemTable {
public:
    void put(std::string key, std::string value) {
        data_[std::move(key)] = Entry{std::move(value), false};
    }

    void del(std::string key) {
        data_[std::move(key)] = Entry{"", true};
    }

    // None  -> key not in this table   (caller must search older SSTables)
    // value -> key found.  is_tombstone flag tells caller it's deleted.
    std::optional<Entry> get(const std::string& key) const {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

    std::size_t size() const noexcept { return data_.size(); }
    bool        empty() const noexcept { return data_.empty(); }

    // Iteration in sorted order for flush.
    using const_iterator = std::map<std::string, Entry>::const_iterator;
    const_iterator begin() const noexcept { return data_.begin(); }
    const_iterator end()   const noexcept { return data_.end(); }

    void clear() { data_.clear(); }

private:
    std::map<std::string, Entry> data_;
};

}  // namespace lsm
