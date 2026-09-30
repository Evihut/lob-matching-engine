#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Open-addressing hash map from OrderId to a 32-bit slot index.
//
// Linear probing with Fibonacci hashing, and backward-shift deletion instead of
// tombstones, so lookups stay short after heavy add/cancel churn. The key
// kReservedOrderId (UINT64_MAX) is the empty marker; find/insert/erase treat it
// as absent, so a caller passing it can never alias an empty slot.
class IdMap {
public:
    static constexpr OrderId kEmptyKey = kReservedOrderId;
    static constexpr std::uint32_t kNotFound = std::numeric_limits<std::uint32_t>::max();

    explicit IdMap(std::uint32_t expected = 1024) { rehash(capacity_for(expected)); }

    std::uint32_t find(OrderId key) const noexcept {
        if (key == kEmptyKey) return kNotFound;
        std::size_t i = home(key);
        while (true) {
            const Slot& s = slots_[i];
            if (s.key == key) return s.value;
            if (s.key == kEmptyKey) return kNotFound;
            i = (i + 1) & mask_;
        }
    }

    // Returns false (and leaves the map unchanged) if the key already exists.
    bool insert(OrderId key, std::uint32_t value) {
        if (key == kEmptyKey) return false;
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        std::size_t i = home(key);
        while (slots_[i].key != kEmptyKey) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = {key, value};
        ++size_;
        return true;
    }

    bool erase(OrderId key) noexcept {
        if (key == kEmptyKey) return false;
        std::size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmptyKey) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift: pull later entries of the probe run into the hole
        // whenever the hole lies between their home slot and where they sit.
        std::size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmptyKey) break;
            const std::size_t k = home(slots_[j].key);
            if (((j - k) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = kEmptyKey;
        --size_;
        return true;
    }

    std::size_t size() const noexcept { return size_; }

private:
    struct Slot {
        OrderId key = kEmptyKey;
        std::uint32_t value = 0;
    };

    static std::size_t capacity_for(std::uint32_t expected) {
        std::size_t cap = 16;
        while (cap < std::size_t{expected} * 2) cap *= 2;
        return cap;
    }

    std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        shift_ = 64 - static_cast<unsigned>(__builtin_ctzll(new_cap));
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != kEmptyKey) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

}  // namespace lob
