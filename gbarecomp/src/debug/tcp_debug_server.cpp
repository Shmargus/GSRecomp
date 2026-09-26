// tcp_debug_server.cpp — see tcp_debug_server.h.

#include "tcp_debug_server.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define CLOSESOCK ::close
#endif

// Debug PC breakpoint global (defined in runtime_bus_bridge.cpp). The
// set_break_pc command writes it; runtime_should_yield() consults it.
extern "C" uint32_t g_runtime_break_pc;

#include "cpu_state.h"
#include "gba_audio.h"
#include "gba_bus.h"
#include "gba_m4a.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "symbol_lookup.h"

namespace gbarecomp::debug {

namespace {

// ─────────────────────────────────────────────────────────────────────
// JSON micro-helpers (same shape as oracle/main.cpp)
// ─────────────────────────────────────────────────────────────────────

void json_emit_hex(std::string& out, const uint8_t* data, std::size_t n) {
    out.reserve(out.size() + n * 2 + 2);
    out.push_back('"');
    static const char* H = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(H[data[i] >> 4]);
        out.push_back(H[data[i] & 0xF]);
    }
    out.push_back('"');
}

bool extract_uint(std::string_view req, std::string_view key, uint64_t& out) {
    auto pos = req.find(key);
    if (pos == std::string_view::npos) return false;
    pos = req.find(':', pos);
    if (pos == std::string_view::npos) return false;
    ++pos;
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '"')) ++pos;
    int base = 10;
    if (pos + 1 < req.size() && req[pos] == '0' &&
        (req[pos + 1] == 'x' || req[pos + 1] == 'X')) {
        base = 16;
        pos += 2;
    }
    uint64_t v = 0;
    bool any = false;
    while (pos < req.size()) {
        char c = req[pos];
        int digit;
        if (c >= '0' && c <= '9')                      digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')   digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')   digit = c - 'A' + 10;
        else break;
        v = v * base + static_cast<uint64_t>(digit);
        any = true;
        ++pos;
    }
    if (!any) return false;
    out = v;
    return true;
}

// Extract a JSON string value for `key` into `out`. Handles the common
// escapes a path can carry on the wire (\\ and \"); other escapes pass
// through verbatim. Returns false if the key/string is absent.
bool extract_string(std::string_view req, std::string_view key,
                    std::string& out) {
    auto pos = req.find(key);
    if (pos == std::string_view::npos) return false;
    pos = req.find(':', pos);
    if (pos == std::string_view::npos) return false;
    ++pos;
    while (pos < req.size() && req[pos] == ' ') ++pos;
    if (pos >= req.size() || req[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < req.size()) {
        char c = req[pos++];
        if (c == '\\' && pos < req.size()) {
            char e = req[pos++];
            out.push_back(e);  // \\ -> \, \" -> ", others -> literal
            continue;
        }
        if (c == '"') return true;
        out.push_back(c);
    }
    return false;  // unterminated string
}

void emit_error(std::string& out, const char* msg) {
    out = "{\"ok\":false,\"error\":\"";
    out += msg;
    out += "\"}";
}

void emit_ok_int(std::string& out, const char* key, uint64_t v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"%s\":%llu}", key,
                  static_cast<unsigned long long>(v));
    out = buf;
}

uint64_t frame_counter(const TcpDebugServer::Context& ctx) {
    if (ctx.sync_frames) return *ctx.sync_frames;
    return ctx.ppu ? ctx.ppu->frame_count() : 0u;
}

// ─────────────────────────────────────────────────────────────────────
// Region readers — pull bytes directly from the bus's backing arrays
// rather than going through bus.read8(), which would mask side effects
// like IF write-1-to-clear.
// ─────────────────────────────────────────────────────────────────────

void cmd_read_region(const uint8_t* base, std::size_t size,
                     std::string_view req, std::string& out,
                     uint32_t bus_base_addr) {
    uint64_t addr = 0, len = 0;
    if (!extract_uint(req, "\"addr\"", addr) ||
        !extract_uint(req, "\"len\"", len)) {
        emit_error(out, "missing addr/len");
        return;
    }
    uint64_t off = (addr >= bus_base_addr) ? addr - bus_base_addr : addr;
    if (off > size || len > size || off + len > size) {
        emit_error(out, "out of range");
        return;
    }
    out  = "{\"ok\":true,\"base\":";
    char ab[32];
    std::snprintf(ab, sizeof(ab), "%u",
                  static_cast<unsigned>(bus_base_addr + off));
    out += ab;
    out += ",\"len\":";
    char lb[32];
    std::snprintf(lb, sizeof(lb), "%llu",
                  static_cast<unsigned long long>(len));
    out += lb;
    out += ",\"data\":";
    json_emit_hex(out, base + off, static_cast<std::size_t>(len));
    out += "}";
}

void cmd_read_io_dynamic(gba::GbaBus& bus, std::string_view req,
                         std::string& out) {
    uint64_t addr = 0, len = 0;
    if (!extract_uint(req, "\"addr\"", addr) ||
        !extract_uint(req, "\"len\"", len)) {
        emit_error(out, "missing addr/len");
        return;
    }
    uint64_t off = (addr >= 0x04000000u) ? addr - 0x04000000u : addr;
    if (off > gba::GbaIo::kIoSize || len > gba::GbaIo::kIoSize ||
        off + len > gba::GbaIo::kIoSize) {
        emit_error(out, "out of range");
        return;
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        bytes[static_cast<std::size_t>(i)] =
            bus.io().read8(static_cast<uint32_t>(off + i));
    }
    out  = "{\"ok\":true,\"base\":";
    char ab[32];
    std::snprintf(ab, sizeof(ab), "%u",
                  static_cast<unsigned>(0x04000000u + off));
    out += ab;
    out += ",\"len\":";
    char lb[32];
    std::snprintf(lb, sizeof(lb), "%llu",
                  static_cast<unsigned long long>(len));
    out += lb;
    out += ",\"data\":";
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

void cmd_audio_samples(gba::GbaBus& bus, std::string_view req,
                       std::string& out) {
    uint64_t max_samples = 4096;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"samples\"", parsed)) {
        max_samples = parsed;
    }
    if (max_samples > 16384) max_samples = 16384;

    std::vector<int16_t> samples(static_cast<std::size_t>(max_samples));
    std::size_t n = bus.audio().drain_samples(samples.data(), samples.size());
    std::vector<uint8_t> bytes(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        uint16_t u = static_cast<uint16_t>(samples[i]);
        bytes[i * 2 + 0] = static_cast<uint8_t>(u & 0xFFu);
        bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
    }

    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"count\":%llu,"
                  "\"samples_generated\":%llu,\"data\":",
                  static_cast<unsigned>(bus.audio().sample_rate()),
                  static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(
                      bus.audio().samples_generated()));
    out = hdr;
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

// Always-on, non-destructive capture-ring query. Unlike audio_samples (which
// drains the playback FIFO), this reads a [start,count] window of the capture
// ring backward from the live head without consuming anything — the probe
// queries history, it does not arm/drain (ring-buffer discipline). Params:
//   "count"  : samples to return (default 8192, cap 65536)
//   "start"  : absolute sample index (samples_generated numbering). If absent,
//              returns the most recent `count` samples (start = head - count).
// Emits the mixed mono stream + raw per-channel arrays as little-endian int16
// hex so the drift comparator can isolate PSG channels for the bit-check.
void cmd_audio_cap(gba::GbaBus& bus, std::string_view req, std::string& out) {
    const auto& audio = bus.audio();
    uint64_t count = 8192;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        count = parsed;
    }
    if (count > gba::GbaAudio::kCapRingSize) count = gba::GbaAudio::kCapRingSize;

    uint64_t head = audio.samples_generated();
    uint64_t oldest = audio.capture_oldest_index();
    uint64_t start;
    if (extract_uint(req, "\"start\"", parsed)) {
        start = parsed;
    } else {
        start = (head > count) ? head - count : oldest;
    }

    std::vector<gba::GbaAudio::CapSample> buf(static_cast<std::size_t>(count));
    uint64_t first = 0;
    std::size_t n = audio.query_capture(start, buf.size(), buf.data(), first);

    auto emit_i16 = [&](const char* key, auto pick) {
        std::vector<uint8_t> bytes(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            uint16_t u = static_cast<uint16_t>(pick(buf[i]));
            bytes[i * 2 + 0] = static_cast<uint8_t>(u & 0xFFu);
            bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
        }
        out += ",\"";
        out += key;
        out += "\":";
        json_emit_hex(out, bytes.data(), bytes.size());
    };

    char hdr[224];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"count\":%llu,\"first\":%llu,"
                  "\"head\":%llu,\"oldest\":%llu",
                  static_cast<unsigned>(audio.sample_rate()),
                  static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(head),
                  static_cast<unsigned long long>(oldest));
    out = hdr;
    emit_i16("mixed",    [](const gba::GbaAudio::CapSample& s){ return s.mixed; });
    emit_i16("ch1",      [](const gba::GbaAudio::CapSample& s){ return s.ch[0]; });
    emit_i16("ch2",      [](const gba::GbaAudio::CapSample& s){ return s.ch[1]; });
    emit_i16("ch3",      [](const gba::GbaAudio::CapSample& s){ return s.ch[2]; });
    emit_i16("ch4",      [](const gba::GbaAudio::CapSample& s){ return s.ch[3]; });
    emit_i16("direct_a", [](const gba::GbaAudio::CapSample& s){ return s.direct_a; });
    emit_i16("direct_b", [](const gba::GbaAudio::CapSample& s){ return s.direct_b; });
    out += "}";
}

void append_fifo_state(std::string& out, const char* name,
                       const gba::GbaAudio::FifoDebugState& fifo) {
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  ",\"%s\":{\"write\":%u,\"read\":%u,\"count\":%u,"
                  "\"shift_word\":%u,\"bytes_remaining\":%u,\"samples\":[",
                  name,
                  static_cast<unsigned>(fifo.write),
                  static_cast<unsigned>(fifo.read),
                  static_cast<unsigned>(fifo.count),
                  static_cast<unsigned>(fifo.shift_word),
                  static_cast<unsigned>(fifo.bytes_remaining));
    out += hdr;
    for (uint32_t i = 0; i < gba::GbaAudio::kMaxSamplesPerEvent; ++i) {
        if (i) out += ",";
        char b[16];
        std::snprintf(b, sizeof(b), "%d", static_cast<int>(fifo.samples[i]));
        out += b;
    }
    out += "]}";
}

void append_audio_timeline_stats(std::string& out, const char* name,
                                 const gba::AudioTimelineStats& s) {
    char b[640];
    std::snprintf(
        b, sizeof(b),
        ",\"%s\":{\"published\":%llu,\"releases\":%llu,"
        "\"unmatched\":%llu,\"wraps\":%llu,\"overwrites\":%llu,"
        "\"sequence_gaps\":%llu,\"resets\":%llu,\"pushes\":%llu,"
        "\"underruns\":%llu,\"last_sequence\":%llu,"
        "\"last_release_sequence\":%llu,\"last_host_cursor\":%llu,"
        "\"phase_q32\":%llu,\"queue\":%u,\"queue_max\":%u}",
        name, static_cast<unsigned long long>(s.published),
        static_cast<unsigned long long>(s.releases),
        static_cast<unsigned long long>(s.unmatched),
        static_cast<unsigned long long>(s.wraps),
        static_cast<unsigned long long>(s.overwrites),
        static_cast<unsigned long long>(s.sequence_gaps),
        static_cast<unsigned long long>(s.resampler_resets),
        static_cast<unsigned long long>(s.resampler_pushes),
        static_cast<unsigned long long>(s.resampler_underruns),
        static_cast<unsigned long long>(s.last_sequence),
        static_cast<unsigned long long>(s.last_release_sequence),
        static_cast<unsigned long long>(s.last_host_cursor),
        static_cast<unsigned long long>(s.last_phase_q32),
        static_cast<unsigned>(s.dma_queue ? s.dma_queue : s.resampler_queue),
        static_cast<unsigned>(s.dma_queue_max ? s.dma_queue_max :
                              s.resampler_queue_max));
    out += b;
}

void append_audio_sample_stats(std::string& out,
                               const gba::VerifierSampleStats& s) {
    char b[1000];
    std::snprintf(
        b, sizeof(b),
        ",\"verifier_samples\":{\"canonical_zero\":%llu,"
        "\"canonical_held\":%llu,\"canonical_repeated\":%llu,"
        "\"native_zero\":%llu,\"native_held\":%llu,"
        "\"native_repeated\":%llu}",
        static_cast<unsigned long long>(s.canonical_zero),
        static_cast<unsigned long long>(s.canonical_held),
        static_cast<unsigned long long>(s.canonical_repeated),
        static_cast<unsigned long long>(s.native_zero),
        static_cast<unsigned long long>(s.native_held),
        static_cast<unsigned long long>(s.native_repeated));
    out += b;
}

void append_audio_diff_trace(std::string& out,
                             const gba::ProducerDiffTrace& t) {
    char b[1100];
    std::snprintf(
        b, sizeof(b),
        ",\"producer_diff\":{\"valid\":%s,\"sequence\":%llu,"
        "\"seed_sequence\":%llu,\"cursor\":%llu,"
        "\"route_a_addr\":%u,\"route_b_addr\":%u,"
        "\"index\":%u,\"route\":%u,\"guest\":%d,"
        "\"native_quantized\":%d,\"guest_dry\":[%d,%d],"
        "\"native_dry\":[%d,%d],\"native_post\":[%d,%d],"
        "\"guest_raw\":[%d,%d],\"guest_seed\":%u,\"native_seed\":%u,"
        "\"native_accumulator\":%u,"
        "\"reverb_start\":[%u,%u],\"old_addr\":%u,"
        "\"guest_old\":[%u,%u],\"native_old\":[%u,%u],\"voices\":[",
        t.valid ? "true" : "false",
        static_cast<unsigned long long>(t.sequence),
        static_cast<unsigned long long>(t.seed_sequence),
        static_cast<unsigned long long>(t.cursor),
        static_cast<unsigned>(t.route_a_addr),
        static_cast<unsigned>(t.route_b_addr),
        static_cast<unsigned>(t.index), static_cast<unsigned>(t.route),
        static_cast<int>(t.guest), static_cast<int>(t.native_quantized),
        static_cast<int>(t.guest_dry_a), static_cast<int>(t.guest_dry_b),
        static_cast<int>(t.native_dry_a), static_cast<int>(t.native_dry_b),
        static_cast<int>(t.native_post_a), static_cast<int>(t.native_post_b),
        static_cast<int>(t.guest_raw_a), static_cast<int>(t.guest_raw_b),
        static_cast<unsigned>(t.guest_seed),
        static_cast<unsigned>(t.native_seed),
        static_cast<unsigned>(t.native_accumulator),
        static_cast<unsigned>(t.reverb_start_a),
        static_cast<unsigned>(t.reverb_start_b),
        static_cast<unsigned>(t.old_route_addr),
        static_cast<unsigned>(t.guest_old_a),
        static_cast<unsigned>(t.guest_old_b),
        static_cast<unsigned>(t.native_old_a),
        static_cast<unsigned>(t.native_old_b));
    out += b;
    for (std::size_t i = 0; i < t.voices.size(); ++i) {
        if (i) out += ",";
        const auto& v = t.voices[i];
        char voice[400];
        std::snprintf(
            voice, sizeof(voice),
            "{\"active\":%s,\"channel\":%u,\"ctype\":%u,\"wave\":%u,"
            "\"resolved_wave\":%u,\"cursor\":%u,\"frac\":%u,"
            "\"step\":%u,\"sample\":%d,"
            "\"gain_right_q9\":%u,\"gain_left_q9\":%u,"
            "\"packed_gain\":%u,\"before\":%u,\"after\":%u}",
            v.active ? "true" : "false", static_cast<unsigned>(v.channel),
            static_cast<unsigned>(v.ctype), static_cast<unsigned>(v.wave),
            static_cast<unsigned>(v.resolved_wave),
            static_cast<unsigned>(v.cursor),
            static_cast<unsigned>(v.frac), static_cast<unsigned>(v.step),
            static_cast<int>(v.sample),
            static_cast<unsigned>(v.gain_right_q9),
            static_cast<unsigned>(v.gain_left_q9),
            static_cast<unsigned>(v.packed_gain),
            static_cast<unsigned>(v.accumulator_before),
            static_cast<unsigned>(v.accumulator_after));
        out += voice;
    }
    out += "]}";
}

void append_audio_canonical_mix(std::string& out,
                                const gba::CanonicalMixTrace& t) {
    char b[320];
    std::snprintf(b, sizeof(b),
        ",\"canonical_mix\":{\"valid\":%s,\"sequence\":%llu,"
        "\"base_addr\":%u,\"count\":%u,\"pre_cursor\":%llu,"
        "\"completion_cursor\":%llu,\"completion_cycles\":%llu,"
        "\"pre_hook_cycles\":%llu,\"first_write_cycles\":%llu,"
        "\"last_write_cycles\":%llu,"
        "\"sound_info\":%u,\"pre_snapshot_generation\":%u,"
        "\"pre_active_voices\":%u",
        t.valid ? "true" : "false",
        static_cast<unsigned long long>(t.sequence),
        static_cast<unsigned>(t.base_addr), static_cast<unsigned>(t.count),
        static_cast<unsigned long long>(t.pre_cursor),
        static_cast<unsigned long long>(t.completion_cursor),
        static_cast<unsigned long long>(t.completion_cycles),
        static_cast<unsigned long long>(t.pre_hook_cycles),
        static_cast<unsigned long long>(t.first_write_cycles),
        static_cast<unsigned long long>(t.last_write_cycles),
        static_cast<unsigned>(t.sound_info),
        static_cast<unsigned>(t.pre_snapshot_generation),
        static_cast<unsigned>(t.pre_active_voices));
    out += b;
    out += ",\"pre_hook_pc\":" +
        std::to_string(static_cast<unsigned>(t.pre_hook_pc));
    out += ",\"pre_hook_source\":" +
        std::to_string(static_cast<unsigned>(t.pre_hook_source));
    out += ",\"pre_channel_status\":[";
    for (std::size_t i = 0; i < t.pre_channel_status.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(static_cast<unsigned>(t.pre_channel_status[i]));
    }
    out += "],\"write_channel_status\":[";
    for (std::size_t i = 0; i < t.write_channel_status.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(static_cast<unsigned>(t.write_channel_status[i]));
    }
    out += "],\"steps\":[";
    for (std::size_t i = 0; i < t.steps.size(); ++i) {
        if (i) out += ",";
        char step[280];
        const auto& s = t.steps[i];
        std::snprintf(step, sizeof(step),
            "{\"pc\":%u,\"mode\":%u,\"cycle\":%llu,\"addr\":%u,"
            "\"before\":%u,\"after\":%u,"
            "\"operand_sample\":%d,\"operand_gain\":%u,"
            "\"actual_before\":%u}",
            static_cast<unsigned>(s.pc), static_cast<unsigned>(s.mode),
            static_cast<unsigned long long>(s.cycle),
            static_cast<unsigned>(s.addr),
            static_cast<unsigned>(s.before), static_cast<unsigned>(s.after),
            static_cast<int>(s.operand_sample),
            static_cast<unsigned>(s.operand_gain),
            static_cast<unsigned>(s.actual_before));
        out += step;
    }
    out += "],\"channel_write_total\":" +
        std::to_string(static_cast<unsigned>(t.channel_write_total));
    out += ",\"voice_identities\":[";
    for (std::size_t i = 0; i < t.voice_identities.size(); ++i) {
        if (i) out += ",";
        const auto& v = t.voice_identities[i];
        char identity[300];
        std::snprintf(identity, sizeof(identity),
            "{\"channel\":%u,\"status\":%u,\"ctype\":%u,"
            "\"channel_base\":%u,\"cursor\":%u,\"frac\":%u,\"wave\":%u,"
            "\"sample_cursor\":%u,\"count\":%u,\"frequency\":%u,"
            "\"gain_right\":%u,\"gain_left\":%u,"
            "\"wave_flags\":%u,\"wave_loop\":%u,\"wave_size\":%u,"
            "\"looped\":%u}",
            static_cast<unsigned>(v.channel), static_cast<unsigned>(v.status),
            static_cast<unsigned>(v.ctype), static_cast<unsigned>(v.channel_base),
            static_cast<unsigned>(v.cursor),
            static_cast<unsigned>(v.frac), static_cast<unsigned>(v.wave),
            static_cast<unsigned>(v.sample_cursor),
            static_cast<unsigned>(v.count), static_cast<unsigned>(v.frequency),
            static_cast<unsigned>(v.gain_right),
            static_cast<unsigned>(v.gain_left),
            static_cast<unsigned>(v.wave_flags), static_cast<unsigned>(v.wave_loop),
            static_cast<unsigned>(v.wave_size), static_cast<unsigned>(v.looped));
        out += identity;
    }
    out += "]";
    out += ",\"channel_writes\":[";
    const std::size_t count = t.channel_writes.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (i) out += ",";
        const auto& w = t.channel_writes[i];
        char entry[220];
        std::snprintf(entry, sizeof(entry),
            "{\"pc\":%u,\"mode\":%u,\"cycle\":%llu,\"addr\":%u,"
            "\"channel\":%u,\"field\":%u,\"before\":%u,\"value\":%u,\"width\":%u}",
            static_cast<unsigned>(w.pc), static_cast<unsigned>(w.mode),
            static_cast<unsigned long long>(w.cycle),
            static_cast<unsigned>(w.addr), static_cast<unsigned>(w.channel),
            static_cast<unsigned>(w.field), static_cast<unsigned>(w.before),
            static_cast<unsigned>(w.value), static_cast<unsigned>(w.width));
        out += entry;
    }
    out += "]}";
}

void append_audio_canonical_boundaries(
        std::string& out, const gba::GbaAudio& audio) {
    const auto& ring = audio.shadow_canonical_boundaries();
    const uint32_t count = audio.shadow_canonical_boundary_count();
    const uint32_t write = audio.shadow_canonical_boundary_write();
    out += ",\"canonical_boundaries\":[";
    for (uint32_t n = 0; n < count; ++n) {
        if (n) out += ",";
        const uint32_t pos = (count == ring.size())
            ? (write + n) %
                static_cast<uint32_t>(ring.size())
            : n;
        const auto& b = ring[pos];
        char item[900];
        std::snprintf(item, sizeof(item),
            "{\"sequence\":%llu,\"last_channel_cycle\":%llu,"
            "\"last_channel_pc\":%u,\"last_channel_addr\":%u,"
            "\"last_channel\":%u,\"last_field\":%u,"
            "\"last_before\":%u,\"last_value\":%u,"
            "\"control_cycle\":%llu,\"control_pc\":%u,"
            "\"control_target\":%u,\"control_kind\":%u,"
            "\"control_mode\":%u,\"first_write_cycle\":%llu,"
            "\"first_write_pc\":%u,\"first_write_addr\":%u,"
            "\"first_write_mode\":%u,\"channel_status\":[",
            static_cast<unsigned long long>(b.sequence),
            static_cast<unsigned long long>(b.last_channel_cycle),
            static_cast<unsigned>(b.last_channel_pc),
            static_cast<unsigned>(b.last_channel_addr),
            static_cast<unsigned>(b.last_channel),
            static_cast<unsigned>(b.last_field),
            static_cast<unsigned>(b.last_channel_before),
            static_cast<unsigned>(b.last_channel_value),
            static_cast<unsigned long long>(b.control_cycle),
            static_cast<unsigned>(b.control_pc),
            static_cast<unsigned>(b.control_target),
            static_cast<unsigned>(b.control_kind),
            static_cast<unsigned>(b.control_mode),
            static_cast<unsigned long long>(b.first_write_cycle),
            static_cast<unsigned>(b.first_write_pc),
            static_cast<unsigned>(b.first_write_addr),
            static_cast<unsigned>(b.first_write_mode));
        out += item;
        for (std::size_t ch = 0; ch < b.channel_status.size(); ++ch) {
            if (ch) out += ",";
            out += std::to_string(static_cast<unsigned>(b.channel_status[ch]));
        }
        out += "],\"voice_identities\":[";
        for (std::size_t ch = 0; ch < b.voice_identities.size(); ++ch) {
            if (ch) out += ",";
            const auto& v = b.voice_identities[ch];
            char voice[220];
            std::snprintf(voice, sizeof(voice),
                "{\"channel\":%u,\"status\":%u,\"ctype\":%u,"
                "\"wave\":%u,\"count\":%u,\"frac\":%u,"
                "\"sample_cursor\":%u,\"gain_right\":%u,"
                "\"gain_left\":%u}",
                static_cast<unsigned>(v.channel),
                static_cast<unsigned>(v.status),
                static_cast<unsigned>(v.ctype), static_cast<unsigned>(v.wave),
                static_cast<unsigned>(v.count), static_cast<unsigned>(v.frac),
                static_cast<unsigned>(v.sample_cursor),
                static_cast<unsigned>(v.gain_right),
                static_cast<unsigned>(v.gain_left));
            out += voice;
        }
        out += "]}";
    }
    out += "]";
}

void append_audio_bad_waves(std::string& out, const gba::GbaAudio& audio) {
    const auto& traces = audio.shadow_bad_wave_traces();
    const uint32_t count = audio.shadow_bad_wave_trace_count();
    out += ",\"bad_waves\":[";
    for (uint32_t i = 0; i < count; ++i) {
        if (i) out += ",";
        const auto& t = traces[i];
        char item[1800];
        std::snprintf(item, sizeof(item),
            "{\"sequence\":%llu,\"boundary_cycle\":%llu,"
            "\"channel\":%u,\"reason\":%u,\"status\":%u,"
            "\"ctype\":%u,\"channel_base\":%u,\"wave\":%u,"
            "\"resolved_wave\":%u,\"resolved_data\":%u,"
            "\"wave_flags\":%u,\"wave_loop\":%u,\"wave_size\":%u,"
            "\"count\":%u,\"frac\":%u,\"cp\":%u,"
            "\"sample_cursor\":%u,\"data_base\":%u,\"data_size\":%u,"
            "\"cp_in_range\":%u,\"status_write\":{\"pc\":%u,\"cycle\":%llu,\"addr\":%u,\"before\":%u,\"value\":%u,\"width\":%u,\"mode\":%u},"
            "\"type_write\":{\"pc\":%u,\"cycle\":%llu,\"addr\":%u,\"before\":%u,\"value\":%u,\"width\":%u,\"mode\":%u},"
            "\"wave_write\":{\"pc\":%u,\"cycle\":%llu,\"addr\":%u,\"before\":%u,\"value\":%u,\"width\":%u,\"mode\":%u}}",
            static_cast<unsigned long long>(t.sequence),
            static_cast<unsigned long long>(t.boundary_cycle),
            static_cast<unsigned>(t.channel), static_cast<unsigned>(t.reason),
            static_cast<unsigned>(t.status), static_cast<unsigned>(t.ctype),
            static_cast<unsigned>(t.channel_base), static_cast<unsigned>(t.wave),
            static_cast<unsigned>(t.resolved_wave), static_cast<unsigned>(t.resolved_data),
            static_cast<unsigned>(t.wave_flags), static_cast<unsigned>(t.wave_loop),
            static_cast<unsigned>(t.wave_size), static_cast<unsigned>(t.count),
            static_cast<unsigned>(t.frac), static_cast<unsigned>(t.cp),
            static_cast<unsigned>(t.sample_cursor), static_cast<unsigned>(t.data_base),
            static_cast<unsigned>(t.data_size), static_cast<unsigned>(t.cp_in_range),
            static_cast<unsigned>(t.status_write.pc), static_cast<unsigned long long>(t.status_write.cycle), static_cast<unsigned>(t.status_write.addr), static_cast<unsigned>(t.status_write.before), static_cast<unsigned>(t.status_write.value), static_cast<unsigned>(t.status_write.width), static_cast<unsigned>(t.status_write.mode),
            static_cast<unsigned>(t.type_write.pc), static_cast<unsigned long long>(t.type_write.cycle), static_cast<unsigned>(t.type_write.addr), static_cast<unsigned>(t.type_write.before), static_cast<unsigned>(t.type_write.value), static_cast<unsigned>(t.type_write.width), static_cast<unsigned>(t.type_write.mode),
            static_cast<unsigned>(t.wave_write.pc), static_cast<unsigned long long>(t.wave_write.cycle), static_cast<unsigned>(t.wave_write.addr), static_cast<unsigned>(t.wave_write.before), static_cast<unsigned>(t.wave_write.value), static_cast<unsigned>(t.wave_write.width), static_cast<unsigned>(t.wave_write.mode));
        std::string record(item);
        if (!record.empty() && record.back() == '}') record.pop_back();
        record += ",\"identity_writes\":{";
        auto append_write = [&](const char* name,
                                const gba::BadWaveTrace::Write& w) {
            if (record.back() != '{') record += ',';
            record += '\"'; record += name; record += "\":{";
            record += "\"pc\":"; record += std::to_string(w.pc);
            record += ",\"cycle\":"; record += std::to_string(w.cycle);
            record += ",\"addr\":"; record += std::to_string(w.addr);
            record += ",\"before\":"; record += std::to_string(w.before);
            record += ",\"value\":"; record += std::to_string(w.value);
            record += ",\"width\":"; record += std::to_string(w.width);
            record += ",\"mode\":"; record += std::to_string(w.mode);
            record += '}';
        };
        append_write("count", t.count_write);
        append_write("frac", t.frac_write);
        append_write("frequency", t.frequency_write);
        append_write("gain_right", t.gain_right_write);
        append_write("gain_left", t.gain_left_write);
        append_write("cp", t.cp_write);
        record += "}}";
        out += record;
    }
    out += "]";
}

void cmd_audio_state(gba::GbaBus& bus, std::string& out) {
    const auto& audio = bus.audio();
    char hdr[1600];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"samples_generated\":%llu,"
                  "\"soundbias\":%u,\"cycles_per_sample\":%u,"
                  "\"samples_per_event\":%u,\"cycle_accumulator\":%u,"
                  "\"cycles_until_event\":%u,\"shadow_enabled\":%s,"
                  "\"native_enabled\":%s,\"shadow_engaged\":%s,"
                  "\"shadow_hooks\":%llu,\"shadow_stale\":%llu,"
                  "\"shadow_bad_waves\":%llu,\"producer_underruns\":%llu,"
                  "\"producer_blocks_judged\":%llu,"
                  "\"producer_blocks_rejected\":%llu,"
                  "\"producer_blocks_incomplete\":%llu,"
                  "\"producer_startup_aligned\":%s,"
                  "\"first_incomplete_block\":%llu,"
                  "\"first_rejected_block\":%llu,"
                  "\"first_incomplete_cursor\":%llu,"
                  "\"first_incomplete_guest_samples\":%u,"
                  "\"first_incomplete_native_samples\":%u,"
                  "\"first_incomplete_expected_samples\":%u,"
                  "\"first_incomplete_route_a\":%u,"
                  "\"first_incomplete_route_b\":%u,"
                  "\"first_rejected_cursor\":%llu,"
                  "\"first_rejected_route_a\":%u,"
                  "\"first_rejected_route_b\":%u,"
                  "\"first_rejected_corr\":%.3f,"
                  "\"first_rejected_ratio\":%.3f,"
                  "\"first_rejected_mae\":%.3f,"
                  "\"shadow_corr\":%.3f,"
                  "\"shadow_ratio\":%.3f",
                  static_cast<unsigned>(audio.sample_rate()),
                  static_cast<unsigned long long>(audio.samples_generated()),
                  static_cast<unsigned>(audio.debug_soundbias()),
                  static_cast<unsigned>(
                      gba::GbaAudio::kSystemHz / audio.sample_rate()),
                  static_cast<unsigned>(audio.debug_samples_per_event()),
                  static_cast<unsigned>(audio.debug_cycle_accumulator()),
                  static_cast<unsigned>(audio.debug_cycles_until_event()),
                  audio.shadow_enabled() ? "true" : "false",
                  audio.native_audio_enabled() ? "true" : "false",
                  audio.shadow_engaged() ? "true" : "false",
                  static_cast<unsigned long long>(audio.shadow_hooks()),
                  static_cast<unsigned long long>(audio.shadow_stale_ticks()),
                  static_cast<unsigned long long>(audio.shadow_bad_waves()),
                  static_cast<unsigned long long>(
                      audio.shadow_producer_underruns()),
                  static_cast<unsigned long long>(
                      audio.shadow_producer_blocks_judged()),
                  static_cast<unsigned long long>(
                      audio.shadow_producer_blocks_rejected()),
                  static_cast<unsigned long long>(
                      audio.shadow_producer_blocks_incomplete()),
                  audio.shadow_producer_startup_aligned() ? "true" : "false",
                  static_cast<unsigned long long>(
                      audio.shadow_first_incomplete_block()),
                  static_cast<unsigned long long>(
                      audio.shadow_first_rejected_block()),
                  static_cast<unsigned long long>(
                      audio.shadow_first_incomplete_cursor()),
                  static_cast<unsigned>(
                      audio.shadow_first_incomplete_guest_samples()),
                  static_cast<unsigned>(
                      audio.shadow_first_incomplete_native_samples()),
                  static_cast<unsigned>(
                      audio.shadow_first_incomplete_expected_samples()),
                  static_cast<unsigned>(audio.shadow_first_incomplete_route_a()),
                  static_cast<unsigned>(audio.shadow_first_incomplete_route_b()),
                  static_cast<unsigned long long>(
                      audio.shadow_first_rejected_cursor()),
                  static_cast<unsigned>(audio.shadow_first_rejected_route_a()),
                  static_cast<unsigned>(audio.shadow_first_rejected_route_b()),
                  static_cast<double>(audio.shadow_first_rejected_correlation()),
                  static_cast<double>(audio.shadow_first_rejected_level_ratio()),
                  static_cast<double>(audio.shadow_first_rejected_mean_abs_error()),
                  static_cast<double>(audio.shadow_correlation()),
                  static_cast<double>(audio.shadow_level_ratio()));
    out = hdr;
    append_audio_timeline_stats(out, "producer_dma",
                                audio.shadow_dma_stats());
    append_audio_timeline_stats(out, "producer_host",
                                audio.shadow_host_stats());
    append_audio_sample_stats(out, audio.shadow_sample_stats());
    append_audio_diff_trace(out, audio.shadow_producer_diff_trace());
    append_audio_canonical_mix(out, audio.shadow_canonical_mix_trace());
    append_audio_canonical_boundaries(out, audio);
    append_audio_bad_waves(out, audio);
    append_fifo_state(out, "fifo_a", audio.debug_fifo_state(0));
    append_fifo_state(out, "fifo_b", audio.debug_fifo_state(1));
    out += "}";
}

void cmd_audio_trace(gba::GbaBus& bus, std::string_view req,
                     std::string& out) {
    uint64_t max_entries = 128;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed)) {
        max_entries = parsed;
    }
    uint32_t available = bus.audio().debug_trace_count();
    if (max_entries > available) max_entries = available;
    if (max_entries > gba::GbaAudio::kFifoTraceSize) {
        max_entries = gba::GbaAudio::kFifoTraceSize;
    }
    uint32_t start = available - static_cast<uint32_t>(max_entries);
    out = "{\"ok\":true,\"entries\":[";
    for (uint32_t i = 0; i < max_entries; ++i) {
        auto tr = bus.audio().debug_trace_entry(start + i);
        if (i) out += ",";
        char item[240];
        std::snprintf(item, sizeof(item),
            "{\"base\":%llu,\"fifo\":%u,\"until\":%u,\"start\":%u,"
            "\"slots\":%u,\"count\":%u,\"remaining\":%u,\"sample\":%d}",
            static_cast<unsigned long long>(tr.sample_base),
            static_cast<unsigned>(tr.fifo_id),
            static_cast<unsigned>(tr.until_cycles),
            static_cast<unsigned>(tr.start_slot),
            static_cast<unsigned>(tr.slots),
            static_cast<unsigned>(tr.count),
            static_cast<unsigned>(tr.bytes_remaining),
            static_cast<int>(tr.sample));
        out += item;
    }
    out += "]}";
}

void cmd_runtime_trace(const TcpDebugServer::Context& ctx,
                       std::string_view req, std::string& out) {
    if (!ctx.runtime_trace_copy) {
        emit_error(out, "runtime trace unavailable");
        return;
    }

    uint64_t max_entries = 128;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed)) {
        max_entries = parsed;
    }
    if (max_entries > 4096) max_entries = 4096;  // full g_trace ring depth

    std::vector<RuntimeTraceEntry> entries(
        static_cast<std::size_t>(max_entries));
    uint32_t n = ctx.runtime_trace_copy(
        entries.data(), static_cast<uint32_t>(entries.size()));

    char hdr[80];
    std::snprintf(hdr, sizeof(hdr), "{\"ok\":true,\"count\":%u,\"entries\":[",
                  static_cast<unsigned>(n));
    out = hdr;
    for (uint32_t i = 0; i < n; ++i) {
        const RuntimeTraceEntry& e = entries[i];
        if (i) out += ",";
        char item[560];
        std::snprintf(
            item, sizeof(item),
            "{\"seq\":%u,\"cycles\":%llu,\"kind\":%u,\"pc\":%u,\"cpsr\":%u,"
            "\"addr\":%u,\"value\":%u,\"aux\":%u,"
            "\"r0\":%u,\"r1\":%u,\"r2\":%u,\"r3\":%u,"
            "\"r4\":%u,\"r5\":%u,\"r12\":%u,\"r13\":%u,\"r14\":%u}",
            static_cast<unsigned>(e.seq),
            static_cast<unsigned long long>(e.cycles),
            static_cast<unsigned>(e.kind),
            static_cast<unsigned>(e.pc),
            static_cast<unsigned>(e.cpsr),
            static_cast<unsigned>(e.addr),
            static_cast<unsigned>(e.value),
            static_cast<unsigned>(e.aux),
            static_cast<unsigned>(e.r0),
            static_cast<unsigned>(e.r1),
            static_cast<unsigned>(e.r2),
            static_cast<unsigned>(e.r3),
            static_cast<unsigned>(e.r4),
            static_cast<unsigned>(e.r5),
            static_cast<unsigned>(e.r12),
            static_cast<unsigned>(e.r13),
            static_cast<unsigned>(e.r14));
        out += item;
    }
    out += "]}";
}

// ── Cycle-anchor sampler (Axis 2) ──────────────────────────────────────
// {"cmd":"cyc_anchor","pc":P,"hits":H} -> {ok,pc,armed,fp_count,count,cyc:[...]}.
// Filters the always-on insn-fingerprint ring by guest PC and returns the
// cumulative g_runtime_cycles stamp of each execution. Consecutive-hit Δ is the
// offset-cancelled cycle ruler peered against the NBA oracle's cyc_anchor.
// armed=0 means GBARECOMP_INSN_TRACE is off (ring empty) — the recomp build with
// the ring armed is where this returns live data; the bios_smoke interpreter
// mirrors into its own local ring and leaves this (recomp) ring empty.
void cmd_cyc_anchor(std::string_view req, std::string& out) {
    uint64_t pc = 0;
    if (!extract_uint(req, "\"pc\"", pc)) { emit_error(out, "missing pc"); return; }
    uint64_t hits = 256, parsed = 0;
    if (extract_uint(req, "\"hits\"", parsed) ||
        extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        hits = parsed;
    }
    if (hits > 65536) hits = 65536;
    std::vector<unsigned long long> cyc(static_cast<std::size_t>(hits));
    uint32_t n = runtime_fp_query_pc(static_cast<uint32_t>(pc),
                                     static_cast<uint32_t>(hits), cyc.data());
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"pc\":%llu,\"armed\":%u,\"fp_count\":%u,"
                  "\"count\":%u,\"cyc\":[",
                  static_cast<unsigned long long>(pc),
                  g_runtime_insn_trace ? 1u : 0u,
                  runtime_fp_count(), n);
    out = hdr;
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ',';
        char b[24];
        std::snprintf(b, sizeof(b), "%llu", cyc[i]);
        out += b;
    }
    out += "]}";
}

// ── IRQ raise/take ring query (Axis 3) ─────────────────────────────────
// {"cmd":"irq_cap","count":C} -> {ok,total,count,entries:[{cycle,src,ret,cpsr,
// from_halt}...]}. Dumps the most recent N IRQ vectorings (TAKE-time) from the
// always-on IRQ-vector ring. `src` is the active IE&IF source mask at the
// vector; `from_halt` distinguishes the wake-from-HALT path. NOTE: raise-time
// (the IF-set instant) is not separately recorded — take-time only (burndown
// Axis-3 gap). Populated by runtime_irq in the recompiled runtime.
void cmd_irq_cap(std::string_view req, std::string& out) {
    uint64_t count = 256, parsed = 0;
    if (extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        count = parsed;
    }
    if (count > 65536) count = 65536;
    std::vector<RuntimeIrqLogEntry> buf(static_cast<std::size_t>(count));
    uint32_t n = runtime_irq_log_copy_recent(buf.data(),
                                             static_cast<uint32_t>(count));
    char hdr[96];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"total\":%u,\"count\":%u,\"entries\":[",
                  runtime_irq_log_count(), n);
    out = hdr;
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ',';
        char item[200];
        std::snprintf(item, sizeof(item),
                      "{\"cycle\":%llu,\"src\":%u,\"ret\":%u,\"cpsr\":%u,"
                      "\"from_halt\":%u}",
                      buf[i].cycles, buf[i].src, buf[i].ret, buf[i].cpsr,
                      buf[i].from_halt);
        out += item;
    }
    out += "]}";
}

// ── MMIO write-trace ring query (Axis 4) ───────────────────────────────
// {"cmd":"mmio_cap","count":C,"start":S?} -> {ok,total,oldest,first,count,
// entries:[{cycle,addr,value,size,pc}...]}. Non-destructive window query of the
// always-on IO write-trace ring (gba_io.cpp). Default returns the most recent
// `count` writes; `start` requests an absolute index window.
void cmd_mmio_cap(std::string_view req, std::string& out) {
    uint64_t count = 4096, parsed = 0;
    if (extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        count = parsed;
    }
    if (count > 65536) count = 65536;
    uint64_t total  = gba::gba_mmio_cap_total();
    uint64_t oldest = gba::gba_mmio_cap_oldest();
    uint64_t start;
    if (extract_uint(req, "\"start\"", parsed)) {
        start = parsed;
    } else {
        start = (total > count) ? total - count : oldest;
    }
    std::vector<gba::MmioCapEntry> buf(static_cast<std::size_t>(count));
    uint64_t first = 0;
    std::size_t n = gba::gba_mmio_cap_query(start, buf.size(), buf.data(), first);
    char hdr[176];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"first\":%llu,"
                  "\"count\":%llu,\"entries\":[",
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(oldest),
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(n));
    out = hdr;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out += ',';
        char item[224];
        std::snprintf(item, sizeof(item),
                      "{\"cycle\":%llu,\"addr\":%llu,\"value\":%llu,\"size\":%u,"
                      "\"pc\":%llu}",
                      static_cast<unsigned long long>(buf[i].cycle),
                      static_cast<unsigned long long>(buf[i].addr),
                      static_cast<unsigned long long>(buf[i].value),
                      buf[i].size,
                      static_cast<unsigned long long>(buf[i].pc));
        out += item;
    }
    out += "]}";
}

// ── Determinism hook (Axis 7) ──────────────────────────────────────────
// {"cmd":"state_hash"} -> {ok,cycles,iwram,ewram,vram,pal,oam,hash}. A cheap
// read-only FNV-1a-64 over IWRAM+EWRAM+VRAM+PAL+OAM plus g_runtime_cycles, so a
// run-twice determinism probe can compare end-state in one call. Per-region
// hashes localize a divergence.
void cmd_state_hash(const TcpDebugServer::Context& ctx, std::string& out) {
    if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
    auto fnv = [](const uint8_t* p, std::size_t n) -> uint64_t {
        uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    };
    uint64_t iw = fnv(ctx.bus->iwram_ptr(), 32 * 1024);
    uint64_t ew = fnv(ctx.bus->ewram_ptr(), 256 * 1024);
    uint64_t vr = fnv(ctx.bus->vram_ptr(), 96 * 1024);
    uint64_t pa = fnv(ctx.bus->pal_ptr(), 1024);
    uint64_t oa = fnv(ctx.bus->oam_ptr(), 1024);
    unsigned long long cyc = g_runtime_cycles;
    uint64_t all = 1469598103934665603ull;
    uint64_t parts[6] = { iw, ew, vr, pa, oa, static_cast<uint64_t>(cyc) };
    for (int k = 0; k < 6; ++k) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&parts[k]);
        for (int j = 0; j < 8; ++j) { all ^= b[j]; all *= 1099511628211ull; }
    }
    char buf[400];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"cycles\":%llu,\"iwram\":\"%016llx\","
                  "\"ewram\":\"%016llx\",\"vram\":\"%016llx\",\"pal\":\"%016llx\","
                  "\"oam\":\"%016llx\",\"hash\":\"%016llx\"}",
                  cyc, static_cast<unsigned long long>(iw),
                  static_cast<unsigned long long>(ew),
                  static_cast<unsigned long long>(vr),
                  static_cast<unsigned long long>(pa),
                  static_cast<unsigned long long>(oa),
                  static_cast<unsigned long long>(all));
    out = buf;
}

void dispatch(const TcpDebugServer::Context& ctx, std::string_view req,
              std::string& out, bool& want_quit, bool& step_failed) {
    out.clear();

    auto contains = [&](const char* tok) {
        return req.find(tok) != std::string_view::npos;
    };

    if (contains("\"ping\"") || req == "ping") {
        out = "{\"ok\":true,\"who\":\"gbarecomp_native\"}";
        return;
    }
    // Batched deterministic frame stepping. This mirrors the independent
    // emulator-oracle control surface: KEYINPUT is written once, then held
    // while real guest code and devices advance for N VBlank intervals.
    // It is test orchestration only--no guest state or timing is injected.
    if (contains("\"run_frames\"")) {
        if (!ctx.step) {
            emit_error(out, "step callback not wired");
            return;
        }
        uint64_t count = 0;
        if (!extract_uint(req, "\"n\"", count)) {
            emit_error(out, "missing n");
            return;
        }
        constexpr uint64_t kMaxBatchFrames = 1'000'000ull;
        if (count > kMaxBatchFrames) {
            emit_error(out, "n too large");
            return;
        }
        uint64_t keyinput = 0;
        if (extract_uint(req, "\"keyinput\"", keyinput)) {
            if (!ctx.bus) {
                emit_error(out, "bus unavailable");
                return;
            }
            ctx.bus->io().set_keyinput(
                static_cast<uint16_t>(keyinput & 0x03FFu));
        }
        uint64_t completed = 0;
        for (; completed < count; ++completed) {
            if (!ctx.step()) {
                step_failed = true;
                break;
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":%s,\"frames\":%llu,\"frame\":%llu}",
                      completed == count ? "true" : "false",
                      static_cast<unsigned long long>(completed),
                      static_cast<unsigned long long>(frame_counter(ctx)));
        out = buf;
        return;
    }
    if (contains("\"frame\"") && !contains("\"emu_")) {
        emit_ok_int(out, "frame", frame_counter(ctx));
        return;
    }
    if (contains("\"step_inst\"")) {
        if (!ctx.step_inst) {
            emit_error(out, "step_inst callback not wired");
            return;
        }
        bool ok = ctx.step_inst();
        if (!ok) step_failed = true;
        // Return PC + CPSR + R0..R14 so the lockstep harness can
        // catch register-level divergence at its true origin (not
        // wait for the cascade into a branch decision).
        uint32_t pc = ctx.recomp_cpu ? ctx.recomp_cpu->R[15]
                                      : (ctx.cpu ? ctx.cpu->R[15] : 0u);
        uint64_t f  = frame_counter(ctx);
        std::string body;
        body.reserve(512);
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
                      "{\"ok\":%s,\"pc\":%u,\"frame\":%llu,"
                      "\"cycles\":%u,\"cycles_elapsed\":%llu",
                      ok ? "true" : "false",
                      static_cast<unsigned>(pc),
                      static_cast<unsigned long long>(f),
                      static_cast<unsigned>(ctx.last_step_cycles ?
                          *ctx.last_step_cycles : 0u),
                      static_cast<unsigned long long>(ctx.cycles_elapsed ?
                          *ctx.cycles_elapsed : 0u));
        body = hdr;
        if (ctx.recomp_cpu) {
            for (int i = 0; i < 15; ++i) {
                char f[48];
                std::snprintf(f, sizeof(f), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.recomp_cpu->R[i]));
                body += f;
            }
            char cpsr_field[48];
            std::snprintf(cpsr_field, sizeof(cpsr_field), ",\"cpsr\":%u",
                          static_cast<unsigned>(ctx.recomp_cpu->cpsr));
            body += cpsr_field;
        } else if (ctx.cpu) {
            for (int i = 0; i < 15; ++i) {
                char f[48];
                std::snprintf(f, sizeof(f), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.cpu->R[i]));
                body += f;
            }
            uint32_t cpsr = 0;
            if (ctx.cpu->cpsr.n) cpsr |= 1u << 31;
            if (ctx.cpu->cpsr.z) cpsr |= 1u << 30;
            if (ctx.cpu->cpsr.c) cpsr |= 1u << 29;
            if (ctx.cpu->cpsr.v) cpsr |= 1u << 28;
            if (ctx.cpu->cpsr.i) cpsr |= 1u << 7;
            if (ctx.cpu->cpsr.f) cpsr |= 1u << 6;
            if (ctx.cpu->cpsr.t) cpsr |= 1u << 5;
            cpsr |= ctx.cpu->cpsr.mode & 0x1Fu;
            char cpsr_field[48];
            std::snprintf(cpsr_field, sizeof(cpsr_field), ",\"cpsr\":%u",
                          static_cast<unsigned>(cpsr));
            body += cpsr_field;
        }
        body += "}";
        out = body;
        return;
    }
    if (contains("\"step\"") || contains("\"step_to_vblank\"")) {
        if (!ctx.step) {
            emit_error(out, "step callback not wired");
            return;
        }
        bool ok = ctx.step();
        if (!ok) step_failed = true;
        uint64_t f = frame_counter(ctx);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":%s,\"frame\":%llu}",
                      ok ? "true" : "false",
                      static_cast<unsigned long long>(f));
        out = buf;
        return;
    }
    // Free-run control. `continue`/`pause` are NON-BLOCKING: they set the game
    // thread's run-state and return immediately, so the server (this thread)
    // stays free to answer observation commands while the game free-runs — even
    // while it is wedged in a busy-spin freeze (observe-the-hung-core). Place
    // these BEFORE the generic readers so the short tokens match first.
    if (contains("\"continue\"") || contains("\"resume\"")) {
        if (ctx.resume) ctx.resume();
        out = "{\"ok\":true,\"run\":\"running\"}";
        return;
    }
    if (contains("\"pause\"")) {
        if (ctx.pause) ctx.pause();
        out = ctx.run_status ? ctx.run_status()
                             : std::string("{\"ok\":true,\"run\":\"paused\"}");
        return;
    }
    if (contains("\"run_status\"")) {
        out = ctx.run_status ? ctx.run_status()
                             : std::string("{\"ok\":false,\"error\":\"no run_status\"}");
        return;
    }
    if (contains("\"read_oam\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->oam_ptr(), 1024, req, out, 0x07000000u);
        return;
    }
    if (contains("\"read_pal\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->pal_ptr(), 1024, req, out, 0x05000000u);
        return;
    }
    if (contains("\"read_vram\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->vram_ptr(), 96 * 1024, req, out, 0x06000000u);
        return;
    }
    if (contains("\"read_iwram\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->iwram_ptr(), 32 * 1024, req, out, 0x03000000u);
        return;
    }
    if (contains("\"read_ewram\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->ewram_ptr(), 256 * 1024, req, out, 0x02000000u);
        return;
    }
    if (contains("\"read_io\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_io_dynamic(*ctx.bus, req, out);
        return;
    }
    if (contains("\"m4a_dump\"")) {
        // Observability (MC-HP-002): read the live MP2K SoundInfo +
        // 12-channel array out of guest RAM and flag any voice whose
        // wave/data pointer left a sane region. Pure read; no exec path.
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        gba::mp2k_dump_live(*ctx.bus, out);
        return;
    }
    if (contains("\"m4a_detect\"")) {
        // Scan the loaded ROM image for the SDK MP2K driver (SoundMain
        // signature → SoundMainRAM hook address). One-shot; static.
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        auto sigs = gba::mp2k_detect(ctx.bus->rom_ptr(), ctx.bus->rom_size());
        out = "{\"ok\":true,\"sigs\":[";
        for (std::size_t i = 0; i < sigs.size(); ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%s{\"sound_main_off\":%llu,\"sound_main_ram\":%u}",
                          i == 0 ? "" : ",",
                          static_cast<unsigned long long>(sigs[i].sound_main_off),
                          static_cast<unsigned>(sigs[i].sound_main_ram));
            out += buf;
        }
        out += "]}";
        return;
    }
    if (contains("\"set_keyinput\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        uint64_t value = 0x03FFu;
        if (!extract_uint(req, "\"value\"", value)) {
            emit_error(out, "missing value");
            return;
        }
        ctx.bus->io().set_keyinput(static_cast<uint16_t>(value & 0x03FFu));
        out = "{\"ok\":true}";
        return;
    }
    if (contains("\"set_break_pc\"")) {
        // Debug PC breakpoint (MC-HP-002): runtime_should_yield() unwinds
        // the current runtime_dispatch when the guest PC reaches this
        // value, so the spin inside a single dispatch can be inspected.
        uint64_t value = 0;
        if (!extract_uint(req, "\"value\"", value)) {
            emit_error(out, "missing value");
            return;
        }
        g_runtime_break_pc = static_cast<uint32_t>(value);
        out = "{\"ok\":true}";
        return;
    }
    if (contains("\"symbol\"")) {
        // Resolve a guest PC to the nearest recompiled function name +
        // offset, e.g. {"name":"UpdateAnimationVariableFrames","offset":16}.
        // Backed by the generated address->name map (symbol_lookup.h).
        uint64_t addr = 0;
        if (!extract_uint(req, "\"addr\"", addr)) {
            emit_error(out, "missing addr");
            return;
        }
        uint32_t off = 0;
        const char* name = gba_symbol_lookup(static_cast<uint32_t>(addr), &off);
        char buf[256];
        if (name) {
            std::snprintf(buf, sizeof(buf),
                          "{\"ok\":true,\"addr\":%llu,\"name\":\"%s\","
                          "\"offset\":%u}",
                          static_cast<unsigned long long>(addr), name, off);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "{\"ok\":true,\"addr\":%llu,\"name\":null,"
                          "\"offset\":0}",
                          static_cast<unsigned long long>(addr));
        }
        out = buf;
        return;
    }
    if (contains("\"ppu_state\"")) {
        if (!ctx.bus || !ctx.ppu) { emit_error(out, "ppu unavailable"); return; }
        auto io16 = [&](uint32_t off) -> uint16_t {
            return ctx.bus->io().read16(off);
        };
        auto io32 = [&](uint32_t off) -> uint32_t {
            uint32_t lo = io16(off);
            uint32_t hi = io16(off + 2);
            return lo | (hi << 16);
        };
        auto append_kv = [](std::string& s, const char* key, uint64_t v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ",\"%s\":%llu",
                          key, static_cast<unsigned long long>(v));
            s += buf;
        };
        std::string body = "{\"ok\":true";
        append_kv(body, "frame", ctx.ppu->frame_count());
        append_kv(body, "dispcnt",  io16(0x00));
        append_kv(body, "dispstat", io16(0x04));
        append_kv(body, "vcount",   io16(0x06));
        for (int bg = 0; bg < 4; ++bg) {
            char k[24];
            std::snprintf(k, sizeof(k), "bg%dcnt", bg);
            append_kv(body, k, io16(0x08 + bg * 2));
            std::snprintf(k, sizeof(k), "bg%dhofs", bg);
            append_kv(body, k, io16(0x10 + bg * 4));
            std::snprintf(k, sizeof(k), "bg%dvofs", bg);
            append_kv(body, k, io16(0x12 + bg * 4));
        }
        append_kv(body, "bg2pa", io16(0x20)); append_kv(body, "bg2pb", io16(0x22));
        append_kv(body, "bg2pc", io16(0x24)); append_kv(body, "bg2pd", io16(0x26));
        append_kv(body, "bg2x",  io32(0x28)); append_kv(body, "bg2y",  io32(0x2C));
        append_kv(body, "bg3pa", io16(0x30)); append_kv(body, "bg3pb", io16(0x32));
        append_kv(body, "bg3pc", io16(0x34)); append_kv(body, "bg3pd", io16(0x36));
        append_kv(body, "bg3x",  io32(0x38)); append_kv(body, "bg3y",  io32(0x3C));
        append_kv(body, "win0h", io16(0x40)); append_kv(body, "win1h", io16(0x42));
        append_kv(body, "win0v", io16(0x44)); append_kv(body, "win1v", io16(0x46));
        append_kv(body, "winin", io16(0x48)); append_kv(body, "winout",io16(0x4A));
        append_kv(body, "mosaic",  io16(0x4C));
        append_kv(body, "bldcnt",  io16(0x50));
        append_kv(body, "bldalpha",io16(0x52));
        append_kv(body, "bldy",    io16(0x54));
        body += "}";
        out = body;
        return;
    }
    if (contains("\"screenshot\"")) {
        if (!ctx.bus || !ctx.ppu) { emit_error(out, "ppu unavailable"); return; }
        const uint8_t* rgb = nullptr;
        std::vector<uint8_t> live;
        if (ctx.ppu->has_latched_framebuffer()) {
            rgb = ctx.ppu->latched_framebuffer();
        } else {
            live.assign(ctx.ppu->render_bytes(), 0);
            ctx.ppu->render(live.data(),
                            ctx.bus->io().read16(0x000),
                            ctx.bus->io().raw(),
                            ctx.bus->vram_ptr(),
                            ctx.bus->oam_ptr(),
                            ctx.bus->pal_ptr());
            rgb = live.data();
        }
        // Dimensions track the active view (240x160 faithful, wider when
        // view-area expansion is on) so oracle tooling sees the real frame.
        out = "{\"ok\":true,\"w\":" + std::to_string(ctx.ppu->render_width()) +
              ",\"h\":" + std::to_string(ctx.ppu->render_height()) + ",\"data\":";
        json_emit_hex(out, rgb, ctx.ppu->render_bytes());
        out += "}";
        return;
    }
    if (contains("\"audio_samples\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_samples(*ctx.bus, req, out);
        return;
    }
    if (contains("\"audio_cap\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_cap(*ctx.bus, req, out);
        return;
    }
    if (contains("\"audio_state\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_state(*ctx.bus, out);
        return;
    }
    if (contains("\"audio_trace\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_trace(*ctx.bus, req, out);
        return;
    }
    if (contains("\"runtime_trace\"")) {
        cmd_runtime_trace(ctx, req, out);
        return;
    }
    if (contains("\"cyc_anchor\"")) {
        cmd_cyc_anchor(req, out);
        return;
    }
    if (contains("\"irq_cap\"")) {
        cmd_irq_cap(req, out);
        return;
    }
    if (contains("\"mmio_cap\"")) {
        cmd_mmio_cap(req, out);
        return;
    }
    if (contains("\"state_hash\"")) {
        cmd_state_hash(ctx, out);
        return;
    }
    if (contains("\"registers\"")) {
        if (!ctx.cpu && !ctx.recomp_cpu) {
            emit_error(out, "cpu unavailable");
            return;
        }
        std::string body = "{\"ok\":true";
        uint32_t cpsr = 0;
        if (ctx.recomp_cpu) {
            for (int i = 0; i < 16; ++i) {
                char field[64];
                std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.recomp_cpu->R[i]));
                body += field;
            }
            cpsr = ctx.recomp_cpu->cpsr;
        } else {
            for (int i = 0; i < 16; ++i) {
                char field[64];
                std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.cpu->R[i]));
                body += field;
            }
            // Pack CPSR the same way enter_irq does.
            if (ctx.cpu->cpsr.n) cpsr |= 1u << 31;
            if (ctx.cpu->cpsr.z) cpsr |= 1u << 30;
            if (ctx.cpu->cpsr.c) cpsr |= 1u << 29;
            if (ctx.cpu->cpsr.v) cpsr |= 1u << 28;
            if (ctx.cpu->cpsr.i) cpsr |= 1u << 7;
            if (ctx.cpu->cpsr.f) cpsr |= 1u << 6;
            if (ctx.cpu->cpsr.t) cpsr |= 1u << 5;
            cpsr |= ctx.cpu->cpsr.mode & 0x1Fu;
        }
        char tail[64];
        std::snprintf(tail, sizeof(tail), ",\"cpsr\":%u}",
                      static_cast<unsigned>(cpsr));
        body += tail;
        out = body;
        return;
    }
    if (contains("\"counters\"")) {
        auto val = [&](uint64_t* p) -> uint64_t { return p ? *p : 0u; };
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"steps\":%llu,\"irq_entries\":%llu,"
            "\"swi_entries\":%llu,\"halt_steps\":%llu,"
            "\"vblank_irqs_raised\":%llu,\"cycles_elapsed\":%llu,"
            "\"last_step_cycles\":%u,\"sync_frames\":%llu}",
            static_cast<unsigned long long>(val(ctx.steps)),
            static_cast<unsigned long long>(val(ctx.irq_entries)),
            static_cast<unsigned long long>(val(ctx.swi_entries)),
            static_cast<unsigned long long>(val(ctx.halt_steps)),
            static_cast<unsigned long long>(val(ctx.vblank_irqs_raised)),
            static_cast<unsigned long long>(val(ctx.cycles_elapsed)),
            static_cast<unsigned>(ctx.last_step_cycles ?
                *ctx.last_step_cycles : 0u),
            static_cast<unsigned long long>(val(ctx.sync_frames)));
        out = buf;
        return;
    }
    if (contains("\"misses\"")) {
        // Live self-heal coverage: the always-on miss/heal bookkeeping queried
        // for the window of interest (no arm-then-run). Reports every PC the
        // interpreter bridged this session, which have healed to native, and
        // the native-call counts, so a TCP-driven repro can confirm a PC flips
        // bridge -> native (interp count freezes, native_calls climbs).
        if (!ctx.misses_query) {
            emit_error(out, "misses_query callback not wired");
            return;
        }
        out = ctx.misses_query();
        return;
    }
    if (contains("\"savestate_save\"")) {
        if (!ctx.savestate_save) {
            emit_error(out, "savestate_save callback not wired");
            return;
        }
        std::string path;
        if (!extract_string(req, "\"path\"", path) || path.empty()) {
            emit_error(out, "missing path");
            return;
        }
        std::string serr;
        if (!ctx.savestate_save(path, serr)) {
            emit_error(out, serr.empty() ? "savestate_save failed" : serr.c_str());
            return;
        }
        out = "{\"ok\":true,\"saved\":\"";
        for (char c : path) { if (c == '"' || c == '\\') out.push_back('\\'); out.push_back(c); }
        out += "\"}";
        return;
    }
    if (contains("\"fp_save\"")) {
        if (!ctx.fp_save) {
            emit_error(out, "fp_save callback not wired (insn fingerprinting "
                            "unavailable)");
            return;
        }
        std::string path;
        if (!extract_string(req, "\"path\"", path) || path.empty()) {
            emit_error(out, "missing path");
            return;
        }
        uint32_t n = ctx.fp_save(path);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%u}",
                      static_cast<unsigned>(n));
        out = buf;
        return;
    }
    if (contains("\"savestate_load\"")) {
        if (!ctx.savestate_load) {
            emit_error(out, "savestate_load callback not wired");
            return;
        }
        std::string path;
        if (!extract_string(req, "\"path\"", path) || path.empty()) {
            emit_error(out, "missing path");
            return;
        }
        std::string serr;
        if (!ctx.savestate_load(path, serr)) {
            emit_error(out, serr.empty() ? "savestate_load failed" : serr.c_str());
            return;
        }
        // Report the resume PC + frame so the client can confirm the
        // restore landed where it expects.
        uint32_t pc = ctx.recomp_cpu ? ctx.recomp_cpu->R[15]
                                     : (ctx.cpu ? ctx.cpu->R[15] : 0u);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":true,\"pc\":%u,\"frame\":%llu}",
                      static_cast<unsigned>(pc),
                      static_cast<unsigned long long>(frame_counter(ctx)));
        out = buf;
        return;
    }
    if (contains("\"quit\"")) {
        out = "{\"ok\":true,\"bye\":true}";
        want_quit = true;
        return;
    }

    emit_error(out, "unknown command");
}

}  // namespace

TcpDebugServer::TcpDebugServer()  = default;
TcpDebugServer::~TcpDebugServer() = default;

bool TcpDebugServer::run(int port, const Context& ctx) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "tcp_debug_server: WSAStartup failed\n");
        return false;
    }
#endif

    socket_t srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::fprintf(stderr, "tcp_debug_server: socket() failed\n");
        return false;
    }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::fprintf(stderr, "tcp_debug_server: bind 127.0.0.1:%d failed\n", port);
        CLOSESOCK(srv);
        return false;
    }
    if (::listen(srv, 1) != 0) {
        std::fprintf(stderr, "tcp_debug_server: listen failed\n");
        CLOSESOCK(srv);
        return false;
    }

    std::printf("native: tcp_debug_server listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);

    bool quit_server = false;
    while (!quit_server) {
        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        socket_t c = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (c == INVALID_SOCKET) continue;

        std::string inbuf;
        std::string resp;
        char rbuf[4096];
        bool client_done = false;
        bool step_failed = false;
        while (!client_done) {
            int n = ::recv(c, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break;
            inbuf.append(rbuf, rbuf + n);
            std::size_t nl;
            while ((nl = inbuf.find('\n')) != std::string::npos) {
                std::string_view line(inbuf.data(), nl);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                bool want_quit = false;
                dispatch(ctx, line, resp, want_quit, step_failed);
                resp.push_back('\n');
                if (::send(c, resp.data(),
                           static_cast<int>(resp.size()), 0) < 0) {
                    client_done = true;
                    break;
                }
                if (want_quit) {
                    client_done = true;
                    quit_server = true;
                    break;
                }
                if (step_failed) {
                    // Don't auto-quit; let the client decide.
                    step_failed = false;
                }
                inbuf.erase(0, nl + 1);
            }
            if (inbuf.size() > 8192) {
                std::string err = "{\"ok\":false,\"error\":\"line too long\"}\n";
                ::send(c, err.data(), static_cast<int>(err.size()), 0);
                break;
            }
        }
        CLOSESOCK(c);
    }

    CLOSESOCK(srv);
#ifdef _WIN32
    WSACleanup();
#endif
    return true;
}

}  // namespace gbarecomp::debug
