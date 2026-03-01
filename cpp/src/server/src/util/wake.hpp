#pragma once

#include <atomic>

#include <sung/basic/logic_gate.hpp>


namespace sung {

    struct PowerRequestBackend {
        virtual ~PowerRequestBackend() = default;

        virtual bool set_system() = 0;
        virtual bool set_display() = 0;
        virtual bool clear_system() = 0;
        virtual bool clear_display() = 0;
    };


    class PowerRequest {

    public:
        PowerRequest(std::string_view reason);
        ~PowerRequest();

        bool ok() const;

        bool set_system_required();
        bool set_display_required();
        bool clear_system_required();
        bool clear_display_required();

        bool is_system_required() const { return system_required_; }
        bool is_display_required() const { return display_required_; }

    private:
        std::unique_ptr<PowerRequestBackend> backend_;
        void* handle_ = nullptr;
        bool system_required_ = false;
        bool display_required_ = false;
    };


    class GatedPowerRequest {

    public:
        GatedPowerRequest();

        void enter();
        void leave();
        void check();

        int count() const;
        bool is_active() const;

    private:
        PowerRequest power_req_;
        std::atomic<int> gate_count_ = 0;
        sung::RetriggerableMMV<sung::MonotonicRealtimeTimer> mmv_;
        sung::EdgeDetector edge_;
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
