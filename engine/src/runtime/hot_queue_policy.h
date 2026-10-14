// hot_queue_policy.h - bounded priority for repeatedly-hit async work.
#pragma once

#include <cstddef>

namespace gbarecomp {

inline constexpr unsigned kHotQueueBurstLimit = 3;

// Select a queue index without mutating the queue. Hot work may bypass cold
// FIFO work, but after three such selections the oldest item must run.
template <class Queue, class IsHot>
std::size_t hot_queue_pick_index(const Queue& queue, unsigned& hot_burst,
                                 IsHot is_hot) {
    if (queue.empty()) return static_cast<std::size_t>(-1);
    if (hot_burst < kHotQueueBurstLimit) {
        for (std::size_t i = 0; i < queue.size(); ++i) {
            if (is_hot(queue[i])) {
                ++hot_burst;
                return i;
            }
        }
    }
    hot_burst = 0;
    return 0;
}

}  // namespace gbarecomp
