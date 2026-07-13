// NameplatesPlugin.cpp
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
#include <GWCA/Modules/GwDat.h>          // Added for texture loading

#include <ToolboxPlugin.h>
#include <imgui.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cwchar>
#include <optional>
#include <d3d9.h>                        // Added for IDirect3DTexture9

// ... (keep all your existing classes: AgentNameCache, Utf8ToWide, ParseSemicolonNameList, etc.)

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
    bool show_zaishen_coin = true;        // ← New setting
};

class NameplatesPlugin : public ToolboxPlugin {
public:
    const char* Name() const override { return "Nameplates"; }

    bool* GetVisiblePtr() override { return &visible_; }
    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override;

    void LoadSettings(const wchar_t* folder) override { /* ... your existing code ... */ 
        LoadSetting("show_zaishen_coin", settings_.show_zaishen_coin);
        // ... rest of your LoadSettings
    }

    void SaveSettings(const wchar_t* folder) override { /* ... your existing code ... */ 
        SaveSetting("show_zaishen_coin", settings_.show_zaishen_coin);
        // ... rest of your SaveSettings
    }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override {
        ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);

        // Load Zaishen coin texture
        auto* texModule = GW::GwDatTextureModule::Instance();
        if (texModule) {
            zaishen_coin_texture = texModule->LoadTextureFromFileId(0x55778);
        }
    }

    void SignalTerminate() override {
        ToolboxPlugin::SignalTerminate();
        // Optional: release texture if needed (usually not required)
    }

    void Draw(IDirect3DDevice9* /*device*/) override {
        if (!settings_.enabled) return;
        DrawNameplates();
    }

private:
    NameplateSettings settings_;
    bool visible_ = true;

    AgentNameCache name_cache_;
    // ... your other members (priority lists, buffers, etc.)

    IDirect3DTexture9* zaishen_coin_texture = nullptr;   // ← Added

    // ... keep your existing helper functions (GetPriorityColor, ShouldShowAllegiance, etc.)

    void DrawNameplates() {
        // ... your existing agent loop (no changes needed until DrawBar)
        for (GW::Agent* agent : *agents) {
            // ... your existing filtering ...

            ImVec2 screen;
            if (!WorldToScreen(living, screen)) continue;

            const std::wstring& name_lower = name_cache_.GetLower(
                living->agent_id, GW::Agents::GetAgentEncName(living->agent_id));

            const bool is_targeted = settings_.color_target && target && living->agent_id == target->agent_id;

            DrawBar(draw_list, screen, living, name_lower, is_targeted);
        }
    }

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living,
                 const std::wstring& name_lower, bool is_targeted) {

        float hp_pct = std::clamp(living->hp, 0.0f, 1.0f);

        const ImVec2 top_left(screen.x - settings_.bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + settings_.bar_width, top_left.y + settings_.bar_height);
        const ImVec2 fill_br(top_left.x + settings_.bar_width * hp_pct, bottom_right.y);

        // Background + HP bar
        draw_list->AddRectFilled(top_left, bottom_right, IM_COL32(40, 40, 40, 200));
        draw_list->AddRectFilled(top_left, fill_br, GetBarColor(living, name_lower, is_targeted));
        draw_list->AddRect(top_left, bottom_right, IM_COL32(0, 0, 0, 180));

        // Zaishen Coin
        if (settings_.show_zaishen_coin && zaishen_coin_texture && living->GetHasQuest()) {
            const float coin_size = settings_.bar_height * 3.0f;        // feel free to tweak
            const ImVec2 coin_pos(
                top_left.x - coin_size - 6.0f,                         // left of the bar
                screen.y - coin_size * 0.5f + settings_.bar_height * 0.5f
            );

            draw_list->AddImage(
                (ImTextureID)zaishen_coin_texture,
                coin_pos,
                ImVec2(coin_pos.x + coin_size, coin_pos.y + coin_size)
            );
        }
    }

    // Helper to get bar color (extracted for clarity)
    ImU32 GetBarColor(const GW::AgentLiving* living, const std::wstring& name_lower, bool is_targeted) {
        if (is_targeted) return IM_COL32(0, 0, 139, 255);
        if (auto priority = GetPriorityColor(name_lower)) return *priority;
        if (settings_.highlight_quest && living->GetHasQuest()) return IM_COL32(255, 179, 71, 255);
        return ColorFor(living->allegiance);
    }

    void DrawSettings() {
        ToolboxPlugin::DrawSettings();
        DrawSettingsInternal();
    }

    void DrawSettingsInternal() {
        // ... your existing settings ...

        ImGui::Checkbox("Show Zaishen coin next to quest NPCs", &settings_.show_zaishen_coin);
    }
};

// Plugin entry point
DLLAPI ToolboxPlugin* ToolboxPluginInstance() {
    static NameplatesPlugin instance;
    return &instance;
}
