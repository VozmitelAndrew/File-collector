#pragma once

#include <cstddef>

namespace fc {

// Bytes from begin (included) to end (not included).
struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;

    std::size_t Size() const noexcept { return end - begin; }

    friend bool operator==(const Range&, const Range&) = default;
};

}  // namespace fc
