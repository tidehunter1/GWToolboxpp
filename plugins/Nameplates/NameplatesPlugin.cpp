// NameplatesPlugin.cpp
// ... (your header comments remain unchanged) ...

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <GWCA/GWCA.h>
#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <ToolboxPlugin.h>
#include <imgui.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cwchar>
#include <optional>
#include <d3d9.h>                          // ← Added for texture

// ... [All your existing helper classes: AgentNameCache, Utf8ToWide, ParseSemicolonNameList stay exactly the same] ...

struct NameplateSettings {
    bool enabled = true;
    bool show_enemies = true;
    bool show_allies = true;
    bool show_neutrals = false;
    bool hide_own_bar = true;
    bool hide_dead = true;
    float max_range = 5000.0f;
    float bar_width = 40.0f;
    float bar_height = 5.0f;
    float head_offset_z = 0.0f;

    std::string priority1_raw;
    std::string priority2_raw;
    std::string priority3_raw;

    bool color_target = true;
    bool highlight_quest = true;
    bool show_zaishen_coin = true;               // ← Added
};

class NameplatesPlugin : public ToolboxPlugin {
public:
    const char* Name() const override { return "Nameplates"; }

    bool* GetVisiblePtr() override { return &visible_; }
    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override;

    void LoadSettings(const wchar_t* folder) override {
        ToolboxPlugin::LoadSettings(folder);
        LoadSetting("visible", visible_);
        LoadSetting("enabled", settings_.enabled);
        LoadSetting("show_enemies", settings_.show_enemies);
        LoadSetting("show_allies", settings_.show_allies);
        LoadSetting("show_neutrals", settings_.show_neutrals);
        LoadSetting("hide_own_bar", settings_.hide_own_bar);
        LoadSetting("hide_dead", settings_.hide_dead);
        LoadSetting("max_range", settings_.max_range);
        LoadSetting("bar_width", settings_.bar_width);
        LoadSetting("bar_height", settings_.bar_height);
        LoadSetting("head_offset_z", settings_.head_offset_z);
        LoadSetting("priority1_raw", settings_.priority1_raw);
        LoadSetting("priority2_raw", settings_.priority2_raw);
        LoadSetting("priority3_raw", settings_.priority3_raw);
        LoadSetting("color_target", settings_.color_target);
        LoadSetting("highlight_quest", settings_.highlight_quest);
        LoadSetting("show_zaishen_coin", settings_.show_zaishen_coin);   // ← Added
        RefreshPriorityBuffersAndLists();
    }

    void SaveSettings(const wchar_t* folder) override {
        SaveSetting("visible", visible_);
        SaveSetting("enabled", settings_.enabled);
        SaveSetting("show_enemies", settings_.show_enemies);
        SaveSetting("show_allies", settings_.show_allies);
        SaveSetting("show_neutrals", settings_.show_neutrals);
        SaveSetting("hide_own_bar", settings_.hide_own_bar);
        SaveSetting("hide_dead", settings_.hide_dead);
        SaveSetting("max_range", settings_.max_range);
        SaveSetting("bar_width", settings_.bar_width);
        SaveSetting("bar_height", settings_.bar_height);
        SaveSetting("head_offset_z", settings_.head_offset_z);
        SaveSetting("priority1_raw", settings_.priority1_raw);
        SaveSetting("priority2_raw", settings_.priority2_raw);
        SaveSetting("priority3_raw", settings_.priority3_raw);
        SaveSetting("color_target", settings_.color_target);
        SaveSetting("highlight_quest", settings_.highlight_quest);
        SaveSetting("show_zaishen_coin", settings_.show_zaishen_coin);   // ← Added
        ToolboxPlugin::SaveSettings(folder);
    }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override {
        ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);

        // Load Zaishen coin texture
        if (auto* texModule = GW::GwDatTextureModule::Instance()) {
            zaishen_coin_texture = texModule->LoadTextureFromFileId(0x55778);
        }
    }

    void SignalTerminate() override {
        ToolboxPlugin::SignalTerminate();
    }

    bool CanTerminate() override { return true; }

    void Draw(IDirect3DDevice9* /*device*/) override {
        if (!settings_.enabled) return;
        DrawNameplates();
    }

private:
    NameplateSettings settings_;
    bool visible_ = true;

    AgentNameCache name_cache_;
    std::vector<std::wstring> priority1_names_, priority2_names_, priority3_names_;

    static constexpr size_t kPriorityBufSize = 512;
    char priority1_buf_[kPriorityBufSize] = {};
    char priority2_buf_[kPriorityBufSize] = {};
    char priority3_buf_[kPriorityBufSize] = {};

    IDirect3DTexture9* zaishen_coin_texture = nullptr;     // ← Added

    static constexpr ImU32 kPriority1Color = IM_COL32(135, 206, 250, 255);
    static constexpr ImU32 kPriority2Color = IM_COL32(255, 105, 180, 255);
    static constexpr ImU32 kPriority3Color = IM_COL32(147, 112, 219, 255);
    static constexpr ImU32 kTargetColor    = IM_COL32(0, 0, 139, 255);
    static constexpr ImU32 kQuestColor     = IM_COL32(255, 179, 71, 255);

    void RefreshPriorityBuffersAndLists() { /* your original */ }
    std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower) const { /* your original */ }

    void DrawNameplates() { /* your original */ }

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living, const std::wstring& name_lower, bool is_targeted) {
        float hp_pct = living->hp;
        hp_pct = hp_pct < 0.f ? 0.f : (hp_pct > 1.f ? 1.f : hp_pct);

        const ImVec2 top_left(screen.x - settings_.bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + settings_.bar_width, top_left.y + settings_.bar_height);
        const ImVec2 fill_bottom_right(top_left.x + settings_.bar_width * hp_pct, bottom_right.y);

        const ImU32 bg_color = IM_COL32(40, 40, 40, 200);
        ImU32 fill_color;
        if (is_targeted) {
            fill_color = kTargetColor;
        }
        else if (const auto priority_color = GetPriorityColor(name_lower)) {
            fill_color = *priority_color;
        }
        else if (settings_.highlight_quest && living->GetHasQuest()) {
            fill_color = kQuestColor;
        }
        else {
            fill_color = ColorFor(living->allegiance);
        }

        draw_list->AddRectFilled(top_left, bottom_right, bg_color);
        draw_list->AddRectFilled(top_left, fill_bottom_right, fill_color);
        draw_list->AddRect(top_left, bottom_right, IM_COL32(0, 0, 0, 180));

        // Zaishen Coin
        if (settings_.show_zaishen_coin && zaishen_coin_texture && living->GetHasQuest()) {
            const float coin_size = settings_.bar_height * 3.0f;
            const ImVec2 coin_pos(top_left.x - coin_size - 6.0f, screen.y - coin_size * 0.5f + settings_.bar_height * 0.5f);

            draw_list->AddImage((ImTextureID)zaishen_coin_texture, coin_pos,
                                ImVec2(coin_pos.x + coin_size, coin_pos.y + coin_size));
        }
    }

    ImU32 ColorFor(GW::Constants::Allegiance allegiance) const {
        // your original function
    }

    void DrawSettingsInternal() {
        // ... your original settings ...

        ImGui::Checkbox("Show Zaishen coin next to quest NPCs", &settings_.show_zaishen_coin);
    }
};

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static NameplatesPlugin instance;
    return &instance;
}
