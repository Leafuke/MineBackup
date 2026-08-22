#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace AtomicFileWriter {

// 区分原子写入的三种提交状态：
// - NotReplaced         target 从未被替换，调用方可安全重试或回滚内存状态；
// - ReplacedNotDurable  target 已被替换（commit point 已越过），
//                       但父目录同步未确认，不允许按“什么都没发生”回滚；
// - Durable             替换且目录同步全部完成。
enum class WriteCommitState {
    NotReplaced,
    ReplacedNotDurable,
    Durable
};

struct WriteOptions {
    bool keepBackup = true;
    bool createParentDirectories = true;
    // Optional deterministic observation point for low-level contention tests.
    std::function<void(const std::error_code&, std::size_t)> replaceFailureObserver;
    // Test-only injection: when set, replaces the real directory sync to
    // deterministically simulate a post-replacement persistence failure.
    // Must remain empty in production code paths.
    std::function<bool(const std::filesystem::path&)> directorySyncOverride;
};

struct WriteResult {
    // 保留兼容含义：完整写流程（含目录同步）完成才为 true。
    bool success = false;
    WriteCommitState commitState = WriteCommitState::NotReplaced;

    std::filesystem::path backupPath;
    std::wstring error;

    // target 是否已被新内容替换（无论目录同步是否确认）。
    bool WasReplaced() const noexcept {
        return commitState != WriteCommitState::NotReplaced;
    }
    bool IsDurable() const noexcept {
        return commitState == WriteCommitState::Durable;
    }
};

WriteResult WriteText(
    const std::filesystem::path& target,
    const std::string& content,
    const WriteOptions& options = {});

} // namespace AtomicFileWriter
