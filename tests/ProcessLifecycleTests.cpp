#include "ProcessLifecycleTests.h"
#include "CliSignalHandler.h"
#include "ProcessRunner.h"
#include "TestSupport.h"
#include "text_to_text.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
#include "ProcessRunnerTestAccess.h"
#endif
extern char** environ;
#endif

using namespace std;
using namespace chrono_literals;

namespace {
bool Until(const function<bool()>& predicate, chrono::milliseconds timeout = 5s) {
    const auto deadline = chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (chrono::steady_clock::now() >= deadline) return false;
        this_thread::sleep_for(10ms);
    }
    return true;
}

void Mark(const filesystem::path& path) { ofstream(path) << "ready"; }

struct Running {
    stop_source cancellation;
    ProcessResult result;
    atomic<bool> done{false};
    jthread worker;
    explicit Running(ProcessSpec spec) : worker([this, spec = std::move(spec)] {
        result = ProcessRunner::Run(spec, cancellation.get_token());
        done.store(true);
    }) {}
    ~Running() { cancellation.request_stop(); worker.join(); }
};

#ifndef _WIN32
int Child(const filesystem::path& executable, const filesystem::path& root, const string& mode) {
    filesystem::create_directories(root);
    if (mode != "term-parent") signal(SIGTERM, SIG_IGN);
    else signal(SIGTERM, SIG_DFL);
    if (mode != "leaf") {
        string exe = executable.string(), directory = (root / "descendant").string();
        string option = "--lifetime-child", leaf = "leaf";
        vector<char*> args{exe.data(), option.data(), directory.data(), leaf.data(), nullptr};
        pid_t child = -1;
        if (posix_spawn(&child, exe.c_str(), nullptr, nullptr, args.data(), environ) != 0) return 2;
        if (!Until([&] { return filesystem::exists(root / "descendant" / "ready"); })) return 3;
    }
    ofstream(root / "pid") << getpid();
    Mark(root / "ready");
    return Until([&] { return filesystem::exists(root / "release"); }, 15s) ? 0 : 4;
}

pid_t ReadPid(const filesystem::path& root) {
    pid_t value = -1;
    ifstream(root / "pid") >> value;
    return value;
}

bool Alive(pid_t process) {
    if (process <= 1) return false;
    if (kill(process, 0) != 0) return errno != ESRCH;
#ifdef __APPLE__
    int query[]{CTL_KERN, KERN_PROC, KERN_PROC_PID, process};
    kinfo_proc info{};
    size_t size = sizeof(info);
    if (sysctl(query, 4, &info, &size, nullptr, 0) != 0) return true;
    return size != 0 && info.kp_proc.p_stat != SZOMB;
#else
    string state;
    ifstream input(filesystem::path("/proc") / to_string(process) / "stat");
    getline(input, state);
    const auto end = state.rfind(')');
    return end != string::npos && state.substr(end + 2, 1) != "Z";
#endif
}

ProcessSpec Spec(const filesystem::path& executable, const filesystem::path& root, const wstring& mode = L"leaf") {
    ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {L"--lifetime-child", root.wstring(), mode};
    spec.timeout = 10s;
    return spec;
}

bool Ready(TestContext& test, const filesystem::path& root) {
    const bool ready = Until([&] { return filesystem::exists(root / "ready"); });
    test.Expect(ready, "lifetime helper must acknowledge startup");
    return ready;
}

void Gone(TestContext& test, const filesystem::path& root) {
    const pid_t child = ReadPid(root);
    const bool gone = Until([&] { return !Alive(child); }, 2s);
    test.Expect(gone, "managed descendant must terminate, including when its parent exits first");
    if (!gone) {
        cerr << "[DETAIL] surviving managed descendant PID=" << child << '\n';
        kill(child, SIGKILL); // This test owns the still-running helper.
    }
}

int Scenario(const string& name, const filesystem::path& executable, const filesystem::path& root) {
    TestContext test;
    filesystem::create_directories(root);
    if (name == "both" || name == "first-exits") {
        Running first(Spec(executable, root / "a", L"parent"));
        if (!Ready(test, root / "a")) return 1;
        Running second(Spec(executable, root / "b", L"parent"));
        if (!Ready(test, root / "b")) return 1;
        if (name == "first-exits") {
            Mark(root / "a" / "release");
            test.Expect(Until([&] { return first.done.load(); }), "A must finish before shutdown begins");
            Gone(test, root / "a" / "descendant");
        }
        ProcessRunner::TerminateActiveProcess();
        ProcessRunner::TerminateActiveProcess();
        const bool allDone = Until([&] { return first.done.load() && second.done.load(); }, 2s);
        test.Expect(allDone, "forced shutdown must finish both concurrent ProcessRunner calls");
        if (!allDone) cerr << "[DETAIL] surviving managed PID/PGID=" << ReadPid(root / "b") << '\n';
        if (allDone) {
            Gone(test, root / "a" / "descendant");
            Gone(test, root / "b" / "descendant");
        }
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
        const auto denied = ProcessRunner::Run(Spec(executable, root / "denied"));
        test.Expect(denied.status == ProcessStatus::Cancelled && !filesystem::exists(root / "denied"),
            "terminal shutdown must reject subsequent launches before spawn");
#endif
    }
    else if (name == "normal" || name == "cancel" || name == "timeout") {
        auto spec = Spec(executable, root / "a", name == "normal" ? L"parent" : L"term-parent");
        if (name == "timeout") spec.timeout = 1500ms;
        Running child(spec);
        if (!Ready(test, root / "a")) return 1;
        if (name == "normal") Mark(root / "a" / "release");
        else if (name == "cancel") child.cancellation.request_stop();
        test.Expect(Until([&] { return child.done.load(); }), "owned child should complete within its deadline");
        if (child.done) {
            const auto expected = name == "normal" ? ProcessStatus::Succeeded
                : name == "cancel" ? ProcessStatus::Cancelled : ProcessStatus::TimedOut;
            test.Expect(child.result.status == expected, "lifecycle cleanup must preserve the process result");
            Gone(test, root / "a" / "descendant");
        }
    }
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
    else if (name == "capacity") {
        ProcessRunner::Testing::SetCapacity(2);
        auto invalid = Spec(executable.parent_path() / "missing-executable", root / "missing");
        test.Expect(ProcessRunner::Run(invalid).status == ProcessStatus::FailedToStart,
            "failed spawn must return its reserved slot");
        Running first(Spec(executable, root / "a"));
        if (!Ready(test, root / "a")) return 1;
        Running second(Spec(executable, root / "b"));
        if (!Ready(test, root / "b")) return 1;
        auto full = ProcessRunner::Run(Spec(executable, root / "c"));
        test.Expect(full.status == ProcessStatus::FailedToStart && full.error.find(L"Too many") != wstring::npos
                && !filesystem::exists(root / "c"), "full registry must reject before creating an unmanaged process");
        Mark(root / "a" / "release");
        test.Expect(Until([&] { return first.done.load(); }), "completed slot must be released");
        Running third(Spec(executable, root / "c"));
        if (Ready(test, root / "c")) Mark(root / "c" / "release");
        Mark(root / "b" / "release");
    }
    else if (name == "spawn-window") {
        struct Barrier { atomic<bool> reached{false}; atomic<bool> release{false}; } barrier;
        ProcessRunner::Testing::SetAfterSpawn([](void* context) noexcept {
            auto& state = *static_cast<Barrier*>(context);
            state.reached = true;
            const auto deadline = chrono::steady_clock::now() + 5s;
            while (!state.release && chrono::steady_clock::now() < deadline) this_thread::sleep_for(1ms);
        }, &barrier);
        Running child(Spec(executable, root / "a"));
        test.Expect(Until([&] { return barrier.reached.load(); }), "test must pause after spawn before registration");
        atomic<bool> stopped{false};
        jthread shutdown([&] { ProcessRunner::TerminateActiveProcess(); stopped = true; });
        test.Expect(Until([] { return ProcessRunner::Testing::IsClosing(); }), "shutdown must close admission before acquiring spawn lock");
        test.Expect(!stopped, "shutdown must not finish while spawned child is unpublished");
        test.Expect(ProcessRunner::Run(Spec(executable, root / "denied")).status == ProcessStatus::Cancelled,
            "new launches must be refused even while the spawn transaction is occupied");
        barrier.release = true;
        shutdown.join();
        test.Expect(Until([&] { return child.done.load(); }), "published child must be killed after the spawn transaction completes");
        ProcessRunner::Testing::SetAfterSpawn(nullptr, nullptr);
    }
    else if (name == "wait-errors") {
        for (const int error : {EINTR, EIO}) {
            const auto directory = root / to_string(error);
            Running child(Spec(executable, directory));
            if (!Ready(test, directory)) return 1;
            ProcessRunner::Testing::FailNextWait(error);
            if (error == EINTR) Mark(directory / "release");
            test.Expect(Until([&] { return child.done.load(); }), "interrupted or failed waits must not orphan the process");
            if (child.done) test.Expect(error == EINTR ? child.result.status == ProcessStatus::Succeeded
                : child.result.status != ProcessStatus::Succeeded && !child.result.error.empty(),
                "EINTR retries, while real wait failures cannot report success");
            Gone(test, directory);
        }
    }
#endif
    else return 2;
    return test.failures ? 1 : 0;
}
#endif

int SignalHost(int argc, char** argv) {
    if (argc != 5) return 2;
    const filesystem::path root = utf8_to_wstring(argv[4]);
    CliSignalHandler signals;
    // request_stop executes callbacks synchronously. The forced path must not
    // depend on this callback or on any task completing cooperative shutdown.
    stop_callback blocked(signals.Token(), [&] {
        Mark(root / "cancel-blocked");
        Until([&] { return filesystem::exists(root / "release-cancel"); }, 15s);
    });
    auto spec = [&](const wchar_t* name) {
        ProcessSpec result;
        result.executable = utf8_to_wstring(argv[2]);
        result.arguments = {utf8_to_wstring(argv[3]), root.wstring(), name};
        result.timeout = 20s;
        return result;
    };
    Running first(spec(L"a"));
    Running second(spec(L"b"));
    Until([&] { return first.done.load() && second.done.load(); }, 20s);
    return 3; // Only the real forced exit can produce the expected exit 9.
}
} // namespace

optional<int> RunProcessLifecycleMode(int argc, char** argv) {
    if (argc < 2) return nullopt;
    const string mode = argv[1];
    if (mode == "--signal-contract-host") return SignalHost(argc, argv);
#ifndef _WIN32
    const auto executable = filesystem::absolute(argv[0]);
    if (mode == "--lifetime-child" && argc == 4) return Child(executable, argv[2], argv[3]);
    if (mode == "--lifecycle-case" && argc == 4) return Scenario(argv[2], executable, argv[3]);
#endif
    if (mode != "--process-lifecycle-tests") return nullopt;
    TestContext test;
    TemporaryDirectory root;
#ifndef _WIN32
    vector<wstring> cases{L"both", L"first-exits", L"normal", L"cancel", L"timeout"};
#ifdef MINEBACKUP_PROCESS_TEST_HOOKS
    cases.insert(cases.end(), {L"capacity", L"spawn-window", L"wait-errors"});
#endif
    for (const auto& name : cases) {
        ProcessSpec spec;
        spec.executable = executable;
        spec.arguments = {L"--lifecycle-case", name, (root.path / name).wstring()};
        spec.timeout = 20s;
        const auto result = ProcessRunner::Run(spec);
        test.Expect(result.status == ProcessStatus::Succeeded, "isolated POSIX process lifecycle scenario");
        cout << "[CASE] " << wstring_to_utf8(name) << " exit=" << result.exitCode << '\n'
             << result.standardOutput << result.standardError;
    }
#else
    // Platform-neutral success/error/cancellation/output behavior is exercised
    // by data_core; native forced Job Object cleanup uses --signal-contract-host.
    cout << "[PASS] Windows lifecycle uses native Job Objects; see CLI process contract\n";
#endif
    return test.failures ? 1 : 0;
}
