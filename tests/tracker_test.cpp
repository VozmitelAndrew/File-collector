#include <gtest/gtest.h>

#include <ostream>
#include <random>
#include <vector>

#include "file_collector/bitmap_tracker.h"
#include "file_collector/interval_tracker.h"

namespace fc {

void PrintTo(const Range& range, std::ostream* os) {
    *os << "[" << range.begin << ", " << range.end << ")";
}

}  // namespace fc

namespace {

using fc::Range;
using Gaps = std::vector<Range>;

template <class Tracker>
class TrackerTest : public ::testing::Test {};

using TrackerTypes = ::testing::Types<fc::IntervalTracker, fc::BitmapTracker>;
TYPED_TEST_SUITE(TrackerTest, TrackerTypes);

TYPED_TEST(TrackerTest, EmptyTracker) {
    TypeParam tracker(10);
    EXPECT_EQ(tracker.Size(), 10u);
    EXPECT_EQ(tracker.CoveredBytes(), 0u);
    EXPECT_FALSE(tracker.IsFull());
    EXPECT_FALSE(tracker.Contains(0, 1));
    EXPECT_TRUE(tracker.Contains(0, 0));
    EXPECT_EQ(tracker.ContiguousFrom(0), 0u);
}

TYPED_TEST(TrackerTest, ZeroSizeIsFull) {
    TypeParam tracker(0);
    EXPECT_TRUE(tracker.IsFull());
    EXPECT_EQ(tracker.ContiguousFrom(0), 0u);
}

TYPED_TEST(TrackerTest, ExampleFromTask) {
    TypeParam tracker(10);
    EXPECT_EQ(tracker.AddAndGetGaps(0, 4), (Gaps{{0, 4}}));
    EXPECT_EQ(tracker.AddAndGetGaps(2, 4), (Gaps{{4, 6}}));
    EXPECT_EQ(tracker.AddAndGetGaps(6, 4), (Gaps{{6, 10}}));
    EXPECT_TRUE(tracker.IsFull());
    EXPECT_EQ(tracker.ContiguousFrom(0), 10u);
}

TYPED_TEST(TrackerTest, DuplicateHasNoGaps) {
    TypeParam tracker(10);
    tracker.Add(2, 5);
    EXPECT_TRUE(tracker.AddAndGetGaps(2, 5).empty());
    EXPECT_TRUE(tracker.AddAndGetGaps(3, 2).empty());
    EXPECT_EQ(tracker.CoveredBytes(), 5u);
}

TYPED_TEST(TrackerTest, EmptyRangeHasNoGaps) {
    TypeParam tracker(10);
    EXPECT_TRUE(tracker.AddAndGetGaps(4, 0).empty());
    EXPECT_EQ(tracker.CoveredBytes(), 0u);
}

TYPED_TEST(TrackerTest, AdjacentRangesMerge) {
    TypeParam tracker(10);
    tracker.Add(0, 4);
    tracker.Add(4, 4);
    EXPECT_EQ(tracker.ContiguousFrom(0), 8u);
    EXPECT_TRUE(tracker.Contains(0, 8));
    EXPECT_FALSE(tracker.Contains(0, 9));
}

TYPED_TEST(TrackerTest, ReverseOrder) {
    TypeParam tracker(9);
    EXPECT_EQ(tracker.AddAndGetGaps(6, 3), (Gaps{{6, 9}}));
    EXPECT_EQ(tracker.AddAndGetGaps(3, 3), (Gaps{{3, 6}}));
    EXPECT_EQ(tracker.AddAndGetGaps(0, 3), (Gaps{{0, 3}}));
    EXPECT_TRUE(tracker.IsFull());
}

TYPED_TEST(TrackerTest, RangeBridgingSeveralIntervals) {
    TypeParam tracker(14);
    tracker.Add(2, 2);
    tracker.Add(6, 2);
    tracker.Add(10, 2);
    EXPECT_EQ(tracker.AddAndGetGaps(0, 14), (Gaps{{0, 2}, {4, 6}, {8, 10}, {12, 14}}));
    EXPECT_TRUE(tracker.IsFull());
}

TYPED_TEST(TrackerTest, PartialOverlapOnBothSides) {
    TypeParam tracker(20);
    tracker.Add(0, 5);
    tracker.Add(10, 5);
    EXPECT_EQ(tracker.AddAndGetGaps(3, 9), (Gaps{{5, 10}}));
    EXPECT_EQ(tracker.ContiguousFrom(0), 15u);
    EXPECT_EQ(tracker.CoveredBytes(), 15u);
}

TYPED_TEST(TrackerTest, ContiguousFromInsideAndOutside) {
    TypeParam tracker(20);
    tracker.Add(5, 5);
    EXPECT_EQ(tracker.ContiguousFrom(0), 0u);
    EXPECT_EQ(tracker.ContiguousFrom(5), 5u);
    EXPECT_EQ(tracker.ContiguousFrom(7), 3u);
    EXPECT_EQ(tracker.ContiguousFrom(10), 0u);
    EXPECT_EQ(tracker.ContiguousFrom(20), 0u);
    EXPECT_EQ(tracker.ContiguousFrom(100), 0u);
}

TYPED_TEST(TrackerTest, ContainsOutOfBounds) {
    TypeParam tracker(10);
    tracker.Add(0, 10);
    EXPECT_TRUE(tracker.Contains(0, 10));
    EXPECT_FALSE(tracker.Contains(5, 6));
    EXPECT_FALSE(tracker.Contains(11, 0));
    EXPECT_FALSE(tracker.Contains(SIZE_MAX, 2));
}

TYPED_TEST(TrackerTest, WordBoundaries) {
    TypeParam tracker(200);
    EXPECT_EQ(tracker.AddAndGetGaps(60, 10), (Gaps{{60, 70}}));
    EXPECT_EQ(tracker.AddAndGetGaps(0, 128), (Gaps{{0, 60}, {70, 128}}));
    EXPECT_EQ(tracker.AddAndGetGaps(127, 73), (Gaps{{128, 200}}));
    EXPECT_TRUE(tracker.IsFull());
}

TYPED_TEST(TrackerTest, CoveredInsideRange) {
    TypeParam tracker(100);
    tracker.Add(10, 10);
    tracker.Add(30, 10);
    tracker.Add(70, 30);

    EXPECT_EQ(tracker.CoveredBytesIn(0, 100), 50u);
    EXPECT_EQ(tracker.CoveredBytesIn(15, 20), 10u);
    EXPECT_EQ(tracker.CoveredBytesIn(20, 10), 0u);
    EXPECT_EQ(tracker.CoveredBytesIn(90, 1000), 10u);
    EXPECT_EQ(tracker.CoveredBytesIn(200, 10), 0u);

    EXPECT_EQ(tracker.CoveredRanges(15, 20), (Gaps{{15, 20}, {30, 35}}));
    EXPECT_EQ(tracker.CoveredRanges(0, 100), (Gaps{{10, 20}, {30, 40}, {70, 100}}));
    EXPECT_TRUE(tracker.CoveredRanges(40, 30).empty());
    EXPECT_TRUE(tracker.CoveredRanges(100, 5).empty());
}

// Compares the tracker with a simple vector<bool> model on random operations.
TYPED_TEST(TrackerTest, RandomizedAgainstNaiveModel) {
    std::mt19937 rng(42);
    for (int round = 0; round < 20; ++round) {
        const std::size_t size = std::uniform_int_distribution<std::size_t>(1, 700)(rng);
        TypeParam tracker(size);
        std::vector<bool> model(size, false);

        for (int op = 0; op < 200 && !tracker.IsFull(); ++op) {
            const std::size_t pos = std::uniform_int_distribution<std::size_t>(0, size - 1)(rng);
            const std::size_t maxLen = std::min<std::size_t>(size - pos, 80);
            const std::size_t len = std::uniform_int_distribution<std::size_t>(0, maxLen)(rng);

            Gaps expected;
            for (std::size_t i = pos; i < pos + len; ++i) {
                if (!model[i]) {
                    if (!expected.empty() && expected.back().end == i) {
                        ++expected.back().end;
                    } else {
                        expected.push_back({i, i + 1});
                    }
                    model[i] = true;
                }
            }
            ASSERT_EQ(tracker.AddAndGetGaps(pos, len), expected);

            std::size_t covered = 0;
            for (bool bit : model) {
                covered += bit;
            }
            ASSERT_EQ(tracker.CoveredBytes(), covered);

            const std::size_t probe = std::uniform_int_distribution<std::size_t>(0, size - 1)(rng);
            std::size_t run = 0;
            while (probe + run < size && model[probe + run]) {
                ++run;
            }
            ASSERT_EQ(tracker.ContiguousFrom(probe), run);
            ASSERT_EQ(tracker.Contains(probe, run), true);
            if (probe + run < size) {
                ASSERT_FALSE(tracker.Contains(probe, run + 1));
            }

            const std::size_t probeLen = std::uniform_int_distribution<std::size_t>(0, size)(rng);
            const std::size_t probeEnd = std::min(size, probe + probeLen);
            Gaps expectedRanges;
            std::size_t expectedCount = 0;
            for (std::size_t i = probe; i < probeEnd; ++i) {
                if (model[i]) {
                    ++expectedCount;
                    if (!expectedRanges.empty() && expectedRanges.back().end == i) {
                        ++expectedRanges.back().end;
                    } else {
                        expectedRanges.push_back({i, i + 1});
                    }
                }
            }
            ASSERT_EQ(tracker.CoveredBytesIn(probe, probeLen), expectedCount);
            ASSERT_EQ(tracker.CoveredRanges(probe, probeLen), expectedRanges);
        }
    }
}

TEST(IntervalTrackerTest, MergesIntoSingleInterval) {
    fc::IntervalTracker tracker(10);
    tracker.Add(0, 2);
    tracker.Add(4, 2);
    tracker.Add(8, 2);
    EXPECT_EQ(tracker.IntervalCount(), 3u);
    tracker.Add(1, 8);
    EXPECT_EQ(tracker.IntervalCount(), 1u);
    EXPECT_TRUE(tracker.IsFull());
}

}  // namespace
