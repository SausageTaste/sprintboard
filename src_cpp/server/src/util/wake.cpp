#include "util/wake.hpp"

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
        return this->ok() &&
               PowerSetRequest(handle_, PowerRequestSystemRequired) == TRUE;
    }

    bool PowerRequest::set_display_required(bool on) {
        return this->ok() &&
               PowerSetRequest(handle_, PowerRequestDisplayRequired) == TRUE;
    }

    bool PowerRequest::clear_system_required() {
        return this->ok() &&
               PowerClearRequest(handle_, PowerRequestSystemRequired) == TRUE;
    }

    bool PowerRequest::clear_display_required() {
        return this->ok() &&
               PowerClearRequest(handle_, PowerRequestDisplayRequired) == TRUE;
    }

}  // namespace sung


// GatedPowerRequest
namespace sung {

    GatedPowerRequest::GatedPowerRequest()
        : power_req_(L"Sprintboard server: keep system awake while active") {}

    void GatedPowerRequest::enter() {
        if (gate_count_.fetch_add(1) == 0) {
            power_req_.set_system_required(true);
            power_req_.set_display_required(true);
        }
    }

    void GatedPowerRequest::leave() {
        if (gate_count_.fetch_sub(1) == 1) {
            power_req_.clear_system_required();
            power_req_.clear_display_required();
        }

        if (gate_count_ < 0)
            gate_count_ = 0;
    }

    int GatedPowerRequest::count() const { return gate_count_.load(); }

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
