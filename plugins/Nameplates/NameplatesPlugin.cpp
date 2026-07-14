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
#include <GWCA/Constants/AgentIDs.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <ToolboxPlugin.h>
#include <Utils/FontLoader.h>
#include <imgui.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cwchar>
#include <optional>
#include <cfloat>

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::string TruncateWithEllipsis(ImFont* font, float font_size, const std::wstring& name, float max_width) {
    const std::string full_utf8 = WideToUtf8(name);
    const ImVec2 full_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, full_utf8.c_str());
    if (full_size.x <= max_width) return full_utf8;

    size_t lo = 0, hi = name.size();
    std::string best_utf8 = WideToUtf8(L"...");
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        const std::string candidate_utf8 = WideToUtf8(name.substr(0, mid) + L"...");
        const ImVec2 candidate_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, candidate_utf8.c_str());
        if (candidate_size.x <= max_width) {
            lo = mid;
            best_utf8 = candidate_utf8;
        }
        else {
            hi = mid - 1;
        }
    }
    return best_utf8;
}

class AgentNameCache {
public:

    struct NameLookup {
        const std::wstring& lower;
        const std::wstring& display;
    };

    NameLookup Get(uint32_t agent_id, const wchar_t* enc_name) {
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
            entry.decoded_display = entry.buffer;
        }
        return { entry.decoded_lower, entry.decoded_display };
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
        std::wstring decoded_display;
        uint64_t last_seen_tick = 0;
    };
    std::unordered_map<uint32_t, Entry> cache_;
    uint64_t tick_ = 0;
    uint64_t last_prune_tick_ = 0;
};

inline bool IsMinipet(uint16_t player_number) {
    static const std::unordered_set<int> ids = {
        GW::Constants::ModelID::Minipet::Charr, GW::Constants::ModelID::Minipet::Dragon,
        GW::Constants::ModelID::Minipet::Rurik, GW::Constants::ModelID::Minipet::Shiro,
        GW::Constants::ModelID::Minipet::Titan, GW::Constants::ModelID::Minipet::Kirin,
        GW::Constants::ModelID::Minipet::NecridHorseman, GW::Constants::ModelID::Minipet::JadeArmor,
        GW::Constants::ModelID::Minipet::Hydra, GW::Constants::ModelID::Minipet::FungalWallow,
        GW::Constants::ModelID::Minipet::SiegeTurtle, GW::Constants::ModelID::Minipet::TempleGuardian,
        GW::Constants::ModelID::Minipet::JungleTroll, GW::Constants::ModelID::Minipet::WhiptailDevourer,
        GW::Constants::ModelID::Minipet::Gwen, GW::Constants::ModelID::Minipet::GwenDoll,
        GW::Constants::ModelID::Minipet::WaterDjinn, GW::Constants::ModelID::Minipet::Lich,
        GW::Constants::ModelID::Minipet::Elf, GW::Constants::ModelID::Minipet::PalawaJoko,
        GW::Constants::ModelID::Minipet::Koss, GW::Constants::ModelID::Minipet::MandragorImp,
        GW::Constants::ModelID::Minipet::HeketWarrior, GW::Constants::ModelID::Minipet::HarpyRanger,
        GW::Constants::ModelID::Minipet::Juggernaut, GW::Constants::ModelID::Minipet::WindRider,
        GW::Constants::ModelID::Minipet::FireImp, GW::Constants::ModelID::Minipet::Aatxe,
        GW::Constants::ModelID::Minipet::ThornWolf, GW::Constants::ModelID::Minipet::Abyssal,
        GW::Constants::ModelID::Minipet::BlackBeast, GW::Constants::ModelID::Minipet::Freezie,
        GW::Constants::ModelID::Minipet::Irukandji, GW::Constants::ModelID::Minipet::MadKingThorn,
        GW::Constants::ModelID::Minipet::ForestMinotaur, GW::Constants::ModelID::Minipet::Mursaat,
        GW::Constants::ModelID::Minipet::Nornbear, GW::Constants::ModelID::Minipet::Ooze,
        GW::Constants::ModelID::Minipet::Raptor, GW::Constants::ModelID::Minipet::RoaringEther,
        GW::Constants::ModelID::Minipet::CloudtouchedSimian, GW::Constants::ModelID::Minipet::CaveSpider,
        GW::Constants::ModelID::Minipet::WhiteRabbit, GW::Constants::ModelID::Minipet::WordofMadness,
        GW::Constants::ModelID::Minipet::DredgeBrute, GW::Constants::ModelID::Minipet::TerrorwebDryder,
        GW::Constants::ModelID::Minipet::Abomination, GW::Constants::ModelID::Minipet::KraitNeoss,
        GW::Constants::ModelID::Minipet::DesertGriffon, GW::Constants::ModelID::Minipet::Kveldulf,
        GW::Constants::ModelID::Minipet::QuetzalSly, GW::Constants::ModelID::Minipet::Jora,
        GW::Constants::ModelID::Minipet::FlowstoneElemental, GW::Constants::ModelID::Minipet::Nian,
        GW::Constants::ModelID::Minipet::DagnarStonepate, GW::Constants::ModelID::Minipet::FlameDjinn,
        GW::Constants::ModelID::Minipet::EyeOfJanthir, GW::Constants::ModelID::Minipet::Seer,
        GW::Constants::ModelID::Minipet::SiegeDevourer, GW::Constants::ModelID::Minipet::ShardWolf,
        GW::Constants::ModelID::Minipet::FireDrake, GW::Constants::ModelID::Minipet::SummitGiantHerder,
        GW::Constants::ModelID::Minipet::OphilNahualli, GW::Constants::ModelID::Minipet::CobaltScabara,
        GW::Constants::ModelID::Minipet::ScourgeManta, GW::Constants::ModelID::Minipet::Ventari,
        GW::Constants::ModelID::Minipet::Oola, GW::Constants::ModelID::Minipet::CandysmithMarley,
        GW::Constants::ModelID::Minipet::ZhuHanuku, GW::Constants::ModelID::Minipet::KingAdelbern,
        GW::Constants::ModelID::Minipet::MOX1, GW::Constants::ModelID::Minipet::MOX2,
        GW::Constants::ModelID::Minipet::MOX3, GW::Constants::ModelID::Minipet::MOX4,
        GW::Constants::ModelID::Minipet::MOX5, GW::Constants::ModelID::Minipet::MOX6,
        GW::Constants::ModelID::Minipet::BrownRabbit, GW::Constants::ModelID::Minipet::Yakkington,
        GW::Constants::ModelID::Minipet::CollectorsEditionKuunavang,
        GW::Constants::ModelID::Minipet::GrayGiant, GW::Constants::ModelID::Minipet::Asura,
        GW::Constants::ModelID::Minipet::DestroyerOfFlesh, GW::Constants::ModelID::Minipet::PolarBear,
        GW::Constants::ModelID::Minipet::CollectorsEditionVaresh, GW::Constants::ModelID::Minipet::Mallyx,
        GW::Constants::ModelID::Minipet::Ceratadon, GW::Constants::ModelID::Minipet::Kanaxai,
        GW::Constants::ModelID::Minipet::Panda, GW::Constants::ModelID::Minipet::IslandGuardian,
        GW::Constants::ModelID::Minipet::NagaRaincaller, GW::Constants::ModelID::Minipet::LonghairYeti,
        GW::Constants::ModelID::Minipet::Oni, GW::Constants::ModelID::Minipet::ShirokenAssassin,
        GW::Constants::ModelID::Minipet::Vizu, GW::Constants::ModelID::Minipet::ZhedShadowhoof,
        GW::Constants::ModelID::Minipet::Grawl, GW::Constants::ModelID::Minipet::GhostlyHero,
        GW::Constants::ModelID::Minipet::Pig, GW::Constants::ModelID::Minipet::GreasedLightning,
        GW::Constants::ModelID::Minipet::WorldFamousRacingBeetle, GW::Constants::ModelID::Minipet::CelestialPig,
        GW::Constants::ModelID::Minipet::CelestialRat, GW::Constants::ModelID::Minipet::CelestialOx,
        GW::Constants::ModelID::Minipet::CelestialTiger, GW::Constants::ModelID::Minipet::CelestialRabbit,
        GW::Constants::ModelID::Minipet::CelestialDragon, GW::Constants::ModelID::Minipet::CelestialSnake,
        GW::Constants::ModelID::Minipet::CelestialHorse, GW::Constants::ModelID::Minipet::CelestialSheep,
        GW::Constants::ModelID::Minipet::CelestialMonkey, GW::Constants::ModelID::Minipet::CelestialRooster,
        GW::Constants::ModelID::Minipet::CelestialDog, GW::Constants::ModelID::Minipet::BlackMoaChick,
        GW::Constants::ModelID::Minipet::Dhuum, GW::Constants::ModelID::Minipet::MadKingsGuard,
        GW::Constants::ModelID::Minipet::SmiteCrawler, GW::Constants::ModelID::Minipet::GuildLord,
        GW::Constants::ModelID::Minipet::HighPriestZhang, GW::Constants::ModelID::Minipet::GhostlyPriest,
        GW::Constants::ModelID::Minipet::RiftWarden, GW::Constants::ModelID::Minipet::MiniatureLegionnaire,
        GW::Constants::ModelID::Minipet::MiniatureConfessorDorian, GW::Constants::ModelID::Minipet::MiniaturePrincessSalma,
        GW::Constants::ModelID::Minipet::MiniatureLivia, GW::Constants::ModelID::Minipet::MiniatureEvennia,
        GW::Constants::ModelID::Minipet::MiniatureConfessorIsaiah, GW::Constants::ModelID::Minipet::MiniaturePeacekeeperEnforcer,
        GW::Constants::ModelID::Minipet::MiniatureMinisterReiko, GW::Constants::ModelID::Minipet::MiniatureEcclesiateXunRao,
    };
    return ids.count(static_cast<int>(player_number)) != 0;
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
    float max_range = 1500.0f;
    float enemy_bar_width = 200.0f;
    float enemy_bar_height = 20.0f;
    float friendly_bar_width = 80.0f;
    float friendly_bar_height = 17.0f;
    float head_offset_z = -59.0f;
    float height_scale = 0.8f;

    std::string priority1_raw;
    std::string priority2_raw;
    std::string priority3_raw;

    bool color_target = true;

    bool highlight_quest = true;
    bool color_by_profession = false;
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
        LoadSetting("max_range", settings_.max_range);
        LoadSetting("enemy_bar_width", settings_.enemy_bar_width);
        LoadSetting("enemy_bar_height", settings_.enemy_bar_height);
        LoadSetting("friendly_bar_width", settings_.friendly_bar_width);
        LoadSetting("friendly_bar_height", settings_.friendly_bar_height);
        LoadSetting("head_offset_z", settings_.head_offset_z);
        LoadSetting("height_scale", settings_.height_scale);
        LoadSetting("priority1_raw", settings_.priority1_raw);
        LoadSetting("priority2_raw", settings_.priority2_raw);
        LoadSetting("priority3_raw", settings_.priority3_raw);
        LoadSetting("color_target", settings_.color_target);
        LoadSetting("highlight_quest", settings_.highlight_quest);
        LoadSetting("color_by_profession", settings_.color_by_profession);
        RefreshPriorityBuffersAndLists();
    }

    void SaveSettings(const wchar_t* folder) override {
        SaveSetting("visible", visible_);
        SaveSetting("enabled", settings_.enabled);
        SaveSetting("show_enemies", settings_.show_enemies);
        SaveSetting("show_allies", settings_.show_allies);
        SaveSetting("show_neutrals", settings_.show_neutrals);
        SaveSetting("max_range", settings_.max_range);
        SaveSetting("enemy_bar_width", settings_.enemy_bar_width);
        SaveSetting("enemy_bar_height", settings_.enemy_bar_height);
        SaveSetting("friendly_bar_width", settings_.friendly_bar_width);
        SaveSetting("friendly_bar_height", settings_.friendly_bar_height);
        SaveSetting("head_offset_z", settings_.head_offset_z);
        SaveSetting("height_scale", settings_.height_scale);
        SaveSetting("priority1_raw", settings_.priority1_raw);
        SaveSetting("priority2_raw", settings_.priority2_raw);
        SaveSetting("priority3_raw", settings_.priority3_raw);
        SaveSetting("color_target", settings_.color_target);
        SaveSetting("highlight_quest", settings_.highlight_quest);
        SaveSetting("color_by_profession", settings_.color_by_profession);
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
    static constexpr ImU32 kTargetColor    = IM_COL32(255, 220, 0, 255);
    static constexpr ImU32 kQuestColor     = IM_COL32(255, 179, 71, 255);
    static constexpr float kBgTintAmount = 0.3f;
    static constexpr float kBgOpacity = 1.0f;

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

        bool have_targeted_bar = false;
        ImVec2 targeted_screen;
        GW::AgentLiving* targeted_living = nullptr;
        std::wstring targeted_name_lower;
        std::wstring targeted_display_name;

        for (GW::Agent* agent : *agents) {
            if (!agent) continue;
            if (!agent->GetIsLivingType()) continue;

            GW::AgentLiving* living = agent->GetAsAgentLiving();
            if (!living) continue;

            if (living->GetIsDead()) continue;
            if (me && living->agent_id == me->agent_id) continue;
            if (IsMinipet(living->player_number)) continue;

            if (!ShouldShowAllegiance(living->allegiance)) continue;

            if (!WithinRange(living, me)) continue;

            ImVec2 screen;
            if (!WorldToScreen(living, view, view_proj, viewport_width, viewport_height, screen)) continue;

            const auto name_lookup = name_cache_.Get(
                living->agent_id, GW::Agents::GetAgentEncName(living->agent_id));

            const bool is_targeted = settings_.color_target && target && living->agent_id == target->agent_id;

            if (is_targeted) {
                have_targeted_bar = true;
                targeted_screen = screen;
                targeted_living = living;
                targeted_name_lower = name_lookup.lower;
                targeted_display_name = name_lookup.display;
                continue;
            }

            DrawBar(draw_list, screen, living, name_lookup.lower, name_lookup.display, false);
        }

        if (have_targeted_bar) {
            DrawBar(draw_list, targeted_screen, targeted_living, targeted_name_lower, targeted_display_name, true);
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
            living->pos.x, living->pos.y, living->z + living->height1 * settings_.height_scale + settings_.head_offset_z, 0.f);

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

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living, const std::wstring& name_lower, const std::wstring& display_name, bool is_targeted) {
        float hp_pct = living->hp;
        hp_pct = hp_pct < 0.f ? 0.f : (hp_pct > 1.f ? 1.f : hp_pct);

        const bool is_enemy = living->allegiance == GW::Constants::Allegiance::Enemy;
        const float bar_width = is_enemy ? settings_.enemy_bar_width : settings_.friendly_bar_width;
        const float bar_height = is_enemy ? settings_.enemy_bar_height : settings_.friendly_bar_height;

        const ImVec2 top_left(screen.x - bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + bar_width, top_left.y + bar_height);
        const ImVec2 fill_bottom_right(top_left.x + bar_width * hp_pct, bottom_right.y);

        ImU32 fill_color;
        if (const auto priority_color = GetPriorityColor(name_lower)) {
            fill_color = *priority_color;
        }
        else if (settings_.highlight_quest && living->GetHasQuest()) {
            fill_color = kQuestColor;
        }
        else if (settings_.color_by_profession && living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable) {
            fill_color = ProfessionColor(living->primary);
        }
        else {
            fill_color = ColorFor(living->allegiance);
        }

        ImVec4 fill_col4 = ImGui::ColorConvertU32ToFloat4(fill_color);
        ImVec4 bg_col4;
        bg_col4.x = fill_col4.x * kBgTintAmount;
        bg_col4.y = fill_col4.y * kBgTintAmount;
        bg_col4.z = fill_col4.z * kBgTintAmount;
        bg_col4.w = kBgOpacity;
        const ImU32 bg_color = ImGui::ColorConvertFloat4ToU32(bg_col4);

        const ImU32 border_color = is_targeted ? kTargetColor : IM_COL32(0, 0, 0, 180);

        draw_list->AddRectFilled(top_left, bottom_right, bg_color);
        draw_list->AddRectFilled(top_left, fill_bottom_right, fill_color);
        draw_list->AddRect(top_left, bottom_right, border_color);

        if (!display_name.empty()) {
            ImFont* font = ImGui::GetFont();
            const float font_size = static_cast<float>(FontLoader::FontSize::header2);

            constexpr float kPadding = 6.f;
            const float max_text_width = bar_width - kPadding * 2.f;

            if (max_text_width > 0.f) {
                const std::string clipped_utf8 = TruncateWithEllipsis(font, font_size, display_name, max_text_width);
                const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, clipped_utf8.c_str());

                const float text_x = top_left.x + kPadding;
                const float text_y = top_left.y + (bar_height - text_size.y) / 2.f;

                static constexpr ImU32 kOutlineColor = IM_COL32(0, 0, 0, 255);
                static constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 255);
                static constexpr float kOutlineOffset = 1.f;

                const char* text_begin = clipped_utf8.c_str();
                const char* text_end = text_begin + clipped_utf8.size();

                draw_list->AddText(font, font_size, ImVec2(text_x - kOutlineOffset, text_y), kOutlineColor, text_begin, text_end);
                draw_list->AddText(font, font_size, ImVec2(text_x + kOutlineOffset, text_y), kOutlineColor, text_begin, text_end);
                draw_list->AddText(font, font_size, ImVec2(text_x, text_y - kOutlineOffset), kOutlineColor, text_begin, text_end);
                draw_list->AddText(font, font_size, ImVec2(text_x, text_y + kOutlineOffset), kOutlineColor, text_begin, text_end);
                draw_list->AddText(font, font_size, ImVec2(text_x, text_y), kTextColor, text_begin, text_end);
            }
        }
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
                return IM_COL32(0, 255, 152, 255);
        }
    }

    ImU32 ProfessionColor(GW::Constants::ProfessionByte prof) const {
        switch (prof) {
            case GW::Constants::ProfessionByte::Warrior:      return IM_COL32(205, 133, 63, 255);
            case GW::Constants::ProfessionByte::Ranger:       return IM_COL32(34, 139, 34, 255);
            case GW::Constants::ProfessionByte::Monk:         return IM_COL32(255, 250, 205, 255);
            case GW::Constants::ProfessionByte::Necromancer:  return IM_COL32(85, 107, 47, 255);
            case GW::Constants::ProfessionByte::Mesmer:       return IM_COL32(219, 112, 147, 255);
            case GW::Constants::ProfessionByte::Elementalist: return IM_COL32(255, 69, 0, 255);
            case GW::Constants::ProfessionByte::Assassin:     return IM_COL32(139, 0, 0, 255);
            case GW::Constants::ProfessionByte::Ritualist:    return IM_COL32(0, 128, 128, 255);
            case GW::Constants::ProfessionByte::Paragon:      return IM_COL32(255, 215, 0, 255);
            case GW::Constants::ProfessionByte::Dervish:      return IM_COL32(222, 184, 135, 255);
            default:                                          return IM_COL32(150, 150, 150, 255);
        }
    }

    void DrawSettingsInternal() {
        ImGui::Checkbox("Enabled", &settings_.enabled);
        ImGui::Checkbox("Show enemies (default red)", &settings_.show_enemies);
        ImGui::Checkbox("Show players/heroes/henchmen", &settings_.show_allies);
        ImGui::Checkbox("Show NPCs", &settings_.show_neutrals);
        ImGui::Checkbox("Color target (yellow border)", &settings_.color_target);
        ImGui::Checkbox("Highlight quest NPCs (light orange)", &settings_.highlight_quest);
        ImGui::Checkbox("Color players/heroes/henchmen by profession", &settings_.color_by_profession);
        ImGui::SliderFloat("Max range", &settings_.max_range, 500.f, 5000.f);
        ImGui::SliderFloat("Enemy bar width", &settings_.enemy_bar_width, 10.f, 200.f);
        ImGui::SliderFloat("Enemy bar height", &settings_.enemy_bar_height, 2.f, 20.f);
        ImGui::SliderFloat("Friendly bar width", &settings_.friendly_bar_width, 10.f, 200.f);
        ImGui::SliderFloat("Friendly bar height", &settings_.friendly_bar_height, 2.f, 20.f);
        ImGui::SliderFloat("Nameplate Axis(Y)", &settings_.head_offset_z, -100.f, 100.f);
        ImGui::SliderFloat("Height scale (bounding box)", &settings_.height_scale, 0.1f, 1.5f);

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
