#include "noria/LfuCache.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  std::optional<std::string> get(noria::LfuCache<std::string, std::string>& cache,
                                 const std::string& key) {
    return cache.get(key, [](const std::string& value) { return value; });
  }

} // namespace

int main() {
  noria::LfuCache<std::string, std::string> frequencyCache(2, 100);
  frequencyCache.put("a", "A", 1);
  frequencyCache.put("b", "B", 1);
  get(frequencyCache, "a");
  get(frequencyCache, "a");
  frequencyCache.put("c", "C", 1);
  expect(frequencyCache.contains("a"), "LFU keeps most frequently used entry");
  expect(!frequencyCache.contains("b"), "LFU evicts least frequently used entry");
  expect(frequencyCache.contains("c"), "LFU admits new entry");

  noria::LfuCache<std::string, std::string> tieCache(2, 100);
  tieCache.put("a", "A", 1);
  tieCache.put("b", "B", 1);
  tieCache.put("c", "C", 1);
  expect(!tieCache.contains("a"), "LFU breaks frequency ties by oldest use");
  expect(tieCache.contains("b"), "LRU tie-break keeps newer peer");
  expect(tieCache.contains("c"), "LRU tie-break keeps newest peer");

  noria::LfuCache<std::string, std::string> weightCache(10, 5);
  weightCache.put("a", "A", 2);
  weightCache.put("b", "B", 2);
  get(weightCache, "a");
  weightCache.put("c", "C", 3);
  expect(weightCache.contains("a"), "weight eviction keeps frequently used entry");
  expect(!weightCache.contains("b"), "weight eviction removes least useful entry");
  expect(weightCache.contains("c"), "weight eviction admits fitting entry");
  expect(weightCache.totalWeight() <= weightCache.maxWeight(), "weight cache stays under cap");

  noria::LfuCache<std::string, std::string> oversizedCache(10, 3);
  expect(!oversizedCache.put("big", "BIG", 4), "oversized entry is rejected");
  expect(!oversizedCache.contains("big"), "oversized entry is not retained");

  if (failures != 0) {
    std::cerr << failures << " LFU cache test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
