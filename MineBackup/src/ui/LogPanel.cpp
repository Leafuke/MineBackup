#include "LogPanel.h"

#include "AppState.h"
#include "AppPaths.h"
#include "ConfigManager.h"
#include "DiagnosticLogExporter.h"
#include "DesktopServices.h"
#include "Globals.h"
#include "Logging.h"
#include "UIHelpers.h"
#include "i18n.h"
#include "imgui.h"
#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
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

constexpr std::size_t kLevelCount =
    static_cast<std::size_t>(LogLevel::Critical) + 1;
constexpr std::size_t kCategoryCount =
    static_cast<std::size_t>(LogCategory::Session) + 1;
constexpr std::size_t kLocalRecordLimit = 20'000;

const char* PlatformName() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* ProfileModeName(AppPathMode mode) {
    switch (mode) {
    case AppPathMode::Installed: return "installed";
    case AppPathMode::Portable: return "portable";
    case AppPathMode::Explicit: return "explicit";
    }
    return "unknown";
}

void AddRedaction(
    minebackup::diagnostics::DiagnosticExportOptions& options,
    const std::wstring& value,
    std::string replacement) {
    if (!value.empty()) {
        options.redactions.push_back(
            {wstring_to_utf8(value), std::move(replacement)});
    }
}

void AddPathRedaction(
    minebackup::diagnostics::DiagnosticExportOptions& options,
    const std::filesystem::path& value,
    std::string replacement) {
    AddRedaction(options, value.wstring(), std::move(replacement));
}

minebackup::diagnostics::DiagnosticExportOptions BuildExportOptions() {
    minebackup::diagnostics::DiagnosticExportOptions options;
    const auto& paths = GetAppPaths();
    options.logsDirectory = paths.logsRoot;
    options.applicationVersion = CURRENT_VERSION;
    options.platform = PlatformName();
    options.profileMode = ProfileModeName(paths.mode);

    AddPathRedaction(
        options, paths.configRoot.parent_path(), "<profile-root>");
    AddPathRedaction(options, paths.configRoot, "<profile-root>");
    AddPathRedaction(
        options, GetExecutablePath().parent_path(), "<application-root>");
    AddRedaction(options, Fontss, "<local-font>");
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE")) {
#else
    if (const char* home = std::getenv("HOME")) {
#endif
        AddPathRedaction(options, std::filesystem::path(home), "<user-home>");
    }

    std::lock_guard lock(g_appState.configsMutex);
    for (const auto& [index, config] : g_appState.configs) {
        (void)index;
        AddRedaction(options, config.saveRoot, "<save-root>");
        AddRedaction(options, config.backupPath, "<backup-root>");
        AddRedaction(options, config.zipPath, "<local-tool>");
        AddRedaction(options, config.rclonePath, "<local-tool>");
        AddRedaction(options, config.cloudWorkingDirectory, "<working-directory>");
        AddRedaction(options, config.snapshotPath, "<snapshot-root>");
        AddRedaction(options, config.othersPath, "<external-root>");
        AddRedaction(options, config.weSnapshotPath, "<worldedit-root>");
        AddRedaction(options, config.rcloneRemotePath, "<rclone-remote>");
    }
	for (const auto& job : g_appState.jobs.jobs) {
		for (const auto& stage : job.stages) {
			for (const auto& step : stage.steps) {
				if (step.type != JobStepType::Process) continue;
				AddRedaction(options, step.process.executable.wstring(), "<local-tool>");
				AddRedaction(options, step.process.workingDirectory.wstring(), "<working-directory>");
			}
		}
	}
    return options;
}

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
        categoryEnabled_.fill(true);
    }

    void Draw() {
        bool appended = false;
        if (!paused_) appended = Refresh();
        appended = DrawToolbar() || appended;
        DrawExportDialog();
        DrawBackendStatus();
        RebuildFilterIfNeeded();
        DrawStream(appended);
        DrawDetailsPopup();
        DrawFooter();
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
        struct LevelOption {
            LogLevel level;
            const char* labelKey;
        };
        static constexpr std::array<LevelOption, kLevelCount> levelOptions{{
            {LogLevel::Trace, "LOG_VIEW_LEVEL_ALL"},
            {LogLevel::Debug, "LOG_VIEW_LEVEL_DEBUG"},
            {LogLevel::Info, "LOG_VIEW_LEVEL_INFO"},
            {LogLevel::Warning, "LOG_VIEW_LEVEL_WARNING"},
            {LogLevel::Error, "LOG_VIEW_LEVEL_ERROR"},
            {LogLevel::Critical, "LOG_VIEW_LEVEL_CRITICAL"},
        }};

        const char* currentLevelLabel = L("LOG_VIEW_LEVEL_INFO");
        for (const auto& option : levelOptions) {
            if (option.level == g_logViewLevel) {
                currentLevelLabel = L(option.labelKey);
                break;
            }
        }
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float levelWidth = std::clamp(availableWidth * 0.34f, 118.0f, 160.0f);
        ImGui::SetNextItemWidth(levelWidth);
        if (ImGui::BeginCombo("##log-view-level", currentLevelLabel)) {
            for (const auto& option : levelOptions) {
                const bool selected = option.level == g_logViewLevel;
                if (ImGui::Selectable(L(option.labelKey), selected)) {
                    g_logViewLevel = option.level;
                    filterDirty_ = true;
                    SaveViewPreferences();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##log-search", L("LOG_SEARCH_HINT"),
                search_, sizeof(search_))) {
            filterDirty_ = true;
        }

        if (ImGui::Button(paused_ ? L("LOG_RESUME_SHORT") : L("LOG_PAUSE_SHORT"))) {
            paused_ = !paused_;
            if (!paused_) appendedOnResume = Refresh();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(L("LOG_AUTO_TAIL"), &g_logViewAutoTail)) {
            SaveViewPreferences();
        }
        const float moreWidth = ImGui::CalcTextSize(L("LOG_MORE")).x
            + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float rightAlignedX = ImGui::GetCursorPosX()
            + ImGui::GetContentRegionAvail().x - moreWidth;
        if (rightAlignedX > ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(rightAlignedX);
        } else {
            ImGui::SameLine();
        }
        if (ImGui::Button(L("LOG_MORE"))) {
            ImGui::OpenPopup("##log-more-menu");
        }
        if (ImGui::BeginPopup("##log-more-menu")) {
            if (ImGui::BeginMenu(L("LOG_CATEGORY_FILTER"))) {
                for (std::size_t index = 0; index < categoryEnabled_.size(); ++index) {
                    const auto category = static_cast<LogCategory>(index);
                    if (ImGui::Checkbox(minebackup::logging::ToString(category),
                            &categoryEnabled_[index])) {
                        filterDirty_ = true;
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(L("LOG_DISPLAY_OPTIONS"))) {
                if (ImGui::Checkbox(L("LOG_SHOW_TIME"), &g_logViewShowTime)) {
                    SaveViewPreferences();
                }
                if (ImGui::Checkbox(L("LOG_SHOW_CATEGORY"), &g_logViewShowCategory)) {
                    SaveViewPreferences();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            const auto selected = FindSelectedRecord();
            if (ImGui::MenuItem(selected ? L("LOG_COPY_ROW") : L("LOG_COPY_FILTERED"))) {
                if (selected) CopyRecord(*selected);
                else CopyFiltered();
            }
            if (ImGui::MenuItem(L("LOG_CLEAR_VIEW"))) ClearView();
            ImGui::Separator();
            if (ImGui::MenuItem(L("LOG_OPEN_DIRECTORY"))) {
                (void)GetDesktopServices()->OpenFolder(GetAppPaths().logsRoot);
            }
            if (ImGui::MenuItem(L("LOG_EXPORT_DIAGNOSTICS"))) {
                openExportDialog_ = true;
            }
            ImGui::EndPopup();
        }
        if (openExportDialog_) {
            ImGui::OpenPopup("##diagnostic-export-confirm");
            openExportDialog_ = false;
        }
        return appendedOnResume;
    }

    void DrawExportDialog() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const UiMetrics metrics = GetUiMetrics();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float maxWindowWidth = (std::max)(
            1.0f, viewport->WorkSize.x * 0.90f);
        const float maxWindowHeight = (std::max)(
            1.0f, viewport->WorkSize.y * 0.90f);
        const float minWindowWidth = (std::min)(
            metrics.Em(24.0f), maxWindowWidth);
        const float initialWindowWidth = (std::min)(
            metrics.Em(32.0f), maxWindowWidth);
        const float maxContentWidth = (std::max)(
            1.0f, maxWindowWidth - style.WindowPadding.x * 2.0f);
        const float wrapWidth = (std::min)(metrics.Em(30.0f), maxContentWidth);

        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(
            viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(
            ImVec2(initialWindowWidth, 0.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(minWindowWidth, 0.0f),
            ImVec2(maxWindowWidth, maxWindowHeight));
        if (ImGui::BeginPopupModal(
                "##diagnostic-export-confirm", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            ImGui::TextWrapped("%s", L("LOG_EXPORT_DIAGNOSTICS_WARNING"));
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button(L("BUTTON_CONFIRM"))) {
                const auto result =
                    minebackup::diagnostics::ExportDiagnostics(BuildExportOptions());
                if (result.success) {
                    exportStatus_ = wstring_to_utf8(MineFormatMessage(
                        "LOG_EXPORT_DIAGNOSTICS_SUCCESS",
                        wstring_to_utf8(result.path.wstring()).c_str()));
                    (void)GetDesktopServices()->RevealInFolder(
                        result.path.parent_path(), result.path);
                    MB_LOG_INFO(minebackup::logging::LogCategory::Application,
                        "diagnostics.export.completed",
                        "Diagnostic log exported to {}", result.path.filename().string());
                } else {
                    exportStatus_ = wstring_to_utf8(MineFormatMessage(
                        "LOG_EXPORT_DIAGNOSTICS_FAILED", result.error.c_str()));
                    MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
                        "diagnostics.export.failed",
                        "Diagnostic log export failed: {}", result.error);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("BUTTON_CANCEL"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!exportStatus_.empty()) {
            ImGui::TextWrapped("%s", exportStatus_.c_str());
        }
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
        const auto categoryIndex = static_cast<std::size_t>(record.category);
        if (record.level < g_logViewLevel) return false;
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

    const char* LevelTag(LogLevel level) const {
        switch (level) {
        case LogLevel::Warning: return L("LOG_LEVEL_TAG_WARNING");
        case LogLevel::Error: return L("LOG_LEVEL_TAG_ERROR");
        case LogLevel::Critical: return L("LOG_LEVEL_TAG_CRITICAL");
        default: return nullptr;
        }
    }

    void DrawStream(bool appended) {
        const float footerReserve = ImGui::GetTextLineHeightWithSpacing() * 1.35f;
        bool openDetails = false;
        if (ImGui::BeginChild(
                "##log-stream", ImVec2(0.0f, -footerReserve), true)) {
            ImGuiListClipper clipper;
            const float rowHeight = ImGui::GetTextLineHeightWithSpacing()
                + ImGui::GetStyle().FramePadding.y * 1.5f;
            clipper.Begin(static_cast<int>(filteredIndices_.size()), rowHeight);
            while (clipper.Step()) {
                for (int visibleIndex = clipper.DisplayStart;
                     visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
                    const auto recordIndex =
                        filteredIndices_[static_cast<std::size_t>(visibleIndex)];
                    if (recordIndex >= records_.size()) continue;
                    const auto& record = *records_[recordIndex];
                    ImGui::PushID(static_cast<int>(record.sequence));
                    if (ImGui::Selectable("##row",
                            selectedSequence_ == record.sequence,
                            ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0.0f, rowHeight))) {
                        selectedSequence_ = record.sequence;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            openDetails = true;
                        }
                    }

                    const ImVec2 rowMin = ImGui::GetItemRectMin();
                    const ImVec2 rowMax = ImGui::GetItemRectMax();
                    auto* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(
                        rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y),
                        ImGui::ColorConvertFloat4ToU32(LevelColor(record.level)),
                        1.5f);

                    float textX = rowMin.x + ImGui::GetStyle().FramePadding.x + 7.0f;
                    const float textY = rowMin.y
                        + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
                    const ImVec4 clip(
                        textX, rowMin.y, rowMax.x - ImGui::GetStyle().FramePadding.x,
                        rowMax.y);
                    auto drawSegment = [&](std::string_view text, ImU32 color) {
                        if (text.empty()) return;
                        drawList->AddText(
                            ImGui::GetFont(), ImGui::GetFontSize(),
                            ImVec2(textX, textY), color, text.data(),
                            text.data() + text.size(), 0.0f, &clip);
                        textX += ImGui::CalcTextSize(
                            text.data(), text.data() + text.size()).x;
                    };
                    if (g_logViewShowTime) {
                        const std::string time =
                            FormatTime(record.timestamp, false) + "  ";
                        drawSegment(time, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                    }
                    if (const char* tag = LevelTag(record.level)) {
                        const std::string label = std::string(tag) + "  ";
                        drawSegment(label,
                            ImGui::ColorConvertFloat4ToU32(LevelColor(record.level)));
                    }
                    if (g_logViewShowCategory) {
                        const std::string category = "["
                            + std::string(minebackup::logging::ToString(record.category))
                            + "]  ";
                        drawSegment(category, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                    }
                    drawSegment(record.message, ImGui::GetColorU32(ImGuiCol_Text));

                    if (ImGui::BeginPopupContextItem("##row-context")) {
                        selectedSequence_ = record.sequence;
                        if (ImGui::MenuItem(L("LOG_VIEW_DETAILS"))) {
                            openDetails = true;
                        }
                        if (ImGui::MenuItem(L("LOG_COPY_MESSAGE"))) {
                            ImGui::SetClipboardText(record.message.c_str());
                        }
                        if (ImGui::MenuItem(L("LOG_COPY_ROW"))) CopyRecord(record);
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                        ImGui::BeginTooltip();
                        ImGui::TextColored(LevelColor(record.level), "%s",
                            minebackup::logging::ToString(record.level));
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s  %s",
                            minebackup::logging::ToString(record.category),
                            FormatTime(record.timestamp, true).c_str());
                        ImGui::PushTextWrapPos(
                            ImGui::GetCursorPosX() + 420.0f * g_uiScale);
                        ImGui::TextUnformatted(record.message.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
            if (appended && g_logViewAutoTail) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        if (openDetails) ImGui::OpenPopup("##log-details");
    }

    void DrawDetailsPopup() {
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(390.0f * g_uiScale, 0.0f),
            ImVec2(680.0f * g_uiScale, 620.0f * g_uiScale));
        if (!ImGui::BeginPopup("##log-details",
                ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }
        const LogRecord* record = FindSelectedRecord();
        if (!record) {
            ImGui::TextDisabled("%s", L("LOG_DETAILS_UNAVAILABLE"));
            ImGui::EndPopup();
            return;
        }

        ImGui::TextUnformatted(L("LOG_DETAILS_TITLE"));
        ImGui::SameLine();
        if (ImGui::SmallButton(L("LOG_COPY_ROW"))) CopyRecord(*record);
        ImGui::Separator();
        ImGui::TextDisabled("%s", L("LOG_DETAIL_MESSAGE"));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520.0f * g_uiScale);
        ImGui::TextUnformatted(record->message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        const auto detailRow = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", value.c_str());
        };
        if (ImGui::BeginTable("##log-details-fields", 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                96.0f * g_uiScale);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            detailRow(L("LOG_COLUMN_TIME"), FormatTime(record->timestamp, true));
            detailRow(L("LOG_COLUMN_LEVEL"),
                minebackup::logging::ToString(record->level));
            detailRow(L("LOG_COLUMN_CATEGORY"),
                minebackup::logging::ToString(record->category));
            detailRow(L("LOG_DETAIL_EVENT"), record->eventId);
            if (!record->sourceFile.empty()) {
                detailRow(L("LOG_DETAIL_SOURCE"), record->sourceFile + ":"
                    + std::to_string(record->sourceLine));
            }
            detailRow(L("LOG_DETAIL_THREAD"), record->threadId);
            detailRow(L("LOG_DETAIL_SESSION"),
                record->sessionId.substr(
                    0, std::min<std::size_t>(8, record->sessionId.size())));
            ImGui::EndTable();
        }

        if (!record->context.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", L("LOG_DETAIL_CONTEXT"));
            if (ImGui::BeginTable("##log-details-context", 2,
                    ImGuiTableFlags_SizingStretchProp
                        | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed,
                    120.0f * g_uiScale);
                ImGui::TableSetupColumn("##context-value",
                    ImGuiTableColumnFlags_WidthStretch);
                for (const auto& field : record->context) {
                    detailRow(field.key.c_str(), field.value);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndPopup();
    }

    void DrawFooter() const {
        ImGui::TextDisabled(L("LOG_VISIBLE_COUNT"),
            static_cast<unsigned long long>(filteredIndices_.size()),
            static_cast<unsigned long long>(records_.size()));
        if (paused_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f),
                "%s", L("LOG_PAUSED_STATUS"));
        }
    }

    const LogRecord* FindSelectedRecord() const {
        const auto selected = std::find_if(records_.begin(), records_.end(),
            [this](const auto& record) {
                return record->sequence == selectedSequence_;
            });
        return selected == records_.end() ? nullptr : selected->get();
    }

    void ClearView() {
        const auto status = minebackup::logging::GetStatus();
        viewStartSequence_ = status.latestSequence;
        cursor_ = status.latestSequence;
        records_.clear();
        filteredIndices_.clear();
        selectedSequence_ = 0;
        filterDirty_ = true;
    }

    static void SaveViewPreferences() {
        (void)SaveConfigs();
    }

    static std::string RecordText(const LogRecord& record) {
        std::ostringstream output;
        output << '#' << record.sequence << ' ' << FormatTime(record.timestamp, true)
               << ' ' << minebackup::logging::ToString(record.level)
               << ' ' << minebackup::logging::ToString(record.category)
               << ' ' << record.message
               << " | event=" << record.eventId
               << " session=" << record.sessionId
               << " thread=" << record.threadId;
        if (!record.sourceFile.empty()) {
            output << " source=" << record.sourceFile << ':' << record.sourceLine;
        }
        if (!record.context.empty()) {
            output << " context=[";
            bool first = true;
            for (const auto& field : record.context) {
                if (!first) output << ';';
                first = false;
                output << field.key << '=' << field.value;
            }
            output << ']';
        }
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
    std::array<bool, kCategoryCount> categoryEnabled_{};
    char search_[256]{};
    std::uint64_t cursor_ = 0;
    std::uint64_t viewStartSequence_ = 0;
    std::uint64_t selectedSequence_ = 0;
    std::uint64_t evictedCount_ = 0;
    bool paused_ = false;
    bool filterDirty_ = true;
    bool showBackendError_ = false;
    bool openExportDialog_ = false;
    std::string lastBackendError_;
    std::string exportStatus_;
};

} // namespace

void DrawLogPanel() {
    static LogPanel panel;
    panel.Draw();
}
