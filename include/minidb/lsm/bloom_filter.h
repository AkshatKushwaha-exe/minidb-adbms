// A small Bloom filter used to skip SSTables that definitely do not contain a
// key, which avoids unnecessary disk reads on point lookups (a key LSM-tree
// optimisation). False positives are possible (we then do the real lookup);
// false negatives are not.
//
// We use the standard double-hashing trick (Kirsch–Mitzenmacher): h_i(x) =
// h1(x) + i*h2(x), so we only need two base hashes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "minidb/record/value.h"

namespace minidb {
namespace lsm {

class BloomFilter {
public:
    // Size the bit array for `expected` keys at ~1% false-positive rate
    // (~9.6 bits/key, k = 7 hashes).
    explicit BloomFilter(std::size_t expected = 1024) {
        std::size_t bits = expected * 10;
        if (bits < 64) bits = 64;
        bits_.assign(bits, false);
        k_ = 7;
    }

    void add(const Value& key) {
        uint64_t h1, h2;
        hashes(key, h1, h2);
        for (int i = 0; i < k_; ++i)
            bits_[(h1 + static_cast<uint64_t>(i) * h2) % bits_.size()] = true;
    }

    // True if the key *might* be present; false means definitely absent.
    bool maybe_contains(const Value& key) const {
        uint64_t h1, h2;
        hashes(key, h1, h2);
        for (int i = 0; i < k_; ++i)
            if (!bits_[(h1 + static_cast<uint64_t>(i) * h2) % bits_.size()])
                return false;
        return true;
    }

private:
    void hashes(const Value& key, uint64_t& h1, uint64_t& h2) const {
        if (key.type() == Type::INT) {
            uint64_t x = static_cast<uint64_t>(key.as_int());
            h1 = std::hash<uint64_t>()(x);
            h2 = std::hash<uint64_t>()(x * 0x9E3779B97F4A7C15ULL + 1);
        } else {
            h1 = std::hash<std::string>()(key.as_text());
            h2 = std::hash<std::string>()(key.as_text() + "\x01salt");
        }
        h2 |= 1;  // ensure odd so the step never gets stuck
    }

    std::vector<bool> bits_;
    int k_;
};

}  // namespace lsm
}  // namespace minidb
