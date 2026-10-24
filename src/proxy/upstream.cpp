#include "proxy/upstream.h"

namespace sluice::proxy {

Upstream* RoundRobin::Pick(Clock::time_point now) {
  for (size_t tried = 0; tried < upstreams_.size(); ++tried) {
    Upstream* candidate = upstreams_[next_];
    next_ = (next_ + 1) % upstreams_.size();
    if (candidate->IsUp(now)) {
      return candidate;
    }
  }
  return nullptr;
}

}  // namespace sluice::proxy
