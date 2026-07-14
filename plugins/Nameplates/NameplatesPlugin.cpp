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
#include <unordered_set>
#include <cwchar>
#include <optional>

class AgentNameCache {
public:

    const std::wstring& GetLower(uint32_t agent_id, const wchar_t* enc_name) {
        Entry& entry = cache_[agent_id];
        entry.last_seen_tick = tick_;
        if (enc_name && wcsncmp(entry.last_enc, enc_name, kMaxEncLen - 1) != 0) {
            wcsncpy_s(entry.last_enc, enc_name, kMaxEncLen - 1);
            entry.buffer[0] = L'\0';
            GW::UI::AsyncDecodeStr(enc_name, entry.buffer, kBufferLen);
        }
        if (entry.buffer[0] != L'\0') {
            entry.decoded_lower = entry.buffer;
            for (auto& c : entry.decoded_lower) {
                c = static_cast<wchar_t>(towlower(c));
            }
        }
        return entry.decoded_lower;
    }

    void MaybePrune() {
        ++tick_;
        if (tick_ - last_prune_tick_ < kPruneIntervalTicks) return;
        last_prune_tick_ = tick_;

        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (tick_ - it->second.last_seen_tick > kPruneIntervalTicks) {
                it = cache_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

private:
    static constexpr size_t kBufferLen = 256;
    static constexpr size_t kMaxEncLen = 64;
    static constexpr uint64_t kPruneIntervalTicks = 1800;
    struct Entry {
        wchar_t last_enc[kMaxEncLen] = {};
        wchar_t buffer[kBufferLen] = {};
        std::wstring decoded_lower;
        uint64_t last_seen_tick = 0;
    };
    std::unordered_map<uint32_t, Entry> cache_;
    uint64_t tick_ = 0;
    uint64_t last_prune_tick_ = 0;
};

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

inline std::unordered_set<std::wstring> ParseSemicolonNameList(const std::string& raw) {
    std::unordered_set<std::wstring> out;
    std::string current;
    auto flush = [&]() {

        size_t start = current.find_first_not_of(" \t\r\n");
        size_t end = current.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) {
            std::wstring w = Utf8ToWide(current.substr(start, end - start + 1));
            for (auto& c : w) c = static_cast<wchar_t>(towlower(c));
            if (!w.empty()) out.insert(std::move(w));
        }
        current.clear();
    };
    for (char c : raw) {
        if (c == ';') {
            flush();
        }
        else {
            current += c;
        }
    }
    flush();
    return out;
}

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
    float bg_tint_amount = 0.3f;
    float bg_opacity = 0.85f;

    std::string priority1_raw;
    std::string priority2_raw;
    std::string priority3_raw;

    bool color_target = true;

    bool highlight_quest = true;
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
        LoadSetting("bg_tint_amount", settings_.bg_tint_amount);
        LoadSetting("bg_opacity", settings_.bg_opacity);
        LoadSetting("priority1_raw", settings_.priority1_raw);
        LoadSetting("priority2_raw", settings_.priority2_raw);
        LoadSetting("priority3_raw", settings_.priority3_raw);
        LoadSetting("color_target", settings_.color_target);
        LoadSetting("highlight_quest", settings_.highlight_quest);
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
        SaveSetting("bg_tint_amount", settings_.bg_tint_amount);
        SaveSetting("bg_opacity", settings_.bg_opacity);
        SaveSetting("priority1_raw", settings_.priority1_raw);
        SaveSetting("priority2_raw", settings_.priority2_raw);
        SaveSetting("priority3_raw", settings_.priority3_raw);
        SaveSetting("color_target", settings_.color_target);
        SaveSetting("highlight_quest", settings_.highlight_quest);
        ToolboxPlugin::SaveSettings(folder);
    }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override {
        ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);

    }

    void SignalTerminate() override {
        ToolboxPlugin::SignalTerminate();

    }

    bool CanTerminate() override { return true; }

    void Draw(IDirect3DDevice9* ) override {
        if (!settings_.enabled) return;
        DrawNameplates();
    }

private:
    NameplateSettings settings_;
    bool visible_ = true;

    AgentNameCache name_cache_;
    std::unordered_set<std::wstring> priority1_names_, priority2_names_, priority3_names_;

    static constexpr size_t kPriorityBufSize = 512;
    char priority1_buf_[kPriorityBufSize] = {};
    char priority2_buf_[kPriorityBufSize] = {};
    char priority3_buf_[kPriorityBufSize] = {};

    static constexpr ImU32 kPriority1Color = IM_COL32(135, 206, 250, 255);
    static constexpr ImU32 kPriority2Color = IM_COL32(255, 105, 180, 255);
    static constexpr ImU32 kPriority3Color = IM_COL32(147, 112, 219, 255);
    static constexpr ImU32 kTargetColor    = IM_COL32(0, 0, 139, 255);
    static constexpr ImU32 kQuestColor     = IM_COL32(255, 179, 71, 255);

    static constexpr float kZNear = 46.875f;
    static constexpr float kZFar  = 48000.f;

    void RefreshPriorityBuffersAndLists() {
        strncpy_s(priority1_buf_, settings_.priority1_raw.c_str(), kPriorityBufSize - 1);
        strncpy_s(priority2_buf_, settings_.priority2_raw.c_str(), kPriorityBufSize - 1);
        strncpy_s(priority3_buf_, settings_.priority3_raw.c_str(), kPriorityBufSize - 1);
        priority1_names_ = ParseSemicolonNameList(settings_.priority1_raw);
        priority2_names_ = ParseSemicolonNameList(settings_.priority2_raw);
        priority3_names_ = ParseSemicolonNameList(settings_.priority3_raw);
    }

    std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower) const {
        if (name_lower.empty()) return std::nullopt;
        if (priority1_names_.count(name_lower)) return kPriority1Color;
        if (priority2_names_.count(name_lower)) return kPriority2Color;
        if (priority3_names_.count(name_lower)) return kPriority3Color;
        return std::nullopt;
    }

    void DrawNameplates() {
        GW::AgentArray* agents = GW::Agents::GetAgentArray();
        if (!agents || !agents->valid()) return;

        GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
        GW::AgentLiving* target = GW::Agents::GetTargetAsAgentLiving();

        DirectX::XMMATRIX view, view_proj;
        float viewport_width, viewport_height;
        if (!BuildFrameProjection(view, view_proj, viewport_width, viewport_height)) return;

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        for (GW::Agent* agent : *agents) {
            if (!agent) continue;
            if (!agent->GetIsLivingType()) continue;

            GW::AgentLiving* living = agent->GetAsAgentLiving();
            if (!living) continue;

            if (settings_.hide_dead && living->GetIsDead()) continue;
            if (settings_.hide_own_bar && me && living->agent_id == me->agent_id) continue;

            if (!ShouldShowAllegiance(living->allegiance)) continue;

            if (!WithinRange(living, me)) continue;

            ImVec2 screen;
            if (!WorldToScreen(living, view, view_proj, viewport_width, viewport_height, screen)) continue;

            const std::wstring& name_lower = name_cache_.GetLower(
                living->agent_id, GW::Agents::GetAgentEncName(living->agent_id));

            const bool is_targeted = settings_.color_target && target && living->agent_id == target->agent_id;

            DrawBar(draw_list, screen, living, name_lower, is_targeted);
        }

        name_cache_.MaybePrune();
    }

    bool ShouldShowAllegiance(GW::Constants::Allegiance allegiance) const {
        switch (allegiance) {
            case GW::Constants::Allegiance::Enemy:
                return settings_.show_enemies;
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Spirit_Pet:
            case GW::Constants::Allegiance::Minion:
                return settings_.show_allies;
            case GW::Constants::Allegiance::Neutral:
            case GW::Constants::Allegiance::Npc_Minipet:
                return settings_.show_neutrals;
            default:
                return false;
        }
    }

    bool WithinRange(const GW::AgentLiving* living, const GW::Agent* me) const {
        if (!me) return true;
        const float dx = living->pos.x - me->pos.x;
        const float dy = living->pos.y - me->pos.y;
        const float dist_sq = dx * dx + dy * dy;
        return dist_sq <= (settings_.max_range * settings_.max_range);
    }

    bool BuildFrameProjection(DirectX::XMMATRIX& out_view, DirectX::XMMATRIX& out_view_proj,
                              float& out_viewport_width, float& out_viewport_height) const {
        const auto cam = GW::CameraMgr::GetCamera();
        if (!cam) return false;

        using namespace DirectX;

        const XMVECTOR eye_pos = XMVectorSet(cam->position.x, cam->position.y, cam->position.z, 0.f);
        const XMVECTOR look_at = XMVectorSet(cam->look_at_target.x, cam->look_at_target.y, cam->look_at_target.z, 0.f);
        const XMVECTOR up      = XMVectorSet(0.f, 0.f, -1.f, 0.f);

        out_view = XMMatrixLookAtLH(eye_pos, look_at, up);

        out_viewport_width  = static_cast<float>(GW::Render::GetViewportWidth());
        out_viewport_height = static_cast<float>(GW::Render::GetViewportHeight());

        const float fov = GW::Render::GetFieldOfView();
        const float aspect = out_viewport_width / out_viewport_height;

        const XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, kZNear, kZFar);

        out_view_proj = out_view * proj;
        return true;
    }

    bool WorldToScreen(const GW::AgentLiving* living, const DirectX::XMMATRIX& view,
                       const DirectX::XMMATRIX& view_proj, float viewport_width, float viewport_height,
                       ImVec2& out) const {
        using namespace DirectX;

        const XMVECTOR world_pos = XMVectorSet(
            living->pos.x, living->pos.y, living->z + living->height1 + settings_.head_offset_z, 0.f);

        const XMVECTOR view_pos = XMVector3TransformCoord(world_pos, view);
        float view_pos_arr[4];
        XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(view_pos_arr), view_pos);
        if (view_pos_arr[2] <= kZNear) return false;

        const XMVECTOR clip_pos = XMVector3TransformCoord(world_pos, view_proj);

        float ndc[4];
        XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(ndc), clip_pos);

        out.x = (ndc[0] * 0.5f + 0.5f) * viewport_width;
        out.y = (1.f - (ndc[1] * 0.5f + 0.5f)) * viewport_height;

        return true;
    }

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living, const std::wstring& name_lower, bool is_targeted) {
        float hp_pct = living->hp;
        hp_pct = hp_pct < 0.f ? 0.f : (hp_pct > 1.f ? 1.f : hp_pct);

        const ImVec2 top_left(screen.x - settings_.bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + settings_.bar_width, top_left.y + settings_.bar_height);
        const ImVec2 fill_bottom_right(top_left.x + settings_.bar_width * hp_pct, bottom_right.y);

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

        ImVec4 fill_col4 = ImGui::ColorConvertU32ToFloat4(fill_color);
        ImVec4 bg_col4;
        bg_col4.x = fill_col4.x * settings_.bg_tint_amount;
        bg_col4.y = fill_col4.y * settings_.bg_tint_amount;
        bg_col4.z = fill_col4.z * settings_.bg_tint_amount;
        bg_col4.w = settings_.bg_opacity;
        const ImU32 bg_color = ImGui::ColorConvertFloat4ToU32(bg_col4);

        draw_list->AddRectFilled(top_left, bottom_right, bg_color);
        draw_list->AddRectFilled(top_left, fill_bottom_right, fill_color);
        draw_list->AddRect(top_left, bottom_right, IM_COL32(0, 0, 0, 180));
    }

    ImU32 ColorFor(GW::Constants::Allegiance allegiance) const {
        switch (allegiance) {
            case GW::Constants::Allegiance::Enemy:
                return IM_COL32(220, 40, 40, 255);
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Spirit_Pet:
            case GW::Constants::Allegiance::Minion:
                return IM_COL32(40, 200, 60, 255);
            default:
                return IM_COL32(200, 200, 60, 255);
        }
    }

    void DrawSettingsInternal() {
        ImGui::Checkbox("Enabled", &settings_.enabled);
        ImGui::Checkbox("Show enemies", &settings_.show_enemies);
        ImGui::Checkbox("Show allies", &settings_.show_allies);
        ImGui::Checkbox("Show neutrals", &settings_.show_neutrals);
        ImGui::Checkbox("Hide own bar", &settings_.hide_own_bar);
        ImGui::Checkbox("Hide dead", &settings_.hide_dead);
        ImGui::Checkbox("Color target (dark blue, overrides all other colors)", &settings_.color_target);
        ImGui::Checkbox("Highlight quest NPCs (light orange)", &settings_.highlight_quest);
        ImGui::SliderFloat("Max range", &settings_.max_range, 500.f, 10000.f);
        ImGui::SliderFloat("Bar width", &settings_.bar_width, 10.f, 100.f);
        ImGui::SliderFloat("Bar height", &settings_.bar_height, 2.f, 20.f);
        ImGui::SliderFloat("Head offset (fine-tune)", &settings_.head_offset_z, -100.f, 100.f);
        ImGui::SliderFloat("Background tint amount", &settings_.bg_tint_amount, 0.f, 1.f);
        ImGui::SliderFloat("Background opacity", &settings_.bg_opacity, 0.f, 1.f);

        ImGui::Separator();
        ImGui::TextUnformatted("Priority name coloring (semicolon-separated, e.g. \"Angry Hog; Angry Bat\")");

        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kPriority1Color).Value);
        const bool p1_changed = ImGui::InputText("Priority 1 (light blue)", priority1_buf_, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (p1_changed) {
            settings_.priority1_raw = priority1_buf_;
            priority1_names_ = ParseSemicolonNameList(settings_.priority1_raw);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kPriority2Color).Value);
        const bool p2_changed = ImGui::InputText("Priority 2 (pink)", priority2_buf_, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (p2_changed) {
            settings_.priority2_raw = priority2_buf_;
            priority2_names_ = ParseSemicolonNameList(settings_.priority2_raw);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kPriority3Color).Value);
        const bool p3_changed = ImGui::InputText("Priority 3 (purple)", priority3_buf_, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (p3_changed) {
            settings_.priority3_raw = priority3_buf_;
            priority3_names_ = ParseSemicolonNameList(settings_.priority3_raw);
        }
    }
};

void NameplatesPlugin::DrawSettings()
{
    ToolboxPlugin::DrawSettings();
    DrawSettingsInternal();
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static NameplatesPlugin instance;
    return &instance;
}
