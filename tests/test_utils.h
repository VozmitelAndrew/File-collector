#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "file_collector/bitmap_tracker.h"
#include "file_collector/file_collector.h"
#include "file_collector/interval_tracker.h"

namespace fc::test {

using Bytes = std::vector<std::uint8_t>;

using CollectorTypes = ::testing::Types<BasicFileCollector<IntervalTracker>,
                                        BasicFileCollector<BitmapTracker>>;

inline Bytes ToBytes(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

inline Bytes ToBytes(std::span<const std::uint8_t> bytes) {
    return Bytes(bytes.begin(), bytes.end());
}

inline Bytes MakeRandomFile(std::size_t size, std::mt19937& rng) {
    Bytes file(size);
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& value : file) {
        value = static_cast<std::uint8_t>(byte(rng));
    }
    return file;
}

struct Chunk {
    std::size_t pos;
    Bytes data;
};

// Splits a file into overlapping chunks that cover it completely, adds some
// duplicates and random sub-chunks, and shuffles the result.
inline std::vector<Chunk> MakeShuffledChunks(const Bytes& file, std::size_t maxChunk, std::mt19937& rng) {
    std::vector<Chunk> chunks;
    auto slice = [&](std::size_t pos, std::size_t len) {
        chunks.push_back({pos, Bytes(file.begin() + pos, file.begin() + pos + len)});
    };

    std::size_t pos = 0;
    while (pos < file.size()) {
        const std::size_t len =
            std::min(std::uniform_int_distribution<std::size_t>(1, maxChunk)(rng), file.size() - pos);
        slice(pos, len);
        const std::size_t overlap = std::uniform_int_distribution<std::size_t>(0, len / 2)(rng);
        pos += len - overlap;
    }

    const std::size_t extra = chunks.size() / 4;
    for (std::size_t i = 0; i < extra && !file.empty(); ++i) {
        const std::size_t start = std::uniform_int_distribution<std::size_t>(0, file.size() - 1)(rng);
        const std::size_t len = std::min(
            std::uniform_int_distribution<std::size_t>(1, maxChunk)(rng), file.size() - start);
        slice(start, len);
    }

    std::shuffle(chunks.begin(), chunks.end(), rng);
    return chunks;
}

}  // namespace fc::test
