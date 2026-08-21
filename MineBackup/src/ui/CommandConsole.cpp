#include "CommandConsole.h"

#include "KnotLinkService.h"
#include "i18n.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

class CommandConsole {
public:
    CommandConsole()
        : commands_{
            "HELP", "HISTORY", "CLEAR", "cmd=PING", "cmd=GET_CAPABILITIES",
            "cmd=GET_STATUS", "cmd=LIST_CONFIGS", "cmd=LIST_FOLDERS",
            "cmd=LIST_BACKUPS", "cmd=GET_CONFIG", "cmd=BACKUP", "cmd=RESTORE",
            "cmd=BACKUP_ALL", "cmd=MARK_IMPORTANT"} {}

    void Draw() {
        ImGui::TextWrapped("%s", L("COMMAND_CONSOLE_HELP"));
        if (ImGui::Button(L("COMMAND_HISTORY_CLEAR"))) {
            history_.clear();
            historyPosition_ = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(L("COMMAND_HISTORY_COUNT"),
            static_cast<int>(history_.size()), 100);
        if (!lastResult_.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", lastResult_.c_str());
        }
        ImGui::Separator();

        bool reclaimFocus = false;
        const auto flags = ImGuiInputTextFlags_EnterReturnsTrue
            | ImGuiInputTextFlags_EscapeClearsAll
            | ImGuiInputTextFlags_CallbackCompletion
            | ImGuiInputTextFlags_CallbackHistory;
        if (ImGui::InputText(L("CONSOLE_INPUT_LABEL"), input_.data(), input_.size(),
                flags, &TextEditCallbackStub, this)) {
            std::string command(input_.data());
            Trim(command);
            if (!command.empty()) Execute(command);
            input_.fill('\0');
            reclaimFocus = true;
        }
        ImGui::SetItemDefaultFocus();
        if (reclaimFocus) ImGui::SetKeyboardFocusHere(-1);
    }

private:
    static int TextEditCallbackStub(ImGuiInputTextCallbackData* data) {
        return static_cast<CommandConsole*>(data->UserData)->TextEditCallback(data);
    }

    int TextEditCallback(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            const char* wordEnd = data->Buf + data->CursorPos;
            const char* wordStart = wordEnd;
            while (wordStart > data->Buf) {
                const char character = wordStart[-1];
                if (character == ' ' || character == '\t'
                    || character == ',' || character == ';') {
                    break;
                }
                --wordStart;
            }
            std::vector<const std::string*> candidates;
            for (const auto& command : commands_) {
                if (StartsWithCaseInsensitive(command,
                        std::string_view(wordStart,
                            static_cast<std::size_t>(wordEnd - wordStart)))) {
                    candidates.push_back(&command);
                }
            }
            if (candidates.size() == 1) {
                data->DeleteChars(static_cast<int>(wordStart - data->Buf),
                    static_cast<int>(wordEnd - wordStart));
                data->InsertChars(data->CursorPos, candidates.front()->c_str());
                data->InsertChars(data->CursorPos, " ");
            }
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            const int previousPosition = historyPosition_;
            if (data->EventKey == ImGuiKey_UpArrow) {
                if (historyPosition_ == -1) {
                    historyPosition_ = static_cast<int>(history_.size()) - 1;
                } else if (historyPosition_ > 0) {
                    --historyPosition_;
                }
            } else if (data->EventKey == ImGuiKey_DownArrow && historyPosition_ != -1) {
                if (++historyPosition_ >= static_cast<int>(history_.size())) {
                    historyPosition_ = -1;
                }
            }
            if (previousPosition != historyPosition_) {
                const char* value = historyPosition_ >= 0
                    ? history_[static_cast<std::size_t>(historyPosition_)].c_str() : "";
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, value);
            }
        }
        return 0;
    }

    void Execute(const std::string& command) {
        historyPosition_ = -1;
        history_.erase(std::remove(history_.begin(), history_.end(), command), history_.end());
        history_.push_back(command);
        if (history_.size() > 100) history_.erase(history_.begin());

        if (EqualsCaseInsensitive(command, "CLEAR")) {
            lastResult_.clear();
        } else if (EqualsCaseInsensitive(command, "HELP")) {
            lastResult_ = L("COMMAND_CONSOLE_HELP");
        } else if (EqualsCaseInsensitive(command, "HISTORY")) {
            lastResult_.clear();
            for (std::size_t index = 0; index < history_.size(); ++index) {
                lastResult_.append(std::to_string(index)).append(": ")
                    .append(history_[index]).push_back('\n');
            }
        } else {
            lastResult_ = minebackup::knotlink::GetKnotLinkService()
                .HandlePayload(command);
        }
    }

    static void Trim(std::string& value) {
        const auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            value.clear();
            return;
        }
        const auto last = value.find_last_not_of(" \t");
        value = value.substr(first, last - first + 1);
    }

    static bool StartsWithCaseInsensitive(std::string_view value, std::string_view prefix) {
        if (prefix.size() > value.size()) return false;
        return std::equal(prefix.begin(), prefix.end(), value.begin(),
            [](unsigned char left, unsigned char right) {
                return std::toupper(left) == std::toupper(right);
            });
    }

    static bool EqualsCaseInsensitive(std::string_view left, std::string_view right) {
        return left.size() == right.size() && StartsWithCaseInsensitive(left, right);
    }

    std::array<char, 2048> input_{};
    std::vector<std::string> commands_;
    std::vector<std::string> history_;
    std::string lastResult_;
    int historyPosition_ = -1;
};

} // namespace

void DrawCommandConsole() {
    static CommandConsole console;
    console.Draw();
}
