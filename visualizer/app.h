#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "transfer.h"

namespace viz {

// All windows of the visualizer. Flow: Open file -> File -> Transfer (+ Range view).
class App {
public:
    App(Config config, std::string configPath, std::vector<std::string> configMessages);

    // Call once per frame, between ImGui::NewFrame() and ImGui::Render().
    void Draw();
    void OnFileDropped(const std::string& path);

private:
    enum class Screen { Open, File, Transfer };

    void DrawOpenWindow();
    void DrawFileWindow();
    void DrawTransferWindow();
    void DrawGrid();
    void DrawRangeWindow();

    void OpenFile();
    void StartTransfer();
    void CloseTransfer();
    void ShowRange();
    void RefreshCells(bool force);
    fc::Range CellRange(std::size_t cell) const;

    Config config_;
    std::string configPath_;
    std::vector<std::string> configMessages_;
    Screen screen_ = Screen::Open;

    // "Open file" window
    char path_[2048] = {};
    std::string openError_;

    // "File" window
    std::optional<SourceFile> file_;
    Mode mode_ = Mode::Stable;

    // "Transfer" window
    std::unique_ptr<Transfer> transfer_;
    int cellCount_ = 0;
    std::vector<float> cells_;  // how much of every cell has arrived, 0..1
    double lastRefresh_ = -1.0;
    std::uint64_t viewOffset_ = 0;
    std::uint64_t viewLength_ = 0;
    std::string rangeError_;

    // "Range view" window
    bool rangeOpen_ = false;
    bool rangeCut_ = false;
    RangeSnapshot snapshot_;
    std::size_t snapshotFileReady_ = 0;
};

}  // namespace viz
