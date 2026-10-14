#include "ram_overlay_registry.h"

#include <algorithm>
#include <utility>

namespace gbarecomp {
namespace {

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

bool ranges_overlap(uint32_t a_start, uint32_t a_end,
                    uint32_t b_start, uint32_t b_end) {
    return a_start < b_end && b_start < a_end;
}

bool entry_less(const RamOverlayDispatchEntry& lhs,
                const RamOverlayDispatchEntry& rhs) {
    if (lhs.pc != rhs.pc) return lhs.pc < rhs.pc;
    return lhs.thumb < rhs.thumb;
}

}  // namespace

const RamOverlayRegistry::RegisteredImage* RamOverlayRegistry::find_image(
    std::string_view identity) const {
    auto it = std::find_if(images_.begin(), images_.end(),
        [identity](const RegisteredImage& image) {
            return image.identity == identity;
        });
    return it == images_.end() ? nullptr : &*it;
}

bool RamOverlayRegistry::register_image(const RamOverlayImageView& image,
                                        std::string* error) {
    if (error) error->clear();
    if (image.identity.empty()) {
        set_error(error, "overlay identity is empty");
        return false;
    }
    if (find_image(image.identity)) {
        set_error(error, "overlay identity is already registered");
        return false;
    }
    if (image.runtime_start >= image.runtime_end) {
        set_error(error, "overlay runtime range is empty or reversed");
        return false;
    }
    if (!image.entries || image.entry_count == 0) {
        set_error(error, "overlay has no dispatch entries");
        return false;
    }

    for (std::size_t i = 0; i < image.entry_count; ++i) {
        const RamOverlayDispatchEntry& entry = image.entries[i];
        if (entry.pc < image.runtime_start || entry.pc >= image.runtime_end) {
            set_error(error, "overlay entry is outside its runtime range");
            return false;
        }
        if (entry.thumb > 1u) {
            set_error(error, "overlay entry has an invalid instruction mode");
            return false;
        }
        if ((entry.pc & 1u) != 0u ||
            (entry.thumb == 0u && (entry.pc & 3u) != 0u)) {
            set_error(error, "overlay entry is not aligned for its mode");
            return false;
        }
        if (!entry.fn) {
            set_error(error, "overlay entry has a null native function");
            return false;
        }
        if (i > 0 && !entry_less(image.entries[i - 1], entry)) {
            set_error(error,
                      "overlay entries are not strictly sorted by (pc, mode)");
            return false;
        }
    }

    RegisteredImage registered;
    registered.identity.assign(image.identity);
    registered.runtime_start = image.runtime_start;
    registered.runtime_end = image.runtime_end;
    registered.entries.assign(image.entries,
                              image.entries + image.entry_count);
    images_.push_back(std::move(registered));
    return true;
}

bool RamOverlayRegistry::activate(std::string_view identity,
                                  std::string* error) {
    if (error) error->clear();
    const RegisteredImage* requested = find_image(identity);
    if (!requested) {
        set_error(error, "overlay identity is not registered");
        return false;
    }
    const uint32_t start = requested->runtime_start;
    const uint32_t end = requested->runtime_end;

    std::erase_if(active_identities_, [&](const std::string& active_identity) {
        const RegisteredImage* active = find_image(active_identity);
        return !active || active_identity == identity ||
               ranges_overlap(start, end,
                              active->runtime_start, active->runtime_end);
    });
    active_identities_.emplace_back(identity);
    return true;
}

std::size_t RamOverlayRegistry::invalidate_range(uint32_t runtime_start,
                                                 uint32_t runtime_end) {
    if (runtime_start >= runtime_end) return 0;
    const std::size_t before = active_identities_.size();
    std::erase_if(active_identities_, [&](const std::string& identity) {
        const RegisteredImage* image = find_image(identity);
        return !image || ranges_overlap(runtime_start, runtime_end,
                                        image->runtime_start,
                                        image->runtime_end);
    });
    return before - active_identities_.size();
}

RamOverlayFunction RamOverlayRegistry::resolve(uint32_t pc, bool thumb) const {
    for (const std::string& identity : active_identities_) {
        const RegisteredImage* image = find_image(identity);
        if (!image || pc < image->runtime_start || pc >= image->runtime_end) {
            continue;
        }
        RamOverlayDispatchEntry key{pc, static_cast<uint8_t>(thumb), nullptr};
        auto it = std::lower_bound(image->entries.begin(), image->entries.end(),
                                   key, entry_less);
        if (it != image->entries.end() && it->pc == pc &&
            (it->thumb != 0u) == thumb) {
            return it->fn;
        }
        return nullptr;
    }
    return nullptr;
}

bool RamOverlayRegistry::dispatch(uint32_t pc, bool thumb) const {
    if (RamOverlayFunction fn = resolve(pc, thumb)) {
        fn();
        return true;
    }
    return false;
}

bool RamOverlayRegistry::is_active(std::string_view identity) const {
    return std::find(active_identities_.begin(), active_identities_.end(),
                     identity) != active_identities_.end();
}

std::string RamOverlayRegistry::active_identity(uint32_t pc) const {
    for (const std::string& identity : active_identities_) {
        const RegisteredImage* image = find_image(identity);
        if (image && pc >= image->runtime_start && pc < image->runtime_end) {
            return identity;
        }
    }
    return {};
}

}  // namespace gbarecomp
