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


    class ScopedWakeLock final {

    public:
        explicit ScopedWakeLock(GatedPowerRequest& gate) : gate_(gate) {
            // must either succeed or throw without partial state
            gate_.enter();
        }

        ~ScopedWakeLock() noexcept {
            // leave() should be noexcept. If it isn't, catch here.
            gate_.leave();
        }

        ScopedWakeLock(const ScopedWakeLock&) = delete;
        ScopedWakeLock& operator=(const ScopedWakeLock&) = delete;
        ScopedWakeLock(ScopedWakeLock&&) = delete;
        ScopedWakeLock& operator=(ScopedWakeLock&&) = delete;

    private:
        GatedPowerRequest& gate_;
    };


    double get_idle_time();

}  // namespace sung
