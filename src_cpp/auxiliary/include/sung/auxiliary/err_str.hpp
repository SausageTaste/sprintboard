#pragma once

#include <expected>


namespace sung {

    using ErrStr = std::expected<void, std::string>;

}
