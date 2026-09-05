#include "ProcessRunner.h"

#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>
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
            constexpr DWORD kTerminationWaitMs = 300;
            WaitForSingleObject(process.hProcess, kTerminationWaitMs);
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

constexpr size_t kMaximumActiveGroups = 64;
mutex g_lifecycleMutex;
array<pid_t, kMaximumActiveGroups> g_activeGroups{};
atomic<bool> g_closing{false};
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
size_t g_capacity = kMaximumActiveGroups;
void (*g_afterSpawn)(void*) noexcept = nullptr;
void* g_afterSpawnContext = nullptr;
atomic<int> g_nextWaitError{0};
#endif

struct OutputPipe {
    int descriptors[2]{-1, -1};
    ~OutputPipe() { Close(0); Close(1); }
    void Close(size_t index) noexcept {
        if (descriptors[index] >= 0) close(exchange(descriptors[index], -1));
    }
    bool Open() {
        if (pipe(descriptors) != 0) return false;
        for (const int descriptor : descriptors) {
            const int flags = fcntl(descriptor, F_GETFD);
            if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) return false;
        }
        const int flags = fcntl(descriptors[0], F_GETFL);
        return flags >= 0 && fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0;
    }
};

struct SpawnSetup {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool actionsReady = false;
    bool attributesReady = false;
    ~SpawnSetup() {
        if (attributesReady) posix_spawnattr_destroy(&attributes);
        if (actionsReady) posix_spawn_file_actions_destroy(&actions);
    }
};

// The owning Run is the only reaper. Keep the leader waitable until all group
// signals have been sent; otherwise PID reuse could turn cleanup into a signal
// to an unrelated process group. Reaping and removal share the shutdown lock.
struct OwnedGroup {
    pid_t process = 0;
    size_t slot = 0;
    ~OwnedGroup() {
        if (process > 0) {
            int ignored = 0;
            Reap(ignored);
        }
    }
    void Signal(int signal) noexcept {
        lock_guard lock(g_lifecycleMutex);
        if (process > 1 && g_activeGroups[slot] == process) kill(-process, signal);
    }
    int Observe(siginfo_t& info) noexcept {
        lock_guard lock(g_lifecycleMutex);
        int result;
        do {
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
            const int injected = g_nextWaitError.exchange(0);
            if (injected) { errno = injected; result = -1; }
            else
#endif
            result = waitid(P_PID, process, &info, WEXITED | WNOHANG | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno == ECHILD) {
            // Ownership was lost (e.g. an external reaper). Never signal a PID
            // whose identity is no longer pinned by our unreaped child.
            g_activeGroups[slot] = 0;
            process = 0;
        }
        return result;
    }
    bool Reap(int& status) noexcept {
        if (process == 0) return false;
        Signal(SIGKILL);
        for (;;) {
            {
                lock_guard lock(g_lifecycleMutex);
                const pid_t waited = waitpid(process, &status, WNOHANG);
                if (waited == process || (waited < 0 && errno != EINTR)) {
                    g_activeGroups[slot] = 0;
                    process = 0;
                    return waited > 0;
                }
            }
            // Never hold the lifecycle lock across a blocking wait.
            this_thread::sleep_for(chrono::milliseconds(10));
        }
    }
};

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
    if (g_closing.load() || stopToken.stop_requested()) {
        result.status = ProcessStatus::Cancelled;
        return result;
    }
    if (spec.executable.empty() || spec.maximumCapturedBytes == 0) {
        result.error = L"The process specification is incomplete.";
        return result;
    }
    vector<string> encoded;
    encoded.push_back(spec.executable.string());
    for (const auto& argument : spec.arguments) encoded.push_back(wstring_to_utf8(argument));
    vector<char*> arguments;
    for (auto& value : encoded) arguments.push_back(value.data());
    arguments.push_back(nullptr);

    OutputPipe output, errorOutput;
    if (!output.Open() || !errorOutput.Open()) {
        result.error = L"Could not create process output pipes.";
        return result;
    }
    SpawnSetup setup;
    setup.actionsReady = posix_spawn_file_actions_init(&setup.actions) == 0;
    if (!setup.actionsReady) {
        result.error = L"Could not initialize process output redirection.";
        return result;
    }
    auto& actions = setup.actions;
    int actionsError = posix_spawn_file_actions_adddup2(&actions, output.descriptors[1], STDOUT_FILENO);
    if (actionsError == 0) actionsError = posix_spawn_file_actions_adddup2(&actions, errorOutput.descriptors[1], STDERR_FILENO);
    if (actionsError == 0) actionsError = posix_spawn_file_actions_addclose(&actions, output.descriptors[0]);
    if (actionsError == 0) actionsError = posix_spawn_file_actions_addclose(&actions, errorOutput.descriptors[0]);
    if (actionsError != 0) {
        result.error = L"Could not configure process output redirection.";
        return result;
    }
#if defined(__APPLE__) || defined(__GLIBC__)
    if (!spec.workingDirectory.empty()
        && posix_spawn_file_actions_addchdir_np(&actions, spec.workingDirectory.c_str()) != 0) {
        result.error = L"Could not apply the process working directory.";
        return result;
    }
#endif
    setup.attributesReady = posix_spawnattr_init(&setup.attributes) == 0;
    if (!setup.attributesReady) {
        result.error = L"Could not initialize the process isolation group.";
        return result;
    }
    auto& attributes = setup.attributes;
    short spawnFlags = POSIX_SPAWN_SETPGROUP;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__APPLE__)
    spawnFlags |= 0x2000;
#endif
    int attributeError = posix_spawnattr_setflags(&attributes, spawnFlags);
    if (attributeError == 0) attributeError = posix_spawnattr_setpgroup(&attributes, 0);
    if (attributeError != 0) {
        result.error = L"Could not configure the process isolation group.";
        return result;
    }

    OwnedGroup owned;
    {
        lock_guard lock(g_lifecycleMutex);
        if (g_closing.load() || stopToken.stop_requested()) {
            result.status = ProcessStatus::Cancelled;
            return result;
        }
        size_t capacity = kMaximumActiveGroups;
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
        capacity = g_capacity;
#endif
        while (owned.slot < capacity && g_activeGroups[owned.slot] != 0) ++owned.slot;
        if (owned.slot == capacity) {
            result.error = L"Too many concurrent external processes are already active.";
            return result;
        }
        g_activeGroups[owned.slot] = -1; // Reserved before spawn, under the lifecycle lock.
        const int spawnError = posix_spawn(&owned.process, spec.executable.c_str(),
            &actions, &attributes, arguments.data(), environ);
        if (spawnError != 0) {
            g_activeGroups[owned.slot] = 0;
            owned.process = 0;
            result.error = L"Could not start the process.";
            return result;
        }
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
        if (g_afterSpawn) g_afterSpawn(g_afterSpawnContext);
#endif
        g_activeGroups[owned.slot] = owned.process;
    }
    output.Close(1);
    errorOutput.Close(1);
    if (spec.useLowPriority) setpriority(PRIO_PROCESS, owned.process, 10);

    const auto started = chrono::steady_clock::now();
    bool terminated = false;
    for (;;) {
        DrainDescriptor(output.descriptors[0], result.standardOutput, spec.maximumCapturedBytes, result.outputTruncated);
        DrainDescriptor(errorOutput.descriptors[0], result.standardError, spec.maximumCapturedBytes, result.outputTruncated);
        siginfo_t info{};
        if (owned.Observe(info) < 0) {
            result.status = ProcessStatus::ExitedWithError;
            result.error = L"Could not wait for the process.";
            break;
        }
        if (info.si_pid != 0) break;
        if (stopToken.stop_requested()) {
            result.status = ProcessStatus::Cancelled;
            terminated = true;
        }
        else if (spec.timeout.count() > 0 && chrono::steady_clock::now() - started >= spec.timeout) {
            result.status = ProcessStatus::TimedOut;
            terminated = true;
        }
        if (terminated) {
            owned.Signal(SIGTERM);
            // Keep the leader unreaped throughout grace. Its descendants may
            // ignore TERM, even when the leader has already exited.
            const auto graceEnd = chrono::steady_clock::now() + chrono::seconds(1);
            while (chrono::steady_clock::now() < graceEnd && !g_closing.load()) {
                this_thread::sleep_for(chrono::milliseconds(10));
            }
            break;
        }
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    int waitStatus = 0;
    const bool reaped = owned.Reap(waitStatus);
    if (!reaped && result.error.empty()) result.error = L"Could not reap the process.";
    DrainDescriptor(output.descriptors[0], result.standardOutput, spec.maximumCapturedBytes, result.outputTruncated);
    DrainDescriptor(errorOutput.descriptors[0], result.standardError, spec.maximumCapturedBytes, result.outputTruncated);
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
    // Close admission before waiting for an in-flight spawn to publish its PID.
    g_closing.store(true);
    lock_guard lock(g_lifecycleMutex);
    for (const pid_t group : g_activeGroups) {
        if (group > 1) ::kill(-group, SIGKILL);
    }
    // Each Run remains the sole reaper and retires its own slot. Do not clear
    // identities before signaling, or permit another launch after shutdown.
#endif
}

#if defined(MINEBACKUP_PROCESS_TEST_HOOKS) && !defined(_WIN32)
namespace Testing {
void SetCapacity(size_t capacity) {
    lock_guard lock(g_lifecycleMutex);
    g_capacity = min(capacity, kMaximumActiveGroups);
}
void SetAfterSpawn(void (*hook)(void*) noexcept, void* context) {
    lock_guard lock(g_lifecycleMutex);
    g_afterSpawn = hook;
    g_afterSpawnContext = context;
}
void FailNextWait(int error) { g_nextWaitError.store(error); }
bool IsClosing() { return g_closing.load(); }
}
#endif

} // namespace ProcessRunner
