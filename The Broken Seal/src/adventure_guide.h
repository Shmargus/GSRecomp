// TBS guide, verified automatic rules and optional verification recorder.
// Never infer completion from a visited room or write guest memory.
#pragma once
#include "adventure_guide_data.h"
#include "adventure_rules.h"
#include "adventure_steps.h"
#include "adventure_walkthrough.h"
#include "adventure_ui.h"
#include "developer_session.h"
#include "game_options.h"
#include "game_hotkeys.h"
#include "host_config_ui.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#include <SDL_keyboard.h>
#include <SDL_video.h>
#endif

namespace gsr::adventure {
inline std::atomic<bool> enabled{false}, recording{false}, restored{true};
inline std::atomic<int> checkpoint{-1}, record_status{0};
inline std::atomic<bool> progress_ready{false};
inline std::atomic<unsigned> collected_djinn{0};
inline std::array<std::atomic<Progress>, std::size(objectives)> progress{};
inline std::atomic<unsigned> current_objective{0};
inline std::atomic<bool> guide_mode{true}, collectibles_enabled{false};
inline bool collectibles_edit = false;
inline std::atomic<const GuideStep*> current_hint{nullptr};
// Live modes never use the log browser's selected entry.
inline const char* tracker_objective() {
    if (!progress_ready.load()) return "Waiting for game progress";
    if (const auto* hint = current_hint.load()) return hint->objective;
    if (const auto route = walkthrough::current.load(); route && route->action)
        return route->action->objective;
    const unsigned i = current_objective.load();
    if (i >= std::size(objectives)) return "Current objective is not mapped yet";
    // Unknown progress must not present a guessed route paragraph as current.
    if (progress[i].load() == Progress::Unknown) return "Current objective is not mapped yet";
    if (progress[i].load() == Progress::InProgress)
        return "Finish the remaining rewards, travel or story scene";
    return objectives[i].tracker ? objectives[i].tracker : "Current objective is not mapped yet";
}
inline bool visible = true, hold = false;
inline std::atomic<bool> guide_hotkey_held{false};
inline bool layout_edit = false, layout_initialized = false, layout_window_focused = false;
inline int layout_x = 16, layout_y = 16, layout_width = 400, font_scale = 2;
inline int chapter = 0, selected = 0;
inline int saved_layout_x = 16, saved_layout_y = 16, saved_layout_width = 400;
inline bool completion = false;
inline std::mutex path_mutex;
inline std::string record_path;
inline void (*previous_draw)() = nullptr;
inline bool (*previous_wants_keyboard)() = nullptr;
// The two save blocks are written from 02000000 and 02001000 by
// 080207C4/08005920 (seamless_mode.h). Snapshot both, including padding.
inline constexpr std::size_t save_bytes = 0x2000;
struct Recorder {
    std::FILE* file = nullptr;
    std::array<unsigned char, save_bytes> previous{};
    bool baseline = false;
    ~Recorder() { if (file) std::fclose(file); }
};
inline Recorder recorder;

inline void close_recording() {
    if (recorder.file) std::fclose(recorder.file);
    recorder.file = nullptr;
    recorder.baseline = false;
}
inline void persist_layout() {
    if (layout_x != saved_layout_x) {
        gbarecomp::game_option_set("AdventureX", layout_x); saved_layout_x = layout_x;
    }
    if (layout_y != saved_layout_y) {
        gbarecomp::game_option_set("AdventureY", layout_y); saved_layout_y = layout_y;
    }
    if (layout_width != saved_layout_width) {
        gbarecomp::game_option_set("AdventureWidth", layout_width); saved_layout_width = layout_width;
    }
}
inline void put(unsigned long long value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) std::fputc((value >> (i * 8)) & 255, recorder.file);
}
inline void sample(const unsigned char* ram, unsigned long long frame, const unsigned char* iwram = nullptr) {
    walkthrough::sample(enabled.load() || collectibles_enabled.load() ? ram : nullptr, iwram);
    current_hint.store(ram && enabled.load() ? guide_for(ram) : nullptr);
    if (ram && enabled.load()) {
        unsigned found = 0;
        for (const auto& rule : rules) {
            const bool done = read_flag(ram, rule.flag);
            progress[objective_index(rule.id)].store(done ? Progress::Complete : Progress::Pending);
            if (std::string_view(rule.id).find("gs1-djinn-") == 0) found += done;
        }
        for (const auto& alias : aliases)
            progress[objective_index(alias.id)].store(progress[objective_index(alias.source)].load());
        progress[objective_index("gs1-01-steps-1")].store(
            prologue_complete(ram) ? Progress::Complete : Progress::Pending);
        for (const auto& battle : battles) {
            const bool won = (read_flag(ram, battle.flag) ||
                (battle.alternate_flag && read_flag(ram, battle.alternate_flag))) &&
                (!battle.second_flag || read_flag(ram, battle.second_flag));
            progress[objective_index(battle.id)].store(!won ? Progress::Pending :
                battle.whole_step ? Progress::Complete : Progress::InProgress);
        }
        progress[objective_index("gs1-quest-28")].store(
            found == 28 ? Progress::Complete : Progress::Pending);
        // Follow the latest observed main-route milestone without pretending
        // that intervening, unmapped objectives have completion rules.
        unsigned next = 0;
        for (unsigned i = 0; i < std::size(objectives); ++i) {
            bool optional = false;
            for (const auto& ch : chapters)
                if (i >= ch.first && i < ch.end) optional = ch.optional;
            if (optional || std::string_view(objectives[i].kind) != "steps") continue;
            const auto state = progress[i].load();
            if (state == Progress::Complete) next = i + 1;
            else if (state == Progress::InProgress) next = i;
        }
        for (; next < std::size(objectives); ++next)
            if (std::string_view(objectives[next].kind) == "steps" &&
                progress[next].load() != Progress::Complete) break;
        if (next < std::size(objectives)) current_objective.store(next);
        collected_djinn.store(found);
        progress_ready.store(true);
    } else {
        progress_ready.store(false);
    }
    if (!recording.load()) {
        close_recording();
        if (record_status.load() != -1) record_status.store(0);
        return;
    }
    if (!ram || record_status.load() == -1) return;
    if (!recorder.file) {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        const auto dir = std::filesystem::path("logs") / ("adventure_" + std::to_string(stamp));
        std::error_code ec;
        if (!std::filesystem::create_directories(dir, ec) || ec) { record_status.store(-1); return; }
        const auto path = dir / "progress.bin";
        recorder.file = std::fopen(path.string().c_str(), "wb");
        if (!recorder.file) { record_status.store(-1); return; }
        { std::lock_guard<std::mutex> lock(path_mutex); record_path = path.string(); }
        std::fwrite("GSQUEST2", 1, 8, recorder.file);
        put(save_bytes, 4);
        // A per-file dictionary preserves stable guide IDs when content is edited.
        put(std::size(objectives), 4);
        for (const auto& objective : objectives) {
            const std::string id(objective.id);
            put(id.size(), 4); std::fwrite(id.data(), 1, id.size(), recorder.file);
        }
        recorder.baseline = false;
        record_status.store(1);
    }
    const bool reset = restored.exchange(false) || !recorder.baseline;
    const int mark = checkpoint.exchange(-1);
    unsigned changes = 0;
    for (unsigned i = 0; i < save_bytes; ++i)
        if (reset || ram[i] != recorder.previous[i]) ++changes;
    if (!changes && mark < 0) return;
    // Explicit little endian: type, frame, map, room, checkpoint, count,
    // followed by count pairs of (u16 offset, u8 current value).
    put(reset ? 1 : 2, 1); put(frame, 8);
    put(ram[0x408] | (unsigned(ram[0x409]) << 8), 2);
    put(ram[0x400] | (unsigned(ram[0x401]) << 8), 2);
    put(mark < 0 ? 0xffffffffu : unsigned(mark), 4); put(changes, 2);
    for (unsigned i = 0; i < save_bytes; ++i) {
        if (!reset && ram[i] == recorder.previous[i]) continue;
        put(i, 2); put(ram[i], 1); recorder.previous[i] = ram[i];
    }
    recorder.baseline = true;
    if (std::fflush(recorder.file) || std::ferror(recorder.file)) {
        close_recording(); record_status.store(-1);
    }
}

#ifdef GBARECOMP_HAVE_IMGUI
inline bool draw_remaining_treasure(const walkthrough::Snapshot* route, float width) {
    if (!route) return false;
    bool shown = false;
    for (const auto& pickup : route->treasure) {
        if (pickup.done == walkthrough::Truth::Yes) continue;
        if (!shown) adventure_ui::draw_text("Uncollected treasure", width, font_scale,
            IM_COL32(255, 216, 110, 255));
        shown = true;
        adventure_ui::draw_text(walkthrough::treasures[pickup.index].label, width, font_scale);
    }
    return shown;
}

inline void page() {
    bool on = enabled.load();
    if (ImGui::Checkbox("Quest Tracker and Guide (TBS)", &on)) {
        enabled.store(on); gbarecomp::game_option_set("AdventureGuide", on);
        if (!on) layout_edit = false;
    }
    bool checklist = collectibles_enabled.load();
    if (ImGui::Checkbox("Area collectibles window", &checklist)) {
        collectibles_enabled.store(checklist);
        gbarecomp::game_option_set("AdventureCollectibles", checklist);
        if (!checklist) collectibles_edit = false;
    }
    if (checklist && ImGui::Button(collectibles_edit ? "Finish collectible window layout" : "Edit collectible window layout"))
        collectibles_edit = !collectibles_edit;
    ImGui::TextWrapped("F1 > Hotkeys configures keyboard and controller shortcuts for the guide and collectibles.");
    if (!on) return;
    bool location_mode = guide_mode.load();
    int overlay_mode = location_mode ? 1 : 0;
    if (ImGui::Combo("Overlay mode", &overlay_mode, "Quest Tracker\0Guide\0")) {
        location_mode = overlay_mode == 1;
        guide_mode.store(location_mode);
        gbarecomp::game_option_set("AdventureGuideMode", location_mode);
    }
    if (location_mode) {
        bool completionist = walkthrough::completionist.load();
        int walkthrough_mode = completionist ? 1 : 0;
        if (ImGui::Combo("Walkthrough", &walkthrough_mode, "Regular walkthrough\0" "100% walkthrough\0")) {
            completionist = walkthrough_mode == 1;
            walkthrough::completionist.store(completionist);
            gbarecomp::game_option_set("AdventureCompletionist", completionist);
        }
        ImGui::TextWrapped("Regular follows the story; 100% also guides optional Djinn, treasure and areas.");
    }
    if (location_mode && progress_ready.load()) {
        if (const auto* hint = current_hint.load()) {
            adventure_ui::draw_text(hint->area, ImGui::GetContentRegionAvail().x, font_scale,
                IM_COL32(255, 216, 110, 255));
            adventure_ui::draw_text(hint->text, ImGui::GetContentRegionAvail().x, font_scale);
        }
    }
    ImGui::TextWrapped("The prologue, all Djinn and major boss fights update automatically. A defeated boss does not confirm later rewards or scenes. Unmapped steps stay untracked.");
    if (progress_ready.load()) ImGui::Text("Djinn: %u / 28", collected_djinn.load());
    if (ImGui::Checkbox("Show objective overlay", &visible))
        gbarecomp::game_option_set("AdventureVisible", visible);
    if (ImGui::Checkbox("Only show while holding the guide binding", &hold))
        gbarecomp::game_option_set("AdventureHold", hold);
    ImGui::TextWrapped("Set keyboard and controller bindings in F1 > Hotkeys: Guide on/off, Guide show/hide, and Collectibles show/hide.");
    if (ImGui::Button(layout_edit ? "Finish overlay layout" : "Edit overlay layout"))
        layout_edit = !layout_edit;
    ImGui::SameLine();
    if (ImGui::Button("Reset overlay layout")) {
        layout_x = layout_y = 16; layout_width = 400; font_scale = 2;
        layout_initialized = false;
        persist_layout();
        gbarecomp::game_option_set("AdventureScale", font_scale);
    }
    const int old_scale = font_scale;
    ImGui::SliderInt("Text size", &font_scale, 1, 3);
    if (font_scale != old_scale) gbarecomp::game_option_set("AdventureScale", font_scale);
    if (layout_edit) ImGui::TextWrapped("Drag the guide to move it; drag a corner to resize it.");
    bool record = recording.load();
    if (ImGui::Checkbox("Record progress for verification", &record)) {
        recording.store(record); record_status.store(0);
        gbarecomp::game_option_set("AdventureRecord", record);
    }
    if (record_status.load() == -1) ImGui::TextWrapped("Recording failed. Check disk space and folder access; turn recording off and on to retry.");
    if (record_status.load() == 1) {
        std::lock_guard<std::mutex> lock(path_mutex);
        ImGui::TextWrapped("Recording locally: %s", record_path.c_str());
    }
    ImGui::TextWrapped("Recording follows save loads and rewinds. These private files stay on this computer.");
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Location walkthrough reference")) {
        const auto route = walkthrough::current.load();
        if (route && route->section) {
            adventure_ui::draw_text(route->section->title, ImGui::GetContentRegionAvail().x, font_scale);
            ImGui::TextWrapped("Location selects this reference automatically. Only verified completion conditions mark steps done; full automatic guidance is still in progress.");
            if (!draw_remaining_treasure(route.get(), ImGui::GetContentRegionAvail().x))
                ImGui::TextUnformatted("No uncollected mapped treasure in this area.");
            for (const auto& row : route->rows) {
                const auto& entry = walkthrough::entries[row.index];
                const char* marker = row.done == walkthrough::Truth::Yes ? "[Done] " :
                    row.milestone == walkthrough::Truth::Yes ? "[In progress; other actions remain] " :
                    row.done == walkthrough::Truth::No ? "[Open] " : "[Not tracked] ";
                std::string text = std::string(marker) + entry.text;
                if (*entry.item) text += std::string(" - ") + entry.item;
                adventure_ui::draw_text(text.c_str(), ImGui::GetContentRegionAvail().x, font_scale);
            }
            ImGui::TextWrapped("Source: %s", route->section->source);
        } else {
            ImGui::TextWrapped("No location reference is selected here yet.");
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Quest log (browsing does not change the live overlay)");
    ImGui::Checkbox("Show optional entries", &completion);
    if (!completion && (chapters[chapter].optional || std::string(objectives[selected].kind) != "steps")) {
        if (chapters[chapter].optional) chapter = 0;
        selected = int(chapters[chapter].first);
    }
    ImGui::TextWrapped("Browsing never marks an entry complete. Progress follows the loaded save and rewinds.");
    if (ImGui::BeginCombo("Chapter", chapters[chapter].title)) {
        for (unsigned i = 0; i < std::size(chapters); ++i) {
            if (!completion && chapters[i].optional) continue;
            if (ImGui::Selectable(chapters[i].title, chapter == int(i))) {
                chapter = int(i); selected = int(chapters[i].first);
            }
        }
        ImGui::EndCombo();
    }
    const auto& ch = chapters[chapter];
    ImGui::TextUnformatted("Entries");
    for (unsigned i = ch.first; i < ch.end; ++i) {
        const auto& item = objectives[i];
        if (!completion && std::string(item.kind) != "steps") continue;
        ImGui::PushID(item.id);
        const Progress state = progress_ready.load() ? progress[i].load() : Progress::Unknown;
        const char* marker = state == Progress::Complete ? "[Done]" :
            state == Progress::InProgress ? "[Boss defeated]" :
            state == Progress::Pending && i == current_objective.load() ? "[Next]" :
            state == Progress::Pending ? "[Open]" : "[Not tracked]";
        const std::string row = std::string(marker) + " " + item.text;
        const float row_width = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f);
        const float row_height = adventure_ui::measure_text_height(row.c_str(), row_width, font_scale) + 8.0f;
        const ImVec2 row_pos = ImGui::GetCursorScreenPos();
        const bool is_selected = selected == int(i);
        if (ImGui::Selectable("##entry", is_selected, ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0, row_height))) {
            selected = int(i);
        }
        const ImVec2 after_row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 4.0f, row_pos.y + 4.0f));
        adventure_ui::draw_text(row.c_str(), row_width, font_scale);
        ImGui::SetCursorScreenPos(after_row);
        ImGui::PopID();
    }
}

inline bool wants_keyboard() {
    if (previous_wants_keyboard && previous_wants_keyboard()) return true;
    return layout_edit || collectibles_edit;
}

inline void draw_collectibles() {
    if (!collectibles_enabled.load() || (gbarecomp::config_ui_visible() && !collectibles_edit)) return;
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + std::max(0.0f, vp->WorkSize.x - float(layout_width)),
        vp->WorkPos.y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(float(layout_width), 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(160.0f, vp->WorkSize.x), 0), vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(collectibles_edit ? 0.92f : 0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing;
    if (!collectibles_edit) flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (ImGui::Begin("Area collectibles##AdventureCollectibles", &open, flags)) {
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        adventure_ui::draw_text("Area collectibles", width, font_scale, IM_COL32(255, 216, 110, 255));
        const auto route = walkthrough::current.load();
        if (!route) adventure_ui::draw_text("Waiting for game progress", width, font_scale);
        else if (!route->area) adventure_ui::draw_text("Collectibles are not mapped here yet", width, font_scale);
        else {
            adventure_ui::draw_text(route->area->title, width, font_scale);
            for (const auto& item : route->collectibles) {
                const bool collected = item.done == walkthrough::Truth::Yes;
                std::string label = collected ? "[x] " : item.done == walkthrough::Truth::No ? "[ ] " : "[?] ";
                const std::string_view text(item.label);
                label += std::string_view(item.kind) == "djinn" ?
                    "Djinni: " + std::string(text.substr(0, text.find(':'))) : std::string(text);
                adventure_ui::draw_text(label.c_str(), width, font_scale,
                    collected ? IM_COL32(160, 180, 160, 255) : IM_COL32(255, 255, 255, 255));
            }
            if (route->collectibles.empty()) adventure_ui::draw_text("No mapped collectibles in this area", width, font_scale);
        }
        if (collectibles_edit && ImGui::Button("Finish layout")) collectibles_edit = false;
    }
    ImGui::End();
    if (!open) {
        collectibles_enabled.store(false); collectibles_edit = false;
        gbarecomp::game_option_set("AdventureCollectibles", 0);
    }
}

inline void draw() {
    if (previous_draw) previous_draw();
    draw_collectibles();
    const bool down = guide_hotkey_held.load();
    if (!enabled.load()) return;
    if ((!layout_edit && gbarecomp::config_ui_visible()) ||
        (!layout_edit && (!visible || (hold && !down)))) return;
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    const float min_x = vp->WorkPos.x;
    const float min_y = vp->WorkPos.y;
    const float max_width = vp->WorkSize.x;
    if (!layout_initialized) {
        layout_x = std::clamp(layout_x, 0, std::max(0, int(vp->WorkSize.x) - 40));
        layout_y = std::clamp(layout_y, 0, std::max(0, int(vp->WorkSize.y) - 40));
        ImGui::SetNextWindowPos(ImVec2(min_x + layout_x, min_y + layout_y), ImGuiCond_Always);
        const float min_width = std::min(160.0f, max_width);
        ImGui::SetNextWindowSize(ImVec2(std::clamp(float(layout_width), min_width, max_width), 0), ImGuiCond_Always);
        layout_initialized = true;
    }
    const int min_width_px = int(std::min(160.0f, max_width));
    const int max_width_px = std::max(min_width_px, int(max_width));
    layout_width = std::clamp(layout_width, min_width_px, max_width_px);
    const float min_window_width = layout_edit ? std::min(160.0f, max_width) : float(layout_width);
    const float max_window_width = layout_edit ? max_width : float(layout_width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_window_width, 0),
        ImVec2(max_window_width, vp->WorkSize.y));
    ImGui::SetNextWindowBgAlpha(layout_edit ? 0.92f : 0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!layout_edit) flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
    else flags |= ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Adventure Guide##AdventureObjective", nullptr, flags)) {
        layout_window_focused = layout_edit && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const float wrap_width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        if (guide_mode.load()) {
            adventure_ui::draw_text(walkthrough::completionist.load() ? "100% walkthrough" : "Regular walkthrough", wrap_width, font_scale,
                IM_COL32(255, 216, 110, 255));
            const GuideStep* hint = current_hint.load();
            const auto route = walkthrough::current.load();
            if (!progress_ready.load()) {
                adventure_ui::draw_text("Waiting for game progress", wrap_width, font_scale);
            } else if (!hint && !(route && route->action)) {
                adventure_ui::draw_text(
                    "Guide steps for this location are not mapped yet. Open F1 > Game for the quest log.",
                    wrap_width, font_scale);
            } else if (hint) {
                if (hint->area && *hint->area)
                    adventure_ui::draw_text(hint->area, wrap_width, font_scale,
                        IM_COL32(255, 255, 255, 255));
                adventure_ui::draw_text("Next action", wrap_width, font_scale,
                    IM_COL32(255, 216, 110, 255));
                adventure_ui::draw_text(hint->objective, wrap_width, font_scale);
            } else {
                adventure_ui::draw_text(route->section->title, wrap_width, font_scale);
                adventure_ui::draw_text("Next action", wrap_width, font_scale,
                    IM_COL32(255, 216, 110, 255));
                adventure_ui::draw_text(route->action->objective, wrap_width, font_scale);
            }

        } else {
            adventure_ui::draw_text(tracker_objective(), wrap_width, font_scale);
            if (progress_ready.load()) {
                char djinn_line[40];
                std::snprintf(djinn_line, sizeof djinn_line, "Djinn: %u / 28", collected_djinn.load());
                adventure_ui::draw_text(djinn_line, wrap_width, font_scale,
                    IM_COL32(255, 255, 255, 255));
            }
        }
        if (layout_edit) {
            adventure_ui::draw_text("Drag to move; resize from a corner", wrap_width, font_scale,
                IM_COL32(255, 216, 110, 255));
            if (ImGui::Button("Finish layout")) layout_edit = false;
        }
        const float target_height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
        const float target_width = layout_edit ? ImGui::GetWindowWidth() : float(layout_width);
        const ImVec2 current_pos = ImGui::GetWindowPos();
        const int clamped_x = std::clamp(int(current_pos.x - min_x), 0,
            std::max(0, int(vp->WorkSize.x - target_width)));
        const int clamped_y = std::clamp(int(current_pos.y - min_y), 0,
            std::max(0, int(vp->WorkSize.y - target_height)));
        if (clamped_x != int(current_pos.x - min_x) || clamped_y != int(current_pos.y - min_y))
            ImGui::SetWindowPos(ImVec2(min_x + clamped_x, min_y + clamped_y));
        layout_x = clamped_x; layout_y = clamped_y;
        if (layout_edit) {
            layout_width = int(target_width);
        }
        persist_layout();
        ImGui::SetWindowSize(ImVec2(target_width, target_height));
    }
    ImGui::End();
}
#endif
// Quest Tracker, Guide and Area collectibles are unfinished: developer
// sessions only (developer_session.h). A player build registers no hotkeys,
// no F1 Game page and ignores saved values, so all of it stays off.
inline void init() {
    if (!gbarecomp::developer_session()) return;
    collectibles_enabled.store(gbarecomp::game_option_get("AdventureCollectibles", 0) != 0);
    enabled.store(gbarecomp::game_option_get("AdventureGuide", 0) != 0);
    guide_mode.store(gbarecomp::game_option_get("AdventureGuideMode", 1) != 0);
    walkthrough::completionist.store(gbarecomp::game_option_get("AdventureCompletionist", 1) != 0);
    recording.store(gbarecomp::game_option_get("AdventureRecord", 0) != 0);
    hold = gbarecomp::game_option_get("AdventureHold", 0) != 0;
    visible = gbarecomp::game_option_get("AdventureVisible", 1) != 0;
    layout_x = gbarecomp::game_option_get("AdventureX", 16);
    layout_y = gbarecomp::game_option_get("AdventureY", 16);
    layout_width = gbarecomp::game_option_get("AdventureWidth", 400);
    saved_layout_x = layout_x; saved_layout_y = layout_y; saved_layout_width = layout_width;
    font_scale = std::clamp(gbarecomp::game_option_get("AdventureScale", 2), 1, 3);
#ifdef GBARECOMP_HAVE_IMGUI
    // An older guide key becomes the default only when the shared binding has
    // no saved value. Existing [KeyMap] settings always override this default.
    const int old_key = gbarecomp::game_option_get("AdventureKey", 0);
    const char* old_name = old_key > 0 && old_key < SDL_NUM_SCANCODES ?
        SDL_GetScancodeName(static_cast<SDL_Scancode>(old_key)) : "";
    auto& shortcuts = gbarecomp::game_hotkeys();
    shortcuts.push_back({"AdventureGuideToggle", "Guide on/off", "", [] {
        const bool on = !enabled.load();
        enabled.store(on); gbarecomp::game_option_set("AdventureGuide", on);
        if (!on) layout_edit = false;
    }});
    shortcuts.push_back({"AdventureGuideVisible", "Guide show/hide", old_name, [] {
        if (!hold) { visible = !visible; gbarecomp::game_option_set("AdventureVisible", visible); }
    }, [](bool down) { guide_hotkey_held.store(down); }});
    shortcuts.push_back({"AdventureCollectibles", "Collectibles show/hide", "", [] {
        const bool on = !collectibles_enabled.load();
        collectibles_enabled.store(on); gbarecomp::game_option_set("AdventureCollectibles", on);
        if (!on) collectibles_edit = false;
    }});
#endif
#ifdef GBARECOMP_HAVE_IMGUI
    previous_draw = gbarecomp::g_config_ui_extra_draw;
    previous_wants_keyboard = gbarecomp::g_config_ui_extra_wants_keyboard;
    gbarecomp::g_config_ui_extra_draw = draw;
    gbarecomp::g_config_ui_extra_wants_keyboard = wants_keyboard;
    gbarecomp::config_ui_set_game_page(page);
#endif
}
} // namespace gsr::adventure
