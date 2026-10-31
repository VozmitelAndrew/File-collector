#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace viz {

struct Config {
    int threads = 4;
    int transferSeconds = 8;
    int cells = 1024;
    int maxFileMb = 512;
    int maxViewBytes = 4096;
    int stableChunks = 400;
    int unstableChunks = 3000;
    int unstableDuplicatesPercent = 20;
    int unstableOverlapPercent = 50;

    std::size_t MaxFileBytes() const { return static_cast<std::size_t>(maxFileMb) * 1024 * 1024; }
};

// Reads a config file. A missing file is fine: the defaults are used.
// Problems (unknown names, bad values) are added to `messages`.
Config LoadConfig(const std::string& path, std::vector<std::string>& messages);

}  // namespace viz
