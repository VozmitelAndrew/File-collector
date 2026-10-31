#pragma once

#include <concepts>
#include <cstddef>
#include <vector>

#include "file_collector/range.h"

namespace fc {

// Remembers which bytes of a file we already have.
// Add methods expect pos + len <= Size(). Read methods cut the range to the file size.
template <class T>
concept CoverageTracker =
    std::constructible_from<T, std::size_t> &&
    requires(T tracker, const T& constTracker, std::size_t pos, std::size_t len) {
        // Marks bytes as received and returns the parts that were new.
        { tracker.AddAndGetGaps(pos, len) } -> std::same_as<std::vector<Range>>;
        { tracker.Add(pos, len) } -> std::same_as<void>;

        { constTracker.Contains(pos, len) } -> std::same_as<bool>;
        // How many bytes in a row we have, starting at pos.
        { constTracker.ContiguousFrom(pos) } -> std::same_as<std::size_t>;
        { constTracker.CoveredBytesIn(pos, len) } -> std::same_as<std::size_t>;
        { constTracker.CoveredRanges(pos, len) } -> std::same_as<std::vector<Range>>;

        { constTracker.CoveredBytes() } -> std::same_as<std::size_t>;
        { constTracker.IsFull() } -> std::same_as<bool>;
        { constTracker.Size() } -> std::same_as<std::size_t>;
    };

}  // namespace fc
