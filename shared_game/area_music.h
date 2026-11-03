// Area-specific soundtrack choices, shared by both games. The game supplies
// its measured player/start/frame sites and location-name text base. Reuses
// the trackers' location lookup and ROM text decoder; no extracted assets.
#pragma once
#include "treasure_tracker.h"
#include "tracker_text.h"
#include "local_music_m4a.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "gba_bus.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace gsr::area_music {

using gbarecomp::m4a_music::Recording;
struct Area { unsigned id; std::string name; };

class Controller {
public:
    Controller(gbarecomp::m4a_music::Replacement& music,
               const gbarecomp::m4a_music::Player& player,
               std::uint32_t start, std::uint32_t frame,
               const Recording* songs, std::size_t count, unsigned name_base,
               std::span<const unsigned> battle_tracks = {})
        : music_(music), player_(player), start_(start), frame_(frame),
          songs_(songs), count_(count), name_base_(name_base), battle_tracks_(battle_tracks) {
        for (std::size_t i = 0; i < count_; ++i)
            if (songs_[i].title && std::none_of(tracks_.begin(), tracks_.end(),
                [&](const Recording* song) { return song->track == songs_[i].track; }))
                tracks_.push_back(songs_ + i);
    }

    static std::string key(unsigned area, unsigned song) {
        return "AreaMusic_" + std::to_string(area) + "_" + std::to_string(song);
    }
    static std::string battle_key(unsigned song) { return "BattleMusic_" + std::to_string(song); }
    static void set_area_choice(unsigned area, int track) {
        // The row replaces this area's complete choice, including older
        // source-specific settings; other areas remain independent.
        const auto prefix = "AreaMusic_" + std::to_string(area) + "_";
        const char* reset = prefix.c_str();
        const gbarecomp::GameOptionValue value{key(area, 0), track};
        gbarecomp::game_options_update({&value, 1}, {&reset, 1});
    }
    void randomize_area(unsigned area) {
        if (tracks_.size() < 2) return;
        const int old = gbarecomp::game_option_get(key(area, 0).c_str(), 0);
        const int seed = new_seed(old < 0 && old != std::numeric_limits<int>::min() ? -old : 0);
        const auto prefix = "AreaMusic_" + std::to_string(area) + "_";
        const char* reset = prefix.c_str();
        const gbarecomp::GameOptionValue values[] = {
            {key(area, 0), -seed}, {gbarecomp::kOptionAreaMusic, 1}};
        gbarecomp::game_options_update(values, {&reset, 1});
    }
    void randomize_track(const std::string& option, const Recording& original) {
        const int previous = gbarecomp::game_option_get(option.c_str(), 0);
        std::vector<const Recording*> candidates;
        for (const auto* song : tracks_)
            if (song->track != original.track && int(song->track) != previous)
                candidates.push_back(song);
        if (candidates.empty()) return;
        const auto* selected = candidates[std::uniform_int_distribution<std::size_t>(0, candidates.size() - 1)(random_)];
        const gbarecomp::GameOptionValue values[] = {
            {option, int(selected->track)}, {gbarecomp::kOptionAreaMusic, 1}};
        gbarecomp::game_options_update(values);
    }
    void randomize_all() {
        std::vector<Area> areas;
        {
            std::lock_guard<std::mutex> lock(view_mutex_);
            areas = areas_;
        }
        if (areas.empty() || tracks_.size() < 2) return;
        const int seed = new_seed(gbarecomp::game_option_get("MusicShuffle", 0));
        std::vector<gbarecomp::GameOptionValue> values{
            {"MusicShuffle", seed}, {gbarecomp::kOptionAreaMusic, 1}};
        for (const auto& area : areas) values.push_back({key(area.id, 0), -seed});
        for (const auto track : battle_tracks_)
            if (const auto* song = by_track(track))
                values.push_back({battle_key(track), int(shuffled(*song, seed)->track)});
        const char* prefixes[] = {"AreaMusic_", "BattleMusic_", "MusicShuffle"};
        gbarecomp::game_options_update(values, prefixes);
    }
    static void reset_all() {
        const char* prefixes[] = {"AreaMusic_", "BattleMusic_", "MusicShuffle"};
        gbarecomp::game_options_update({}, prefixes);
    }

    // The accepted header store precedes ALL target-specific initialisation.
    // Both measured drivers still use R7 for voicegroup, tracks and reverb.
    // Leave the stored header and R2 priority as requested: the game's
    // StartOrContinue equality/priority checks must keep their original input.
    void header_write(std::uint32_t pc, std::uint32_t address,
                      std::uint32_t source, std::uint32_t width) {
        if (pc != player_.header_store || address != player_.info || width != 4) return;
        const auto selected = resolve(source, current_area());
        source_ = source;
        played_ = selected;
        force_ = false;
        g_cpu.R[7] = selected;
        music_.set_playback_header(source, selected);
    }

    // The frame-wait routine is outside the sound IRQ and already supports
    // guest_call. Let the original MPlayStart initialise native audio when an
    // area/choice changes; its accepted-store hook above also selects the
    // recording. In particular, towns sharing a continued song stay separate.
    void on_entry(std::uint32_t pc) {
        if (pc != frame_ || restarting_) return;
        prepare_areas();
        const int area = current_area();
        const auto source = bus_read_u32(player_.info);
        const auto* song = by_header(source);
        if (!song) return;
        if (source != source_ || !played_) {
            source_ = source;
            played_ = source;
        }
        const auto selected = resolve(source, area);
        if (!force_ && selected == played_) return;
        // Do not revive stopped music, unpause it, or interrupt a fade/jingle
        // duck. The pending choice is picked up when ordinary music resumes.
        const auto status = bus_read_u32(player_.info + 4);
        if ((status & 0x80000000u) || !(status & 0xFFFFu) ||
            bus_read_u16(player_.info + 0x24)) return;
        for (unsigned i = 0; i < player_.track_count; ++i) {
            const auto at = player_.tracks + i * player_.track_bytes;
            if ((bus_read_u8(at) & 0xC0u) == 0x80u && bus_read_u8(at + 0x13) != 64u)
                return;
        }
        restarting_ = true;
        std::uint32_t ignored = 0;
        gbarecomp::guest_call(start_ | 1u, player_.info, source, 0, 0, &ignored);
        restarting_ = false;
    }

    void on_load() {
        // Native track pointers in the state may belong to a previous choice.
        // Reapply current preferences through the driver at the safe frame site.
        // Untouched/default installations retain exact native music resume.
        // Once this feature has been used, even an explicit Off must restore
        // original tracks when loading a state made with a replacement.
        force_ = gbarecomp::game_option_get(gbarecomp::kOptionAreaMusic, -1) != -1;
        source_ = played_ = 0;
        music_.set_playback_header(0, 0);
    }

    void install() {
        installed_ = this;
        gbarecomp::config_ui_set_music_page(draw_installed);
    }

private:
    int new_seed(int previous) {
        for (;;) {
            const int seed = std::uniform_int_distribution<int>(1, std::numeric_limits<int>::max())(random_);
            if (seed == previous) continue;
            // With three or more choices another cycle can change every old
            // assignment too; rerolling must not leave the currently heard
            // theme unchanged just because an area has several story themes.
            if (previous > 0 && tracks_.size() > 2 &&
                std::any_of(tracks_.begin(), tracks_.end(), [&](const Recording* song) {
                    return shuffled(*song, seed)->track == shuffled(*song, previous)->track;
                })) continue;
            return seed;
        }
    }
    const Recording* shuffled(const Recording& source, int seed) const {
        if (tracks_.size() < 2 || seed <= 0) return &source;
        std::vector<std::size_t> order(tracks_.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::mt19937 generator(static_cast<unsigned>(seed));
        std::shuffle(order.begin(), order.end(), generator);
        // The shuffled order is read as one loop: each theme plays the track
        // after it. No theme keeps its own track; with only two tracks this
        // simply swaps them.
        for (std::size_t i = 0; i < order.size(); ++i)
            if (tracks_[order[i]]->track == source.track)
                return tracks_[order[(i + 1) % order.size()]];
        return &source;
    }
    const Recording* by_header(std::uint32_t header) const {
        for (std::size_t i = 0; i < count_; ++i)
            if (songs_[i].header == header) return songs_ + i;
        return nullptr;
    }
    const Recording* by_track(unsigned track) const {
        for (std::size_t i = 0; i < count_; ++i)
            if (songs_[i].track == track) return songs_ + i;
        return nullptr;
    }
    unsigned canonical(unsigned name) const {
        const auto* game = treasure_tracker::game;
        if (!game) return name;
        for (const auto& p : game->place_names) {
            if (p.offset != name) continue;
            for (const auto& q : game->place_names)
                if (q.place == p.place) name = std::min(name, q.offset);
            break;
        }
        return name;
    }
    int current_area() const {
        const auto* bus = gbarecomp::active_bus();
        const auto* game = treasure_tracker::game;
        if (!bus || !game || !bus->rom_ptr() || !bus->ewram_ptr()) return -1;
        const auto* at = bus->ewram_ptr() + game->current_room;
        bool found = false;
        const auto name = treasure_tracker::location(*game, bus->rom_ptr(), bus->rom_size(),
            at[0] | (at[1] << 8), at[2] | (at[3] << 8), &found);
        return found ? int(canonical(name)) : -1;
    }
    std::uint32_t resolve(std::uint32_t source, int area) const {
        const auto* song = by_header(source);
        if (!song || !gbarecomp::game_option_get(gbarecomp::kOptionAreaMusic, 0))
            return source;
        const auto chosen = [&](const std::string& option) -> const Recording* {
            const int choice = gbarecomp::game_option_get(option.c_str(), 0);
            return choice > 0 ? by_track(unsigned(choice)) : nullptr;
        };
        // Keep earlier, explicitly saved source-specific choices until the
        // player edits/resets their area row. Battle themes have their own
        // rows, so a location theme never swallows battles or their results.
        if (area >= 0)
            if (const auto* target = chosen(key(unsigned(area), song->track))) return target->header;
        if (is_battle(song->track)) {
            if (const auto* target = chosen(battle_key(song->track))) return target->header;
        } else if (area >= 0) {
            const int value = gbarecomp::game_option_get(key(unsigned(area), 0).c_str(), 0);
            if (value < 0 && value != std::numeric_limits<int>::min()) return shuffled(*song, -value)->header;
            if (const auto* target = value > 0 ? by_track(unsigned(value)) : nullptr) return target->header;
        } else {
            // Randomize all also covers music played outside a named area.
            return shuffled(*song, gbarecomp::game_option_get("MusicShuffle", 0))->header;
        }
        return source;
    }
    bool is_battle(unsigned track) const {
        return std::find(battle_tracks_.begin(), battle_tracks_.end(), track) != battle_tracks_.end();
    }
    void prepare_areas() {
        if (prepared_) return;
        const auto* bus = gbarecomp::active_bus();
        const auto* game = treasure_tracker::game;
        const auto* text = tracker_text::config;
        if (!bus || !game || !text || !bus->rom_ptr()) return;
        const auto* rom = bus->rom_ptr();
        const auto size = bus->rom_size();
        mods::TextEncoder decoder({rom, size}, text->text_table, text->tree_table);
        std::vector<Area> areas;
        for (std::size_t at = game->location_records; at + 8 <= size; at += 8) {
            if (rom[at] == 0xFFu && rom[at + 1] == 0xFFu) break;
            const unsigned offset = rom[at + 4] | (rom[at + 5] << 8) |
                (unsigned(rom[at + 6]) << 16) | (unsigned(rom[at + 7]) << 24);
            const unsigned id = canonical(offset);
            if (std::any_of(areas.begin(), areas.end(), [id](const Area& a) { return a.id == id; })) continue;
            std::string name;
            if (!decoder.decode(name_base_ + id, &name)) continue;
            name = tracker_text::utf8(name);
            // Unused developer maps ("Test 0".."Test 7", "Shop Test", ...) in
            // every language; no real place name contains "test".
            std::string lower = name;
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find("test") != std::string::npos) continue;
            if (!name.empty()) areas.push_back({id, std::move(name)});
        }
        std::sort(areas.begin(), areas.end(), [](const Area& a, const Area& b) { return a.name < b.name; });
        std::lock_guard<std::mutex> lock(view_mutex_);
        areas_ = std::move(areas);
        prepared_ = true;
    }
    static void draw_installed() { if (installed_) installed_->draw(); }
    void draw() {
#ifdef GBARECOMP_HAVE_IMGUI
        using namespace gbarecomp;
        bool ready;
        {
            std::lock_guard<std::mutex> lock(view_mutex_);
            ready = !areas_.empty() && tracks_.size() > 1;
        }
        const float random_width = ImGui::CalcTextSize(ui_text("Randomize all")).x + ImGui::GetStyle().FramePadding.x * 2;
        const float reset_width = ImGui::CalcTextSize(ui_text("Reset all")).x + ImGui::GetStyle().FramePadding.x * 2;
        const float available = std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x);
        const float fit = std::min(1.0f, available / (random_width + reset_width));
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(ui_text("Randomize all"), ImVec2(random_width * fit, 0))) randomize_all();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui_text("Randomize all"));
        ImGui::SameLine();
        if (ImGui::Button(ui_text("Reset all"), ImVec2(reset_width * fit, 0))) reset_all();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui_text("Reset all music choices."));
        ImGui::TextWrapped("%s", ui_text("Randomize all music, including story themes. Reset all restores every music choice."));
        bool enabled = game_option_get(kOptionAreaMusic, 0) != 0;
        if (ImGui::Checkbox(ui_text("Enable area music choices"), &enabled))
            game_option_set(kOptionAreaMusic, enabled ? 1 : 0);
        ImGui::TextWrapped("%s", ui_text("Choose a track for each area or battle theme. Reset restores that entry's game music."));
        ImGui::TextWrapped("%s", ui_text("Area choices leave battle music separate. Default follows the game's normal story and location changes."));
        std::vector<Area> areas;
        {
            std::lock_guard<std::mutex> lock(view_mutex_);
            areas = areas_;
        }
        ImGui::TextUnformatted(ui_text("Search"));
        filter_.Draw("##MusicSearch", -1);
        ImGui::SeparatorText(ui_text("Areas"));
        if (areas.empty()) ImGui::TextWrapped("%s", ui_text("Load a game to choose area music."));
        if (begin_choices("MusicAreas")) {
            for (const auto& area : areas) {
                if (!filter_.PassFilter(area.name.c_str())) continue;
                bool specific = false;
                track_choices([&](const Recording& song) {
                    const int value = game_option_get(key(area.id, song.track).c_str(), 0);
                    if (value > 0 && by_track(unsigned(value))) specific = true;
                });
                draw_choice(area.name.c_str(), key(area.id, 0), nullptr, int(area.id), specific);
                // Preserve and expose settings made with the earlier source
                // picker. Editing/resetting the area row clears just that
                // area's old rules. Each individual old rule has Reset too.
                track_choices([&](const Recording& song) {
                    const auto option = key(area.id, song.track);
                    const int value = game_option_get(option.c_str(), 0);
                    if (value <= 0 || !by_track(unsigned(value))) return;
                    const auto label = area.name + " / " + song.title;
                    draw_choice(label.c_str(), option, &song);
                });
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText(ui_text("Battle music"));
        if (begin_choices("MusicBattles")) {
            track_choices([&](const Recording& song) {
                if (is_battle(song.track) && filter_.PassFilter(song.title))
                    draw_choice(song.title, battle_key(song.track), &song);
            });
            ImGui::EndTable();
        }
        if (!enabled) ImGui::TextWrapped("%s", ui_text("Choices are saved. Enable area music choices to hear them."));
#endif
    }
#ifdef GBARECOMP_HAVE_IMGUI
    static bool begin_choices(const char* id) {
        if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchSame |
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings)) return false;
        ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Randomize", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        const float reset_width = ImGui::CalcTextSize(gbarecomp::ui_text("Reset")).x +
            ImGui::GetStyle().FramePadding.x * 2;
        ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, reset_width);
        return true;
    }
    void draw_choice(const char* label, const std::string& option,
                     const Recording* original, int area = -1, bool specific = false) {
        using namespace gbarecomp;
        const int choice = game_option_get(option.c_str(), 0);
        const bool shuffle = area >= 0 && choice < 0 && choice != std::numeric_limits<int>::min();
        const auto* target = choice > 0 ? by_track(unsigned(choice)) : nullptr;
        const char* default_name = original ? original->title : ui_text("Default (game music)");
        const char* preview = shuffle ? ui_text("Randomized") : target ? target->title : default_name;
        const auto save = [&](int track) {
            if (area >= 0) set_area_choice(unsigned(area), track);
            else game_option_set(option.c_str(), track);
        };
        ImGui::PushID(option.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", label);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Track", preview)) {
            if (ImGui::Selectable(default_name, !target && !shuffle)) save(0);
            track_choices([&](const Recording& song) {
                if (original && original->track == song.track) return;
                if (ImGui::Selectable(song.title, target && target->track == song.track)) save(int(song.track));
            });
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shuffle ? ui_text("Each original theme has its own replacement.") : preview);
        ImGui::TableNextColumn();
        if (dice_button()) {
            if (area >= 0) randomize_area(unsigned(area));
            else if (original) randomize_track(option, *original);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(choice == 0 && !specific);
        if (ImGui::Button(ui_text("Reset"))) save(0);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    static bool dice_button() {
        const float side = ImGui::GetFrameHeight();
        const bool clicked = ImGui::Button("##Randomize", ImVec2(side, side));
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        auto* draw = ImGui::GetWindowDrawList();
        const auto ink = ImGui::GetColorU32(ImGuiCol_Text);
        const float inset = side / 4;
        const ImVec2 a(min.x + inset, min.y + inset), b(max.x - inset, max.y - inset);
        draw->AddRect(a, b, ink);
        const float dot = (b.x - a.x) / 10;
        for (const auto& position : {ImVec2(0.25f, 0.25f), ImVec2(0.75f, 0.25f),
                                    ImVec2(0.5f, 0.5f), ImVec2(0.25f, 0.75f), ImVec2(0.75f, 0.75f)})
            draw->AddCircleFilled(ImVec2(a.x + (b.x - a.x) * position.x,
                                        a.y + (b.y - a.y) * position.y), dot, ink);
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
            ImGui::SetTooltip("%s", gbarecomp::ui_text("Randomize this entry"));
        return clicked;
    }
#endif
#ifdef GBARECOMP_HAVE_IMGUI
    template<class F> void track_choices(F f) const {
        for (const auto* entry : tracks_) {
            const auto& song = *entry;
            ImGui::PushID(int(song.track)); f(song); ImGui::PopID();
        }
    }
#endif
    inline static Controller* installed_ = nullptr;
    gbarecomp::m4a_music::Replacement& music_;
    gbarecomp::m4a_music::Player player_;
    std::uint32_t start_, frame_;
    const Recording* songs_;
    std::size_t count_;
    std::vector<const Recording*> tracks_;
    unsigned name_base_;
    std::span<const unsigned> battle_tracks_;
    std::uint32_t source_ = 0, played_ = 0;
    bool force_ = false, restarting_ = false, prepared_ = false;
    std::mutex view_mutex_;
    std::vector<Area> areas_;
    std::mt19937 random_{std::random_device{}()};
#ifdef GBARECOMP_HAVE_IMGUI
    ImGuiTextFilter filter_;
#endif
};
} // namespace gsr::area_music
