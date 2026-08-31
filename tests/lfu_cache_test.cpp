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

  std::optional<std::string> get(noria::LFUCache<std::string, std::string>& cache,
                                 const std::string& key) {
    return cache.get(key, [](const std::string& value) { return value; });
  }

} // namespace

int main() {
  noria::LFUCache<std::string, std::string> frequencyCache(2);
  frequencyCache.put("a", "A");
  frequencyCache.put("b", "B");
  get(frequencyCache, "a");
  get(frequencyCache, "a");
  frequencyCache.put("c", "C");
  expect(frequencyCache.contains("a"), "LFU keeps most frequently used entry");
  expect(!frequencyCache.contains("b"), "LFU evicts least frequently used entry");
  expect(frequencyCache.contains("c"), "LFU admits new entry");

  noria::LFUCache<std::string, std::string> tieCache(2);
  tieCache.put("a", "A");
  tieCache.put("b", "B");
  tieCache.put("c", "C");
  expect(!tieCache.contains("a"), "LFU breaks frequency ties by oldest use");
  expect(tieCache.contains("b"), "LFU keeps the newer peer");
  expect(tieCache.contains("c"), "LFU retains the admitted entry");

  noria::LFUCache<std::string, std::string> cloneCache(1);
  cloneCache.put("a", "value");
  const auto length = cloneCache.get("a", [](const std::string& value) { return value.size(); });
  expect(length == std::optional<std::size_t>(5), "get supports a distinct clone result type");
  expect(get(cloneCache, "a") == std::optional<std::string>("value"),
         "get retains the cached value while promoting it");

  noria::LFUCache<std::string, std::string> duplicateCache(1);
  expect(duplicateCache.put("a", "first"), "first insertion succeeds");
  expect(!duplicateCache.put("a", "second"), "duplicate insertion is rejected");
  expect(get(duplicateCache, "a") == std::optional<std::string>("first"),
         "duplicate insertion does not replace the value");

  noria::LFUCache<std::string, std::string> disabledCache(0);
  expect(!disabledCache.put("a", "A"), "zero-capacity cache rejects entries");

  noria::LFUCache<std::string, std::string> churnCache(32);
  for (std::size_t index = 0; index < 32; ++index) {
    expect(churnCache.put("key" + std::to_string(index), std::to_string(index)),
           "cache accepts entries through the reserved capacity");
  }
  for (std::size_t index = 0; index < 32; ++index) {
    for (std::size_t access = 0; access < index % 5; ++access) {
      (void)get(churnCache, "key" + std::to_string(index));
    }
  }
  for (std::size_t index = 0; index < 32; ++index) {
    expect(churnCache.put("new" + std::to_string(index), std::to_string(index)),
           "cache remains usable after repeated frequency-index updates");
  }
  expect(churnCache.size() == 32, "cache maintains its capacity through eviction churn");
  churnCache.clear();
  expect(churnCache.size() == 0, "clear empties both hash table indexes");
  expect(churnCache.put("again", "value"), "cache is reusable after clearing indexes");
  expect(get(churnCache, "again") == std::optional<std::string>("value"),
         "cache retrieves entries after index reuse");

  if (failures != 0) {
    std::cerr << failures << " LFU cache test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
