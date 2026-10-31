#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <thread>
#include <vector>

#include "config.h"
#include "transfer.h"

namespace {

using namespace std::chrono_literals;
using viz::Mode;
using viz::Transfer;
namespace fs = std::filesystem;

viz::SourceFile MakeFile(std::size_t size, std::uint32_t seed = 1) {
    std::mt19937 rng(seed);
    viz::SourceFile file;
    file.name = "test.bin";
    file.data.resize(size);
    for (auto& byte : file.data) {
        byte = static_cast<std::uint8_t>(rng());
    }
    return file;
}

viz::Config FastConfig() {
    viz::Config config;
    config.transferSeconds = 0;
    config.threads = 8;
    config.stableChunks = 300;
    config.unstableChunks = 2000;
    return config;
}

viz::Config SlowConfig() {
    viz::Config config = FastConfig();
    config.transferSeconds = 30;
    return config;
}

bool WaitUntil(const std::function<bool()>& condition, std::chrono::seconds timeout = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A temporary folder that is removed at the end of the test.
class TempDir {
public:
    TempDir() : path_(fs::temp_directory_path() / ("fc_test_" + std::to_string(std::random_device{}()))) {
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path File(const std::string& name, std::size_t size) const {
        const fs::path file = path_ / name;
        std::ofstream out(file, std::ios::binary);
        for (std::size_t i = 0; i < size; ++i) {
            out.put(static_cast<char>(i * 7));
        }
        return file;
    }
    const fs::path& Path() const { return path_; }

private:
    fs::path path_;
};

// ---------- Plans ----------

void ExpectChunksCoverFile(const std::vector<fc::Range>& chunks, std::size_t size) {
    std::vector<bool> covered(size, false);
    for (const fc::Range& range : chunks) {
        ASSERT_LT(range.begin, range.end) << "empty chunk";
        ASSERT_LE(range.end, size);
        for (std::size_t i = range.begin; i < range.end; ++i) {
            covered[i] = true;
        }
    }
    for (std::size_t i = 0; i < size; ++i) {
        ASSERT_TRUE(covered[i]) << "byte " << i << " is never sent";
    }
}

TEST(ChunksTest, CoversFileWithoutEmptyChunks) {
    const viz::Config config = FastConfig();
    for (std::size_t size : {1u, 2u, 7u, 100u, 299u, 300u, 301u, 4096u, 100000u}) {
        for (Mode mode : {Mode::Stable, Mode::Unstable}) {
            SCOPED_TRACE(std::string(viz::ModeName(mode)) + ", size " + std::to_string(size));
            ExpectChunksCoverFile(viz::SplitIntoChunks(size, mode, config, 42), size);
        }
    }
}

TEST(ChunksTest, EmptyFileHasNoChunks) {
    EXPECT_TRUE(viz::SplitIntoChunks(0, Mode::Stable, FastConfig(), 1).empty());
    EXPECT_TRUE(viz::SplitIntoChunks(0, Mode::Unstable, FastConfig(), 1).empty());
}

TEST(ChunksTest, StableIsInOrderWithoutOverlaps) {
    const auto chunks = viz::SplitIntoChunks(1000, Mode::Stable, FastConfig(), 1);
    ASSERT_EQ(chunks.size(), 300u);
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i].begin, chunks[i - 1].end);
    }
}

TEST(ChunksTest, MoreChunksThanBytes) {
    viz::Config config = FastConfig();
    config.stableChunks = 1000;
    config.unstableChunks = 1000;
    EXPECT_EQ(viz::SplitIntoChunks(10, Mode::Stable, config, 1).size(), 10u);
    ExpectChunksCoverFile(viz::SplitIntoChunks(10, Mode::Unstable, config, 1), 10);
}

// ---------- Loading files and settings ----------

TEST(LoadFileTest, ReadsFile) {
    TempDir dir;
    const fs::path path = dir.File("data.bin", 1000);
    std::string error;
    auto file = viz::LoadFile(path.string(), 1 << 20, error);
    ASSERT_TRUE(file.has_value()) << error;
    EXPECT_EQ(file->data.size(), 1000u);
    EXPECT_EQ(file->name, "data.bin");
    EXPECT_EQ(file->data[3], static_cast<std::uint8_t>(21));
}

TEST(LoadFileTest, RejectsBadInput) {
    TempDir dir;
    std::string error;

    EXPECT_FALSE(viz::LoadFile("", 1 << 20, error));
    EXPECT_EQ(error, "Please enter a file path.");

    EXPECT_FALSE(viz::LoadFile("   ", 1 << 20, error));
    EXPECT_EQ(error, "Please enter a file path.");

    EXPECT_FALSE(viz::LoadFile((dir.Path() / "missing.bin").string(), 1 << 20, error));
    EXPECT_EQ(error, "File not found.");

    EXPECT_FALSE(viz::LoadFile(dir.Path().string(), 1 << 20, error));
    EXPECT_EQ(error, "This is a folder, not a file.");

    EXPECT_FALSE(viz::LoadFile(dir.File("empty.bin", 0).string(), 1 << 20, error));
    EXPECT_EQ(error, "The file is empty. There is nothing to send.");

    EXPECT_FALSE(viz::LoadFile(dir.File("big.bin", 2 << 20).string(), 1 << 20, error));
    EXPECT_NE(error.find("too big"), std::string::npos);
}

TEST(LoadFileTest, CleansPastedPath) {
    TempDir dir;
    const fs::path path = dir.File("my file.bin", 10);
    std::string error;
    EXPECT_TRUE(viz::LoadFile("  \"" + path.string() + "\"\n", 1 << 20, error)) << error;
    EXPECT_TRUE(viz::LoadFile("'" + path.string() + "'", 1 << 20, error)) << error;
    EXPECT_EQ(viz::CleanPath("\"\""), "");
}

TEST(ConfigTest, ReadsValuesAndReportsProblems) {
    TempDir dir;
    const fs::path path = dir.Path() / "config.ini";
    {
        std::ofstream out(path);
        out << "# comment\n"
               "threads = 2\n"
               "cells=64   # inline comment\n"
               "transfer_seconds = abc\n"
               "max_view_bytes = 5\n"
               "colour = red\n"
               "just some text\n";
    }
    std::vector<std::string> messages;
    const viz::Config config = viz::LoadConfig(path.string(), messages);
    EXPECT_EQ(config.threads, 2);
    EXPECT_EQ(config.cells, 64);
    EXPECT_EQ(config.transferSeconds, viz::Config{}.transferSeconds);
    EXPECT_EQ(config.maxViewBytes, 16);
    EXPECT_EQ(messages.size(), 4u);
}

TEST(ConfigTest, MissingFileGivesDefaults) {
    std::vector<std::string> messages;
    const viz::Config config = viz::LoadConfig("/no/such/config.ini", messages);
    EXPECT_EQ(config.threads, viz::Config{}.threads);
    EXPECT_EQ(messages.size(), 1u);
}

// ---------- Transfer, many threads. Run under TSan and ASan to catch races and leaks. ----------

class TransferTest : public ::testing::TestWithParam<Mode> {};

INSTANTIATE_TEST_SUITE_P(Modes, TransferTest, ::testing::Values(Mode::Stable, Mode::Unstable),
                         [](const auto& info) { return std::string(viz::ModeName(info.param)); });

TEST_P(TransferTest, FinishesAndMatchesSource) {
    Transfer transfer(MakeFile(300000), GetParam(), FastConfig(), 7);
    ASSERT_TRUE(WaitUntil([&] {
        transfer.Update();
        return transfer.GetState() == Transfer::State::Done;
    }));
    EXPECT_TRUE(transfer.Matches());
    EXPECT_EQ(transfer.ReadyBytes(), transfer.Size());
    EXPECT_GE(transfer.ChunksAccepted(), 1u);
    EXPECT_LE(transfer.ChunksSent(), transfer.ChunksTotal());

    const viz::RangeSnapshot snapshot = transfer.Snapshot(1000, 64);
    EXPECT_EQ(snapshot.receivedCount, 64u);
    EXPECT_TRUE(std::equal(snapshot.bytes.begin(), snapshot.bytes.end(), transfer.File().data.begin() + 1000));
}

TEST_P(TransferTest, OneByteFile) {
    Transfer transfer(MakeFile(1), GetParam(), FastConfig(), 7);
    ASSERT_TRUE(WaitUntil([&] {
        transfer.Update();
        return transfer.GetState() == Transfer::State::Done;
    }));
    EXPECT_TRUE(transfer.Matches());
}

TEST_P(TransferTest, EmptyFileIsDoneAtOnce) {
    Transfer transfer(MakeFile(0), GetParam(), FastConfig(), 7);
    transfer.Update();
    EXPECT_EQ(transfer.GetState(), Transfer::State::Done);
    EXPECT_TRUE(transfer.Matches());
    EXPECT_EQ(transfer.ChunksTotal(), 0u);
    EXPECT_TRUE(transfer.Snapshot(0, 10).bytes.empty());
}

TEST_P(TransferTest, StopInTheMiddle) {
    Transfer transfer(MakeFile(500000), GetParam(), SlowConfig(), 7);
    ASSERT_TRUE(WaitUntil([&] { return transfer.ReadyBytes() > 0; }));
    transfer.Stop();

    EXPECT_EQ(transfer.GetState(), Transfer::State::Stopped);
    EXPECT_EQ(transfer.SendersRunning(), 0u);
    const std::size_t ready = transfer.ReadyBytes();
    EXPECT_LT(ready, transfer.Size());

    // Nothing changes after Stop.
    std::this_thread::sleep_for(20ms);
    transfer.Update();
    EXPECT_EQ(transfer.ReadyBytes(), ready);
    EXPECT_EQ(transfer.GetState(), Transfer::State::Stopped);

    // Only bytes that really arrived are marked, and they are correct.
    const viz::RangeSnapshot snapshot = transfer.Snapshot(0, transfer.Size());
    EXPECT_EQ(snapshot.receivedCount, ready);
    for (std::size_t i = 0; i < snapshot.bytes.size(); ++i) {
        if (snapshot.received[i]) {
            ASSERT_EQ(snapshot.bytes[i], transfer.File().data[i]) << "byte " << i;
        }
    }
}

TEST_P(TransferTest, DestroyWhileSending) {
    for (int i = 0; i < 5; ++i) {
        Transfer transfer(MakeFile(200000, i), GetParam(), SlowConfig(), i);
        std::this_thread::sleep_for(2ms);
    }
}

TEST_P(TransferTest, StopTwiceAndFromManyThreads) {
    Transfer transfer(MakeFile(200000), GetParam(), SlowConfig(), 7);
    std::vector<std::thread> stoppers;
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&] { transfer.Stop(); });
    }
    for (auto& thread : stoppers) {
        thread.join();
    }
    transfer.Stop();
    EXPECT_EQ(transfer.GetState(), Transfer::State::Stopped);
}

TEST_P(TransferTest, ReadersWhileSending) {
    Transfer transfer(MakeFile(400000), GetParam(), FastConfig(), 7);
    std::atomic<bool> done{false};
    std::atomic<std::size_t> wrongBytes{0};

    std::vector<std::thread> readers;
    for (unsigned r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            std::mt19937 rng(r);
            std::uniform_int_distribution<std::size_t> pick(0, transfer.Size() - 1);
            while (!done.load()) {
                const std::size_t pos = pick(rng);
                transfer.ReadyBytes(pos, 4096);
                const viz::RangeSnapshot snapshot = transfer.Snapshot(pos, 512);
                for (std::size_t i = 0; i < snapshot.bytes.size(); ++i) {
                    if (snapshot.received[i] && snapshot.bytes[i] != transfer.File().data[pos + i]) {
                        ++wrongBytes;
                    }
                }
            }
        });
    }

    // The UI thread: calls Update all the time, like every frame.
    const bool finished = WaitUntil([&] {
        transfer.Update();
        return transfer.GetState() == Transfer::State::Done;
    });
    done = true;
    for (auto& thread : readers) {
        thread.join();
    }

    ASSERT_TRUE(finished);
    EXPECT_EQ(wrongBytes.load(), 0u);
    EXPECT_TRUE(transfer.Matches());
}

TEST_P(TransferTest, StopRacesWithFinish) {
    for (std::uint32_t round = 0; round < 20; ++round) {
        Transfer transfer(MakeFile(5000, round), GetParam(), FastConfig(), round);
        std::thread updater([&] {
            for (int i = 0; i < 200 && transfer.GetState() == Transfer::State::Running; ++i) {
                transfer.Update();
            }
        });
        transfer.Stop();
        updater.join();
        const auto state = transfer.GetState();
        EXPECT_TRUE(state == Transfer::State::Stopped || state == Transfer::State::Done);
        if (state == Transfer::State::Done) {
            EXPECT_TRUE(transfer.Matches());
        }
    }
}

}  // namespace
