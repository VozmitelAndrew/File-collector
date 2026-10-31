#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <type_traits>

#include "file_collector/file_collector.h"
#include "test_utils.h"

namespace {

using fc::ChunkStatus;
using fc::test::Bytes;
using fc::test::ToBytes;
using namespace std::chrono_literals;

template <class Collector>
class CollectorTest : public ::testing::Test {
protected:
    Collector collector;
};

TYPED_TEST_SUITE(CollectorTest, fc::test::CollectorTypes);

TYPED_TEST(CollectorTest, ExampleFromTask) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ABCD")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 2, ToBytes("CDEF")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 6, ToBytes("GHIJ")), ChunkStatus::Accepted);

    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("ABCDEFGHIJ"));
}

TYPED_TEST(CollectorTest, ReverseOrder) {
    auto& c = this->collector;
    c.CollectFile(1, 9);
    EXPECT_EQ(c.OnNewChunk(1, 6, ToBytes("789")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 3, ToBytes("456")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("123")), ChunkStatus::Accepted);

    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("123456789"));
}

TYPED_TEST(CollectorTest, DuplicatesAreDetected) {
    auto& c = this->collector;
    c.CollectFile(1, 8);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("abcd")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("abcd")), ChunkStatus::Duplicate);
    EXPECT_EQ(c.OnNewChunk(1, 1, ToBytes("bc")), ChunkStatus::Duplicate);
    EXPECT_EQ(c.OnNewChunk(1, 2, ToBytes("cdef")), ChunkStatus::Accepted);
    EXPECT_EQ(c.ReadyBytes(1), 6u);
}

TYPED_TEST(CollectorTest, ChunkCoveringEverythingAfterPieces) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    EXPECT_EQ(c.OnNewChunk(1, 2, ToBytes("cd")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 6, ToBytes("gh")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("abcdefghij")), ChunkStatus::Accepted);

    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("abcdefghij"));
}

TYPED_TEST(CollectorTest, UnknownFile) {
    auto& c = this->collector;
    EXPECT_EQ(c.OnNewChunk(42, 0, ToBytes("x")), ChunkStatus::UnknownFile);
    EXPECT_FALSE(c.TakeFile(42).has_value());
    EXPECT_FALSE(c.ReadyBytes(42).has_value());
    EXPECT_FALSE(c.CancelFile(42));
    EXPECT_FALSE(c.ReadAvailable(42, 0, [](std::span<const std::uint8_t>) { FAIL(); }));
}

TYPED_TEST(CollectorTest, OutOfRange) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    EXPECT_EQ(c.OnNewChunk(1, 5, ToBytes("x")), ChunkStatus::OutOfRange);
    EXPECT_EQ(c.OnNewChunk(1, 2, ToBytes("xyz")), ChunkStatus::OutOfRange);
    EXPECT_EQ(c.OnNewChunk(1, SIZE_MAX, ToBytes("xy")), ChunkStatus::OutOfRange);
    EXPECT_EQ(c.OnNewChunk(1, 1, ToBytes("xyz")), ChunkStatus::Accepted);
    EXPECT_EQ(c.ReadyBytes(1), 3u);
}

TYPED_TEST(CollectorTest, EmptyChunk) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    EXPECT_EQ(c.OnNewChunk(1, 2, Bytes{}), ChunkStatus::Duplicate);
    EXPECT_EQ(c.OnNewChunk(1, 4, Bytes{}), ChunkStatus::Duplicate);
    EXPECT_EQ(c.ReadyBytes(1), 0u);
}

TYPED_TEST(CollectorTest, ZeroSizeFileIsReadyImmediately) {
    auto& c = this->collector;
    c.CollectFile(1, 0);
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(file->Empty());
}

TYPED_TEST(CollectorTest, CollectFileTwice) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ab")), ChunkStatus::Accepted);
    EXPECT_NO_THROW(c.CollectFile(1, 4));
    EXPECT_EQ(c.ReadyBytes(1), 2u);
    EXPECT_THROW(c.CollectFile(1, 5), std::invalid_argument);
}

TYPED_TEST(CollectorTest, TakeIncompleteFileFails) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ab")), ChunkStatus::Accepted);
    EXPECT_FALSE(c.TakeFile(1).has_value());

    EXPECT_EQ(c.OnNewChunk(1, 2, ToBytes("cd")), ChunkStatus::Accepted);
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("abcd"));
}

TYPED_TEST(CollectorTest, FileIsGoneAfterTake) {
    auto& c = this->collector;
    c.CollectFile(1, 2);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ab")), ChunkStatus::Accepted);
    ASSERT_TRUE(c.TakeFile(1).has_value());

    EXPECT_FALSE(c.TakeFile(1).has_value());
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ab")), ChunkStatus::UnknownFile);
    EXPECT_FALSE(c.ReadyBytes(1).has_value());
    EXPECT_FALSE(c.CancelFile(1));
}

TYPED_TEST(CollectorTest, SameIdCanBeReusedAfterTake) {
    auto& c = this->collector;
    c.CollectFile(1, 2);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ab")), ChunkStatus::Accepted);
    ASSERT_TRUE(c.TakeFile(1).has_value());

    c.CollectFile(1, 3);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("xyz")), ChunkStatus::Accepted);
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("xyz"));
}

TYPED_TEST(CollectorTest, ReadAvailableOnPartialFile) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ABCD")), ChunkStatus::Accepted);
    EXPECT_EQ(c.OnNewChunk(1, 6, ToBytes("GHIJ")), ChunkStatus::Accepted);

    auto readAt = [&](std::size_t pos) {
        Bytes result;
        EXPECT_TRUE(c.ReadAvailable(1, pos, [&](std::span<const std::uint8_t> bytes) {
            result = ToBytes(bytes);
        }));
        return result;
    };

    EXPECT_EQ(readAt(0), ToBytes("ABCD"));
    EXPECT_EQ(readAt(2), ToBytes("CD"));
    EXPECT_TRUE(readAt(4).empty());
    EXPECT_EQ(readAt(6), ToBytes("GHIJ"));
    EXPECT_TRUE(readAt(10).empty());
    EXPECT_TRUE(readAt(100).empty());
}

TYPED_TEST(CollectorTest, ReadAvailableDoesNotCopy) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("abcd")), ChunkStatus::Accepted);

    const std::uint8_t* seen = nullptr;
    c.ReadAvailable(1, 0, [&](std::span<const std::uint8_t> bytes) { seen = bytes.data(); });
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(seen, file->Data());
}

TYPED_TEST(CollectorTest, ReadRangeGivesReadyPieces) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    c.OnNewChunk(1, 0, ToBytes("ABC"));
    c.OnNewChunk(1, 5, ToBytes("FG"));
    c.OnNewChunk(1, 9, ToBytes("J"));

    std::vector<std::pair<std::size_t, Bytes>> pieces;
    EXPECT_TRUE(c.ReadRange(1, 1, 8, [&](std::size_t offset, std::span<const std::uint8_t> bytes) {
        pieces.emplace_back(offset, ToBytes(bytes));
    }));
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_EQ(pieces[0], std::make_pair(std::size_t{1}, ToBytes("BC")));
    EXPECT_EQ(pieces[1], std::make_pair(std::size_t{5}, ToBytes("FG")));

    bool called = false;
    EXPECT_TRUE(c.ReadRange(1, 3, 2, [&](std::size_t, std::span<const std::uint8_t>) { called = true; }));
    EXPECT_FALSE(called);
    EXPECT_FALSE(c.ReadRange(7, 0, 1, [&](std::size_t, std::span<const std::uint8_t>) { called = true; }));
    EXPECT_FALSE(called);
}

TYPED_TEST(CollectorTest, ReadyBytesInRange) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    c.OnNewChunk(1, 0, ToBytes("ABC"));
    c.OnNewChunk(1, 5, ToBytes("FG"));
    EXPECT_EQ(c.ReadyBytes(1, 0, 10), 5u);
    EXPECT_EQ(c.ReadyBytes(1, 2, 4), 2u);
    EXPECT_EQ(c.ReadyBytes(1, 3, 2), 0u);
    EXPECT_EQ(c.ReadyBytes(1, 6, 100), 1u);
    EXPECT_EQ(c.ReadyBytes(1, 50, 5), 0u);
    EXPECT_FALSE(c.ReadyBytes(9, 0, 5).has_value());
}

TYPED_TEST(CollectorTest, CopyAvailable) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ABCDEF")), ChunkStatus::Accepted);

    std::array<std::uint8_t, 4> small{};
    EXPECT_EQ(c.CopyAvailable(1, 1, small), 4u);
    EXPECT_EQ(ToBytes(small), ToBytes("BCDE"));

    std::array<std::uint8_t, 16> big{};
    EXPECT_EQ(c.CopyAvailable(1, 3, big), 3u);
    EXPECT_EQ(ToBytes(std::span(big).first(3)), ToBytes("DEF"));

    EXPECT_EQ(c.CopyAvailable(1, 7, big), 0u);
    EXPECT_EQ(c.CopyAvailable(99, 0, big), 0u);
}

TYPED_TEST(CollectorTest, ReadyBytesTracksProgress) {
    auto& c = this->collector;
    c.CollectFile(1, 10);
    EXPECT_EQ(c.ReadyBytes(1), 0u);
    c.OnNewChunk(1, 0, ToBytes("ABCD"));
    EXPECT_EQ(c.ReadyBytes(1), 4u);
    c.OnNewChunk(1, 2, ToBytes("CDEF"));
    EXPECT_EQ(c.ReadyBytes(1), 6u);
}

TYPED_TEST(CollectorTest, CancelFile) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("ab")), ChunkStatus::Accepted);
    EXPECT_TRUE(c.CancelFile(1));

    EXPECT_FALSE(c.CancelFile(1));
    EXPECT_EQ(c.OnNewChunk(1, 2, ToBytes("cd")), ChunkStatus::UnknownFile);
    EXPECT_FALSE(c.TakeFile(1).has_value());
    EXPECT_FALSE(c.ReadyBytes(1).has_value());
}

TYPED_TEST(CollectorTest, FilesAreIndependent) {
    auto& c = this->collector;
    c.CollectFile(1, 3);
    c.CollectFile(2, 3);
    c.OnNewChunk(1, 0, ToBytes("aaa"));
    c.OnNewChunk(2, 0, ToBytes("bb"));

    auto first = c.TakeFile(1);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(ToBytes(first->Span()), ToBytes("aaa"));
    EXPECT_FALSE(c.TakeFile(2).has_value());
    EXPECT_EQ(c.ReadyBytes(2), 2u);
}

TYPED_TEST(CollectorTest, WaitAndTakeTimesOut) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    c.OnNewChunk(1, 0, ToBytes("ab"));

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(c.WaitAndTakeFile(1, 50ms).has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 50ms);
    EXPECT_EQ(c.ReadyBytes(1), 2u);
}

TYPED_TEST(CollectorTest, WaitAndTakeReturnsCompleteFileAtOnce) {
    auto& c = this->collector;
    c.CollectFile(1, 2);
    c.OnNewChunk(1, 0, ToBytes("ok"));
    auto file = c.WaitAndTakeFile(1, 0ms);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("ok"));
}

TYPED_TEST(CollectorTest, RandomOrderOverlappingChunks) {
    auto& c = this->collector;
    std::mt19937 rng(7);
    for (std::uint32_t id = 0; id < 20; ++id) {
        const std::size_t size = std::uniform_int_distribution<std::size_t>(1, 5000)(rng);
        const Bytes original = fc::test::MakeRandomFile(size, rng);
        c.CollectFile(id, size);
        for (auto& chunk : fc::test::MakeShuffledChunks(original, 300, rng)) {
            const auto status = c.OnNewChunk(id, chunk.pos, std::move(chunk.data));
            ASSERT_TRUE(status == ChunkStatus::Accepted || status == ChunkStatus::Duplicate);
        }
        auto file = c.TakeFile(id);
        ASSERT_TRUE(file.has_value());
        EXPECT_EQ(ToBytes(file->Span()), original);
    }
}

TEST(OwnedFileTest, ReleaseGivesUpBuffer) {
    fc::OwnedFile file(std::unique_ptr<std::uint8_t[]>(new std::uint8_t[3]{1, 2, 3}), 3);
    auto raw = file.Release();
    EXPECT_TRUE(file.Empty());
    EXPECT_EQ(file.Data(), nullptr);
    EXPECT_EQ(raw[2], 3);
}

TEST(FileCollectorAliasTest, UsesTrackerSelectedAtBuildTime) {
#if defined(FC_TRACKER_BITMAP)
    static_assert(std::is_same_v<fc::DefaultTracker, fc::BitmapTracker>);
#else
    static_assert(std::is_same_v<fc::DefaultTracker, fc::IntervalTracker>);
#endif
    fc::FileCollector c;
    c.CollectFile(1, 10);
    c.OnNewChunk(1, 0, ToBytes("ABCD"));
    c.OnNewChunk(1, 2, ToBytes("CDEF"));
    c.OnNewChunk(1, 6, ToBytes("GHIJ"));
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("ABCDEFGHIJ"));
}

}  // namespace
