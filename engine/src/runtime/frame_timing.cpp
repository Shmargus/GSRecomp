#include "frame_timing.h"

#include <cmath>

namespace gbarecomp {

void FramePeriodStepper::set_target_hz(double target_hz) {
    if (!(target_hz > 0.0) || !std::isfinite(target_hz))
        target_hz = kGbaFrameHz;
    target_hz_ = target_hz;
    const long double exact_ns = 1000000000.0L /
        static_cast<long double>(target_hz_);
    whole_ns_ = static_cast<std::int64_t>(std::floor(exact_ns));
    fractional_ns_ = exact_ns - static_cast<long double>(whole_ns_);
    reset_phase();
}

std::int64_t FramePeriodStepper::next_period_ns() {
    std::int64_t period = whole_ns_;
    remainder_ns_ += fractional_ns_;
    if (remainder_ns_ >= 1.0L) {
        const auto carry = static_cast<std::int64_t>(remainder_ns_);
        period += carry;
        remainder_ns_ -= static_cast<long double>(carry);
    }
    return period;
}

}  // namespace gbarecomp
