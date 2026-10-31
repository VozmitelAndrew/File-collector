#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "file_collector/file_collector.h"
#include "test_utils.h"

namespace {

using fc::ChunkStatus;
using fc::test::Bytes;
using fc::test::Chunk;
using fc::test::ToBytes;
using namespace std::chrono_literals;

constexpr std::size_t kThreads = 8;

// Releases all threads at once so that they really run concurrently.
class StartGate {
public:
    void Wait() const {
        while (!open_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    void Open() { open_.store(true, std::memory_order_release); }

private:
    std::atomic<bool> open_{false};
};

std::vector<std::vector<Chunk>> SplitBetweenThreads(std::vector<Chunk> chunks, std::size_t threads) {
    std::vector<std::vector<Chunk>> parts(threads);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        parts[i % threads].push_back(std::move(chunks[i]));
    }
    return parts;
}

void JoinAll(std::vector<std::thread>& threads) {
    for (auto& thread : threads) {
        thread.join();
    }
}

template <class Collector>
class CollectorMtTest : public ::testing::Test {
protected:
    Collector collector;
    std::mt19937 rng{2026};
};

TYPED_TEST_SUITE(CollectorMtTest, fc::test::CollectorTypes);

TYPED_TEST(CollectorMtTest, ManyWritersOneFile) {
    auto& c = this->collector;
    const Bytes original = fc::test::MakeRandomFile(1 << 20, this->rng);
    c.CollectFile(1, original.size());
    auto parts = SplitBetweenThreads(fc::test::MakeShuffledChunks(original, 4096, this->rng), kThreads);

    StartGate gate;
    std::atomic<std::size_t> unexpected{0};
    std::vector<std::thread> writers;
    for (auto& part : parts) {
        writers.emplace_back([&c, &gate, &unexpected, &part] {
            gate.Wait();
            for (auto& chunk : part) {
                const auto status = c.OnNewChunk(1, chunk.pos, std::move(chunk.data));
                if (status != ChunkStatus::Accepted && status != ChunkStatus::Duplicate) {
                    ++unexpected;
                }
            }
        });
    }
    gate.Open();
    JoinAll(writers);

    EXPECT_EQ(unexpected.load(), 0u);
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), original);
}

TYPED_TEST(CollectorMtTest, ManyWritersManyFiles) {
    auto& c = this->collector;
    constexpr std::uint32_t kFiles = 16;

    std::vector<Bytes> originals;
    std::vector<std::pair<std::uint32_t, Chunk>> all;
    for (std::uint32_t id = 0; id < kFiles; ++id) {
        originals.push_back(fc::test::MakeRandomFile(64 * 1024 + id * 333, this->rng));
        c.CollectFile(id, originals.back().size());
        for (auto& chunk : fc::test::MakeShuffledChunks(originals.back(), 2048, this->rng)) {
            all.emplace_back(id, std::move(chunk));
        }
    }
    std::shuffle(all.begin(), all.end(), this->rng);

    std::vector<std::vector<std::pair<std::uint32_t, Chunk>>> parts(kThreads);
    for (std::size_t i = 0; i < all.size(); ++i) {
        parts[i % kThreads].push_back(std::move(all[i]));
    }

    StartGate gate;
    std::vector<std::thread> writers;
    for (auto& part : parts) {
        writers.emplace_back([&c, &gate, &part] {
            gate.Wait();
            for (auto& [id, chunk] : part) {
                c.OnNewChunk(id, chunk.pos, std::move(chunk.data));
            }
        });
    }
    gate.Open();
    JoinAll(writers);

    for (std::uint32_t id = 0; id < kFiles; ++id) {
        auto file = c.TakeFile(id);
        ASSERT_TRUE(file.has_value()) << "file " << id;
        EXPECT_EQ(ToBytes(file->Span()), originals[id]) << "file " << id;
    }
}

TYPED_TEST(CollectorMtTest, ReadersSeeOnlyCorrectBytesWhileWriting) {
    auto& c = this->collector;
    const Bytes original = fc::test::MakeRandomFile(256 * 1024, this->rng);
    c.CollectFile(1, original.size());
    auto parts = SplitBetweenThreads(fc::test::MakeShuffledChunks(original, 2048, this->rng), kThreads);

    StartGate gate;
    std::atomic<bool> writersDone{false};
    std::atomic<std::size_t> mismatches{0};
    std::atomic<std::size_t> bytesSeen{0};

    std::vector<std::thread> readers;
    for (unsigned seed = 0; seed < 3; ++seed) {
        readers.emplace_back([&, seed] {
            std::mt19937 local(seed);
            std::uniform_int_distribution<std::size_t> pick(0, original.size() - 1);
            gate.Wait();
            while (!writersDone.load()) {
                const std::size_t pos = pick(local);
                c.ReadAvailable(1, pos, [&](std::span<const std::uint8_t> bytes) {
                    if (!bytes.empty() && std::memcmp(bytes.data(), original.data() + pos, bytes.size()) != 0) {
                        ++mismatches;
                    }
                    bytesSeen += bytes.size();
                });
            }
        });
    }

    std::vector<std::thread> writers;
    for (auto& part : parts) {
        writers.emplace_back([&c, &gate, &part] {
            gate.Wait();
            for (auto& chunk : part) {
                c.OnNewChunk(1, chunk.pos, std::move(chunk.data));
            }
        });
    }

    gate.Open();
    JoinAll(writers);
    writersDone = true;
    JoinAll(readers);

    EXPECT_EQ(mismatches.load(), 0u);
    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), original);
}

TYPED_TEST(CollectorMtTest, WaitAndTakeWakesUpWhenFileIsComplete) {
    auto& c = this->collector;
    const Bytes original = fc::test::MakeRandomFile(128 * 1024, this->rng);
    c.CollectFile(1, original.size());
    auto parts = SplitBetweenThreads(fc::test::MakeShuffledChunks(original, 1024, this->rng), kThreads);

    std::optional<fc::OwnedFile> result;
    std::thread waiter([&] { result = c.WaitAndTakeFile(1, 30s); });

    StartGate gate;
    std::vector<std::thread> writers;
    for (auto& part : parts) {
        writers.emplace_back([&c, &gate, &part] {
            gate.Wait();
            for (auto& chunk : part) {
                c.OnNewChunk(1, chunk.pos, std::move(chunk.data));
            }
        });
    }
    gate.Open();
    JoinAll(writers);
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(ToBytes(result->Span()), original);
}

TYPED_TEST(CollectorMtTest, OnlyOneThreadTakesTheFile) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    c.OnNewChunk(1, 0, ToBytes("data"));

    StartGate gate;
    std::atomic<int> winners{0};
    std::vector<std::thread> takers;
    for (std::size_t i = 0; i < kThreads; ++i) {
        takers.emplace_back([&] {
            gate.Wait();
            if (c.TakeFile(1)) {
                ++winners;
            }
        });
    }
    gate.Open();
    JoinAll(takers);
    EXPECT_EQ(winners.load(), 1);
}

TYPED_TEST(CollectorMtTest, TakeWaitsForActiveReader) {
    auto& c = this->collector;
    c.CollectFile(1, 4);
    c.OnNewChunk(1, 0, ToBytes("data"));

    std::atomic<bool> readerInside{false};
    std::atomic<bool> readerFinished{false};
    std::thread reader([&] {
        c.ReadAvailable(1, 0, [&](std::span<const std::uint8_t> bytes) {
            readerInside = true;
            std::this_thread::sleep_for(100ms);
            EXPECT_EQ(ToBytes(bytes), ToBytes("data"));
            readerFinished = true;
        });
    });

    while (!readerInside.load()) {
        std::this_thread::yield();
    }
    auto file = c.TakeFile(1);
    EXPECT_TRUE(readerFinished.load());
    reader.join();

    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("data"));
}

TYPED_TEST(CollectorMtTest, ReadingDoesNotBlockWriting) {
    auto& c = this->collector;
    c.CollectFile(1, 8);
    c.OnNewChunk(1, 0, ToBytes("abcd"));

    std::atomic<bool> readerInside{false};
    std::atomic<bool> writerDone{false};
    std::thread reader([&] {
        c.ReadAvailable(1, 0, [&](std::span<const std::uint8_t>) {
            readerInside = true;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!writerDone.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        });
    });

    while (!readerInside.load()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(c.OnNewChunk(1, 4, ToBytes("efgh")), ChunkStatus::Accepted);
    writerDone = true;
    reader.join();

    auto file = c.TakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(ToBytes(file->Span()), ToBytes("abcdefgh"));
}

TYPED_TEST(CollectorMtTest, CancelWhileWriting) {
    auto& c = this->collector;
    const Bytes original = fc::test::MakeRandomFile(512 * 1024, this->rng);
    c.CollectFile(1, original.size());
    auto parts = SplitBetweenThreads(fc::test::MakeShuffledChunks(original, 1024, this->rng), kThreads);

    StartGate gate;
    std::atomic<std::size_t> unexpected{0};
    std::vector<std::thread> writers;
    for (auto& part : parts) {
        writers.emplace_back([&c, &gate, &unexpected, &part] {
            gate.Wait();
            for (auto& chunk : part) {
                const auto status = c.OnNewChunk(1, chunk.pos, std::move(chunk.data));
                if (status == ChunkStatus::OutOfRange) {
                    ++unexpected;
                }
            }
        });
    }
    std::thread canceller([&] {
        gate.Wait();
        std::this_thread::sleep_for(1ms);
        c.CancelFile(1);
    });

    gate.Open();
    JoinAll(writers);
    canceller.join();

    EXPECT_EQ(unexpected.load(), 0u);
    EXPECT_FALSE(c.ReadyBytes(1).has_value());
    EXPECT_EQ(c.OnNewChunk(1, 0, ToBytes("x")), ChunkStatus::UnknownFile);
}

TYPED_TEST(CollectorMtTest, CollectAndTakeDifferentFilesConcurrently) {
    auto& c = this->collector;
    constexpr std::uint32_t kPerThread = 200;

    StartGate gate;
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    for (std::uint32_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            gate.Wait();
            for (std::uint32_t i = 0; i < kPerThread; ++i) {
                const std::uint32_t id = t * kPerThread + i;
                c.CollectFile(id, 4);
                c.OnNewChunk(id, 2, ToBytes("cd"));
                c.OnNewChunk(id, 0, ToBytes("ab"));
                auto file = c.TakeFile(id);
                if (!file || ToBytes(file->Span()) != ToBytes("abcd")) {
                    ++failures;
                }
            }
        });
    }
    gate.Open();
    JoinAll(workers);
    EXPECT_EQ(failures.load(), 0u);
}

}  // namespace
