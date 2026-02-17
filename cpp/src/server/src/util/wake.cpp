#include "util/wake.hpp"

#include <print>

#ifdef _WIN32
    #include <windows.h>
#endif


// PowerRequest
namespace sung {

    PowerRequest::PowerRequest(const wchar_t* reason) {
#ifdef _WIN32
        REASON_CONTEXT rc{};
        rc.Version = POWER_REQUEST_CONTEXT_VERSION;
        rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = const_cast<wchar_t*>(reason);

        handle_ = PowerCreateRequest(&rc);
#endif
    }

    PowerRequest::~PowerRequest() {
#ifdef _WIN32
        if (this->ok())
            CloseHandle(handle_);
#endif
    }

    bool PowerRequest::ok() const {
#ifdef _WIN32
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#endif
    }

    bool PowerRequest::set_system_required(bool on) {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerSetRequest(handle_, PowerRequestSystemRequired))
            return false;

        system_required_ = true;
        return true;
#endif
        return false;
    }

    bool PowerRequest::set_display_required(bool on) {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerSetRequest(handle_, PowerRequestDisplayRequired))
            return false;

        display_required_ = true;
        return true;
#endif
        return false;
    }

    bool PowerRequest::clear_system_required() {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerClearRequest(handle_, PowerRequestSystemRequired))
            return false;

        system_required_ = false;
        return true;
#else
        return false;
#endif
    }

    bool PowerRequest::clear_display_required() {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerClearRequest(handle_, PowerRequestDisplayRequired))
            return false;

        display_required_ = false;
        return true;
#else
        return false;
#endif
    }

}  // namespace sung


// GatedPowerRequest
namespace sung {

    GatedPowerRequest::GatedPowerRequest()
        : power_req_(L"Sprintboard server: keep system awake while active") {}

    void GatedPowerRequest::enter() {
        gate_count_ += 1;
        mmv_.notify_signal(0 < gate_count_.load());
    }

    void GatedPowerRequest::leave() {
        gate_count_ -= 1;

        if (gate_count_ < 0)
            gate_count_ = 0;

        mmv_.notify_signal(0 < gate_count_.load());
    }

    void GatedPowerRequest::check() {
        constexpr double tolerance_sec = 25 * 60;
        edge_.notify_signal(mmv_.poll_signal(tolerance_sec));

        const auto edge_type = edge_.check_edge();
        if (edge_type == sung::EdgeDetector::Type::rising) {
            power_req_.set_system_required(true);
            // power_req_.set_display_required(true);
            std::println("System wake requested");
        } else if (edge_type == sung::EdgeDetector::Type::falling) {
            power_req_.clear_system_required();
            // power_req_.clear_display_required();
            std::println("System wake released");
        }
    }

    int GatedPowerRequest::count() const { return gate_count_.load(); }

    bool GatedPowerRequest::is_active() const {
        return power_req_.is_system_required() ||
               power_req_.is_display_required();
    }

}  // namespace sung


namespace sung {

    double get_idle_time() {
#ifdef _WIN32
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);

        GetLastInputInfo(&lii);

        const auto ms = GetTickCount() - lii.dwTime;
        return ms / 1000.0;
#else
        return 0.0;
#endif
    }

}  // namespace sung
