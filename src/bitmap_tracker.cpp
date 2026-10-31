#include "file_collector/bitmap_tracker.h"

#include <algorithm>
#include <bit>
#include <cassert>

#include "file_collector/coverage_tracker.h"

namespace fc {

static_assert(CoverageTracker<BitmapTracker>);

namespace {

constexpr std::size_t kWordBits = 64;
constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

// Bits from `bit` to 63. bit must be < 64.
std::uint64_t MaskFrom(std::size_t bit) {
    return kAllOnes << bit;
}

// Bits from 0 to bit - 1. bit can be from 0 to 64.
std::uint64_t MaskBelow(std::size_t bit) {
    return bit == 0 ? 0 : kAllOnes >> (kWordBits - bit);
}

// Calls f(wordIndex, mask) for every word that has bits inside [begin, end).
template <class F>
void ForEachWord(std::size_t begin, std::size_t end, F&& f) {
    if (begin >= end) {
        return;
    }
    const std::size_t first = begin / kWordBits;
    const std::size_t last = (end - 1) / kWordBits;
    if (first == last) {
        f(first, MaskFrom(begin % kWordBits) & MaskBelow(end - first * kWordBits));
        return;
    }
    f(first, MaskFrom(begin % kWordBits));
    for (std::size_t i = first + 1; i < last; ++i) {
        f(i, kAllOnes);
    }
    f(last, MaskBelow(end - last * kWordBits));
}

}  // namespace

BitmapTracker::BitmapTracker(std::size_t size)
    : size_(size), words_((size + kWordBits - 1) / kWordBits, 0) {}

std::vector<Range> BitmapTracker::AddAndGetGaps(std::size_t pos, std::size_t len) {
    std::vector<Range> gaps;
    Insert(pos, pos + len, &gaps);
    return gaps;
}

void BitmapTracker::Add(std::size_t pos, std::size_t len) {
    Insert(pos, pos + len, nullptr);
}

void BitmapTracker::Insert(std::size_t begin, std::size_t end, std::vector<Range>* gaps) {
    assert(begin <= end && end <= size_);
    std::size_t cursor = begin;
    while (cursor < end) {
        const std::size_t gapBegin = FindFirst(cursor, end, false);
        if (gapBegin == end) {
            break;
        }
        const std::size_t gapEnd = FindFirst(gapBegin, end, true);
        ForEachWord(gapBegin, gapEnd, [&](std::size_t i, std::uint64_t mask) { words_[i] |= mask; });
        covered_ += gapEnd - gapBegin;
        if (gaps) {
            gaps->push_back({gapBegin, gapEnd});
        }
        cursor = gapEnd;
    }
}

std::size_t BitmapTracker::FindFirst(std::size_t from, std::size_t to, bool value) const {
    if (from >= to) {
        return to;
    }
    std::size_t index = from / kWordBits;
    std::uint64_t word = (value ? words_[index] : ~words_[index]) & MaskFrom(from % kWordBits);
    while (true) {
        if (word != 0) {
            const std::size_t found = index * kWordBits + std::countr_zero(word);
            return std::min(found, to);
        }
        ++index;
        if (index * kWordBits >= to) {
            return to;
        }
        word = value ? words_[index] : ~words_[index];
    }
}

std::size_t BitmapTracker::ClipEnd(std::size_t pos, std::size_t len) const {
    return pos >= size_ ? pos : pos + std::min(len, size_ - pos);
}

bool BitmapTracker::Contains(std::size_t pos, std::size_t len) const {
    if (pos > size_ || len > size_ - pos) {
        return false;
    }
    return FindFirst(pos, pos + len, false) == pos + len;
}

std::size_t BitmapTracker::ContiguousFrom(std::size_t pos) const {
    if (pos >= size_) {
        return 0;
    }
    return FindFirst(pos, size_, false) - pos;
}

std::size_t BitmapTracker::CoveredBytesIn(std::size_t pos, std::size_t len) const {
    std::size_t total = 0;
    ForEachWord(pos, ClipEnd(pos, len), [&](std::size_t i, std::uint64_t mask) {
        total += std::popcount(words_[i] & mask);
    });
    return total;
}

std::vector<Range> BitmapTracker::CoveredRanges(std::size_t pos, std::size_t len) const {
    std::vector<Range> result;
    const std::size_t end = ClipEnd(pos, len);
    std::size_t cursor = pos;
    while (cursor < end) {
        const std::size_t begin = FindFirst(cursor, end, true);
        if (begin == end) {
            break;
        }
        const std::size_t stop = FindFirst(begin, end, false);
        result.push_back({begin, stop});
        cursor = stop;
    }
    return result;
}

}  // namespace fc
