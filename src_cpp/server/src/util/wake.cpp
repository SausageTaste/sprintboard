#include "util/wake.hpp"

#include <print>

#include <windows.h>


// PowerRequest
namespace sung {

    PowerRequest::PowerRequest(const wchar_t* reason) {
        REASON_CONTEXT rc{};
        rc.Version = POWER_REQUEST_CONTEXT_VERSION;
        rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = const_cast<wchar_t*>(reason);

        handle_ = PowerCreateRequest(&rc);
    }

    PowerRequest::~PowerRequest() {
        if (this->ok())
            CloseHandle(handle_);
    }

    bool PowerRequest::ok() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    bool PowerRequest::set_system_required(bool on) {
        if (!this->ok())
            return false;
        if (TRUE != PowerSetRequest(handle_, PowerRequestSystemRequired))
            return false;

        system_required_ = true;
        return true;
    }

    bool PowerRequest::set_display_required(bool on) {
        if (!this->ok())
            return false;
        if (TRUE != PowerSetRequest(handle_, PowerRequestDisplayRequired))
            return false;

        display_required_ = true;
        return true;
    }

    bool PowerRequest::clear_system_required() {
        if (!this->ok())
            return false;
        if (TRUE != PowerClearRequest(handle_, PowerRequestSystemRequired))
            return false;

        system_required_ = false;
        return true;
    }

    bool PowerRequest::clear_display_required() {
        if (!this->ok())
            return false;
        if (TRUE != PowerClearRequest(handle_, PowerRequestDisplayRequired))
            return false;

        display_required_ = false;
        return true;
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
        constexpr double tolerance_sec = 15 * 60;
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
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);

        GetLastInputInfo(&lii);

        const auto ms = GetTickCount() - lii.dwTime;
        return ms / 1000.0;
    }

}  // namespace sung
