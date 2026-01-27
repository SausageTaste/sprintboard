#include "util/wake.hpp"

#include <windows.h>


namespace sung {

    double get_idle_time() {
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);

        GetLastInputInfo(&lii);

        const auto ms = GetTickCount() - lii.dwTime;
        return ms / 1000.0;
    }

}  // namespace sung
