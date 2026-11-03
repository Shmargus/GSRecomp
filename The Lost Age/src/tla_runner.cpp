// Golden Sun: The Lost Age -- the bare runner.
//
// The first-boot runner: the gbarecomp engine and the translated game code,
// with none of Golden Sun 1's game-specific hooks (effects, RAM-code
// identity tables). Code the static translation did not find -- RAM-resident
// routines and anything reached only through data -- goes through the
// engine's interpreter bridge and self-heal path, which log each miss so it
// can be added to config/tla/main.toml with evidence.
//
// Routines the game copies into RAM buffers at moving addresses run from
// position-independent translations (src/tla_ram_images.cpp). Instant text
// is the settings screen's fourth Message-speed choice (src/tla_text_speed.cpp).
//
// Golden Sun 1 features are brought over one at a time, each re-measured on
// this ROM; none of GS1's addresses apply here.

// The engine plug-in calls this runner's main directly.
#define SDL_MAIN_HANDLED

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "crash_handler.h"
#include "game_ram_layout.h"
#include "memory_snapshot.h"
#include "runtime.h"
#include "view_config.h"
#include "tla.h"
#include "host_config_ui.h"
#include "../../shared_game/field_character.h"
#include "../../shared_game/spanish_menu.h"
#include "../../shared_game/italian_menu.h"
#include "../../shared_game/french_menu.h"
#include "../../shared_game/german_menu.h"

namespace {

void on_function_entry(std::uint32_t pc) {
    if (tla_retreat_travel_on_entry(pc)) return;
    gsr::field_character::on_entry(pc);
    tla_publish_menu_art();
    tla_text_on_entry(pc);
    tla_gpu_field_on_entry(pc);
    tla_widescreen_on_entry(pc);
    tla_hard_mode_on_entry(pc);
    tla_gs_save_import_on_entry(pc);
    tla_local_music_on_entry(pc);
    tla_custom_battle_on_entry(pc);
    tla_enemy_hp_bar_on_entry(pc);
    tla_sprite_memory_on_entry(pc);
}

}  // namespace

int main(int argc, char** argv) {
    // First thing in main, so as much of the run as possible is covered.
    gbarecomp::crash_handler_install(nullptr);
    // Game RAM layout for the shared engine (game_ram_layout.h). All other
    // fields stay zero, so the Broken Seal-only crash recorders are inactive.
    // TODO-EVIDENCE: inherited from the engine's former built-in Broken Seal
    // value so host effect capture behaves as before; not measured on this
    // ROM.
    {
        GameRamLayout layout;
        layout.iwram_heap_first = 0x03002000u;
        layout.iwram_heap_last = 0x030077FFu;
        runtime_set_game_ram_layout(layout);
    }
    gsr_install_crash_snapshot_writer();

    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-window") == 0) headless = true;
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "TLARecomp --rom <Golden Sun - The Lost Age.gba>\n"
                "\n"
                "Runs without the BIOS by default. Expected ROM SHA-1: %s\n",
                tla_rom::sha1());
            return 0;
        }
    }

    if (tla_rom::spanish())
        gbarecomp::config_ui_set_translations(gsr::spanish_menu::kTable,
            static_cast<int>(std::size(gsr::spanish_menu::kTable)));
    else if (tla_rom::version()==tla_rom::Version::Italian)
        gbarecomp::config_ui_set_translations(gsr::italian_menu::kTable,
            static_cast<int>(std::size(gsr::italian_menu::kTable)));
    else if (tla_rom::version()==tla_rom::Version::French)
        gbarecomp::config_ui_set_translations(gsr::french_menu::kTable,
            static_cast<int>(std::size(gsr::french_menu::kTable)));
    else if (tla_rom::version()==tla_rom::Version::German)
        gbarecomp::config_ui_set_translations(gsr::german_menu::kTlaTable.data(),
            static_cast<int>(gsr::german_menu::kTlaTable.size()));
    else gbarecomp::config_ui_set_translations(nullptr,0);
    tla_install_ram_images();

    gbarecomp::RunOptions options;
    tla_install_text_speed(options);
    tla_install_walk_speed(options);
    // Runner options that are not engine settings live next to the program.
    {
        std::string dir = argc > 0 ? argv[0] : "";
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
        gbarecomp::game_options_load(dir + "game_options.ini");
    }
    tla_install_battle_speed(options);
    tla_install_encounters(options);
    tla_install_widescreen(options);
    tla_install_settings_page(options);
    tla_install_hard_mode(options);
    tla_install_cheat_menu(options);
    tla_install_local_music(options);
    tla_install_walk_through_walls(options);
    tla_install_spell_sparks(options);
    tla_install_gs_save_import(options, headless);
    tla_install_custom_battle(options);
    tla_install_enemy_hp_bar();
    tla_install_trackers();
    tla_install_text(options);
    tla_install_rewind();
    tla_install_gpu_field();
    tla_install_retreat_travel(options);
    {
        namespace field = gsr::field_character;
        static const field::Game walking_character{
            TLA_ROM(0x08021A84u), TLA_ROM(0x08300000u), 0x0300006Cu,
            0x02000458u, 0x18u, 0x19u, 0x1Cu,
            field::kHeroes, {},
        };
        field::install(walking_character);
    }
    options.function_entry_observer = on_function_entry;
    options.builtin_game_name = "Golden Sun: The Lost Age";
    options.builtin_rom_sha1 = tla_rom::sha1();
    // Same as Golden Sun 1: no Nintendo code runs; the
    // runtime's own stand-ins replace every BIOS service.
    options.no_bios_by_default = true;
    options.launcher_region = tla_rom::name();
    options.launcher_game_config = "game.toml";
    options.max_view_width = 480;
    options.max_view_height = 320;
    options.aspect_view = true;
    options.launcher_expose_widescreen = true;
    options.launcher_expose_adaptive_view = false;
    options.widescreen_view_width = 360;
    options.widescreen_view_height = 240;
    options.launcher_aspect_labels = gbarecomp::kViewModeLabels;
    options.launcher_aspect_view_widths = gbarecomp::kViewModeWidths;
    options.launcher_aspect_view_heights = gbarecomp::kViewModeHeights;
    options.launcher_num_aspects = gbarecomp::kViewModeCount;

    const int result = gbarecomp::run_game(argc, argv, options);
    gbarecomp::crash_handler_mark_clean_exit();
    return result;
}
