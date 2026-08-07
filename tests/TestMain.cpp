#include "BackupPipelineTest.h"
#include "CloudSyncTests.h"
#include "ExternalToolManager.h"
#include "HistoryRepositoryTests.h"
#include "ProcessRunner.h"
#include "ProcessToolTests.h"
#include "RuntimeInfrastructureTests.h"
#include "StorageMigrationTests.h"
#include "TestSupport.h"
#include "text_to_text.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "version") {
        const auto executable = std::filesystem::absolute(argv[0]).wstring();
        const bool managed = executable.find(ExternalToolManager::RcloneVersion) != std::wstring::npos;
        std::cout << (managed ? "rclone v1.74.4\n" : "rclone v9.9.9\n");
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--process-helper-echo") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
        int wideArgumentCount = 0;
        LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideArgumentCount);
        if (!wideArguments || wideArgumentCount != argc) return 2;
#endif
        for (int index = 2; index < argc; ++index) {
#ifdef _WIN32
            std::cout << wstring_to_utf8(wideArguments[index]) << '\n';
#else
            std::cout << argv[index] << '\n';
#endif
        }
#ifdef _WIN32
        LocalFree(wideArguments);
#endif
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--process-helper-output") {
        std::cout << std::string(10000, 'o');
        std::cerr << std::string(10000, 'e');
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--process-helper-cwd") {
        std::cout << wstring_to_utf8(std::filesystem::current_path().wstring()) << '\n';
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--process-helper-grandchild") {
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        std::ofstream(argv[2]) << "should-not-exist";
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--process-helper-spawn") {
#ifdef _WIN32
        ProcessSpec child;
        child.executable = std::filesystem::absolute(argv[0]);
        child.arguments = {L"--process-helper-grandchild", std::filesystem::path(argv[2]).wstring()};
        return ProcessRunner::Run(child).status == ProcessStatus::Succeeded ? 0 : 1;
#else
        // Spawn directly so the grandchild inherits the process group created
        // by the outer ProcessRunner. A nested ProcessRunner intentionally
        // creates another isolation boundary and is not an inheriting child.
        std::string executable = std::filesystem::absolute(argv[0]).string();
        std::string mode = "--process-helper-grandchild";
        std::vector<char*> childArguments{executable.data(), mode.data(), argv[2], nullptr};
        pid_t child = -1;
        if (posix_spawn(&child, executable.c_str(), nullptr, nullptr,
                childArguments.data(), environ) != 0) {
            return 2;
        }
        int status = 0;
        pid_t waited = -1;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
#endif
    }
    TestContext test;
    TemporaryDirectory temporary;
    const auto executable = std::filesystem::absolute(argv[0]);
    RunStorageMigrationTests(test, temporary.path);
    RunProcessToolTests(test, executable, temporary.path);
    RunBackupPipelineTests(test, temporary.path);
    RunHistoryRepositoryTests(test, temporary.path);
    RunRuntimeInfrastructureTests(test, temporary.path);
    RunCloudSyncTests(test);

    if (test.failures == 0) {
        std::cout << "[PASS] MineBackup data-core tests\n";
        return 0;
    }
    std::cerr << test.failures << " test assertion(s) failed\n";
    return 1;
}
