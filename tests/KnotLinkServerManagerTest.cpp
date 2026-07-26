#include "KnotLinkServerManager.h"

#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace minebackup::knotlink;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

class FakePlatform final : public IKnotLinkServerPlatform {
public:
    KnotLinkServerDiscovery discovery{
        true, true, L"C:\\KnotLinkService.exe", "3.0.0.0", {}};
    bool processRunning = false;
    bool signalReady = false;
    bool responderReady = false;
    bool startResult = true;
    bool waitResult = true;
    int startCalls = 0;
    int waitCalls = 0;

    KnotLinkServerDiscovery Discover() override { return discovery; }
    bool IsProcessRunning() override { return processRunning; }
    bool IsPortReady(unsigned short port) override {
        return port == KnotLinkServerManager::SignalPort
            ? signalReady
            : responderReady;
    }
    bool Start(const std::filesystem::path&) override {
        ++startCalls;
        return startResult;
    }
    bool WaitForReady(
        std::chrono::milliseconds,
        unsigned short,
        unsigned short) override {
        ++waitCalls;
        return waitResult;
    }
};

void TestDisabledAndPlatformStates() {
    auto platform = std::make_shared<FakePlatform>();
    KnotLinkServerManager manager(platform);
    Check(manager.Refresh(false).state == KnotLinkServerState::Disabled,
          "disabled integration should use Disabled state");

    platform->discovery.managedPlatform = false;
    platform->discovery.installed = false;
    platform->discovery.detail =
        "KnotLink does not currently provide a server for this platform.";
    Check(manager.Refresh(true).state == KnotLinkServerState::NotInstalled,
          "non-Windows without listening ports should explain server unavailability");

    platform->signalReady = true;
    platform->responderReady = true;
    Check(manager.Refresh(true).state == KnotLinkServerState::Ready,
          "non-Windows should accept a future external local server");
}

void TestCompatibilityGate() {
    auto platform = std::make_shared<FakePlatform>();
    KnotLinkServerManager manager(platform);

    platform->discovery.installed = false;
    Check(manager.Refresh(true).state == KnotLinkServerState::NotInstalled,
          "missing installation should be detected");

    platform->discovery.installed = true;
    platform->discovery.version.clear();
    Check(manager.Refresh(true).state == KnotLinkServerState::Incompatible,
          "unknown Windows server version should be blocked");

    platform->discovery.version = "2.9.9";
    Check(manager.Refresh(true).state == KnotLinkServerState::Incompatible,
          "server below 3.0.0 should be blocked");

    platform->discovery.version = "3.0.0.0";
    Check(manager.Refresh(true).state == KnotLinkServerState::Stopped,
          "compatible installed server without ports should be stopped");
    Check(KnotLinkServerManager::IsVersionCompatible("3.1.0"),
          "newer semantic version should be compatible");
    Check(!KnotLinkServerManager::IsVersionCompatible("3.0"),
          "incomplete version should be rejected");
    Check(!KnotLinkServerManager::IsVersionCompatible("3.0.0-preview"),
          "version suffix should not bypass compatibility gate");
}

void TestSmartStartup() {
    auto platform = std::make_shared<FakePlatform>();
    KnotLinkServerManager manager(platform);

    auto status = manager.EnsureReady(true, false);
    Check(status.state == KnotLinkServerState::Stopped &&
              platform->startCalls == 0,
          "auto-start disabled should not launch the server");

    status = manager.EnsureReady(true, true);
    Check(status.state == KnotLinkServerState::Ready &&
              platform->startCalls == 1 && platform->waitCalls == 1,
          "compatible stopped server should start and wait for both ports");

    platform->waitResult = false;
    status = manager.EnsureReady(true, true);
    Check(status.state == KnotLinkServerState::Failed,
          "port readiness timeout should enter Failed state");

    platform->waitResult = true;
    platform->startResult = false;
    status = manager.EnsureReady(true, true);
    Check(status.state == KnotLinkServerState::Failed,
          "process launch failure should enter Failed state");

    platform->startResult = true;
    platform->processRunning = true;
    status = manager.Refresh(true);
    Check(status.state == KnotLinkServerState::Stopped &&
              status.processRunning,
          "running process without both ports should remain non-ready");
}

} // namespace

int main() {
    TestDisabledAndPlatformStates();
    TestCompatibilityGate();
    TestSmartStartup();
    if (failures == 0) {
        std::cout << "KnotLink server manager tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
