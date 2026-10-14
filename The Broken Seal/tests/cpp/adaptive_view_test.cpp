#include "../../../launcher/collection_view_settings.h"
#include "../../src/widescreen_policy.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) {
    using namespace gbarecomp;
    namespace wide = gsr::widescreen;
    for (int mode = 0; mode < kViewModeCount; ++mode) {
        for (int aspect = 0; aspect < kViewAspectCount; ++aspect) {
            const auto view = resolve_aspect_view_geometry(
                kViewModeWidths[mode], kViewModeHeights[mode], aspect, 1920, 1080);
            assert(wide::golden_sun_expanded_view_active(view.width, view.height,
                view.extra_left, view.extra_right, view.extra_top, view.extra_bottom) == (mode != 0));
        }
    }
    assert(!wide::golden_sun_expanded_view_active(480, 320, 120, 120, 40, 40));
    assert(!wide::golden_sun_expanded_view_active(482, 320, 121, 121, 80, 80));
    assert(!wide::golden_sun_expanded_view_active(480, 322, 120, 120, 81, 81));

    // The complete view stays inside a sufficiently large room. A smaller
    // room uses only available native camera travel, with no reversed bounds.
    for (int spare = 0; spare <= 320; ++spare) {
        for (const auto margins : {std::pair{21, 22}, std::pair{60, 60},
                                   std::pair{120, 120}, std::pair{80, 80}}) {
            const auto inset = resolve_view_camera_insets(spare, margins.first, margins.second);
            assert(inset.before >= 0 && inset.after >= 0);
            assert(inset.before + inset.after <= spare);
            if (spare >= margins.first + margins.second) {
                assert(inset.before == margins.first && inset.after == margins.second);
            }
        }
    }

    assert(argc == 2);
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    {
        std::ofstream file(directory / "config.ini");
        file << "[Enhancements]\nWidescreen=true\nCpuOverclock=1\n"
                "[Picture]\nAspectRatio=4:3\nColorProfile=warm\n"
                "[Video]\nIntegerScale=false\n[Other]\nRetain=this value\n";
    }
    auto settings = gsr_collection::load_view_settings(directory);
    assert(settings.mode == 1 && settings.aspect == ViewAspectFourThree);
    assert(!settings.sharp_pixels);
    settings.mode = 2;
    settings.aspect = ViewAspectMinimalSharpPixels;
    assert(gsr_collection::save_view_settings(directory, settings));
    settings = gsr_collection::load_view_settings(directory);
    assert(settings.mode == 2 && settings.aspect == ViewAspectMinimalSharpPixels);
    assert(settings.sharp_pixels);
    settings.mode = 0;
    settings.sharp_pixels = false;
    assert(gsr_collection::save_view_settings(directory, settings));
    settings = gsr_collection::load_view_settings(directory);
    assert(!settings.sharp_pixels); // Original ignores the remembered adaptive aspect.
    settings.aspect = ViewAspectSixteenNine;
    assert(gsr_collection::save_view_settings(directory, settings));
    settings = gsr_collection::load_view_settings(directory);
    assert(settings.mode == 0 && settings.aspect == ViewAspectSixteenNine);
    std::ifstream file(directory / "config.ini");
    const std::string contents{std::istreambuf_iterator<char>(file), {}};
    assert(contents.find("CpuOverclock=1") != std::string::npos);
    assert(contents.find("ColorProfile=warm") != std::string::npos);
    assert(contents.find("Retain=this value") != std::string::npos);
    settings.mode = 99;
    assert(!gsr_collection::save_view_settings(directory, settings));
}
