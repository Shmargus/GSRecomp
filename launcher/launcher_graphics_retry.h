#pragma once

#include <string_view>

namespace gsr {
inline bool should_offer_direct3d_retry(bool compatibility_run,
                                      unsigned long exit_code,
                                      bool fresh_crash_report,
                                      std::string_view session,
                                      std::string_view report) {
    if (compatibility_run || exit_code == 0) return false;
    if (session.find("[host-window] graphics-ready") != std::string_view::npos ||
        session.find("host_window: renderer=") != std::string_view::npos) return false;
    if (session.find("[host-window] graphics-startup=opengl") != std::string_view::npos)
        return true;
    return fresh_crash_report &&
           report.find("ChoosePixelFormat") != std::string_view::npos &&
           report.find("wgl") != std::string_view::npos &&
           report.find("SDL") != std::string_view::npos;
}
} // namespace gsr
