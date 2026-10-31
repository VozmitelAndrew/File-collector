#include "app.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace viz {

namespace {

const ImVec4 kRed(0.95f, 0.40f, 0.40f, 1.0f);
const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kYellow(0.95f, 0.80f, 0.35f, 1.0f);

constexpr int kMaxCells = 4096;
constexpr std::size_t kBytesPerLine = 16;

std::string FormatSize(std::size_t bytes) {
    char text[64];
    if (bytes < 1024) {
        std::snprintf(text, sizeof(text), "%zu B", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::snprintf(text, sizeof(text), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return text;
}

// 1234567 -> "1,234,567"
std::string WithCommas(std::size_t number) {
    std::string digits = std::to_string(number);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return digits;
}

// Gray (or red after Stop) when nothing arrived, green when everything arrived, a mix in between.
ImU32 CellColor(float progress, bool stopped) {
    const ImVec4 empty = stopped ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f) : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    const ImVec4 full(0.20f, 0.70f, 0.30f, 1.0f);
    const ImVec4 color(empty.x + (full.x - empty.x) * progress, empty.y + (full.y - empty.y) * progress,
                       empty.z + (full.z - empty.z) * progress, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(color);
}

// One line of the hex view: offset, 16 bytes in hex, the same bytes as text. "??" = not arrived.
std::string FormatLine(const RangeSnapshot& snapshot, std::size_t line) {
    const std::size_t begin = line * kBytesPerLine;
    const std::size_t end = std::min(begin + kBytesPerLine, snapshot.bytes.size());
    char text[32];
    std::snprintf(text, sizeof(text), "%10zu  ", snapshot.offset + begin);
    std::string result = text;

    for (std::size_t i = 0; i < kBytesPerLine; ++i) {
        if (i == kBytesPerLine / 2) {
            result += ' ';
        }
        const std::size_t index = begin + i;
        if (index >= end) {
            result += "   ";
        } else if (!snapshot.received[index]) {
            result += "?? ";
        } else {
            std::snprintf(text, sizeof(text), "%02X ", snapshot.bytes[index]);
            result += text;
        }
    }

    result += " |";
    for (std::size_t index = begin; index < end; ++index) {
        const unsigned char byte = snapshot.bytes[index];
        if (!snapshot.received[index]) {
            result += ' ';
        } else {
            result += std::isprint(byte) ? static_cast<char>(byte) : '.';
        }
    }
    result += '|';
    return result;
}

bool LineIsComplete(const RangeSnapshot& snapshot, std::size_t line) {
    const std::size_t begin = line * kBytesPerLine;
    const std::size_t end = std::min(begin + kBytesPerLine, snapshot.bytes.size());
    for (std::size_t i = begin; i < end; ++i) {
        if (!snapshot.received[i]) {
            return false;
        }
    }
    return true;
}

void CenterNextWindow() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

}  // namespace

App::App(Config config, std::string configPath, std::vector<std::string> configMessages)
    : config_(config), configPath_(std::move(configPath)), configMessages_(std::move(configMessages)) {}

void App::Draw() {
    if (transfer_) {
        transfer_->Update();
        RefreshCells(false);
    }

    switch (screen_) {
        case Screen::Open:
            DrawOpenWindow();
            break;
        case Screen::File:
            DrawFileWindow();
            break;
        case Screen::Transfer:
            DrawTransferWindow();
            break;
    }

    if (rangeOpen_ && transfer_) {
        DrawRangeWindow();
    }
}

void App::OnFileDropped(const std::string& path) {
    if (screen_ != Screen::Open) {
        return;
    }
    std::snprintf(path_, sizeof(path_), "%s", path.c_str());
    OpenFile();
}

// ---------- Open file ----------

void App::DrawOpenWindow() {
    CenterNextWindow();
    if (ImGui::Begin("Open file", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Paste the path to a file and press Open.");
        ImGui::SetNextItemWidth(460);
        const bool enter = ImGui::InputText("##path", path_, sizeof(path_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Open") || enter) {
            OpenFile();
        }
        ImGui::TextDisabled("You can also drag a file into this window.");

        if (!openError_.empty()) {
            ImGui::TextColored(kRed, "%s", openError_.c_str());
        }

        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520);
        ImGui::TextDisabled("Settings: %s", configPath_.c_str());
        for (const std::string& message : configMessages_) {
            ImGui::PushStyleColor(ImGuiCol_Text, kYellow);
            ImGui::TextWrapped("%s", message.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void App::OpenFile() {
    std::string error;
    auto file = LoadFile(path_, config_.MaxFileBytes(), error);
    if (!file) {
        openError_ = error;
        return;
    }
    openError_.clear();
    file_ = std::move(file);
    mode_ = Mode::Stable;
    screen_ = Screen::File;
}

// ---------- File ----------

void App::DrawFileWindow() {
    CenterNextWindow();
    if (ImGui::Begin("File", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Name: %s", file_->name.c_str());
        ImGui::Text("Size: %s (%s bytes)", FormatSize(file_->data.size()).c_str(),
                    WithCommas(file_->data.size()).c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420);
        ImGui::TextDisabled("%s", file_->path.c_str());
        ImGui::PopTextWrapPos();

        ImGui::Separator();
        ImGui::TextUnformatted("How should the data arrive?");
        if (ImGui::BeginTabBar("mode")) {
            if (ImGui::BeginTabItem("Stable")) {
                mode_ = Mode::Stable;
                ImGui::BulletText("Big chunks that come almost in order.");
                ImGui::BulletText("No duplicates, no overlaps.");
                ImGui::BulletText("Tracker: intervals.");
                ImGui::TextDisabled("Few gaps, so a short list of intervals is enough.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Unstable")) {
                mode_ = Mode::Unstable;
                ImGui::BulletText("Small chunks in random order.");
                ImGui::BulletText("Some chunks overlap, some come twice.");
                ImGui::BulletText("Tracker: bitmap.");
                ImGui::TextDisabled("Many small gaps, so one bit per byte works better.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("Transfer")) {
            StartTransfer();
        } else {
            ImGui::SameLine();
            if (ImGui::Button("Back")) {
                file_.reset();
                screen_ = Screen::Open;
            }
        }
    }
    ImGui::End();
}

void App::StartTransfer() {
    transfer_ = std::make_unique<Transfer>(std::move(*file_), mode_, config_);
    file_.reset();

    const std::size_t size = transfer_->Size();
    cellCount_ = static_cast<int>(std::clamp<std::size_t>(config_.cells, 1, std::min<std::size_t>(size, kMaxCells)));
    RefreshCells(true);

    viewOffset_ = 0;
    viewLength_ = std::min<std::size_t>(size, config_.maxViewBytes);
    rangeError_.clear();
    rangeOpen_ = false;
    screen_ = Screen::Transfer;
}

// ---------- Transfer ----------

void App::DrawTransferWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos + ImVec2(20, 20), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(viewport->WorkSize - ImVec2(40, 40), ImGuiCond_Appearing);
    if (!ImGui::Begin("Transfer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const auto state = transfer_->GetState();
    const std::size_t size = transfer_->Size();

    // Top row: buttons and status.
    ImGui::BeginDisabled(state != Transfer::State::Running);
    if (ImGui::Button("Stop")) {
        transfer_->Stop();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New file")) {
        CloseTransfer();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    switch (state) {
        case Transfer::State::Running:
            ImGui::Text("Sending... (%zu threads)", transfer_->SendersRunning());
            break;
        case Transfer::State::Done:
            if (transfer_->Matches()) {
                ImGui::TextColored(kGreen, "Done. The file matches the source.");
            } else {
                ImGui::TextColored(kRed, "Done, but the file does NOT match the source!");
            }
            break;
        case Transfer::State::Stopped:
            ImGui::TextColored(kRed, "Stopped. Red cells are data that did not arrive.");
            break;
    }

    ImGui::Text("File: %s, %s", transfer_->File().name.c_str(), FormatSize(size).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  Mode: %s, tracker: %s", ModeName(transfer_->GetMode()), TrackerName(transfer_->GetMode()));

    const std::size_t ready = transfer_->ReadyBytes();
    const float progress = static_cast<float>(static_cast<double>(ready) / static_cast<double>(size));
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "%.1f%%  (%s of %s)", progress * 100.0f, FormatSize(ready).c_str(),
                  FormatSize(size).c_str());
    ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
    ImGui::Text("Chunks: %zu of %zu sent, %zu with new data, %zu duplicates", transfer_->ChunksSent(),
                transfer_->ChunksTotal(), transfer_->ChunksAccepted(), transfer_->ChunksDuplicate());

    // View a range of bytes.
    ImGui::Separator();
    ImGui::TextUnformatted("View a range of bytes");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Offset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputScalar("##offset", ImGuiDataType_U64, &viewOffset_);
    ImGui::SameLine();
    ImGui::TextUnformatted("Length");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputScalar("##length", ImGuiDataType_U64, &viewLength_);
    ImGui::SameLine();
    if (ImGui::Button("View")) {
        ShowRange();
    }
    ImGui::TextDisabled("Click a cell to fill these fields. Max length: %d bytes.", config_.maxViewBytes);
    if (!rangeError_.empty()) {
        ImGui::TextColored(kRed, "%s", rangeError_.c_str());
    }

    // The grid.
    ImGui::Separator();
    const int maxCells = static_cast<int>(std::min<std::size_t>(size, kMaxCells));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Cells");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderInt("##cells", &cellCount_, std::min(16, maxCells), maxCells)) {
        cellCount_ = std::clamp(cellCount_, 1, maxCells);
        RefreshCells(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("1 cell is about %s", FormatSize(size / cells_.size()).c_str());

    DrawGrid();
    ImGui::End();
}

void App::DrawGrid() {
    const std::size_t count = cells_.size();
    const ImVec2 space = ImGui::GetContentRegionAvail();
    if (count == 0 || space.x < 8 || space.y < 8) {
        return;
    }

    // Pick the biggest square cell size that lets all cells fit into the free space.
    float side = std::floor(std::sqrt(space.x * space.y / static_cast<float>(count)));
    while (side > 2) {
        const std::size_t columns = std::max<std::size_t>(1, static_cast<std::size_t>(space.x / side));
        const std::size_t rows = (count + columns - 1) / columns;
        if (static_cast<float>(rows) * side <= space.y) {
            break;
        }
        side -= 1;
    }
    side = std::max(side, 2.0f);
    const std::size_t columns = std::max<std::size_t>(1, static_cast<std::size_t>(space.x / side));
    const std::size_t rows = (count + columns - 1) / columns;
    const ImVec2 gridSize(static_cast<float>(columns) * side, static_cast<float>(rows) * side);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("grid", gridSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool stopped = transfer_->GetState() == Transfer::State::Stopped;
    const float gap = side >= 6 ? 1.0f : 0.0f;

    draw->AddRectFilled(origin, origin + gridSize, IM_COL32(20, 20, 20, 255));
    for (std::size_t i = 0; i < count; ++i) {
        const ImVec2 corner = origin + ImVec2(static_cast<float>(i % columns) * side,
                                              static_cast<float>(i / columns) * side);
        draw->AddRectFilled(corner + ImVec2(gap, gap), corner + ImVec2(side - gap, side - gap),
                            CellColor(cells_[i], stopped));
    }
    draw->AddRect(origin - ImVec2(2, 2), origin + gridSize + ImVec2(2, 2), IM_COL32(200, 200, 200, 255));

    if (!hovered) {
        return;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos - origin;
    const std::size_t column = static_cast<std::size_t>(std::max(0.0f, mouse.x / side));
    const std::size_t row = static_cast<std::size_t>(std::max(0.0f, mouse.y / side));
    const std::size_t index = row * columns + column;
    if (column >= columns || index >= count) {
        return;
    }

    const ImVec2 corner = origin + ImVec2(static_cast<float>(column) * side, static_cast<float>(row) * side);
    draw->AddRect(corner, corner + ImVec2(side, side), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);

    const fc::Range range = CellRange(index);
    ImGui::SetTooltip("Cell %zu\nBytes %s - %s\nReceived: %.1f%%\nClick to view these bytes", index,
                      WithCommas(range.begin).c_str(), WithCommas(range.end - 1).c_str(), cells_[index] * 100.0f);
    if (clicked) {
        viewOffset_ = range.begin;
        viewLength_ = std::min<std::size_t>(range.Size(), config_.maxViewBytes);
        rangeError_.clear();
    }
}

fc::Range App::CellRange(std::size_t cell) const {
    const std::size_t size = transfer_->Size();
    const std::size_t count = cells_.size();
    return {cell * size / count, (cell + 1) * size / count};
}

void App::RefreshCells(bool force) {
    // Asking the collector about every cell is not free, 20 times a second is enough.
    const double now = ImGui::GetTime();
    if (!force && now - lastRefresh_ < 0.05) {
        return;
    }
    lastRefresh_ = now;
    cells_.resize(static_cast<std::size_t>(cellCount_));
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const fc::Range range = CellRange(i);
        cells_[i] = static_cast<float>(static_cast<double>(transfer_->ReadyBytes(range.begin, range.Size())) /
                                       static_cast<double>(range.Size()));
    }
}

void App::CloseTransfer() {
    rangeOpen_ = false;
    transfer_.reset();  // stops the sender threads
    cells_.clear();
    screen_ = Screen::Open;
}

// ---------- Range view ----------

void App::ShowRange() {
    const std::size_t size = transfer_->Size();
    if (viewOffset_ >= size) {
        rangeError_ = "Offset must be less than the file size (" + WithCommas(size) + ").";
        return;
    }
    if (viewLength_ == 0) {
        rangeError_ = "Length must be more than 0.";
        return;
    }
    const std::size_t length = std::min<std::size_t>(
        {static_cast<std::size_t>(viewLength_), static_cast<std::size_t>(config_.maxViewBytes),
         size - static_cast<std::size_t>(viewOffset_)});
    rangeError_.clear();
    rangeCut_ = length < viewLength_;
    snapshot_ = transfer_->Snapshot(static_cast<std::size_t>(viewOffset_), length);
    snapshotFileReady_ = transfer_->ReadyBytes();
    rangeOpen_ = true;
    ImGui::SetWindowFocus("Range view");
}

void App::DrawRangeWindow() {
    ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_Appearing);
    CenterNextWindow();
    if (!ImGui::Begin("Range view", &rangeOpen_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const std::size_t length = snapshot_.bytes.size();
    ImGui::Text("Bytes %s - %s (%s bytes)", WithCommas(snapshot_.offset).c_str(),
                WithCommas(snapshot_.offset + length - 1).c_str(), WithCommas(length).c_str());
    if (rangeCut_) {
        ImGui::TextDisabled("The range was cut to fit the file and max_view_bytes = %d.", config_.maxViewBytes);
    }
    ImGui::Text("Received in this range: %.1f%%", 100.0 * static_cast<double>(snapshot_.receivedCount) /
                                                          static_cast<double>(length));
    ImGui::Text("Received in the whole file: %.1f%%", 100.0 * static_cast<double>(snapshotFileReady_) /
                                                              static_cast<double>(transfer_->Size()));
    if (ImGui::Button("Refresh")) {
        snapshot_ = transfer_->Snapshot(snapshot_.offset, length);
        snapshotFileReady_ = transfer_->ReadyBytes();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("?? = this byte has not arrived. Offsets are decimal, bytes are hex.");

    ImGui::BeginChild("bytes", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const std::size_t lines = (length + kBytesPerLine - 1) / kBytesPerLine;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines));
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            const std::string text = FormatLine(snapshot_, static_cast<std::size_t>(line));
            if (LineIsComplete(snapshot_, static_cast<std::size_t>(line))) {
                ImGui::TextUnformatted(text.c_str());
            } else {
                ImGui::TextColored(kYellow, "%s", text.c_str());
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace viz
