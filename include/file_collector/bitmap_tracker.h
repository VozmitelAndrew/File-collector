#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "file_collector/range.h"

namespace fc {

// One bit per byte of the file. Always uses Size() / 8 bytes of memory,
// but does not care how many gaps there are. Works with 64 bits at a time.
class BitmapTracker {
public:
    explicit BitmapTracker(std::size_t size);

    std::vector<Range> AddAndGetGaps(std::size_t pos, std::size_t len);
    void Add(std::size_t pos, std::size_t len);

    bool Contains(std::size_t pos, std::size_t len) const;
    std::size_t ContiguousFrom(std::size_t pos) const;
    std::size_t CoveredBytesIn(std::size_t pos, std::size_t len) const;
    std::vector<Range> CoveredRanges(std::size_t pos, std::size_t len) const;

    std::size_t CoveredBytes() const noexcept { return covered_; }
    bool IsFull() const noexcept { return covered_ == size_; }
    std::size_t Size() const noexcept { return size_; }

private:
    void Insert(std::size_t begin, std::size_t end, std::vector<Range>* gaps);
    // Position of the first bit equal to `value` in [from, to), or `to` if there is none.
    std::size_t FindFirst(std::size_t from, std::size_t to, bool value) const;
    std::size_t ClipEnd(std::size_t pos, std::size_t len) const;

    std::size_t size_;
    std::size_t covered_ = 0;
    std::vector<std::uint64_t> words_;
};

}  // namespace fc
