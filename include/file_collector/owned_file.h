#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace fc {

// A complete file. The buffer now belongs to whoever holds this object.
class OwnedFile {
public:
    OwnedFile() = default;
    OwnedFile(std::unique_ptr<std::uint8_t[]> data, std::size_t size) noexcept
        : data_(std::move(data)), size_(size) {}

    std::uint8_t* Data() noexcept { return data_.get(); }
    const std::uint8_t* Data() const noexcept { return data_.get(); }
    std::size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }

    std::span<std::uint8_t> Span() noexcept { return {data_.get(), size_}; }
    std::span<const std::uint8_t> Span() const noexcept { return {data_.get(), size_}; }

    // Hands the raw buffer over to the caller.
    std::unique_ptr<std::uint8_t[]> Release() noexcept {
        size_ = 0;
        return std::move(data_);
    }

private:
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_ = 0;
};

}  // namespace fc
