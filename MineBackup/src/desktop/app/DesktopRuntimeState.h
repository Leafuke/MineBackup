#pragma once
#include "DataModels.h"
#include "ModVersion.h"
#include <atomic>
#include <mutex>
#include <condition_variable>

struct AutoBackupTask {
	std::wstring taskName;
    std::wstring configId;
    std::wstring sourcePath;
};

struct MyFolder {
	std::wstring path;		// 世界文件夹路径
	std::wstring name;		// 世界名（文件夹名）
	std::wstring desc;		// 描述
	Config config;			// 所属配置
	int configIndex = -1;	// 所属配置索引
	int worldIndex = -1;	// 世界索引
};

enum class HotRestoreState {
	IDLE,              // 空闲状态
	WAITING_FOR_MOD,   // 已发送请求，正在等待模组响应
	RESTORING,         // 模组已响应，正在执行还原
};

// KnotLink 联动模组状态信息
// 用于跟踪联动模组的检测结果和通信状态
struct KnotLinkModInfo {
	std::atomic<bool> modDetected{false};       // 是否检测到联动模组
	std::string modVersion;                      // 模组版本号
	std::atomic<bool> versionCompatible{false}; // 模组版本是否兼容

	// 最低要求的模组版本号
	static constexpr const char* MIN_MOD_VERSION = "3.0.0";

	// 异步响应同步机制
	std::mutex mtx;
	std::condition_variable cv;

	// 响应标志 (受 mtx 保护)
	bool handshakeReceived = false;            // 收到握手响应
	bool worldSaveComplete = false;            // 模组已完成世界保存 (用于热备份)
	bool worldSaveAndExitComplete = false;     // 模组已完成世界保存并退出 (用于热还原)
	bool rejoinResponseReceived = false;       // 收到重进世界结果
	bool rejoinSuccess = false;                // 重进世界是否成功

	// 重置单次操作的标志 (在每次操作前调用)
	void resetForOperation() {
		std::lock_guard<std::mutex> lock(mtx);
		handshakeReceived = false;
		worldSaveComplete = false;
		worldSaveAndExitComplete = false;
		rejoinResponseReceived = false;
		rejoinSuccess = false;
	}

	// 完全重置
	void resetDetection() {
		modDetected = false;
		modVersion.clear();
		versionCompatible = false;
		resetForOperation();
	}

	// 版本比较
    static bool IsVersionCompatible(const std::string& current, const std::string& required) {
        return ModVersion::IsCompatible(current, required);
    }

	// 通知指定标志并唤醒等待线程
	void notifyFlag(bool KnotLinkModInfo::* flag, bool value = true) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			this->*flag = value;
		}
		cv.notify_all();
	}

	// 等待指定标志变为 true，带超时
	bool waitForFlag(bool KnotLinkModInfo::* flag, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mtx);
		return cv.wait_for(lock, timeout, [this, flag]() { return this->*flag; });
	}
};
