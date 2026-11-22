#pragma once

// Per-frame snapshots that share instead of copy.
//
// Each frame a game fills one snapshot of its memory for the graphics-card
// renderer. The present queue, the F12 history and the world map's held view
// keep a pointer to it rather than a copy, and nothing changes a snapshot
// once it is handed out. take() gives a snapshot no one else holds: one whose
// last holder let go, with its buffers' memory kept, or a new one. Safe to
// release from any thread.

#include <memory>
#include <mutex>
#include <vector>

namespace gbarecomp {

template <class T>
class SnapshotPool {
    struct Free {
        std::mutex lock;
        std::vector<std::unique_ptr<T>> items;
    };
    // Shared with every handed-out snapshot, so one released after the pool
    // itself (at exit) still has somewhere to go.
    std::shared_ptr<Free> free_ = std::make_shared<Free>();

public:
    std::shared_ptr<T> take() {
        std::unique_ptr<T> item;
        {
            std::lock_guard<std::mutex> guard(free_->lock);
            if (!free_->items.empty()) {
                item = std::move(free_->items.back());
                free_->items.pop_back();
            }
        }
        if (!item) item = std::make_unique<T>();
        return std::shared_ptr<T>(item.release(), [free = free_](T* released) {
            std::lock_guard<std::mutex> guard(free->lock);
            free->items.emplace_back(released);
        });
    }
};

}  // namespace gbarecomp
