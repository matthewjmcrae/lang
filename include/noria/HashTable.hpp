#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace noria {

// An unordered-map-compatible associative container backed by contiguous,
// open-addressed storage. Collision resolution uses double hashing; erased
// slots are tombstones and are reused by later insertions.
template <typename Key, typename Val, typename Hasher = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class HashTable {
private:
    using stored_type = std::pair<const Key, Val>;
    enum class State : std::uint8_t { Empty, Occupied, Deleted };

    struct Slot {
        std::optional<stored_type> value;
        State state = State::Empty;
    };

public:
    using key_type = Key;
    using mapped_type = Val;
    using value_type = stored_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher = Hasher;
    using key_equal = KeyEqual;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;

    template <bool IsConst>
    class BasicIterator {
        friend class HashTable;
        template <bool>
        friend class BasicIterator;

        using table_type =
            std::conditional_t<IsConst, const HashTable, HashTable>;
        table_type* table_ = nullptr;
        size_type index_ = 0;

        BasicIterator(table_type* table, size_type index)
            : table_(table), index_(index) {
            skipVacant();
        }

        void skipVacant() {
            while (table_ != nullptr && index_ < table_->slots_.size() &&
                   table_->slots_[index_].state != State::Occupied) {
                ++index_;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename HashTable::value_type;
        using difference_type = typename HashTable::difference_type;
        using reference =
            std::conditional_t<IsConst, const value_type&, value_type&>;
        using pointer =
            std::conditional_t<IsConst, const value_type*, value_type*>;

        BasicIterator() = default;

        template <bool B = IsConst, typename = std::enable_if_t<B>>
        BasicIterator(const BasicIterator<false>& other)
            : table_(other.table_), index_(other.index_) {}

        reference operator*() const { return *table_->slots_[index_].value; }
        pointer operator->() const { return &**this; }

        BasicIterator& operator++() {
            ++index_;
            skipVacant();
            return *this;
        }

        BasicIterator operator++(int) {
            BasicIterator copy = *this;
            ++*this;
            return copy;
        }

        template <bool OtherConst>
        friend bool operator==(const BasicIterator& left,
                               const BasicIterator<OtherConst>& right) {
            return left.table_ == right.table_ && left.index_ == right.index_;
        }
    };

    using iterator = BasicIterator<false>;
    using const_iterator = BasicIterator<true>;

    explicit HashTable(size_type bucketCount = kMinimumCapacity,
                       const Hasher& hash = Hasher{},
                       const KeyEqual& equal = KeyEqual{})
        : hasher_(hash), equal_(equal) {
        initialize(bucketCount);
    }

    // Compatibility with the original interface. The tombstone value is used
    // only by get(); slot state is tracked independently, so this value may
    // also be stored as an ordinary mapped value.
    HashTable(size_type bucketCount, Val tombstone,
              const Hasher& hash = Hasher{},
              const KeyEqual& equal = KeyEqual{})
        : missingValue_(std::move(tombstone)), hasher_(hash), equal_(equal) {
        initialize(bucketCount);
    }

    HashTable(std::initializer_list<value_type> values,
              size_type bucketCount = kMinimumCapacity,
              const Hasher& hash = Hasher{},
              const KeyEqual& equal = KeyEqual{})
        : hasher_(hash), equal_(equal) {
        initialize(bucketCount);
        reserve(values.size());
        insert(values.begin(), values.end());
    }

    template <typename InputIt>
    HashTable(InputIt first, InputIt last,
              size_type bucketCount = kMinimumCapacity,
              const Hasher& hash = Hasher{},
              const KeyEqual& equal = KeyEqual{})
        : hasher_(hash), equal_(equal) {
        initialize(bucketCount);
        insert(first, last);
    }

    HashTable(const HashTable&) = default;
    HashTable(HashTable&&) noexcept = default;

    HashTable& operator=(const HashTable& other) {
        if (this != &other) {
            HashTable copy(other);
            swap(copy);
        }
        return *this;
    }

    HashTable& operator=(HashTable&&) noexcept = default;

    HashTable& operator=(std::initializer_list<value_type> values) {
        clear();
        reserve(values.size());
        insert(values);
        return *this;
    }

    [[nodiscard]] iterator begin() noexcept { return iterator(this, 0); }
    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator(this, 0);
    }
    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] iterator end() noexcept {
        return iterator(this, slots_.size());
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return const_iterator(this, slots_.size());
    }
    [[nodiscard]] const_iterator cend() const noexcept { return end(); }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type max_size() const noexcept {
        return slots_.max_size();
    }

    void clear() noexcept {
        for (Slot& slot : slots_) {
            slot.value.reset();
            slot.state = State::Empty;
        }
        size_ = 0;
        deleted_ = 0;
    }

    std::pair<iterator, bool> insert(const value_type& value) {
        return tryEmplaceImpl(value.first, value.second);
    }

    std::pair<iterator, bool> insert(value_type&& value) {
        // value_type has a const key, as required by unordered_map, so the key
        // is copied while the mapped value can still be moved.
        return tryEmplaceImpl(value.first, std::move(value.second));
    }

    template <typename P>
    std::pair<iterator, bool> insert(P&& value) {
        return emplace(std::forward<P>(value));
    }

    template <typename InputIt>
    void insert(InputIt first, InputIt last) {
        for (; first != last; ++first) {
            insert(*first);
        }
    }

    void insert(std::initializer_list<value_type> values) {
        insert(values.begin(), values.end());
    }

    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type value(std::forward<Args>(args)...);
        return insert(std::move(value));
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args) {
        return tryEmplaceImpl(key, std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(Key&& key, Args&&... args) {
        return tryEmplaceImpl(std::move(key), std::forward<Args>(args)...);
    }

    template <typename M>
    std::pair<iterator, bool> insert_or_assign(const Key& key, M&& value) {
        return insertOrAssignImpl(key, std::forward<M>(value));
    }

    template <typename M>
    std::pair<iterator, bool> insert_or_assign(Key&& key, M&& value) {
        return insertOrAssignImpl(std::move(key), std::forward<M>(value));
    }

    // Camel-case compatibility with the initial draft.
    template <typename M>
    std::pair<iterator, bool> insertOrAssign(Key key, M&& value) {
        return insert_or_assign(std::move(key), std::forward<M>(value));
    }

    iterator erase(const_iterator position) {
        const size_type index = position.index_;
        iterator next(this, index + 1);
        eraseAt(index);
        return next;
    }

    iterator erase(iterator position) {
        return erase(const_iterator(position));
    }

    iterator erase(const_iterator first, const_iterator last) {
        while (first != last) {
            first = erase(first);
        }
        return iterator(this, first.index_);
    }

    size_type erase(const Key& key) {
        const size_type index = findIndex(key);
        if (index == npos) {
            return 0;
        }
        eraseAt(index);
        return 1;
    }

    bool remove(const Key& key) { return erase(key) != 0; }

    [[nodiscard]] Val& at(const Key& key) {
        const size_type index = findIndex(key);
        if (index == npos) {
            throw std::out_of_range("HashTable::at: key not found");
        }
        return slots_[index].value->second;
    }

    [[nodiscard]] const Val& at(const Key& key) const {
        const size_type index = findIndex(key);
        if (index == npos) {
            throw std::out_of_range("HashTable::at: key not found");
        }
        return slots_[index].value->second;
    }

    Val& operator[](const Key& key) { return try_emplace(key).first->second; }
    Val& operator[](Key&& key) {
        return try_emplace(std::move(key)).first->second;
    }

    [[nodiscard]] iterator find(const Key& key) {
        const size_type index = findIndex(key);
        return index == npos ? end() : iterator(this, index);
    }

    [[nodiscard]] const_iterator find(const Key& key) const {
        const size_type index = findIndex(key);
        return index == npos ? end() : const_iterator(this, index);
    }

    [[nodiscard]] bool contains(const Key& key) const {
        return findIndex(key) != npos;
    }

    [[nodiscard]] size_type count(const Key& key) const {
        return contains(key) ? 1 : 0;
    }

    [[nodiscard]] std::pair<iterator, iterator> equal_range(const Key& key) {
        iterator found = find(key);
        if (found == end()) {
            return {found, found};
        }
        iterator after = found;
        ++after;
        return {found, after};
    }

    [[nodiscard]] std::pair<const_iterator, const_iterator>
    equal_range(const Key& key) const {
        const_iterator found = find(key);
        if (found == end()) {
            return {found, found};
        }
        const_iterator after = found;
        ++after;
        return {found, after};
    }

    // Compatibility accessor from the original draft. Prefer find() or at().
    [[nodiscard]] Val get(const Key& key) const {
        const_iterator found = find(key);
        if (found != end()) {
            return found->second;
        }
        if (missingValue_) {
            return *missingValue_;
        }
        throw std::out_of_range("HashTable::get: key not found");
    }

    [[nodiscard]] size_type bucket_count() const noexcept {
        return slots_.size();
    }
    [[nodiscard]] size_type capacity() const noexcept { return bucket_count(); }
    [[nodiscard]] float load_factor() const noexcept {
        return slots_.empty()
                   ? 0.0F
                   : static_cast<float>(size_) /
                         static_cast<float>(slots_.size());
    }
    [[nodiscard]] float max_load_factor() const noexcept {
        return maxLoadFactor_;
    }

    void max_load_factor(float value) {
        if (!(value > 0.0F && value < 1.0F)) {
            throw std::invalid_argument(
                "HashTable max_load_factor must be between 0 and 1");
        }
        maxLoadFactor_ = value;
        if (size_ > maxEntries(slots_.size())) {
            rehash(capacityFor(size_));
        }
    }

    void reserve(size_type desiredSize) { rehash(capacityFor(desiredSize)); }

    void rehash(size_type requestedBuckets) {
        requestedBuckets =
            nextPrime(maximum(requestedBuckets, capacityFor(size_)));
        if (requestedBuckets == slots_.size() && deleted_ == 0) {
            return;
        }

        std::vector<Slot> oldSlots = std::move(slots_);
        slots_ = std::vector<Slot>(requestedBuckets);
        size_ = 0;
        deleted_ = 0;
        for (Slot& slot : oldSlots) {
            if (slot.state == State::Occupied) {
                tryEmplaceWithoutGrowth(slot.value->first,
                                        std::move(slot.value->second));
            }
        }
    }

    [[nodiscard]] Hasher hash_function() const { return hasher_; }
    [[nodiscard]] KeyEqual key_eq() const { return equal_; }

    void swap(HashTable& other) {
        using std::swap;
        swap(slots_, other.slots_);
        swap(size_, other.size_);
        swap(deleted_, other.deleted_);
        swap(maxLoadFactor_, other.maxLoadFactor_);
        swap(missingValue_, other.missingValue_);
        swap(hasher_, other.hasher_);
        swap(equal_, other.equal_);
    }

    friend bool operator==(const HashTable& left, const HashTable& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (const auto& [key, value] : left) {
            const_iterator found = right.find(key);
            if (found == right.end() || !(found->second == value)) {
                return false;
            }
        }
        return true;
    }

    friend void swap(HashTable& left, HashTable& right) { left.swap(right); }

private:
    struct Probe {
        size_type index;
        bool found;
    };

    static constexpr size_type kMinimumCapacity = 5;
    static constexpr size_type npos = static_cast<size_type>(-1);

    std::vector<Slot> slots_;
    size_type size_ = 0;
    size_type deleted_ = 0;
    float maxLoadFactor_ = 0.70F;
    std::optional<Val> missingValue_;
    [[no_unique_address]] Hasher hasher_;
    [[no_unique_address]] KeyEqual equal_;

    void initialize(size_type bucketCount) {
        slots_.resize(nextPrime(maximum(bucketCount, kMinimumCapacity)));
    }

    template <typename K, typename... Args>
    std::pair<iterator, bool> tryEmplaceImpl(K&& key, Args&&... args) {
        Probe probe = locateForInsert(key);
        if (probe.found) {
            return {iterator(this, probe.index), false};
        }
        if (needsRehashForInsert()) {
            prepareForInsert();
            probe = locateForInsert(key);
        }
        occupy(probe.index, std::forward<K>(key),
               std::forward<Args>(args)...);
        return {iterator(this, probe.index), true};
    }

    template <typename K, typename M>
    std::pair<iterator, bool> insertOrAssignImpl(K&& key, M&& mapped) {
        Probe probe = locateForInsert(key);
        if (probe.found) {
            slots_[probe.index].value->second = std::forward<M>(mapped);
            return {iterator(this, probe.index), false};
        }
        if (needsRehashForInsert()) {
            prepareForInsert();
            probe = locateForInsert(key);
        }
        occupy(probe.index, std::forward<K>(key), std::forward<M>(mapped));
        return {iterator(this, probe.index), true};
    }

    template <typename K, typename M>
    void tryEmplaceWithoutGrowth(K&& key, M&& mapped) {
        const Probe probe = locateForInsert(key);
        occupy(probe.index, std::forward<K>(key), std::forward<M>(mapped));
    }

    template <typename K, typename... Args>
    void occupy(size_type index, K&& key, Args&&... args) {
        Slot& slot = slots_[index];
        const bool reused = slot.state == State::Deleted;
        slot.value.emplace(std::piecewise_construct,
                           std::forward_as_tuple(std::forward<K>(key)),
                           std::forward_as_tuple(std::forward<Args>(args)...));
        slot.state = State::Occupied;
        ++size_;
        if (reused) {
            --deleted_;
        }
    }

    void eraseAt(size_type index) {
        Slot& slot = slots_[index];
        slot.value.reset();
        slot.state = State::Deleted;
        --size_;
        ++deleted_;
    }

    [[nodiscard]] size_type findIndex(const Key& key) const {
        if (slots_.empty()) {
            return npos;
        }
        const auto [first, step] = hashes(key);
        size_type index = first;
        for (size_type examined = 0; examined < slots_.size(); ++examined) {
            const Slot& slot = slots_[index];
            if (slot.state == State::Empty) {
                return npos;
            }
            if (slot.state == State::Occupied &&
                equal_(slot.value->first, key)) {
                return index;
            }
            index = advance(index, step, slots_.size());
        }
        return npos;
    }

    [[nodiscard]] Probe locateForInsert(const Key& key) const {
        if (slots_.empty()) {
            return {npos, false};
        }
        const auto [first, step] = hashes(key);
        size_type index = first;
        size_type firstDeleted = npos;
        for (size_type examined = 0; examined < slots_.size(); ++examined) {
            const Slot& slot = slots_[index];
            if (slot.state == State::Empty) {
                return {firstDeleted == npos ? index : firstDeleted, false};
            }
            if (slot.state == State::Deleted) {
                if (firstDeleted == npos) {
                    firstDeleted = index;
                }
            } else if (equal_(slot.value->first, key)) {
                return {index, true};
            }
            index = advance(index, step, slots_.size());
        }
        return {firstDeleted, false};
    }

    [[nodiscard]] std::pair<size_type, size_type>
    hashes(const Key& key) const {
        const size_type hash = static_cast<size_type>(hasher_(key));
        const size_type buckets = slots_.size();
        return {hash % buckets,
                1 + mix(hash ^ static_cast<size_type>(0x9e3779b9U)) %
                        (buckets - 1)};
    }

    static size_type advance(size_type index, size_type step,
                             size_type capacity) noexcept {
        index += step;
        return index >= capacity ? index - capacity : index;
    }

    [[nodiscard]] bool needsRehashForInsert() const noexcept {
        return size_ + deleted_ + 1 > maxEntries(slots_.size());
    }

    void prepareForInsert() {
        if (size_ + 1 <= maxEntries(slots_.size())) {
            rehash(slots_.size());
        } else {
            rehash(slots_.size() * 2 + 1);
        }
    }

    [[nodiscard]] size_type maxEntries(size_type buckets) const noexcept {
        return static_cast<size_type>(
            static_cast<double>(buckets) * maxLoadFactor_);
    }

    [[nodiscard]] size_type capacityFor(size_type entries) const {
        if (entries == 0) {
            return kMinimumCapacity;
        }
        return static_cast<size_type>(
            std::ceil(static_cast<double>(entries) / maxLoadFactor_));
    }

    static size_type mix(size_type value) noexcept {
        if constexpr (sizeof(size_type) >= 8) {
            value ^= value >> 30;
            value *= static_cast<size_type>(0xbf58476d1ce4e5b9ULL);
            value ^= value >> 27;
            value *= static_cast<size_type>(0x94d049bb133111ebULL);
            value ^= value >> 31;
        } else {
            value ^= value >> 16;
            value *= static_cast<size_type>(0x7feb352dU);
            value ^= value >> 15;
            value *= static_cast<size_type>(0x846ca68bU);
            value ^= value >> 16;
        }
        return value;
    }

    static constexpr size_type maximum(size_type left,
                                       size_type right) noexcept {
        return left < right ? right : left;
    }

    static bool isPrime(size_type value) noexcept {
        if (value < 2) {
            return false;
        }
        if (value % 2 == 0) {
            return value == 2;
        }
        for (size_type divisor = 3; divisor <= value / divisor; divisor += 2) {
            if (value % divisor == 0) {
                return false;
            }
        }
        return true;
    }

    static size_type nextPrime(size_type value) {
        if (value <= 2) {
            return 2;
        }
        if (value % 2 == 0) {
            ++value;
        }
        while (!isPrime(value)) {
            if (value > static_cast<size_type>(-1) - 2) {
                throw std::length_error("HashTable capacity is too large");
            }
            value += 2;
        }
        return value;
    }
};

} // namespace noria
