#include "task/img_walker.hpp"

#include <print>

#include <sung/basic/time.hpp>

#include "sung/image/png.hpp"


namespace {

    class Task : public sung::ITask {

    public:
        Task(const sung::ServerConfigs& cfg) : cfg_(cfg) {}

        void run() override {
            sung::MonotonicRealtimeTimer timer;

            std::vector<sung::Path> png_files;

            for (const auto& [name, binding_info] : cfg_.dir_bindings()) {
                for (const auto& local_dir : binding_info.local_dirs_) {
                    for (auto entry :
                         sung::fs::recursive_directory_iterator(local_dir)) {
                        if (entry.path().extension() == ".png") {
                            png_files.push_back(entry.path());
                        }
                    }
                }
            }

            for (const auto& p : png_files) {
                (void)p;
            }

            std::print("ImgWalker: scanned {} files\n", png_files.size());
            std::print(
                "ImgWalker: elapsed time {:.3f} sec\n",
                timer.check_get_elapsed()
            );
        }

    private:
        sung::ServerConfigs cfg_;
    };

}  // namespace


namespace sung {

    std::shared_ptr<ITask> create_img_walker_task(const ServerConfigs& cfg) {
        return std::make_shared<::Task>(cfg);
    }

}  // namespace sung
