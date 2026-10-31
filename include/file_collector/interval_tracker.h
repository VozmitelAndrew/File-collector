#pragma once

#include <cstddef>
#include <map>
#include <vector>

#include "file_collector/range.h"

namespace fc {

// Keeps received bytes as a list of intervals that do not touch each other.
// Good when there are few gaps: memory depends on the number of gaps, not on the file size.
class IntervalTracker {
public:
    explicit IntervalTracker(std::size_t size);

    std::vector<Range> AddAndGetGaps(std::size_t pos, std::size_t len);
    void Add(std::size_t pos, std::size_t len);

    bool Contains(std::size_t pos, std::size_t len) const;
    std::size_t ContiguousFrom(std::size_t pos) const;
    std::size_t CoveredBytesIn(std::size_t pos, std::size_t len) const;
    std::vector<Range> CoveredRanges(std::size_t pos, std::size_t len) const;

    std::size_t CoveredBytes() const noexcept { return covered_; }
    bool IsFull() const noexcept { return covered_ == size_; }
    std::size_t Size() const noexcept { return size_; }

    std::size_t IntervalCount() const noexcept { return intervals_.size(); }

private:
    void Insert(std::size_t begin, std::size_t end, std::vector<Range>* gaps);
    template <class F>
    void ForEachCovered(std::size_t pos, std::size_t len, F&& f) const;

    std::size_t size_;
    std::size_t covered_ = 0;
    std::map<std::size_t, std::size_t> intervals_;  // begin -> end
};

}  // namespace fc
