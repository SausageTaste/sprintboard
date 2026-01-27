#pragma once

#include <atomic>


namespace sung {

    class PowerRequest {

    public:
        PowerRequest(const wchar_t* reason);
        ~PowerRequest();

        bool ok() const;

        bool set_system_required(bool on);
        bool set_display_required(bool on);
        bool clear_system_required();
        bool clear_display_required();

    private:
        void* handle_ = nullptr;
    };


    class GatedPowerRequest {

    public:
        GatedPowerRequest();

        void enter();
        void leave();

        int count() const;

    private:
        PowerRequest power_req_;
        std::atomic<int> gate_count_ = 0;
    };


    double get_idle_time();

}  // namespace sung
