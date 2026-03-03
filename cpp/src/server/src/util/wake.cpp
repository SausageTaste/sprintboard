#include "util/wake.hpp"

#include <optional>
#include <print>
#include <string_view>

#ifdef _WIN32
    #include <windows.h>
#elif defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <IOKit/pwr_mgt/IOPMLib.h>
#endif


namespace {

#ifdef __APPLE__

    std::optional<IOPMAssertionID> create_assertion(
        std::string_view reason, const bool display
    ) {
        if (reason.empty())
            reason = "Prevent sleep";

        const CFStringRef reason_cfs = CFStringCreateWithBytes(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(reason.data()),
            static_cast<CFIndex>(reason.size()),
            kCFStringEncodingUTF8,
            false
        );
        if (!reason_cfs)
            return std::nullopt;

        IOPMAssertionID assertion_id = 0;
        const IOReturn r = IOPMAssertionCreateWithName(
            display ? kIOPMAssertionTypePreventUserIdleDisplaySleep
                    : kIOPMAssertionTypePreventUserIdleSystemSleep,
            kIOPMAssertionLevelOn,
            reason_cfs,
            &assertion_id
        );

        CFRelease(reason_cfs);

        if (r == kIOReturnSuccess)
            return assertion_id;

        return std::nullopt;
    }

    bool release_assertion(const IOPMAssertionID assertion_id) {
        if (assertion_id == 0)
            return false;
        return kIOReturnSuccess == IOPMAssertionRelease(assertion_id);
    }


    class MacPowerRequestBackend : public sung::PowerRequestBackend {

    public:
        MacPowerRequestBackend(std::string_view reason) : reason_(reason) {}

        ~MacPowerRequestBackend() {
            if (system_assertion_)
                release_assertion(*system_assertion_);
            if (display_assertion_)
                release_assertion(*display_assertion_);
        }

        bool set_system() override {
            if (system_assertion_)
                return true;

            system_assertion_ = ::create_assertion(reason_, false);
            return system_assertion_.has_value();
        }

        bool set_display() override {
            if (display_assertion_)
                return true;

            display_assertion_ = ::create_assertion(reason_, true);
            return display_assertion_.has_value();
        }

        bool clear_system() override {
            if (!system_assertion_)
                return true;

            const bool r = release_assertion(*system_assertion_);
            system_assertion_.reset();
            return r;
        }

        bool clear_display() override {
            if (!display_assertion_)
                return true;

            const bool r = release_assertion(*display_assertion_);
            display_assertion_.reset();
            return r;
        }

    private:
        std::string reason_;
        std::optional<IOPMAssertionID> system_assertion_;
        std::optional<IOPMAssertionID> display_assertion_;
    };

#endif

}  // namespace


// PowerRequest
namespace sung {

    PowerRequest::PowerRequest(std::string_view reason) {
#ifdef _WIN32
        std::wstring reason_w(reason.begin(), reason.end());

        REASON_CONTEXT rc{};
        rc.Version = POWER_REQUEST_CONTEXT_VERSION;
        rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = reason_w.data();

        handle_ = PowerCreateRequest(&rc);

#elif defined(__APPLE__)
        backend_ = std::make_unique<MacPowerRequestBackend>(reason);
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
#elif defined(__APPLE__)
        return backend_ != nullptr;
#else
        return false;
#endif
    }

    bool PowerRequest::set_system_required() {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerSetRequest(handle_, PowerRequestSystemRequired))
            return false;

        system_required_ = true;
        return true;

#elif defined(__APPLE__)
        if (backend_)
            return backend_->set_system();

#endif
        return false;
    }

    bool PowerRequest::set_display_required() {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerSetRequest(handle_, PowerRequestDisplayRequired))
            return false;

        display_required_ = true;
        return true;

#elif defined(__APPLE__)
        if (backend_)
            return backend_->set_display();

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

#elif defined(__APPLE__)
        if (backend_)
            return backend_->clear_system();

#endif
        return false;
    }

    bool PowerRequest::clear_display_required() {
#ifdef _WIN32
        if (!this->ok())
            return false;
        if (TRUE != PowerClearRequest(handle_, PowerRequestDisplayRequired))
            return false;

        display_required_ = false;
        return true;
#elif defined(__APPLE__)
        if (backend_)
            return backend_->clear_display();

#endif
        return false;
    }

}  // namespace sung


// GatedPowerRequest
namespace sung {

    GatedPowerRequest::GatedPowerRequest()
        : power_req_("Sprintboard server: keep system awake while active") {}

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
            power_req_.set_system_required();
            // power_req_.set_display_required();
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
