#include "transfer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace viz {

const char* ModeName(Mode mode) {
    return mode == Mode::Stable ? "Stable" : "Unstable";
}

const char* TrackerName(Mode mode) {
    return mode == Mode::Stable ? "intervals" : "bitmap";
}

std::string CleanPath(std::string path) {
    const auto begin = path.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    path = path.substr(begin, path.find_last_not_of(" \t\r\n") - begin + 1);

    if (path.size() >= 2 && (path.front() == '"' || path.front() == '\'') && path.back() == path.front()) {
        path = path.substr(1, path.size() - 2);
    }
    if (path.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME")) {
            path = std::string(home) + path.substr(1);
        }
    }
    return path;
}

std::optional<SourceFile> LoadFile(const std::string& rawPath, std::size_t maxBytes, std::string& error) {
    namespace fs = std::filesystem;

    const std::string path = CleanPath(rawPath);
    if (path.empty()) {
        error = "Please enter a file path.";
        return std::nullopt;
    }

    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (!fs::exists(status)) {
        error = ec && ec != std::errc::no_such_file_or_directory ? "Can't access this path." : "File not found.";
        return std::nullopt;
    }
    if (fs::is_directory(status)) {
        error = "This is a folder, not a file.";
        return std::nullopt;
    }
    if (!fs::is_regular_file(status)) {
        error = "This is not a regular file.";
        return std::nullopt;
    }

    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = "Can't get the file size.";
        return std::nullopt;
    }
    if (size == 0) {
        error = "The file is empty. There is nothing to send.";
        return std::nullopt;
    }
    if (size > maxBytes) {
        error = "The file is too big. The limit is " + std::to_string(maxBytes / (1024 * 1024)) +
                " MB (max_file_mb in config.ini).";
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Can't open the file. Maybe you have no permission to read it.";
        return std::nullopt;
    }

    SourceFile file;
    file.path = path;
    file.name = fs::path(path).filename().string();
    file.data.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(file.data.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) {
        error = "Could not read the whole file.";
        return std::nullopt;
    }
    error.clear();
    return file;
}

std::vector<fc::Range> SplitIntoChunks(std::size_t fileSize, Mode mode, const Config& config, std::uint32_t seed) {
    std::vector<fc::Range> chunks;
    if (fileSize == 0) {
        return chunks;
    }

    if (mode == Mode::Stable) {
        // Equal chunks one after another, no overlaps.
        const std::size_t count = std::clamp<std::size_t>(config.stableChunks, 1, fileSize);
        for (std::size_t i = 0; i < count; ++i) {
            chunks.push_back({i * fileSize / count, (i + 1) * fileSize / count});
        }
        return chunks;
    }

    // Small chunks of random size, neighbours overlap, some are sent twice, all shuffled.
    std::mt19937 rng(seed);
    const std::size_t average = std::max<std::size_t>(1, fileSize / std::max(1, config.unstableChunks));
    std::uniform_int_distribution<std::size_t> length(std::max<std::size_t>(1, average / 2),
                                                      average + average / 2);
    std::size_t pos = 0;
    while (pos < fileSize) {
        const std::size_t len = std::min(length(rng), fileSize - pos);
        chunks.push_back({pos, pos + len});
        const std::size_t maxOverlap = (len - 1) * config.unstableOverlapPercent / 100;
        pos += len - std::uniform_int_distribution<std::size_t>(0, maxOverlap)(rng);
    }

    const std::size_t originals = chunks.size();
    const std::size_t copies = originals * config.unstableDuplicatesPercent / 100;
    std::uniform_int_distribution<std::size_t> pick(0, originals - 1);
    for (std::size_t i = 0; i < copies; ++i) {
        const fc::Range copy = chunks[pick(rng)];
        chunks.push_back(copy);
    }
    std::shuffle(chunks.begin(), chunks.end(), rng);
    return chunks;
}

Transfer::Transfer(SourceFile file, Mode mode, const Config& config, std::uint32_t seed)
    : file_(std::move(file)), mode_(mode) {
    if (mode == Mode::Stable) {
        collector_ = std::make_unique<Collector>(std::in_place_index<0>);
    } else {
        collector_ = std::make_unique<Collector>(std::in_place_index<1>);
    }
    WithCollector([&](auto& collector) { collector.CollectFile(kFileId, Size()); });

    const std::vector<fc::Range> chunks = SplitIntoChunks(Size(), mode, config, seed);
    chunksTotal_ = chunks.size();
    if (chunks.empty()) {
        return;
    }

    // Chunk i goes to thread i % threads, so in Stable mode the file fills up from left to right.
    const std::size_t threads = std::min<std::size_t>(std::max(1, config.threads), chunks.size());
    std::vector<std::vector<fc::Range>> parts(threads);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        parts[i % threads].push_back(chunks[i]);
    }

    const std::size_t perThread = (chunks.size() + threads - 1) / threads;
    const auto delay = std::chrono::microseconds(
        static_cast<std::int64_t>(config.transferSeconds) * 1'000'000 / static_cast<std::int64_t>(perThread));

    sendersRunning_ = threads;
    for (std::size_t t = 0; t < threads; ++t) {
        senders_.emplace_back(&Transfer::SendLoop, this, std::move(parts[t]), delay,
                              mode == Mode::Unstable, seed + static_cast<std::uint32_t>(t) + 1);
    }
}

Transfer::~Transfer() {
    Stop();
}

void Transfer::SendLoop(std::vector<fc::Range> chunks, std::chrono::microseconds delay, bool jitter,
                               std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::int64_t> random(0, 2 * delay.count());

    for (const fc::Range& range : chunks) {
        if (stop_.load()) {
            break;
        }
        // A fresh vector per chunk, like a buffer received from the network.
        std::vector<std::uint8_t> chunk(file_.data.begin() + static_cast<std::ptrdiff_t>(range.begin),
                                        file_.data.begin() + static_cast<std::ptrdiff_t>(range.end));
        const fc::ChunkStatus status = WithCollector([&](auto& collector) {
            return collector.OnNewChunk(kFileId, range.begin, std::move(chunk));
        });
        ++sent_;
        if (status == fc::ChunkStatus::Accepted) {
            ++accepted_;
        } else if (status == fc::ChunkStatus::Duplicate) {
            ++duplicates_;
        }
        Pause(jitter ? std::chrono::microseconds(random(rng)) : delay);
    }
    --sendersRunning_;
}

void Transfer::Pause(std::chrono::microseconds time) const {
    // Sleep in small steps, so Stop() does not have to wait long.
    constexpr auto kStep = std::chrono::milliseconds(10);
    while (time.count() > 0 && !stop_.load()) {
        const auto step = std::min<std::chrono::microseconds>(time, kStep);
        std::this_thread::sleep_for(step);
        time -= step;
    }
}

void Transfer::Stop() {
    {
        std::lock_guard lock(resultMutex_);
        stop_ = true;
        if (state_ == State::Running) {
            state_ = State::Stopped;
        }
    }
    JoinSenders();
}

void Transfer::JoinSenders() {
    std::lock_guard lock(joinMutex_);
    for (auto& sender : senders_) {
        if (sender.joinable()) {
            sender.join();
        }
    }
}

void Transfer::Update() {
    if (state_ != State::Running || ReadyBytes() != Size()) {
        return;
    }
    std::lock_guard lock(resultMutex_);
    if (state_ != State::Running) {
        return;
    }
    auto file = WithCollector([](auto& collector) { return collector.TakeFile(kFileId); });
    if (!file) {
        return;
    }
    matches_ = std::equal(file->Span().begin(), file->Span().end(), file_.data.begin(), file_.data.end());
    result_ = std::move(file);
    state_ = State::Done;
    stop_ = true;  // only duplicates are left, no need to send them
}

bool Transfer::Matches() const {
    std::lock_guard lock(resultMutex_);
    return matches_;
}

std::size_t Transfer::ReadyBytes(std::size_t pos, std::size_t len) const {
    std::lock_guard lock(resultMutex_);
    if (result_) {
        return pos >= Size() ? 0 : std::min(len, Size() - pos);
    }
    return WithCollector([&](auto& collector) {
        return collector.ReadyBytes(kFileId, pos, len).value_or(0);
    });
}

RangeSnapshot Transfer::Snapshot(std::size_t pos, std::size_t len) const {
    RangeSnapshot snapshot;
    snapshot.offset = pos;
    if (pos >= Size()) {
        return snapshot;
    }
    len = std::min(len, Size() - pos);
    snapshot.bytes.assign(len, 0);
    snapshot.received.assign(len, false);

    auto copy = [&](std::size_t offset, std::span<const std::uint8_t> bytes) {
        std::copy(bytes.begin(), bytes.end(), snapshot.bytes.begin() + static_cast<std::ptrdiff_t>(offset - pos));
        std::fill_n(snapshot.received.begin() + static_cast<std::ptrdiff_t>(offset - pos), bytes.size(), true);
        snapshot.receivedCount += bytes.size();
    };

    std::lock_guard lock(resultMutex_);
    if (result_) {
        copy(pos, result_->Span().subspan(pos, len));
    } else {
        WithCollector([&](auto& collector) { collector.ReadRange(kFileId, pos, len, copy); });
    }
    return snapshot;
}

}  // namespace viz
