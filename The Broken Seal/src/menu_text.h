#pragma once
#include <string>
#include "host_config_ui.h"

namespace gsr {
// The translation table is UTF-8; both games' native menu glyphs use Latin-1.
inline std::string menu_text(const char* english) {
    std::string out;
    const auto* s=reinterpret_cast<const unsigned char*>(gbarecomp::ui_text(english));
    for(;*s;++s) {
        if((*s==0xC2u||*s==0xC3u)&&(s[1]&0xC0u)==0x80u) {
            out+=static_cast<char>(((*s&0x03u)<<6)|(s[1]&0x3Fu));
            ++s;
        } else out+=static_cast<char>(*s);
    }
    return out;
}
}
