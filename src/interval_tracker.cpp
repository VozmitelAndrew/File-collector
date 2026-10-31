#include "file_collector/interval_tracker.h"

#include <algorithm>
#include <cassert>
#include <iterator>

#include "file_collector/coverage_tracker.h"

namespace fc {

static_assert(CoverageTracker<IntervalTracker>);

IntervalTracker::IntervalTracker(std::size_t size) : size_(size) {}

std::vector<Range> IntervalTracker::AddAndGetGaps(std::size_t pos, std::size_t len) {
    std::vector<Range> gaps;
    Insert(pos, pos + len, &gaps);
    return gaps;
}

void IntervalTracker::Add(std::size_t pos, std::size_t len) {
    Insert(pos, pos + len, nullptr);
}

void IntervalTracker::Insert(std::size_t begin, std::size_t end, std::vector<Range>* gaps) {
    assert(begin <= end && end <= size_);
    if (begin == end) {
        return;
    }

    // Start from the previous interval if it overlaps or touches the new one.
    auto it = intervals_.upper_bound(begin);
    if (it != intervals_.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= begin) {
            it = prev;
        }
    }

    std::size_t mergedBegin = begin;
    std::size_t mergedEnd = end;
    std::size_t cursor = begin;  // bytes before cursor are already checked

    // Remove every interval that overlaps or touches [begin, end) and collect the holes between them.
    while (it != intervals_.end() && it->first <= end) {
        if (gaps && it->first > cursor) {
            gaps->push_back({cursor, it->first});
        }
        cursor = std::max(cursor, it->second);
        mergedBegin = std::min(mergedBegin, it->first);
        mergedEnd = std::max(mergedEnd, it->second);
        covered_ -= it->second - it->first;
        it = intervals_.erase(it);
    }

    if (gaps && cursor < end) {
        gaps->push_back({cursor, end});
    }

    intervals_.emplace_hint(it, mergedBegin, mergedEnd);
    covered_ += mergedEnd - mergedBegin;
}

bool IntervalTracker::Contains(std::size_t pos, std::size_t len) const {
    if (pos > size_ || len > size_ - pos) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    auto it = intervals_.upper_bound(pos);
    if (it == intervals_.begin()) {
        return false;
    }
    --it;
    return it->second >= pos + len;
}

std::size_t IntervalTracker::ContiguousFrom(std::size_t pos) const {
    if (pos >= size_) {
        return 0;
    }
    auto it = intervals_.upper_bound(pos);
    if (it == intervals_.begin()) {
        return 0;
    }
    --it;
    return it->second > pos ? it->second - pos : 0;
}

template <class F>
void IntervalTracker::ForEachCovered(std::size_t pos, std::size_t len, F&& f) const {
    if (pos >= size_) {
        return;
    }
    const std::size_t end = pos + std::min(len, size_ - pos);
    auto it = intervals_.upper_bound(pos);
    if (it != intervals_.begin()) {
        --it;  // the previous interval may reach into the range
    }
    for (; it != intervals_.end() && it->first < end; ++it) {
        const std::size_t begin = std::max(pos, it->first);
        const std::size_t stop = std::min(end, it->second);
        if (begin < stop) {
            f(Range{begin, stop});
        }
    }
}

std::size_t IntervalTracker::CoveredBytesIn(std::size_t pos, std::size_t len) const {
    std::size_t total = 0;
    ForEachCovered(pos, len, [&](Range range) { total += range.Size(); });
    return total;
}

std::vector<Range> IntervalTracker::CoveredRanges(std::size_t pos, std::size_t len) const {
    std::vector<Range> result;
    ForEachCovered(pos, len, [&](Range range) { result.push_back(range); });
    return result;
}

}  // namespace fc
