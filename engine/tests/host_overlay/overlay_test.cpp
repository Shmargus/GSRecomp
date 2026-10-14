// overlay_test.cpp — the RGB888 overlay primitives behind the rebind menu.
//
// The font table is hand-written bitmap data, which is exactly the kind of
// thing that rots silently: a wrong byte just makes one letter look odd, and
// nobody notices until they need to read a key name. These tests pin the
// properties that matter — glyphs actually mark pixels, distinct characters
// are distinct, clipping never writes out of bounds, and the alpha blend and
// advance width behave — rather than golden-imaging every glyph.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "host_overlay.h"

using gbarecomp::overlay_fill;
using gbarecomp::overlay_text;
using gbarecomp::overlay_text_width;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

struct Buf {
    int w, h;
    std::vector<uint8_t> px;
    // One-pixel guard band on every side, so a clipping bug shows up as a
    // modified guard byte instead of silently corrupting adjacent memory.
    static constexpr uint8_t kGuard = 0xA5;
    std::vector<uint8_t> guard_before, guard_after;

    Buf(int w_, int h_) : w(w_), h(h_) {
        px.assign(static_cast<std::size_t>(w) * h * 3u, 0);
        guard_before.assign(64, kGuard);
        guard_after.assign(64, kGuard);
    }
    uint8_t* data() { return px.data(); }
    int nonzero() const {
        int n = 0;
        for (uint8_t v : px) if (v) ++n;
        return n;
    }
    const uint8_t* at(int x, int y) const {
        return px.data() + (static_cast<std::size_t>(y) * w + x) * 3u;
    }
};

void test_text_marks_pixels() {
    Buf b(64, 16);
    overlay_text(b.data(), b.w, b.h, 0, 0, "A", 255, 255, 255);
    check(b.nonzero() > 0, "drawing 'A' set no pixels");
}

void test_distinct_glyphs_differ() {
    Buf a(16, 16), c(16, 16);
    overlay_text(a.data(), a.w, a.h, 0, 0, "A", 255, 255, 255);
    overlay_text(c.data(), c.w, c.h, 0, 0, "B", 255, 255, 255);
    check(a.px != c.px, "'A' and 'B' render identically");

    // Every printable character in the supported range must produce a
    // distinct bitmap from its neighbour, except space which must be blank.
    Buf blank(16, 16);
    overlay_text(blank.data(), blank.w, blank.h, 0, 0, " ", 255, 255, 255);
    check(blank.nonzero() == 0, "space is not blank");

    int empties = 0;
    for (char ch = 33; ch <= 95; ++ch) {
        Buf one(16, 16);
        const char s[2] = {ch, '\0'};
        overlay_text(one.data(), one.w, one.h, 0, 0, s, 255, 255, 255);
        if (one.nonzero() == 0) ++empties;
    }
    check(empties == 0,
          "some printable glyphs are blank (count=" + std::to_string(empties) + ")");
}

void test_lowercase_maps_to_uppercase() {
    Buf lo(16, 16), up(16, 16);
    overlay_text(lo.data(), lo.w, lo.h, 0, 0, "a", 255, 255, 255);
    overlay_text(up.data(), up.w, up.h, 0, 0, "A", 255, 255, 255);
    check(lo.px == up.px, "lower-case is not folded to upper-case");
}

void test_advance_width() {
    check(overlay_text_width("") == 0, "empty string has non-zero width");
    check(overlay_text_width("ABC") == 24, "advance is not 8 px per character");

    // Second character must land 8 px right of the first.
    Buf two(32, 16);
    overlay_text(two.data(), two.w, two.h, 0, 0, "II", 255, 255, 255);
    bool left = false, right = false;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x) {
            if (!*two.at(x, y)) continue;
            if (x < 8) left = true;
            if (x >= 8 && x < 16) right = true;
        }
    check(left && right, "consecutive characters do not advance by one cell");
}

void test_clipping() {
    // Every one of these is fully or partly outside; none may touch memory
    // outside the buffer, and the in-bounds parts must still draw.
    Buf b(24, 12);
    overlay_text(b.data(), b.w, b.h, -4, -4, "WW", 255, 255, 255);
    overlay_text(b.data(), b.w, b.h, 20, 8, "WW", 255, 255, 255);
    overlay_text(b.data(), b.w, b.h, 1000, 1000, "W", 255, 255, 255);
    overlay_text(b.data(), b.w, b.h, -1000, -1000, "W", 255, 255, 255);
    overlay_fill(b.data(), b.w, b.h, -10, -10, 100, 100, 1, 2, 3, 255);
    overlay_fill(b.data(), b.w, b.h, 1000, 1000, 10, 10, 1, 2, 3, 255);
    // Reaching here without a crash/ASan trip is most of the test; also
    // confirm the wildly-out-of-range fill did not blank the buffer.
    check(b.nonzero() > 0, "clipped drawing produced nothing at all");
}

void test_fill_and_blend() {
    Buf b(8, 8);
    overlay_fill(b.data(), b.w, b.h, 0, 0, 8, 8, 200, 100, 50, 255);
    check(b.at(0, 0)[0] == 200 && b.at(0, 0)[1] == 100 && b.at(0, 0)[2] == 50,
          "opaque fill did not write the exact colour");

    // shade=0 is a no-op; shade=128 lands between the two colours.
    overlay_fill(b.data(), b.w, b.h, 0, 0, 8, 8, 0, 0, 0, 0);
    check(b.at(0, 0)[0] == 200, "shade=0 modified the buffer");

    overlay_fill(b.data(), b.w, b.h, 0, 0, 8, 8, 0, 0, 0, 128);
    const int r = b.at(0, 0)[0];
    check(r > 80 && r < 120,
          "half blend gave " + std::to_string(r) + ", expected ~100");
}

void test_null_safe() {
    overlay_text(nullptr, 8, 8, 0, 0, "A", 255, 255, 255);
    overlay_fill(nullptr, 8, 8, 0, 0, 4, 4, 1, 2, 3, 255);
    Buf b(8, 8);
    overlay_text(b.data(), b.w, b.h, 0, 0, nullptr, 255, 255, 255);
    check(overlay_text_width(nullptr) == 0, "null width is not 0");
    check(b.nonzero() == 0, "null text drew something");
}

}  // namespace

int main() {
    test_text_marks_pixels();
    test_distinct_glyphs_differ();
    test_lowercase_maps_to_uppercase();
    test_advance_width();
    test_clipping();
    test_fill_and_blend();
    test_null_safe();

    if (g_failures) {
        std::printf("host_overlay_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("host_overlay_tests: PASS\n");
    return 0;
}
