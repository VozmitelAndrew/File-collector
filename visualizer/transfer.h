#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "config.h"
#include "file_collector/file_collector.h"

namespace viz {

enum class Mode { Stable, Unstable };

const char* ModeName(Mode mode);
const char* TrackerName(Mode mode);

struct SourceFile {
    std::string path;
    std::string name;
    std::vector<std::uint8_t> data;
};

// Cleans a pasted path: removes spaces and quotes around it, turns "~/" into the home folder.
std::string CleanPath(std::string path);

// Reads the whole file. On failure returns nullopt and puts a short message for the user into `error`.
std::optional<SourceFile> LoadFile(const std::string& path, std::size_t maxBytes, std::string& error);

// Cuts a file into chunks for the given mode. Chunks are never empty and together cover the whole file.
std::vector<fc::Range> SplitIntoChunks(std::size_t fileSize, Mode mode, const Config& config, std::uint32_t seed);

// A copy of some bytes of the file, with a flag for every byte: did it arrive or not.
struct RangeSnapshot {
    std::size_t offset = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<bool> received;
    std::size_t receivedCount = 0;
};

// Sends a file to a FileCollector from several threads, like a network would.
// All public methods are safe to call from any thread.
class Transfer {
public:
    enum class State { Running, Done, Stopped };

    Transfer(SourceFile file, Mode mode, const Config& config,
                    std::uint32_t seed = std::random_device{}());
    ~Transfer();

    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;

    // Stops the sender threads and waits for them.
    void Stop();
    // Call it often (the UI does it every frame). When the file is complete,
    // takes it from the collector and compares it with the source.
    void Update();

    State GetState() const { return state_.load(); }
    Mode GetMode() const { return mode_; }
    const SourceFile& File() const { return file_; }
    std::size_t Size() const { return file_.data.size(); }

    std::size_t ReadyBytes() const { return ReadyBytes(0, Size()); }
    std::size_t ReadyBytes(std::size_t pos, std::size_t len) const;
    RangeSnapshot Snapshot(std::size_t pos, std::size_t len) const;

    // Only meaningful when the state is Done.
    bool Matches() const;

    std::size_t ChunksTotal() const { return chunksTotal_; }
    std::size_t ChunksSent() const { return sent_.load(); }
    std::size_t ChunksAccepted() const { return accepted_.load(); }
    std::size_t ChunksDuplicate() const { return duplicates_.load(); }
    std::size_t SendersRunning() const { return sendersRunning_.load(); }

private:
    using Collector = std::variant<fc::BasicFileCollector<fc::IntervalTracker>,
                                   fc::BasicFileCollector<fc::BitmapTracker>>;
    static constexpr std::uint32_t kFileId = 1;

    template <class F>
    decltype(auto) WithCollector(F&& f) const {
        return std::visit(std::forward<F>(f), *collector_);
    }

    void SendLoop(std::vector<fc::Range> chunks, std::chrono::microseconds delay, bool jitter,
                  std::uint32_t seed);
    void Pause(std::chrono::microseconds time) const;
    void JoinSenders();

    const SourceFile file_;
    const Mode mode_;
    std::unique_ptr<Collector> collector_;
    std::size_t chunksTotal_ = 0;

    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Running};
    std::atomic<std::size_t> sent_{0};
    std::atomic<std::size_t> accepted_{0};
    std::atomic<std::size_t> duplicates_{0};
    std::atomic<std::size_t> sendersRunning_{0};

    // Protects result_, matches_ and changes of state_.
    mutable std::mutex resultMutex_;
    std::optional<fc::OwnedFile> result_;
    bool matches_ = false;

    std::mutex joinMutex_;
    std::vector<std::thread> senders_;
};

}  // namespace viz
