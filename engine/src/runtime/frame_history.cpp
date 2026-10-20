#include "frame_history.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <utility>

#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "memory_snapshot.h"
#include "runtime_bus_bridge.h"

namespace gbarecomp {
namespace {

constexpr std::size_t kRows = gba::GbaPpu::kScreenHeight;
constexpr std::size_t kRowBytes = gba::GbaPpu::kLineIoBytes;

double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool write_file(const std::string& path, const void* data, std::size_t bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, bytes, f) == bytes;
    return std::fclose(f) == 0 && ok;
}

bool close_file(std::FILE* f) {
    if (!f) return false;
    const bool ok = std::ferror(f) == 0;
    return std::fclose(f) == 0 && ok;
}

}  // namespace

HistoryFrame& FrameHistory::record(const std::uint8_t* rgb, std::uint32_t width,
                                   std::uint32_t height) {
    return record(rgb, width, height, capture());
}

HistoryFrame FrameHistory::capture() {
    HistoryFrame fr;
    fr.guest_frame = runtime_current_frame();
    if (const gba::GbaPpu* ppu = active_ppu()) {
        const std::uint8_t* rows = ppu->latched_native_line_io();
        const std::int32_t* refs = ppu->latched_native_affine_line_refs();
        fr.rows.assign(rows, rows + kRows * kRowBytes);
        fr.refs.assign(refs, refs + kRows * 4);
    } else {
        fr.rows.clear();
        fr.refs.clear();
    }
    return fr;
}

void FrameHistory::clear() {
    count_ = next_ = 0;
    last_present_ms_ = -1.0;
    last_live_guest_ = 0;
}

HistoryFrame& FrameHistory::record(const std::uint8_t* rgb, std::uint32_t width,
                                   std::uint32_t height, HistoryFrame frame) {
    if (ring_.empty()) ring_.resize(kFrames);
    const auto guest = runtime_current_frame();
    if (count_ && guest < last_live_guest_) clear();
    last_live_guest_ = guest;
    const double now = now_ms();
    HistoryFrame& fr = ring_[next_];
    fr = std::move(frame);
    fr.host_ms = last_present_ms_ >= 0.0 ? now - last_present_ms_ : 0.0;
    fr.width = width;
    fr.height = height;
    fr.rgb.assign(rgb, rgb + static_cast<std::size_t>(width) * height * 3u);
    next_ = (next_ + 1) % kFrames;
    if (count_ < kFrames) ++count_;
    last_present_ms_ = now;
    return fr;
}

const HistoryFrame& FrameHistory::back(std::size_t i) const {
    return ring_[(next_ + kFrames - 1 - i) % kFrames];
}

const HistoryFrame& FrameHistory::oldest_first(std::size_t i) const {
    return ring_[(next_ + kFrames - count_ + i) % kFrames];
}

std::string FrameHistory::write(const std::string& note,
                                const char* note_file) const {
    std::error_code ec;
    std::string dir;
    for (unsigned n = 1; n <= 9999; ++n) {
        const std::string candidate =
            "logs/" + std::string(prefix_) + (n < 10 ? "000" : n < 100 ? "00" : n < 1000 ? "0" : "") +
            std::to_string(n);
        if (!std::filesystem::exists(candidate, ec)) {
            dir = candidate;
            break;
        }
    }
    if (dir.empty() || !std::filesystem::create_directories(dir, ec)) {
        std::fprintf(stderr, "[F12] could not create a logs/%s directory\n", prefix_);
        return {};
    }
    // Only on save: no extra collection, timers or copies in record().
    const auto trigger_frame = runtime_current_frame();
    const auto context = gsr_write_diagnostic_files(dir.c_str(), "capture");
    unsigned write_failures = (!context.context_written ? 1u : 0u) +
        (context.trail_available && !context.trail_written ? 1u : 0u);
    std::size_t scene_frames = 0, held_frames = 0, gpu_frames = 0;
    std::size_t largest_gap_order = 0;
    double interval_sum = 0.0, largest_gap = 0.0;
    std::vector<double> intervals;
    const bool have_memory = active_bus() != nullptr;
    std::FILE* index = std::fopen((dir + "/index.csv").c_str(), "wb");
    if (index)
        std::fprintf(index, "order,guest_frame,host_ms,battle_2x,file,scene_frame,"
                            "gpu_drawn,held_frame,has_scene,width,height,scene_file,image_written\n");
    for (std::size_t i = 0; i < count_; ++i) {
        const HistoryFrame& fr = oldest_first(i);
        scene_frames += !fr.scene.empty();
        held_frames += fr.held_frame;
        gpu_frames += fr.gpu_drawn;
        // The first interval starts outside the saved history.
        if (i) {
            intervals.push_back(fr.host_ms);
            interval_sum += fr.host_ms;
            if (fr.host_ms > largest_gap) {
                largest_gap = fr.host_ms;
                largest_gap_order = i;
            }
        }
        char name[32], scene_name[32];
        std::snprintf(name, sizeof name, "frame_%03zu.ppm", i);
        std::snprintf(scene_name, sizeof scene_name, "frame_%03zu.bin", i);
        bool image_written = false;
        if (std::FILE* f = std::fopen((dir + "/" + name).c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", fr.width, fr.height);
            std::fwrite(fr.rgb.data(), 1, fr.rgb.size(), f);
            image_written = close_file(f);
        }
        if (!image_written) ++write_failures;
        bool scene_written = false;
        if (!fr.scene.empty()) {
            scene_written = write_file(dir + "/" + scene_name, fr.scene.data(), fr.scene.size());
            if (!scene_written)
                std::fprintf(stderr, "[F12] could not write %s/%s\n", dir.c_str(), scene_name);
            if (!scene_written) ++write_failures;
        }
        if (index)
            std::fprintf(index, "%zu,%llu,%.2f,%d,%s,%llu,%d,%d,%d,%u,%u,%s,%d\n", i,
                         fr.guest_frame, fr.host_ms, fr.battle_2x ? 1 : 0, name,
                         static_cast<unsigned long long>(fr.scene_frame),
                         fr.gpu_drawn ? 1 : 0, fr.held_frame ? 1 : 0,
                         scene_written ? 1 : 0, fr.width, fr.height,
                         scene_written ? scene_name : "", image_written ? 1 : 0);
    }
    if (!close_file(index)) ++write_failures;
    if (gba::GbaBus* bus = active_bus()) {
        bool memory_written = false;
        if (std::FILE* f = std::fopen((dir + "/memory.bin").c_str(), "wb")) {
            std::fwrite(bus->ewram_ptr(), 1, 0x40000, f);
            std::fwrite(bus->iwram_ptr(), 1, 0x8000, f);
            std::fwrite(bus->io().raw(), 1, 0x400, f);
            std::fwrite(bus->pal_ptr(), 1, 0x400, f);
            std::fwrite(bus->vram_ptr(), 1, 0x18000, f);
            std::fwrite(bus->oam_ptr(), 1, 0x400, f);
            memory_written = close_file(f);
        }
        if (!memory_written) ++write_failures;
    }
    // The newest snapshot under its old single-frame name, the SAME bytes as
    // the newest history entry.
    if (count_ && !back(0).scene.empty() &&
        !write_file(dir + "/gpu_frame.bin", back(0).scene.data(), back(0).scene.size()))
        ++write_failures;
    if (std::FILE* f = std::fopen((dir + "/rows.csv").c_str(), "wb")) {
        std::fprintf(f, "order,row");
        for (std::size_t o = 0; o < kRowBytes; o += 2) std::fprintf(f, ",io%02zX", o);
        std::fprintf(f, ",bg2x,bg2y,bg3x,bg3y\n");
        for (std::size_t i = 0; i < count_; ++i) {
            const HistoryFrame& fr = oldest_first(i);
            if (fr.rows.size() != kRows * kRowBytes || fr.refs.size() != kRows * 4) continue;
            for (std::size_t y = 0; y < kRows; ++y) {
                const std::uint8_t* r = &fr.rows[y * kRowBytes];
                std::fprintf(f, "%zu,%zu", i, y);
                for (std::size_t o = 0; o < kRowBytes; o += 2)
                    std::fprintf(f, ",%04X", static_cast<unsigned>(r[o] | (r[o + 1] << 8)));
                const std::int32_t* ref = &fr.refs[y * 4];
                std::fprintf(f, ",%d,%d,%d,%d\n", ref[0], ref[1], ref[2], ref[3]);
            }
        }
        if (!close_file(f)) ++write_failures;
    } else ++write_failures;
    if (!note.empty() && !write_file(dir + "/" + note_file, note.data(), note.size()))
        ++write_failures;
    std::sort(intervals.begin(), intervals.end());
    double median = 0.0;
    if (!intervals.empty()) {
        const auto mid = intervals.size() / 2;
        median = intervals.size() % 2 ? intervals[mid] :
            (intervals[mid - 1] + intervals[mid]) / 2.0;
    }
    if (std::FILE* f = std::fopen((dir + "/capture_info.txt").c_str(), "wb")) {
        std::fprintf(f, "capture_info_version=1\nsource=%s\n"
                        "live_trigger_frame=%llu\n"
                        "memory_state=live_trigger_not_historical_picture\n"
                        "live_memory_available=%d\nframes=%zu\n"
                        "first_guest_frame=%llu\nlast_guest_frame=%llu\n"
                        "frames_with_scene=%zu\ngpu_frames=%zu\nheld_frames=%zu\n"
                        "between_saved_frames_ms=%.3f\nmedian_interval_ms=%.3f\n"
                        "largest_interval_ms=%.3f\nlargest_interval_order=%zu\n"
                        "timing=host_presentation_intervals_not_cpu_cost\n"
                        "context_written=%d\ntrail_available=%d\ntrail_written=%d\n"
                        "write_failures=%u\ncomplete=%d\n",
                     prefix_, trigger_frame, have_memory ? 1 : 0, count_,
                     count_ ? oldest_first(0).guest_frame : 0ull,
                     count_ ? back(0).guest_frame : 0ull,
                     scene_frames, gpu_frames, held_frames, interval_sum, median,
                     largest_gap, largest_gap_order, context.context_written ? 1 : 0,
                     context.trail_available ? 1 : 0, context.trail_written ? 1 : 0,
                     write_failures, write_failures ? 0 : 1);
        if (!close_file(f)) ++write_failures;
    } else ++write_failures;
    if (write_failures) {
        std::fprintf(stderr, "[F12] incomplete capture: %u file writes failed; evidence kept in %s\n",
                     write_failures, dir.c_str());
        return {};
    }
    std::fprintf(stderr, "[F12] %zu frames (guest frames %llu..%llu) written to %s\n",
                 count_, count_ ? oldest_first(0).guest_frame : 0ull,
                 count_ ? back(0).guest_frame : 0ull, dir.c_str());
    return dir;
}

bool spell_setup_torn_frame(const gba::GbaPpu& ppu) {
    constexpr std::size_t kRowBytes = gba::GbaPpu::kLineIoBytes;
    const uint8_t* rows = ppu.latched_native_line_io();
    const bool* valid = ppu.latched_native_line_io_valid();
    const int32_t* refs = ppu.latched_native_affine_line_refs();
    auto io16 = [&](std::size_t row, std::size_t offset) {
        const uint8_t* r = rows + row * kRowBytes + offset;
        return static_cast<uint16_t>(r[0] | (r[1] << 8));
    };
    if (!valid[0]) return false;
    const uint16_t dispcnt = io16(0, 0x00);
    if ((dispcnt & 7u) != 1u || (dispcnt & 0x0400u) == 0u) return false;  // mode 1, BG2
    if (refs[0] != 0 || refs[1] != 0) return false;                       // BG2 at (0, 0)
    for (std::size_t y = 1; y < gba::GbaPpu::kScreenHeight; ++y)
        if (valid[y] && io16(y, 0x0C) != io16(0, 0x0C)) return true;  // line table running
    return false;
}

}  // namespace gbarecomp
