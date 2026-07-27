#include "Console.h"

#include "KnotLinkService.h"

#include <cstdio>
#include <mutex>
#include <string>

Console console;
std::mutex consoleMutex;

std::string ProcessCommand(const std::string& command, Console* targetConsole) {
    Console& output = targetConsole != nullptr ? *targetConsole : console;
    return minebackup::knotlink::GetKnotLinkService().HandlePayload(command, output);
}

void ConsoleLog(Console* targetConsole, const char* format, ...) {
    std::lock_guard<std::mutex> lock(consoleMutex);

    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, IM_ARRAYSIZE(buffer), format, arguments);
    buffer[IM_ARRAYSIZE(buffer) - 1] = 0;
    va_end(arguments);

    if (targetConsole != nullptr) {
        targetConsole->AddLog("%s", buffer);
    } else {
        minebackup::logging::LogLegacyMessage(buffer);
    }
}

void Console::ExecCommand(const char* commandLine) {
    AddLog("# %s\n", commandLine);

    HistoryPos = -1;
    for (int index = History.Size - 1; index >= 0; --index) {
        if (Stricmp(History[index], commandLine) == 0) {
            ImGui::MemFree(History[index]);
            History.erase(History.begin() + index);
            break;
        }
    }
    History.push_back(Strdup(commandLine));

    if (Stricmp(commandLine, "CLEAR") == 0) {
        ClearLog();
    } else if (Stricmp(commandLine, "HELP") == 0) {
        AddLog("Local controls: HELP, CLEAR, HISTORY");
        AddLog("KnotLink v2 commands use key=value fields, for example:");
        AddLog("  cmd=PING");
        AddLog("  cmd=LIST_BACKUPS;config_id=my-config;folder=0");
        AddLog("Mutating commands also require from and request_id.");
    } else if (Stricmp(commandLine, "HISTORY") == 0) {
        const int first = History.Size - 10;
        for (int index = first > 0 ? first : 0; index < History.Size; ++index) {
            AddLog("%3d: %s\n", index, History[index]);
        }
    } else {
        const std::string result = ProcessCommand(commandLine, this);
        AddLog("-> %s", result.c_str());
    }

    ScrollToBottom = true;
}
