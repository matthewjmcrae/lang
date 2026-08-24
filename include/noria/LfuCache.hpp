#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace noria {

  template <typename Key, typename Value> class LfuCache {
  public:
    LfuCache(std::size_t maxEntries, std::size_t maxWeight)
        : maxEntries_(maxEntries), maxWeight_(maxWeight) {}

    template <typename Clone>
    auto get(const Key& key, Clone clone)
        -> std::optional<std::invoke_result_t<Clone, const Value&>> {
      const auto found = entries_.find(key);
      if (found == entries_.end()) {
        return std::nullopt;
      }

      ++found->second.frequency;
      found->second.lastUsedTick = ++tick_;
      return clone(found->second.value);
    }

    bool put(Key key, Value value, std::size_t weight) {
      if (maxEntries_ == 0 || maxWeight_ == 0 || weight > maxWeight_) {
        return false;
      }

      const auto existing = entries_.find(key);
      if (existing != entries_.end()) {
        totalWeight_ -= existing->second.weight;
        entries_.erase(existing);
      }

      Entry entry;
      entry.value = std::move(value);
      entry.weight = weight;
      entry.frequency = 1;
      entry.lastUsedTick = ++tick_;
      totalWeight_ += weight;
      entries_.emplace(std::move(key), std::move(entry));
      evictToCapacity();
      return entries_.size() <= maxEntries_ && totalWeight_ <= maxWeight_;
    }

    bool contains(const Key& key) const { return entries_.find(key) != entries_.end(); }

    std::size_t size() const { return entries_.size(); }
    std::size_t totalWeight() const { return totalWeight_; }
    std::size_t maxEntries() const { return maxEntries_; }
    std::size_t maxWeight() const { return maxWeight_; }

    void clear() {
      entries_.clear();
      totalWeight_ = 0;
      tick_ = 0;
    }

  private:
    struct Entry {
      Value value;
      std::size_t weight = 0;
      std::size_t frequency = 0;
      std::uint64_t lastUsedTick = 0;
    };

    void evictToCapacity() {
      while (entries_.size() > maxEntries_ || totalWeight_ > maxWeight_) {
        evictOne();
      }
    }

    void evictOne() {
      auto victim = entries_.begin();
      for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        if (iterator->second.frequency < victim->second.frequency ||
            (iterator->second.frequency == victim->second.frequency &&
             iterator->second.lastUsedTick < victim->second.lastUsedTick)) {
          victim = iterator;
        }
      }

      totalWeight_ -= victim->second.weight;
      entries_.erase(victim);
    }

    std::size_t maxEntries_ = 0;
    std::size_t maxWeight_ = 0;
    std::size_t totalWeight_ = 0;
    std::uint64_t tick_ = 0;
    std::unordered_map<Key, Entry> entries_;
  };

} // namespace noria
