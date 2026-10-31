#include "config.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>

namespace viz {

namespace {

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Takes the value out of `values`, checks it and writes it to `out`.
void Read(std::map<std::string, std::string>& values, const std::string& name, int& out, int min, int max,
          std::vector<std::string>& messages) {
    auto it = values.find(name);
    if (it == values.end()) {
        return;
    }
    const std::string text = it->second;
    values.erase(it);

    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        messages.push_back("Setting '" + name + "': '" + text + "' is not a number, using " +
                           std::to_string(out) + ".");
        return;
    }
    if (value < min || value > max) {
        const int clamped = std::clamp(value, min, max);
        messages.push_back("Setting '" + name + "': " + text + " is out of range [" + std::to_string(min) +
                           ", " + std::to_string(max) + "], using " + std::to_string(clamped) + ".");
        value = clamped;
    }
    out = value;
}

}  // namespace

Config LoadConfig(const std::string& path, std::vector<std::string>& messages) {
    Config config;
    std::ifstream in(path);
    if (!in) {
        messages.push_back("Settings file not found (" + path + "), using defaults.");
        return config;
    }

    std::map<std::string, std::string> values;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        line = Trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            messages.push_back("Settings line " + std::to_string(lineNumber) + " has no '=': " + line);
            continue;
        }
        values[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
    }

    Read(values, "threads", config.threads, 1, 64, messages);
    Read(values, "transfer_seconds", config.transferSeconds, 0, 600, messages);
    Read(values, "cells", config.cells, 1, 4096, messages);
    Read(values, "max_file_mb", config.maxFileMb, 1, 16384, messages);
    Read(values, "max_view_bytes", config.maxViewBytes, 16, 1 << 20, messages);
    Read(values, "stable_chunks", config.stableChunks, 1, 1000000, messages);
    Read(values, "unstable_chunks", config.unstableChunks, 1, 1000000, messages);
    Read(values, "unstable_duplicates_percent", config.unstableDuplicatesPercent, 0, 100, messages);
    Read(values, "unstable_overlap_percent", config.unstableOverlapPercent, 0, 90, messages);

    for (const auto& [name, value] : values) {
        messages.push_back("Unknown setting '" + name + "' is ignored.");
    }
    return config;
}

}  // namespace viz
