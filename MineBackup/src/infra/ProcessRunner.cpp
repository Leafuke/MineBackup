#include "ProcessRunner.h"

#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace std;

namespace ProcessRunner {
namespace {

void AppendBounded(string& destination, const char* data, size_t count, size_t maximum, bool& truncated) {
    if (destination.size() >= maximum) {
        truncated = truncated || count != 0;
        return;
    }
    const size_t accepted = min(count, maximum - destination.size());
    destination.append(data, accepted);
    truncated = truncated || accepted != count;
}

#ifdef _WIN32

wstring QuoteWindowsArgument(const wstring& argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == wstring::npos) return argument;
    wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        }
        else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

void DrainPipe(HANDLE pipe, string& destination, size_t maximum, bool& truncated) {
    array<char, 8192> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr)
            || read == 0) break;
        AppendBounded(destination, buffer.data(), read, maximum, truncated);
    }
}

ProcessResult RunPlatform(const ProcessSpec& spec, stop_token stopToken) {
    ProcessResult result;
    if (spec.executable.empty() || spec.maximumCapturedBytes == 0) {
        result.error = L"The process specification is incomplete.";
        return result;
    }

    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr, stderrRead = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0)
        || !CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0)
        || !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
        if (stdoutRead) CloseHandle(stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        result.error = L"Could not create process output pipes.";
        return result;
    }

    wstring commandLine = QuoteWindowsArgument(spec.executable.wstring());
    for (const auto& argument : spec.arguments) commandLine += L" " + QuoteWindowsArgument(argument);
    vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        CloseHandle(stdoutRead); CloseHandle(stdoutWrite); CloseHandle(stderrRead); CloseHandle(stderrWrite);
        result.error = L"Could not create a process lifetime job.";
        return result;
    }

    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (spec.useLowPriority) flags |= BELOW_NORMAL_PRIORITY_CLASS;
    const wchar_t* workingDirectory = spec.workingDirectory.empty() ? nullptr : spec.workingDirectory.c_str();
    const BOOL created = CreateProcessW(spec.executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        flags, nullptr, workingDirectory, &startup, &process);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!created || !AssignProcessToJobObject(job, process.hProcess)) {
        if (created) {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        CloseHandle(job); CloseHandle(stdoutRead); CloseHandle(stderrRead);
        result.error = created ? L"Could not assign the process to its lifetime job." : L"Could not start the process.";
        return result;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    const auto started = chrono::steady_clock::now();
    bool terminated = false;
    for (;;) {
        DrainPipe(stdoutRead, result.standardOutput, spec.maximumCapturedBytes, result.outputTruncated);
        DrainPipe(stderrRead, result.standardError, spec.maximumCapturedBytes, result.outputTruncated);
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) break;
        if (stopToken.stop_requested()) {
            result.status = ProcessStatus::Cancelled;
            terminated = true;
        }
        else if (spec.timeout.count() > 0 && chrono::steady_clock::now() - started >= spec.timeout) {
            result.status = ProcessStatus::TimedOut;
            terminated = true;
        }
        if (terminated) {
            TerminateJobObject(job, 1);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    DrainPipe(stdoutRead, result.standardOutput, spec.maximumCapturedBytes, result.outputTruncated);
    DrainPipe(stderrRead, result.standardError, spec.maximumCapturedBytes, result.outputTruncated);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    if (!terminated) result.status = exitCode == 0 ? ProcessStatus::Succeeded : ProcessStatus::ExitedWithError;
    CloseHandle(process.hProcess); CloseHandle(job); CloseHandle(stdoutRead); CloseHandle(stderrRead);
    return result;
}

#else

atomic<pid_t> g_activeChildGroup{0};

void DrainDescriptor(int descriptor, string& destination, size_t maximum, bool& truncated) {
    array<char, 8192> buffer{};
    for (;;) {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count > 0) AppendBounded(destination, buffer.data(), static_cast<size_t>(count), maximum, truncated);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
}

ProcessResult RunPlatform(const ProcessSpec& spec, stop_token stopToken) {
    ProcessResult result;
    if (spec.executable.empty() || spec.maximumCapturedBytes == 0) {
        result.error = L"The process specification is incomplete.";
        return result;
    }
    int stdoutPipe[2]{-1, -1};
    int stderrPipe[2]{-1, -1};
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        for (const int descriptor : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]}) {
            if (descriptor >= 0) close(descriptor);
        }
        result.error = L"Could not create process output pipes.";
        return result;
    }
    for (const int descriptor : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]}) {
        if (descriptor >= 0) {
            const int flags = fcntl(descriptor, F_GETFD);
            if (flags >= 0) fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
        }
    }

    posix_spawn_file_actions_t actions{};
    const int actionsInitError = posix_spawn_file_actions_init(&actions);
    if (actionsInitError != 0) {
        close(stdoutPipe[0]); close(stdoutPipe[1]); close(stderrPipe[0]); close(stderrPipe[1]);
        result.error = L"Could not initialize process output redirection.";
        return result;
    }
    int actionsError = posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
    if (actionsError == 0) actionsError = posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
    if (actionsError == 0) actionsError = posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
    if (actionsError == 0) actionsError = posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
    if (actionsError != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(stdoutPipe[0]); close(stdoutPipe[1]); close(stderrPipe[0]); close(stderrPipe[1]);
        result.error = L"Could not configure process output redirection.";
        return result;
    }
#if defined(__APPLE__) || defined(__GLIBC__)
    if (!spec.workingDirectory.empty()) {
        const int directoryError = posix_spawn_file_actions_addchdir_np(&actions, spec.workingDirectory.c_str());
        if (directoryError != 0) {
            posix_spawn_file_actions_destroy(&actions);
            close(stdoutPipe[0]); close(stdoutPipe[1]); close(stderrPipe[0]); close(stderrPipe[1]);
            result.error = L"Could not apply the process working directory.";
            return result;
        }
    }
#endif
    posix_spawnattr_t attributes{};
    const int attributeInitError = posix_spawnattr_init(&attributes);
    if (attributeInitError != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(stdoutPipe[0]); close(stdoutPipe[1]); close(stderrPipe[0]); close(stderrPipe[1]);
        result.error = L"Could not initialize the process isolation group.";
        return result;
    }
    short spawnFlags = POSIX_SPAWN_SETPGROUP;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__APPLE__)
    spawnFlags |= 0x2000;
#endif
    int attributeError = posix_spawnattr_setflags(&attributes, spawnFlags);
    if (attributeError == 0) attributeError = posix_spawnattr_setpgroup(&attributes, 0);
    if (attributeError != 0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(stdoutPipe[0]); close(stdoutPipe[1]); close(stderrPipe[0]); close(stderrPipe[1]);
        result.error = L"Could not configure the process isolation group.";
        return result;
    }

    vector<string> encoded;
    encoded.push_back(spec.executable.string());
    for (const auto& argument : spec.arguments) encoded.push_back(wstring_to_utf8(argument));
    vector<char*> arguments;
    for (auto& value : encoded) arguments.push_back(value.data());
    arguments.push_back(nullptr);
    pid_t process = -1;
    const int spawnError = posix_spawn(&process, spec.executable.c_str(), &actions, &attributes, arguments.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(stdoutPipe[1]); close(stderrPipe[1]);
    if (spawnError != 0) {
        close(stdoutPipe[0]); close(stderrPipe[0]);
        result.error = L"Could not start the process.";
        return result;
    }
    g_activeChildGroup.store(process, memory_order_relaxed);
    struct ActiveGroupGuard {
        ~ActiveGroupGuard() {
            g_activeChildGroup.store(0, memory_order_relaxed);
        }
    } groupGuard;
    if (spec.useLowPriority) setpriority(PRIO_PROCESS, process, 10);
    fcntl(stdoutPipe[0], F_SETFL, fcntl(stdoutPipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(stderrPipe[0], F_SETFL, fcntl(stderrPipe[0], F_GETFL) | O_NONBLOCK);

    const auto started = chrono::steady_clock::now();
    bool terminated = false;
    int waitStatus = 0;
    for (;;) {
        DrainDescriptor(stdoutPipe[0], result.standardOutput, spec.maximumCapturedBytes, result.outputTruncated);
        DrainDescriptor(stderrPipe[0], result.standardError, spec.maximumCapturedBytes, result.outputTruncated);
        const pid_t waited = waitpid(process, &waitStatus, WNOHANG);
        if (waited == process) break;
        if (waited < 0) {
            result.error = L"Could not wait for the process.";
            break;
        }
        if (stopToken.stop_requested()) {
            result.status = ProcessStatus::Cancelled;
            terminated = true;
        }
        else if (spec.timeout.count() > 0 && chrono::steady_clock::now() - started >= spec.timeout) {
            result.status = ProcessStatus::TimedOut;
            terminated = true;
        }
        if (terminated) {
            kill(-process, SIGTERM);
            const auto graceEnd = chrono::steady_clock::now() + chrono::seconds(1);
            bool parentReaped = false;
            while (chrono::steady_clock::now() < graceEnd) {
                if (!parentReaped) {
                    const pid_t graceWait = waitpid(process, &waitStatus, WNOHANG);
                    if (graceWait == process) parentReaped = true;
                    else if (graceWait < 0) parentReaped = true;
                }
                this_thread::sleep_for(chrono::milliseconds(20));
            }
            // The direct child may exit on TERM while a descendant ignores it. Kill
            // the process group after the grace period regardless of parent state.
            kill(-process, SIGKILL);
            if (!parentReaped) waitpid(process, &waitStatus, 0);
            break;
        }
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    DrainDescriptor(stdoutPipe[0], result.standardOutput, spec.maximumCapturedBytes, result.outputTruncated);
    DrainDescriptor(stderrPipe[0], result.standardError, spec.maximumCapturedBytes, result.outputTruncated);
    close(stdoutPipe[0]); close(stderrPipe[0]);
    if (!terminated && result.error.empty()) {
        result.exitCode = WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 128 + WTERMSIG(waitStatus);
        result.status = result.exitCode == 0 ? ProcessStatus::Succeeded : ProcessStatus::ExitedWithError;
    }
    return result;
}

#endif

} // namespace

ProcessResult Run(const ProcessSpec& spec, stop_token stopToken) {
    return RunPlatform(spec, stopToken);
}

void TerminateActiveProcess() {
#ifndef _WIN32
    const pid_t group = g_activeChildGroup.exchange(0);
    if (group > 0) {
        ::kill(-group, SIGKILL);
    }
#endif
}

} // namespace ProcessRunner
