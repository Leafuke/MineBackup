#include "TaskCoordinator.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <memory>
#include <future>
#include <thread>

using namespace std;

namespace {
thread_local stop_token g_currentStopToken;
}

struct TaskCoordinator::Impl {
    struct TaskRecord {
        wstring name;
        shared_ptr<atomic<bool>> done;
        jthread worker;
    };

    mutable mutex stateMutex;
    bool accepting = true;
    vector<unique_ptr<TaskRecord>> tasks;
    map<wstring, shared_ptr<mutex>> resources;
    mutex eventMutex;
    vector<TaskEvent> events;
};

TaskCoordinator& TaskCoordinator::Instance() {
    static TaskCoordinator coordinator;
    return coordinator;
}

TaskCoordinator::TaskCoordinator() : impl_(new Impl()) {}

TaskCoordinator::~TaskCoordinator() {
    StopAndJoin();
    delete impl_;
}

bool TaskCoordinator::Submit(wstring name, vector<wstring> resourceKeys, TaskFunction function) {
    if (!function) return false;
    sort(resourceKeys.begin(), resourceKeys.end());
    resourceKeys.erase(unique(resourceKeys.begin(), resourceKeys.end()), resourceKeys.end());

    lock_guard lock(impl_->stateMutex);
    if (!impl_->accepting) return false;
    impl_->tasks.erase(remove_if(impl_->tasks.begin(), impl_->tasks.end(), [](const auto& task) {
        return task->done->load();
    }), impl_->tasks.end());
    vector<shared_ptr<mutex>> resourceMutexes;
    for (const auto& key : resourceKeys) {
        if (key.empty()) continue;
        auto& resource = impl_->resources[key];
        if (!resource) resource = make_shared<mutex>();
        resourceMutexes.push_back(resource);
    }

    auto record = make_unique<Impl::TaskRecord>();
    record->name = std::move(name);
    record->done = make_shared<atomic<bool>>(false);
    const auto taskName = record->name;
    record->worker = jthread([this, taskName, locks = std::move(resourceMutexes),
        task = std::move(function), done = record->done](stop_token token) mutable {
        try {
            vector<unique_lock<mutex>> heldLocks;
            heldLocks.reserve(locks.size());
            for (const auto& resource : locks) heldLocks.emplace_back(*resource);
            if (token.stop_requested()) {
                done->store(true);
                return;
            }
            g_currentStopToken = token;
            task(token);
        }
        catch (...) {
            PostEvent({L"task-failed", taskName});
        }
        g_currentStopToken = {};
        done->store(true);
    });
    impl_->tasks.push_back(std::move(record));
    return true;
}

bool TaskCoordinator::RequestStop(const wstring& name) {
    lock_guard lock(impl_->stateMutex);
    bool found = false;
    for (auto& task : impl_->tasks) {
        if (task->name == name && !task->done->load()) {
            task->worker.request_stop();
            found = true;
        }
    }
    return found;
}

bool TaskCoordinator::SubmitAndWait(wstring name, vector<wstring> resourceKeys, TaskFunction function) {
    auto completion = make_shared<promise<void>>();
    auto completed = completion->get_future();
    const bool accepted = Submit(std::move(name), std::move(resourceKeys),
        [task = std::move(function), completion](stop_token token) {
            try {
                task(token);
                completion->set_value();
            }
            catch (...) {
                completion->set_exception(current_exception());
                throw;
            }
        });
    if (!accepted) return false;
    try {
        completed.get();
        return true;
    }
    catch (...) {
        return false;
    }
}

void TaskCoordinator::PostEvent(TaskEvent event) {
    lock_guard lock(impl_->eventMutex);
    impl_->events.push_back(std::move(event));
}

vector<TaskEvent> TaskCoordinator::PollEvents() {
    lock_guard lock(impl_->eventMutex);
    vector<TaskEvent> events;
    events.swap(impl_->events);
    return events;
}

void TaskCoordinator::StopAndJoin() {
    if (!impl_) return;
    vector<unique_ptr<Impl::TaskRecord>> tasks;
    {
        lock_guard lock(impl_->stateMutex);
        if (!impl_->accepting && impl_->tasks.empty()) return;
        impl_->accepting = false;
        for (auto& task : impl_->tasks) task->worker.request_stop();
        tasks.swap(impl_->tasks);
    }
    tasks.clear(); // jthread destruction joins outside stateMutex.
}

bool TaskCoordinator::IsAcceptingTasks() const {
    if (!impl_) return false;
    lock_guard lock(impl_->stateMutex);
    return impl_->accepting;
}

stop_token TaskCoordinator::CurrentStopToken() {
    return g_currentStopToken;
}

wstring TaskCoordinator::WorldResourceKey(const wstring& configId, const filesystem::path& worldPath) {
    error_code error;
    auto absolute = filesystem::absolute(worldPath, error);
    if (error) absolute = worldPath;
    error.clear();
    auto path = filesystem::weakly_canonical(absolute, error);
    if (error) path = absolute.lexically_normal();
    wstring identity = configId + L"|" + path.wstring();
#ifdef _WIN32
    transform(identity.begin(), identity.end(), identity.begin(), ::towlower);
#endif
    return L"world:" + identity;
}

wstring TaskCoordinator::CloudResourceKey(const wstring& profileIdentity) {
    return L"cloud:" + profileIdentity;
}

wstring TaskCoordinator::AutoBackupTaskName(int configIndex, int worldIndex) {
    static atomic<unsigned long long> nextInstance{1};
    return L"auto-backup:" + to_wstring(configIndex) + L":" + to_wstring(worldIndex)
        + L":" + to_wstring(nextInstance.fetch_add(1));
}
