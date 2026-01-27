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

    // Call clear when done
    bool PowerRequest::clear_system_required() {
        return this->ok() &&
               PowerClearRequest(handle_, PowerRequestSystemRequired) == TRUE;
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
