#include "util/task.hpp"


// TaskManager
namespace sung {

    struct TaskManager::PeriodicTask {
        std::shared_ptr<ITask> task_;
        sung::MonotonicRealtimeTimer timer_;
        double interval_ = 0;
    };


    TaskManager::TaskManager() {
        thread_ = std::thread([this]() {
            while (!stop_) {
                this->run_once();
                sung::sleep_naive(0.1);
            }
        });
    }

    TaskManager::~TaskManager() {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    void TaskManager::add_periodic_task(
        std::shared_ptr<ITask> task, double interval
    ) {
        auto& entry = periodic_tasks_.emplace_back();
        entry.task_ = std::move(task);
        entry.interval_ = interval;
        entry.timer_.check();
    }

    void TaskManager::add_periodic_task(
        std::function<void()> func, double interval
    ) {
        struct FuncTask : ITask {
            FuncTask(std::function<void()> f) : func_(std::move(f)) {}
            void run() override { func_(); }
            std::function<void()> func_;
        };

        this->add_periodic_task(
            std::make_shared<FuncTask>(std::move(func)), interval
        );
    }

    void TaskManager::run_once() {
        std::lock_guard lock{ mut_ };

        for (auto& entry : periodic_tasks_) {
            if (entry.timer_.check_if_elapsed(entry.interval_)) {
                entry.task_->run();
                entry.timer_.check();
                return;
            }
        }
    }

}  // namespace sung
