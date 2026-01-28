#pragma once

#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <sung/basic/time.hpp>


namespace sung {

    struct ITask {
        virtual ~ITask() = default;
        virtual void run() = 0;
    };


    class TaskManager {

    public:
        TaskManager();
        ~TaskManager();

        TaskManager(const TaskManager&) = delete;
        TaskManager& operator=(const TaskManager&) = delete;
        TaskManager(TaskManager&&) = delete;
        TaskManager& operator=(TaskManager&&) = delete;

        void add_periodic_task(std::shared_ptr<ITask> task, double interval);
        void add_periodic_task(std::function<void()> func, double interval);

    private:
        void run_once();

        struct PeriodicTask;

        std::mutex mut_;
        std::thread thread_;
        std::atomic_bool stop_ = false;

        std::vector<PeriodicTask> periodic_tasks_;
    };

}  // namespace sung
