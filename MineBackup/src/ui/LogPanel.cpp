#include "LogPanel.h"

#include "AppPaths.h"
#include "DesktopServices.h"
#include "Logging.h"
#include "i18n.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using minebackup::logging::LogCategory;
using minebackup::logging::LogLevel;
using minebackup::logging::LogRecord;

constexpr std::size_t kLevelCount = 6;
constexpr std::size_t kCategoryCount = 12;
constexpr std::size_t kLocalRecordLimit = 20'000;

std::string FormatTime(
    std::chrono::system_clock::time_point timestamp, bool includeDate) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer),
        includeDate ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &local);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;
    std::ostringstream output;
    output << buffer << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count();
    return output.str();
}

bool ContainsCaseInsensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::search(value.begin(), value.end(), query.begin(), query.end(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != value.end();
}

ImVec4 LevelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    case LogLevel::Debug: return ImVec4(0.55f, 0.75f, 0.95f, 1.0f);
    case LogLevel::Info: return ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
    case LogLevel::Warning: return ImVec4(1.0f, 0.78f, 0.28f, 1.0f);
    case LogLevel::Error: return ImVec4(1.0f, 0.40f, 0.40f, 1.0f);
    case LogLevel::Critical: return ImVec4(1.0f, 0.20f, 0.55f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

class LogPanel {
public:
    LogPanel() {
        levelEnabled_.fill(false);
        levelEnabled_[static_cast<std::size_t>(LogLevel::Info)] = true;
        levelEnabled_[static_cast<std::size_t>(LogLevel::Warning)] = true;
        levelEnabled_[static_cast<std::size_t>(LogLevel::Error)] = true;
        levelEnabled_[static_cast<std::size_t>(LogLevel::Critical)] = true;
        categoryEnabled_.fill(true);
    }

    void Draw() {
        bool appended = false;
        if (!paused_) appended = Refresh();
        appended = DrawToolbar() || appended;
        DrawBackendStatus();
        RebuildFilterIfNeeded();
        DrawTable(appended);
        DrawDetails();
    }

private:
    bool Refresh() {
        const auto read = minebackup::logging::ReadAfter(cursor_);
        cursor_ = read.latestSequence;
        if (read.requestedSequenceWasEvicted) {
            records_.clear();
            filteredIndices_.clear();
            filterDirty_ = true;
        }

        const auto oldSize = records_.size();
        bool appended = false;
        for (const auto& record : read.records) {
            if (record->sequence > viewStartSequence_) {
                records_.push_back(record);
                appended = true;
            }
        }
        if (records_.size() > kLocalRecordLimit) {
            records_.erase(records_.begin(),
                records_.begin() + static_cast<std::ptrdiff_t>(
                    records_.size() - kLocalRecordLimit));
            filterDirty_ = true;
        } else if (!filterDirty_) {
            for (std::size_t index = oldSize; index < records_.size(); ++index) {
                if (PassesFilter(*records_[index])) filteredIndices_.push_back(index);
            }
        }
        evictedCount_ = read.evictedCount;
        return appended;
    }

    bool DrawToolbar() {
        bool appendedOnResume = false;
        if (ImGui::Button(paused_ ? L("LOG_RESUME") : L("LOG_PAUSE"))) {
            paused_ = !paused_;
            if (!paused_) appendedOnResume = Refresh();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("LOG_CLEAR_VIEW"))) {
            const auto status = minebackup::logging::GetStatus();
            viewStartSequence_ = status.latestSequence;
            cursor_ = status.latestSequence;
            records_.clear();
            filteredIndices_.clear();
            selectedSequence_ = 0;
            filterDirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(L("BUTTON_COPY"))) CopyFiltered();
        ImGui::SameLine();
        if (ImGui::Button(L("LOG_OPEN_DIRECTORY"))) {
            (void)GetDesktopServices()->OpenFolder(GetAppPaths().logsRoot);
        }
        ImGui::SameLine();
        ImGui::Checkbox(L("LOG_AUTO_TAIL"), &autoTail_);

        if (ImGui::Button(L("LOG_LEVEL_FILTER"))) ImGui::OpenPopup("##log-level-filter");
        if (ImGui::BeginPopup("##log-level-filter")) {
            const std::array<LogLevel, kLevelCount> levels = {
                LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                LogLevel::Warning, LogLevel::Error, LogLevel::Critical};
            for (const auto level : levels) {
                const auto index = static_cast<std::size_t>(level);
                if (ImGui::Checkbox(minebackup::logging::ToString(level), &levelEnabled_[index])) {
                    filterDirty_ = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("LOG_CATEGORY_FILTER"))) ImGui::OpenPopup("##log-category-filter");
        if (ImGui::BeginPopup("##log-category-filter")) {
            for (std::size_t index = 0; index < categoryEnabled_.size(); ++index) {
                const auto category = static_cast<LogCategory>(index);
                if (ImGui::Checkbox(minebackup::logging::ToString(category),
                        &categoryEnabled_[index])) {
                    filterDirty_ = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputTextWithHint("##log-search", L("LOG_SEARCH_HINT"),
                search_, sizeof(search_))) {
            filterDirty_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu / %zu", filteredIndices_.size(), records_.size());
        return appendedOnResume;
    }

    void DrawBackendStatus() {
        const auto status = minebackup::logging::GetStatus();
        if (!status.lastBackendError.empty()
            && status.lastBackendError != lastBackendError_) {
            lastBackendError_ = status.lastBackendError;
            showBackendError_ = true;
        }
        if (showBackendError_) {
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                "%s: %s", L("LOG_BACKEND_ERROR"), lastBackendError_.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(L("LOG_DISMISS"))) showBackendError_ = false;
        }
        if (evictedCount_ > 0) {
            ImGui::TextDisabled(L("LOG_EVICTED_COUNT"),
                static_cast<unsigned long long>(evictedCount_));
        }
        if (status.previousSessionAbnormal) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f),
                "%s", L("LOG_PREVIOUS_SESSION_ABNORMAL"));
        }
    }

    bool PassesFilter(const LogRecord& record) const {
        const auto levelIndex = static_cast<std::size_t>(record.level);
        const auto categoryIndex = static_cast<std::size_t>(record.category);
        if (levelIndex >= levelEnabled_.size() || !levelEnabled_[levelIndex]) return false;
        if (categoryIndex >= categoryEnabled_.size() || !categoryEnabled_[categoryIndex]) return false;
        const std::string_view query(search_);
        if (query.empty()) return true;
        if (ContainsCaseInsensitive(record.message, query)
            || ContainsCaseInsensitive(record.eventId, query)
            || ContainsCaseInsensitive(minebackup::logging::ToString(record.category), query)
            || ContainsCaseInsensitive(minebackup::logging::ToString(record.level), query)) {
            return true;
        }
        for (const auto& field : record.context) {
            if (ContainsCaseInsensitive(field.key, query)
                || ContainsCaseInsensitive(field.value, query)) {
                return true;
            }
        }
        return false;
    }

    void RebuildFilterIfNeeded() {
        if (!filterDirty_) return;
        filteredIndices_.clear();
        filteredIndices_.reserve(records_.size());
        for (std::size_t index = 0; index < records_.size(); ++index) {
            if (PassesFilter(*records_[index])) filteredIndices_.push_back(index);
        }
        filterDirty_ = false;
    }

    void DrawTable(bool appended) {
        const auto flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_SizingStretchProp;
        const float detailsReserve = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
        if (!ImGui::BeginTable("##structured-log-table", 4, flags,
                ImVec2(0.0f, -detailsReserve))) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(L("LOG_COLUMN_TIME"), ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn(L("LOG_COLUMN_LEVEL"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(L("LOG_COLUMN_CATEGORY"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn(L("LOG_COLUMN_MESSAGE"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        clipper.Begin(static_cast<int>(filteredIndices_.size()), rowHeight);
        while (clipper.Step()) {
            for (int visibleIndex = clipper.DisplayStart;
                 visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
                const auto recordIndex = filteredIndices_[static_cast<std::size_t>(visibleIndex)];
                if (recordIndex >= records_.size()) continue;
                const auto& record = *records_[recordIndex];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                ImGui::TableSetColumnIndex(0);
                const std::string id = "##log-row-" + std::to_string(record.sequence);
                if (ImGui::Selectable(id.c_str(), selectedSequence_ == record.sequence,
                        ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowHeight))) {
                    selectedSequence_ = record.sequence;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(FormatTime(record.timestamp, false).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(LevelColor(record.level), "%s",
                    minebackup::logging::ToString(record.level));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(minebackup::logging::ToString(record.category));
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(record.message.c_str());
            }
        }
        if (appended && autoTail_) ImGui::SetScrollHereY(1.0f);
        ImGui::EndTable();
    }

    void DrawDetails() {
        const auto selected = std::find_if(records_.begin(), records_.end(),
            [this](const auto& record) { return record->sequence == selectedSequence_; });
        if (selected == records_.end()) {
            ImGui::TextDisabled("%s", L("LOG_SELECT_DETAILS"));
            return;
        }
        const auto& record = **selected;
        if (ImGui::Button(L("LOG_COPY_ROW"))) CopyRecord(record);
        ImGui::SameLine();
        ImGui::Text("#%llu  %s  %s/%s",
            static_cast<unsigned long long>(record.sequence),
            FormatTime(record.timestamp, true).c_str(),
            minebackup::logging::ToString(record.level),
            minebackup::logging::ToString(record.category));
        ImGui::TextWrapped("%s", record.message.c_str());
        ImGui::TextDisabled("event=%s  session=%s  thread=%s",
            record.eventId.c_str(), record.sessionId.c_str(), record.threadId.c_str());
        if (!record.sourceFile.empty()) {
            ImGui::TextDisabled("source=%s:%u", record.sourceFile.c_str(),
                static_cast<unsigned int>(record.sourceLine));
        }
        if (!record.context.empty()) {
            std::string context;
            for (const auto& field : record.context) {
                if (!context.empty()) context.append("; ");
                context.append(field.key).append("=").append(field.value);
            }
            ImGui::TextWrapped("context: %s", context.c_str());
        }
    }

    static std::string RecordText(const LogRecord& record) {
        std::ostringstream output;
        output << '#' << record.sequence << ' ' << FormatTime(record.timestamp, true)
               << ' ' << minebackup::logging::ToString(record.level)
               << ' ' << minebackup::logging::ToString(record.category)
               << ' ' << record.eventId << ' ' << record.message;
        return output.str();
    }

    static void CopyRecord(const LogRecord& record) {
        ImGui::SetClipboardText(RecordText(record).c_str());
    }

    void CopyFiltered() const {
        std::string output;
        for (const auto index : filteredIndices_) {
            if (index >= records_.size()) continue;
            output.append(RecordText(*records_[index])).push_back('\n');
        }
        ImGui::SetClipboardText(output.c_str());
    }

    std::vector<std::shared_ptr<const LogRecord>> records_;
    std::vector<std::size_t> filteredIndices_;
    std::array<bool, kLevelCount> levelEnabled_{};
    std::array<bool, kCategoryCount> categoryEnabled_{};
    char search_[256]{};
    std::uint64_t cursor_ = 0;
    std::uint64_t viewStartSequence_ = 0;
    std::uint64_t selectedSequence_ = 0;
    std::uint64_t evictedCount_ = 0;
    bool paused_ = false;
    bool autoTail_ = true;
    bool filterDirty_ = true;
    bool showBackendError_ = false;
    std::string lastBackendError_;
};

} // namespace

void DrawLogPanel() {
    static LogPanel panel;
    panel.Draw();
}
