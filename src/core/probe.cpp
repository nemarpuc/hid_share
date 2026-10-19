// SPDX-License-Identifier: MIT
#include "hid_share/probe.hpp"

namespace hids {

void ProbeAxis::press(const int counts) {
    total_ += counts;
    last_ = counts;
    ++count_;
}

void ProbeAxis::clear() noexcept {
    total_ = 0;
    last_ = 0;
    count_ = 0;
}

int ProbeAxis::total() const noexcept { return total_; }

int ProbeAxis::last() const noexcept { return last_; }

int ProbeAxis::finish(const int fine) const noexcept {
    if (count_ < 2 || last_ != fine)
        return 0;
    return total_ - last_;
}

} // namespace hids
