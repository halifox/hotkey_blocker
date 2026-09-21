#include "InjectionOperationReaper.h"

#include <chrono>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::chrono::milliseconds kPollInterval(100);

struct RetainedOperation {
    std::shared_ptr<InjectionOperation> operation;
    std::shared_ptr<void> policyMappingLifetime;
};

class OperationReaper final {
public:
    void Retain(std::shared_ptr<InjectionOperation> operation,
                std::shared_ptr<void> policyMappingLifetime) {
        std::lock_guard lock(m_mutex);
        m_operations.push_back(
            {std::move(operation), std::move(policyMappingLifetime)});
        if (!m_workerStarted) {
            StartWorker();
        }
        m_condition.notify_one();
    }

private:
    void StartWorker() {
        std::thread worker;
        try {
            worker = std::thread([this] { Run(); });
        } catch (...) {
            return;
        }
        worker.detach();
        m_workerStarted = true;
    }

    void Run() {
        for (;;) {
            std::vector<RetainedOperation> operations;
            {
                std::unique_lock lock(m_mutex);
                if (m_operations.empty()) {
                    m_condition.wait(lock, [this] { return !m_operations.empty(); });
                } else {
                    m_condition.wait_for(lock, kPollInterval);
                }
                operations.swap(m_operations);
            }

            std::vector<RetainedOperation> pending;
            pending.reserve(operations.size());
            for (RetainedOperation& retained : operations) {
                InjectionCompletion completion;
                if (!retained.operation->TryComplete(completion)) {
                    pending.push_back(std::move(retained));
                }
            }

            if (!pending.empty()) {
                std::lock_guard lock(m_mutex);
                m_operations.insert(m_operations.end(),
                                    std::make_move_iterator(pending.begin()),
                                    std::make_move_iterator(pending.end()));
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::vector<RetainedOperation> m_operations;
    bool m_workerStarted = false;
};

OperationReaper& GetOperationReaper() {
    static OperationReaper* reaper = new OperationReaper();
    return *reaper;
}

}  // namespace

void InjectionOperationReaper::Retain(std::shared_ptr<InjectionOperation> operation,
                                      std::shared_ptr<void> policyMappingLifetime) {
    if (operation) {
        GetOperationReaper().Retain(std::move(operation), std::move(policyMappingLifetime));
    }
}
