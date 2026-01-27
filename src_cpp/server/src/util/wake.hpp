#pragma once


namespace sung {

    class PowerRequest {

    public:
        PowerRequest(const wchar_t* reason);
        ~PowerRequest();

        bool ok() const;

        bool set_system_required(bool on);
        bool set_display_required(bool on);
        bool clear_system_required();

    private:
        void* handle_ = nullptr;
    };


    double get_idle_time();

}  // namespace sung
