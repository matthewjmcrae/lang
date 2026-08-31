#include "noria/HashTable.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  struct ConstantHash {
    std::size_t operator()(int) const noexcept { return 0; }
  };

} // namespace

int main() {
  noria::HashTable<int, std::string, ConstantHash> collisions;
  const auto [one, insertedOne] = collisions.emplace(1, "one");
  expect(insertedOne && one->second == "one", "emplace inserts a collision-chain entry");
  expect(collisions.emplace(2, "two").second, "double hashing reaches a second slot");
  expect(collisions.emplace(3, "three").second, "double hashing reaches a third slot");
  expect(!collisions.emplace(2, "replacement").second,
         "emplace rejects a duplicate collision-chain key");
  expect(collisions.at(2) == "two", "duplicate emplace preserves mapped value");

  expect(collisions.erase(2) == 1, "erase marks a collision-chain entry deleted");
  expect(collisions.find(3) != collisions.end(), "lookup probes past a tombstone");
  expect(collisions.emplace(4, "four").second, "insertion reuses a tombstone");
  expect(collisions.contains(4), "tombstone-reused entry is discoverable");

  noria::HashTable<int, int> values;
  values[10] = 100;
  expect(values.at(10) == 100, "operator[] inserts a default mapped value");
  bool missingThrows = false;
  try {
    (void)values.at(99);
  } catch (const std::out_of_range&) {
    missingThrows = true;
  }
  expect(missingThrows, "at throws for a missing key");

  values.reserve(128);
  for (int index = 0; index < 128; ++index) {
    values.insert_or_assign(index, index * 3);
  }
  expect(values.size() == 128, "reserve and growth retain all inserted entries");
  for (int index = 0; index < 128; ++index) {
    expect(values.at(index) == index * 3, "growth preserves mapped values");
  }

  std::size_t iterated = 0;
  for (const auto& [key, value] : values) {
    expect(value == key * 3, "iteration exposes each stored pair");
    ++iterated;
  }
  expect(iterated == values.size(), "iteration visits every entry exactly once");

  auto iterator = values.find(64);
  iterator = values.erase(iterator);
  expect(!values.contains(64), "iterator erase removes the requested entry");
  expect(iterator == values.end() || iterator->first != 64,
         "iterator erase returns the following entry");
  expect(values.erase(999) == 0, "key erase reports a missing key");

  noria::HashTable<int, int> copied = values;
  expect(copied == values, "copy construction preserves contents");
  noria::HashTable<int, int> assigned;
  assigned = copied;
  expect(assigned == copied, "copy assignment preserves contents");
  noria::HashTable<int, int> moved = std::move(copied);
  expect(moved == values, "move construction preserves contents");
  copied[500] = 5;
  expect(copied.at(500) == 5, "a moved-from table remains reusable");

  values.clear();
  expect(values.empty() && values.begin() == values.end(),
         "clear removes occupied slots and resets iteration");

  if (failures != 0) {
    std::cerr << failures << " hash table test failure(s)\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
