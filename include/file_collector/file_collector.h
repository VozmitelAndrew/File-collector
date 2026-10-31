#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "file_collector/bitmap_tracker.h"
#include "file_collector/coverage_tracker.h"
#include "file_collector/interval_tracker.h"
#include "file_collector/owned_file.h"
#include "file_collector/range.h"

namespace fc {

enum class ChunkStatus {
    Accepted,     // the chunk had new bytes
    Duplicate,    // we already had all bytes of the chunk
    UnknownFile,  // no such file: never added, cancelled or already taken
    OutOfRange,   // the chunk goes past the end of the file
};

// Collects files from chunks. Chunks can come in any order, overlap,
// repeat, and come from many threads at once.
//
// How locking works:
//   - filesMutex_ protects the map of files. We hold it only to find a file.
//   - Each file has stateMutex for its trackers. Bytes are copied without it:
//     a writer first "claims" the new bytes, so two writers never write the same byte.
//   - Each file has lifetimeMutex. Readers take it shared, TakeFile takes it
//     exclusive. So reading never stops writing, it only makes TakeFile wait.
template <CoverageTracker Tracker>
class BasicFileCollector {
public:
    BasicFileCollector() = default;
    BasicFileCollector(const BasicFileCollector&) = delete;
    BasicFileCollector& operator=(const BasicFileCollector&) = delete;

    // Adds a file. Adding it again with the same size does nothing,
    // with another size throws std::invalid_argument.
    void CollectFile(std::uint32_t fileId, std::size_t fileSize) {
        // Allocate before locking: a big allocation should not block other files.
        auto fresh = std::make_shared<FileState>(fileSize);
        StatePtr old;  // destroyed after the lock is released

        std::unique_lock lock(filesMutex_);
        auto [it, inserted] = files_.try_emplace(fileId, fresh);
        if (inserted) {
            return;
        }
        {
            std::lock_guard stateLock(it->second->stateMutex);
            if (it->second->taken || it->second->cancelled) {
                old = std::exchange(it->second, std::move(fresh));
                return;
            }
        }
        if (it->second->size != fileSize) {
            throw std::invalid_argument("file " + std::to_string(fileId) +
                                        " is already added with size " +
                                        std::to_string(it->second->size));
        }
    }

    ChunkStatus OnNewChunk(std::uint32_t fileId, std::size_t pos, std::vector<std::uint8_t>&& chunk) {
        const StatePtr state = Find(fileId);
        if (!state) {
            return ChunkStatus::UnknownFile;
        }
        if (pos > state->size || chunk.size() > state->size - pos) {
            return ChunkStatus::OutOfRange;
        }

        // Step 1: claim the bytes nobody has claimed yet.
        std::vector<Range> gaps;
        {
            std::lock_guard lock(state->stateMutex);
            if (state->taken || state->cancelled) {
                return ChunkStatus::UnknownFile;
            }
            if (chunk.empty() || state->complete) {
                return ChunkStatus::Duplicate;
            }
            gaps = state->claimed.AddAndGetGaps(pos, chunk.size());
        }
        if (gaps.empty()) {
            return ChunkStatus::Duplicate;
        }

        // Step 2: copy them. No lock needed, these bytes belong only to us.
        for (const Range& gap : gaps) {
            std::memcpy(state->data.get() + gap.begin, chunk.data() + (gap.begin - pos), gap.Size());
        }

        // Step 3: mark them as ready for readers.
        bool becameComplete = false;
        {
            std::lock_guard lock(state->stateMutex);
            for (const Range& gap : gaps) {
                state->ready.Add(gap.begin, gap.Size());
            }
            if (state->ready.IsFull()) {
                state->complete = true;
                becameComplete = true;
            }
        }
        if (becameComplete) {
            state->completed.notify_all();
        }
        return ChunkStatus::Accepted;
    }

    // Gives away a complete file without copying and removes it from the collector.
    // Returns nullopt if the file is unknown, not complete or already taken.
    std::optional<OwnedFile> TakeFile(std::uint32_t fileId) {
        const StatePtr state = Find(fileId);
        if (!state) {
            return std::nullopt;
        }
        auto file = TakeFrom(*state);
        if (file) {
            Erase(fileId, state);
        }
        return file;
    }

    // Same as TakeFile, but waits up to `timeout` for the file to be complete.
    std::optional<OwnedFile> WaitAndTakeFile(std::uint32_t fileId, std::chrono::milliseconds timeout) {
        const StatePtr state = Find(fileId);
        if (!state) {
            return std::nullopt;
        }
        {
            std::unique_lock lock(state->stateMutex);
            const bool finished = state->completed.wait_for(lock, timeout, [&] {
                return state->complete || state->taken || state->cancelled;
            });
            if (!finished || state->taken || state->cancelled) {
                return std::nullopt;
            }
        }
        auto file = TakeFrom(*state);
        if (file) {
            Erase(fileId, state);
        }
        return file;
    }

    // Reads without copying. Calls reader(bytes) with the ready bytes in a row
    // starting at pos (can be empty). The bytes are valid only inside the call.
    // Do not call TakeFile for the same file from inside the reader: it will wait forever.
    // Returns false if there is no such file.
    template <class Reader>
        requires std::invocable<Reader&, std::span<const std::uint8_t>>
    bool ReadAvailable(std::uint32_t fileId, std::size_t pos, Reader&& reader) const {
        const StatePtr state = Find(fileId);
        if (!state) {
            return false;
        }
        std::shared_lock lifetimeLock(state->lifetimeMutex);
        std::size_t available = 0;
        {
            std::lock_guard lock(state->stateMutex);
            if (state->taken || state->cancelled) {
                return false;
            }
            available = state->ready.ContiguousFrom(pos);
        }
        std::span<const std::uint8_t> bytes;
        if (available > 0) {
            bytes = {state->data.get() + pos, available};
        }
        std::invoke(reader, bytes);
        return true;
    }

    // Reads without copying. Calls reader(offset, bytes) for every ready piece
    // inside [pos, pos + len). Same rules as ReadAvailable.
    template <class Reader>
        requires std::invocable<Reader&, std::size_t, std::span<const std::uint8_t>>
    bool ReadRange(std::uint32_t fileId, std::size_t pos, std::size_t len, Reader&& reader) const {
        const StatePtr state = Find(fileId);
        if (!state) {
            return false;
        }
        std::shared_lock lifetimeLock(state->lifetimeMutex);
        std::vector<Range> pieces;
        {
            std::lock_guard lock(state->stateMutex);
            if (state->taken || state->cancelled) {
                return false;
            }
            pieces = state->ready.CoveredRanges(pos, len);
        }
        for (const Range& piece : pieces) {
            std::invoke(reader, piece.begin,
                        std::span<const std::uint8_t>(state->data.get() + piece.begin, piece.Size()));
        }
        return true;
    }

    // Copies ready bytes in a row starting at pos into out. Returns how many were copied.
    std::size_t CopyAvailable(std::uint32_t fileId, std::size_t pos, std::span<std::uint8_t> out) const {
        std::size_t copied = 0;
        ReadAvailable(fileId, pos, [&](std::span<const std::uint8_t> bytes) {
            copied = std::min(bytes.size(), out.size());
            if (copied > 0) {
                std::memcpy(out.data(), bytes.data(), copied);
            }
        });
        return copied;
    }

    // How many bytes of the file are ready.
    std::optional<std::size_t> ReadyBytes(std::uint32_t fileId) const {
        return ReadyBytes(fileId, 0, SIZE_MAX);
    }

    // How many bytes are ready inside [pos, pos + len).
    std::optional<std::size_t> ReadyBytes(std::uint32_t fileId, std::size_t pos, std::size_t len) const {
        const StatePtr state = Find(fileId);
        if (!state) {
            return std::nullopt;
        }
        std::lock_guard lock(state->stateMutex);
        if (state->taken || state->cancelled) {
            return std::nullopt;
        }
        if (pos == 0 && len >= state->size) {
            return state->ready.CoveredBytes();
        }
        return state->ready.CoveredBytesIn(pos, len);
    }

    // Removes a file we do not need any more, for example if the transfer was aborted.
    bool CancelFile(std::uint32_t fileId) {
        StatePtr state;
        {
            std::unique_lock lock(filesMutex_);
            auto it = files_.find(fileId);
            if (it == files_.end()) {
                return false;
            }
            state = std::move(it->second);
            files_.erase(it);
        }
        {
            std::lock_guard lock(state->stateMutex);
            if (state->taken) {
                return false;
            }
            state->cancelled = true;
        }
        state->completed.notify_all();
        return true;
    }

private:
    struct FileState {
        explicit FileState(std::size_t fileSize)
            // No zero fill on purpose: every byte is written before anyone can read it.
            : size(fileSize),
              data(new std::uint8_t[fileSize]),
              claimed(fileSize),
              ready(fileSize),
              complete(ready.IsFull()) {}

        const std::size_t size;
        std::unique_ptr<std::uint8_t[]> data;

        // Protected by stateMutex.
        Tracker claimed;  // bytes some writer is going to write
        Tracker ready;    // bytes that are written and can be read
        bool complete;
        bool taken = false;
        bool cancelled = false;

        mutable std::shared_mutex lifetimeMutex;
        mutable std::mutex stateMutex;
        std::condition_variable completed;
    };

    using StatePtr = std::shared_ptr<FileState>;

    StatePtr Find(std::uint32_t fileId) const {
        std::shared_lock lock(filesMutex_);
        auto it = files_.find(fileId);
        return it == files_.end() ? nullptr : it->second;
    }

    void Erase(std::uint32_t fileId, const StatePtr& state) {
        std::unique_lock lock(filesMutex_);
        auto it = files_.find(fileId);
        if (it != files_.end() && it->second == state) {
            files_.erase(it);
        }
    }

    static std::optional<OwnedFile> TakeFrom(FileState& state) {
        {
            // Quick check, so TakeFile on an incomplete file does not wait for readers.
            std::lock_guard lock(state.stateMutex);
            if (!state.complete || state.taken || state.cancelled) {
                return std::nullopt;
            }
        }
        std::unique_lock lifetimeLock(state.lifetimeMutex);
        std::lock_guard lock(state.stateMutex);
        if (!state.complete || state.taken || state.cancelled) {
            return std::nullopt;
        }
        state.taken = true;
        return OwnedFile(std::move(state.data), state.size);
    }

    mutable std::shared_mutex filesMutex_;
    std::unordered_map<std::uint32_t, StatePtr> files_;
};

#if defined(FC_TRACKER_BITMAP)
using DefaultTracker = BitmapTracker;
#else
using DefaultTracker = IntervalTracker;
#endif

using FileCollector = BasicFileCollector<DefaultTracker>;

}  // namespace fc
