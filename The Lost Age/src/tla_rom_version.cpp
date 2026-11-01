#include "tla_rom_version.h"

extern "C" const char gsr_game_rom_sha1[];

namespace tla_rom {
namespace {
namespace rr = gbarecomp::rom_revision;
constexpr const char* kTag = "tla-rom";
constexpr const char* sha1s[] = {
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) sha1,
#include "tla_rom_revisions.inc"
#undef GSR_ROM_REVISION
};
constexpr const char* names[] = {
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) name,
#include "tla_rom_revisions.inc"
#undef GSR_ROM_REVISION
};
constexpr rr::Row addresses[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de) {en, {es, it, fr, de}},
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de)
#define TLA_TEXT_ID(en, es, it, fr, de)
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
constexpr rr::OverlayRow overlays[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de)
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de) {#image, en, {es, it, fr, de}},
#define TLA_TEXT_ID(en, es, it, fr, de)
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
constexpr rr::Row texts[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de)
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de)
#define TLA_TEXT_ID(en, es, it, fr, de) {en, {es, it, fr, de}},
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
unsigned revision(){
    static const unsigned index=rr::identify(sha1s,gsr_game_rom_sha1,kTag);
    return index;
}
template<typename Row> std::uint32_t counterpart(const Row* row,const char* kind,std::uint32_t english){
    if(!row) rr::unmapped(kTag,name(),kind,english);
    return row->localized[revision()-1u];
}
}
const char* sha1(){return gsr_game_rom_sha1;}
Version version(){return static_cast<Version>(revision());}
bool spanish(){return version()==Version::Spanish;}
const char* name(){return names[revision()];}
std::uint32_t address(std::uint32_t english){
    return revision()?counterpart(rr::find_sorted(addresses,english),"ROM address",english):english;
}
std::uint32_t text(std::uint32_t english){
    return revision()?counterpart(rr::find_sorted(texts,english),"text ID",english):english;
}
std::uint32_t overlay(const char* image,std::uint32_t english){
    return revision()?counterpart(rr::find(overlays,image,english),image,english):english;
}
}
