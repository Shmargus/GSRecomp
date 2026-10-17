#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace gbarecomp {

// Own the presentation state for exactly the copy behind the GPU fence.
// Surface is a GpuSurface (or a fault-injecting surface in the focused bench).
// invalidate() touches no GL: state-load hooks may run without a context.
template<class Frame>
class TextureReadbackQueue {
public:
    void invalidate() {
        pending_.reset();
        last_guest_.reset();
    }

    template<class Surface, class Texture>
    std::optional<Frame> present(Surface& surface, Texture texture, int width, int height,
                  std::uint8_t* rgb, std::uint64_t guest, Frame current,
                  bool synchronous = false) {
        // Repeated/enhanced presentations, resizes and clock jumps are
        // boundaries, never permission to show a stale queued picture.
        const bool boundary = synchronous || width != width_ || height != height_ ||
            (last_guest_ && guest <= *last_guest_);
        if (boundary || !pending_) {
            pending_.reset();
            surface.cancel_texture_readback();
        }
        width_ = width;
        height_ = height;
        last_guest_ = guest;

        std::optional<Frame> previous;
        if (pending_ && surface.finish_texture_readback(width, height, rgb))
            previous = std::move(pending_);
        pending_.reset();

        if (!previous && !surface.read_texture_rgb(texture, width, height, rgb)) {
            surface.cancel_texture_readback();
            return std::nullopt;
        }
        if (!synchronous && surface.begin_texture_readback(texture, width, height)) {
            if (previous) {
                pending_ = std::move(current);
                return previous;
            }
            // Warm-up/fence failure shows the current picture and queues
            // the same owned state; the next callback may repeat it once.
            pending_ = current;
        } else {
            surface.cancel_texture_readback();
            // A failed enqueue still leaves a successfully received older
            // copy usable, but only with its OWN state. Otherwise the caller
            // keeps its current synchronous picture. Never relabel old RGB.
            if (previous) return previous;
        }
        return current;
    }

private:
    std::optional<Frame> pending_;
    std::optional<std::uint64_t> last_guest_;
    int width_ = 0, height_ = 0;
};

}  // namespace gbarecomp
