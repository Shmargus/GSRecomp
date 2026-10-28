// host_config_ui.cpp — see host_config_ui.h.

#include "host_config_ui.h"
#include "game_options.h"
#include "host_window.h"
#include "view_config.h"
#include "developer_session.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>

namespace gbarecomp {
// Defined unconditionally (see host_config_ui.h) so a game runner can assign
// it the same way in every build configuration.
void (*g_config_ui_extra_draw)() = nullptr;
bool (*g_config_ui_extra_wants_keyboard)() = nullptr;
namespace {
const GameOptionSwitch* g_game_switches = nullptr;
int g_game_switch_count = 0;
void (*g_game_page_draw)() = nullptr;
std::mutex g_menu_art_mutex;
std::array<uint8_t, 256 * 32> g_menu_tiles{};
std::array<uint8_t, 224> g_menu_widths{};
std::array<uint16_t, 16> g_menu_palette{};
bool g_menu_art_pending = false;
bool g_open_cheats_requested = false;
}  // namespace
void config_ui_set_game_page(void (*draw)()) { g_game_page_draw = draw; }
void config_ui_set_game_switches(const GameOptionSwitch* switches, int count) {
    g_game_switches = switches;
    g_game_switch_count = switches ? count : 0;
}
void config_ui_set_menu_art(const uint8_t* tiles, const uint8_t* widths,
                            const uint16_t* palette) {
    if (!tiles || !widths || !palette) return;
    std::lock_guard<std::mutex> lock(g_menu_art_mutex);
    std::memcpy(g_menu_tiles.data(), tiles, g_menu_tiles.size());
    std::memcpy(g_menu_widths.data(), widths, g_menu_widths.size());
    std::memcpy(g_menu_palette.data(), palette, sizeof(g_menu_palette));
    g_menu_art_pending = true;
}
void config_ui_open_cheats() {
    std::lock_guard<std::mutex> lock(g_menu_art_mutex);
    g_open_cheats_requested = true;
    config_ui_set_visible(true);
}
}  // namespace gbarecomp

#ifdef GBARECOMP_HAVE_IMGUI

#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "imgui_impl_opengl3.h"
#include "frame_timing.h"
#include "temporal_blend.h"

namespace gbarecomp {

namespace {

bool g_ready = false;
bool g_visible = false;
int g_selected_page = 0;
SDL_Renderer* g_renderer = nullptr;
// The MAIN window and its GL context, captured at config_ui_init() (opengl
// path only). SHUTDOWN-FREEZE-01: with multi-viewport, ImGui may leave a
// torn-off viewport's GL context current at exit; shutdown makes the MAIN
// context current again first, or ImGui_ImplOpenGL3_Shutdown() deletes GL
// objects against the wrong context and DestroyPlatformWindows() can destroy
// the context that owns them.
SDL_Window* g_window = nullptr;
SDL_GLContext g_gl_context = nullptr;
// Which ImGui rendering backend is live, set once at config_ui_init() from
// host_window's SDL_GetRendererInfo check. Routes config_ui_draw() and
// config_ui_shutdown() between imgui_impl_opengl3 (multi-viewport) and
// imgui_impl_sdlrenderer2 (docking only).
bool g_use_opengl = false;
SDL_Texture* g_badge_sdl_texture = nullptr;
GLuint g_badge_gl_texture = 0;
ImVec2 g_badge_size;
char g_badge_caption[256] = {};
char g_badge_flourish[256] = {};
SDL_Texture* g_menu_sdl_texture = nullptr;
GLuint g_menu_gl_texture = 0;
bool g_menu_art_ready = false;
bool g_scroll_to_selected_tab = false;
const char* g_context_help = nullptr;
int g_help_page = -1;
int g_video_section = 0;
bool g_setting_label_hovered = false;
bool g_help_panel_visible = true;
bool g_fit_default_window = false;
float g_default_fit_height = 0;
const ImVec4 kGold      (0.93f, 0.76f, 0.30f, 1.00f);
const ImVec4 kParchment (0.96f, 0.93f, 0.84f, 1.00f);
const char* const kNavigationHelp =
    "L/R or PgUp/PgDn: Categories\nArrows/D-pad: Move\nEsc/B: Back   Enter/A: Select";

void destroy_menu_texture() {
    if (g_menu_gl_texture) glDeleteTextures(1, &g_menu_gl_texture);
    if (g_menu_sdl_texture) SDL_DestroyTexture(g_menu_sdl_texture);
    g_menu_gl_texture = 0;
    g_menu_sdl_texture = nullptr;
}

bool upload_menu_texture(const uint8_t* tiles, const uint16_t* palette) {
    std::array<uint8_t, 128 * 128 * 4> rgba{};
    for (int tile = 0; tile < 256; ++tile) {
        const int tx = (tile & 15) * 8, ty = (tile >> 4) * 8;
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            const int pixel = y * 8 + x;
            const uint8_t packed = tiles[tile * 32 + pixel / 2];
            const unsigned index = (pixel & 1) ? packed >> 4 : packed & 15;
            const uint16_t c = palette[index];
            const size_t out = (static_cast<size_t>(ty + y) * 128 + tx + x) * 4;
            rgba[out + 0] = static_cast<uint8_t>(((c & 31) << 3) | ((c & 31) >> 2));
            rgba[out + 1] = static_cast<uint8_t>((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
            rgba[out + 2] = static_cast<uint8_t>((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
            rgba[out + 3] = index ? 255 : 0;
        }
    }
    destroy_menu_texture();
    if (g_use_opengl) {
        GLint old_texture = 0, old_alignment = 0, old_row_length = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);
        glGenTextures(1, &g_menu_gl_texture);
        glBindTexture(GL_TEXTURE_2D, g_menu_gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // SDL leaves its last upload's row stride in the shared GL context.
        // This atlas is tightly packed; using the game texture's stride reads
        // past rgba and corrupts the menu's frame and background.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_length);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
        return g_menu_gl_texture != 0;
    }
    g_menu_sdl_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC, 128, 128);
    if (!g_menu_sdl_texture) return false;
    SDL_SetTextureScaleMode(g_menu_sdl_texture, SDL_ScaleModeNearest);
    SDL_SetTextureBlendMode(g_menu_sdl_texture, SDL_BLENDMODE_BLEND);
    return SDL_UpdateTexture(g_menu_sdl_texture, nullptr, rgba.data(), 128 * 4) == 0;
}

ImTextureID menu_texture_id() {
    return g_use_opengl ? static_cast<ImTextureID>(g_menu_gl_texture)
        : static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_menu_sdl_texture));
}

// One palette for all standard widgets, including game-owned F1 controls.
// Opaque native panels keep the running game's artwork out of the controls.
void apply_menu_colors(ImVec4 deep, ImVec4 blue, ImVec4 light,
                       ImVec4 silver, ImVec4 text, ImVec4 shadow) {
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = silver;
    c[ImGuiCol_WindowBg] = deep;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = deep;
    c[ImGuiCol_Border] = silver;
    c[ImGuiCol_BorderShadow] = shadow;
    for (ImGuiCol role : {ImGuiCol_FrameBg, ImGuiCol_Button,
                         ImGuiCol_TitleBg, ImGuiCol_TitleBgCollapsed,
                         ImGuiCol_MenuBarBg, ImGuiCol_ScrollbarBg,
                         ImGuiCol_Tab, ImGuiCol_TabUnfocused,
                         ImGuiCol_TableHeaderBg}) c[role] = deep;
    for (ImGuiCol role : {ImGuiCol_FrameBgHovered, ImGuiCol_ButtonHovered,
                         ImGuiCol_Header, ImGuiCol_TitleBgActive,
                         ImGuiCol_ScrollbarGrab, ImGuiCol_SliderGrab,
                         ImGuiCol_TabUnfocusedActive}) c[role] = blue;
    for (ImGuiCol role : {ImGuiCol_FrameBgActive, ImGuiCol_ButtonActive,
                         ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive,
                         ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ScrollbarGrabActive,
                         ImGuiCol_SliderGrabActive, ImGuiCol_TabHovered,
                         ImGuiCol_TabActive, ImGuiCol_TextSelectedBg,
                         ImGuiCol_DockingPreview}) c[role] = light;
    c[ImGuiCol_CheckMark] = text;
    c[ImGuiCol_NavHighlight] = text;
    c[ImGuiCol_Separator] = silver;
    c[ImGuiCol_SeparatorHovered] = text;
    c[ImGuiCol_SeparatorActive] = text;
    c[ImGuiCol_ResizeGrip] = silver;
    c[ImGuiCol_ResizeGripHovered] = light;
    c[ImGuiCol_ResizeGripActive] = text;
    c[ImGuiCol_TableBorderStrong] = silver;
    c[ImGuiCol_TableBorderLight] = blue;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(text.x, text.y, text.z, 0.04f);
}

void apply_pending_menu_art() {
    std::array<uint8_t, 256 * 32> tiles{};
    std::array<uint8_t, 224> widths{};
    std::array<uint16_t, 16> palette{};
    {
        std::lock_guard<std::mutex> lock(g_menu_art_mutex);
        if (!g_menu_art_pending) return;
        tiles = g_menu_tiles;
        widths = g_menu_widths;
        palette = g_menu_palette;
        g_menu_art_pending = false;
    }
    g_menu_art_ready = upload_menu_texture(tiles.data(), palette.data());
    auto palette_color = [&](int i) {
        const unsigned c = palette[i];
        return ImVec4((c & 31) / 31.0f, ((c >> 5) & 31) / 31.0f,
                      ((c >> 10) & 31) / 31.0f, 1.0f);
    };
    apply_menu_colors(palette_color(7), palette_color(4), palette_color(10),
                      palette_color(12), palette_color(1), palette_color(3));

    // ImGui's 1.91 atlas custom-glyph API is used to insert the ROM's 8x8
    // bitmaps, retaining the ROM palette's letter (1) and shadow (3) colors.
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = 8.0f;
    static const ImWchar no_glyphs[] = {0};
    cfg.GlyphRanges = no_glyphs;
    ImFont* font = atlas->AddFontDefault(&cfg);
    int rects[224];
    for (int i = 0; i < 224; ++i)
        rects[i] = atlas->AddCustomRectFontGlyph(font,
            static_cast<ImWchar>(32 + i), 8, 8,
            static_cast<float>(widths[i]), ImVec2(0, 0));
    atlas->Build();
    unsigned char* pixels = nullptr;
    int atlas_w = 0, atlas_h = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &atlas_w, &atlas_h);
    for (int i = 0; i < 224; ++i) {
        const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(rects[i]);
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            const int p = y * 8 + x;
            const uint8_t packed = tiles[(32 + i) * 32 + p / 2];
            const unsigned index = (p & 1) ? packed >> 4 : packed & 15;
            const size_t out = (static_cast<size_t>(r->Y + y) * atlas_w +
                                r->X + x) * 4;
            if (index == 1 || index == 3) {
                const uint16_t c = palette[index];
                pixels[out + 0] = static_cast<uint8_t>(((c & 31) << 3) | ((c & 31) >> 2));
                pixels[out + 1] = static_cast<uint8_t>((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
                pixels[out + 2] = static_cast<uint8_t>((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
                pixels[out + 3] = 255;
            } else {
                pixels[out + 3] = 0;
            }
        }
    }
    font->Scale = 2.0f;
    if (g_use_opengl) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
        GLint old_texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(atlas->TexID));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
    } else {
        ImGui_ImplSDLRenderer2_DestroyFontsTexture();
        ImGui_ImplSDLRenderer2_CreateFontsTexture();
        SDL_SetTextureScaleMode(reinterpret_cast<SDL_Texture*>(
            static_cast<intptr_t>(atlas->TexID)), SDL_ScaleModeNearest);
    }
}

void draw_menu_tile(ImDrawList* draw, int tile, ImVec2 pos, ImVec2 size,
                    ImU32 tint = IM_COL32_WHITE) {
    if (!g_menu_art_ready) return;
    const ImVec2 uv0((tile & 15) / 16.0f, (tile >> 4) / 16.0f);
    const ImVec2 uv1(uv0.x + 1.0f / 16.0f, uv0.y + 1.0f / 16.0f);
    draw->AddImage(menu_texture_id(), pos, ImVec2(pos.x + size.x, pos.y + size.y),
                   uv0, uv1, tint);
}

void draw_menu_frame(ImVec2 pos, ImVec2 size) {
    if (!g_menu_art_ready || size.x < 32.0f || size.y < 32.0f) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    // Border only: filling here would cover the translucent window beneath it.
    for (float x = pos.x + 16.0f; x < pos.x + size.x - 16.0f; x += 16.0f) {
        draw_menu_tile(draw, 17, ImVec2(x, pos.y), ImVec2(16, 16));
        draw_menu_tile(draw, 20, ImVec2(x, pos.y + size.y - 16), ImVec2(16, 16));
    }
    for (float y = pos.y + 16.0f; y < pos.y + size.y - 16.0f; y += 16.0f) {
        draw_menu_tile(draw, 22, ImVec2(pos.x, y), ImVec2(16, 16));
        draw_menu_tile(draw, 23, ImVec2(pos.x + size.x - 16, y), ImVec2(16, 16));
    }
    draw_menu_tile(draw, 16, pos, ImVec2(16, 16));
    draw_menu_tile(draw, 18, ImVec2(pos.x + size.x - 16, pos.y), ImVec2(16, 16));
    draw_menu_tile(draw, 19, ImVec2(pos.x, pos.y + size.y - 16), ImVec2(16, 16));
    draw_menu_tile(draw, 21, ImVec2(pos.x + size.x - 16,
        pos.y + size.y - 16), ImVec2(16, 16));
    draw->PopClipRect();
}

// Native 8x8 window pieces form each control, rather than an ImGui outline.
// Separate splitters allow these wrappers inside ImGui's own table channels.
void native_control_frame(ImDrawList* draw, ImVec2 lo, ImVec2 hi, ImU32 face,
                          float frame_edge = 8.0f) {
    draw->AddRectFilled(lo, hi, face);
    if (!g_menu_art_ready) return;
    const float edge = std::min(frame_edge, std::min(hi.x - lo.x, hi.y - lo.y) * 0.5f);
    const ImU32 tint = ImGui::GetColorU32(ImVec4(1, 1, 1, 1));
    const float w = hi.x - lo.x, h = hi.y - lo.y;
    draw_menu_tile(draw, 16, lo, ImVec2(edge, edge), tint);
    draw_menu_tile(draw, 18, ImVec2(hi.x-edge,lo.y), ImVec2(edge,edge), tint);
    draw_menu_tile(draw, 19, ImVec2(lo.x,hi.y-edge), ImVec2(edge,edge), tint);
    draw_menu_tile(draw, 21, ImVec2(hi.x-edge,hi.y-edge), ImVec2(edge,edge), tint);
    draw_menu_tile(draw, 17, ImVec2(lo.x+edge,lo.y), ImVec2(w-2*edge,edge), tint);
    draw_menu_tile(draw, 20, ImVec2(lo.x+edge,hi.y-edge), ImVec2(w-2*edge,edge), tint);
    draw_menu_tile(draw, 22, ImVec2(lo.x,lo.y+edge), ImVec2(edge,h-2*edge), tint);
    draw_menu_tile(draw, 23, ImVec2(hi.x-edge,lo.y+edge), ImVec2(edge,h-2*edge), tint);
}

bool native_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    const ImVec4 normal = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const ImVec4 hovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
    const ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    for (ImGuiCol role : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive})
        ImGui::PushStyleColor(role, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    const bool focus = ImGui::IsItemFocused();
    const ImU32 face = ImGui::GetColorU32(ImGui::IsItemActive() ? active :
        (ImGui::IsItemHovered() || focus) ? hovered : normal);
    layers.SetCurrentChannel(draw, 0);
    native_control_frame(draw, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), face);
    layers.Merge(draw);
    return pressed;
}

// One row at every width. Mouse scrolling, navigation focus and explicit
// category changes all reveal the selected tab without reducing the font.
void tab_strip(const char* id, const char* const* names, int count,
               int& selected, bool reveal = false) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = 0;
    for (int i = 0; i < count; ++i)
        width += ImGui::CalcTextSize(names[i]).x + style.FramePadding.x * 2 +
            (i ? style.ItemSpacing.x : 0);
    const float height = ImGui::GetFrameHeight() +
        (width > ImGui::GetContentRegionAvail().x ? style.ScrollbarSize : 0);
    if (ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_NavFlattened,
                         ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int i = 0; i < count; ++i) {
            if (i) ImGui::SameLine();
            const bool active = selected == i;
            ImGui::PushStyleColor(ImGuiCol_Button, active
                ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, kGold);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            const bool pressed = active ? native_button(names[i]) : ImGui::Button(names[i]);
            if (pressed) selected = i;
            ImGui::PopStyleVar();
            if (active && reveal) ImGui::SetScrollHereX();
            const bool show_help = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
            ImGui::PopStyleColor(active ? 2 : 1);
            if (show_help) ImGui::SetTooltip("%s", kNavigationHelp);
        }
    }
    ImGui::EndChild();
}

bool native_selectable(const char* label, bool selected) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    for (ImGuiCol role : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})
        ImGui::PushStyleColor(role, ImVec4(0,0,0,0));
    const bool pressed = ImGui::Selectable((std::string("##")+label).c_str(), selected,
                                           0, ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::PopStyleColor(3);
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const bool highlight = selected || ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const float padding = ImGui::GetStyle().FramePadding.x;
    const ImVec2 text_pos(lo.x + padding, lo.y + (hi.y-lo.y-ImGui::GetFontSize())*0.5f);
    if (highlight) draw->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), ">");
    draw->AddText(ImVec2(text_pos.x + ImGui::CalcTextSize("> ").x, text_pos.y),
                  ImGui::GetColorU32(ImGuiCol_Text), label);
    if (highlight) {
        layers.SetCurrentChannel(draw, 0);
        native_control_frame(draw, lo, hi, ImGui::GetColorU32(ImGuiCol_HeaderActive));
    }
    layers.Merge(draw);
    return pressed;
}

bool native_combo(const char* id, const char* value) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const ImVec2 hi(lo.x + ImGui::CalcItemWidth(), lo.y + ImGui::GetFrameHeight());
    ImDrawListSplitter layers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    const bool open = ImGui::BeginCombo(id, nullptr, ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    const float padding = ImGui::GetStyle().FramePadding.x;
    const float y = lo.y + (hi.y-lo.y-ImGui::GetFontSize())*0.5f;
    const float arrow = ImGui::CalcTextSize(">").x;
    draw->PushClipRect(lo, ImVec2(hi.x-padding-arrow-padding,hi.y), true);
    draw->AddText(ImVec2(lo.x+padding,y), ImGui::GetColorU32(ImGuiCol_Text), value);
    draw->PopClipRect();
    draw->AddText(ImVec2(hi.x-padding-arrow,y), ImGui::GetColorU32(ImGuiCol_Text), ">");
    layers.SetCurrentChannel(draw, 0);
    native_control_frame(draw, lo, hi, ImGui::GetColorU32(open ? ImGuiCol_FrameBgActive : ImGuiCol_FrameBg));
    layers.Merge(draw);
    if (open) {
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        native_control_frame(ImGui::GetWindowDrawList(), pos,
            ImVec2(pos.x+size.x,pos.y+size.y), ImGui::GetColorU32(ImGuiCol_PopupBg));
    }
    return open;
}

template <typename T>
bool native_slider(const char* label, T* value, T lo, T hi, const char* format) {
    char text[64], low[64], high[64];
    std::snprintf(text, sizeof(text), format, *value);
    std::snprintf(low, sizeof(low), format, lo);
    std::snprintf(high, sizeof(high), format, hi);
    const float value_w = std::max({ImGui::CalcTextSize(text).x,
        ImGui::CalcTextSize(low).x, ImGui::CalcTextSize(high).x});
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float width = std::max(ImGui::GetFontSize(), ImGui::CalcItemWidth() - value_w - gap);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float center_y = start.y + ImGui::GetFrameHeight() / 2;
    const float edge = ImGui::GetFontSize() / 4;
    const ImGuiID id = ImGui::GetCurrentWindow()->GetID(label);
    const bool editing = ImGui::TempInputIsActive(id) ||
        (ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         ImGui::IsMouseHoveringRect(start, ImVec2(start.x + width, start.y + ImGui::GetFrameHeight()))) ||
        (GImGui->NavActivateId == id && (GImGui->NavActivateFlags & ImGuiActivateFlags_PreferInput));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    // Cache the opaque thumb color before hiding the slider's built-in text.
    const ImVec4 active_grab = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!editing) {
        for (ImGuiCol role : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered,
                             ImGuiCol_FrameBgActive, ImGuiCol_Text})
            ImGui::PushStyleColor(role, ImVec4(0, 0, 0, 0));
    }
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,
        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
        active_grab);
    ImGui::SetNextItemWidth(width);
    if (!editing) draw->PushClipRect(ImVec2(start.x, center_y - edge),
        ImVec2(start.x + width, center_y + edge), true);
    bool changed;
    if constexpr (std::is_same_v<T, int>)
        changed = ImGui::SliderInt(label, value, lo, hi, format);
    else
        changed = ImGui::SliderFloat(label, value, lo, hi, format);
    if (!editing) draw->PopClipRect();
    ImGui::PopStyleColor(editing ? 2 : 6);
    std::snprintf(text, sizeof(text), format, *value);
    draw->AddText(ImVec2(start.x + width + gap, center_y - ImGui::GetFontSize() / 2),
        ImGui::GetColorU32(ImGuiCol_Text), text);
    layers.SetCurrentChannel(draw, 0);
    native_control_frame(draw, ImVec2(start.x, center_y - edge * 2),
        ImVec2(start.x + width, center_y + edge * 2),
        ImGui::GetColorU32(ImGuiCol_FrameBg), edge);
    layers.Merge(draw);
    return changed;
}

// Real checkbox input with a smaller square made from the game's frame tiles.
// Keep the normal hit area and row height for mouse/controller accessibility.
bool native_checkbox(const char* id, bool* value) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    for (ImGuiCol role : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered,
                         ImGuiCol_FrameBgActive, ImGuiCol_CheckMark})
        ImGui::PushStyleColor(role, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    const bool changed = ImGui::Checkbox(id, value);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    const ImVec2 start = ImGui::GetItemRectMin();
    const float edge = ImGui::GetFontSize() / 4;
    const float size = ImGui::GetFontSize() + edge * 2;
    const float inset = (ImGui::GetFrameHeight() - size) / 2;
    const ImVec2 lo(start.x + inset, start.y + inset), hi(lo.x + size, lo.y + size);
    const bool focused = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    layers.SetCurrentChannel(draw, 0);
    native_control_frame(draw, lo, hi,
        ImGui::GetColorU32(focused ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), edge);
    layers.SetCurrentChannel(draw, 1);
    if (*value) ImGui::RenderCheckMark(draw, ImVec2(lo.x + edge, lo.y + edge),
        ImGui::GetColorU32(ImGuiCol_CheckMark), ImGui::GetFontSize());
    layers.Merge(draw);
    return changed;
}

// Draw a full native panel behind its contents without adding another scroll
// window or changing widget IDs. The existing page remains the scroll owner.
struct SettingsPanel {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* draw = window->DrawList;
    ImDrawListSplitter layers;
    ImVec2 start = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float right = window->WorkRect.Max.x;
    float padding = ImGui::GetFontSize();
    explicit SettingsPanel(const char* title) {
        layers.Split(draw, 2);
        layers.SetCurrentChannel(draw, 1);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, padding));
        ImGui::Indent(padding);
        window->WorkRect.Max.x -= padding;
        ImGui::TextColored(kGold, "%s", title);
        ImGui::Spacing();
    }
    ~SettingsPanel() {
        ImGui::Unindent(padding);
        ImGui::Dummy(ImVec2(0, padding));
        ImGui::EndGroup();
        const float bottom = ImGui::GetItemRectMax().y;
        window->WorkRect.Max.x = right;
        layers.SetCurrentChannel(draw, 0);
        native_control_frame(draw, start, ImVec2(start.x + width, bottom),
            ImGui::GetColorU32(ImGuiCol_Header));
        layers.Merge(draw);
    }
};

void clear_edition_badge() {
    if (g_badge_gl_texture) glDeleteTextures(1, &g_badge_gl_texture);
    if (g_badge_sdl_texture) SDL_DestroyTexture(g_badge_sdl_texture);
    g_badge_gl_texture = 0;
    g_badge_sdl_texture = nullptr;
    g_badge_caption[0] = g_badge_flourish[0] = '\0';
}

// Which bind box is armed, if any. Exactly one capture can be live at a time,
// and while it is, every key/pad press is swallowed and consumed as the bind.
// HotkeyPad is the controller half of a hotkey row (Hotkey is its keyboard
// half: same row, two independent bind boxes).
enum class Capture { None, Key, Pad, Hotkey, HotkeyPad };
Capture g_capture = Capture::None;
int     g_capture_index = -1;
int     g_capture_pad_slot = 0;
bool    g_config_window_recover = false;

// GBA buttons in controller order rather than KEYINPUT bit order, with the
// bit each row maps to. Reads like a pad, not like a hardware register.
const struct { const char* label; int bit; } kButtons[] = {
    { "D-Pad Up",    6 }, { "D-Pad Down",  7 },
    { "D-Pad Left",  5 }, { "D-Pad Right", 4 },
    { "A",           0 }, { "B",           1 },
    { "L",           9 }, { "R",           8 },
    { "Start",       3 }, { "Select",      2 },
};
constexpr int kButtonCount = static_cast<int>(sizeof(kButtons) /
                                              sizeof(kButtons[0]));

const char* key_label(int scancode) {
    if (scancode == 0) return "(unset)";
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return (n && *n) ? n : "(unset)";
}

// Guards the literal in host_config_ui.h against SDL's real enum value.
static_assert(kPadTriggerLeftValue == static_cast<int>(SDL_CONTROLLER_BUTTON_MAX),
             "kPadTriggerLeftValue must track SDL_CONTROLLER_BUTTON_MAX so the "
             "synthetic L2/R2 ids never collide with a real pad button");

const char* pad_label(int button) {
    if (button < 0) return "(unset)";
    // L2/R2 and the stick directions are synthetic ids, not an
    // SDL_GameControllerButton; label them from the shared name table
    // ("lefttrigger", "leftstickup", ...), which uses the same lowercase style
    // as SDL's button strings ("leftshoulder", "dpup").
    if (const char* s = pad_synth_name(button)) return s;
    const char* n = SDL_GameControllerGetStringForButton(
        static_cast<SDL_GameControllerButton>(button));
    return (n && *n) ? n : "(unset)";
}

// Hotkeys carry modifiers, so they render as "Shift+P" rather than a bare key.
void hotkey_label(char* out, std::size_t n, int keycode, unsigned mods) {
    if (keycode == 0) { std::snprintf(out, n, "(unset)"); return; }
    char buf[96];
    buf[0] = '\0';
    if (mods & KMOD_CTRL)  std::strncat(buf, "Ctrl+",  sizeof(buf) - 1);
    if (mods & KMOD_ALT)   std::strncat(buf, "Alt+",   sizeof(buf) - 1);
    if (mods & KMOD_SHIFT) std::strncat(buf, "Shift+", sizeof(buf) - 1);
    const char* kn = SDL_GetKeyName(static_cast<SDL_Keycode>(keycode));
    std::snprintf(out, n, "%s%s", buf, (kn && *kn) ? kn : "?");
}

// Keep explanations in the fixed help panel, for mouse and nav focus alike.
void caption(const char* text) {
    if (g_setting_label_hovered ||
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ||
        ImGui::IsItemFocused() || ImGui::IsItemActive()) {
        g_context_help = text;
        if (!g_help_panel_visible) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                std::min(ImGui::CalcTextSize("Mute while fast forwarding").x,
                         ImGui::GetMainViewport()->WorkSize.x - ImGui::GetStyle().WindowPadding.x * 2));
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    g_setting_label_hovered = false;
}

// Every setting uses the same label/value columns. Width follows the actual
// label measurements and available space; long labels wrap on small windows.
void setting_label(const char* label, float minimum_value = 0) {
    const float start = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float value_min = minimum_value > 0 ? minimum_value :
        ImGui::CalcTextSize("3:2 (original) >").x + ImGui::GetStyle().FramePadding.x * 2;
    const float column = std::min(
        ImGui::CalcTextSize("Mute while fast forwarding").x +
            ImGui::GetStyle().ItemInnerSpacing.x * 2,
        available * 0.46f);
    const bool inline_value = available - column >= value_min;
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(start + (inline_value
        ? column - ImGui::GetStyle().ItemInnerSpacing.x : available));
    ImGui::TextUnformatted(label);
    g_setting_label_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::PopTextWrapPos();
    if (inline_value) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + column);
        ImGui::SetNextItemWidth(available - column);
    } else {
        // Keep values usable on narrow windows; give labels their own line.
        ImGui::SetNextItemWidth(available);
    }
}

int dpad_slider_step();

bool setting_checkbox(const char* label, bool* value) {
    setting_label(label, ImGui::GetFrameHeight());
    const std::string id = std::string("###") + label;
    bool changed = native_checkbox(id.c_str(), value);
    if (g_setting_label_hovered && !(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        *value = !*value;
        changed = true;
    }
    int direction = dpad_slider_step();
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        const ImGuiID owner = ImGui::GetItemID();
        ImGui::SetKeyOwner(ImGuiKey_LeftArrow, owner);
        ImGui::SetKeyOwner(ImGuiKey_RightArrow, owner);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, owner)) direction=-1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, owner)) direction=1;
    }
    if (direction && *value != (direction>0)) { *value=direction>0; changed=true; }
    if (g_setting_label_hovered || ImGui::IsItemFocused() ||
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        g_context_help = *value ? "Checked. Select to clear."
                                : "Unchecked. Select to enable.";
    return changed;
}

bool setting_combo(const char* label, const char* value) {
    setting_label(label);
    return native_combo((std::string("##") + label).c_str(), value);
}

bool setting_items(const char* label, int* value, const char* items) {
    const char* current = items;
    for (int i=0; i<*value && *current; ++i) current += std::strlen(current)+1;
    bool changed = false;
    if (setting_combo(label, current)) {
        int i = 0;
        for (const char* item=items; *item; item+=std::strlen(item)+1,++i) {
            if (native_selectable(item, i==*value)) { *value=i; changed=true; }
            if (i==*value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void section(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
}

// A bind row: label on the left, a button showing the current binding that
// arms capture when clicked, and a clear button. Returns true if changed.
// `column` places the value/clear pair at a table column other than 1/2
// (draw_hotkeys_page puts the pad half of a row at columns 3/4, beside the
// keyboard half at 1/2).
bool bind_row(const char* label, const char* value, bool armed,
              Capture kind, int index, bool* cleared, int column = 0) {
    const bool is_pad = (kind == Capture::Pad || kind == Capture::HotkeyPad);
    ImGui::PushID(index * 8 + static_cast<int>(kind));
    if (column == 0) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
    } else {
        ImGui::TableSetColumnIndex(column);
    }

    bool clicked = false;
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        clicked = native_button(is_pad ? "press a button..." : "press a key...",
                                ImVec2(-FLT_MIN, 0));
        ImGui::PopStyleColor();
    } else {
        clicked = native_button(value, ImVec2(-FLT_MIN, 0));
    }
    ImGui::TableSetColumnIndex(column == 0 ? 2 : column + 1);
    *cleared = native_button(is_pad ? "clear##p" : "clear##k");
    ImGui::PopID();
    return clicked;
}

// D-pad Left/Right change a nav-focused (not yet activated) slider directly.
// Call right after the slider. Returns -1 / +1 per press (with key repeat),
// else 0. The keys are claimed so nav does not also move focus sideways.
int dpad_slider_step() {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return 0;
    const ImGuiID id = ImGui::GetItemID();
    ImGui::SetKeyOwner(ImGuiKey_GamepadDpadLeft, id);
    ImGui::SetKeyOwner(ImGuiKey_GamepadDpadRight, id);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, id))
        return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, id))
        return 1;
    return 0;
}

// What a slider did this frame: `changed` on any new value (drag or D-pad),
// `committed` when the player let go of a drag or stepped with the D-pad.
struct SliderEdit {
    bool changed = false;
    bool committed = false;
};

// The D-pad half of a slider; call right after the ImGui slider.
template <typename T>
SliderEdit finish_slider(bool dragged, T* value, T lo, T hi, T step) {
    SliderEdit edit{dragged, ImGui::IsItemDeactivatedAfterEdit()};
    if (const int dir = dpad_slider_step()) {
        const T v = std::clamp(static_cast<T>(*value + step * dir), lo, hi);
        if (v != *value) {
            *value = v;
            edit.changed = edit.committed = true;
        }
    }
    return edit;
}

SliderEdit slider_int(const char* label, int* value, int lo, int hi, const char* format) {
    setting_label(label);
    return finish_slider(native_slider((std::string("##") + label).c_str(), value, lo, hi, format), value, lo, hi, 1);
}

SliderEdit slider_float(const char* label, float* value, float lo, float hi, float step, const char* format) {
    setting_label(label);
    return finish_slider(native_slider((std::string("##") + label).c_str(), value, lo, hi, format), value, lo, hi, step);
}

void draw_controls_page(ConfigUiState* st) {
    section("Buttons");
    caption(st->stick_bindings_available
        ? "Use Controller and Alternate for D-pad and stick together. Click a box to bind; Esc cancels."
        : "Click a box, then press the key or controller button to use. Esc cancels.");

    const ImGuiStyle& style = ImGui::GetStyle();
    float button_label_w = 0;
    for (const auto& button : kButtons)
        button_label_w = std::max(button_label_w, ImGui::CalcTextSize(button.label).x);
    const float clear_w = ImGui::CalcTextSize("x").x + style.FramePadding.x * 2;
    const float bind_w = ImGui::CalcTextSize("press a button...").x + style.FramePadding.x * 2;
    const bool stacked = ImGui::GetContentRegionAvail().x <
        button_label_w + (bind_w + clear_w) * 3 + style.CellPadding.x * 14;
    // Both layouts use identical capture and clear handlers.
    auto binding = [&](int bit, int slot) {
        ImGui::PushID(slot);
        const bool keyboard = slot < 0;
        const Capture capture = keyboard ? Capture::Key : Capture::Pad;
        const bool armed = g_capture == capture && g_capture_index == bit &&
            (keyboard || g_capture_pad_slot == slot);
        int& value = keyboard ? st->key_bind[bit] :
            (slot == 0 ? st->pad_bind : st->pad_bind_alt)[bit];
        const int column = stacked ? 1 : (keyboard ? 1 : 3 + slot * 2);
        if (stacked) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(keyboard ? "Keyboard" : slot == 0 ? "Controller" : "Alternate");
        }
        ImGui::TableSetColumnIndex(column);
        if (armed) ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (native_button(armed ? (keyboard ? "press a key..." : "press a button...")
                                 : (keyboard ? key_label(value) : pad_label(value)),
                          ImVec2(-FLT_MIN, 0))) {
            g_capture = armed ? Capture::None : capture;
            g_capture_index = bit;
            if (!keyboard) g_capture_pad_slot = slot;
        }
        if (armed) ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(column + 1);
        if (native_button("x")) {
            value = keyboard ? 0 : -1;
            st->binds_changed = true;
        }
        ImGui::PopID();
    };
    if (stacked) {
        for (int i = 0; i < kButtonCount; ++i) {
            ImGui::PushID(i);
            section(kButtons[i].label);
            if (ImGui::BeginTable("binding", 3, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed,
                    ImGui::CalcTextSize("Controller").x);
                ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, clear_w);
                for (int slot = -1; slot < 2; ++slot) binding(kButtons[i].bit, slot);
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    } else if (ImGui::BeginTable("binds", 7,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, button_label_w);
        for (const char* name : {"Keyboard", "Controller", "Alternate"}) {
            ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, clear_w);
        }
        ImGui::TableHeadersRow();
        for (int i = 0; i < kButtonCount; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kButtons[i].label);
            for (int slot = -1; slot < 2; ++slot) binding(kButtons[i].bit, slot);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (native_button("Restore defaults")) {
        static const int kDefaultKeys[10] = {
            SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT,
            SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V,
            SDL_SCANCODE_C,
        };
        static const int kDefaultPads[10] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
            SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
            SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        };
        for (int i = 0; i < 10; ++i) {
            st->key_bind[i] = kDefaultKeys[i];
            st->pad_bind[i] = kDefaultPads[i];
            st->pad_bind_alt[i] = -1;
        }
        st->binds_changed = true;
    }
}

void draw_hotkeys_page(ConfigUiState* st) {
    section("Hotkeys");
    caption("Shortcuts that work while playing. Each can have a key, a "
            "controller button, or both. Auto Fire presses A or B rapidly "
            "while held. Soft Reset restarts the game; unsaved progress is lost.");
    const ImGuiStyle& style = ImGui::GetStyle();
    float action_w = 0;
    for (int h = 0; h < st->hotkey_count; ++h)
        action_w = std::max(action_w,
            ImGui::CalcTextSize(st->hotkey_names ? st->hotkey_names[h] : "?").x);
    const float clear_w = ImGui::CalcTextSize("clear").x + style.FramePadding.x * 2;
    const float bind_w = ImGui::CalcTextSize("press a button...").x + style.FramePadding.x * 2;
    const bool stacked = ImGui::GetContentRegionAvail().x <
        action_w + (bind_w + clear_w) * 2 + style.CellPadding.x * 10;
    if (ImGui::BeginTable("hotkeys", stacked ? 3 : 5,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, clear_w);
        if (!stacked) {
            ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, clear_w);
            ImGui::TableHeadersRow();
        }
        for (int h = 0; h < st->hotkey_count; ++h) {
            if (stacked) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(kGold, "%s", st->hotkey_names ? st->hotkey_names[h] : "?");
                ImGui::PopTextWrapPos();
            }
            // Keyboard half.
            char label[128];
            hotkey_label(label, sizeof(label), st->hotkey_key[h],
                         st->hotkey_mods[h]);
            bool key_cleared = false;
            const bool key_armed =
                (g_capture == Capture::Hotkey && g_capture_index == h);
            if (bind_row(stacked ? "Keyboard" : (st->hotkey_names ? st->hotkey_names[h] : "?"),
                         label, key_armed, Capture::Hotkey, h, &key_cleared)) {
                if (key_armed) g_capture = Capture::None;
                else { g_capture = Capture::Hotkey; g_capture_index = h; }
            }
            if (key_cleared) {
                st->hotkey_key[h] = 0;
                st->hotkey_mods[h] = 0;
                st->binds_changed = true;
            }

            // Controller half: same row, columns 3/4.
            bool pad_cleared = false;
            const bool pad_armed =
                (g_capture == Capture::HotkeyPad && g_capture_index == h);
            if (bind_row(stacked ? "Controller" : "", pad_label(st->hotkey_pad[h]), pad_armed,
                         Capture::HotkeyPad, h, &pad_cleared, stacked ? 0 : 3)) {
                if (pad_armed) g_capture = Capture::None;
                else { g_capture = Capture::HotkeyPad; g_capture_index = h; }
            }
            if (pad_cleared) {
                st->hotkey_pad[h] = -1;
                st->binds_changed = true;
            }
        }
        ImGui::EndTable();
    }
}

float display_group_min_width() {
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::CalcTextSize("Sharp pixels").x +
        ImGui::CalcTextSize("3:2 (original) >").x +
        style.FramePadding.x * 2 + style.WindowPadding.x * 2 +
        style.ItemInnerSpacing.x;
}

void draw_video_page(ConfigUiState* st) {
    const char* names[] = {"Display", "Colours", "Effects"};
    const int previous = g_video_section;
    tab_strip("VideoSections", names, 3, g_video_section);
    if (previous != g_video_section) g_context_help = "Choose a setting for help.";
    if (g_video_section == 0) {
        // Two groups only when both can show their longest label and value.
        // This follows the actual font and padding, including display scaling.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float group_min = display_group_min_width();
        const bool side_by_side = ImGui::GetContentRegionAvail().x >=
            group_min * 2 + style.WindowPadding.x * 4;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
            ImVec2(style.FramePadding.x, 0));
        const bool groups = ImGui::BeginTable("DisplayGroups", side_by_side ? 2 : 1,
            ImGuiTableFlags_SizingStretchSame);
        if (groups) ImGui::TableNextColumn();
        {
            SettingsPanel panel("Window");
            // The slider edits a menu-local copy: host_window republishes st->scale
            // from the live window every frame, which would overwrite a dragged value
            // before release. The copy follows the window only while the slider is
            // idle, and the (expensive) resize is requested once, on release.
            static int pending_scale = 0;
            static bool scale_active = false;
            const int max_scale = std::clamp(st->max_scale, 1, 8);
            if (!scale_active) pending_scale = st->scale;
            pending_scale = std::clamp(pending_scale, 1, max_scale);
            if (st->fullscreen) ImGui::BeginDisabled();
            setting_label("Window size", ImGui::CalcTextSize("8x").x +
                ImGui::GetFontSize() * 2 + ImGui::GetStyle().ItemInnerSpacing.x);
            native_slider("##Window size", &pending_scale, 1, max_scale, "%dx");
            if (!st->fullscreen) {
                const int dir = dpad_slider_step();
                if (dir != 0) {
                    pending_scale = std::clamp(pending_scale + dir, 1, max_scale);
                    if (pending_scale != st->scale) {
                        st->scale = pending_scale;
                        st->video_changed = true;
                    }
                }
            }
            scale_active = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit() && pending_scale != st->scale) {
                st->scale = pending_scale;
                st->video_changed = true;
            }
            if (st->fullscreen) ImGui::EndDisabled();
            caption(st->fullscreen
                ? "Leave fullscreen to change the window size."
                : "Size of the game window. Applied when you let go of the slider.");
            if (setting_checkbox("Fullscreen", &st->fullscreen)) st->video_changed = true;
            if (setting_checkbox("Show FPS", &st->show_fps)) st->video_changed = true;
            caption("Shows frames drawn per second and game speed in the corner of "
                    "the screen. 100% is full speed.");
        }
        if (groups) ImGui::TableNextColumn();
        {
            SettingsPanel panel("Picture");
            const bool sharp_fill = st->aspect_view_available && st->widescreen &&
                                    st->aspect == ViewAspectMinimalSharpPixels;
            if (sharp_fill) ImGui::BeginDisabled();
            if (setting_checkbox("Sharp pixels", &st->integer_scale))
                st->video_changed = true;
            if (sharp_fill) ImGui::EndDisabled();
            caption(sharp_fill ? "Minimal Sharp Pixels uses whole-pixel scaling."
                               : "Keeps pixel sizes uniform. Off scales to fit the selected aspect ratio.");
            if (setting_checkbox("V-Sync", &st->vsync)) st->video_changed = true;
            caption("Matches the monitor's refresh to prevent tearing.");
            // Only changes the picture in Expanded View (a wider-than-3:2 view).
            const bool aspect_active = st->widescreen_available && st->widescreen;
            const int aspect_count = st->aspect_view_available ? kViewAspectCount : 2;
            const int aspect_choice = std::clamp(st->aspect, 0, aspect_count - 1);
            if (!aspect_active) ImGui::BeginDisabled();
            if (setting_combo("Aspect ratio", kViewAspectLabels[aspect_choice])) {
                for (int i = 0; i < aspect_count; ++i) {
                    if (native_selectable(kViewAspectLabels[i], i == aspect_choice)) {
                        st->aspect = i;
                        if (i == ViewAspectMinimalSharpPixels) st->integer_scale = true;
                        st->video_changed = true;
                    }
                    if (i == aspect_choice) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!aspect_active) ImGui::EndDisabled();
            caption(aspect_active
                ? (st->aspect_view_available
                    ? "Fits inside the selected view size. Window follows its shape; "
                      "Minimal Sharp Pixels adds only enough scenery to fill it."
                    : "4:3 shows the middle of the wide picture; 3:2 shows the full width.")
                : "Choose Wide or Extra Wide to change the aspect ratio.");
        }
        if (groups) ImGui::EndTable();
        ImGui::PopStyleVar();
    } else if (g_video_section == 1) {
        section("Colours");
        const bool colours_available = st->colour_choices && st->colour_choice_count > 0;
        int choice = 0;
        if (colours_available) {
            for (int i = 0; i < st->colour_choice_count; ++i)
                if (st->colour_choices[i].value == st->screen_kind) choice = i;
        }
        if (!colours_available) ImGui::BeginDisabled();
        if (setting_combo("Colours", colours_available
                             ? st->colour_choices[choice].label : "Not yet available")) {
            for (int i = 0; i < st->colour_choice_count; ++i) {
                if (native_selectable(st->colour_choices[i].label, i == choice)) {
                    st->screen_kind = st->colour_choices[i].value;
                    choice = i;
                    st->video_changed = true;
                }
                if (i == choice) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (!colours_available) ImGui::EndDisabled();
        caption(colours_available
            ? "The game's colours were made bright for the dark original GBA "
              "screen. Handheld tones them down like a modern handheld's GBA "
              "mode; Soft, Natural, Warm and Deep calm them less. Custom lets "
              "you set them yourself."
            : "Colour adjustments are not yet available for this game.");
        if (colours_available && st->colour_choices[choice].custom) {
            const SliderEdit sat =
                slider_int("Saturation", &st->color_saturation, 0, 200, "%d%%");
            if (sat.changed) st->video_changed = true;
            if (sat.committed) st->color_save = true;
            caption("100% is the game's own colours, 0% is black and white.");
            const SliderEdit hue = slider_int("Hue", &st->color_hue, -180, 180, "%d");
            if (hue.changed) st->video_changed = true;
            if (hue.committed) st->color_save = true;
            caption("Turns every colour around the colour wheel. 0 is the "
                    "game's own.");
            const SliderEdit bri =
                slider_int("Brightness", &st->color_brightness, 50, 150, "%d%%");
            if (bri.changed) st->video_changed = true;
            if (bri.committed) st->color_save = true;
            caption("100% is the game's own brightness.");
            const SliderEdit warm =
                slider_int("Warmth", &st->color_warmth, 0, 100, "%d%%");
            if (warm.changed) st->video_changed = true;
            if (warm.committed) st->color_save = true;
            caption("Pulls greens toward yellow, like the Warm profile (100%).");
            const SliderEdit dark =
                slider_int("Darkening", &st->color_darken, 0, 50, "%d%%");
            if (dark.changed) st->video_changed = true;
            if (dark.committed) st->color_save = true;
            caption("Deepens the darker colours. Natural uses 10%, Deep 25%.");
        }
        {
            static constexpr const char* kVisionLabels[] = {"Normal", "Deuteranopia",
                                                            "Tritanopia"};
            const int vision = std::clamp(st->color_vision, 0, 2);
            if (setting_combo("Colour vision", kVisionLabels[vision])) {
                for (int i = 0; i < 3; ++i) {
                    if (native_selectable(kVisionLabels[i], i == vision)) {
                        st->color_vision = i;
                        st->video_changed = true;
                    }
                    if (i == vision) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            caption("For colour-blind players: moves colours you cannot tell apart "
                    "into ones you can. Deuteranopia is red-green, Tritanopia "
                    "blue-yellow. Works with any Colours choice and filter.");
        }
    } else {
        section("Screen filter");
        {
            const bool filters_available = st->screen_filters_supported && st->screen_filters_available;
            if (!filters_available) ImGui::BeginDisabled();
            static constexpr const char* kFilterLabels[] = {"Off", "LCD3x", "xBR",
                                                           "CRT Lottes", "ScaleFX"};
            const int filter_choice =
                st->screen_filter >= 1 && st->screen_filter <= 4
                    ? st->screen_filter : 0;
            if (setting_combo("Filter (test)", kFilterLabels[filter_choice])) {
                for (int i = 0; i < 5; ++i) {
                    if (native_selectable(kFilterLabels[i], i == filter_choice)) {
                        st->screen_filter = i;
                        st->video_changed = true;
                    }
                    if (i == filter_choice) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!filters_available) ImGui::EndDisabled();
            caption(filters_available
                ? "Test option. LCD3x imitates the GBA screen's grid, CRT Lottes "
                  "an old TV; xBR and ScaleFX smooth the pixel art."
                : (st->screen_filters_supported
                    ? "Enable Screen Filters in the launcher. Requires OpenGL."
                    : "Screen filters are not yet available for this game."));
        }

    }
}

void draw_audio_page(ConfigUiState* st) {
    section("Sound");
    if (slider_int("Volume", &st->volume, 0, 100, "%d%%").changed)
        st->audio_changed = true;
    if (setting_checkbox("Mute", &st->mute)) st->audio_changed = true;
    // Same game_options.ini key as the launcher's cog; the game reads it live.
    section("Music mod");
    const bool available = local_music_available();
    ImGui::BeginDisabled(!available);
    bool music_mod = available && game_option_get(kOptionMusicMod, 0) != 0;
    if (setting_checkbox("Enable Music Mod", &music_mod))
        game_option_set(kOptionMusicMod, music_mod ? 1 : 0);
    ImGui::EndDisabled();
    caption(available
        ? "Plays the recordings in the collection's Music folder instead of "
          "the game's songs. Sound effects stay the same. Switches straight away."
        : "No music folder found. Put recordings in Music/TBS or Music/TLA "
          "next to the launcher (see MUSIC_MODS.md) and restart the game.");
}

void draw_turbo_page(ConfigUiState* st) {
    section("Fast Forward");
    caption("Set the Fast Forward buttons on the Hotkeys page. Hold runs "
            "fast while pressed; Toggle switches it on and off.");
    // 0.1 is ImGui's own controller step for a "%.1f" float slider.
    if (slider_float("Speed", &st->turbo_multiplier, 1.0f,
                     static_cast<float>(kMaxTurboMultiplier), 0.1f, "%.1fx")
            .committed)
        st->speed_changed = true;
    if (setting_checkbox("As fast as possible", &st->uncapped))
        st->speed_changed = true;
    caption("Ignores the speed above and runs as fast as your PC allows.");
    if (setting_checkbox("Mute while fast forwarding", &st->mute_during_turbo))
        st->speed_changed = true;
}

void draw_enhancements_page(ConfigUiState* st) {
    section("Flicker");
    // VFX-FLICKER-02: selective temporal blend ("LCD ghosting"). Legacy
    // config values 4..6 were whole-frame modes that are no longer offered;
    // migrate them through the normal change path so an old file cannot keep
    // a hidden mode active. Greyed, not hidden, while 2x scene interpolation
    // is on (see host_window.cpp's present()).
    static const char kTemporalBlendItems[] =
        "Off\0" "Light\0" "Medium\0" "Strong\0";
    if (st->temporal_blend_index >= 4 &&
        st->temporal_blend_index < gbarecomp::kTemporalBlendItemCount) {
        // Same mapping as host_window.cpp's TemporalBlend load: 4..6 -> 1..3.
        st->temporal_blend_index -= 3;
        st->enhancements_changed = true;
    }
    if (st->temporal_blend_index < 0 || st->temporal_blend_index > 3)
        st->temporal_blend_index = gbarecomp::kTemporalBlendDefaultIndex;
    const bool blocked =
        st->frame_interpolation_available && st->frame_interpolation_2x;
    if (blocked) ImGui::BeginDisabled();
    if (setting_items("Flicker reduction", &st->temporal_blend_index,
                     kTemporalBlendItems))
        st->enhancements_changed = true;
    if (blocked) ImGui::EndDisabled();
    caption(blocked
        ? "Unavailable in the current presentation mode."
        : "Smooths effects that flash every other frame, such as barriers "
          "and the world map, the way the GBA screen did. Normal movement "
          "stays sharp.");

}

void draw_troubleshooting_page(ConfigUiState* st) {
    section("Menu controls");
    ImGui::TextWrapped("%s", kNavigationHelp);
    ImGui::TextWrapped("Drag the heading to move the menu. Drag its edges or corners to resize it.");
    section("Troubleshooting");
    if (setting_checkbox("Crash log", &st->crash_log))
        st->logging_changed = true;
    caption("Records what the game was doing, so a crash or freeze report "
            "shows where it happened. Slows the game down; turn it on only "
            "if you are hunting a crash.");
}

void draw_game_page() {
    section("Game");
    for (int i = 0; i < g_game_switch_count; ++i) {
        const GameOptionSwitch& s = g_game_switches[i];
        if (std::strcmp(s.key, walk_through_walls::kOptionKey) == 0) continue;
        bool on = game_option_get(s.key, 0) != 0;
        if (setting_checkbox(s.label, &on)) game_option_set(s.key, on ? 1 : 0);
        caption(s.caption);
    }
    if (g_game_page_draw) g_game_page_draw();
}

void draw_cheats_page(ConfigUiState* st) {
    section("Battle cheats");
    if (setting_checkbox("Infinite HP", &st->infinite_hp)) st->cheats_changed = true;
    if (setting_checkbox("Infinite PP", &st->infinite_pp)) st->cheats_changed = true;
    caption("Party HP and PP are restored as the game uses them.");
    static const char* const steps[] = {
        "0x", "0.5x", "1x", "2x", "5x", "10x", "25x", "50x", "100x"};
    static const char* const drop_steps[] = {
        "0x", "0.5x", "1x", "2x", "5x", "10x"};
    auto reward_combo = [](const char* label, const char* key,
                          const char* const* labels, int count) {
        int value = game_option_get(key, 2);
        if (value < 0 || value >= count) value = 2;
            if (setting_combo(label, labels[value])) {
            for (int i = 0; i < count; ++i) {
                const bool selected = value == i;
                if (native_selectable(labels[i], selected))
                    game_option_set(key, i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    };
    reward_combo("Experience", kOptionExpMultiplier, steps, 9);
    reward_combo("Coins", kOptionCoinMultiplier, steps, 9);
    reward_combo("Drop chance", kOptionDropMultiplier, drop_steps, 6);
    caption("Changes rewards after battles. 1x keeps the game's normal rates.");
    for (int i = 0; i < g_game_switch_count; ++i) {
        const GameOptionSwitch& s = g_game_switches[i];
        if (std::strcmp(s.key, walk_through_walls::kOptionKey) != 0) continue;
        section("Field cheats");
        bool on = game_option_get(s.key, 0) != 0;
        if (setting_checkbox(s.label, &on)) game_option_set(s.key, on ? 1 : 0);
        caption(s.caption);
    }
}

void draw_fps_overlay(const ConfigUiState* st) {
    if (!st || !st->show_fps) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 8.0f,
                                   viewport->WorkPos.y + 8.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(0.70f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##FpsOverlay", nullptr, kFlags)) {
        ImGui::TextColored(kGold, "%.0f FPS", st->fps);
        ImGui::SameLine();
        ImGui::TextColored(kParchment, "%.0f%% speed",
                           st->emulation_speed_percent);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_debug_overlay(const ConfigUiState* st) {
    if (!st || !st->debug_overlay || g_visible) return;
    const DebugOverlayState& d = st->debug_overlay_state;
    const bool any_test = d.self_heal_ram || d.cost_probe ||
        d.present_cadence || d.ram_churn_probe ||
        d.additional_debug_logging;
    const bool any_audio = d.audio_status_available ||
        d.turbo_audio_muted || d.turbo_audio_uncapped;

    // Keep this in the upper-right during gameplay. NoInputs makes it
    // informational only: mouse, keyboard, and controller focus stay in the
    // game. Size/position clamps keep tiny test windows fully covered.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float max_w = viewport->WorkSize.x > 16.0f
        ? viewport->WorkSize.x - 16.0f : viewport->WorkSize.x;
    const float max_h = viewport->WorkSize.y > 16.0f
        ? viewport->WorkSize.y - 16.0f : viewport->WorkSize.y;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 8.0f,
               viewport->WorkPos.y + 8.0f),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(max_w, max_h));
    ImGui::SetNextWindowBgAlpha(0.76f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##DebugOverlay", nullptr, kFlags)) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float max_x = viewport->WorkPos.x + viewport->WorkSize.x - size.x;
        const float max_y = viewport->WorkPos.y + viewport->WorkSize.y - size.y;
        const float clamped_x = std::clamp(pos.x, viewport->WorkPos.x, max_x);
        const float clamped_y = std::clamp(pos.y, viewport->WorkPos.y, max_y);
        if (clamped_x != pos.x || clamped_y != pos.y)
            ImGui::SetWindowPos(ImVec2(clamped_x, clamped_y));
        if (any_test) {
            ImGui::TextDisabled("Test Variables");
            if (d.self_heal_ram) ImGui::TextUnformatted("Self-heal RAM: on");
            if (d.cost_probe) ImGui::TextUnformatted("Cost Probe: on");
            if (d.present_cadence) ImGui::TextUnformatted("Present Cadence: on");
            if (d.ram_churn_probe) ImGui::TextUnformatted("RAM Churn Probe: on");
            if (d.additional_debug_logging)
                ImGui::TextUnformatted("Additional Debug Logging: on");
        }
        if (any_audio) {
            if (any_test) ImGui::Spacing();
            ImGui::TextDisabled("Audio Variables");
            ImGui::TextUnformatted("Audio Engine: GBA");
            if (d.turbo_audio_muted)
                ImGui::TextUnformatted("Mute During Turbo: on");
            if (d.turbo_audio_uncapped)
                ImGui::TextUnformatted("Uncapped Turbo: on");
        }
        if (!any_test && !any_audio)
            ImGui::TextDisabled("None");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

// Golden Sun look: blue windows, silver borders and crisp square controls.
// The game supplies the original bitmap font, window pieces and palette.
void apply_modern_style() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 0.0f;
    style.PopupRounding     = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 0.0f;
    style.WindowBorderSize  = 2.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 2.0f;
    style.PopupBorderSize   = 2.0f;

    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;

    style.WindowPadding     = ImVec2(24.0f, 20.0f);
    style.FramePadding      = ImVec2(12.0f, 9.0f);
    style.CellPadding       = ImVec2(6.0f, 5.0f);
    style.ItemSpacing       = ImVec2(9.0f, 7.0f);
    style.ItemInnerSpacing  = ImVec2(7.0f, 5.0f);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 14.0f;
    style.SeparatorTextBorderSize = 2.0f;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    // Matching fallback until the game publishes its own measured palette.
    apply_menu_colors(ImVec4(0.0f, 0.29f, 0.32f, 1.0f),
                      ImVec4(0.0f, 0.39f, 0.55f, 1.0f),
                      ImVec4(0.03f, 0.52f, 0.74f, 1.0f),
                      ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                      ImVec4(1, 1, 1, 1), ImVec4(0, 0, 0, 1));
}

bool config_ui_init(SDL_Window* window, SDL_Renderer* renderer,
                     bool use_opengl) {
    if (g_ready || !window || !renderer) return g_ready;
    g_fit_default_window = false;
    g_default_fit_height = 0;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // we persist settings ourselves, not via imgui.ini
    // Keep the F1 menu usable without a mouse: the sidebar and controls are
    // ordinary ImGui navigation items, driven by keyboard and controller.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable |
                      ImGuiConfigFlags_NavEnableKeyboard |
                      ImGuiConfigFlags_NavEnableGamepad;
    // Otherwise a press beside a slider (on its label or the window's empty
    // space) drags the whole menu away from the mouse. Windows move by their
    // title bar only.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = true;
    if (use_opengl) {
        // Real multi-viewport: torn-off panels become their own OS windows,
        // each an SDL window + GL context ImGui creates/destroys itself via
        // the platform backend below.
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    ImGui::StyleColorsDark();
    apply_modern_style();
    bool backend_ok;
    if (use_opengl) {
        backend_ok = ImGui_ImplSDL2_InitForOpenGL(window,
                                                   SDL_GL_GetCurrentContext());
        if (backend_ok) backend_ok = ImGui_ImplOpenGL3_Init("#version 130");
    } else {
        backend_ok = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
        if (backend_ok) backend_ok = ImGui_ImplSDLRenderer2_Init(renderer);
    }
    if (!backend_ok) {
        ImGui::DestroyContext();
        return false;
    }
    g_renderer = renderer;
    g_use_opengl = use_opengl;
    g_window = window;
    // SHUTDOWN-FREEZE-01: the context ImGui_ImplSDL2_InitForOpenGL() was
    // wired to is the MAIN window's; capture it now, while it is current, so
    // shutdown can restore it.
    g_gl_context = use_opengl ? SDL_GL_GetCurrentContext() : nullptr;
    g_config_window_recover = true;
    g_ready = true;
    return true;
}

void config_ui_shutdown() {
    if (!g_ready) return;
    if (g_use_opengl) {
        // A torn-off viewport's GL context may be current (ImGui switches
        // contexts per viewport). Restore the MAIN context first so
        // ImGui_ImplOpenGL3_Shutdown() deletes its GL objects against the
        // context that owns them, and DestroyPlatformWindows() below tears
        // down the detached viewports while the main context still exists.
        if (g_window && g_gl_context) {
            SDL_GL_MakeCurrent(g_window, g_gl_context);
        }
        clear_edition_badge();
        destroy_menu_texture();
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::DestroyPlatformWindows();
        }
        ImGui_ImplOpenGL3_Shutdown();
    } else {
        clear_edition_badge();
        destroy_menu_texture();
        ImGui_ImplSDLRenderer2_Shutdown();
    }
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_renderer = nullptr;
    g_window = nullptr;
    g_gl_context = nullptr;
    g_use_opengl = false;
    g_config_window_recover = false;
    g_ready = false;
}

void config_ui_set_edition_badge(const char* bitmap_path, const char* caption,
                                 const char* flourish) {
    if (!g_ready) return;
    clear_edition_badge();
    if (!bitmap_path || !caption || !*caption) return;
    SDL_Surface* bitmap = SDL_LoadBMP(bitmap_path);
    if (!bitmap) return;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(bitmap, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(bitmap);
    if (!rgba) return;
    if (g_use_opengl) {
        GLint old_texture = 0, old_alignment = 0, old_row_length = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);
        glGenTextures(1, &g_badge_gl_texture);
        glBindTexture(GL_TEXTURE_2D, g_badge_gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_length);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
    } else {
        g_badge_sdl_texture = SDL_CreateTextureFromSurface(g_renderer, rgba);
        if (g_badge_sdl_texture)
            SDL_SetTextureBlendMode(g_badge_sdl_texture, SDL_BLENDMODE_BLEND);
    }
    // Half the supplied bitmap's native size keeps the footer compact.
    g_badge_size = ImVec2(rgba->w / 2.0f, rgba->h / 2.0f);
    SDL_FreeSurface(rgba);
    if (!g_badge_gl_texture && !g_badge_sdl_texture) return;
    std::snprintf(g_badge_caption, sizeof(g_badge_caption), "%s", caption);
    std::snprintf(g_badge_flourish, sizeof(g_badge_flourish), "%s",
                  flourish ? flourish : "");
}

bool config_ui_visible() { return g_ready && g_visible; }

void config_ui_set_visible(bool on) {
    if (!g_ready) return;
    if (on && !g_visible) g_config_window_recover = true;
    g_visible = on;
    if (!on) g_capture = Capture::None;
}

void config_ui_toggle() { config_ui_set_visible(!g_visible); }

bool config_ui_handle_event(const SDL_Event* e) {
    if (!g_ready || !e) return false;

    // A live capture outranks everything: the very next key or pad press is
    // the binding and must not reach ImGui (which would treat it as UI input)
    // or the guest (which would treat it as gameplay).
    if (g_visible && g_capture != Capture::None) {
        if (e->type == SDL_KEYDOWN && e->key.repeat == 0) return true;
        if (e->type == SDL_CONTROLLERBUTTONDOWN) return true;
    }

    if (g_visible && g_capture == Capture::None &&
        e->type == SDL_CONTROLLERBUTTONDOWN &&
        e->cbutton.button == SDL_CONTROLLER_BUTTON_B &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        config_ui_set_visible(false);
        return true;
    }

    // SDL_Renderer rewrites the game window's mouse motion into its logical
    // coordinates whenever a logical size is set (the 240x160 view sets one).
    // ImGui reads the real desktop position while no button is held but only
    // these events during a drag, so a dragged slider would see the pointer
    // jump by the window scale. Give ImGui the same real position it reads
    // between drags.
    SDL_Event event = *e;
    if (event.type == SDL_MOUSEMOTION && g_window &&
        event.motion.windowID == SDL_GetWindowID(g_window)) {
        int global_x = 0, global_y = 0, window_x = 0, window_y = 0;
        SDL_GetGlobalMouseState(&global_x, &global_y);
        SDL_GetWindowPosition(g_window, &window_x, &window_y);
        event.motion.x = global_x - window_x;
        event.motion.y = global_y - window_y;
    }
    ImGui_ImplSDL2_ProcessEvent(&event);

    if (!g_visible) {
        // The F1 menu itself is closed, but game-owned extra content (e.g. a
        // function-call tracer window) may still want the keyboard -- see
        // g_config_ui_extra_wants_keyboard (host_config_ui.h). ImGui already
        // has the event via ProcessEvent above; consume it here too so the
        // host doesn't ALSO feed it to guest keyinput or a hotkey.
        if (!g_config_ui_extra_wants_keyboard || !g_config_ui_extra_wants_keyboard())
            return false;
        switch (e->type) {
            case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
                return true;
            default:
                return false;
        }
    }

    // While visible the menu owns the mouse and keyboard. Window/quit events
    // still belong to the host.
    const ImGuiIO& io = ImGui::GetIO();
    switch (e->type) {
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
            return io.WantCaptureMouse;
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
            return true;
        default:
            return false;
    }
}

// Consume a raw key/pad press into whichever bind box is armed. Called by
// host_window before it does anything else with the event.
bool config_ui_capture_key(ConfigUiState* st, int scancode, int keycode,
                           unsigned mods) {
    if (!g_visible || g_capture == Capture::None) return false;
    if (keycode == SDLK_ESCAPE) { g_capture = Capture::None; return true; }
    if (g_capture == Capture::Key && g_capture_index >= 0) {
        for (int i = 0; i < 10; ++i)
            if (i != g_capture_index && st->key_bind[i] == scancode)
                st->key_bind[i] = 0;   // move, never duplicate
        st->key_bind[g_capture_index] = scancode;
        st->binds_changed = true;
    } else if (g_capture == Capture::Hotkey && g_capture_index >= 0) {
        st->hotkey_key[g_capture_index] = keycode;
        st->hotkey_mods[g_capture_index] =
            mods & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT);
        st->binds_changed = true;
    } else {
        return false;   // a pad box is armed; a key does not satisfy it
    }
    g_capture = Capture::None;
    return true;
}

bool config_ui_capture_pad(ConfigUiState* st, int button) {
    if (!g_visible || g_capture_index < 0) return false;
    if (pad_is_stick(button) && !st->stick_bindings_available) return false;
    // L2/R2's synthetic ids are hotkey-only, not gameplay binds: a trigger
    // pull while a Controls-page pad box is armed is not consumed. Stick
    // directions (also synthetic ids) are accepted for both.
    const bool is_trigger = (button == kPadTriggerLeft || button == kPadTriggerRight);
    if (g_capture == Capture::Pad) {
        if (is_trigger) return false;
        for (int slot = 0; slot < 2; ++slot) {
            int* bindings = slot == 0 ? st->pad_bind : st->pad_bind_alt;
            for (int i = 0; i < 10; ++i)
                if ((i != g_capture_index || slot != g_capture_pad_slot) &&
                    bindings[i] == button)
                    bindings[i] = -1;
        }
        (g_capture_pad_slot == 0 ? st->pad_bind : st->pad_bind_alt)[g_capture_index] = button;
    } else if (g_capture == Capture::HotkeyPad) {
        // Unlike the gameplay pad above, a hotkey's controller button is not
        // deduped against the other hotkey rows: Fast Forward hold and toggle
        // sharing one button, say, is legitimate, not a conflict to resolve
        // silently.
        st->hotkey_pad[g_capture_index] = button;
    } else {
        return false;
    }
    st->binds_changed = true;
    g_capture = Capture::None;
    return true;
}

void config_ui_draw(ConfigUiState* st) {
    if (!g_ready || !st) return;
    // Escape backs out of a popup before closing the whole settings window.
    const bool popup_was_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    {
        std::lock_guard<std::mutex> lock(g_menu_art_mutex);
        if (g_open_cheats_requested) {
            g_selected_page = 6;
            g_scroll_to_selected_tab = true;
            config_ui_set_visible(true);
            g_open_cheats_requested = false;
        }
    }
    if (g_use_opengl) {
        ImGui_ImplOpenGL3_NewFrame();
    } else {
        ImGui_ImplSDLRenderer2_NewFrame();
    }
    // Initialize backend device objects before installing the bitmap atlas;
    // first-frame font creation would otherwise replace its nearest filtering.
    apply_pending_menu_art();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (g_visible) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float margin = style.WindowPadding.x;
        float preferred_w = margin * 2;
        for (const char* name : {"Video", "Audio", "Controls", "Hotkeys",
                                "Speed", "Game", "Cheats", "Help"})
            preferred_w += ImGui::CalcTextSize(name).x + style.FramePadding.x * 2 +
                style.ItemSpacing.x;
        preferred_w = std::max(preferred_w, display_group_min_width() * 2 +
            margin * 6 + style.FramePadding.x * 2 + style.ScrollbarSize);
        const float preferred_h = ImGui::GetFrameHeightWithSpacing() * 15 +
            ImGui::GetTextLineHeightWithSpacing() * 5 + style.WindowPadding.y * 2;
        const ImVec2 minimum(
            ImGui::CalcTextSize("Restore defaults   Quit Game").x + margin * 2,
            ImGui::GetFrameHeightWithSpacing() * 5 + style.WindowPadding.y * 2);
        const bool saved_layout = st->menu_width > 0 && st->menu_height > 0;
        const ImVec2 size(
            std::clamp(saved_layout ? float(st->menu_width) : preferred_w,
                std::min(minimum.x, viewport->WorkSize.x), viewport->WorkSize.x),
            std::clamp(saved_layout ? float(st->menu_height) : preferred_h,
                std::min(minimum.y, viewport->WorkSize.y), viewport->WorkSize.y));
        if (!ImGui::FindWindowByName("Settings###Configuration"))
            g_fit_default_window = !saved_layout;
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(
            viewport->WorkPos.x + (saved_layout
                ? std::clamp(float(st->menu_x), 0.0f, viewport->WorkSize.x - size.x)
                : (viewport->WorkSize.x - size.x) * 0.5f),
            viewport->WorkPos.y + (saved_layout
                ? std::clamp(float(st->menu_y), 0.0f, viewport->WorkSize.y - size.y)
                : (viewport->WorkSize.y - size.y) * 0.5f)),
            ImGuiCond_FirstUseEver);
        // Retain the player's size/position, but recover inside the game when
        // its viewport shrinks or F1 reopens. Never reduce the font to make it fit.
        if (ImGuiWindow* previous = ImGui::FindWindowByName("Settings###Configuration")) {
            const ImVec2 fitted(std::min(previous->Size.x, viewport->WorkSize.x),
                                std::min(previous->Size.y, viewport->WorkSize.y));
            if (fitted.x != previous->Size.x || fitted.y != previous->Size.y)
                ImGui::SetNextWindowSize(fitted);
            const ImVec2 pos(
                std::clamp(previous->Pos.x, viewport->WorkPos.x,
                    viewport->WorkPos.x + viewport->WorkSize.x - fitted.x),
                std::clamp(previous->Pos.y, viewport->WorkPos.y,
                    viewport->WorkPos.y + viewport->WorkSize.y - fitted.y));
            if (g_config_window_recover || pos.x != previous->Pos.x || pos.y != previous->Pos.y)
                ImGui::SetNextWindowPos(pos);
        }
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(minimum.x, viewport->WorkSize.x),
                   std::min(minimum.y, viewport->WorkSize.y)), viewport->WorkSize);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_fit_default_window = false;
        if (g_fit_default_window && g_default_fit_height > 0) {
            const float height = std::clamp(g_default_fit_height,
                std::min(minimum.y, viewport->WorkSize.y), viewport->WorkSize.y);
            ImGui::SetNextWindowSize(ImVec2(size.x, height));
            ImGui::SetNextWindowPos(ImVec2(
                viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5f,
                viewport->WorkPos.y + (viewport->WorkSize.y - height) * 0.5f));
        }
        ImGui::SetNextWindowViewport(viewport->ID);
        bool window_open = true;
        if (ImGui::Begin("Settings###Configuration", &window_open,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking)) {
            g_config_window_recover = false;
            draw_menu_frame(ImGui::GetWindowPos(), ImGui::GetWindowSize());
            const ImVec2 title_pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##SettingsDrag",
                ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight()));
            if (ImGui::IsItemActivated())
                ImGui::StartMouseMovingWindow(ImGui::GetCurrentWindow());
            ImGui::SetCursorScreenPos(title_pos);
            ImGui::TextColored(kGold, "Settings");
            const char* saved_hint = "Saved automatically";
            const float saved_w = ImGui::CalcTextSize(saved_hint).x;
            if (ImGui::GetContentRegionAvail().x > saved_w + ImGui::GetStyle().ItemSpacing.x) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - saved_w);
                ImGui::TextDisabled("%s", saved_hint);
            }
            ImGui::Spacing();

            // Horizontal tab strip and one scrollable page. Game is present
            // only when this game registered game-owned options.
            struct Page { const char* name; int id; };
            const Page kPages[] = {
                {"Video", 0}, {"Audio", 1}, {"Controls", 2},
                {"Hotkeys", 3}, {"Speed", 4},
                {"Game", 5}, {"Cheats", 6}, {"Help", 7},
            };
            const bool show_game = g_game_switch_count > 0 || g_game_page_draw;
            Page pages[8];
            int page_count = 0;
            for (const Page& p : kPages)
                if (p.id != 5 || show_game) pages[page_count++] = p;
            if (g_selected_page == 5 && !show_game) g_selected_page = 6;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                g_capture == Capture::None) {
                const bool left = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_PageUp, false);
                const bool right = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_PageDown, false);
                if (left || right) {
                    int current = 0;
                    while (current + 1 < page_count &&
                           pages[current].id != g_selected_page) ++current;
                    current = (current + (right ? 1 : page_count - 1)) % page_count;
                    g_selected_page = pages[current].id;
                    g_scroll_to_selected_tab = true;
                }
            }
            const char* tab_names[8];
            int active_tab = 0;
            for (int i = 0; i < page_count; ++i) {
                tab_names[i] = pages[i].name;
                if (pages[i].id == g_selected_page) active_tab = i;
            }
            tab_strip("ConfigTabs", tab_names, page_count, active_tab,
                      g_scroll_to_selected_tab);
            g_selected_page = pages[active_tab].id;
            g_scroll_to_selected_tab = false;
            static const char* const page_help[] = {
                "Choose a setting for help.",
                "Adjust volume and choose original or recorded music.",
                "Select a binding, then press a key or controller button. Esc cancels; x clears a binding.",
                "Assign shortcuts for actions while playing. Select a box to change its binding.",
                "Set the fast-forward speed. Hold or Toggle bindings are on the Hotkeys tab.",
                "Adjust game options. Changes take effect as you play.",
                "Change battle cheats and rewards. Normal reward rates are 1x.",
                "Turn on crash logging only when investigating a crash or freeze."
            };
            const bool page_changed = g_help_page != g_selected_page;
            if (page_changed) {
                g_context_help = page_help[g_selected_page];
                g_help_page = g_selected_page;
            }
            ImGui::Spacing();

            const bool have_badge = g_badge_caption[0] != '\0';
            const float badge_h = have_badge
                ? std::max(g_badge_size.y, ImGui::GetTextLineHeightWithSpacing() * 2)
                  + ImGui::GetStyle().ItemSpacing.y : 0.0f;
            // A short help footer yields to settings on short windows. Longer
            // explanations remain available by scrolling or tooltip.
            // Budget the Display settings and headings, contextual help
            // and the footer at the current font size, rather than a pixel cutoff.
            const bool compact = ImGui::GetContentRegionAvail().y <
                ImGui::GetFrameHeightWithSpacing() * 7 +
                ImGui::GetTextLineHeightWithSpacing() * 3 +
                ImGui::GetTextLineHeight() + style.FramePadding.y * 2;
            float help_h = ImGui::GetTextLineHeight() + style.FramePadding.y * 2;
            const float footer_h = ImGui::GetFrameHeight() + badge_h +
                style.ItemSpacing.y * 3 + style.SeparatorTextBorderSize;
            const bool show_help = ImGui::GetContentRegionAvail().y >=
                footer_h + help_h + ImGui::GetFrameHeightWithSpacing() * 3;
            if (!show_help) help_h = 0;
            g_help_panel_visible = show_help;
            const float content_h = std::max(ImGui::GetFrameHeight(),
                ImGui::GetContentRegionAvail().y - footer_h - help_h);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                ImVec2(style.ItemSpacing.x, compact ? style.ItemSpacing.y : ImGui::GetTextLineHeight()));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                ImVec2(style.FramePadding.x, style.FramePadding.y));
            ImGui::BeginChild("ConfigContent", ImVec2(0.0f, content_h),
                              ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_AlwaysUseWindowPadding);
            if (page_changed)
                ImGui::SetScrollY(0);
            switch (g_selected_page) {
                case 0:
                    draw_video_page(st);
                    if (g_video_section == 2) draw_enhancements_page(st);
                    break;
                case 1: draw_audio_page(st); break;
                case 2: draw_controls_page(st); break;
                case 3: draw_hotkeys_page(st); break;
                case 4: draw_turbo_page(st); break;
                case 5: draw_game_page(); break;
                case 6: draw_cheats_page(st); break;
                default: draw_troubleshooting_page(st); break;
            }
            const ImGuiWindow* content_window = ImGui::GetCurrentWindow();
            const float content_needed = content_window->DC.CursorMaxPos.y -
                content_window->Pos.y + content_window->Scroll.y + content_window->WindowPadding.y;
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::Separator();
            if (show_help) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
                ImGui::BeginChild("SettingHelp", ImVec2(0, help_h),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_AlwaysUseWindowPadding);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(g_context_help ? g_context_help : "Choose a setting for help.");
                ImGui::PopTextWrapPos();
                if (ImGui::IsItemHovered() && g_context_help)
                    ImGui::SetTooltip("%s", g_context_help);
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
            const char* quit_label = "Quit Game";
            const float quit_w = ImGui::CalcTextSize(quit_label).x +
                                 ImGui::GetStyle().FramePadding.x * 2.0f;
            if (native_button("Back")) window_open = false;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", kNavigationHelp);
            const char* footer_hint = "Esc/B: Back   Enter/A: Select";
            if (ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() >
                ImGui::CalcTextSize(footer_hint).x + quit_w + style.ItemSpacing.x * 2 +
                ImGui::CalcTextSize("Back").x + style.FramePadding.x * 2) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", footer_hint);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s", kNavigationHelp);
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - quit_w);
            if (native_button(quit_label)) st->request_quit = true;

            if (have_badge) {
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float text_w = std::max(ImGui::CalcTextSize(g_badge_caption).x,
                                             ImGui::CalcTextSize(g_badge_flourish).x);
                const float badge_w = text_w + gap + g_badge_size.x;
                ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x,
                    ImGui::GetContentRegionMax().x - badge_w));
                ImGui::BeginGroup();
                ImGui::Dummy(ImVec2(0, std::max(0.0f,
                    (g_badge_size.y - ImGui::GetTextLineHeightWithSpacing() * 2) / 2)));
                ImGui::TextColored(kGold, "%s", g_badge_caption);
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.83f, 1.0f), "%s",
                                   g_badge_flourish);
                ImGui::EndGroup();
                ImGui::SameLine();
                const ImTextureID texture = g_use_opengl
                    ? static_cast<ImTextureID>(g_badge_gl_texture)
                    : static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_badge_sdl_texture));
                ImGui::Image(texture, g_badge_size);
            }

            // Measure the actual page and chrome for a compact first-run size.
            // Once fitted (or manually dragged/resized), geometry is user-owned.
            if (g_fit_default_window) {
                const ImGuiWindow* root = ImGui::GetCurrentWindow();
                g_default_fit_height = std::clamp(std::ceil(
                    root->DC.CursorMaxPos.y - root->Pos.y + root->Scroll.y + root->WindowPadding.y -
                    content_h + content_needed),
                    std::min(minimum.y, viewport->WorkSize.y), viewport->WorkSize.y);
                if (g_default_fit_height == root->Size.y) g_fit_default_window = false;
            }
            if (!g_fit_default_window && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 pos = ImGui::GetWindowPos(), extent = ImGui::GetWindowSize();
                const int x = int(std::round(pos.x - viewport->WorkPos.x));
                const int y = int(std::round(pos.y - viewport->WorkPos.y));
                const int width = int(std::round(extent.x)), height = int(std::round(extent.y));
                if (st->menu_x != x || st->menu_y != y ||
                    st->menu_width != width || st->menu_height != height) {
                    st->menu_x = x; st->menu_y = y;
                    st->menu_width = width; st->menu_height = height;
                    st->menu_layout_changed = true;
                }
            }

            // Esc closes the menu, unless it is cancelling a bind capture.
            if (!popup_was_open && g_capture == Capture::None &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                window_open = false;
        }
        ImGui::End();
        if (!window_open) config_ui_set_visible(false);
    }

    draw_fps_overlay(st);

    // Unlike the F1 configuration window, this stays visible during normal
    // gameplay whenever the persisted Debug overlay switch is on.
    draw_debug_overlay(st);

    // Game-owned extra ImGui content (see host_config_ui.h). Drawn every
    // frame, independent of g_visible, same as the debug overlay above.
    if (g_config_ui_extra_draw) g_config_ui_extra_draw();

    ImGui::Render();
    if (g_use_opengl) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Multi-viewport: render each torn-off panel into its own OS
        // window/GL context. Save the current context and restore it so the
        // caller's SDL_RenderPresent, right after this returns, targets the
        // main window again.
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_window, backup_context);
        }
    } else {
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    }
}

}  // namespace gbarecomp

#else  // !GBARECOMP_HAVE_IMGUI — no-op stubs

namespace gbarecomp {
bool config_ui_init(SDL_Window*, SDL_Renderer*, bool) { return false; }
void config_ui_shutdown() {}
void config_ui_set_edition_badge(const char*, const char*, const char*) {}
bool config_ui_handle_event(const SDL_Event*) { return false; }
bool config_ui_visible() { return false; }
void config_ui_toggle() {}
void config_ui_set_visible(bool) {}
bool config_ui_capture_key(ConfigUiState*, int, int, unsigned) { return false; }
bool config_ui_capture_pad(ConfigUiState*, int) { return false; }
void config_ui_draw(ConfigUiState*) {}
}  // namespace gbarecomp

#endif
