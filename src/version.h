#pragma once

#include <string_view>

namespace sluice {

// Returns the gateway's current semantic version string.
std::string_view GetVersion();

}  // namespace sluice
