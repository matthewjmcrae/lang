#pragma once

#include "noria/HashTable.hpp"

#include <cstddef>
#include <iterator>
#include <list>
#include <optional>
#include <type_traits>
#include <utility>

namespace noria {

  template <typename Key, typename Value> class LFUCache {
  public:
    explicit LFUCache(std::size_t maxEntries) : maxEntries_(maxEntries) {
      keyNode_.reserve(maxEntries_);
      frequencyNode_.reserve(maxEntries_);
    }

    template <typename Clone>
    auto get(const Key& key, Clone clone)
        -> std::optional<std::invoke_result_t<Clone, const Value&>> {
      const auto found = keyNode_.find(key);
      if (found == keyNode_.end()) {
        return std::nullopt;
      }

      auto source = found->second.node;
      auto result = clone(found->second.entry->second);

      const std::size_t newFrequency = source->frequency + 1;
      auto targetFound = frequencyNode_.find(newFrequency);
      typename NodeList::iterator target;
      if (targetFound == frequencyNode_.end()) {
        target = data_.insert(std::next(source), Node{newFrequency, {}});
        frequencyNode_.emplace(newFrequency, target);
      } else {
        target = targetFound->second;
      }

      target->entries.splice(target->entries.end(), source->entries, found->second.entry);
      found->second.node = target;

      if (source->entries.empty()) {
        frequencyNode_.erase(source->frequency);
        data_.erase(source);
      }

      return result;
    }

    bool put(Key key, Value value) {
      if (maxEntries_ == 0 || keyNode_.find(key) != keyNode_.end()) {
        return false;
      }

      auto frequencyOne = frequencyNode_.find(1);
      typename NodeList::iterator target;
      if (frequencyOne == frequencyNode_.end()) {
        target = data_.insert(data_.begin(), Node{1, {}});
        frequencyNode_.emplace(1, target);
      } else {
        target = frequencyOne->second;
      }

      target->entries.emplace_back(std::move(key), std::move(value));
      auto entry = std::prev(target->entries.end());
      keyNode_.emplace(entry->first, Location{target, entry});
      evictToCapacity();
      return true;
    }

    bool contains(const Key& key) const { return keyNode_.find(key) != keyNode_.end(); }

    std::size_t size() const { return keyNode_.size(); }
    std::size_t maxEntries() const { return maxEntries_; }

    void clear() {
      keyNode_.clear();
      frequencyNode_.clear();
      data_.clear();
    }

  private:
    struct Node {
      std::size_t frequency = 0;
      std::list<std::pair<Key, Value>> entries;
    };

    using NodeList = std::list<Node>;
    using EntryList = std::list<std::pair<Key, Value>>;

    struct Location {
      typename NodeList::iterator node;
      typename EntryList::iterator entry;
    };

    void evictToCapacity() {
      while (keyNode_.size() > maxEntries_) {
        evictOne();
      }
    }

    void evictOne() {
      auto lowestFrequency = data_.begin();
      auto victim = lowestFrequency->entries.begin();
      keyNode_.erase(victim->first);
      lowestFrequency->entries.erase(victim);

      if (lowestFrequency->entries.empty()) {
        frequencyNode_.erase(lowestFrequency->frequency);
        data_.erase(lowestFrequency);
      }
    }

    std::size_t maxEntries_ = 0;
    NodeList data_;
    HashTable<Key, Location> keyNode_;
    HashTable<std::size_t, typename NodeList::iterator> frequencyNode_;
  };

} // namespace noria
