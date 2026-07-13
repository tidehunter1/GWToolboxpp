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

#include <ToolboxPlugin.h>
#include <imgui.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cwchar>
#include <optional>
#include <d3d9.h>                    // For IDirect3DTexture9

// ---------------------------------------------------------------------------
// Your existing helper classes (AgentNameCache, etc.) - unchanged
// ---------------------------------------------------------------------------
class AgentNameCache {
public:
    const std::wstring& GetLower(uint32_t agent_id, const wchar_t* enc_name) {
        // ... your existing implementation ...
    }
private:
    // ... your existing Entry struct ...
};

// ... keep Utf8ToWide, ParseSemicolonNameList, etc. exactly as you had them ...

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
    bool show_zaishen_coin = true;          // New
};

class NameplatesPlugin : public ToolboxPlugin {
public:
    const char* Name() const override { return "Nameplates"; }

    bool* GetVisiblePtr() override { return &visible_; }
    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override;

    void LoadSettings(const wchar_t* folder) override {
        ToolboxPlugin::LoadSettings(folder);
        // ... your existing LoadSetting calls ...
        LoadSetting("show_zaishen_coin", settings_.show_zaishen_coin);
        RefreshPriorityBuffersAndLists();
    }

    void SaveSettings(const wchar_t* folder) override {
        // ... your existing SaveSetting calls ...
        SaveSetting("show_zaishen_coin", settings_.show_zaishen_coin);
        ToolboxPlugin::SaveSettings(folder);
    }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override {
        ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);

        // Load Zaishen coin texture
        if (auto* texModule = GW::GetGwDatTextureModule()) {
            zaishen_coin_texture = texModule->LoadTextureFromFileId(0x55778);
        }
    }

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

    IDirect3DTexture9* zaishen_coin_texture = nullptr;   // Added

    // ... your color constants and other members ...

    void RefreshPriorityBuffersAndLists() { /* your code */ }
    std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower) const { /* your code */ }

    void DrawNameplates() { /* your existing loop */ }

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living,
                 const std::wstring& name_lower, bool is_targeted) {

        float hp_pct = std::clamp(living->hp, 0.0f, 1.0f);

        const ImVec2 top_left(screen.x - settings_.bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + settings_.bar_width, top_left.y + settings_.bar_height);
        const ImVec2 fill_br(top_left.x + settings_.bar_width * hp_pct, bottom_right.y);

        draw_list->AddRectFilled(top_left, bottom_right, IM_COL32(40, 40, 40, 200));
        draw_list->AddRectFilled(top_left, fill_br, GetBarColor(living, name_lower, is_targeted));
        draw_list->AddRect(top_left, bottom_right, IM_COL32(0, 0, 0, 180));

        // Zaishen Coin Icon
        if (settings_.show_zaishen_coin && zaishen_coin_texture && living->GetHasQuest()) {
            const float coin_size = settings_.bar_height * 3.0f;
            const ImVec2 coin_pos(top_left.x - coin_size - 6.0f, 
                                  screen.y - coin_size * 0.5f + settings_.bar_height * 0.5f);

            draw_list->AddImage((ImTextureID)zaishen_coin_texture, coin_pos,
                                ImVec2(coin_pos.x + coin_size, coin_pos.y + coin_size));
        }
    }

    ImU32 GetBarColor(const GW::AgentLiving* living, const std::wstring& name_lower, bool is_targeted) {
        if (is_targeted) return IM_COL32(0, 0, 139, 255);
        if (auto p = GetPriorityColor(name_lower)) return *p;
        if (settings_.highlight_quest && living->GetHasQuest()) return IM_COL32(255, 179, 71, 255);
        return ColorFor(living->allegiance);
    }

    // ... keep all your other helper functions (WorldToScreen, ColorFor, etc.) unchanged ...

    void DrawSettingsInternal() {
        // ... your existing UI ...
        ImGui::Checkbox("Show Zaishen coin next to quest NPCs", &settings_.show_zaishen_coin);
    }
};

DLLAPI ToolboxPlugin* ToolboxPluginInstance() {
    static NameplatesPlugin instance;
    return &instance;
}
