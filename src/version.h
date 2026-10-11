#ifndef SLUICE_VERSION_H_
#define SLUICE_VERSION_H_

#include <string_view>

namespace sluice {

// Returns the gateway's current semantic version string.
std::string_view GetVersion();

}  // namespace sluice

#endif  // SLUICE_VERSION_H_
